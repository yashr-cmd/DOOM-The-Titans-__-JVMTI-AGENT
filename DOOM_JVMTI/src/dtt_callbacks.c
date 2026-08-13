/* ============================================================================
 * dtt_callbacks.c - see dtt_callbacks.h for design notes.
 * ============================================================================ */

#include "../include/dtt_callbacks.h"
#include "../include/dtt_config.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_log.h"
#include "../include/dtt_protected.h"
#include "../include/dtt_score.h"
#include "../include/dtt_cache.h"
#include "../include/dtt_auth.h"

#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Shadow-classloader + consecutive-offense tracking table.
 *
 * Keyed by protected class name, records:
 *   - a weak global reference to the ClassLoader that first prepared
 *     this class (for shadow-classloader detection in ClassPrepare)
 *   - a running consecutive-offense counter (for escalating repeat
 *     tamper attempts in ClassFileLoadHook)
 *
 * This is separate from dtt_cache.c (which stores bytecode) because
 * the two have different lifetimes and different JNI/JVMTI
 * requirements (this table holds a jweak that must be released with
 * DeleteWeakGlobalRef; the bytecode cache is plain malloc'd memory).
 * ------------------------------------------------------------------------- */
typedef struct {
    char name[DTT_CACHE_NAME_MAX];
    jweak loader_ref;   /* weak ref to the ClassLoader that first loaded this class */
    int offense_count;  /* consecutive unauthorized-tamper hits observed so far */
    int in_use;
} dtt_track_entry_t;

static dtt_track_entry_t g_track[DTT_CACHE_CAPACITY];
static dtt_mutex_t g_track_mutex;
static int g_track_ready = 0;

void dtt_callbacks_module_init(void) {
    memset(g_track, 0, sizeof(g_track));
    DTT_MUTEX_INIT(&g_track_mutex);
    g_track_ready = 1;
}

void dtt_callbacks_module_shutdown(void) {
    /* Note: any remaining jweak references are simply dropped here.
     * They point at ClassLoader objects belonging to a JVM that is
     * itself shutting down (VMDeath already fired by the time
     * Agent_OnUnload runs), so there is nothing unsafe left to clean
     * up via JNI at this point. */
    if (!g_track_ready) {
        return;
    }
    DTT_MUTEX_DESTROY(&g_track_mutex);
    g_track_ready = 0;
}

static dtt_track_entry_t *dtt_track_find_locked(const char *name) {
    int i;
    for (i = 0; i < DTT_CACHE_CAPACITY; i++) {
        if (g_track[i].in_use && strncmp(g_track[i].name, name, DTT_CACHE_NAME_MAX) == 0) {
            return &g_track[i];
        }
    }
    return NULL;
}

static dtt_track_entry_t *dtt_track_find_or_create_locked(const char *name) {
    dtt_track_entry_t *entry = dtt_track_find_locked(name);
    int i;

    if (entry != NULL) {
        return entry;
    }
    for (i = 0; i < DTT_CACHE_CAPACITY; i++) {
        if (!g_track[i].in_use) {
            strncpy(g_track[i].name, name, DTT_CACHE_NAME_MAX - 1);
            g_track[i].name[DTT_CACHE_NAME_MAX - 1] = '\0';
            g_track[i].loader_ref = NULL;
            g_track[i].offense_count = 0;
            g_track[i].in_use = 1;
            return &g_track[i];
        }
    }
    return NULL; /* table full - fail-safe: caller just skips tracking for this class */
}

/* ---------------------------------------------------------------------------
 * VMInit / VMDeath - lifecycle logging only. No capability requirements.
 * ------------------------------------------------------------------------- */
static void JNICALL dtt_on_vm_init(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread) {
    (void)jvmti_env;
    (void)jni_env;
    (void)thread;
    dtt_log(DTT_LOG_INFO, "JVM initialization complete - DTT native integrity agent is live");
}

static void JNICALL dtt_on_vm_death(jvmtiEnv *jvmti_env, JNIEnv *jni_env) {
    (void)jvmti_env;
    (void)jni_env;
    dtt_log(DTT_LOG_INFO, "JVM shutting down - final integrity score was %d (%s)",
            dtt_score_get(), dtt_status_name(dtt_score_get_status()));
}

/* ---------------------------------------------------------------------------
 * ClassPrepare - shadow-classloader detection for protected classes only.
 * We never veto or delay loading here (JVMTI does not allow that from
 * ClassPrepare); we only observe and score.
 * ------------------------------------------------------------------------- */
static void JNICALL dtt_on_class_prepare(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                                          jthread thread, jclass klass) {
    char signature_buf[DTT_CACHE_NAME_MAX];
    char class_name[DTT_CACHE_NAME_MAX];
    char *sig = NULL;
    jobject loader = NULL;
    jvmtiError err;
    dtt_track_entry_t *entry;

    (void)thread;

    if (!g_track_ready) {
        return;
    }

    err = (*jvmti_env)->GetClassSignature(jvmti_env, klass, &sig, NULL);
    if (err != JVMTI_ERROR_NONE || sig == NULL) {
        return; /* fail-safe: nothing to check, just move on */
    }

    strncpy(signature_buf, sig, sizeof(signature_buf) - 1);
    signature_buf[sizeof(signature_buf) - 1] = '\0';
    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)sig);

    dtt_strip_class_signature(signature_buf, class_name, (int)sizeof(class_name));

    if (!dtt_is_protected_class(class_name)) {
        return; /* not one of ours - none of our business */
    }

    err = (*jvmti_env)->GetClassLoader(jvmti_env, klass, &loader);
    if (err != JVMTI_ERROR_NONE) {
        return; /* fail-safe: can't determine loader, skip this check */
    }

    DTT_MUTEX_LOCK(&g_track_mutex);
    entry = dtt_track_find_or_create_locked(class_name);
    if (entry == NULL) {
        DTT_MUTEX_UNLOCK(&g_track_mutex);
        return; /* tracking table full - fail-safe, just skip */
    }

    if (entry->loader_ref == NULL) {
        /* First time we've seen this protected class prepared - record
         * its loader as the legitimate one and move on. */
        if (loader != NULL) {
            entry->loader_ref = (*jni_env)->NewWeakGlobalRef(jni_env, loader);
        }
        DTT_MUTEX_UNLOCK(&g_track_mutex);
        return;
    }

    /* We've seen this class name before - check whether the loader
     * instance matches the one we recorded. A mismatch means a second,
     * different ClassLoader is defining a class with the exact same
     * name as one of our protected classes: a classic "shadow class"
     * technique for smuggling in a fake replacement. */
    {
        jobject recorded = (*jni_env)->NewLocalRef(jni_env, entry->loader_ref);
        int is_same = (recorded != NULL && loader != NULL &&
                       (*jni_env)->IsSameObject(jni_env, recorded, loader));
        int recorded_was_collected = (entry->loader_ref != NULL && recorded == NULL);

        if (recorded != NULL) {
            (*jni_env)->DeleteLocalRef(jni_env, recorded);
        }

        DTT_MUTEX_UNLOCK(&g_track_mutex);

        if (!is_same && !recorded_was_collected) {
            dtt_score_add(DTT_POINTS_SHADOW_CLASSLOADER, "DTT-201",
                          "Protected class '%s' was prepared by a different ClassLoader than "
                          "the one that first loaded it - possible shadow-class attempt",
                          class_name);
        }
    }
}

/* ---------------------------------------------------------------------------
 * ClassFileLoadHook - the core detection + mitigation logic.
 *
 * Behavior summary:
 *   1. name == NULL or not a protected class -> ignore completely.
 *      This agent NEVER inspects, scores, or interferes with bytecode
 *      belonging to anything other than DTT's own classes.
 *   2. First (non-redefinition) load of a protected class -> cache its
 *      bytecode as "known good" and let it load unmodified.
 *   3. Redefinition/retransformation of a protected class while the
 *      calling thread is inside an authorized-transform window (see
 *      dtt_auth.h) -> this is the DTT Java runtime agent doing its own
 *      legitimate patching. Refresh the cache and let it through.
 *   4. Redefinition/retransformation of a protected class WITHOUT
 *      authorization -> unauthorized tampering. Score it, and if we
 *      have a cached known-good copy, substitute it back in via
 *      new_class_data so the tampering is neutralized in place. If we
 *      cannot safely do that (no cached copy, allocation failure),
 *      score the extra uncertainty and let the JVM continue - we never
 *      throw, abort, or otherwise risk crashing Minecraft over this.
 * ------------------------------------------------------------------------- */
static void JNICALL dtt_on_class_file_load_hook(
        jvmtiEnv *jvmti_env,
        JNIEnv *jni_env,
        jclass class_being_redefined,
        jobject loader,
        const char *name,
        jobject protection_domain,
        jint class_data_len,
        const unsigned char *class_data,
        jint *new_class_data_len,
        unsigned char **new_class_data) {

    dtt_track_entry_t *entry;
    const unsigned char *cached_data = NULL;
    jint cached_len = 0;
    int has_cache;
    int bonus;

    (void)jni_env;
    (void)loader;
    (void)protection_domain;

    if (name == NULL) {
        /* Some early JVM-internal loads (e.g. anonymous/hidden classes
         * before naming is resolved) present with name == NULL. We
         * cannot classify these safely, so - fail-safe - we ignore
         * them entirely rather than guessing. */
        return;
    }

    if (!dtt_is_protected_class(name)) {
        return; /* not ours - never touched */
    }

    if (class_being_redefined == NULL) {
        /* Ordinary first-time class load, not a redefinition. Cache it
         * as the known-good baseline and let it proceed unmodified. */
        dtt_cache_store(name, class_data, class_data_len);
        return;
    }

    /* From here on: this IS a redefinition/retransformation of one of
     * OUR protected classes. */

    if (dtt_auth_is_active()) {
        /* The DTT Java runtime agent told us (via the JNI bridge) that
         * it is about to legitimately patch its own classes. Trust it,
         * refresh our known-good copy, and reset the offense streak. */
        dtt_cache_store(name, class_data, class_data_len);

        if (g_track_ready) {
            DTT_MUTEX_LOCK(&g_track_mutex);
            entry = dtt_track_find_or_create_locked(name);
            if (entry != NULL) {
                entry->offense_count = 0;
            }
            DTT_MUTEX_UNLOCK(&g_track_mutex);
        }

        dtt_score_add(0, "DTT-000", "Authorized internal transform of protected class '%s'", name);
        return;
    }

    /* Unauthorized redefinition attempt against a protected class. */
    bonus = 0;
    if (g_track_ready) {
        DTT_MUTEX_LOCK(&g_track_mutex);
        entry = dtt_track_find_or_create_locked(name);
        if (entry != NULL) {
            entry->offense_count++;
            bonus = entry->offense_count * DTT_REPEAT_OFFENSE_BONUS_PER_HIT;
            if (bonus > DTT_REPEAT_OFFENSE_BONUS_CAP) {
                bonus = DTT_REPEAT_OFFENSE_BONUS_CAP;
            }
        }
        DTT_MUTEX_UNLOCK(&g_track_mutex);
    }

    dtt_score_add(DTT_POINTS_UNAUTHORIZED_REDEFINE + bonus, "DTT-101",
                  "Unauthorized redefinition attempt on protected class '%s' (consecutive "
                  "offense bonus: +%d)", name, bonus);

    has_cache = dtt_cache_lookup(name, &cached_data, &cached_len);

    if (has_cache && cached_len > 0) {
        unsigned char *replacement = NULL;
        jvmtiError alloc_err = (*jvmti_env)->Allocate(jvmti_env, cached_len, &replacement);

        if (alloc_err == JVMTI_ERROR_NONE && replacement != NULL) {
            memcpy(replacement, cached_data, (size_t)cached_len);
            *new_class_data_len = cached_len;
            *new_class_data = replacement;

            dtt_score_add(DTT_POINTS_TAMPER_BLOCKED_OK, "DTT-102",
                          "Isolated tampering: restored known-good bytecode for protected "
                          "class '%s' instead of the tampered version", name);
            return;
        }

        dtt_log(DTT_LOG_WARN,
                "wanted to restore known-good bytecode for '%s' but jvmtiEnv->Allocate failed "
                "(err=%d) - allowing JVM to continue with the substituted class and relying on "
                "the Java-side DTT runtime agent's own defenses instead",
                name, (int)alloc_err);
    }

    /* Could not safely isolate the tampering (no cached baseline yet,
     * or allocation failed). We deliberately do NOT try to block the
     * class load by other means (e.g. returning garbage bytes to
     * force a VerifyError) because that risks destabilizing or
     * crashing the JVM, which this agent must never do. Instead we
     * record the elevated uncertainty and let Minecraft continue; the
     * Java-side DTT runtime agent can read this score/reason back out
     * via the bridge and react (e.g. re-apply its own protections). */
    dtt_score_add(DTT_POINTS_REDEFINE_COULD_NOT_BLOCK, "DTT-103",
                  "Could not safely isolate tampering on protected class '%s' (no cached "
                  "baseline or allocation failure) - reported to Java-side runtime agent, "
                  "JVM execution continues", name);
}

/* ---------------------------------------------------------------------------
 * Public entry point used by dtt_agent.c to wire up the callback struct.
 * ------------------------------------------------------------------------- */
void dtt_callbacks_build(jvmtiEventCallbacks *callbacks, int class_hook_available) {
    memset(callbacks, 0, sizeof(jvmtiEventCallbacks));

    callbacks->VMInit = dtt_on_vm_init;
    callbacks->VMDeath = dtt_on_vm_death;
    callbacks->ClassPrepare = dtt_on_class_prepare;

    if (class_hook_available) {
        callbacks->ClassFileLoadHook = dtt_on_class_file_load_hook;
    } else {
        dtt_log(DTT_LOG_WARN,
                "can_generate_all_class_hook_events capability unavailable - tamper "
                "detection/isolation for protected classes is DISABLED on this JVM; all "
                "other agent functionality continues normally");
    }
}
