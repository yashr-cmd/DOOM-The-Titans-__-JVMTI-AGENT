#ifndef DTT_WATCHDOG_H
#define DTT_WATCHDOG_H

#include <jvmti.h>
#include <jni.h>

void dtt_watchdog_module_init(void);
void dtt_watchdog_module_shutdown(void);
void dtt_watchdog_track_class(jvmtiEnv *jvmti_env, jclass klass, const char *class_name);
void dtt_watchdog_start(jvmtiEnv *jvmti_env, JNIEnv *jni_env, int scan_interval_ms,
                         int bytecode_capability_available, int redefine_capability_available);
void dtt_watchdog_request_stop(void);

#endif /* DTT_WATCHDOG_H */