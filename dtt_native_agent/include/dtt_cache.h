#ifndef DTT_CACHE_H
#define DTT_CACHE_H

#include <jvmti.h>

/* ============================================================================
 * dtt_cache.h
 *
 * Keeps a "known good" copy of the bytecode for each protected class,
 * captured the first time it loads (or any time it is legitimately,
 * authorized-ly, re-transformed). If an unauthorized actor later tries
 * to redefine that same class with different bytecode, dtt_callbacks.c
 * can substitute this cached copy back in through the ClassFileLoadHook
 * "new_class_data" output parameter - isolating the tampering attempt
 * without throwing, crashing, or otherwise destabilizing the JVM.
 *
 * This is plain malloc/free-backed storage (not JVMTI Allocate/Deallocate)
 * because it is long-lived agent-owned memory, not a single hook-call
 * return value. When we actually substitute bytecode back into a hook
 * call, dtt_callbacks.c copies out of this cache into a buffer obtained
 * from jvmtiEnv->Allocate, per the JVMTI spec for new_class_data.
 * ============================================================================ */

void dtt_cache_init(void);
void dtt_cache_shutdown(void);

/* Store (or refresh) the known-good bytecode for a protected class.
 * Silently ignores classes above DTT_CACHE_MAX_CLASS_BYTES or once
 * DTT_CACHE_CAPACITY distinct classes are already tracked (logs a
 * warning in that case) - caching is a best-effort defense, never a
 * hard requirement for the JVM to keep running. */
void dtt_cache_store(const char *class_name, const unsigned char *data, jint len);

/* Looks up cached bytecode for class_name. On success returns 1 and
 * sets *out_data and *out_len to point at cache-owned memory (valid
 * until the next dtt_cache_store for the same class, or shutdown -
 * callers must copy the bytes out, e.g. into a jvmtiEnv->Allocate
 * buffer, before returning from the hook). Returns 0 if nothing is
 * cached for that class yet. */
int dtt_cache_lookup(const char *class_name, const unsigned char **out_data, jint *out_len);

#endif /* DTT_CACHE_H */
