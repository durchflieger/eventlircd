
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/un.h>       /* XSI */
#include <syslog.h>       /* XSI */

#include "monitor.h"
#include "lge.h"

#ifdef UNUSED
# error cannot define UNUSED because it is already defined
#endif
#if defined(__GNUC__)
# define UNUSED(x) x __attribute__((unused))
#else
# define UNUSED(x) x
#endif

#define LGE_RX_START_TIMEOUT	6000000
#define LGE_RX_TIMEOUT		1000000
#define LGE_QUEUE_SIZE		128

static int devfd = -1;

static int lge_rx_state;
static int lge_cmd_ok;
static struct timeval lge_timeout;
static unsigned int lge_cmd, lge_pause, lge_rx_value, lge_tx_value;
static unsigned int lge_queue_in, lge_queue_out;
static unsigned int lge_queue[LGE_QUEUE_SIZE];

typedef struct {
	char cmd1;
	char cmd2;
	unsigned int pause;
} lge_cmd_t;

#define LGE_NUM_CMDS	30
static lge_cmd_t lge_cmd_tab[LGE_NUM_CMDS] = {
    { 0,0,0 },
    { 'K', 'A', 7500000 },
    { 'K', 'C', 0 },
    { 'K', 'D', 0 },
    { 'K', 'E', 0 },
    { 'K', 'F', 0 },
    { 'K', 'G', 0 },
    { 'K', 'H', 0 },
    { 'K', 'I', 0 },
    { 'K', 'J', 0 },
    { 'K', 'K', 0 },
    { 'K', 'L', 0 },
    { 'K', 'M', 0 },
    { 'K', 'N', 0 },
    { 'K', 'Q', 0 },
    { 'K', 'T', 0 },
    { 'K', 'U', 0 },
    { 'K', 'V', 0 },
    { 'K', 'W', 0 },
    { 'K', '$', 0 },
    { 'K', 'Z', 0 },
    { 0,0,0 },
    { 0,0,0 },
    { 0,0,0 },
    { 0,0,0 },
    { 0,0,0 },
    { 0,0,0 },
    { 'M', 'C', 0 },
    { 'X', 'B', 0 },
    { 'X', 'Y', 0 }
};

int lge_exit(void) {
	if (devfd != -1) {
		if (monitor_client_remove(devfd) != 0)
			return -1;

		if (close(devfd) == -1) {
			syslog(LOG_ERR, "closing serial port failed: %s\n", strerror(errno));
			return -1;
		}
		devfd = -1;
	}
	return 0;
}

static void set_lge_timeout(struct timeval *now, unsigned int pause) {
	struct timeval p;
	p.tv_sec = pause / 1000000;
	p.tv_usec = pause % 1000000;
	timeradd(now, &p, &lge_timeout);
	monitor_timeout(devfd, &p);
    	syslog(LOG_DEBUG, "set lge timeout: %d\n", pause);
}

static int send_lge_telegram(struct timeval *now)
{
	char msg[12];
	snprintf(msg, sizeof(msg), "%c%c 00 %02X\r", lge_cmd_tab[lge_cmd].cmd1, lge_cmd_tab[lge_cmd].cmd2, (lge_pause > 0) ? 0x0FF : lge_tx_value);
	if (write(devfd, msg, 9) == (ssize_t)-1) {
    		syslog(LOG_ERR, "writing data to serial port failed: %s\n", strerror(errno));
    		return -1;
	}
    	syslog(LOG_DEBUG, "send lge data: '%s'\n", msg);

		// Prepare receiver for reply telegram
	lge_rx_state = 1;
	lge_cmd_ok = 0;
	set_lge_timeout(now, LGE_RX_START_TIMEOUT);

	return 0;
}

static int send_lge_cmd(struct timeval *now)
{
	unsigned int code;
	struct timeval t;

	if (lge_rx_state != 0 || lge_queue_in == lge_queue_out)
		return 0;

	code = lge_queue[lge_queue_out];
	lge_queue_out = (lge_queue_out + 1) % LGE_QUEUE_SIZE;

	if (code == 0) {
		monitor_sigterm_handler(0);
		return 0;
	}

	lge_cmd = code >> 8;
	lge_tx_value = code & 0x0FF;
	lge_pause = lge_cmd_tab[lge_cmd].pause;

	if (now == NULL) {
		now = &t;
		if (monitor_now(now) < 0)
			return -1;
	}

	return send_lge_telegram(now);
}

static void process_lge_reply(char c)
{
	int state = lge_rx_state;

	if (state == 1) {
		if (c == lge_cmd_tab[lge_cmd].cmd2)
			state = 2;	// Start of telegram
	} else if (state == 6) {
		if (c == 'N' || c == 'O')
			state = 7;
		else
			state = 1;
	} else if (state == 7) {
		if (c == 'G') {
			lge_cmd_ok = 0;
			state = 8;
		} else if (c == 'K') {
			lge_cmd_ok = 1;
			state = 8;
		} else
			state = 1;
	} else if (state == 8) {
		if (c >= '0' && c <= '9') {
			lge_rx_value = (c - '0') << 4;
			state = 9;
		} else if (c >= 'A' && c <= 'F') {
			lge_rx_value = (c + 10 - 'A') << 4;
			state = 9;
		} else
			state = 1;
	} else if (state == 9) {
		if (c >= '0' && c <= '9') {
			lge_rx_value |= (c - '0');
			state = 10;
		} else if (c >= 'A' && c <= 'F') {
			lge_rx_value |= (c + 10 - 'A');
			state = 10;
		} else
			state = 1;
	} else if (state == 12) {
		state = 0;	// end of telegram
	} else if (state) {
		++state;
	}

	lge_rx_state = state;
}

static int lge_handler(void* UNUSED(id), int ready, struct timeval *now) {
	char msg[100];
	ssize_t n, i;
	struct timeval pause;

	if (ready) {
		n = read(devfd, &msg, sizeof(msg)-1);
		if (n == (ssize_t)-1) {
    			syslog(LOG_ERR, "reading data from serial port failed: %s\n", strerror(errno));
    			return -1;
		}
		msg[n] = 0;
    		syslog(LOG_DEBUG, "read lge data: '%s'\n", msg);

		for (i = 0; i < n; ++i)
			process_lge_reply(msg[i]);

		if (lge_rx_state > 0) {
			set_lge_timeout(now, LGE_RX_TIMEOUT);
			return 0;
		}

		if (!lge_cmd_ok) {
    			syslog(LOG_ERR, "lge command failed: %02X\n", lge_cmd);
			lge_queue_in = lge_queue_out = 0;
			return 0;
		}

    		syslog(LOG_DEBUG, "lge rx value: %02X\n", lge_rx_value);

		if (lge_pause > 0) {
			if (lge_tx_value != lge_rx_value) {
				set_lge_timeout(now, lge_pause);
				return 0;
			}
			lge_pause = 0;
		}

		return send_lge_cmd(now);
	}

	if (lge_rx_state > 0 || lge_pause > 0) {
		if (!timercmp(now, &lge_timeout, <)) {
			if (lge_rx_state > 0) {
    				syslog(LOG_ERR, "lge command timeout: %02X\n", lge_cmd);
				lge_rx_state = 0;
				lge_pause = 0;
				lge_queue_in = lge_queue_out = 0;
				return 0;
			}
			lge_pause = 0;
			return send_lge_telegram(now);
		}
		timersub(&lge_timeout, now, &pause);
		monitor_timeout(devfd, &pause);
	}

	return 0;
}

int lge_push(unsigned int code) {
	unsigned int i;

	i = (lge_queue_in + 1) % LGE_QUEUE_SIZE;
	if (i == lge_queue_out) {
    		syslog(LOG_ERR, "lge command queue overflow\n");
    		return -1;
	}

	if (code == 0 && lge_rx_state == 0) {
    		syslog(LOG_ERR, "illegal empty lge off command sequence\n");
    		return -1;
	}

	lge_queue[lge_queue_in] = code;
	lge_queue_in = i;

	return 0;
}

int lge_send(const char *seq, struct timeval *now) {
	char *s, *p;
	char buf[100];
	unsigned int code, i;

	if (seq != NULL && devfd != -1) {
		s = strtok_r(strncpy(buf, seq, sizeof(buf)), " ,", &p);
		while (s != NULL) {
			if (sscanf(s, "%x", &code) != 1) {
    				syslog(LOG_ERR, "illegal lge code: %s\n", s);
    				return -1;
			}

			i = code >> 8;
			if (i >= LGE_NUM_CMDS || lge_cmd_tab[i].cmd1 == 0) {
    				syslog(LOG_ERR, "illegal lge command: %s\n", s);
    				return -1;
			}

			if (lge_push(code) < 0)
				return -1;

			s = strtok_r(NULL, " ,", &p);
		}
	}
	return send_lge_cmd(now);
}

int lge_init(const char *devname) {
	struct termios tio;
	int flags;

	lge_rx_state = 0;
	lge_pause = 0;
	lge_queue_in = lge_queue_out = 0;
	timerclear(&lge_timeout);

	devfd = open(devname, O_RDWR|O_NOCTTY);
	if (devfd == -1) {
		syslog(LOG_ERR, "could not open serial port device %s: %s\n", devname, strerror(errno));
		return -1;
	}

	if (tcgetattr(devfd, &tio) == -1) {
		syslog(LOG_ERR, "getting configuration of serial port failed: %s\n", strerror(errno));
		lge_exit();
		return -1;
	}
	cfmakeraw(&tio);
	tio.c_cflag |= (CS8 | CLOCAL | CREAD);
	cfsetospeed(&tio, B9600);
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(devfd, TCSANOW, &tio) == -1) {
		syslog(LOG_ERR, "setting configuration of serial port failed: %s\n", strerror(errno));
		lge_exit();
		return -1;
	}

	tcflush(devfd, TCIOFLUSH);

	flags = fcntl(devfd, F_GETFL);
	fcntl(devfd, F_SETFL, flags | O_NONBLOCK);

	if (monitor_client_add(devfd, &lge_handler, NULL) != 0)
		return -1;

	return 0;
}

