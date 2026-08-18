#include "../include/dtt_log.h"
#include "../include/dtt_platform.h"
#include <stdio.h>
#include <stdarg.h>

static dtt_log_level_t g_min_level = DTT_LOG_INFO;
static dtt_mutex_t g_log_mutex;
static int g_log_mutex_ready = 0;

static void dtt_log_ensure_mutex(void) {
    if (!g_log_mutex_ready) {
        DTT_MUTEX_INIT(&g_log_mutex);
        g_log_mutex_ready = 1;
    }
}

void dtt_log_set_level(dtt_log_level_t level) {
    g_min_level = level;
}

static const char *dtt_log_level_tag(dtt_log_level_t level) {
    switch (level) {
        case DTT_LOG_ERROR: return "ERROR";
        case DTT_LOG_WARN:  return "WARN ";
        case DTT_LOG_INFO:  return "INFO ";
        case DTT_LOG_DEBUG: return "DEBUG";
        default:            return "?????";
    }
}

void dtt_log(dtt_log_level_t level, const char *fmt, ...) {
    va_list args;

    if (level > g_min_level) {
        return; /* below configured verbosity, skip cheaply */
    }

    dtt_log_ensure_mutex();
    DTT_MUTEX_LOCK(&g_log_mutex);

    fprintf(stderr, "[DTT-Native][%s] ", dtt_log_level_tag(level));
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);

    DTT_MUTEX_UNLOCK(&g_log_mutex);
}
