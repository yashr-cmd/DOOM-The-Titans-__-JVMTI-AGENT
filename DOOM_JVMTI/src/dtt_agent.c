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
#include "../include/dtt_score.h"
#include "../include/dtt_cache.h"
#include "../include/dtt_callbacks.h"
#include "../include/dtt_auth.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static jvmtiEnv *g_jvmti = NULL;
static JavaVM *g_vm = NULL;
static volatile int g_active = 0;
static int g_class_hook_available = 0;
static char g_platform_string[64];

int dtt_agent_is_active(void) {
    return g_active;
}

const char *dtt_agent_platform_string(void) {
    return g_platform_string;
}

/* ---------------------------------------------------------------------------
 * Options parsing.
 *
 * Accepts a comma-separated "key=value" options string, e.g.:
 *   -agentpath:/path/to/dtt_agent.dll=suspicious=20,highrisk=50,critical=80,loglevel=debug
 *
 * All keys are optional; anything not provided keeps its compiled-in
 * default from dtt_config.h. Unknown keys are logged and ignored
 * rather than treated as a fatal error - this parser must never be
 * able to prevent the agent (and therefore the JVM) from starting.
 * ------------------------------------------------------------------------- */
typedef struct {
    int suspicious;
    int high_risk;
    int critical;
    dtt_log_level_t log_level;
} dtt_parsed_options_t;

static void dtt_parse_options(const char *options, dtt_parsed_options_t *out) {
    char buffer[1024];
    char *token;
    char *saveptr = NULL;

    out->suspicious = -1;
    out->high_risk = -1;
    out->critical = -1;
    out->log_level = DTT_LOG_INFO;

    if (options == NULL || options[0] == '\0') {
        return;
    }

    strncpy(buffer, options, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

#if defined(DTT_OS_WINDOWS)
    token = strtok_s(buffer, ",", &saveptr);
#else
    token = strtok_r(buffer, ",", &saveptr);
#endif

    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq != NULL) {
            *eq = '\0';
            {
                const char *key = token;
                const char *value = eq + 1;

                if (strcmp(key, "suspicious") == 0) {
                    out->suspicious = atoi(value);
                } else if (strcmp(key, "highrisk") == 0 || strcmp(key, "high_risk") == 0) {
                    out->high_risk = atoi(value);
                } else if (strcmp(key, "critical") == 0) {
                    out->critical = atoi(value);
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
        }
#if defined(DTT_OS_WINDOWS)
        token = strtok_s(NULL, ",", &saveptr);
#else
        token = strtok_r(NULL, ",", &saveptr);
#endif
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

    /* Step 2: negotiate capabilities. We only ever request the single
     * capability this agent actually needs beyond the always-available
     * baseline events (VMInit/VMDeath/ClassPrepare need nothing extra).
     * If the JVM can't grant it, we disable ClassFileLoadHook-based
     * tamper detection/isolation but keep everything else running. */
    memset(&caps, 0, sizeof(caps));
    caps.can_generate_all_class_hook_events = 1;

    err = (*jvmti)->AddCapabilities(jvmti, &caps);
    if (err == JVMTI_ERROR_NONE) {
        g_class_hook_available = 1;
        dtt_log(DTT_LOG_INFO, "can_generate_all_class_hook_events capability acquired");
    } else {
        g_class_hook_available = 0;
        dtt_log(DTT_LOG_WARN,
                "could not acquire can_generate_all_class_hook_events (err=%d) - continuing "
                "with reduced (detection-only-via-ClassPrepare) protection", (int)err);
    }

    /* Step 3: initialize internal state modules before any callback
     * could possibly fire. */
    dtt_score_init(parsed.suspicious, parsed.high_risk, parsed.critical);
    dtt_cache_init();
    dtt_callbacks_module_init();

    /* Step 4: register only the callbacks we actually use. */
    dtt_callbacks_build(&callbacks, g_class_hook_available);
    err = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
    if (err != JVMTI_ERROR_NONE) {
        dtt_log(DTT_LOG_ERROR,
                "SetEventCallbacks failed (err=%d) - native integrity monitoring will be "
                "UNAVAILABLE for this session; Minecraft will continue normally", (int)err);
        dtt_callbacks_module_shutdown();
        dtt_cache_shutdown();
        dtt_score_shutdown();
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
    dtt_callbacks_module_shutdown();
    dtt_cache_shutdown();
    dtt_score_shutdown();

    g_jvmti = NULL;
    g_vm = NULL;
}
