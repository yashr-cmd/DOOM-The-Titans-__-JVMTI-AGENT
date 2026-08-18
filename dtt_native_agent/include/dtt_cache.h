#ifndef DTT_CACHE_H
#define DTT_CACHE_H

#include <jvmti.h>

void dtt_cache_init(void);
void dtt_cache_shutdown(void);
void dtt_cache_store(const char *class_name, const unsigned char *data, jint len);

int dtt_cache_lookup(const char *class_name, const unsigned char **out_data, jint *out_len);

#endif /* DTT_CACHE_H */