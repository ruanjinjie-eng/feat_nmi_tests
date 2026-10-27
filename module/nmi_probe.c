// SPDX-License-Identifier: GPL-2.0
/*
 * nmi_probe: FEAT_NMI / pseudo-NMI capability probe and functional tests.
 *
 * Reported through /proc/nmi_probe:
 *   supports=   PE advertises FEAT_NMI (ARM64_HAS_NMI)
 *   active=     kernel actually uses architected NMIs (ARM64_NMI)
 *   pseudo=     kernel uses GIC priority masking / pseudo-NMIs
 *   runs/fast_runs/slow_runs + delta stats:
 *               single-target backtrace IPI latency test.  A helper thread
 *               on another CPU disables local IRQs for HOLD_MS; the sender
 *               then triggers cpumask_backtrace().  If the IPI is an
 *               NMI/pseudo-NMI it is delivered during the IRQ-off window
 *               and the call returns quickly, otherwise it blocks until
 *               the helper re-enables IRQs.
 *   bc_* :      broadcast variant: helpers on several CPUs hold IRQs off,
 *               then one backtrace IPI is broadcast to all of them at once
 *               (concurrent NMI delivery).
 *
 * Writing "1" to /proc/nmi_probe reruns the whole series.
 */
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <asm/cpufeature.h>
#include <asm/interrupts/common_flags.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>

#define HOLD_MS			1500
#define FAST_THRESHOLD_US	750000
/* Printing one register dump per target costs time; scale the broadcast
 * threshold but keep it well below HOLD_MS so regular IRQs are still slow. */
#define BC_THRESHOLD_US		(HOLD_MS * 1000 * 3 / 4)

static unsigned int runs = 1;
module_param(runs, uint, 0600);
MODULE_PARM_DESC(runs, "backtrace IPI latency iterations (default 1)");

static unsigned int bc_max = 4;
module_param(bc_max, uint, 0600);
MODULE_PARM_DESC(bc_max, "max CPUs targeted by the broadcast test (default 4)");

static unsigned int hold_ms = 1500;
module_param(hold_ms, uint, 0600);
MODULE_PARM_DESC(hold_ms, "IRQ-off window per test iteration in ms (default 1500)");

static bool caps_supports;
static bool caps_active;
static bool caps_pseudo;

static unsigned int runs_done;
static unsigned int fast_runs;
static s64 last_delta_us = -1;
static s64 min_delta_us = S64_MAX;
static s64 max_delta_us = -1;

static unsigned int bc_runs_done;
static unsigned int bc_fast_runs;
static unsigned int bc_last_cpus;
static s64 bc_last_delta_us = -1;
static s64 bc_min_delta_us = S64_MAX;
static s64 bc_max_delta_us = -1;

struct holder_ctx {
	atomic_t ready;
	atomic_t go;
	atomic_t irqoff;
	atomic_t done;
	bool mask_allint;	/* instead of DAIF, mask NMIs via ALLINT */
};

static struct holder_ctx single_ctx;

/*
 * Wait for the sender's GO, then spin with local IRQs disabled.  The window
 * starts only when the sender is ready, so the sender always gets the full
 * HOLD_MS to observe delivery.  If NMIs work, the backtrace IPI must be
 * serviced during this window; if it is a plain IRQ it will only be
 * serviced after local_irq_enable().
 */
static int holder_fn(void *data)
{
	struct holder_ctx *c = data;
	ktime_t end;

	atomic_set(&c->ready, 1);
	while (!atomic_read(&c->go)) {
		if (kthread_should_stop())
			return 0;
		cpu_relax();
	}

	if (c->mask_allint)
		_allint_set();
	else
		local_irq_disable();
	atomic_set(&c->irqoff, 1);
	end = ktime_add_ms(ktime_get(), hold_ms ? hold_ms : HOLD_MS);
	while (ktime_before(ktime_get(), end))
		cpu_relax();
	if (c->mask_allint)
		_allint_clear();
	else
		local_irq_enable();
	atomic_set(&c->done, 1);

	return 0;
}

static struct task_struct *start_holder(struct holder_ctx *c, int cpu)
{
	struct task_struct *t;

	atomic_set(&c->ready, 0);
	atomic_set(&c->go, 0);
	atomic_set(&c->irqoff, 0);
	atomic_set(&c->done, 0);

	t = kthread_create_on_cpu(holder_fn, c, cpu, "nmi_holder/%u");
	if (IS_ERR(t))
		return t;
	wake_up_process(t);
	return t;
}

static bool wait_flag_sleep(atomic_t *a, int val, int timeout_ms)
{
	int i;

	for (i = 0; i < timeout_ms; i++) {
		if (atomic_read(a) == val)
			return true;
		msleep(1);
	}
	return false;
}

static void stop_holder(struct task_struct *t)
{
	if (t)
		kthread_stop(t);
}

/*
 * Single-target latency test.  Returns the measured time, or -1 if the
 * test could not run.
 */
static s64 run_single_test_ctx(struct holder_ctx *ctx, bool mask_allint)
{
	cpumask_t mask;
	struct task_struct *t;
	ktime_t start, stop;
	s64 delta = -1;
	int target, i, tries;
	bool pinned = false;

	ctx->mask_allint = mask_allint;
	target = raw_smp_processor_id() == 0 ? 1 : 0;
	if (!cpu_online(target))
		target = cpumask_first(cpu_online_mask);

	t = start_holder(ctx, target);
	if (IS_ERR(t))
		return -1;

	if (!wait_flag_sleep(&ctx->ready, 1, 1000)) {
		pr_warn("nmi_probe: holder thread did not start\n");
		goto out;
	}

	/* Pin ourselves to a CPU that is not the target. */
	for (tries = 0; tries < 1000; tries++) {
		preempt_disable();
		if (smp_processor_id() != target) {
			pinned = true;
			break;
		}
		preempt_enable();
		msleep(1);
	}
	if (!pinned)
		goto out;

	cpumask_clear(&mask);
	cpumask_set_cpu(target, &mask);

	atomic_set(&ctx->go, 1);
	for (i = 0; i < 20000 && !atomic_read(&ctx->irqoff); i++)
		udelay(10);

	if (!atomic_read(&ctx->irqoff)) {
		pr_warn("nmi_probe: helper never disabled IRQs\n");
		preempt_enable();
		goto out;
	}

	start = ktime_get();
	cpumask_backtrace(&mask, -1);
	stop = ktime_get();
	delta = ktime_to_us(ktime_sub(stop, start));
	preempt_enable();

out:
	/* kthread_stop() waits for the IRQ-off window to finish if needed. */
	stop_holder(t);
	return delta;
}

static s64 run_single_test(void)
{
	return run_single_test_ctx(&single_ctx, false);
}

/*
 * ALLINT masking: with ALLINT set, an NMI must stay pending even though
 * DAIF is clear.  The helper masks via ALLINT instead of DAIF, so a working
 * implementation only delivers the backtrace IPI after ALLINT is cleared
 * (the call is slow).  If the NMI were delivered while ALLINT is set, the
 * architected masking would be broken.
 */
static struct holder_ctx allint_ctx;
static s64 allint_mask_delta_us = -1;
static int allint_mask_ok = -1;

static void run_allint_mask_test(void)
{
	s64 delta;

	if (!caps_active)
		return;

	delta = run_single_test_ctx(&allint_ctx, true);
	if (delta < 0)
		return;

	allint_mask_delta_us = delta;
	allint_mask_ok = delta >
		(s64)((hold_ms ? hold_ms : HOLD_MS) * 1000 / 2);
}

/*
 * Broadcast latency test: helpers on up to bc_max CPUs hold IRQs off and a
 * single backtrace IPI is broadcast to all of them, exercising concurrent
 * NMI delivery (the same path as sysrq-l / hardlockup all-CPU backtrace).
 */
static s64 run_broadcast_test(unsigned int *nr_cpus_out)
{
	struct holder_ctx *ctx;
	struct task_struct **tasks;
	cpumask_t mask;
	ktime_t start, stop;
	s64 delta = -1;
	unsigned int nr;
	int cpu, i, tries;
	bool pinned = false, all_ready, all_off = false;

	get_cpu();
	cpumask_copy(&mask, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), &mask);
	put_cpu();

	if (cpumask_weight(&mask) > bc_max) {
		unsigned int seen = 0;

		for_each_cpu(cpu, &mask)
			if (seen++ >= bc_max)
				cpumask_clear_cpu(cpu, &mask);
	}
	nr = cpumask_weight(&mask);
	*nr_cpus_out = nr;
	if (!nr)
		return -1;

	ctx = kcalloc(nr_cpu_ids, sizeof(*ctx), GFP_KERNEL);
	tasks = kcalloc(nr_cpu_ids, sizeof(*tasks), GFP_KERNEL);
	if (!ctx || !tasks) {
		kfree(ctx);
		kfree(tasks);
		return -1;
	}

	for_each_cpu(cpu, &mask) {
		tasks[cpu] = start_holder(&ctx[cpu], cpu);
		if (IS_ERR(tasks[cpu])) {
			tasks[cpu] = NULL;
			cpumask_clear_cpu(cpu, &mask);
		}
	}

	/* Wait for all helpers to be ready (IRQs still on). */
	for (i = 0; i < 2000; i++) {
		all_ready = true;
		for_each_cpu(cpu, &mask) {
			if (!atomic_read(&ctx[cpu].ready)) {
				all_ready = false;
				break;
			}
		}
		if (all_ready)
			break;
		msleep(1);
	}

	/* Pin ourselves off the target set. */
	for (tries = 0; tries < 1000; tries++) {
		preempt_disable();
		if (!cpumask_test_cpu(smp_processor_id(), &mask)) {
			pinned = true;
			break;
		}
		preempt_enable();
		msleep(1);
	}
	if (!pinned)
		goto out;

	for_each_cpu(cpu, &mask)
		atomic_set(&ctx[cpu].go, 1);

	for (i = 0; i < 20000; i++) {
		all_off = true;
		for_each_cpu(cpu, &mask) {
			if (!atomic_read(&ctx[cpu].irqoff)) {
				all_off = false;
				break;
			}
		}
		if (all_off)
			break;
		udelay(10);
	}

	if (all_off) {
		start = ktime_get();
		cpumask_backtrace(&mask, -1);
		stop = ktime_get();
		delta = ktime_to_us(ktime_sub(stop, start));
	}
	preempt_enable();

out:
	for_each_cpu(cpu, &mask)
		stop_holder(tasks[cpu]);
	kfree(tasks);
	kfree(ctx);
	return delta;
}

/*
 * Exception-state / irqflags semantics.  These directly exercise the
 * DAIF+PMR+ALLINT refactor:
 *   - arch_irqs_disabled() must account for ALLINT (FEAT_NMI);
 *   - local_irq_disable() must leave ALLINT clear (NMIs stay unmasked);
 *   - save/restore and nesting must preserve the state;
 *   - with pseudo-NMIs, masking goes through PMR, not DAIF.
 * With CONFIG_DEBUG_IRQFLAGS the arch code additionally validates the
 * DAIF/PMR/ALLINT combination on these operations.
 */
static bool irqflags_ok;
static int allint_clear_on_disable = -1;
static int allint_reported = -1;
static int pmr_irq_disable = -1;

static void run_irqflags_test(void)
{
	bool ok = true;
	unsigned long f1, f2;

	/* save/restore round trip */
	local_irq_save(f1);
	ok &= arch_irqs_disabled();
	ok &= !arch_irqs_disabled_flags(f1);
	local_irq_restore(f1);
	ok &= !arch_irqs_disabled();

	/* nesting */
	local_irq_save(f1);
	local_irq_disable();
	local_irq_save(f2);
	local_irq_restore(f2);
	ok &= arch_irqs_disabled();
	local_irq_restore(f1);
	ok &= !arch_irqs_disabled();

	if (caps_active) {
#ifdef CONFIG_ARM64_NMI
		u64 allint;

		/*
		 * Architected NMI is enabled: DAIF.I must not mask the
		 * superpriority interrupts, i.e. ALLINT stays clear.
		 */
		local_irq_disable();
		allint = read_sysreg_s(SYS_ALLINT);
		allint_clear_on_disable = !(allint & ALLINT_ALLINT);
		ok &= allint_clear_on_disable;
		local_irq_enable();

		/* arch_irqs_disabled() must consider ALLINT too. */
		_allint_set();
		allint_reported = arch_irqs_disabled();
		_allint_clear();
		ok &= allint_reported;
		ok &= !arch_irqs_disabled();
#endif
	} else if (caps_pseudo) {
		local_irq_disable();
		pmr_irq_disable = (read_sysreg_s(SYS_ICC_PMR_EL1) ==
				   GIC_PRIO_IRQOFF);
		ok &= pmr_irq_disable && arch_irqs_disabled();
		local_irq_enable();
		ok &= (read_sysreg_s(SYS_ICC_PMR_EL1) == GIC_PRIO_IRQON);
		ok &= !arch_irqs_disabled();
	} else {
		local_irq_disable();
		ok &= arch_irqs_disabled();
		local_irq_enable();
		ok &= !arch_irqs_disabled();
	}

	irqflags_ok = ok;
}

/*
 * Kprobe paths:
 *   - probing a normal function must work (and exercises the ALLINT
 *     save/restore added around kprobes);
 *   - probing an instruction that accesses ALLINT must be rejected
 *     (MSR ALLINT at a function entry, MRS ALLINT inside a function).
 */
#define ALLINT_MRS_INSN	0xd5384300U	/* mrs xN, allint (Rt masked) */

static int kprobe_rc = -1;
static int kprobe_hits;
static int kprobe_msr_reject_rc = -1000;	/* -1000: not tested */
static int kprobe_mrs_reject_rc = -1000;

static int nmi_probe_kprobe_pre(struct kprobe *p, struct pt_regs *regs)
{
	kprobe_hits++;
	return 0;
}

static struct kprobe vfs_kp = {
	.symbol_name = "vfs_read",
	.pre_handler = nmi_probe_kprobe_pre,
};

static unsigned int find_allint_mrs(void *start, unsigned int max_bytes)
{
	u32 *p = start;
	unsigned int i;

	for (i = 0; i < max_bytes / sizeof(u32); i++) {
		u32 insn = READ_ONCE(p[i]);

		if ((insn & ~0x1fU) == ALLINT_MRS_INSN)
			return i;
	}
	return ~0U;
}

static void run_kprobe_reject_tests(void)
{
	static const char *const syms[] = {
		"get_cpu_fpsimd_context",
		"fpsimd_bind_state_to_cpu",
		"enable_debug_monitors",
	};
	unsigned int i;

	/* MRS ALLINT: must be rejected by the insn decoder. */
	for (i = 0; i < ARRAY_SIZE(syms); i++) {
		struct kprobe tmp = {
			.symbol_name = syms[i],
			.pre_handler = nmi_probe_kprobe_pre,
		};
		void *start;
		unsigned int off;

		if (register_kprobe(&tmp))
			continue;
		start = tmp.addr;
		off = find_allint_mrs(start, 512);
		unregister_kprobe(&tmp);
		if (off == ~0U)
			continue;

		{
			struct kprobe bad = {
				.addr = (kprobe_opcode_t *)start + off,
				.pre_handler = nmi_probe_kprobe_pre,
			};

			kprobe_mrs_reject_rc = register_kprobe(&bad);
			if (kprobe_mrs_reject_rc == 0)
				unregister_kprobe(&bad);
		}
		break;
	}

	/* MSR ALLINT at the first instruction of nmi_enable. */
	{
		struct kprobe msr = {
			.symbol_name = "nmi_enable",
			.pre_handler = nmi_probe_kprobe_pre,
		};

		kprobe_msr_reject_rc = register_kprobe(&msr);
		if (kprobe_msr_reject_rc == 0)
			unregister_kprobe(&msr);
	}
}

/*
 * Validate the genirq change: add_interrupt_randomness() must not be called
 * in NMI context (it is unsafe).  Probe it and count how many calls happen
 * while in_nmi() is true during the NMI backtrace tests.
 */
static int random_kprobe_rc = -1;
static int random_hits;
static int random_in_nmi;

static int nmi_probe_random_pre(struct kprobe *p, struct pt_regs *regs)
{
	random_hits++;
	if (in_nmi())
		random_in_nmi++;
	return 0;
}

static struct kprobe random_kp = {
	.symbol_name = "add_interrupt_randomness",
	.pre_handler = nmi_probe_random_pre,
};

static void run_kprobe_test(void)
{
	kprobe_rc = register_kprobe(&vfs_kp);
	random_kprobe_rc = register_kprobe(&random_kp);
	run_kprobe_reject_tests();
}

/*
 * Arm a CPU with local IRQs disabled for hold_ms, then return to the
 * caller.  A subsequent panic makes smp_send_stop() time out on the normal
 * stop IPI and fall back to IPI_CPU_STOP_NMI.
 */
static struct holder_ctx stop_ctx;

static void arm_nmi_stop(void)
{
	struct task_struct *t;
	int target, i, tries;
	bool pinned = false;

	target = raw_smp_processor_id() == 0 ? 1 : 0;
	if (!cpu_online(target))
		target = cpumask_first(cpu_online_mask);

	t = start_holder(&stop_ctx, target);
	if (IS_ERR(t))
		return;
	if (!wait_flag_sleep(&stop_ctx.ready, 1, 1000)) {
		kthread_stop(t);
		return;
	}

	for (tries = 0; tries < 1000; tries++) {
		preempt_disable();
		if (smp_processor_id() != target) {
			pinned = true;
			break;
		}
		preempt_enable();
		msleep(1);
	}
	if (!pinned) {
		kthread_stop(t);
		return;
	}

	atomic_set(&stop_ctx.go, 1);
	for (i = 0; i < 20000 && !atomic_read(&stop_ctx.irqoff); i++)
		udelay(10);
	preempt_enable();

	pr_info("nmi_probe: armed stop test on CPU%d hold_ms=%u\n",
		target, hold_ms);
}

static void reset_stats(void)
{
	runs_done = 0;
	fast_runs = 0;
	last_delta_us = -1;
	min_delta_us = S64_MAX;
	max_delta_us = -1;
	bc_runs_done = 0;
	bc_fast_runs = 0;
	bc_last_cpus = 0;
	bc_last_delta_us = -1;
	bc_min_delta_us = S64_MAX;
	bc_max_delta_us = -1;
	allint_mask_delta_us = -1;
	allint_mask_ok = -1;
}

static void run_probe_series(void)
{
	unsigned int i, n = runs ? runs : 1;
	s64 delta;
	unsigned int cpus;

	for (i = 0; i < n; i++) {
		delta = run_single_test();
		if (delta < 0)
			continue;
		runs_done++;
		last_delta_us = delta;
		if (delta < FAST_THRESHOLD_US)
			fast_runs++;
		if (delta < min_delta_us)
			min_delta_us = delta;
		if (delta > max_delta_us)
			max_delta_us = delta;
	}

	delta = run_broadcast_test(&cpus);
	if (delta >= 0) {
		bc_runs_done++;
		bc_last_cpus = cpus;
		bc_last_delta_us = delta;
		if (delta < BC_THRESHOLD_US)
			bc_fast_runs++;
		if (delta < bc_min_delta_us)
			bc_min_delta_us = delta;
		if (delta > bc_max_delta_us)
			bc_max_delta_us = delta;
	}

	run_allint_mask_test();
}

static int nmi_probe_show(struct seq_file *m, void *v)
{
	bool use_nmi = caps_active || caps_pseudo;

	seq_printf(m, "supports=%d\n", caps_supports);
	seq_printf(m, "active=%d\n", caps_active);
	seq_printf(m, "pseudo=%d\n", caps_pseudo);
	seq_printf(m, "runs_requested=%u\n", runs ? runs : 1);
	seq_printf(m, "runs_done=%u\n", runs_done);
	seq_printf(m, "fast_runs=%u\n", fast_runs);
	seq_printf(m, "slow_runs=%u\n", runs_done - fast_runs);
	seq_printf(m, "last_delta_us=%lld\n", last_delta_us);
	seq_printf(m, "min_delta_us=%lld\n",
		   min_delta_us == S64_MAX ? -1 : min_delta_us);
	seq_printf(m, "max_delta_us=%lld\n", max_delta_us);
	seq_printf(m, "bc_runs=%u\n", bc_runs_done);
	seq_printf(m, "bc_fast_runs=%u\n", bc_fast_runs);
	seq_printf(m, "bc_cpus=%u\n", bc_last_cpus);
	seq_printf(m, "bc_last_delta_us=%lld\n", bc_last_delta_us);
	seq_printf(m, "bc_min_delta_us=%lld\n",
		   bc_min_delta_us == S64_MAX ? -1 : bc_min_delta_us);
	seq_printf(m, "bc_max_delta_us=%lld\n", bc_max_delta_us);
	seq_printf(m, "ran=%d\n", runs_done > 0);
	/* fast is only true if every single-target run was delivered as NMI */
	seq_printf(m, "fast=%d\n",
		   (runs_done && fast_runs == runs_done) ? 1 : 0);
	seq_printf(m, "bc_fast=%d\n",
		   (bc_runs_done && bc_fast_runs == bc_runs_done) ? 1 : 0);
	seq_printf(m, "irqflags_ok=%d\n", irqflags_ok);
	seq_printf(m, "allint_clear_on_disable=%d\n", allint_clear_on_disable);
	seq_printf(m, "allint_reported=%d\n", allint_reported);
	seq_printf(m, "pmr_irq_disable=%d\n", pmr_irq_disable);
	seq_printf(m, "kprobe_rc=%d\n", kprobe_rc);
	seq_printf(m, "kprobe_hits=%d\n", kprobe_hits);
	seq_printf(m, "kprobe_msr_reject_rc=%d\n", kprobe_msr_reject_rc);
	seq_printf(m, "kprobe_mrs_reject_rc=%d\n", kprobe_mrs_reject_rc);
	seq_printf(m, "random_kprobe_rc=%d\n", random_kprobe_rc);
	seq_printf(m, "random_hits=%d\n", random_hits);
	seq_printf(m, "random_in_nmi=%d\n", random_in_nmi);
	seq_printf(m, "allint_mask_delta_us=%lld\n", allint_mask_delta_us);
	seq_printf(m, "allint_mask_ok=%d\n", allint_mask_ok);
	(void)use_nmi;

	return 0;
}

static int nmi_probe_open(struct inode *inode, struct file *file)
{
	return single_open(file, nmi_probe_show, NULL);
}

static ssize_t nmi_probe_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char buf[8];

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;

	if (buf[0] == '1' || buf[0] == 'r' || buf[0] == 'R') {
		reset_stats();
		run_probe_series();
		pr_info("nmi_probe: rerun single=%u/%u broadcast=%u/%u\n",
			fast_runs, runs_done, bc_fast_runs, bc_runs_done);
	} else if (buf[0] == 's' || buf[0] == 'S') {
		arm_nmi_stop();
	}
	return count;
}

static const struct proc_ops nmi_probe_ops = {
	.proc_open	= nmi_probe_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= nmi_probe_write,
};

static int __init nmi_probe_init(void)
{
	caps_supports = system_supports_nmi();
	caps_active = system_uses_nmi();
	caps_pseudo = system_uses_irq_prio_masking();

	reset_stats();
	run_kprobe_test();
	run_probe_series();
	run_irqflags_test();

	if (!proc_create("nmi_probe", 0644, NULL, &nmi_probe_ops))
		return -ENOMEM;

	pr_info("nmi_probe: supports=%d active=%d pseudo=%d single=%u/%u bc=%u/%u irqflags=%d kprobe=%d/%d\n",
		caps_supports, caps_active, caps_pseudo,
		fast_runs, runs_done, bc_fast_runs, bc_runs_done,
		irqflags_ok, kprobe_rc, kprobe_hits);
	return 0;
}

static void __exit nmi_probe_exit(void)
{
	if (kprobe_rc == 0)
		unregister_kprobe(&vfs_kp);
	if (random_kprobe_rc == 0)
		unregister_kprobe(&random_kp);
	remove_proc_entry("nmi_probe", NULL);
}

module_init(nmi_probe_init);
module_exit(nmi_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("arm64 FEAT_NMI/pseudo-NMI capability and delivery probe");
