#ifndef DTT_LOG_H
#define DTT_LOG_H

/* ============================================================================
 * dtt_log.h - tiny leveled logger. Writes to stderr with a "[DTT-Native]"
 * prefix so log lines are easy to grep out of the Minecraft/Forge console
 * alongside the Java-side DTT runtime agent's own log lines.
 *
 * Deliberately minimal: no file I/O, no rotation, no dependencies beyond
 * the C standard library, so it can never itself become a stability or
 * startup-failure risk.
 * ============================================================================ */

typedef enum {
    DTT_LOG_ERROR = 0,
    DTT_LOG_WARN  = 1,
    DTT_LOG_INFO  = 2,
    DTT_LOG_DEBUG = 3
} dtt_log_level_t;

/* Sets the minimum level that will actually be printed. Default is
 * DTT_LOG_INFO. Controlled via the "loglevel=" agent option. */
void dtt_log_set_level(dtt_log_level_t level);

/* printf-style logging, e.g. dtt_log(DTT_LOG_WARN, "capability %s unavailable", name); */
void dtt_log(dtt_log_level_t level, const char *fmt, ...);

#endif /* DTT_LOG_H */
