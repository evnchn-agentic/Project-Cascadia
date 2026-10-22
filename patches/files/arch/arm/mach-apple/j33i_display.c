// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple TV 3 (J33i): switch iBoot's HDMI output from 1280x720p60 to 1920x1080p60 before
 * simplefb probes. iBoot only knows "720p-hdmi". The sequence and its values are
 * docs/research/j33i-display-1080p.md section 4, run live from userspace first: the PHY PLL
 * locks, the TV (ES02 capture) reports 1920x1080, and a 1080p test image scans out straight.
 *
 *   - PHY (I2C bridge words at 0x39780088..a4): only regs 3, 16 and 27 differ from the
 *     74.25 MHz row iBoot programmed (148.5 MHz row of the ADT phy-cfg / iBoot's table);
 *   - HDMI CORE and TG timing: CEA 1080p (iBoot's 720p porches are swapped, iOS's are not);
 *   - DisplayPipe and RGBOUT sizes, plane stride 7680; AVI VIC 16.
 *
 * The framebuffer in the DT is 1920x1080 to match. If the PHY is not in the state this
 * expects, nothing is written and the DT framebuffer is wrong: log it loudly.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/printk.h>

#define HDMI_CTRL	0x39700000
#define HDMI_CORE	0x39710000
#define HDMI_TG		0x39750000
#define HDMI_PHY	0x39780000
#define DISP_PIPE	0x3a200000
#define RGBOUT		0x39600000

/* (offset, value) pairs: CORE timing, then TG timing, two byte registers per value */
static const u16 core_1080[][2] = {
	{ 0x0a0, 0x18 }, { 0x0a4, 0x01 }, { 0x0b8, 0x2d }, { 0x0bc, 0x00 },
	{ 0x0b0, 0x65 }, { 0x0b4, 0x04 }, { 0x0c0, 0x65 }, { 0x0c4, 0x04 },
	{ 0x0c8, 0x98 }, { 0x0cc, 0x08 }, { 0x110, 0x93 }, { 0x114, 0x04 },
	{ 0x118, 0x65 }, { 0x11c, 0x04 }, { 0x120, 0x56 }, { 0x124, 0x00 },
	{ 0x128, 0x82 }, { 0x12c, 0x00 }, { 0x138, 0x04 }, { 0x13c, 0x00 },
	{ 0x130, 0x09 }, { 0x134, 0x00 }, { 0x148, 0x69 }, { 0x14c, 0x04 },
	{ 0x140, 0x6e }, { 0x144, 0x04 }, { 0x150, 0xa4 }, { 0x154, 0x04 },
	{ 0x158, 0xa4 }, { 0x15c, 0x04 },
};
static const u16 tg_1080[][2] = {
	{ 0x18, 0x98 }, { 0x1c, 0x08 }, { 0x20, 0x18 }, { 0x24, 0x01 },
	{ 0x28, 0x80 }, { 0x2c, 0x07 }, { 0x30, 0x65 }, { 0x34, 0x04 },
	{ 0x40, 0x66 }, { 0x44, 0x04 }, { 0x48, 0x2d }, { 0x4c, 0x00 },
	{ 0x50, 0x38 }, { 0x54, 0x04 }, { 0x58, 0x66 }, { 0x5c, 0x04 },
	{ 0x60, 0x93 }, { 0x64, 0x04 }, { 0x80, 0x66 }, { 0x84, 0x04 },
	{ 0x90, 0x66 }, { 0x94, 0x04 },
};

static void phy_wait(void __iomem *phy)
{
	for (int i = 0; i < 1000 && (readl(phy + 0x84) & 1); i++)
		udelay(1);
}

static void phy_rmw(void __iomem *phy, u32 off, u32 mask, u32 val)
{
	phy_wait(phy);
	writel((readl(phy + off) & ~mask) | val, phy + off);
}

static int __init j33i_display_1080p(void)
{
	void __iomem *ctrl, *core, *tg, *phy, *pipe, *rgb;
	int i;

	if (!of_machine_is_compatible("apple,j33iap"))
		return 0;
	ctrl = ioremap(HDMI_CTRL, 0x100);
	core = ioremap(HDMI_CORE, 0x1000);
	tg = ioremap(HDMI_TG, 0x100);
	phy = ioremap(HDMI_PHY, 0x100);
	pipe = ioremap(DISP_PIPE, 0x5000);
	rgb = ioremap(RGBOUT, 0x100);
	if (!ctrl || !core || !tg || !phy || !pipe || !rgb)
		goto out;

	/* Only from the state iBoot leaves: the 74.25 MHz PHY row and 1280x720 in the pipe. */
	if ((readl(phy + 0x88) >> 24) != 0x35 || readl(pipe + 0x1030) != 0x050002d0) {
		pr_err("j33i-display: not iBoot's 720p state (PHY 0x%08x, pipe 0x%08x); left as is, the 1080p framebuffer in the DT will be wrong\n",
		       readl(phy + 0x88), readl(pipe + 0x1030));
		goto out;
	}

	writel(0, tg);					/* TG off */
	writel(readl(core) & ~1, core);			/* HDMI system enable off */
	phy_rmw(phy, 0xa4, 0xff000000, 0);		/* PHY reg 31 = 0: mode set */
	phy_rmw(phy, 0x88, 0xff000000, 0x15000000);	/* reg 3 */
	phy_rmw(phy, 0x98, 0x000000ff, 0x0e);		/* reg 16 */
	phy_rmw(phy, 0xa0, 0xff000000, 0x02000000);	/* reg 27 */
	phy_rmw(phy, 0xa4, 0, 0x80000000);		/* reg 31 = 0x80: done */
	phy_wait(phy);
	mdelay(10);
	for (i = 0; i < 1000 && !(readl(ctrl + 0x20) & 1); i++)
		udelay(10);

	for (i = 0; i < ARRAY_SIZE(core_1080); i++)
		writel(core_1080[i][1], core + core_1080[i][0]);
	for (i = 0; i < ARRAY_SIZE(tg_1080); i++)
		writel(tg_1080[i][1], tg + tg_1080[i][0]);

	writel(0x07800438, pipe + 0x1030);
	writel((readl(pipe + 0x4048) & 0x3f) | 0x1e00, pipe + 0x4048);	/* stride 7680 */
	writel(0x07800438, pipe + 0x4050);
	writel(0x07800438, pipe + 0x4058);
	writel(0x07800438, pipe + 0x4070);
	writel(1919, rgb + 0x40);
	writel(1079, rgb + 0x34);
	writel(39, rgb + 0x6c);
	writel(readl(pipe + 0x1038) | 0x100, pipe + 0x1038);

	if ((readl(core + 0x40) & 3) == 2) {		/* HDMI, not DVI: AVI VIC 16 */
		writel(0x10, core + 0x72c);
		writel(0xe7, core + 0x71c);
	}
	writel(1, tg);
	writel(readl(core) | 1, core);
	pr_info("j33i-display: HDMI now 1920x1080p60 (PHY lock %s)\n",
		readl(ctrl + 0x20) & 1 ? "ok" : "NOT seen");
out:
	iounmap(rgb);
	iounmap(pipe);
	iounmap(phy);
	iounmap(tg);
	iounmap(core);
	iounmap(ctrl);
	return 0;
}
arch_initcall(j33i_display_1080p);
