/* ============================================================================
 * dtt_threads.c - see dtt_threads.h for design notes.
 * ============================================================================ */

#include "../include/dtt_threads.h"
#include "../include/dtt_config.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Hostile-thread registry.
 *
 * One entry per flagged thread, keyed by a strong global reference to the
 * java.lang.Thread object (IsSameObject-based identity, so it stays valid
 * for the thread's whole lifetime regardless of reentrancy/renaming).
 * Each entry also carries a small per-class offense counter so that
 * "repeat offender" is defined precisely: the SAME thread attacking the
 * SAME protected class more than once.
 * ------------------------------------------------------------------------- */
typedef struct {
    char class_name[DTT_CACHE_NAME_MAX];
    int count;
} dtt_class_offense_t;

typedef struct {
    jobject thread_ref;                    /* strong global ref to the hostile Thread */
    char name[DTT_THREAD_NAME_MAX];        /* snapshot for log lines */
    dtt_class_offense_t classes[DTT_THREAD_CLASS_OFFENSES];
    int in_use;
} dtt_thread_entry_t;

static dtt_thread_entry_t g_threads[DTT_THREAD_TRACK_CAPACITY];
static dtt_mutex_t g_threads_mutex;
static int g_threads_ready = 0;
static int g_reflect_after_hits = 2;
static int g_absorbed_threads = 0;
static int g_reflected_events = 0;

void dtt_threads_module_init(void) {
    memset(g_threads, 0, sizeof(g_threads));
    DTT_MUTEX_INIT(&g_threads_mutex);
    g_threads_ready = 1;
    g_reflect_after_hits = 2;
    g_absorbed_threads = 0;
    g_reflected_events = 0;
}

void dtt_threads_module_shutdown(void) {
    /* Any remaining global refs are intentionally left alone: they point
     * at Thread objects belonging to a JVM that is itself shutting down
     * (VMDeath already fired), so there is nothing safe left to delete
     * them through JNI at this point. */
    if (!g_threads_ready) {
        return;
    }
    DTT_MUTEX_DESTROY(&g_threads_mutex);
    g_threads_ready = 0;
}

void dtt_threads_configure(int reflect_after_hits) {
    if (reflect_after_hits >= 1) {
        g_reflect_after_hits = reflect_after_hits;
    }
}

int dtt_threads_absorbed_thread_count(void) {
    return g_absorbed_threads;
}

int dtt_threads_reflected_event_count(void) {
    return g_reflected_events;
}

void dtt_threads_format_name(jvmtiEnv *jvmti_env, jthread thread,
                             char *out_buf, int out_buf_len) {
    jvmtiThreadInfo info;
    jvmtiError err;

    if (out_buf == NULL || out_buf_len <= 0) {
        return;
    }
    out_buf[0] = '\0';

    if (jvmti_env == NULL || thread == NULL) {
        strncpy(out_buf, "(unnamed)", (size_t)out_buf_len - 1);
        out_buf[out_buf_len - 1] = '\0';
        return;
    }

    memset(&info, 0, sizeof(info));
    err = (*jvmti_env)->GetThreadInfo(jvmti_env, thread, &info);
    if (err == JVMTI_ERROR_NONE && info.name != NULL) {
        snprintf(out_buf, (size_t)out_buf_len, "%s%s",
                 info.name, info.is_daemon ? " [daemon]" : "");
        (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)info.name);
    } else {
        strncpy(out_buf, "(unnamed)", (size_t)out_buf_len - 1);
        out_buf[out_buf_len - 1] = '\0';
    }
}

/* Caller must hold g_threads_mutex. */
static dtt_thread_entry_t *dtt_threads_find_locked(JNIEnv *jni_env, jthread thread) {
    int i;
    for (i = 0; i < DTT_THREAD_TRACK_CAPACITY; i++) {
        if (g_threads[i].in_use && g_threads[i].thread_ref != NULL) {
            if ((*jni_env)->IsSameObject(jni_env, g_threads[i].thread_ref, thread)) {
                return &g_threads[i];
            }
        }
    }
    return NULL;
}

/* Caller must hold g_threads_mutex. */
static dtt_thread_entry_t *dtt_threads_create_locked(JNIEnv *jni_env, jthread thread,
                                                     const char *name) {
    int i;
    for (i = 0; i < DTT_THREAD_TRACK_CAPACITY; i++) {
        if (!g_threads[i].in_use) {
            g_threads[i].thread_ref = (*jni_env)->NewGlobalRef(jni_env, thread);
            if (g_threads[i].thread_ref == NULL) {
                return NULL; /* OOM - caller still absorbs the attempt */
            }
            strncpy(g_threads[i].name, name, DTT_THREAD_NAME_MAX - 1);
            g_threads[i].name[DTT_THREAD_NAME_MAX - 1] = '\0';
            memset(g_threads[i].classes, 0, sizeof(g_threads[i].classes));
            g_threads[i].in_use = 1;
            g_absorbed_threads++;
            return &g_threads[i];
        }
    }
    return NULL; /* table full - fail-safe, caller still absorbs */
}

/* Caller must hold g_threads_mutex. */
static void dtt_threads_bump_class_locked(dtt_thread_entry_t *entry, const char *class_name) {
    int i;
    for (i = 0; i < DTT_THREAD_CLASS_OFFENSES; i++) {
        if (entry->classes[i].count > 0 &&
            strncmp(entry->classes[i].class_name, class_name, DTT_CACHE_NAME_MAX) == 0) {
            entry->classes[i].count++;
            return;
        }
    }
    for (i = 0; i < DTT_THREAD_CLASS_OFFENSES; i++) {
        if (entry->classes[i].count == 0) {
            strncpy(entry->classes[i].class_name, class_name, DTT_CACHE_NAME_MAX - 1);
            entry->classes[i].class_name[DTT_CACHE_NAME_MAX - 1] = '\0';
            entry->classes[i].count = 1;
            return;
        }
    }
    /* Per-thread class table full - fail-safe: leave counts unchanged,
     * treated as a first offense so the caller still absorbs. */
}

dtt_thread_action_t dtt_threads_offend(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                                       jthread thread, const char *class_name) {
    dtt_thread_entry_t *entry;
    char name_buf[DTT_THREAD_NAME_MAX];
    dtt_thread_action_t action = DTT_ACTION_ABSORB;
    int i;
    int count = 0;

    if (!g_threads_ready || jni_env == NULL || class_name == NULL) {
        return DTT_ACTION_ABSORB; /* fail-safe: still neutralize the attempt */
    }

    if (thread == NULL && jvmti_env != NULL) {
        (*jvmti_env)->GetCurrentThread(jvmti_env, &thread);
    }
    if (thread == NULL) {
        return DTT_ACTION_ABSORB; /* cannot identify the thread, still neutralize */
    }

    dtt_threads_format_name(jvmti_env, thread, name_buf, (int)sizeof(name_buf));

    DTT_MUTEX_LOCK(&g_threads_mutex);

    entry = dtt_threads_find_locked(jni_env, thread);
    if (entry == NULL) {
        entry = dtt_threads_create_locked(jni_env, thread, name_buf);
    }
    if (entry == NULL) {
        DTT_MUTEX_UNLOCK(&g_threads_mutex);
        return DTT_ACTION_ABSORB;
    }

    dtt_threads_bump_class_locked(entry, class_name);
    for (i = 0; i < DTT_THREAD_CLASS_OFFENSES; i++) {
        if (entry->classes[i].count > 0 &&
            strncmp(entry->classes[i].class_name, class_name, DTT_CACHE_NAME_MAX) == 0) {
            count = entry->classes[i].count;
            break;
        }
    }

    if (count >= g_reflect_after_hits) {
        action = DTT_ACTION_REFLECT;
        g_reflected_events++;
    }

    DTT_MUTEX_UNLOCK(&g_threads_mutex);

    return action;
}
