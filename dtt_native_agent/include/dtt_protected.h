#ifndef DTT_PROTECTED_H
#define DTT_PROTECTED_H

int dtt_is_protected_class(const char *jvm_class_name);
void dtt_strip_class_signature(const char *signature, char *out_buf, int out_buf_len);

#endif /* DTT_PROTECTED_H */