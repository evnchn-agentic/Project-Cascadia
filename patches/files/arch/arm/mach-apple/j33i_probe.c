// SPDX-License-Identifier: GPL-2.0-only
/*
 * J33i boot probe: "atvprobe=core|late" resets the box from that initcall level through
 * iBoot's own reset line (SoC GPIO 28 <- 0x213, platform_reset in 12H847 iBoot). It brackets
 * how far a boot gets with no console: after the reset, stock iBoot comes up in Recovery.
 * initramfs/init does the same for a bare "atvprobe" once userspace runs.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/string.h>

#define J33I_GPIO28	0x3fa00070

static char probe_at[8];

static int __init atvprobe_setup(char *s)
{
	strscpy(probe_at, s ?: "", sizeof(probe_at));
	return 1;
}
__setup("atvprobe=", atvprobe_setup);

static void __init atvprobe_fire(const char *level)
{
	void __iomem *r;

	if (strcmp(probe_at, level))
		return;
	r = ioremap(J33I_GPIO28, 4);
	if (r)
		writel(0x213, r);
	for (;;)
		cpu_relax();
}

static int __init atvprobe_core(void) { atvprobe_fire("core"); return 0; }
core_initcall(atvprobe_core);
static int __init atvprobe_late(void) { atvprobe_fire("late"); return 0; }
late_initcall(atvprobe_late);
