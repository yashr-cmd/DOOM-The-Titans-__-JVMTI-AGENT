/* ============================================================================
 * dtt_callbacks.c - see dtt_callbacks.h for design notes.
 * ============================================================================ */

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

#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Shadow-classloader tracking table.
 *
 * Keyed by protected class name, records a weak global reference to the
 * ClassLoader that first prepared this class, so that a protected class
 * name being prepared by an unexpected second ClassLoader instance (the
 * classic "shadow class" pattern) can be recognized in both ClassPrepare
 * and ClassFileLoadHook.
 *
 * This is separate from dtt_cache.c (which stores bytecode) because the
 * two have different lifetimes and different JNI/JVMTI requirements (this
 * table holds a jweak that must be released with DeleteWeakGlobalRef; the
 * bytecode cache is plain malloc'd memory).
 * ------------------------------------------------------------------------- */
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
            g_track[i].in_use = 1;
            return &g_track[i];
        }
    }
    return NULL; /* table full - fail-safe: caller just skips tracking for this class */
}

/* Returns 1 if `loader` is trying to define `class_name` and a DIFFERENT
 * ClassLoader instance was recorded as the one that first loaded that
 * name - i.e. this is a shadow-class load. Returns 0 (fail-safe) if there
 * is no recorded loader yet, the loader is NULL, the recorded loader has
 * been collected, or the class is not tracked. Never touches or mutates
 * anything - observation only. */
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
            /* Recorded loader was garbage-collected - cannot compare,
             * fail-safe: assume this load is legitimate. */
            is_shadow = 0;
        }
    }
    DTT_MUTEX_UNLOCK(&g_track_mutex);

    return is_shadow;
}

/* ---------------------------------------------------------------------------
 * VMInit / VMDeath - lifecycle logging only. No capability requirements.
 * ------------------------------------------------------------------------- */
static void JNICALL dtt_on_vm_init(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread) {
    (void)thread;
    dtt_log(DTT_LOG_INFO, "JVM initialization complete - DTT native integrity agent is live");

    /* Start the live-bytecode watchdog here (not in Agent_OnLoad) because
     * RunAgentThread needs a fully-initialized JVM plus a JNIEnv to
     * construct the java.lang.Thread it runs on - both are only
     * guaranteed available once VMInit has fired. */
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

/* ---------------------------------------------------------------------------
 * ClassPrepare - shadow-classloader detection for protected classes only.
 * We never veto or delay loading here (JVMTI does not allow that from
 * ClassPrepare); we only observe. The actual absorption of shadow loads
 * happens earlier, in ClassFileLoadHook (which fires before the class is
 * defined and can substitute known-good bytecode).
 * ------------------------------------------------------------------------- */
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
        return; /* not one of ours - none of our business */
    }

    /* Register/refresh this class's methods with the live-bytecode
     * watchdog regardless of the shadow-loader outcome below - even a
     * legitimately-reloaded protected class needs a fresh baseline. */
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
     * technique for smuggling in a fake replacement. Flag the thread
     * that did it so it is absorbed/reflected from now on. */
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

/* ---------------------------------------------------------------------------
 * ClassFileLoadHook - the core detection + absorb/reflect logic.
 *
 * Behavior summary:
 *   1. name == NULL or not a protected class -> ignore completely.
 *      This agent NEVER inspects, absorbs, reflects, or interferes with
 *      bytecode belonging to anything other than DTT's own classes.
 *   2. First (non-redefinition) load of a protected class:
 *        - by its original loader         -> cache as "known good", let load.
 *        - by a DIFFERENT loader (shadow) -> flag the loading thread and
 *          absorb: substitute known-good bytecode so the smuggled version
 *          never lands.
 *   3. Redefinition/retransformation of a protected class while the
 *      calling thread is inside an authorized-transform window (see
 *      dtt_auth.h) -> the DTT Java runtime agent doing its own legitimate
 *      patching. Refresh the cache and let it through.
 *   4. Redefinition/retransformation of a protected class WITHOUT
 *      authorization -> the calling thread is flagged ("absorbed") and:
 *        - first offense  -> ABSORB: substitute cached known-good
 *                            bytecode; the hostile thread keeps running
 *                            normally, unaware.
 *        - repeat offense -> REFLECT: hand the JVM deliberately-invalid
 *                            class data so the hostile redefinition call
 *                            fails with a ClassFormatError thrown on the
 *                            offending thread itself - its own attack
 *                            bounced back onto it. We never suspend,
 *                            stop, interrupt, or kill any thread.
 *   5. If absorption is impossible (no cached baseline) we reflect
 *      instead; if even that fails (allocation error) we let the JVM
 *      continue and rely on the Java-side runtime agent - we never
 *      throw, abort, or otherwise risk crashing Minecraft.
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

    const unsigned char *cached_data = NULL;
    jint cached_len = 0;
    int has_cache;

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
        /* Ordinary first-time class load, not a redefinition. */
        if (dtt_is_shadow_load(jvmti_env, jni_env, loader, name)) {
            /* A different ClassLoader is trying to smuggle in a class under
             * the name of one of ours. Flag the loading thread and absorb
             * the load with our known-good bytes. (We deliberately never
             * REFLECT a first-time load: returning invalid bytes there
             * would throw on the loading thread, which is indistinguishable
             * from a legitimate loader in this path.) */
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

        /* Legitimate first load (or a load we cannot classify) - cache the
         * bytes as the known-good baseline and let it proceed unmodified. */
        dtt_cache_store(name, class_data, class_data_len);
        return;
    }

    /* From here on: this IS a redefinition/retransformation of one of
     * OUR protected classes. */

    if (dtt_auth_is_active()) {
        /* The DTT Java runtime agent told us (via the JNI bridge) that
         * it is about to legitimately patch its own classes. Trust it
         * and refresh our known-good copy. */
        dtt_cache_store(name, class_data, class_data_len);
        dtt_log(DTT_LOG_INFO, "Authorized internal transform of protected class '%s' "
                "accepted - cache refreshed", name);
        return;
    }

    /* Unauthorized redefinition attempt against a protected class.
     * Identify and flag the offending thread, then absorb (first offense)
     * or reflect (repeat offense on the same class). */
    {
        dtt_thread_action_t action = dtt_threads_offend(jvmti_env, jni_env, NULL, name);

        if (action == DTT_ACTION_REFLECT) {
            /* Repeat offender on this class: bounce the attempt back onto
             * the hostile thread itself. Handing the JVM deliberately-
             * invalid class data makes the hostile redefinition call fail
             * with a ClassFormatError thrown on the offending thread - its
             * own attack reflected back at it. The protected class is never
             * touched. No suspend/stop/interrupt/kill is ever used. */
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
            /* Allocation failed - fall through to the absorb attempt below. */
        }

        /* Absorb: substitute our cached known-good bytecode so the
         * tampering never lands, and the hostile thread keeps running
         * normally (it simply observes either success-with-our-bytes or,
         * if the JVM reports it, its call being neutralized). */
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

        /* No cached baseline (or allocation failed): we cannot absorb, so
         * the only safe defense left is to reflect the attempt - make it
         * fail on the offending thread - rather than let unknown bytecode
         * land. This still never touches, suspends, or kills anything. */
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

        /* Even reflection failed (out of memory). We deliberately do NOT
         * try to block the redefinition by other means because that risks
         * destabilizing or crashing the JVM, which this agent must never
         * do. Let Minecraft continue; the Java-side DTT runtime agent can
         * still re-apply its own protections. */
        dtt_log(DTT_LOG_WARN,
                "could not absorb or reflect tampering on protected class '%s' (allocation "
                "failure) - JVM continues; Java-side runtime agent defenses remain active",
                name);
    }
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
                "can_generate_all_class_hook_events capability unavailable - absorb/reflect "
                "protection for protected classes is DISABLED on this JVM; shadow-class "
                "detection and the live-bytecode watchdog continue normally");
    }
}
