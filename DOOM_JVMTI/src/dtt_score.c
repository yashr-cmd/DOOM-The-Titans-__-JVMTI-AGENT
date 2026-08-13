/* ============================================================================
 * dtt_score.c - see dtt_score.h for design notes.
 * ============================================================================ */

#include "../include/dtt_score.h"
#include "../include/dtt_config.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    char line[DTT_REASON_LINE_MAX];
} dtt_reason_entry_t;

static int g_score = 0;
static int g_threshold_suspicious = DTT_DEFAULT_THRESHOLD_SUSPICIOUS;
static int g_threshold_high_risk  = DTT_DEFAULT_THRESHOLD_HIGH_RISK;
static int g_threshold_critical   = DTT_DEFAULT_THRESHOLD_CRITICAL;

static dtt_reason_entry_t g_reasons[DTT_REASON_LOG_CAPACITY];
static int g_reason_head  = 0; /* index the NEXT entry will be written to */
static int g_reason_count = 0; /* number of valid entries, up to capacity */

static dtt_mutex_t g_score_mutex;
static int g_score_ready = 0;

void dtt_score_init(int suspicious, int high_risk, int critical) {
    memset(g_reasons, 0, sizeof(g_reasons));
    g_reason_head = 0;
    g_reason_count = 0;
    g_score = 0;

    if (suspicious > 0) g_threshold_suspicious = suspicious;
    if (high_risk  > 0) g_threshold_high_risk  = high_risk;
    if (critical   > 0) g_threshold_critical   = critical;

    DTT_MUTEX_INIT(&g_score_mutex);
    g_score_ready = 1;
}

void dtt_score_shutdown(void) {
    if (!g_score_ready) {
        return;
    }
    DTT_MUTEX_DESTROY(&g_score_mutex);
    g_score_ready = 0;
}

void dtt_score_configure_thresholds(int suspicious, int high_risk, int critical) {
    if (!g_score_ready) {
        return;
    }

    DTT_MUTEX_LOCK(&g_score_mutex);

    if (suspicious > 0) g_threshold_suspicious = suspicious;
    if (high_risk  > 0) g_threshold_high_risk  = high_risk;
    if (critical   > 0) g_threshold_critical   = critical;

    /* Keep tiers in ascending order no matter what combination the
     * caller passed - a misconfigured threshold should never make the
     * status reporting nonsensical (e.g. "critical" below "suspicious"). */
    if (g_threshold_high_risk <= g_threshold_suspicious) {
        g_threshold_high_risk = g_threshold_suspicious + 1;
    }
    if (g_threshold_critical <= g_threshold_high_risk) {
        g_threshold_critical = g_threshold_high_risk + 1;
    }

    DTT_MUTEX_UNLOCK(&g_score_mutex);

    dtt_log(DTT_LOG_INFO, "thresholds reconfigured: suspicious=%d high_risk=%d critical=%d",
            g_threshold_suspicious, g_threshold_high_risk, g_threshold_critical);
}

void dtt_score_add(int points, const char *code, const char *fmt, ...) {
    char message[DTT_REASON_LINE_MAX];
    char line[DTT_REASON_LINE_MAX];
    va_list args;
    jlong ts;

    if (!g_score_ready) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    ts = dtt_now_millis();
    snprintf(line, sizeof(line), "[%lld] %s (+%d): %s",
             (long long)ts, code != NULL ? code : "DTT-000", points, message);

    DTT_MUTEX_LOCK(&g_score_mutex);

    g_score += points;
    if (g_score < DTT_SCORE_MIN) g_score = DTT_SCORE_MIN;
    if (g_score > DTT_SCORE_MAX) g_score = DTT_SCORE_MAX;

    strncpy(g_reasons[g_reason_head].line, line, DTT_REASON_LINE_MAX - 1);
    g_reasons[g_reason_head].line[DTT_REASON_LINE_MAX - 1] = '\0';
    g_reason_head = (g_reason_head + 1) % DTT_REASON_LOG_CAPACITY;
    if (g_reason_count < DTT_REASON_LOG_CAPACITY) {
        g_reason_count++;
    }

    DTT_MUTEX_UNLOCK(&g_score_mutex);

    if (points > 0) {
        dtt_log(DTT_LOG_WARN, "%s", line);
    } else {
        dtt_log(DTT_LOG_INFO, "%s", line);
    }
}

int dtt_score_get(void) {
    int value;
    if (!g_score_ready) {
        return 0;
    }
    DTT_MUTEX_LOCK(&g_score_mutex);
    value = g_score;
    DTT_MUTEX_UNLOCK(&g_score_mutex);
    return value;
}

dtt_status_t dtt_score_get_status(void) {
    int value = dtt_score_get();
    if (value >= g_threshold_critical)  return DTT_STATUS_CRITICAL;
    if (value >= g_threshold_high_risk) return DTT_STATUS_HIGH_RISK;
    if (value >= g_threshold_suspicious) return DTT_STATUS_SUSPICIOUS;
    return DTT_STATUS_NORMAL;
}

const char *dtt_status_name(dtt_status_t status) {
    switch (status) {
        case DTT_STATUS_NORMAL:    return "NORMAL";
        case DTT_STATUS_SUSPICIOUS: return "SUSPICIOUS";
        case DTT_STATUS_HIGH_RISK: return "HIGH_RISK";
        case DTT_STATUS_CRITICAL:  return "CRITICAL";
        default:                   return "UNKNOWN";
    }
}

int dtt_score_snapshot_reasons(char out[][256], int max_entries) {
    int i;
    int copied = 0;
    int idx;

    if (!g_score_ready || out == NULL || max_entries <= 0) {
        return 0;
    }

    DTT_MUTEX_LOCK(&g_score_mutex);

    /* Walk backwards from the most recently written entry so the
     * caller gets newest-first ordering. */
    for (i = 0; i < g_reason_count && copied < max_entries; i++) {
        idx = (g_reason_head - 1 - i + DTT_REASON_LOG_CAPACITY) % DTT_REASON_LOG_CAPACITY;
        strncpy(out[copied], g_reasons[idx].line, DTT_REASON_LINE_MAX - 1);
        out[copied][DTT_REASON_LINE_MAX - 1] = '\0';
        copied++;
    }

    DTT_MUTEX_UNLOCK(&g_score_mutex);

    return copied;
}
