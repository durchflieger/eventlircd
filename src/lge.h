#ifndef _EVENTLIRCD_LGE_H_
#define _EVENTLIRCD_LGE_H_ 1

int lge_exit(void);
int lge_init(const char *devname);
int lge_send(const char *seq);

#endif
