#ifndef DTT_THREADS_H
#define DTT_THREADS_H

#include <jvmti.h>
#include <jni.h>

/* ============================================================================
 * dtt_threads.h
 *
 * The "absorb / reflect" thread defense. This module replaces the old
 * integrity-score system. There is no score, no status tier, no reason
 * code. Instead, the agent watches protected DTT classes (see
 * dtt_protected.h) and, the moment anything tries to touch them from a
 * thread that is not DTT's own authorized-transform window (see
 * dtt_auth.h), it deals with that THREAD directly - and it never kills,
 * suspends, stops, or interrupts any thread.
 *
 * Two responses, chosen by how often the SAME thread keeps attacking the
 * SAME protected class:
 *
 *   DTT_ACTION_ABSORB   - the offending thread is flagged by identity and
 *       every attempt it makes against a protected class is silently
 *       neutralized: the tampered bytecode is replaced with the cached
 *       known-good version (via ClassFileLoadHook's new_class_data), so
 *       the attack never lands and the hostile thread keeps running
 *       normally, unaware that its payload was absorbed.
 *
 *   DTT_ACTION_REFLECT  - a thread that keeps hitting the SAME protected
 *       class (>= DTT_REFLECT_AFTER_HITS offenses) has its attack bounced
 *       back onto its own thread: the hook returns deliberately invalid
 *       class data, so the hostile redefinition call fails with a
 *       ClassFormatError on the thread that made it - its own attack
 *       reflected back at it. We never call any suspend/stop/interrupt/
 *       terminate API; the offending thread's fate is entirely its own
 *       code's exception handling.
 *
 * Thread identity is tracked with a strong global reference to the
 * java.lang.Thread object, so a hostile thread stays flagged for its
 * whole lifetime. The table is capped (DTT_THREAD_TRACK_CAPACITY); once
 * full, new threads are absorbed per-event without being added, so a
 * hostile actor can never exhaust native heap this way.
 * ============================================================================ */

typedef enum {
    DTT_ACTION_NONE = 0,    /* not an offense / nothing to do */
    DTT_ACTION_ABSORB = 1,  /* neutralize by substituting known-good */
    DTT_ACTION_REFLECT = 2  /* repeat offender: bounce the attempt back */
} dtt_thread_action_t;

/* One-time setup/teardown. dtt_threads_configure() must be called at
 * least once (from dtt_agent.c) before the module is used. */
void dtt_threads_module_init(void);
void dtt_threads_module_shutdown(void);
void dtt_threads_configure(int reflect_after_hits);

/* Records that `thread` just made an unauthorized attempt to touch
 * protected `class_name`. Returns the action the caller should take for
 * THIS attempt:
 *   - first offense            -> DTT_ACTION_ABSORB
 *   - repeat on the same class -> DTT_ACTION_REFLECT
 * The thread is flagged permanently regardless of the returned action.
 *
 * `thread` may be NULL, in which case the current thread is resolved
 * via GetCurrentThread (used from ClassFileLoadHook, which does not
 * receive a jthread). If even that fails the thread is not tracked but
 * ABSORB is still returned so the caller still neutralizes the attempt.
 *
 * NOTE for load-path callers (no redefinition in progress): reflecting
 * is only ever valid on a redefinition attempt. Callers that cannot
 * safely reflect (e.g. a first-time class load) must clamp the result
 * to ABSORB - dtt_callbacks.c does this.
 */
dtt_thread_action_t dtt_threads_offend(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                                       jthread thread, const char *class_name);

/* How many distinct threads have been flagged as hostile so far, and how
 * many total reflect events have been issued. Exposed to Java via the
 * JNI bridge (dtt_bridge.c). */
int dtt_threads_absorbed_thread_count(void);
int dtt_threads_reflected_event_count(void);

/* Reformat a hostile thread's name into a caller buffer for logging
 * (e.g. "Hostile-Thread-42 [daemon]"). Used by dtt_threads.c internally
 * and by dtt_callbacks.c for its log lines. Fail-safe: always writes a
 * NUL-terminated string, even on JVMTI failure. */
void dtt_threads_format_name(jvmtiEnv *jvmti_env, jthread thread,
                             char *out_buf, int out_buf_len);

#endif /* DTT_THREADS_H */
