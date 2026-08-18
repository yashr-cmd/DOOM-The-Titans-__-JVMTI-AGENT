#ifndef DTT_CALLBACKS_H
#define DTT_CALLBACKS_H

#include <jvmti.h>
#include <jni.h>

void dtt_callbacks_module_init(void);
void dtt_callbacks_module_shutdown(void);
void dtt_callbacks_build(jvmtiEventCallbacks *callbacks, int class_hook_available);

#endif /* DTT_CALLBACKS_H */