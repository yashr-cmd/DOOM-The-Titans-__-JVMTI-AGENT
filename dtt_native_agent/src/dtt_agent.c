/* ============================================================================
 * dtt_agent.c
 *
 * Entry point / lifecycle glue for the DTT native integrity agent.
 * Implements the three standard JVMTI agent entry points the JVM looks
 * up by name in the compiled shared library:
 *
 *   Agent_OnLoad    - called when the JVM starts with
 *                      -agentlib:dtt_agent or -agentpath:/path/to/dtt_agent.{dll,so,dylib}
 *   Agent_OnAttach  - called when the agent is loaded dynamically after
 *                      the JVM is already running, via the Attach API
 *                      (matches the existing AttachHelper.java workflow
 *                      used elsewhere in this project's Java tooling)
 *   Agent_OnUnload  - called during JVM shutdown for orderly cleanup
 *
 * FAIL-SAFE CONTRACT: Agent_OnLoad/Agent_OnAttach ALWAYS return JNI_OK,
 * even if something inside initialization fails. Per the JVMTI spec,
 * returning anything other than JNI_OK from Agent_OnLoad prevents the
 * JVM from starting at all - and this agent must never be capable of
 * stopping Minecraft from launching. Every failure path below instead
 * disables the specific piece of functionality that failed and logs a
 * warning, leaving the rest of the agent (or, in the worst case, an
 * entirely inert agent) in place.
 * ============================================================================ */

#include "../include/dtt_agent.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_config.h"
#include "../include/dtt_log.h"
#include "../include/dtt_cache.h"
#include "../include/dtt_callbacks.h"
#include "../include/dtt_auth.h"
#include "../include/dtt_watchdog.h"
#include "../include/dtt_threads.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static jvmtiEnv *g_jvmti = NULL;
static JavaVM *g_vm = NULL;
static volatile int g_active = 0;
static int g_class_hook_available = 0;
static int g_bytecode_capability_available = 0;
static int g_redefine_capability_available = 0;
static int g_scan_interval_ms = DTT_WATCHDOG_DEFAULT_INTERVAL_MS;
static char g_platform_string[64];

int dtt_agent_is_active(void) {
    return g_active;
}

const char *dtt_agent_platform_string(void) {
    return g_platform_string;
}

int dtt_agent_bytecode_capability(void) {
    return g_bytecode_capability_available;
}

int dtt_agent_redefine_capability(void) {
    return g_redefine_capability_available;
}

int dtt_agent_scan_interval_ms(void) {
    return g_scan_interval_ms;
}

/* ---------------------------------------------------------------------------
 * Options parsing.
 *
 * Accepts a comma-separated "key=value" options string, e.g.:
 *   -agentpath:/path/to/dtt_agent.dll=reflectafter=2,scaninterval=4000,loglevel=debug
 *
 * All keys are optional; anything not provided keeps its compiled-in
 * default from dtt_config.h. Unknown keys are logged and ignored
 * rather than treated as a fatal error - this parser must never be
 * able to prevent the agent (and therefore the JVM) from starting.
 * ------------------------------------------------------------------------- */
typedef struct {
    int scan_interval_ms;
    int reflect_after_hits;
    dtt_log_level_t log_level;
} dtt_parsed_options_t;

static void dtt_parse_options(const char *options, dtt_parsed_options_t *out) {
    const char *p;

    out->scan_interval_ms = -1;
    out->reflect_after_hits = -1;
    out->log_level = DTT_LOG_INFO;

    if (options == NULL || options[0] == '\0') {
        return;
    }

    /* Manual tokenizer: strtok_s/strtok_r are not portable across the
     * MSVC/MinGW CRT split, so split on ',' ourselves. */
    p = options;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *comma = strchr(p, ',');
        const char *end;
        char key[64];
        char value[128];
        int key_len;
        int val_len;

        if (comma == NULL) {
            comma = p + strlen(p);
        }
        end = (eq != NULL && eq < comma) ? eq : comma;

        key_len = (int)(end - p);
        if (key_len > (int)sizeof(key) - 1) {
            key_len = (int)sizeof(key) - 1;
        }
        strncpy(key, p, (size_t)key_len);
        key[key_len] = '\0';

        if (eq != NULL && eq < comma) {
            val_len = (int)(comma - (eq + 1));
            if (val_len > (int)sizeof(value) - 1) {
                val_len = (int)sizeof(value) - 1;
            }
            strncpy(value, eq + 1, (size_t)val_len);
            value[val_len] = '\0';

            if (strcmp(key, "scaninterval") == 0 || strcmp(key, "scan_interval_ms") == 0) {
                out->scan_interval_ms = atoi(value);
            } else if (strcmp(key, "reflectafter") == 0 || strcmp(key, "reflect_after_hits") == 0) {
                out->reflect_after_hits = atoi(value);
            } else if (strcmp(key, "loglevel") == 0 || strcmp(key, "log_level") == 0) {
                if (strcmp(value, "error") == 0) out->log_level = DTT_LOG_ERROR;
                else if (strcmp(value, "warn") == 0) out->log_level = DTT_LOG_WARN;
                else if (strcmp(value, "info") == 0) out->log_level = DTT_LOG_INFO;
                else if (strcmp(value, "debug") == 0) out->log_level = DTT_LOG_DEBUG;
            } else {
                /* Unknown option key - not fatal, just ignored. */
                fprintf(stderr, "[DTT-Native][WARN ] ignoring unknown agent option '%s'\n", key);
            }
        }

        if (*comma == ',') {
            p = comma + 1;
        } else {
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Shared init logic for Agent_OnLoad and Agent_OnAttach.
 * ------------------------------------------------------------------------- */
static jint dtt_agent_init(JavaVM *vm, char *options) {
    jvmtiEnv *jvmti = NULL;
    jvmtiError err;
    jvmtiCapabilities caps;
    jvmtiEventCallbacks callbacks;
    dtt_parsed_options_t parsed;
    jint rc;

    snprintf(g_platform_string, sizeof(g_platform_string), "%s/%s", DTT_PLATFORM_NAME, DTT_ARCH_NAME);

    dtt_parse_options(options, &parsed);
    dtt_log_set_level(parsed.log_level);

    dtt_log(DTT_LOG_INFO, "DTT native integrity agent starting up (%s)", g_platform_string);

    g_vm = vm;

    /* Step 1: obtain the JVMTI environment. If this fails outright,
     * there is nothing else we can safely do - log it and bail out to
     * "inert" state. We still return JNI_OK (see file header comment):
     * Minecraft must be able to start with or without us. */
    rc = (*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1_2);
    if (rc != JNI_OK || jvmti == NULL) {
        dtt_log(DTT_LOG_ERROR,
                "GetEnv(JVMTI_VERSION_1_2) failed (rc=%d) - native integrity monitoring will "
                "be UNAVAILABLE for this session; the Java-side DTT runtime agent will fall "
                "back to its own protections and Minecraft will continue normally", (int)rc);
        return JNI_OK;
    }
    g_jvmti = jvmti;

    /* Step 2: negotiate capabilities. Each is requested and checked
     * independently, and every one is optional in the sense that
     * losing it only narrows what the agent can do - it never aborts
     * startup. Three capabilities, three independent jobs:
     *   can_generate_all_class_hook_events - ClassFileLoadHook, catches
     *       tampering that goes through RedefineClasses/RetransformClasses.
     *   can_get_bytecodes - lets the watchdog (dtt_watchdog.c) read a
     *       method's CURRENT live bytecode, to catch tampering that
     *       does NOT go through those APIs (e.g. direct memory patching
     *       via sun.misc.Unsafe).
     *   can_redefine_classes - lets the watchdog push known-good
     *       bytecode back in when it finds drift. Without it, the
     *       watchdog still detects and reports drift, it just can't
     *       repair it. */
    memset(&caps, 0, sizeof(caps));
    caps.can_generate_all_class_hook_events = 1;
    caps.can_get_bytecodes = 1;
    caps.can_redefine_classes = 1;

    err = (*jvmti)->AddCapabilities(jvmti, &caps);
    if (err == JVMTI_ERROR_NONE) {
        /* AddCapabilities can partially fail on some JVMs even while
         * returning an error only for the capabilities it couldn't
         * grant - re-check what we actually got via GetCapabilities so
         * we degrade precisely instead of all-or-nothing. */
    }
    {
        jvmtiCapabilities granted;
        memset(&granted, 0, sizeof(granted));
        if ((*jvmti)->GetCapabilities(jvmti, &granted) == JVMTI_ERROR_NONE) {
            g_class_hook_available = granted.can_generate_all_class_hook_events ? 1 : 0;
            g_bytecode_capability_available = granted.can_get_bytecodes ? 1 : 0;
            g_redefine_capability_available = granted.can_redefine_classes ? 1 : 0;
        } else {
            /* Could not even confirm what we have - fail-safe: assume
             * nothing was granted and run fully inert on the class/
             * bytecode side rather than guess. */
            g_class_hook_available = 0;
            g_bytecode_capability_available = 0;
            g_redefine_capability_available = 0;
        }
    }

    dtt_log(DTT_LOG_INFO,
            "capabilities: class-hook=%s bytecode-read=%s redefine=%s",
            g_class_hook_available ? "yes" : "no",
            g_bytecode_capability_available ? "yes" : "no",
            g_redefine_capability_available ? "yes" : "no");

    if (parsed.scan_interval_ms > 0) {
        g_scan_interval_ms = parsed.scan_interval_ms;
    }

    /* Step 3: initialize internal state modules before any callback
     * could possibly fire. */
    dtt_threads_module_init();
    dtt_threads_configure(parsed.reflect_after_hits);
    dtt_cache_init();
    dtt_callbacks_module_init();
    dtt_watchdog_module_init();

    /* Step 4: register only the callbacks we actually use. */
    dtt_callbacks_build(&callbacks, g_class_hook_available);
    err = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_ERROR,
                "SetEventCallbacks failed (err=%d) - native integrity monitoring will be "
                "UNAVAILABLE for this session; Minecraft will continue normally", (int)err);
        dtt_watchdog_module_shutdown();
        dtt_callbacks_module_shutdown();
        dtt_cache_shutdown();
        dtt_threads_module_shutdown();
        g_jvmti = NULL;
        return JNI_OK;
    }

    /* Step 5: enable notifications, one event at a time so a failure
     * on one doesn't take the others down with it. */
    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_WARN, "failed to enable VM_INIT event (err=%d)", (int)err);
    }

    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL);
    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_WARN, "failed to enable VM_DEATH event (err=%d)", (int)err);
    }

    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, NULL);
    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_WARN, "failed to enable CLASS_PREPARE event (err=%d)", (int)err);
    }

    if (g_class_hook_available) {
        err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
        if (err != JVMTI_ERROR_NONE) {
            dtt_log(DTT_LOG_WARN,
                    "failed to enable CLASS_FILE_LOAD_HOOK event (err=%d) - tamper "
                    "detection/isolation disabled for this session", (int)err);
            g_class_hook_available = 0;
        }
    }

    g_active = 1;
    dtt_log(DTT_LOG_INFO,
            "DTT native integrity agent initialized successfully (class-hook protection: %s)",
            g_class_hook_available ? "ENABLED" : "DISABLED");

    return JNI_OK;
}

/* ---------------------------------------------------------------------------
 * Standard JVMTI agent entry points.
 * ------------------------------------------------------------------------- */

/* Called when the JVM starts with -agentlib: or -agentpath:. */
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;
    return dtt_agent_init(vm, options);
}

/* Called when the agent is attached to an already-running JVM via the
 * Attach API (com.sun.tools.attach.VirtualMachine#loadAgentPath),
 * mirroring the dynamic-attach workflow already used by this
 * project's AttachHelper.java on the Java side. */
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;
    return dtt_agent_init(vm, options);
}

/* Called during JVM shutdown. Cleanup here is best-effort and must
 * never block or throw - the JVM is already tearing down. */
JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;

    if (!g_active) {
        return;
    }

    dtt_log(DTT_LOG_INFO, "DTT native integrity agent shutting down");

    g_active = 0;
    dtt_watchdog_request_stop();
    dtt_callbacks_module_shutdown();
    dtt_cache_shutdown();
    dtt_threads_module_shutdown();

    g_jvmti = NULL;
    g_vm = NULL;
}
