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
 * The framebuffer in the DT is 1920x1080 to match. If the hardware is not in the state this
 * expects, nothing is written and the DT framebuffer is put back to 1280x720.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

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

/* iBoot's 74.25 MHz PHY words (0x88..0xa0; reg 0, the low byte of 0x88, is not checked) */
static const u32 phy_720[] = { 0x353ed100, 0x04f82040, 0xd973a081, 0x80aca045,
			       0x04118007, 0x82442202, 0x0124a554 };

static bool phy_idle(void __iomem *phy)
{
	for (int i = 0; i < 1000; i++) {
		if (!(readl(phy + 0x84) & 1))
			return true;
		udelay(1);
	}
	return false;
}

static bool phy_rmw(void __iomem *phy, u32 off, u32 mask, u32 val)
{
	if (!phy_idle(phy))
		return false;
	writel((readl(phy + off) & ~mask) | val, phy + off);
	return true;
}

/* Leave the DT framebuffer describing what iBoot left (1280x720), so a skipped switch
 * still gives a correct console. */
static void fb_back_to_720p(void)
{
	static const struct { const char *name; u32 val; } p[] = {
		{ "width", 1280 }, { "height", 720 }, { "stride", 5120 } };
	struct device_node *np = of_find_compatible_node(NULL, NULL, "simple-framebuffer");

	for (int i = 0; np && i < ARRAY_SIZE(p); i++) {
		struct property *prop = kzalloc(sizeof(*prop) + 4, GFP_KERNEL);

		if (!prop)
			break;
		prop->name = kstrdup(p[i].name, GFP_KERNEL);
		prop->length = 4;
		prop->value = prop + 1;
		*(__be32 *)prop->value = cpu_to_be32(p[i].val);
		of_update_property(np, prop);
	}
	of_node_put(np);
}

/* "atv_hdmi=720" keeps iBoot's 720p, for a TV that will not take 1080p. */
static bool keep_720p __initdata;

static int __init atv_hdmi_setup(char *s)
{
	keep_720p = s && !strcmp(s, "720");
	return 1;
}
__setup("atv_hdmi=", atv_hdmi_setup);

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
		goto skip;
	if (keep_720p) {
		pr_info("j33i-display: atv_hdmi=720, staying at 1280x720\n");
		goto skip;
	}

	/* Only from the state iBoot leaves: its 74.25 MHz PHY row, 1280x720 in the pipe, the
	 * scanout at the DT framebuffer, and an idle PHY bridge. */
	for (i = 0; i < ARRAY_SIZE(phy_720); i++)
		if ((readl(phy + 0x88 + 4 * i) & (i ? ~0u : ~0xffu)) != phy_720[i])
			break;
	if (i < ARRAY_SIZE(phy_720) || readl(pipe + 0x1030) != 0x050002d0 ||
	    readl(pipe + 0x4044) != 0x9f570000 || !phy_idle(phy)) {
		pr_err("j33i-display: not iBoot's 720p state (PHY word %d 0x%08x, pipe 0x%08x, base 0x%08x); staying at 720p\n",
		       i, readl(phy + 0x88 + 4 * min_t(int, i, ARRAY_SIZE(phy_720) - 1)),
		       readl(pipe + 0x1030), readl(pipe + 0x4044));
		goto skip;
	}

	writel(0, tg);					/* TG off */
	writel(readl(core) & ~1, core);			/* HDMI system enable off */
	/* From here on the output is being changed: a stuck PHY or no PLL lock can only be
	 * reported (the picture is lost either way; reboot recovers). */
	if (!(phy_rmw(phy, 0xa4, 0xff000000, 0) &&		/* PHY reg 31 = 0: mode set */
	      phy_rmw(phy, 0x88, 0xff000000, 0x15000000) &&	/* reg 3 */
	      phy_rmw(phy, 0x98, 0x000000ff, 0x0e) &&		/* reg 16 */
	      phy_rmw(phy, 0xa0, 0xff000000, 0x02000000) &&	/* reg 27 */
	      phy_rmw(phy, 0xa4, 0, 0x80000000) &&		/* reg 31 = 0x80: done */
	      phy_idle(phy)))
		pr_err("j33i-display: HDMI PHY bridge stuck busy while switching to 1080p\n");
	mdelay(10);
	for (i = 0; i < 1000 && !(readl(ctrl + 0x20) & 1); i++)
		udelay(10);
	if (!(readl(ctrl + 0x20) & 1))
		pr_err("j33i-display: HDMI PHY PLL did not lock at 148.5 MHz\n");

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
	goto out;
skip:
	fb_back_to_720p();
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
