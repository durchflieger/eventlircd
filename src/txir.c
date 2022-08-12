//
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>       /* XSI */
#include <syslog.h>       /* XSI */

#include "monitor.h"
#include "txir.h"

#ifdef UNUSED
# error cannot define UNUSED because it is already defined
#endif
#if defined(__GNUC__)
# define UNUSED(x) x __attribute__((unused))
#else
# define UNUSED(x) x
#endif

static const char *txir_socket_path;
static int txir_fd = -1;

void txir_init(const char *path)
{
	txir_socket_path = path;
}

int txir_exit(void)
{
	if (txir_fd != -1) {
		if (monitor_client_remove(txir_fd) != 0)
			return -1;

		if (close(txir_fd) == -1) {
			syslog(LOG_ERR, "closing txir socket failed: %s\n", strerror(errno));
			return -1;
		}
		txir_fd = -1;
	}
	return 0;
}

static int txir_handler(void* UNUSED(id), int ready, struct timeval* UNUSED(now))
{
	if (ready)
	{
		char msg[256];
		ssize_t n = read(txir_fd, &msg, sizeof(msg) - 1);
		if (n == (ssize_t)-1) {
    	syslog(LOG_ERR, "reading data from txir socket failed: %s\n", strerror(errno));
    	return -1;
		}
	}
	return 0;
}

int txir_send(const char *cmd)
{
	if (txir_socket_path == NULL)
		return 0;

	if (txir_fd == -1)
	{
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (fd != -1) {
			struct sockaddr_un addr;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			strncpy(addr.sun_path, txir_socket_path, sizeof(addr.sun_path) - 1);
			if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != -1)
				txir_fd = fd;
		}
		if (txir_fd == -1)
		{
			syslog(LOG_ERR, "could not open txir socket %s: %s\n", txir_socket_path, strerror(errno));
			close(fd);
			return -1;
		}

		if (monitor_client_add(txir_fd, &txir_handler, NULL) != 0)
			return -1;
	}

	char buf[256];
	ssize_t n = snprintf(buf, sizeof(buf), "%s\n", cmd);
	if (write(txir_fd, buf, n) != n)
	{
		syslog(LOG_ERR, "write to txir socket failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}
