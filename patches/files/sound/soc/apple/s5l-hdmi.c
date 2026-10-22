// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Apple TV 3's HDMI audio: S5L8947X I2S0 + CDMA into the HDMI TX's own
 * I2S receiver.  Stereo, 16-bit, 44.1 or 48 kHz.
 *
 * docs/research/j33i-audio.md has the evidence; the register setup is the
 * one tools/audio/hdmitone.c made audible on the box (fed by the CPU, not by
 * DMA: the DMA feed below is untested):
 *
 *   I2S master (0x34195000)  BCLK/LRCK only (32 fs), clocked by NCO 0
 *   I2S0 (0x34190000)        slave to those clocks; fed by CDMA channel
 *                            0x17 on request line 1 with 16-bit transfers,
 *                            burst 4, as the ADT's dma-channels say: one
 *                            sample per FIFO write, L then R (from the ADT,
 *                            not seen on hardware)
 *   HDMI TX (0x39700000)     Exynos v1.4 IP.  iBoot leaves it running in HDMI
 *                            mode; this sets up its I2S receiver, ACR (N
 *                            fixed, CTS measured, so the pixel clock does
 *                            not matter), audio sample packets and the audio
 *                            InfoFrame, as iOS's startAudioLink does.
 *
 * The CDMA ring and the hrtimer that reads its position are s5l-audio.c's.
 * A plain ALSA card: there is no codec to link to.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>

/* I2S (S5L8900 heritage, version 3) */
#define I2S_CLKCON		0x00
#define I2S_TXCON		0x04
#define I2S_TXCOM		0x08
#define I2S_TXFIFO		0x10
#define I2S_RXCON		0x30
#define I2S_RXCOM		0x34
#define I2S_STATUS		0x3c
#define I2S_TXCOM_ON		0x01feca06	/* DMA request, interface on, watermark */
#define I2S_CLKCON_STOP		0x34

/* HDMI TX, Exynos regs-hdmi.h v1.4 names */
#define HDMI_AUDIO_CLKSEL	0x070	/* ctrl; bit 0 clear = I2S */
#define HDMI_CON_0		0x000	/* core */
#define HDMI_ASP_EN		BIT(2)
#define HDMI_MODE_SEL		0x040
#define HDMI_MODE_DVI		1
#define HDMI_ASP_CON		0x300
#define HDMI_ASP_SP_FLAT	0x304
#define HDMI_ASP_CHCFG(i)	(0x310 + 4 * (i))
#define HDMI_ACR_CON		0x400
#define HDMI_ACR_N(i)		(0x430 + 4 * (i))
#define HDMI_ACR_MEASURED_CTS	4
#define HDMI_AUI_CON		0x800
#define HDMI_AUI_HEADER(i)	(0x810 + 4 * (i))
#define HDMI_AUI_CHECK_SUM	0x81c
#define HDMI_AUI_BYTE(i)	(0x820 + 4 * ((i) - 1))
#define HDMI_AUI_EVERY_VSYNC	2
#define HDMI_I2S_CLK_CON	0x00	/* hdmi-i2s */
#define HDMI_I2S_CON_1		0x04
#define HDMI_I2S_CON_2		0x08
#define HDMI_I2S_PIN_SEL(i)	(0x0c + 4 * (i))
#define HDMI_I2S_MUX_CON	0x20
#define HDMI_I2S_CH_ST_CON	0x24
#define HDMI_I2S_CH_ST(i)	(0x28 + 4 * (i))
#define HDMI_I2S_MUX_CH		0x54
#define HDMI_I2S_MUX_CUV	0x58

/* CDMA channel */
#define CDMA_CLK_ON(ch)		(4 * ((ch) / 32))
#define CDMA_CH(ch)		(0x1000 * (ch))
#define CDMA_CSR		0x00
#define CDMA_DCR		0x04
#define CDMA_DAR		0x08
#define CDMA_DBR		0x0c
#define CDMA_MAR		0x10
#define CDMA_CAR		0x14
#define CDMA_ERR		0x18
#define CDMA_CSR_ABORT		BIT(1)
#define CDMA_CSR_RUNNING	BIT(16)
#define CDMA_CSR_CLEAR		(BIT(18) | BIT(19) | BIT(20))
#define CDMA_CSR_START		(0x02000000 | CDMA_CSR_CLEAR | BIT(3) | BIT(0))
#define CDMA_CMD_DATA		0x02000103
#define CDMA_DBR_MAX		0x7ffffff0
#define CDMA_MAX_CMDS		64

#define S5L_NCO_I2S_MASTER	0

struct s5l_hdmi {
	struct device *dev;
	void __iomem *mst, *i2s, *ctrl, *core, *hi2s, *nco, *cdma;
	struct clk *i2s_clk, *cdma_clk;
	u32 nco_ref;
	u32 fifo;			/* I2S0's TX FIFO, physical */
	unsigned int ch, req;

	struct snd_pcm_substream *substream;
	__le32 *cmds;
	dma_addr_t cmds_dma;
	dma_addr_t buf_dma;
	unsigned int buf_bytes, period_bytes, periods;
	unsigned int last_period;
	struct hrtimer timer;
	ktime_t tick;
	bool running;
	spinlock_t lock;
};

static int s5l_nco_set(struct s5l_hdmi *a, int n, u32 hz)
{
	void __iomem *r = a->nco + 16 * n;
	u32 v;
	int ret;

	writel(hz ? 0x90000000 : 0, r);
	ret = readl_poll_timeout_atomic(r, v, !(v & BIT(30)), 1, 1000);
	if (ret || !hz)
		return ret;
	writel(2 * hz, r + 4);
	ret = readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	writel(2 * hz - a->nco_ref, r + 8);
	ret |= readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	writel(0x90000400, r);
	ret |= readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	writel(0x90000c00, r);
	ret |= readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	return ret ? -ETIMEDOUT : 0;
}

/* --- I2S and the HDMI TX's audio --------------------------------------- */

/* iOS's configure for 16-bit stereo: the master makes the clocks, I2S0 the data */
static void s5l_i2s_configure(void __iomem *i2s, u32 txcon, u32 clkcon)
{
	writel(1, i2s + I2S_STATUS);
	writel(txcon, i2s + I2S_TXCON);
	writel(0x1000, i2s + I2S_RXCON);
	writel(0, i2s + I2S_TXCOM);
	writel(0, i2s + I2S_RXCOM);
	writel(clkcon, i2s + I2S_CLKCON);
}

static void s5l_hdmi_rmw(void __iomem *r, u32 clear, u32 set)
{
	writel((readl(r) & ~clear) | set, r);
}

/* startAudioLink's register writes (j33i-audio.md section 7, steps 3-10) */
static void s5l_hdmi_audio_start(struct s5l_hdmi *a, unsigned int rate)
{
	unsigned int n = rate == 44100 ? 6272 : 6144;
	static const u8 aui_header[3] = { 0x84, 0x01, 0x0a };
	/* IEC 60958 channel status: PCM, no copyright, Fs, 16 bits */
	u8 ch_st[5] = { 0x04, 0x00, 0x00, rate == 44100 ? 0x00 : 0x02, 0x02 };
	int i;

	s5l_hdmi_rmw(a->ctrl + HDMI_AUDIO_CLKSEL, 1, 0);

	for (i = 0; i < 3; i++)
		writel(0x77, a->hi2s + HDMI_I2S_PIN_SEL(i));
	writel(0x07, a->hi2s + HDMI_I2S_PIN_SEL(3));
	s5l_hdmi_rmw(a->hi2s + HDMI_I2S_CON_1, 3, 0);
	/* basic I2S (bits 1:0 = 0; iBoot leaves 2, left-justified), 16-bit, 32 fs */
	s5l_hdmi_rmw(a->hi2s + HDMI_I2S_CON_2, 0x7f, 1 << 2);
	s5l_hdmi_rmw(a->hi2s + HDMI_I2S_CLK_CON, 0, 1);
	for (i = 0; i < 5; i++)
		writel(ch_st[i], a->hi2s + HDMI_I2S_CH_ST(i));
	s5l_hdmi_rmw(a->hi2s + HDMI_I2S_CH_ST_CON, 0, 1);
	writel(0x03, a->hi2s + HDMI_I2S_MUX_CH);
	writel(0x03, a->hi2s + HDMI_I2S_MUX_CUV);
	writel(0x77, a->hi2s + HDMI_I2S_MUX_CON);

	for (i = 0; i < 3; i++)
		writel((n >> (8 * i)) & 0xff, a->core + HDMI_ACR_N(i));
	s5l_hdmi_rmw(a->core + HDMI_ACR_CON, 7, HDMI_ACR_MEASURED_CTS);

	writel(0, a->core + HDMI_ASP_CON);
	writel(0, a->core + HDMI_ASP_SP_FLAT);
	for (i = 0; i < 4; i++)
		writel(0x08 + 0x12 * i, a->core + HDMI_ASP_CHCFG(i));
	s5l_hdmi_rmw(a->core + HDMI_CON_0, 0, HDMI_ASP_EN);

	/* audio InfoFrame: 2 channels, the rest from the stream */
	for (i = 0; i < 3; i++)
		writel(aui_header[i], a->core + HDMI_AUI_HEADER(i));
	writel(0x01, a->core + HDMI_AUI_BYTE(1));
	for (i = 2; i <= 10; i++)
		writel(0, a->core + HDMI_AUI_BYTE(i));
	writel(0x70, a->core + HDMI_AUI_CHECK_SUM);	/* 0x100 - (0x84 + 0x01 + 0x0a + 0x01) */
	writel(HDMI_AUI_EVERY_VSYNC, a->core + HDMI_AUI_CON);
}

/* iOS's stop: every bit the start set, and only those */
static void s5l_hdmi_audio_stop(struct s5l_hdmi *a)
{
	s5l_hdmi_rmw(a->core + HDMI_CON_0, HDMI_ASP_EN, 0);
	writel(0, a->core + HDMI_AUI_CON);
	s5l_hdmi_rmw(a->core + HDMI_ACR_CON, 7, 0);
	s5l_hdmi_rmw(a->hi2s + HDMI_I2S_MUX_CON, 0x11, 0);
	s5l_hdmi_rmw(a->hi2s + HDMI_I2S_CLK_CON, 1, 0);
}

/* --- CDMA (s5l-audio.c's) ------------------------------------------------- */

static void __iomem *s5l_ch(struct s5l_hdmi *a)
{
	return a->cdma + CDMA_CH(a->ch);
}

static void s5l_dma_stop(struct s5l_hdmi *a)
{
	void __iomem *ch = s5l_ch(a);

	writel(CDMA_CSR_ABORT, ch + CDMA_CSR);
	udelay(10);
	writel(CDMA_CSR_CLEAR, ch + CDMA_CSR);
}

/* start (or restart) the ring at command @first */
static int s5l_dma_start(struct s5l_hdmi *a, unsigned int first)
{
	void __iomem *ch = s5l_ch(a);
	/* 16-bit (the FIFO takes a sample a write), burst 4, memory to device. Measured on an HDMI
	 * capture: the right channel arrives bit-exact, the left is missing (open); 32-bit writes
	 * gave silence. */
	u32 dcr = (a->req << 16) | 0x20 | 0x04 | 0x02;

	writel(CDMA_CSR_CLEAR, ch + CDMA_CSR);
	writel(CDMA_CSR_ABORT, ch + CDMA_CSR);
	udelay(10);
	writel(dcr, ch + CDMA_DCR);
	writel(a->fifo & 0xffffff, ch + CDMA_DAR);
	writel(CDMA_DBR_MAX, ch + CDMA_DBR);
	writel(a->cmds_dma + 32 * first, ch + CDMA_CAR);
	if ((readl(ch + CDMA_DCR) & 0x003f00fe) != dcr) {
		dev_err(a->dev, "CDMA channel %u is locked (DCR %08x)\n",
			a->ch, readl(ch + CDMA_DCR));
		return -EBUSY;
	}
	writel(CDMA_CSR_START, ch + CDMA_CSR);
	return 0;
}

static void s5l_build_ring(struct s5l_hdmi *a)
{
	unsigned int i;

	for (i = 0; i < a->periods; i++) {
		__le32 *c = a->cmds + 8 * i;

		c[0] = cpu_to_le32(a->cmds_dma + 32 * ((i + 1) % a->periods));
		c[1] = cpu_to_le32(CDMA_CMD_DATA);
		c[2] = cpu_to_le32(a->buf_dma + i * a->period_bytes);
		c[3] = cpu_to_le32(a->period_bytes);
	}
	wmb();
}

static unsigned int s5l_dma_pos(struct s5l_hdmi *a)
{
	u32 mar = readl(s5l_ch(a) + CDMA_MAR);

	if (mar < a->buf_dma || mar >= a->buf_dma + a->buf_bytes)
		return 0;
	return mar - a->buf_dma;
}

static enum hrtimer_restart s5l_tick(struct hrtimer *t)
{
	struct s5l_hdmi *a = container_of(t, struct s5l_hdmi, timer);
	struct snd_pcm_substream *ss;
	unsigned int period;
	unsigned long flags;
	u32 csr;

	spin_lock_irqsave(&a->lock, flags);
	if (!a->running) {
		spin_unlock_irqrestore(&a->lock, flags);
		return HRTIMER_NORESTART;
	}
	ss = a->substream;
	period = s5l_dma_pos(a) / a->period_bytes;
	csr = readl(s5l_ch(a) + CDMA_CSR);
	if (!(csr & CDMA_CSR_RUNNING)) {
		/* the byte count ran out (2 GB, ~3 hours) or it hit an error */
		dev_dbg(a->dev, "CDMA stopped (CSR %08x ERR %08x), restarting\n",
			csr, readl(s5l_ch(a) + CDMA_ERR));
		s5l_dma_start(a, (period + 1) % a->periods);
	}
	spin_unlock_irqrestore(&a->lock, flags);

	if (period != a->last_period) {
		a->last_period = period;
		snd_pcm_period_elapsed(ss);
	}
	hrtimer_forward_now(t, a->tick);
	return HRTIMER_RESTART;
}

/* --- PCM ------------------------------------------------------------------ */

static const struct snd_pcm_hardware s5l_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 512 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = CDMA_MAX_CMDS,
};

static int s5l_pcm_open(struct snd_pcm_substream *substream)
{
	struct s5l_hdmi *a = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	rt->hw = s5l_pcm_hw;
	/* a burst is 4 transfers of 2 bytes; keep commands 16-byte aligned */
	ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	/* the channel's own clock, before anything touches its registers */
	writel(BIT(a->ch % 32), a->cdma + CDMA_CLK_ON(a->ch));
	a->substream = substream;
	return 0;
}

static int s5l_pcm_close(struct snd_pcm_substream *substream)
{
	struct s5l_hdmi *a = snd_pcm_substream_chip(substream);

	hrtimer_cancel(&a->timer);
	s5l_dma_stop(a);
	s5l_hdmi_audio_stop(a);
	writel(0, a->mst + I2S_TXCOM);
	writel(I2S_CLKCON_STOP, a->i2s + I2S_CLKCON);
	writel(I2S_CLKCON_STOP, a->mst + I2S_CLKCON);
	s5l_nco_set(a, S5L_NCO_I2S_MASTER, 0);
	a->substream = NULL;
	return 0;
}

static int s5l_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l_hdmi *a = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	/* iOS sends no audio in DVI mode, and switching modes blanks the picture */
	if ((readl(a->core + HDMI_MODE_SEL) & 3) == HDMI_MODE_DVI) {
		dev_err(a->dev, "the HDMI link is in DVI mode\n");
		return -ENXIO;
	}

	hrtimer_cancel(&a->timer);
	s5l_dma_stop(a);
	a->buf_dma = rt->dma_addr;
	a->buf_bytes = snd_pcm_lib_buffer_bytes(substream);
	a->period_bytes = snd_pcm_lib_period_bytes(substream);
	a->periods = rt->periods;
	a->last_period = 0;

	/* hdmitone's order: the ports, the bit clock, the master on, the TX */
	s5l_i2s_configure(a->mst, 0x03000081, 1);
	s5l_i2s_configure(a->i2s, 0x03100099, 0x15);
	ret = s5l_nco_set(a, S5L_NCO_I2S_MASTER, 32 * rt->rate);
	if (ret)
		return ret;
	writel(I2S_TXCOM_ON, a->mst + I2S_TXCOM);
	s5l_hdmi_audio_start(a, rt->rate);
	s5l_build_ring(a);

	/* twice a period, at least once every 10 ms */
	a->tick = ns_to_ktime(min_t(u64, div_u64((u64)NSEC_PER_SEC *
				   rt->period_size, 2 * rt->rate),
				   10 * NSEC_PER_MSEC));
	return 0;
}

static int s5l_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct s5l_hdmi *a = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&a->lock, flags);
		ret = s5l_dma_start(a, a->last_period);
		if (!ret) {
			/* the DMA answers I2S0's request, then the port starts */
			writel(I2S_TXCOM_ON, a->i2s + I2S_TXCOM);
			a->running = true;
		}
		spin_unlock_irqrestore(&a->lock, flags);
		if (!ret)
			hrtimer_start(&a->timer, a->tick, HRTIMER_MODE_REL);
		return ret;
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&a->lock, flags);
		a->running = false;
		writel(0, a->i2s + I2S_TXCOM);
		s5l_dma_stop(a);
		spin_unlock_irqrestore(&a->lock, flags);
		/* may be called from the timer itself (an xrun): don't wait */
		hrtimer_try_to_cancel(&a->timer);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t s5l_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct s5l_hdmi *a = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime, s5l_dma_pos(a));
}

static const struct snd_pcm_ops s5l_pcm_ops = {
	.open = s5l_pcm_open,
	.close = s5l_pcm_close,
	.prepare = s5l_pcm_prepare,
	.trigger = s5l_pcm_trigger,
	.pointer = s5l_pcm_pointer,
};

/* --- probe ---------------------------------------------------------------- */

static int s5l_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct s5l_hdmi *a;
	int ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	spin_lock_init(&a->lock);

	/* devm_ioremap, not _resource: the NCOs sit inside the PMGR */
#define MAP(field, name) do {							\
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);	\
		if (!res)							\
			return dev_err_probe(dev, -EINVAL, "no %s window\n", name); \
		a->field = devm_ioremap(dev, res->start, resource_size(res));	\
		if (!a->field)							\
			return -ENOMEM;						\
	} while (0)
	MAP(mst, "i2s-master");
	MAP(i2s, "i2s");
	MAP(ctrl, "hdmi-ctrl");
	MAP(core, "hdmi-core");
	MAP(hi2s, "hdmi-i2s");
	MAP(nco, "nco");
	MAP(cdma, "cdma");
#undef MAP
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "i2s");
	a->fifo = res->start + I2S_TXFIFO;

	if (of_property_read_u32(dev->of_node, "apple,nco-reference-hz", &a->nco_ref))
		a->nco_ref = 342000000;
	if (of_property_read_u32_index(dev->of_node, "apple,dma-channel", 0, &a->ch) ||
	    of_property_read_u32_index(dev->of_node, "apple,dma-channel", 1, &a->req))
		return dev_err_probe(dev, -EINVAL, "no apple,dma-channel\n");

	/* Held for as long as the driver is bound: one of AUDIO/I2S/CDMA also feeds the Ethernet
	 * block (switching them off at close, or clk_disable_unused at boot, left the Ethernet
	 * status register reading junk and its interrupt storming). */
	/* AUDIO first: enabling I2S does not power its parent through the gate driver */
	if (IS_ERR(devm_clk_get_enabled(dev, "audio")))
		return dev_err_probe(dev, -ENODEV, "no AUDIO power state\n");
	a->i2s_clk = devm_clk_get_enabled(dev, "i2s");
	if (IS_ERR(a->i2s_clk))
		return dev_err_probe(dev, PTR_ERR(a->i2s_clk), "no I2S power state\n");
	a->cdma_clk = devm_clk_get_enabled(dev, "cdma");
	if (IS_ERR(a->cdma_clk))
		return dev_err_probe(dev, PTR_ERR(a->cdma_clk), "no CDMA power state\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	a->cmds = dmam_alloc_coherent(dev, 32 * CDMA_MAX_CMDS, &a->cmds_dma, GFP_KERNEL);
	if (!a->cmds)
		return -ENOMEM;

	hrtimer_init(&a->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	a->timer.function = s5l_tick;

	ret = snd_devm_card_new(dev, -1, NULL,
				THIS_MODULE, 0, &card);
	if (ret)
		return ret;
	strscpy(card->driver, "s5l-hdmi", sizeof(card->driver));
	strscpy(card->shortname, "AppleTV HDMI", sizeof(card->shortname));
	strscpy(card->longname, "Apple TV 3 HDMI audio", sizeof(card->longname));

	ret = snd_pcm_new(card, "HDMI", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	pcm->private_data = a;
	strscpy(pcm->name, "HDMI", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &s5l_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev, 128 * 1024,
				       s5l_pcm_hw.buffer_bytes_max);

	return snd_card_register(card);
}

static const struct of_device_id s5l_hdmi_of_match[] = {
	{ .compatible = "apple,s5l8947x-hdmi-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_hdmi_of_match);

static struct platform_driver s5l_hdmi_driver = {
	.driver = {
		.name = "s5l-hdmi",
		.of_match_table = s5l_hdmi_of_match,
	},
	.probe = s5l_hdmi_probe,
};
module_platform_driver(s5l_hdmi_driver);

MODULE_DESCRIPTION("Apple TV 3 (S5L8947X) HDMI audio: I2S0, CDMA, HDMI TX");
MODULE_LICENSE("GPL");
