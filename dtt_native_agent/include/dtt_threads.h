#ifndef DTT_THREADS_H
#define DTT_THREADS_H

#include <jvmti.h>
#include <jni.h>

typedef enum {
    DTT_ACTION_NONE = 0,    /* not an offense / nothing to do */
    DTT_ACTION_ABSORB = 1,  /* neutralize by substituting known-good */
    DTT_ACTION_REFLECT = 2  /* repeat offender: bounce the attempt back */
} dtt_thread_action_t;

void dtt_threads_module_init(void);
void dtt_threads_module_shutdown(void);
void dtt_threads_configure(int reflect_after_hits);

dtt_thread_action_t dtt_threads_offend(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                                       jthread thread, const char *class_name);

int dtt_threads_absorbed_thread_count(void);
int dtt_threads_reflected_event_count(void);

void dtt_threads_format_name(jvmtiEnv *jvmti_env, jthread thread,
                             char *out_buf, int out_buf_len);

#endif /* DTT_THREADS_H */