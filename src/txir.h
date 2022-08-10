#ifndef _TXIR_H_
#define _TXIR_H_ 1

#include <sys/time.h>

void txir_init(const char *path);
int txir_exit(void);
int txir_send(const char *cmd);

#endif
