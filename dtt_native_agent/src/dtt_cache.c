#include "../include/dtt_cache.h"
#include "../include/dtt_config.h"
#include "../include/dtt_platform.h"
#include "../include/dtt_log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[DTT_CACHE_NAME_MAX];
    unsigned char *data;
    jint len;
    int in_use;
} dtt_cache_entry_t;

static dtt_cache_entry_t g_cache[DTT_CACHE_CAPACITY];
static int g_cache_count = 0;
static dtt_mutex_t g_cache_mutex;
static int g_cache_ready = 0;

void dtt_cache_init(void) {
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;
    DTT_MUTEX_INIT(&g_cache_mutex);
    g_cache_ready = 1;
}

void dtt_cache_shutdown(void) {
    int i;

    if (!g_cache_ready) {
        return;
    }

    DTT_MUTEX_LOCK(&g_cache_mutex);
    for (i = 0; i < DTT_CACHE_CAPACITY; i++) {
        if (g_cache[i].in_use && g_cache[i].data != NULL) {
            free(g_cache[i].data);
            g_cache[i].data = NULL;
        }
        g_cache[i].in_use = 0;
    }
    g_cache_count = 0;
    DTT_MUTEX_UNLOCK(&g_cache_mutex);

    DTT_MUTEX_DESTROY(&g_cache_mutex);
    g_cache_ready = 0;
}

static dtt_cache_entry_t *dtt_cache_find_locked(const char *class_name) {
    int i;
    for (i = 0; i < DTT_CACHE_CAPACITY; i++) {
        if (g_cache[i].in_use && strncmp(g_cache[i].name, class_name, DTT_CACHE_NAME_MAX) == 0) {
            return &g_cache[i];
        }
    }
    return NULL;
}

static dtt_cache_entry_t *dtt_cache_find_free_slot_locked(void) {
    int i;
    for (i = 0; i < DTT_CACHE_CAPACITY; i++) {
        if (!g_cache[i].in_use) {
            return &g_cache[i];
        }
    }
    return NULL;
}

void dtt_cache_store(const char *class_name, const unsigned char *data, jint len) {
    dtt_cache_entry_t *entry;
    unsigned char *copy;

    if (!g_cache_ready || class_name == NULL || data == NULL || len <= 0) {
        return;
    }

    if (len > DTT_CACHE_MAX_CLASS_BYTES) {
        dtt_log(DTT_LOG_WARN,
                "refusing to cache '%s': %d bytes exceeds cache limit (%d) - protection for "
                "this class will fall back to detect-and-report only",
                class_name, (int)len, DTT_CACHE_MAX_CLASS_BYTES);
        return;
    }

    copy = (unsigned char *)malloc((size_t)len);
    if (copy == NULL) {
        dtt_log(DTT_LOG_WARN, "cache allocation failed for '%s' (%d bytes) - out of memory?",
                class_name, (int)len);
        return;
    }
    memcpy(copy, data, (size_t)len);

    DTT_MUTEX_LOCK(&g_cache_mutex);

    entry = dtt_cache_find_locked(class_name);
    if (entry == NULL) {
        entry = dtt_cache_find_free_slot_locked();
        if (entry == NULL) {
            DTT_MUTEX_UNLOCK(&g_cache_mutex);
            dtt_log(DTT_LOG_WARN,
                    "bytecode cache full (%d entries) - cannot track '%s'; it will not be "
                    "eligible for automatic tamper-rollback until the cache frees up",
                    DTT_CACHE_CAPACITY, class_name);
            free(copy);
            return;
        }
        strncpy(entry->name, class_name, DTT_CACHE_NAME_MAX - 1);
        entry->name[DTT_CACHE_NAME_MAX - 1] = '\0';
        entry->in_use = 1;
        g_cache_count++;
    } else if (entry->data != NULL) {
        free(entry->data);
    }

    entry->data = copy;
    entry->len = len;

    DTT_MUTEX_UNLOCK(&g_cache_mutex);
}

int dtt_cache_lookup(const char *class_name, const unsigned char **out_data, jint *out_len) {
    dtt_cache_entry_t *entry;
    int found = 0;

    if (!g_cache_ready || class_name == NULL || out_data == NULL || out_len == NULL) {
        return 0;
    }

    DTT_MUTEX_LOCK(&g_cache_mutex);
    entry = dtt_cache_find_locked(class_name);
    if (entry != NULL) {
        *out_data = entry->data;
        *out_len = entry->len;
        found = 1;
    }
    DTT_MUTEX_UNLOCK(&g_cache_mutex);

    return found;
}