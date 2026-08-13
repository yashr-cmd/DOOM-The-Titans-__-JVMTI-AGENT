#ifndef DTT_SCORE_H
#define DTT_SCORE_H

/* ============================================================================
 * dtt_score.h
 *
 * The running integrity/risk score for suspicious instrumentation
 * behavior, plus a small ring buffer of human-readable "reason codes"
 * explaining *why* the score moved. This is the data the Java-side DTT
 * runtime agent reads back through dtt_bridge.c.
 *
 * Score never reflects "another agent/transformer exists" by itself -
 * only concrete behaviors observed against protected DTT classes (see
 * dtt_callbacks.c for what actually calls dtt_score_add()).
 * ============================================================================ */

typedef enum {
    DTT_STATUS_NORMAL = 0,
    DTT_STATUS_SUSPICIOUS,
    DTT_STATUS_HIGH_RISK,
    DTT_STATUS_CRITICAL
} dtt_status_t;

/* One-time setup/teardown. Safe to call dtt_score_init() with any of
 * the thresholds as -1 to keep the compiled-in default from
 * dtt_config.h for that tier. */
void dtt_score_init(int suspicious, int high_risk, int critical);
void dtt_score_shutdown(void);

/* Runtime reconfiguration of thresholds, exposed to Java via
 * dtt_bridge.c's configureThresholds(). Values <= 0 are ignored
 * (keeps the previous value) so a partial/accidental call can't zero
 * out a threshold. Thresholds are also clamped into ascending order
 * (suspicious < high_risk < critical) to keep status reporting sane
 * even if misconfigured. */
void dtt_score_configure_thresholds(int suspicious, int high_risk, int critical);

/* Adds `points` to the running score (clamped to
 * [DTT_SCORE_MIN, DTT_SCORE_MAX]) and appends a formatted reason-log
 * entry. `points` may be 0 for purely informational entries (e.g. "we
 * blocked an attempt" is worth logging even though the risk itself was
 * already scored elsewhere). `code` should be a short stable
 * identifier like "DTT-101" so the Java side / a human reading logs
 * can pattern-match on it; `fmt` + varargs is a printf-style human
 * readable message. */
void dtt_score_add(int points, const char *code, const char *fmt, ...);

int dtt_score_get(void);
dtt_status_t dtt_score_get_status(void);
const char *dtt_status_name(dtt_status_t status);

/* Copies up to max_entries most-recent reason-log lines (newest
 * first), each already formatted as:
 *   "[epoch_ms] CODE (+points): message"
 * into caller-supplied fixed-width buffers (each DTT_REASON_LINE_MAX
 * bytes, see dtt_config.h). Returns the number of entries actually
 * copied (<= max_entries and <= however many exist so far). */
int dtt_score_snapshot_reasons(char out[][256], int max_entries);

#endif /* DTT_SCORE_H */
