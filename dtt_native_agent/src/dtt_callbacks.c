#include "../include/dtt_callbacks.h"
#include "../include/dtt_config.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_log.h"
#include "../include/dtt_protected.h"
#include "../include/dtt_cache.h"
#include "../include/dtt_auth.h"
#include "../include/dtt_watchdog.h"
#include "../include/dtt_agent.h"
#include "../include/dtt_threads.h"
#include "../include/dtt_vanilla_guard.h"

#include <string.h>
#include <stdlib.h>

typedef struct {
    char name[DTT_CACHE_NAME_MAX];
    jweak loader_ref;   /* weak ref to the ClassLoader that first loaded this class */
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
            g_track[i].in_use = 1;
            return &g_track[i];
        }
    }
    return NULL; /* table full - fail-safe: caller just skips tracking for this class */
}

static int dtt_is_shadow_load(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jobject loader,
                              const char *class_name) {
    dtt_track_entry_t *entry;
    jobject recorded = NULL;
    int is_same;
    int is_shadow = 0;

    (void)jvmti_env;

    if (!g_track_ready || jni_env == NULL || loader == NULL || class_name == NULL) {
        return 0;
    }

    DTT_MUTEX_LOCK(&g_track_mutex);
    entry = dtt_track_find_locked(class_name);
    if (entry != NULL && entry->loader_ref != NULL) {
        recorded = (*jni_env)->NewLocalRef(jni_env, entry->loader_ref);
        if (recorded != NULL) {
            is_same = (*jni_env)->IsSameObject(jni_env, recorded, loader);
            (*jni_env)->DeleteLocalRef(jni_env, recorded);
            is_shadow = !is_same;
        } else {
            is_shadow = 0;
        }
    }
    DTT_MUTEX_UNLOCK(&g_track_mutex);

    return is_shadow;
}

static void JNICALL dtt_on_vm_init(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread) {
    (void)thread;
    dtt_callbacks_mark_vm_init_done();
    dtt_log(DTT_LOG_INFO, "JVM initialization complete - DTT native integrity agent is live");
    dtt_watchdog_start(jvmti_env, jni_env, dtt_agent_scan_interval_ms(),
                       dtt_agent_bytecode_capability(), dtt_agent_redefine_capability());
}

static void JNICALL dtt_on_vm_death(jvmtiEnv *jvmti_env, JNIEnv *jni_env) {
    (void)jvmti_env;
    (void)jni_env;
    dtt_watchdog_request_stop();
    dtt_log(DTT_LOG_INFO, "JVM shutting down - %d hostile thread(s) absorbed, %d reflect "
            "event(s) issued",
            dtt_threads_absorbed_thread_count(), dtt_threads_reflected_event_count());
}

static void JNICALL dtt_on_class_prepare(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                                          jthread thread, jclass klass) {
    char signature_buf[DTT_CACHE_NAME_MAX];
    char class_name[DTT_CACHE_NAME_MAX];
    char *sig = NULL;
    jobject loader = NULL;
    jvmtiError err;
    dtt_track_entry_t *entry;

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
        dtt_callbacks_note_class_prepared(class_name);
        return; /* not one of ours - none of our business */
    }

    dtt_watchdog_track_class(jvmti_env, klass, class_name);

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
        if (loader != NULL) {
            entry->loader_ref = (*jni_env)->NewWeakGlobalRef(jni_env, loader);
        }
        DTT_MUTEX_UNLOCK(&g_track_mutex);
        return;
    }

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
            (void)dtt_threads_offend(jvmti_env, jni_env, thread, class_name);
            dtt_log(DTT_LOG_WARN,
                    "protected class '%s' was prepared by a different ClassLoader than the "
                    "one that first loaded it - possible shadow-class attempt; the loading "
                    "thread has been flagged for absorption",
                    class_name);
        }
    }
}

static volatile int g_vm_init_done = 0;
static int g_modlauncher_detected = 0;
static int g_modlauncher_checked = 0;

void dtt_callbacks_mark_vm_init_done(void) {
    g_vm_init_done = 1;
}

void dtt_callbacks_note_class_prepared(const char *class_name) {
    if (!g_modlauncher_detected && class_name != NULL &&
        strncmp(class_name, "cpw/mods/modlauncher/Launcher",
                sizeof("cpw/mods/modlauncher/Launcher")) == 0) {
        g_modlauncher_detected = 1;
        g_modlauncher_checked = 1;
        dtt_log(DTT_LOG_INFO, "ModLauncher detected - vanilla guard injection disabled for compatibility");
    }
}

/*
 * ModLauncher presence check.
 *
 * IMPORTANT: FindClass must NEVER run before VMInit. During bootstrap
 * (System.initPhase3) the app class loader is not functional yet: the lookup
 * throws NoClassDefFoundError mid-module-load and kills VM initialization,
 * or (if the class is reachable from the bootclasspath) recursively loads
 * ModLauncher's whole dependency graph from inside ClassFileLoadHook until
 * the thread overflows its stack. Both paths abort any JVM launched with
 * -agentpath.
 */
static int is_modlauncher_present(JNIEnv *jni_env) {
    jclass modlauncher_class;

    if (g_modlauncher_checked) {
        return g_modlauncher_detected;
    }
    if (!g_vm_init_done) {
        /* Too early to probe safely. ModLauncher (when present) always
         * prepares after VMInit and long before the first Minecraft class,
         * so dtt_callbacks_note_class_prepared() will flag it in time. */
        return 0;
    }

    g_modlauncher_checked = 1;
    modlauncher_class = (*jni_env)->FindClass(jni_env, "cpw/mods/modlauncher/Launcher");
    if ((*jni_env)->ExceptionCheck(jni_env)) {
        /* Class not resolvable through the system loader: clear the pending
         * NoClassDefFoundError instead of leaking it into whatever Java
         * frame triggered this class load. */
        (*jni_env)->ExceptionClear(jni_env);
        modlauncher_class = NULL;
    }
    g_modlauncher_detected = (modlauncher_class != NULL);
    if (modlauncher_class != NULL) {
        (*jni_env)->DeleteLocalRef(jni_env, modlauncher_class);
        dtt_log(DTT_LOG_INFO, "ModLauncher detected - vanilla guard injection disabled for compatibility");
    }

    return g_modlauncher_detected;
}

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

    const unsigned char *cached_data = NULL;
    jint cached_len = 0;
    int has_cache;

    (void)protection_domain;

    if (name == NULL) {
        return;
    }

    if (!dtt_is_protected_class(name)) {
        /* Not one of our mod classes. Try vanilla guard injection:
         * if this class is a vanilla entity choke-point (LivingEntity,
         * ServerPlayer, Entity, SynchedEntityData), inject a guard
         * prologue that calls our Java-side protection before the
         * operation takes effect. Only on first load, not redefinition.
         * SKIP if ModLauncher is present to avoid incompatibility. */
        if (class_being_redefined == NULL && g_vm_init_done && !is_modlauncher_present(jni_env)) {
            dtt_cf_result_t vg_result = dtt_vanilla_guard_intercept(
                name, class_data, class_data_len);
            if (vg_result.data != NULL) {
                unsigned char *replacement = NULL;
                jvmtiError alloc_err = (*jvmti_env)->Allocate(
                    jvmti_env, vg_result.len, &replacement);
                if (alloc_err == JVMTI_ERROR_NONE && replacement != NULL) {
                    memcpy(replacement, vg_result.data, (size_t)vg_result.len);
                    *new_class_data_len = vg_result.len;
                    *new_class_data = replacement;
                }
                dtt_cf_result_free(&vg_result);
            }
        }
        return; /* vanilla guard attempted or not applicable - move on */
    }

    if (class_being_redefined == NULL) {
        /* Ordinary first-time class load, not a redefinition. */
        if (dtt_is_shadow_load(jvmti_env, jni_env, loader, name)) {
            (void)dtt_threads_offend(jvmti_env, jni_env, NULL, name);

            has_cache = dtt_cache_lookup(name, &cached_data, &cached_len);
            if (has_cache && cached_len > 0) {
                unsigned char *replacement = NULL;
                jvmtiError alloc_err = (*jvmti_env)->Allocate(jvmti_env, cached_len, &replacement);
                if (alloc_err == JVMTI_ERROR_NONE && replacement != NULL) {
                    memcpy(replacement, cached_data, (size_t)cached_len);
                    *new_class_data_len = cached_len;
                    *new_class_data = replacement;
                    dtt_log(DTT_LOG_WARN,
                            "ABSORBED shadow-class load of protected class '%s' - known-good "
                            "bytecode substituted, smuggled version discarded",
                            name);
                    return;
                }
            }
            dtt_log(DTT_LOG_WARN,
                    "shadow-class load of protected class '%s' detected but no cached "
                    "known-good bytecode was available to absorb it with",
                    name);
            return;
        }
        dtt_cache_store(name, class_data, class_data_len);
        return;
    }

    if (dtt_auth_is_active()) {
        dtt_cache_store(name, class_data, class_data_len);
        dtt_log(DTT_LOG_INFO, "Authorized internal transform of protected class '%s' "
                "accepted - cache refreshed", name);
        return;
    }

    {
        dtt_thread_action_t action = dtt_threads_offend(jvmti_env, jni_env, NULL, name);

        if (action == DTT_ACTION_REFLECT) {
            unsigned char *junk = NULL;
            jvmtiError alloc_err = (*jvmti_env)->Allocate(jvmti_env, 1, &junk);
            if (alloc_err == JVMTI_ERROR_NONE && junk != NULL) {
                junk[0] = 0x00; /* invalid class-file magic -> ClassFormatError */
                *new_class_data_len = 1;
                *new_class_data = junk;
                dtt_log(DTT_LOG_WARN,
                        "REFLECTED repeat tampering attempt on protected class '%s' - the "
                        "hostile thread's own redefinition call now fails on its own thread",
                        name);
                return;
            }
        }

        has_cache = dtt_cache_lookup(name, &cached_data, &cached_len);
        if (has_cache && cached_len > 0) {
            unsigned char *replacement = NULL;
            jvmtiError alloc_err = (*jvmti_env)->Allocate(jvmti_env, cached_len, &replacement);

            if (alloc_err == JVMTI_ERROR_NONE && replacement != NULL) {
                memcpy(replacement, cached_data, (size_t)cached_len);
                *new_class_data_len = cached_len;
                *new_class_data = replacement;
                dtt_log(DTT_LOG_WARN,
                        "ABSORBED unauthorized redefinition attempt on protected class '%s' "
                        "- known-good bytecode substituted, hostile thread continues untouched",
                        name);
                return;
            }
        }

        {
            unsigned char *junk = NULL;
            jvmtiError alloc_err = (*jvmti_env)->Allocate(jvmti_env, 1, &junk);
            if (alloc_err == JVMTI_ERROR_NONE && junk != NULL) {
                junk[0] = 0x00;
                *new_class_data_len = 1;
                *new_class_data = junk;
                dtt_log(DTT_LOG_WARN,
                        "could not ABSORB tampering on protected class '%s' (no cached "
                        "baseline) - reflecting the attempt back onto the hostile thread "
                        "instead", name);
                return;
            }
        }

        dtt_log(DTT_LOG_WARN,
                "could not absorb or reflect tampering on protected class '%s' (allocation "
                "failure) - JVM continues; Java-side runtime agent defenses remain active",
                name);
    }
}

void dtt_callbacks_build(jvmtiEventCallbacks *callbacks, int class_hook_available) {
    memset(callbacks, 0, sizeof(jvmtiEventCallbacks));

    callbacks->VMInit = dtt_on_vm_init;
    callbacks->VMDeath = dtt_on_vm_death;
    callbacks->ClassPrepare = dtt_on_class_prepare;

    if (class_hook_available) {
        callbacks->ClassFileLoadHook = dtt_on_class_file_load_hook;
    } else {
        dtt_log(DTT_LOG_WARN,
                "can_generate_all_class_hook_events capability unavailable - absorb/reflect "
                "protection for protected classes is DISABLED on this JVM; shadow-class "
                "detection and the live-bytecode watchdog continue normally");
    }
}