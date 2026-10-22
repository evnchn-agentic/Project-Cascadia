// mcuctl: talk to the Apple TV 3's MSP430 (IR remote, front LED) on uart1, 250000 8N1.
// Frame: 53 74 op seq lenHi lenLo body crcHi crcLo, CRC-16/CCITT-FALSE over 53 74 .. body
// (docs/research/j33i-mcu-protocol.md). Only GetVersion (0x02), SetLED (0x04) and SetState (0x07)
// can be sent: everything else the firmware knows is untraced, and the bootloader link (uart0)
// can reflash the chip, so this tool never opens it.
//   mcuctl version          print the firmware version
//   mcuctl led MODE         0 off, 1 on, 2 slow blink, 3 fast blink, 4 one blink
//   mcuctl state N          0 booted, 1 standby, 2 normal
//   mcuctl listen SECONDS   print every frame; IR presses decoded
#include <asm/termbits.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const char *DEV = "/dev/ttySAC1";
static int fd;
static uint8_t seq = 1;

static uint16_t crc16_from(uint16_t c, const uint8_t *p, int n)
{
	while (n--) {
		c ^= *p++ << 8;
		for (int i = 0; i < 8; i++)
			c = c & 0x8000 ? c << 1 ^ 0x1021 : c << 1;
	}
	return c;
}

/* The CRC also covers the 53 74 sync bytes. */
static uint16_t crc16(const uint8_t *p, int n)
{
	static const uint8_t sync[2] = { 0x53, 0x74 };
	return crc16_from(crc16_from(0xffff, sync, 2), p, n);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int getbyte(double until)
{
	uint8_t b;
	struct pollfd p = { fd, POLLIN, 0 };
	int ms = (until - now()) * 1000;
	if (ms <= 0 || poll(&p, 1, ms) <= 0 || read(fd, &b, 1) != 1)
		return -1;
	return b;
}

/* Read one frame into f (op at f[0]); returns its length from op to the end of the body, or -1. */
static int getframe(uint8_t *f, double until)
{
	int b, n, len;
	for (;;) {
		if ((b = getbyte(until)) < 0)
			return -1;
		if (b != 0x53)
			continue;
		if ((b = getbyte(until)) != 0x74)
			continue;
		for (n = 0; n < 4; n++)
			if ((b = getbyte(until)) < 0)
				return -1;
			else
				f[n] = b;
		len = f[2] << 8 | f[3];
		if (len > 64)
			continue;
		for (n = 0; n < len + 2; n++)
			if ((b = getbyte(until)) < 0)
				return -1;
			else
				f[4 + n] = b;
		if (crc16(f, 4 + len) != (f[4 + len] << 8 | f[5 + len])) {
			printf("bad crc:");
			for (n = 0; n < 6 + len; n++)
				printf(" %02x", f[n]);
			printf("\n");
			continue;
		}
		return 4 + len;
	}
}

static void show(const uint8_t *f, int n)
{
	printf("op %02x seq %02x:", f[0], f[1]);
	for (int i = 4; i < n; i++)
		printf(" %02x", f[i]);
	if ((f[0] == 0x10 || f[0] == 0x11) && n >= 8)
		printf("   %s key 0x%03x remote %02x", f[0] == 0x10 ? "IR" : "IR-repeat",
		       (f[4] & 0x1f) << 8 | f[7] >> 1, f[6]);
	if (f[0] == 0xff) {
		printf("   \"");
		for (int i = 4; i < n; i++)
			putchar(f[i] >= 32 && f[i] < 127 ? f[i] : '.');
		printf("\"");
	}
	printf("\n");
	fflush(stdout);
}

/* Send one command, wait up to 3 s for the reply with its sequence number; 0 on ACK (0x80). */
static int cmd(uint8_t op, const uint8_t *body, int len)
{
	uint8_t f[80] = { 0x53, 0x74, op, seq, len >> 8, len & 0xff };
	uint16_t c;
	memcpy(f + 6, body, len);
	c = crc16_from(0xffff, f, 6 + len);
	f[6 + len] = c >> 8;
	f[7 + len] = c & 0xff;
	if (write(fd, f, 8 + len) != 8 + len)
		return perror("write"), -1;
	double until = now() + 3;
	uint8_t r[80];
	int n;
	while ((n = getframe(r, until)) >= 0) {
		show(r, n);
		if (r[1] == seq) {
			seq += 2;
			return r[0] == 0x80 ? 0 : -1;
		}
	}
	printf("no reply\n");
	seq += 2;
	return -1;
}

int main(int argc, char **argv)
{
	struct termios2 t;
	if (argc < 2)
		return fprintf(stderr, "usage: mcuctl version | led 0-4 | state 0-2 | listen SECONDS\n"), 2;
	if ((fd = open(DEV, O_RDWR | O_NOCTTY)) < 0)
		return perror(DEV), 1;
	ioctl(fd, TCGETS2, &t);
	t.c_iflag = t.c_oflag = t.c_lflag = 0;
	t.c_cflag = CS8 | CREAD | CLOCAL | BOTHER;
	t.c_ispeed = t.c_ospeed = 250000;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (ioctl(fd, TCSETS2, &t) < 0)
		return perror("TCSETS2"), 1;
	ioctl(fd, TCFLSH, TCIOFLUSH);

	if (!strcmp(argv[1], "version"))
		return cmd(0x02, NULL, 0) ? 1 : 0;
	if (argc > 2 && !strcmp(argv[1], "led")) {
		uint8_t m = atoi(argv[2]);
		if (m > 4)
			return fprintf(stderr, "led modes are 0-4\n"), 2;
		return cmd(0x04, &m, 1) ? 1 : 0;
	}
	if (argc > 2 && !strcmp(argv[1], "state")) {
		uint8_t s = atoi(argv[2]);
		if (s > 2)
			return fprintf(stderr, "states are 0-2\n"), 2;
		return cmd(0x07, &s, 1) ? 1 : 0;
	}
	if (argc > 2 && !strcmp(argv[1], "listen")) {
		double until = now() + atof(argv[2]);
		uint8_t r[80];
		int n;
		while ((n = getframe(r, until)) >= 0)
			show(r, n);
		return 0;
	}
	return fprintf(stderr, "unknown command\n"), 2;
}
