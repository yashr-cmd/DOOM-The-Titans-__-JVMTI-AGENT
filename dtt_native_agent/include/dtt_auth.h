#ifndef DTT_AUTH_H
#define DTT_AUTH_H

void dtt_auth_begin(void);
void dtt_auth_end(void);

/* Returns non-zero if the calling thread is currently inside an
 * authorized-transform window. */
int dtt_auth_is_active(void);

#endif /* DTT_AUTH_H */