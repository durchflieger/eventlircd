#ifndef _EVENTLIRCD_LGE_H_
#define _EVENTLIRCD_LGE_H_ 1

#include <sys/time.h>

int lge_exit(void);
int lge_init(const char *devname);
int lge_push(unsigned int code);
int lge_send(const char *seq, struct timeval *now);

#endif
