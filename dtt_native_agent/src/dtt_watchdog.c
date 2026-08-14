/* ============================================================================
 * dtt_watchdog.c - see dtt_watchdog.h for design notes.
 * ============================================================================ */

#include "../include/dtt_watchdog.h"
#include "../include/dtt_config.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_log.h"
#include "../include/dtt_cache.h"
#include "../include/dtt_auth.h"
#include "../include/dtt_protected.h"

#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Per-method tracking table: one entry per method we know how to
 * re-check. Keyed by (class_name, jmethodID). jmethodIDs are only
 * guaranteed stable until the declaring class is redefined, so any
 * successful repair or legitimate re-track for a class must refresh
 * every entry for that class name (dtt_watchdog_track_class does this
 * by clearing old entries for the class before adding fresh ones).
 * ------------------------------------------------------------------------- */
typedef struct {
    char class_name[DTT_CACHE_NAME_MAX];
    jmethodID method;
    unsigned long baseline_hash;
    int in_use;
} dtt_method_entry_t;

static dtt_method_entry_t g_methods[DTT_METHOD_TRACK_CAPACITY];
static dtt_mutex_t g_methods_mutex;
static int g_methods_ready = 0;

static volatile int g_stop_requested = 0;
static volatile int g_watchdog_running = 0;

/* FNV-1a: fast, dependency-free, more than sufficient for detecting
 * "did these bytes change" - this is a tamper-evidence check, not a
 * cryptographic integrity guarantee, so collision resistance beyond
 * "won't happen by accident" is not required. */
static unsigned long dtt_fnv1a(const unsigned char *data, jint len) {
    unsigned long hash = 2166136261UL;
    jint i;
    for (i = 0; i < len; i++) {
        hash ^= (unsigned long)data[i];
        hash *= 16777619UL;
    }
    return hash;
}

void dtt_watchdog_module_init(void) {
    memset(g_methods, 0, sizeof(g_methods));
    DTT_MUTEX_INIT(&g_methods_mutex);
    g_methods_ready = 1;
    g_stop_requested = 0;
    g_watchdog_running = 0;
}

void dtt_watchdog_module_shutdown(void) {
    if (!g_methods_ready) {
        return;
    }
    DTT_MUTEX_DESTROY(&g_methods_mutex);
    g_methods_ready = 0;
}

void dtt_watchdog_request_stop(void) {
    g_stop_requested = 1;
}

/* Removes every existing entry for class_name (used before re-adding
 * fresh entries, so a re-track never leaves stale jmethodIDs behind).
 * Caller must hold g_methods_mutex. */
static void dtt_watchdog_clear_class_locked(const char *class_name) {
    int i;
    for (i = 0; i < DTT_METHOD_TRACK_CAPACITY; i++) {
        if (g_methods[i].in_use && strncmp(g_methods[i].class_name, class_name, DTT_CACHE_NAME_MAX) == 0) {
            g_methods[i].in_use = 0;
        }
    }
}

static int dtt_watchdog_add_locked(const char *class_name, jmethodID method, unsigned long hash) {
    int i;
    for (i = 0; i < DTT_METHOD_TRACK_CAPACITY; i++) {
        if (!g_methods[i].in_use) {
            strncpy(g_methods[i].class_name, class_name, DTT_CACHE_NAME_MAX - 1);
            g_methods[i].class_name[DTT_CACHE_NAME_MAX - 1] = '\0';
            g_methods[i].method = method;
            g_methods[i].baseline_hash = hash;
            g_methods[i].in_use = 1;
            return 1;
        }
    }
    return 0; /* table full - fail-safe: this method just won't be watched */
}

void dtt_watchdog_track_class(jvmtiEnv *jvmti_env, jclass klass, const char *class_name) {
    jint method_count = 0;
    jmethodID *methods = NULL;
    jvmtiError err;
    int i;

    if (!g_methods_ready || jvmti_env == NULL || klass == NULL || class_name == NULL) {
        return;
    }
    if (!dtt_is_protected_class(class_name)) {
        return; /* never track anything outside our own packages */
    }

    err = (*jvmti_env)->GetClassMethods(jvmti_env, klass, &method_count, &methods);
    if (err != JVMTI_ERROR_NONE || methods == NULL) {
        /* Not fatal - GetClassMethods can fail if can_get_bytecodes-adjacent
         * capabilities are unavailable, or for a handful of edge-case
         * classes (e.g. some proxy/lambda forms). Skip tracking this
         * class rather than treating it as an error. */
        return;
    }

    DTT_MUTEX_LOCK(&g_methods_mutex);
    dtt_watchdog_clear_class_locked(class_name);

    for (i = 0; i < method_count; i++) {
        unsigned char *bytecode = NULL;
        jint bytecode_len = 0;

        err = (*jvmti_env)->GetBytecodes(jvmti_env, methods[i], &bytecode_len, &bytecode);
        if (err == JVMTI_ERROR_NONE && bytecode != NULL) {
            unsigned long hash = dtt_fnv1a(bytecode, bytecode_len);
            if (!dtt_watchdog_add_locked(class_name, methods[i], hash)) {
                dtt_log(DTT_LOG_WARN,
                        "method-tracking table full (%d entries) - some methods of '%s' will "
                        "not be watched by the live-bytecode integrity check",
                        DTT_METHOD_TRACK_CAPACITY, class_name);
            }
            (*jvmti_env)->Deallocate(jvmti_env, bytecode);
        }
        /* Abstract/native methods have no bytecode (JVMTI_ERROR_NULL_POINTER
         * or similar) - simply not tracked, which is correct, there is
         * nothing to tamper with there. */
    }

    DTT_MUTEX_UNLOCK(&g_methods_mutex);
    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)methods);
}

/* Attempts to push our cached known-good bytecode back in for a class
 * whose live bytecode was found to have drifted. Opens our OWN
 * authorized-transform window first (this is DTT's own agent thread
 * performing a sanctioned repair, not an external actor - without
 * this, our own ClassFileLoadHook would immediately re-flag the
 * repair it is about to observe as "unauthorized"). Returns 1 on
 * success, 0 otherwise. */
static int dtt_watchdog_attempt_reflect(jvmtiEnv *jvmti_env, jclass klass, const char *class_name) {
    const unsigned char *cached_data = NULL;
    jint cached_len = 0;
    jvmtiClassDefinition def;
    jvmtiError err;

    if (!dtt_cache_lookup(class_name, &cached_data, &cached_len) || cached_len <= 0) {
        return 0; /* nothing known-good to restore yet */
    }

    def.klass = klass;
    def.class_byte_count = cached_len;
    def.class_bytes = cached_data;

    dtt_auth_begin();
    err = (*jvmti_env)->RedefineClasses(jvmti_env, 1, &def);
    dtt_auth_end();

    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_WARN, "RedefineClasses repair for '%s' failed (err=%d)", class_name, (int)err);
        return 0;
    }

    /* Redefinition invalidates jmethodIDs for changed methods - re-track
     * the class from scratch so future scans use valid IDs and a hash
     * baseline that matches what we just restored. */
    dtt_watchdog_track_class(jvmti_env, klass, class_name);
    return 1;
}

/* One full pass over every tracked method. Runs on the watchdog's own
 * agent thread. */
static void dtt_watchdog_scan_once(jvmtiEnv *jvmti_env, int redefine_capability_available) {
    int i;
    /* Snapshot indices to check outside the lock so a slow GetBytecodes/
     * RedefineClasses call for one entry never blocks tracking updates
     * (e.g. from ClassPrepare on another thread) for the whole table. */
    for (i = 0; i < DTT_METHOD_TRACK_CAPACITY; i++) {
        char class_name[DTT_CACHE_NAME_MAX];
        jmethodID method;
        unsigned long baseline;
        int is_tracked;
        unsigned char *bytecode = NULL;
        jint bytecode_len = 0;
        jvmtiError err;

        DTT_MUTEX_LOCK(&g_methods_mutex);
        is_tracked = g_methods[i].in_use;
        if (is_tracked) {
            strncpy(class_name, g_methods[i].class_name, DTT_CACHE_NAME_MAX - 1);
            class_name[DTT_CACHE_NAME_MAX - 1] = '\0';
            method = g_methods[i].method;
            baseline = g_methods[i].baseline_hash;
        }
        DTT_MUTEX_UNLOCK(&g_methods_mutex);

        if (!is_tracked) {
            continue;
        }

        err = (*jvmti_env)->GetBytecodes(jvmti_env, method, &bytecode_len, &bytecode);
        if (err != JVMTI_ERROR_NONE || bytecode == NULL) {
            /* Method/class likely went away (unloaded, or jmethodID stale
             * from an unrelated redefinition) - not itself suspicious,
             * just drop this stale entry. */
            DTT_MUTEX_LOCK(&g_methods_mutex);
            if (g_methods[i].in_use && g_methods[i].method == method) {
                g_methods[i].in_use = 0;
            }
            DTT_MUTEX_UNLOCK(&g_methods_mutex);
            continue;
        }

        {
            unsigned long current = dtt_fnv1a(bytecode, bytecode_len);
            (*jvmti_env)->Deallocate(jvmti_env, bytecode);

            if (current == baseline) {
                continue; /* untouched - the common case */
            }

            /* Bytecode changed without ever going through
             * ClassFileLoadHook - i.e. not via RedefineClasses/
             * RetransformClasses. This is the out-of-band tamper
             * pattern (e.g. sun.misc.Unsafe method-body overwrite).
             * There is no single thread we can attribute this to, so
             * the only response is to reflect it: push the known-good
             * bytecode back in via RedefineClasses. */
            dtt_log(DTT_LOG_WARN,
                    "live bytecode drift detected on protected class '%s' outside the "
                    "normal class-load/redefinition path - method body changed in place",
                    class_name);

            if (redefine_capability_available) {
                jclass declaring = NULL;
                jvmtiError decl_err = (*jvmti_env)->GetMethodDeclaringClass(jvmti_env, method, &declaring);

                if (decl_err == JVMTI_ERROR_NONE && declaring != NULL) {
                    if (dtt_watchdog_attempt_reflect(jvmti_env, declaring, class_name)) {
                        dtt_log(DTT_LOG_INFO,
                                "reflected live bytecode drift on '%s' - known-good bytecode "
                                "restored via RedefineClasses", class_name);
                    } else {
                        dtt_log(DTT_LOG_WARN,
                                "could not reflect live bytecode drift on '%s' (no cached "
                                "baseline or RedefineClasses failed) - reported only, JVM "
                                "continues", class_name);
                    }
                } else {
                    dtt_log(DTT_LOG_WARN,
                            "could not reflect live bytecode drift on '%s' (declaring class "
                            "lookup failed) - reported only, JVM continues", class_name);
                }
            } else {
                dtt_log(DTT_LOG_WARN,
                        "could not reflect live bytecode drift on '%s' (can_redefine_classes "
                        "unavailable on this JVM) - reported only, JVM continues",
                        class_name);
            }
        }
    }
}

/* The watchdog's main loop, run as a proper JVMTI agent thread (see
 * dtt_watchdog_start). Never touches anything outside protected DTT
 * classes, and never touches any thread - it only re-reads and
 * (optionally) redefines bytecode. */
typedef struct {
    int interval_ms;
    int redefine_capability_available;
} dtt_watchdog_args_t;

static void JNICALL dtt_watchdog_proc(jvmtiEnv *jvmti_env, JNIEnv *jni_env, void *arg) {
    dtt_watchdog_args_t *args = (dtt_watchdog_args_t *)arg;
    int interval_ms = args->interval_ms;
    int redefine_capability_available = args->redefine_capability_available;

    (void)jni_env;
    free(args);

    g_watchdog_running = 1;
    dtt_log(DTT_LOG_INFO, "live bytecode integrity watchdog started (interval=%dms, repair=%s)",
            interval_ms, redefine_capability_available ? "enabled" : "detect-only");

    while (!g_stop_requested) {
        dtt_sleep_ms((unsigned int)interval_ms);
        if (g_stop_requested) {
            break;
        }
        dtt_watchdog_scan_once(jvmti_env, redefine_capability_available);
    }

    dtt_log(DTT_LOG_INFO, "live bytecode integrity watchdog stopped");
    g_watchdog_running = 0;
}

void dtt_watchdog_start(jvmtiEnv *jvmti_env, JNIEnv *jni_env, int scan_interval_ms,
                         int bytecode_capability_available, int redefine_capability_available) {
    jclass thread_class;
    jmethodID ctor;
    jobject thread_obj;
    jvmtiError err;
    dtt_watchdog_args_t *args;
    int interval = scan_interval_ms;

    if (!bytecode_capability_available) {
        dtt_log(DTT_LOG_WARN,
                "can_get_bytecodes unavailable - live bytecode integrity watchdog DISABLED; "
                "only ClassFileLoadHook-based detection remains active");
        return;
    }

    if (interval < DTT_WATCHDOG_MIN_INTERVAL_MS) {
        interval = DTT_WATCHDOG_DEFAULT_INTERVAL_MS;
    }

    /* RunAgentThread needs a java.lang.Thread object to represent the
     * native thread it creates - build a plain, unstarted one via JNI. */
    thread_class = (*jni_env)->FindClass(jni_env, "java/lang/Thread");
    if (thread_class == NULL) {
        dtt_log(DTT_LOG_WARN, "could not find java/lang/Thread - watchdog thread not started");
        return;
    }
    ctor = (*jni_env)->GetMethodID(jni_env, thread_class, "<init>", "()V");
    if (ctor == NULL) {
        dtt_log(DTT_LOG_WARN, "could not find Thread() constructor - watchdog thread not started");
        return;
    }
    thread_obj = (*jni_env)->NewObject(jni_env, thread_class, ctor);
    if (thread_obj == NULL) {
        dtt_log(DTT_LOG_WARN, "could not construct Thread object - watchdog thread not started");
        return;
    }

    args = (dtt_watchdog_args_t *)malloc(sizeof(dtt_watchdog_args_t));
    if (args == NULL) {
        dtt_log(DTT_LOG_WARN, "allocation failed - watchdog thread not started");
        return;
    }
    args->interval_ms = interval;
    args->redefine_capability_available = redefine_capability_available;

    err = (*jvmti_env)->RunAgentThread(jvmti_env, thread_obj, dtt_watchdog_proc, args,
                                        JVMTI_THREAD_MIN_PRIORITY);
    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_WARN, "RunAgentThread failed (err=%d) - watchdog thread not started", (int)err);
        free(args);
        return;
    }

    if (!redefine_capability_available) {
        dtt_log(DTT_LOG_WARN,
                "can_redefine_classes unavailable - watchdog will detect and report live "
                "bytecode drift but cannot automatically restore known-good bytecode");
    }
}
