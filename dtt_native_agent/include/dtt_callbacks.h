#ifndef DTT_CALLBACKS_H
#define DTT_CALLBACKS_H

#include <jvmti.h>
#include <jni.h>

void dtt_callbacks_module_init(void);
void dtt_callbacks_module_shutdown(void);
void dtt_callbacks_build(jvmtiEventCallbacks *callbacks, int class_hook_available);
void dtt_callbacks_mark_vm_init_done(void);
void dtt_callbacks_note_class_prepared(const char *class_name);

#endif /* DTT_CALLBACKS_H */