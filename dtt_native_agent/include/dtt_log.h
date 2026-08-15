#ifndef DTT_LOG_H
#define DTT_LOG_H

typedef enum {
    DTT_LOG_ERROR = 0,
    DTT_LOG_WARN  = 1,
    DTT_LOG_INFO  = 2,
    DTT_LOG_DEBUG = 3
} dtt_log_level_t;

void dtt_log_set_level(dtt_log_level_t level);
void dtt_log(dtt_log_level_t level, const char *fmt, ...);

#endif /* DTT_LOG_H */