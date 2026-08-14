/* ============================================================================
 * dtt_auth.c - see dtt_auth.h for design notes.
 * ============================================================================ */

#include "../include/dtt_auth.h"
#include "../include/dtt_platform.h"

/* Thread-local nesting depth. 0 = not authorized, >0 = authorized
 * (possibly nested). Each thread gets its own copy automatically via
 * DTT_THREAD_LOCAL, so there is no cross-thread race to guard with a
 * mutex here. */
static DTT_THREAD_LOCAL int g_auth_depth = 0;

void dtt_auth_begin(void) {
    g_auth_depth++;
}

void dtt_auth_end(void) {
    if (g_auth_depth > 0) {
        g_auth_depth--;
    }
    /* If a caller mismatches end() without a matching begin(), we
     * simply clamp at 0 rather than going negative - fail-safe: a
     * buggy caller can only ever end up "not authorized" (the safer
     * default), never stuck "always authorized". */
}

int dtt_auth_is_active(void) {
    return g_auth_depth > 0;
}
