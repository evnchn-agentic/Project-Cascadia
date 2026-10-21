// SPDX-License-Identifier: GPL-2.0-only
/*
 * J33i boot probe: "atvprobe=core|arch|subsys|fs|devsync|late" (also jiffy, panic) resets the box from that initcall level through
 * iBoot's own reset line (SoC GPIO 28 <- 0x213, platform_reset in 12H847 iBoot). It brackets
 * how far a boot gets with no console: after the reset, stock iBoot comes up in Recovery.
 * initramfs/init does the same for a bare "atvprobe" once userspace runs.
 */
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/string.h>

#define J33I_GPIO28	0x3fa00070

static char probe_at[8];

static int __init atvprobe_setup(char *s)
{
	strscpy(probe_at, s ?: "", sizeof(probe_at));
	return 1;
}
__setup("atvprobe=", atvprobe_setup);

static void __iomem *gpio28;

static void j33i_reset(void)
{
	if (gpio28)
		writel(0x213, gpio28);
	for (;;)
		cpu_relax();
}

static void __init atvprobe_fire(const char *level)
{
	if (!strcmp(probe_at, level))
		j33i_reset();
}

/* "atvprobe=panic": reset on any panic instead (no restart handler here), so a reset after a
 * hang-looking boot tells a panic apart from a true hang. */
static int j33i_panic(struct notifier_block *nb, unsigned long ev, void *p)
{
	j33i_reset();
	return NOTIFY_DONE;
}
static struct notifier_block j33i_panic_nb = { .notifier_call = j33i_panic };

static int __init atvprobe_core(void)
{
	gpio28 = ioremap(J33I_GPIO28, 4);
	if (!strcmp(probe_at, "panic"))
		atomic_notifier_chain_register(&panic_notifier_list, &j33i_panic_nb);
	/* "atvprobe=jiffy": reset only if the tick runs (jiffies move across a ~0.5 s busy-wait),
	 * hang if it does not.  Tells a working clockevent from a dead one. */
	if (!strcmp(probe_at, "jiffy")) {
		unsigned long j0 = jiffies;

		mdelay(500);
		if (time_after(jiffies, j0))
			j33i_reset();
		for (;;)
			cpu_relax();
	}
	atvprobe_fire("core");
	return 0;
}
core_initcall(atvprobe_core);
static int __init atvprobe_arch(void) { atvprobe_fire("arch"); return 0; }
arch_initcall(atvprobe_arch);
/* arch_initcall_sync: after every level-3 initcall; linked from arch/, so before drivers/of's
 * of_platform_default_populate_init in the same 3s level. */
static int __init atvprobe_archsync(void) { atvprobe_fire("archsy"); return 0; }
arch_initcall_sync(atvprobe_archsync);
static int __init atvprobe_subsys(void) { atvprobe_fire("subsys"); return 0; }
subsys_initcall(atvprobe_subsys);
static int __init atvprobe_fs(void) { atvprobe_fire("fs"); return 0; }
fs_initcall(atvprobe_fs);
static int __init atvprobe_devsync(void) { atvprobe_fire("devsync"); return 0; }
device_initcall_sync(atvprobe_devsync);
static int __init atvprobe_late(void) { atvprobe_fire("late"); return 0; }
late_initcall(atvprobe_late);
