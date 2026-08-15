#ifndef DTT_AGENT_H
#define DTT_AGENT_H

int dtt_agent_is_active(void);

const char *dtt_agent_platform_string(void);

int dtt_agent_bytecode_capability(void);
int dtt_agent_redefine_capability(void);
int dtt_agent_scan_interval_ms(void);

#endif /* DTT_AGENT_H */