#ifndef DTT_WATCHDOG_H
#define DTT_WATCHDOG_H

#include <jvmti.h>
#include <jni.h>

/* ============================================================================
 * dtt_watchdog.h
 *
 * A second, independent line of detection alongside ClassFileLoadHook.
 *
 * WHY THIS EXISTS: ClassFileLoadHook only fires when bytecode is changed
 * through the official JVMTI RedefineClasses/RetransformClasses APIs.
 * Some hostile mods instead use sun.misc.Unsafe to reach into the JVM's
 * internal Method/ConstMethod structures and overwrite a method's
 * bytecode directly - no redefinition call is ever made, so the hook
 * never fires and that tampering is otherwise invisible.
 *
 * This module closes that gap: it runs as a proper JVMTI agent thread
 * (started via RunAgentThread, so it is registered with the JVM like
 * any other thread, safe to call JVMTI functions from) that wakes up
 * periodically and re-reads the CURRENT live bytecode of every method
 * belonging to a protected DTT class, comparing it against a hash
 * captured when the method was last known-good. A mismatch means the
 * method's bytecode changed without going through any hook we already
 * watch - i.e. tampering by direct memory manipulation.
 *
 * On detection, if we have the class's known-good bytes cached (see
 * dtt_cache.h) and the can_redefine_classes capability was granted, we
 * "reflect" the tampering away by pushing our clean bytecode back in
 * via RedefineClasses - the same isolate-don't-crash philosophy as
 * ClassFileLoadHook's substitution, just reached through a different
 * API because we're not inside a hook call this time.
 *
 * HARD GUARANTEE: this module only ever reads bytecode of, and only
 * ever calls RedefineClasses on, classes that dtt_is_protected_class()
 * says belong to Transfinity Improved / Chaos Mobs / the runtime
 * agent. It never enumerates, inspects, or modifies any other class,
 * and it never touches threads at all (no suspend/stop/interrupt of
 * anything, DTT's own or otherwise) - it only ever repairs bytecode.
 * ============================================================================ */

void dtt_watchdog_module_init(void);
void dtt_watchdog_module_shutdown(void);

/* Called from ClassPrepare (see dtt_callbacks.c) for every protected
 * class as it loads/reloads. Snapshots the current bytecode hash of
 * each of the class's methods as the new "known good" baseline the
 * watchdog will compare future readings against. Safe to call
 * repeatedly for the same class (e.g. after a legitimate patch) - old
 * entries for that class are refreshed, not duplicated. */
void dtt_watchdog_track_class(jvmtiEnv *jvmti_env, jclass klass, const char *class_name);

/* Starts the background watchdog as a JVMTI agent thread. Call once,
 * from the VMInit callback (needs a live JNIEnv to construct the
 * jthread RunAgentThread requires). bytecode_capability_available
 * gates whether the watchdog runs at all (it is useless without
 * can_get_bytecodes); redefine_capability_available separately gates
 * whether it can attempt repairs or only detect-and-report. Both are
 * checked so the watchdog degrades gracefully rather than failing
 * outright on a JVM that only grants partial capabilities. */
void dtt_watchdog_start(jvmtiEnv *jvmti_env, JNIEnv *jni_env, int scan_interval_ms,
                         int bytecode_capability_available, int redefine_capability_available);

/* Asks the watchdog loop to exit at its next wake-up. Does not force
 * the thread down mid-iteration - fail-safe, lets whatever it's doing
 * finish cleanly. Call from VMDeath before the rest of the agent tears
 * down. */
void dtt_watchdog_request_stop(void);

#endif /* DTT_WATCHDOG_H */
