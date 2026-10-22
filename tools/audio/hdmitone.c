// hdmitone: first HDMI audio on the Apple TV 3, by direct register writes (no driver, no DMA).
// Runs iOS's startAudioLink sequence (docs/research/j33i-audio.md sections 3, 6, 7) for 48 kHz
// 16-bit stereo, then writes a square wave into i2s0's TX FIFO for SECONDS.
//   hdmitone [SECONDS] [HZ] [PORT 0-3]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int memfd;

static volatile uint32_t *map(uint32_t pa)
{
	void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, pa & ~0xfffu);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return (volatile uint32_t *)((char *)p + (pa & 0xfff));
}
#define R(b, off) ((b)[(off) / 4])

static void spin_until(volatile uint32_t *r, uint32_t mask, int set, const char *what)
{
	for (int i = 0; i < 100000; i++)
		if (!!(*r & mask) == set)
			return;
	fprintf(stderr, "timeout waiting on %s (0x%08x)\n", what, *r);
}

static void gate_on(uint32_t pa, const char *name)
{
	volatile uint32_t *g = map(pa);
	*g = (*g & ~0x10fu) | 0xf;
	for (int i = 0; i < 100000; i++)
		if (((*g ^ (*g >> 4)) & 0xf) == 0)
			return;
	fprintf(stderr, "gate %s stuck at 0x%08x\n", name, *g);
}

/* P105 s5l-audio.c s5l_nco_set, NCO reference 342 MHz (ADT clock-frequencies). */
static void nco_set(int n, uint32_t hz)
{
	volatile uint32_t *r = map(0x3f100110 + 16 * n);
	r[0] = 0x90000000;
	spin_until(r, 1u << 30, 0, "NCO bit 30");
	r[1] = 2 * hz;
	spin_until(r, 1u << 9, 0, "NCO bit 9");
	r[2] = 2 * hz - 342000000u;
	spin_until(r, 1u << 9, 0, "NCO bit 9");
	r[0] = 0x90000400;
	spin_until(r, 1u << 9, 0, "NCO bit 9");
	r[0] = 0x90000c00;
	spin_until(r, 1u << 9, 0, "NCO bit 9");
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 10;
	int hz = argc > 2 ? atoi(argv[2]) : 1000;
	int port = argc > 3 ? atoi(argv[3]) : 0;

	if ((memfd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
		return perror("/dev/mem"), 1;

	gate_on(0x3f10109c, "AUDIO");
	gate_on(0x3f1010a0, "I2S");

	volatile uint32_t *mst = map(0x34195000), *slv = map(0x34190000 + 0x1000 * port);
	volatile uint32_t *ctrl = map(0x39700000), *core0 = map(0x39710000), *asp = map(0x39710300),
			  *acr = map(0x39710400), *aui = map(0x39710800), *hi2s = map(0x39740000);

	/* I2S master (clocks), 48 kHz 16-bit: section 3 */
	R(mst, 0x3c) = 1;
	R(mst, 0x04) = 0x03000081;
	R(mst, 0x30) = 0x1000;
	R(mst, 0x08) = 0;
	R(mst, 0x34) = 0;
	R(mst, 0x00) = 1;
	/* the data port, slave to the master's clocks */
	R(slv, 0x3c) = 1;
	R(slv, 0x04) = 0x03100099;
	R(slv, 0x30) = 0x1000;
	R(slv, 0x08) = 0;
	R(slv, 0x34) = 0;
	R(slv, 0x00) = 0x15;
	nco_set(0, 1536000);
	R(mst, 0x08) = 0x01feca06;
	R(slv, 0x08) = 0x01feca06;

	/* HDMI: audio from I2S (CTRL+0x70 bit 0 off), then the receiver: section 7 steps 3-6 */
	R(ctrl, 0x70) &= ~1u;
	R(hi2s, 0x0c) = 0x77; R(hi2s, 0x10) = 0x77; R(hi2s, 0x14) = 0x77; R(hi2s, 0x18) = 0x07;
	R(hi2s, 0x04) &= ~3u;
	R(hi2s, 0x08) = (R(hi2s, 0x08) & ~0x7cu) | (1 << 2);	/* 16-bit, 32 fs */
	R(hi2s, 0x00) |= 1;
	R(hi2s, 0x28) = 0x04; R(hi2s, 0x2c) = 0x00; R(hi2s, 0x30) = 0x00;
	R(hi2s, 0x34) = 0x02; R(hi2s, 0x38) = 0x02;		/* channel status: 48 kHz */
	R(hi2s, 0x24) |= 1;
	R(hi2s, 0x54) = 0x03;
	R(hi2s, 0x58) = 0x03;
	R(hi2s, 0x20) = 0x77;
	/* ACR: N = 6144 (48 kHz), CTS measured */
	R(acr, 0x30) = 6144 & 0xff; R(acr, 0x34) = (6144 >> 8) & 0xff; R(acr, 0x38) = 6144 >> 16;
	R(acr, 0x00) = (R(acr, 0x00) & ~7u) | 4;
	/* audio sample packets for 2 channels */
	R(asp, 0x00) = 0;
	R(asp, 0x04) = 0;
	R(asp, 0x10) = 0x08; R(asp, 0x14) = 0x1a; R(asp, 0x18) = 0x2c; R(asp, 0x1c) = 0x3e;
	R(core0, 0x00) |= 4;
	/* audio InfoFrame: 84 01 0a, 2 channels, every VSYNC */
	uint8_t hdr[3] = { 0x84, 0x01, 0x0a }, b1 = 0x01, sum = 0;
	for (int i = 0; i < 3; i++)
		sum += hdr[i], R(aui, 0x10 + 4 * i) = hdr[i];
	sum += b1;
	R(aui, 0x20) = b1;
	for (int i = 1; i < 10; i++)
		R(aui, 0x20 + 4 * i) = 0;
	R(aui, 0x1c) = (uint8_t)(0x100 - sum);
	R(aui, 0x00) = 2;

	printf("set up: CON_0 %08x MODE_SEL %08x ACR_CON %08x AUI_CON %08x MUX_CON %08x i2s%d STATUS %08x\n",
	       R(core0, 0), R(core0, 0x40), R(acr, 0), R(aui, 0), R(hi2s, 0x20), port, R(slv, 0x3c));
	fflush(stdout);

	/* square wave, paced at 48 kHz frames; one 32-bit word = L | R << 16 (GUESS) */
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long frames = 0, total = secs * 48000, half = 48000 / (2 * hz);
	while (frames < total) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		long due = ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec)) / 20833;
		for (; frames < due && frames < total; frames++) {
			int16_t s = (frames / half) & 1 ? 8000 : -8000;
			R(slv, 0x10) = (uint16_t)s | (uint32_t)(uint16_t)s << 16;
		}
	}
	printf("wrote %ld frames; STATUS %08x\n", frames, R(slv, 0x3c));
	return 0;
}
