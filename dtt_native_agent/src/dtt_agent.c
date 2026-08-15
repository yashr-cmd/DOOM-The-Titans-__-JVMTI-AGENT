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

    rc = (*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1_2);
    if (rc != JNI_OK || jvmti == NULL) {
        dtt_log(DTT_LOG_ERROR,
                "GetEnv(JVMTI_VERSION_1_2) failed (rc=%d) - native integrity monitoring will "
                "be UNAVAILABLE for this session; the Java-side DTT runtime agent will fall "
                "back to its own protections and Minecraft will continue normally", (int)rc);
        return JNI_OK;
    }
    g_jvmti = jvmti;

    memset(&caps, 0, sizeof(caps));
    caps.can_generate_all_class_hook_events = 1;
    caps.can_get_bytecodes = 1;
    caps.can_redefine_classes = 1;

    err = (*jvmti)->AddCapabilities(jvmti, &caps);
    if (err == JVMTI_ERROR_NONE) {
    }
    {
        jvmtiCapabilities granted;
        memset(&granted, 0, sizeof(granted));
        if ((*jvmti)->GetCapabilities(jvmti, &granted) == JVMTI_ERROR_NONE) {
            g_class_hook_available = granted.can_generate_all_class_hook_events ? 1 : 0;
            g_bytecode_capability_available = granted.can_get_bytecodes ? 1 : 0;
            g_redefine_capability_available = granted.can_redefine_classes ? 1 : 0;
        } else {
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

    dtt_threads_module_init();
    dtt_threads_configure(parsed.reflect_after_hits);
    dtt_cache_init();
    dtt_callbacks_module_init();
    dtt_watchdog_module_init();

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

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;
    return dtt_agent_init(vm, options);
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;
    return dtt_agent_init(vm, options);
}

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