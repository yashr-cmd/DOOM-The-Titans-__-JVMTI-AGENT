#ifndef DTT_AUTH_H
#define DTT_AUTH_H

/* ============================================================================
 * dtt_auth.h
 *
 * Implements the "authorized transform window" that lets the Java-side
 * DTT runtime agent perform its own legitimate class redefinitions
 * (e.g. re-patching the Praetor armor lock system after a hot reload)
 * without the native layer mistaking its own mod for an attacker.
 *
 * The Java bridge calls beginAuthorizedTransform()/endAuthorizedTransform()
 * (see dtt_bridge.c) immediately before/after it triggers a redefinition
 * via JVMTI RedefineClasses/RetransformClasses. Because ClassFileLoadHook
 * always fires synchronously on the thread that requested the
 * redefinition, a per-thread flag is enough to distinguish "this is us"
 * from "this is someone else touching our classes".
 *
 * The counter supports nesting (a transform that itself triggers a
 * nested transform) so begin/end pairs can never under/over-flow into
 * a stuck state from a single mismatched call.
 * ============================================================================ */

void dtt_auth_begin(void);
void dtt_auth_end(void);

/* Returns non-zero if the calling thread is currently inside an
 * authorized-transform window. */
int dtt_auth_is_active(void);

#endif /* DTT_AUTH_H */
