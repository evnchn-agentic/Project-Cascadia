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
#include <linux/hrtimer.h>
#include <linux/kernel_stat.h>
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

/* "atvprobe=ctrc|ctrs": does the 24 MHz watchdog counter (the clocksource and delay timer) still
 * run at core / subsys level?  Two reads around a pure CPU loop (no delay-timer use); reset only
 * if it advanced, so a hang means the counter is frozen. */
static void __init atvprobe_ctr(const char *level)
{
	void __iomem *w;
	u32 a, b;
	volatile unsigned long i;

	if (strcmp(probe_at, level))
		return;
	w = ioremap(0x3f103020, 4);
	a = readl(w);
	for (i = 0; i < 20000000; i++)
		;
	b = readl(w);
	if (b != a)
		j33i_reset();
	for (;;)
		cpu_relax();
}

/* "atvprobe=ctrw": at core, spin ~2 s timed by PMCCNTR (the CPU cycle counter, ~850 MHz, not the
 * watchdog), then the same counter check.  A hang = the watchdog counter dies after a fixed
 * time, not because of some initcall. */
static inline u32 j33i_pmccntr(void)
{
	u32 v;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
	return v;
}

static void __init atvprobe_ctrw(void)
{
	u32 t0, n = 0;

	if (strcmp(probe_at, "ctrw"))
		return;
	t0 = j33i_pmccntr();
	while (n < 4) {			/* 4 x ~0.5 s; PMCCNTR wraps every ~5 s */
		if (j33i_pmccntr() - t0 > 425000000u) {
			t0 = j33i_pmccntr();
			n++;
		}
	}
	strscpy(probe_at, "ctrc", sizeof(probe_at));
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

/* "atvprobe=storm": a hrtimer (hard-irq context, so it still runs during an IRQ storm) samples
 * the total interrupt count every 500 ms; at 4 s it resets if the last 500 ms saw more than
 * 20000 interrupts (a storm) and otherwise lets the boot carry on.  Reset = storm. */
static struct hrtimer storm_timer;
static unsigned long storm_last;
static int storm_ticks;

static enum hrtimer_restart storm_cb(struct hrtimer *t)
{
	unsigned long now = kstat_cpu_irqs_sum(0), d = now - storm_last;

	storm_last = now;
	if (++storm_ticks == 8) {
		if (d > 20000)
			j33i_reset();
		return HRTIMER_NORESTART;
	}
	hrtimer_forward_now(t, ms_to_ktime(500));
	return HRTIMER_RESTART;
}

static int __init atvprobe_core(void)
{
	gpio28 = ioremap(J33I_GPIO28, 4);
	if (!strcmp(probe_at, "storm")) {
		storm_last = kstat_cpu_irqs_sum(0);
		hrtimer_init(&storm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
		storm_timer.function = storm_cb;
		hrtimer_start(&storm_timer, ms_to_ktime(500), HRTIMER_MODE_REL_HARD);
	}
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
	atvprobe_ctrw();
	atvprobe_ctr("ctrc");
	atvprobe_fire("core");
	return 0;
}
core_initcall(atvprobe_core);
static int __init atvprobe_arch(void) { atvprobe_ctr("ctra"); atvprobe_fire("arch"); return 0; }
arch_initcall(atvprobe_arch);
/* arch_initcall_sync: after every level-3 initcall; linked from arch/, so before drivers/of's
 * of_platform_default_populate_init in the same 3s level. */
static int __init atvprobe_archsync(void) { atvprobe_ctr("ctrx"); atvprobe_fire("archsy"); return 0; }
arch_initcall_sync(atvprobe_archsync);
static int __init atvprobe_subsys(void)
{
	/* "atvprobe=wait": the same checkpoint, but only after 3 s of mdelay with interrupts on:
	 * a hang here means something kills the CPU over time (an IRQ storm), not an initcall. */
	if (!strcmp(probe_at, "wait")) {
		mdelay(3000);
		j33i_reset();
	}
	/* waitnp: nothing but IRQ handlers can run during the wait; waitirq: not even those. */
	if (!strcmp(probe_at, "waitnp")) {
		preempt_disable();
		mdelay(3000);
		j33i_reset();
	}
	if (!strcmp(probe_at, "waitirq")) {
		local_irq_disable();
		mdelay(3000);
		j33i_reset();
	}
	atvprobe_ctr("ctrs");
	atvprobe_fire("subsys");
	return 0;
}
subsys_initcall(atvprobe_subsys);
static int __init atvprobe_fs(void) { atvprobe_fire("fs"); return 0; }
fs_initcall(atvprobe_fs);
static int __init atvprobe_devsync(void) { atvprobe_fire("devsync"); return 0; }
device_initcall_sync(atvprobe_devsync);
static int __init atvprobe_late(void) { atvprobe_fire("late"); return 0; }
late_initcall(atvprobe_late);
