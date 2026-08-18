#include "../include/dtt_protected.h"
#include <string.h>
#include <stddef.h>

static const char *const DTT_PROTECTED_PREFIXES[] = {
    "net/mcreator/transfinityimproved/",  /* Transfinity Improved (DOOM The Titans) */
    "net/mcreator/chaosmobs/",            /* Chaos Mobs                             */
    "runtime/",                           /* companion runtime-agent package        */
    NULL
};

int dtt_is_protected_class(const char *jvm_class_name) {
    int i;

    if (jvm_class_name == NULL) {
        return 0;
    }

    for (i = 0; DTT_PROTECTED_PREFIXES[i] != NULL; i++) {
        size_t prefix_len = strlen(DTT_PROTECTED_PREFIXES[i]);
        if (strncmp(jvm_class_name, DTT_PROTECTED_PREFIXES[i], prefix_len) == 0) {
            return 1;
        }
    }

    return 0;
}

void dtt_strip_class_signature(const char *signature, char *out_buf, int out_buf_len) {
    size_t len;
    size_t copy_len;

    if (out_buf == NULL || out_buf_len <= 0) {
        return;
    }
    out_buf[0] = '\0';

    if (signature == NULL) {
        return;
    }

    len = strlen(signature);

    if (len >= 2 && signature[0] == 'L' && signature[len - 1] == ';') {
        copy_len = len - 2;
        if (copy_len >= (size_t)out_buf_len) {
            copy_len = (size_t)out_buf_len - 1;
        }
        memcpy(out_buf, signature + 1, copy_len);
        out_buf[copy_len] = '\0';
        return;
    }

    copy_len = len;
    if (copy_len >= (size_t)out_buf_len) {
        copy_len = (size_t)out_buf_len - 1;
    }
    memcpy(out_buf, signature, copy_len);
    out_buf[copy_len] = '\0';
}