
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>

#include "monitor.h"
#include "lge.h"

#define LGE_RX_START_TIMEOUT	6000000
#define LGE_RX_TIMEOUT		1000000
#define LGE_QUEUE_SIZE		128

static int devfd = -1;

static int lge_rx_state;
static int lge_cmd_ok;
static timeval lge_timeout;
static unsigned int lge_rx_value, lge_tx_value;
static int lge_queue_in, lge_queue_out;
static unsigned int lge_queue[LGE_QUEUE_SIZE];

typedef struct {
	uint8_t cmd1;
	uint8_t cmd2;
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

int lge_init(const char *devname) {
	struct termios tio;

	lge_queue_in = lge_queue_out = 0;
	timerclear(&lge_timeout);

	devfd = open(devname, O_WRONLY|O_NOCTTY);
	if (devfd == -1) {
		syslog(LOG_ERR, "could not open serial port device %s: %s\n", devname, strerror(errno));
		return -1;
	}

	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = (CS8 | CSTOPB | CLOCAL);
	cfsetospeed(&tio, B9600);
	if (tcsetattr(devfd, TCSANOW, &tio) == -1) {
		syslog(LOG_ERR, "setting configuration of serial port failed: %s\n", strerror(errno));
		lge_exit();
		return -1;
	}

	tcflush(devfd, TCIOFLUSH);

	if (monitor_client_add(devfd, &lge_handler, NULL) != 0)
		return -1;

	return 0;
}

static void set_lge_timeout(struct timeval *now, unsigned int pause) {
	struct timeval p;
	p.tv_sec = pause / 1000000;
	p.tv_usec = pause % 1000000;
	timeradd(now, &p, &lge_timeout);
	monitor_timeout(devfd, &t);
}

static int send_lge_telegram(struct timeval *now)
{
	uint8_t msg[10];
	snprintf(msg, sizeof(msg), "%c%c 00 %02X\r", lge_cmd_tab[lge_cmd].cmd1, lge_cmd_tab[lge_cmd].cmd2, (lge_pause > 0) ? 0x0FF : lge_tx_value);
	if (write(devfd, msg, 9) == (ssize_t)-1) {
    		syslog(LOG_ERR, "writing data to serial port failed: %s\n", strerror(errno));
    		return -1;
	}

		// Prepare receiver for reply telegram
	lge_rx_state = 1;
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

static void process_lge_reply(uint8_t c)
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
	uint8_t buf[100];
	ssize_t n, i;

	if (ready) {
		n = read(devfd, &buf, sizeof(buf));
		if (n == (ssize_t)-1) {
    			syslog(LOG_ERR, "reading data from serial port failed: %s\n", strerror(errno));
    			return -1;
		}

		for (i = 0; i < n; ++i)
			process_lge_reply(buf[i]);

		if (lge_rx_state > 0) {
			set_lge_timeout(now, LGE_RX_TIMEOUT);
			return 0;
		}

		if (!lge_cmd_ok) {
    			syslog(LOG_ERR, "lge command failed: %x\n", lge_cmd);
			lge_queue_in = lge_queue_out = 0;
			return 0;
		}

		if (lge_pause > 0) {
			if (lge_tx_value != lge_rx_value)
				return set_lge_timeout(now, lge_pause);
			lge_pause = 0;
		}

		return send_lge_cmd(now);
	}

	if ((lge_rx_state > 0 || lge_pause > 0) && timercmp(now, &lge_timeout, >)) {
		if (lge_rx_state > 0) {
    			syslog(LOG_ERR, "lge command timeout: %x\n", lge_cmd);
			lge_queue_in = lge_queue_out = 0;
			return 0;
		}
		lge_pause = 0;
		return send_lge_telegram(now);
	}

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
    				syslog(LOG_ERR, "illegal lge code: %s\n", seq);
    				return -1;
			}

			i = code >> 8;
			if (i >= LGE_NUM_CMDS || lge_cmd_tab[i].cmd1 == 0) {
    				syslog(LOG_ERR, "illegal lge command: %s\n", seq);
    				return -1;
			}

			i = (lge_queue_in + 1) % LGE_QUEUE_SIZE;
			if (i != lge_queue_out) {
				lge_queue[lge_queue_in] = code;
				lge_queue_in = i;
			}

			s = strtok_r(NULL, " ,", &p);
		}
	}
	return send_lge_cmd(now);
}
