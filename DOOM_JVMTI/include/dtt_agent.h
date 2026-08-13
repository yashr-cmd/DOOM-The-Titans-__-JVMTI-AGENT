#ifndef DTT_AGENT_H
#define DTT_AGENT_H

/* ============================================================================
 * dtt_agent.h
 *
 * Small accessors shared between dtt_agent.c (which owns the JVMTI
 * lifecycle: Agent_OnLoad / Agent_OnAttach / Agent_OnUnload) and
 * dtt_bridge.c (which needs to know whether the agent is actually up
 * before answering Java-side queries).
 *
 * Agent_OnLoad/Agent_OnAttach/Agent_OnUnload themselves are declared by
 * the JVM spec / <jvmti.h>, not here - they are just implemented in
 * dtt_agent.c with the exact signatures the JVM expects to find by
 * name in the shared library.
 * ============================================================================ */

/* Returns 1 once agent initialization has completed successfully
 * (capabilities negotiated, callbacks registered, internal state set
 * up). Returns 0 before that point, if initialization failed, or
 * after Agent_OnUnload has run. The JNI bridge uses this to decide
 * whether to answer real data or fail-safe defaults. */
int dtt_agent_is_active(void);

/* Returns a short static string like "Windows/x86_64" describing the
 * platform this build was compiled for - useful for the Java side to
 * log/confirm which native binary actually got loaded. */
const char *dtt_agent_platform_string(void);

#endif /* DTT_AGENT_H */
