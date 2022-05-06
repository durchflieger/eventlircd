
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#define INVALID_DEV_HANDLE      -1
#define LGE_RX_START_TIMEOUT	60
#define LGE_RX_TIMEOUT		10

static int devfd = INVALID_DEV_HANDLE;
static struct termios tio;

enum { LGE_OK, LGE_UNKNOWN_CMD_ERR, LGE_NG_ERR, LGE_TIMEOUT_ERR };

static int lge_ret_code;
static int lge_rx_state;
static timeval lge_pause_time;
static uint8_t lge_rx_cmd2;
static uint8_t lge_rx_value;

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
	if (devfd != INVALID_DEV_HANDLE) {
		if (close(devfd) == -1) {
			syslog(LOG_ERR, "closing serial port failed: %s\n", strerror(errno));
			return -1;
		}
		devfd = INVALID_DEV_HANDLE;
	}
	return 0;
}

int lge_init(const char *devname) {
	devfd = open(devname, O_WRONLY|O_NOCTTY);
	if (devfd == INVALID_DEV_HANDLE) {
		syslog(LOG_ERR, "could not open serial port device %s: %s\n", devname, strerror(errno));
		return -1;
	}

	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = (CS8 | CSTOPB | CLOCAL);
	tio.c_cc[VTIME] = LGE_RX_START_TIMEOUT;
	cfsetospeed(&tio, B9600);
	if (tcsetattr(devfd, TCSANOW, &tio) == -1) {
		syslog(LOG_ERR, "setting configuration of serial port failed: %s\n", strerror(errno));
		lge_exit();
		return -1;
	}

	if (tcgetattr(devfd, &tio) == -1) {
		syslog(LOG_ERR, "getting configuration of serial port failed: %s\n", strerror(errno));
		lge_exit();
		return -1;
	}

	tcflush(devfd, TCIOFLUSH);
	timerclear(&lge_pause_time);
}

static int get_now(struct timeval *time) {
	if (gettimeofday(time, NULL) == -1) {
    		syslog(LOG_ERR, "getting time of day failed: %s\n", strerror(errno));
    		return -1;
	}
	return 0;
}

static int do_sleep(struct timeval *time) {
	struct timespec t;
	t.tv_sec = time.tv_sec;
	t.tv_nsec = time.tv_usec * 1000;
	if (nanosleep(&t, NULL) == -1) {
		if (errno != EINTR)
    			syslog(LOG_ERR, "nanosleep failed: %s\n", strerror(errno));
    		return -1;
	}
	return 0;
}

static int set_read_timeout(cc_t t) {
	if (tio.c_cc[VTIME] != t) {
		tio.c_cc[VTIME] = t;
  		if (tcsetattr(devfd, TCSANOW, &tio) == -1) {
    			syslog(LOG_ERR, "setting configuration of serial port failed: %s\n", strerror(errno));
    			return -1;
		}
	}
	return 0;
}

static int usart_send(uint8_t c) {
	if (write(devfd, &c, 1) == (ssize_t)-1) {
    		syslog(LOG_ERR, "writing data to serial port failed: %s\n", strerror(errno));
    		return -1;
	}
	return 0;
}

static int usart_drain(void) {
	if (tcdrain(devfd) == -1) {
    		syslog(LOG_ERR, "draining data to serial port failed: %s\n", strerror(errno));
    		return -1;
	}
	return 0;
}

static int read_lge_reply(void)
{
	uint8_t c;
	ssize_t n = read(devfd, &c, 1);
	if (n == (ssize_t)-1) {
    		syslog(LOG_ERR, "reading data from serial port failed: %s\n", strerror(errno));
    		return -1;
	}

	int state = lge_rx_state;

	if (n == (ssize_t)0) {
		lge_ret_code = LGE_TIMEOUT_ERR;
		state = 0;
	} else {
		if (set_read_timeout(LGE_RX_TIMEOUT) == -1)
    			return -1;
	}

	if (state == 1) {
		if (c == lge_rx_cmd2)
			state = 2;	// Start of telegram
	} else if (state == 6) {
		if (c == 'N' || c == 'O')
			state = 7;
		else
			state = 1;
	} else if (state == 7) {
		if (c == 'G') {
			lge_ret_code = LGE_NG_ERR;
			state = 8;
		} else if (c == 'K') {
			lge_ret_code = LGE_OK;
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
	} else if (state == 12)
		state = 0;	// end of telegram
	else if (state)
		++state;

	lge_rx_state = state;
	return 0;
}

static int send_lge_telegram(uint8_t c1, uint8_t c2, uint8_t v, unsigned int p)
{
	uint8_t c;
	struct timeval now, pause;

	if (timerisset(&lge_pause_time)) {
		if (get_now(&now) == -1)
			return -1;
		if (timercmp(&lge_pause_time, &now, >)) {
			timersub(&lge_pause_time, &now, &pause);
			if (do_sleep(&pause) == -1)
				return -1;
		}
		timerclear(&lge_pause_time);
	}

		// Prepare receiver for reply telegram
	lge_rx_cmd2 = c2;
	lge_rx_state = 1;
	if (set_read_timeout(LGE_RX_START_TIMEOUT) == -1)
		return -1;

		// Send command telegram
	if (usart_send(c1) == -1)
		return -1;
	if (usart_send(c2) == -1)
		return -1;
	if (usart_send(' ') == -1)
		return -1;
	if (usart_send('0') == -1)
		return -1;
	if (usart_send('0') == -1)
		return -1;
	if (usart_send(' ') == -1)
		return -1;
	c = v >> 4;
	if (c > 9)
		c += 'A' - 10;
	else
		c += '0';
	if (usart_send(c) == -1)
		return -1;
	c = v & 0x0F;
	if (c > 9)
		c += 'A' - 10;
	else
		c += '0';
	if (usart_send(c) == -1)
		return -1;
	if (usart_send('\r') == -1)
		return -1;

	if (usart_drain() == -1)
		return -1;
	
		// Wait for reply telegram
	while (lge_rx_state) {
		if (read_lge_reply() == -1)
			return -1;
	}


	if (p > 0 && lge_ret_code == LGE_OK && v != 0xFF) {
		if (get_now(&now) == -1)
			return -1;
		pause.tv_sec = p / 1000000;
		pause.tv_usec = p % 1000000;
		timeradd(&now, &pause, &lge_pause_time);
	} else {
		timerclear(&lge_pause_time);
	}

	return 0;
}

static int send_lge_cmd(unsigned int cmd, uint8_t v)
{
	uint8_t c1, c2;
	unsigned int p;

	lge_ret_code = LGE_UNKNOWN_CMD_ERR;
	if (cmd < LGE_NUM_CMDS && (c1 = lge_cmd_tab[cmd].cmd1))
	{
		c2 = lge_cmd_tab[cmd].cmd2;
		p = lge_cmd_tab[cmd].pause;

		if (p > 0 && v != 0xFF)
		{		// Check current status before sending command with pause time
			if (send_lge_telegram(c1, c2, 0xFF, p) == -1)
				return -1;
			if (lge_ret_code == LGE_OK && lge_rx_value != v) {
				if (send_lge_telegram(c1, c2, v, p) == -1)
					return -1;
			}
		} else {
			if (send_lge_telegram(c1, c2, v, p) == -1)
				return -1;
		}
	}
	return 0;
}

int lge_send(const char *seq) {
	char *s, *p;
	char buf[100];
	unsigned int v;

	if (seq != NULL && devfd != INVALID_DEV_HANDLE) {
		s = strtok_r(strncpy(buf, seq, sizeof(buf)), " ,", &p);
		while (s != NULL) {
			if (sscanf(s, "%x", &v) != 1) {
    				syslog(LOG_ERR, "processing code sequence failed: %s\n", seq);
    				return -1;
			}

			if (send_lge_cmd(v >> 8, (uint8_t)v) == -1)
				return -1;

			if (lge_ret_code != LGE_OK) {
    				syslog(LOG_ERR, "lge command %x failed: %d\n", v, lge_ret_code);
				break;
			}

			s = strtok_r(NULL, " ,", &p);
		}
	}
	return 0;
}
