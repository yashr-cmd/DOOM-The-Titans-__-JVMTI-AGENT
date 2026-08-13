#ifndef DTT_CALLBACKS_H
#define DTT_CALLBACKS_H

#include <jvmti.h>
#include <jni.h>

/* ============================================================================
 * dtt_callbacks.h
 *
 * The actual JVMTI event handlers. Only the events this agent truly
 * needs are implemented and registered (see dtt_agent.c):
 *
 *   VMInit                - fires once the JVM is fully initialized;
 *                            used only to log that the agent came up
 *                            cleanly. Requires no special capability.
 *
 *   VMDeath                - fires during orderly JVM shutdown; used
 *                            only to log a final summary. Requires no
 *                            special capability.
 *
 *   ClassPrepare            - fires once per class after it is fully
 *                            prepared. Used only for protected DTT
 *                            classes, to detect "shadow class" loading
 *                            (the same class name loaded by an
 *                            unexpected second ClassLoader instance).
 *                            Requires no special capability.
 *
 *   ClassFileLoadHook       - fires before a class's bytecode is
 *                            actually defined/redefined in the JVM.
 *                            This is the only event used for both
 *                            detection AND mitigation: on an
 *                            unauthorized redefinition of a protected
 *                            class we can substitute our own cached,
 *                            known-good bytecode via the hook's
 *                            new_class_data output parameter, which
 *                            safely neutralizes the tampering without
 *                            throwing or crashing anything. Requires
 *                            the can_generate_all_class_hook_events
 *                            capability; if that capability is
 *                            unavailable on a given JVM, this callback
 *                            is simply never registered and the agent
 *                            continues running in detection-only mode
 *                            for the events that remain available.
 *
 * Module init/shutdown here only sets up the small internal tracking
 * tables (shadow-classloader detection, consecutive-offense counters)
 * that live alongside the callbacks - it does not touch JVMTI itself,
 * that is all orchestrated from dtt_agent.c.
 * ============================================================================ */

void dtt_callbacks_module_init(void);
void dtt_callbacks_module_shutdown(void);

/* Fills in only the callback function pointers this agent uses.
 * class_hook_available controls whether ClassFileLoadHook is wired up
 * at all - callers must only enable JVMTI_EVENT_CLASS_FILE_LOAD_HOOK
 * notifications when this was true, since the capability negotiation
 * happens in dtt_agent.c before this is called. */
void dtt_callbacks_build(jvmtiEventCallbacks *callbacks, int class_hook_available);

#endif /* DTT_CALLBACKS_H */
