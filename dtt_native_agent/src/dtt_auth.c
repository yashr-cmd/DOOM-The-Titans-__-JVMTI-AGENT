#include "../include/dtt_auth.h"
#include "../include/dtt_platform.h"

static DTT_THREAD_LOCAL int g_auth_depth = 0;

void dtt_auth_begin(void) {
    g_auth_depth++;
}

void dtt_auth_end(void) {
    if (g_auth_depth > 0) {
        g_auth_depth--;
    }
}

int dtt_auth_is_active(void) {
    return g_auth_depth > 0;
}