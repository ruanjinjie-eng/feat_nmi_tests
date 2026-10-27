// SPDX-License-Identifier: GPL-2.0
/*
 * Guest init for the arm64 FEAT_NMI test suite.
 *
 * Evidence collected:
 *   1. capability/delivery probe from /proc/nmi_probe (kernel module):
 *      architected vs pseudo vs no NMI, and whether the IPI backtrace is
 *      really delivered while local IRQs are disabled.
 *   2. kernel log: GIC NMI capability, pseudo-NMI setup, pseudo-vs-arch
 *      precedence, PMU NMI registration ("using NMIs").
 *   3. irq debugfs: which IRQs have IRQS_NMI (IPIs, arm-pmu).
 *
 * Optional tests selected with nmi_extra=:
 *   sysrq       - sysrq-l cross-CPU backtrace
 *   perf        - userspace perf cycles event while the PMU IRQ is an NMI
 *   hotplug     - offline/online CPU1, then rerun the IPI probe
 *   suspend     - RTC-wakeup suspend/resume, then rerun the IPI probe
 *   hardlockup  - perf/NMI hardlockup watchdog using lib/test_lockup.ko
 *   efi         - read an EFI variable through the runtime wrappers
 *
 * Expectations are passed as:
 *   nmi_expect=auto
 * or
 *   nmi_expect=active:1,fast:1,nmi_irqs:gt0,pmu_nmi:1,...
 * Values may be 0, 1, gt0, any.  In auto mode consistency is derived from
 * the detected capabilities, which makes the same image usable on real
 * hardware.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DMESG_SZ	(2 * 1024 * 1024)
#define MODULE_PATH	"/nmi_probe.ko"
#define TEST_LOCKUP_PATH "/test_lockup.ko"
#define PROC_PATH	"/proc/nmi_probe"

static char dmesg_buf[DMESG_SZ];
static int nr_fail;

struct probe {
	int supports, active, pseudo;
	unsigned long runs_req, runs_done, fast_runs, slow_runs;
	long long last_us, min_us, max_us;
	int ran, fast;
	unsigned long bc_runs, bc_fast_runs, bc_cpus;
	long long bc_last_us, bc_min_us, bc_max_us;
	int bc_fast;
	int irqflags_ok;
	int allint_clear_on_disable, allint_reported, pmr_irq_disable;
	int kprobe_rc, kprobe_hits, kprobe_msr_reject_rc, kprobe_mrs_reject_rc;
	int random_kprobe_rc, random_hits, random_in_nmi;
	long long allint_mask_delta_us;
	int allint_mask_ok;
};

struct nmi_scan {
	int total, ipi, ppi, other;
	char detail[768];
};

struct expect {
	char key[40];
	char val[40];
};

#define MAX_EXP 32
static struct expect exps[MAX_EXP];
static int nr_exp;
static int auto_mode;

static int need_sysrq, need_perf, need_hotplug, need_suspend;
static int need_hardlockup, need_efi, need_stress, need_crash;
static unsigned long nmi_runs = 1;
static unsigned long nmi_bc_max = 4;
static char nmi_shutdown[16] = "poweroff";

static void out(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void report(const char *fmt, ...)
{
	va_list ap;

	out("NMI_TEST: ");
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	out("\n");
	fflush(stdout);
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	out("NMI_TEST: FAIL: ");
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	out("\n");
	fflush(stdout);
	nr_fail++;
}

static void mkdir_p(const char *path)
{
	mkdir(path, 0755);
}

static void setup_fs(void)
{
	mkdir_p("/proc");
	mkdir_p("/sys");
	mkdir_p("/dev");
	mount("proc", "/proc", "proc", 0, "");
	mount("sysfs", "/sys", "sysfs", 0, "");
	mount("devtmpfs", "/dev", "devtmpfs", 0, "");

	/* From here on stdout goes to the serial console. */
	int c = open("/dev/console", O_RDWR);
	if (c >= 0) {
		dup2(c, 0);
		dup2(c, 1);
		dup2(c, 2);
		if (c > 2)
			close(c);
	}

	mkdir_p("/sys/kernel/debug");
	mount("debugfs", "/sys/kernel/debug", "debugfs", 0, "");
}

static int read_file(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY);
	ssize_t n, off = 0;

	if (fd < 0)
		return -1;
	while (off < (ssize_t)sz - 1) {
		n = read(fd, buf + off, sz - 1 - off);
		if (n < 0) {
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		off += n;
	}
	close(fd);
	buf[off] = 0;
	return off;
}

static int write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	size_t len = strlen(val);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, val, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

static int read_int_file(const char *path)
{
	char buf[64];
	int n;

	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;
	n = atoi(buf);
	return n;
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static int read_dmesg(void)
{
	long n;

	n = syscall(SYS_syslog, 3 /* SYSLOG_ACTION_READ_ALL */,
		    dmesg_buf, sizeof(dmesg_buf) - 1);
	if (n < 0)
		return -1;
	dmesg_buf[n] = 0;
	return n;
}

static int load_module(const char *path, const char *params)
{
	char *buf;
	struct stat st;
	int fd, ret;

	if (stat(path, &st) < 0)
		return -ENOENT;
	buf = malloc(st.st_size);
	if (!buf)
		return -ENOMEM;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		free(buf);
		return -errno;
	}
	if (read(fd, buf, st.st_size) != st.st_size) {
		close(fd);
		free(buf);
		return -EIO;
	}
	close(fd);
	ret = syscall(SYS_init_module, buf, (unsigned long)st.st_size,
		      params ? params : "");
	free(buf);
	return ret ? -errno : 0;
}

/*
 * Copy the value of key= from the kernel command line without modifying it
 * (there may be more parameters after this one).
 */
static int cmdline_get(const char *cmdline, const char *key, char *out,
		       size_t out_sz)
{
	const char *p = strstr(cmdline, key);
	size_t len;

	if (!p || !out_sz)
		return 0;
	p += strlen(key);
	len = strcspn(p, " \n\r");
	if (len >= out_sz)
		len = out_sz - 1;
	memcpy(out, p, len);
	out[len] = 0;
	return 1;
}

static void parse_expect(char *text)
{
	char *save = NULL, *tok, *colon;

	for (tok = strtok_r(text, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		colon = strchr(tok, ':');
		if (!colon || nr_exp >= MAX_EXP)
			continue;
		*colon = 0;
		snprintf(exps[nr_exp].key, sizeof(exps[nr_exp].key), "%s", tok);
		snprintf(exps[nr_exp].val, sizeof(exps[nr_exp].val), "%s",
			 colon + 1);
		nr_exp++;
	}
}

static const char *exp_get(const char *key)
{
	int i;

	for (i = 0; i < nr_exp; i++)
		if (!strcmp(exps[i].key, key))
			return exps[i].val;
	return NULL;
}

static void parse_bool_list(char *text, int *sysrq, int *perf, int *hotplug,
			    int *suspend, int *hardlockup, int *efi,
			    int *stress, int *crash)
{
	char *save = NULL, *tok;

	for (tok = strtok_r(text, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		if (!strcmp(tok, "sysrq"))
			*sysrq = 1;
		else if (!strcmp(tok, "perf"))
			*perf = 1;
		else if (!strcmp(tok, "hotplug"))
			*hotplug = 1;
		else if (!strcmp(tok, "suspend"))
			*suspend = 1;
		else if (!strcmp(tok, "hardlockup"))
			*hardlockup = 1;
		else if (!strcmp(tok, "efi"))
			*efi = 1;
		else if (!strcmp(tok, "stress"))
			*stress = 1;
		else if (!strcmp(tok, "crash"))
			*crash = 1;
		else if (!strcmp(tok, "all")) {
			*sysrq = *perf = *hotplug = *suspend = 1;
			*hardlockup = *efi = *stress = 1;
		}
	}
}

static int parse_probe(struct probe *p)
{
	char buf[4096];
	char *save = NULL, *line;

	memset(p, 0, sizeof(*p));
	p->last_us = p->min_us = p->max_us = -1;
	p->allint_clear_on_disable = -1;
	p->allint_reported = -1;
	p->pmr_irq_disable = -1;
	p->kprobe_rc = -1;
	p->kprobe_msr_reject_rc = -1000;
	p->kprobe_mrs_reject_rc = -1000;
	p->random_kprobe_rc = -1;
	p->allint_mask_delta_us = -1;
	p->allint_mask_ok = -1;

	if (read_file(PROC_PATH, buf, sizeof(buf)) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');
		const char *v;

		if (!eq)
			continue;
		*eq = 0;
		v = eq + 1;
		if (!strcmp(line, "supports"))
			p->supports = atoi(v);
		else if (!strcmp(line, "active"))
			p->active = atoi(v);
		else if (!strcmp(line, "pseudo"))
			p->pseudo = atoi(v);
		else if (!strcmp(line, "runs_requested"))
			p->runs_req = strtoul(v, NULL, 0);
		else if (!strcmp(line, "runs_done"))
			p->runs_done = strtoul(v, NULL, 0);
		else if (!strcmp(line, "fast_runs"))
			p->fast_runs = strtoul(v, NULL, 0);
		else if (!strcmp(line, "slow_runs"))
			p->slow_runs = strtoul(v, NULL, 0);
		else if (!strcmp(line, "last_delta_us"))
			p->last_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "min_delta_us"))
			p->min_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "max_delta_us"))
			p->max_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "ran"))
			p->ran = atoi(v);
		else if (!strcmp(line, "fast"))
			p->fast = atoi(v);
		else if (!strcmp(line, "bc_runs"))
			p->bc_runs = strtoul(v, NULL, 0);
		else if (!strcmp(line, "bc_fast_runs"))
			p->bc_fast_runs = strtoul(v, NULL, 0);
		else if (!strcmp(line, "bc_cpus"))
			p->bc_cpus = strtoul(v, NULL, 0);
		else if (!strcmp(line, "bc_last_delta_us"))
			p->bc_last_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "bc_min_delta_us"))
			p->bc_min_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "bc_max_delta_us"))
			p->bc_max_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "bc_fast"))
			p->bc_fast = atoi(v);
		else if (!strcmp(line, "irqflags_ok"))
			p->irqflags_ok = atoi(v);
		else if (!strcmp(line, "allint_clear_on_disable"))
			p->allint_clear_on_disable = atoi(v);
		else if (!strcmp(line, "allint_reported"))
			p->allint_reported = atoi(v);
		else if (!strcmp(line, "pmr_irq_disable"))
			p->pmr_irq_disable = atoi(v);
		else if (!strcmp(line, "kprobe_rc"))
			p->kprobe_rc = atoi(v);
		else if (!strcmp(line, "kprobe_hits"))
			p->kprobe_hits = atoi(v);
		else if (!strcmp(line, "kprobe_msr_reject_rc"))
			p->kprobe_msr_reject_rc = atoi(v);
		else if (!strcmp(line, "kprobe_mrs_reject_rc"))
			p->kprobe_mrs_reject_rc = atoi(v);
		else if (!strcmp(line, "random_kprobe_rc"))
			p->random_kprobe_rc = atoi(v);
		else if (!strcmp(line, "random_hits"))
			p->random_hits = atoi(v);
		else if (!strcmp(line, "random_in_nmi"))
			p->random_in_nmi = atoi(v);
		else if (!strcmp(line, "allint_mask_delta_us"))
			p->allint_mask_delta_us = strtoll(v, NULL, 0);
		else if (!strcmp(line, "allint_mask_ok"))
			p->allint_mask_ok = atoi(v);
	}
	return 0;
}

/*
 * Walk irq debugfs and collect descriptors with IRQS_NMI.  The action name
 * tells us which in-kernel users got NMI delivery (IPI and arm-pmu here).
 */
static void scan_nmi_irqs(struct nmi_scan *s)
{
	const char *dirpath = "/sys/kernel/debug/irq/irqs";
	DIR *dir;
	struct dirent *de;
	char path[512], buf[8192];

	memset(s, 0, sizeof(*s));

	dir = opendir(dirpath);
	if (!dir)
		return;
	while ((de = readdir(dir))) {
		char name[64] = "?";
		unsigned long hw = 0;
		char *p;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
		if (read_file(path, buf, sizeof(buf)) < 0)
			continue;
		if (!strstr(buf, "IRQS_NMI"))
			continue;

		s->total++;

		p = strstr(buf, "hwirq:");
		if (p)
			hw = strtoul(p + 6, NULL, 16);

		p = strstr(buf, "device:");
		if (p) {
			p += 7;
			while (*p == ' ' || *p == '\t')
				p++;
			snprintf(name, sizeof(name), "%.*s",
				 (int)strcspn(p, "\n"), p);
		}

		/*
		 * The action name is not available for per-CPU IRQs
		 * (debugfs prints "(null)"), so classify by hwirq:
		 *   SGI (0-15)  -> IPI
		 *   PPI (16-31) -> typically the PMU
		 *   LPIs        -> GICv5 IPIs (reported as "other")
		 */
		if (hw < 16)
			s->ipi++;
		else if (hw < 32)
			s->ppi++;
		else
			s->other++;

		strncat(s->detail, name, sizeof(s->detail) - strlen(s->detail) - 1);
		{
			char tmp[48];

			snprintf(tmp, sizeof(tmp), "@%lu,", hw);
			strncat(s->detail, tmp,
				sizeof(s->detail) - strlen(s->detail) - 1);
		}
	}
	closedir(dir);
}

/* Sum the per-CPU counts on the /proc/interrupts line for an action. */
static long long irq_count(const char *action)
{
	char buf[65536];
	char *line, *save = NULL;
	long long sum = 0;

	if (read_file("/proc/interrupts", buf, sizeof(buf)) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *p;
		int seen_colon = 0;

		if (!strstr(line, action))
			continue;
		for (p = line; *p; p++) {
			if (*p == ':') {
				seen_colon = 1;
				continue;
			}
			if (!seen_colon)
				continue;
			if (*p >= '0' && *p <= '9')
				sum += strtoll(p, &p, 10);
			else if (*p != ' ')
				break;
		}
		return sum;
	}
	return -1;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/*
 * Userspace perf sampling.  The cycles counter overflows periodically and
 * the PMU IRQ is delivered as an NMI when the kernel/irqchip support it.
 */
static int perf_test(int *irq_delta, unsigned long long *count)
{
	struct perf_event_attr attr;
	long long before, after, end;
	void *ring;
	int fd, ret;

	before = irq_count("arm-pmu");

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_HARDWARE;
	attr.size = sizeof(attr);
	attr.config = PERF_COUNT_HW_CPU_CYCLES;
	attr.sample_period = 2000000;
	attr.sample_type = PERF_SAMPLE_IP;
	attr.disabled = 1;
	attr.exclude_hv = 1;
	attr.wakeup_events = 1;

	fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
	if (fd < 0)
		return -errno;

	ring = mmap(NULL, (1 + 8) * 4096, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, 0);

	ioctl(fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

	end = now_ms() + 250;
	while (now_ms() < end) {
		volatile unsigned long x;

		for (x = 0; x < 100000; x++)
			;
	}

	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
	*count = 0;
	ret = read(fd, count, sizeof(*count));

	if (ring != MAP_FAILED)
		munmap(ring, (1 + 8) * 4096);
	close(fd);

	after = irq_count("arm-pmu");
	*irq_delta = (before >= 0 && after >= 0) ? (int)(after - before) : -1;

	return ret == (int)sizeof(*count) ? 0 : -EIO;
}

static int count_substr(const char *hay, const char *needle)
{
	const char *p = hay;
	int n = 0;

	while ((p = strstr(p, needle))) {
		n++;
		p += strlen(needle);
	}
	return n;
}

static int parse_cpu_list(const char *s, int *cpus, int max)
{
	int n = 0;

	while (*s && n < max) {
		char *end;
		long a, b;

		a = strtol(s, &end, 10);
		if (end == s)
			break;
		s = end;
		b = a;
		if (*s == '-') {
			s++;
			b = strtol(s, &end, 10);
			if (end == s)
				break;
			s = end;
		}
		for (; a <= b && n < max; a++)
			cpus[n++] = (int)a;
		if (*s == ',')
			s++;
	}
	return n;
}

/*
 * Concurrency / perf stress: run system-wide perf sampling events on every
 * online CPU while another process floods sysrq-l (cross-CPU backtrace NMI).
 */
static int stress_test(int *pmu_delta, int *bt_count)
{
#define MAX_STRESS_CPUS 256
	char online[256];
	int cpus[MAX_STRESS_CPUS], fds[MAX_STRESS_CPUS];
	void *rings[MAX_STRESS_CPUS];
	int nr, i, nfd = 0, bt_before;
	struct perf_event_attr attr;
	long long before, after, end;
	unsigned long long sum = 0;
	pid_t child;

	*pmu_delta = -1;
	*bt_count = 0;

	if (read_file("/sys/devices/system/cpu/online", online,
		      sizeof(online)) < 0)
		return 0;
	nr = parse_cpu_list(online, cpus, MAX_STRESS_CPUS);
	if (!nr)
		return 0;

	if (read_dmesg() >= 0)
		bt_before = count_substr(dmesg_buf, "NMI backtrace for cpu");
	else
		bt_before = 0;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_HARDWARE;
	attr.size = sizeof(attr);
	attr.config = PERF_COUNT_HW_CPU_CYCLES;
	attr.sample_period = 200000;
	attr.sample_type = PERF_SAMPLE_IP;
	attr.disabled = 1;
	attr.exclude_hv = 1;
	attr.wakeup_events = 1;

	before = irq_count("arm-pmu");

	for (i = 0; i < nr; i++) {
		int fd = syscall(SYS_perf_event_open, &attr, -1, cpus[i], -1, 0);

		if (fd < 0)
			continue;
		rings[nfd] = mmap(NULL, (1 + 2) * 4096,
				  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		fds[nfd++] = fd;
		ioctl(fd, PERF_EVENT_IOC_RESET, 0);
		ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
	}

	child = fork();
	if (child == 0) {
		for (i = 0; i < 4; i++) {
			write_file("/proc/sysrq-trigger", "l");
			usleep(200000);
		}
		_exit(0);
	}

	end = now_ms() + 2000;
	while (now_ms() < end)
		usleep(20000);

	if (child > 0)
		waitpid(child, NULL, 0);

	for (i = 0; i < nfd; i++) {
		unsigned long long cnt = 0;

		ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);
		if (read(fds[i], &cnt, sizeof(cnt)) == (int)sizeof(cnt))
			sum += cnt;
		if (rings[i] != MAP_FAILED)
			munmap(rings[i], (1 + 2) * 4096);
		close(fds[i]);
	}

	after = irq_count("arm-pmu");
	*pmu_delta = (before >= 0 && after >= 0) ? (int)(after - before) : -1;

	if (read_dmesg() >= 0)
		*bt_count = count_substr(dmesg_buf, "NMI backtrace for cpu") -
			    bt_before;

	return sum > 0;
#undef MAX_STRESS_CPUS
}

static int sysrq_test(void)
{
	if (write_file("/proc/sysrq-trigger", "l") < 0)
		return -1;
	usleep(500000);
	if (read_dmesg() < 0)
		return -1;
	return strstr(dmesg_buf, "NMI backtrace for cpu") != NULL;
}

static int wait_cpu_online(const char *path, int want, int timeout_ms)
{
	long long end = now_ms() + timeout_ms;

	while (now_ms() < end) {
		if (read_int_file(path) == want)
			return 1;
		usleep(50000);
	}
	return 0;
}

static int hotplug_test(int *retest_fast)
{
	const char *on = "/sys/devices/system/cpu/cpu1/online";
	struct probe p;

	if (!file_exists(on))
		return -1;
	if (write_file(on, "0") < 0)
		return -1;
	if (!wait_cpu_online(on, 0, 5000))
		return 0;
	if (write_file(on, "1") < 0)
		return -1;
	if (!wait_cpu_online(on, 1, 5000))
		return 0;

	if (write_file(PROC_PATH, "1") < 0)
		return -1;
	if (parse_probe(&p) < 0)
		return -1;
	*retest_fast = p.fast;
	return 1;
}

static int suspend_test(int *retest_fast)
{
	char states[128];
	char alarm[32];
	struct probe p;
	int fd;
	time_t now = time(NULL);

	if (read_file("/sys/power/state", states, sizeof(states)) < 0 ||
	    !strstr(states, "mem"))
		return -1;

	fd = open("/sys/class/rtc/rtc0/wakealarm", O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, "0", 1) < 0) {
		close(fd);
		return -1;
	}
	snprintf(alarm, sizeof(alarm), "%lld", (long long)now + 5);
	if (write(fd, alarm, strlen(alarm)) < 0) {
		close(fd);
		return -1;
	}
	close(fd);

	report("suspend_begin=1");
	if (write_file("/sys/power/state", "mem") < 0)
		return -1;
	report("suspend_resumed=1");

	if (write_file(PROC_PATH, "1") < 0)
		return -1;
	if (parse_probe(&p) < 0)
		return -1;
	*retest_fast = p.fast;
	return 1;
}

static int hardlockup_test(void)
{
	int before, after, rc;

	if (!file_exists("/proc/sys/kernel/nmi_watchdog"))
		return -1;

	write_file("/proc/sys/kernel/hardlockup_panic", "0");
	/* Avoid a soft-lockup report; /proc/sys/kernel/watchdog is the master
	 * switch and would disable the hardlockup detector too. */
	write_file("/proc/sys/kernel/soft_watchdog", "0");
	/*
	 * The arm64 perf watchdog period is max_cpu_freq * watchdog_thresh
	 * cycles (5 GHz if the platform has no cpufreq driver).  Two
	 * overflows are needed to declare a hard lockup, so use the minimum
	 * threshold and a long enough spin.
	 */
	write_file("/proc/sys/kernel/watchdog_thresh", "1");
	write_file("/proc/sys/kernel/hardlockup_all_cpu_backtrace", "1");
	/* Force a reconfiguration of the hardlockup detector. */
	write_file("/proc/sys/kernel/nmi_watchdog", "0");
	if (write_file("/proc/sys/kernel/nmi_watchdog", "1") < 0)
		return -1;
	usleep(500000);

	before = read_int_file("/sys/kernel/hardlockup_count");
	{
		long long pmu_before = irq_count("arm-pmu");

		report("hardlockup_setup thresh=%d nmi_wd=%d hl_count=%d pmu_irqs=%lld",
		       read_int_file("/proc/sys/kernel/watchdog_thresh"),
		       read_int_file("/proc/sys/kernel/nmi_watchdog"),
		       before, pmu_before);
	}

	/*
	 * test_lockup always returns -EAGAIN after doing its work, so any
	 * return value is fine; success is measured through the watchdog.
	 */
	rc = load_module(TEST_LOCKUP_PATH,
			 "time_secs=14 disable_irq=1 iterations=1");
	write_file("/proc/sys/kernel/nmi_watchdog", "0");
	write_file("/proc/sys/kernel/soft_watchdog", "1");
	if (rc == -ENOENT)
		return -2;

	after = read_int_file("/sys/kernel/hardlockup_count");
	report("hardlockup_after hl_count=%d pmu_irqs=%lld",
	       after, irq_count("arm-pmu"));
	if (read_dmesg() < 0)
		return -1;

	if (before >= 0 && after > before)
		return 1;
	return strstr(dmesg_buf, "Watchdog detected hard LOCKUP") != NULL;
}

static int efi_test(void)
{
	const char *dirpath = "/sys/firmware/efi/efivars";
	DIR *dir;
	struct dirent *de;
	char path[512], buf[256];
	int ok = 0;

	/* efivarfs is not auto-mounted; do it ourselves. */
	mkdir_p("/sys/firmware/efi/efivars");
	mount("efivarfs", "/sys/firmware/efi/efivars", "efivarfs", 0, "");

	dir = opendir(dirpath);
	if (!dir)
		return -1;
	while ((de = readdir(dir))) {
		int i;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
		if (read_file(path, buf, sizeof(buf)) < 0)
			continue;
		/* Exercise the runtime wrappers repeatedly (ALLINT checks). */
		for (i = 0; i < 64; i++)
			read_file(path, buf, sizeof(buf));
		ok = 1;
		break;
	}
	closedir(dir);
	return ok;
}

static int expect_match(const char *key, int got, int is_count)
{
	const char *exp = exp_get(key);

	if (!exp)
		return 1;
	if (!strcmp(exp, "any"))
		return 1;
	if (!strcmp(exp, "gt0"))
		return got > 0;
	if (is_count)
		return got == atoi(exp);
	return atoi(exp) == got;
}

static void check(const char *key, int got, int is_count)
{
	const char *exp = exp_get(key);

	if (!exp)
		return;
	report("%s=%d expected=%s", key, got, exp);
	if (!expect_match(key, got, is_count))
		fail("%s: expected %s, got %d", key, exp, got);
}

static void check_dmesg_health(void)
{
	static const char *bad[] = {
		"WARNING: CPU:",
		"kernel BUG at",
		"Unable to handle kernel",
		"Internal error",
		"Oops:",
		"corrupted by EFI call",
	};
	unsigned int i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (strstr(dmesg_buf, bad[i])) {
			report("dmesg_has=%s", bad[i]);
			fail("kernel log contains '%s'", bad[i]);
		}
	}
}

int main(void)
{
	char cmdline[4096], arg[512];
	struct probe pr;
	struct nmi_scan scan;
	int gic_nmi, pseudo_log, arch_off_log, pmu_log;
	int use_nmi, nmi_effective, chip_nmi, external_nmi, pmu_present;
	int retest_fast = -1;
	int hotplug_ok = -1, suspend_ok = -1, hardlockup = -1;
	int perf_ok = -1, sysrq_bt = -1, efi_ok = -1;
	int perf_delta = -1;
	int stress_ok = -1, stress_pmu_delta = -1, stress_bt = -1;
	unsigned long long perf_count = 0;
	char mod_params[96];

	setup_fs();

	if (read_dmesg() < 0)
		report("dmesg_unavailable=1");

	if (read_file("/proc/cmdline", cmdline, sizeof(cmdline)) < 0)
		cmdline[0] = 0;

	if (cmdline_get(cmdline, "nmi_expect=", arg, sizeof(arg))) {
		if (!strcmp(arg, "auto"))
			auto_mode = 1;
		else
			parse_expect(arg);
	}

	if (cmdline_get(cmdline, "nmi_extra=", arg, sizeof(arg)))
		parse_bool_list(arg, &need_sysrq, &need_perf, &need_hotplug,
				&need_suspend, &need_hardlockup, &need_efi,
				&need_stress, &need_crash);

	if (cmdline_get(cmdline, "nmi_runs=", arg, sizeof(arg))) {
		nmi_runs = strtoul(arg, NULL, 0);
		if (!nmi_runs)
			nmi_runs = 1;
	}

	if (cmdline_get(cmdline, "nmi_bc_max=", arg, sizeof(arg))) {
		nmi_bc_max = strtoul(arg, NULL, 0);
		if (!nmi_bc_max)
			nmi_bc_max = 1;
	}

	if (cmdline_get(cmdline, "nmi_shutdown=", arg, sizeof(arg)))
		snprintf(nmi_shutdown, sizeof(nmi_shutdown), "%.*s",
			 (int)sizeof(nmi_shutdown) - 1, arg);

	/* Boot-time evidence. */
	gic_nmi = strstr(dmesg_buf, "Non-maskable interrupt property is supported") != NULL;
	pseudo_log = strstr(dmesg_buf, "Pseudo-NMIs enabled using") != NULL;
	arch_off_log = strstr(dmesg_buf, "Pseudo NMI enabled, not using architected NMI") != NULL;
	pmu_log = strstr(dmesg_buf, "using NMIs") != NULL;

	snprintf(mod_params, sizeof(mod_params), "runs=%lu bc_max=%lu",
		 nmi_runs, nmi_bc_max);
	if (load_module(MODULE_PATH, mod_params) < 0)
		fail("could not load %s", MODULE_PATH);

	if (parse_probe(&pr) < 0) {
		report("proc_missing=1");
		memset(&pr, 0, sizeof(pr));
		pr.supports = pr.active = pr.pseudo = -1;
	}

	scan_nmi_irqs(&scan);
	if (scan.total == 0 && !file_exists("/sys/kernel/debug/irq/irqs"))
		report("debugfs_irqs_unavailable=1");

	use_nmi = (pr.active || pr.pseudo) ? 1 : 0;
	/*
	 * use_nmi         kernel enabled an NMI mode (PE supports it)
	 * chip_nmi        the irqchip registered NMI IRQs (IRQS_NMI)
	 * external_nmi    NMI delivery works without IRQS_NMI (e.g. SDEI)
	 * nmi_effective   NMIs are actually delivered
	 *
	 * With PE FEAT_NMI but no irqchip support the kernel deliberately
	 * falls back to regular IRQs (use_nmi=1, nmi_effective=0).
	 */
	chip_nmi = use_nmi && scan.total > 0;
	external_nmi = use_nmi && !chip_nmi && pr.runs_done && pr.fast;
	nmi_effective = chip_nmi || external_nmi;
	pmu_present = pmu_log || irq_count("arm-pmu") >= 0;

	report("supports=%d active=%d pseudo=%d runs=%lu fast_runs=%lu slow_runs=%lu last_delta_us=%lld min_delta_us=%lld max_delta_us=%lld fast=%d",
	       pr.supports, pr.active, pr.pseudo, pr.runs_done, pr.fast_runs,
	       pr.slow_runs, pr.last_us, pr.min_us, pr.max_us, pr.fast);
	report("gic_nmi=%d gic_nmi_log=%d pseudo_log=%d arch_off_log=%d pmu_log=%d pmu_present=%d",
	       gic_nmi, gic_nmi, pseudo_log, arch_off_log, pmu_log, pmu_present);
	report("nmi_irqs=%d ipi_nmi=%d ppi_nmi=%d other_nmi=%d irq_list=%s",
	       scan.total, scan.ipi, scan.ppi, scan.other,
	       scan.detail[0] ? scan.detail : "-");
	report("bc_runs=%lu bc_fast_runs=%lu bc_cpus=%lu bc_last_delta_us=%lld bc_min_delta_us=%lld bc_max_delta_us=%lld bc_fast=%d",
	       pr.bc_runs, pr.bc_fast_runs, pr.bc_cpus, pr.bc_last_us,
	       pr.bc_min_us, pr.bc_max_us, pr.bc_fast);
	report("irqflags_ok=%d allint_clear_on_disable=%d allint_reported=%d pmr_irq_disable=%d",
	       pr.irqflags_ok, pr.allint_clear_on_disable,
	       pr.allint_reported, pr.pmr_irq_disable);
	report("kprobe_rc=%d kprobe_hits=%d kprobe_msr_reject_rc=%d kprobe_mrs_reject_rc=%d",
	       pr.kprobe_rc, pr.kprobe_hits,
	       pr.kprobe_msr_reject_rc, pr.kprobe_mrs_reject_rc);
	report("random_kprobe_rc=%d random_hits=%d random_in_nmi=%d",
	       pr.random_kprobe_rc, pr.random_hits, pr.random_in_nmi);
	report("allint_mask_delta_us=%lld allint_mask_ok=%d",
	       pr.allint_mask_delta_us, pr.allint_mask_ok);

	/* Internal consistency: these hold regardless of the expectation. */
	if (pr.active && pr.pseudo)
		fail("both architected and pseudo NMI are active");
	if (use_nmi && !chip_nmi) {
		if (external_nmi)
			report("external_nmi=1 (NMI delivered without IRQS_NMI, e.g. SDEI)");
		else
			report("nmi_fallback=1 (PE NMI without irqchip NMI support)");
	}
	if (!use_nmi && pr.runs_done && pr.fast)
		fail("fast=1 (IPI delivered with IRQs off) but no NMI is in use");
	if (chip_nmi && pr.runs_done && !pr.fast)
		fail("irqchip NMIs are registered but the IPI was not delivered with IRQs off");
	if (!use_nmi && scan.total)
		fail("IRQS_NMI present (%d) but no NMI is in use", scan.total);
	if (pmu_present && pmu_log != chip_nmi)
		fail("PMU NMI registration (%d) inconsistent with irqchip NMI (%d)",
		     pmu_log, chip_nmi);
	if (!nmi_effective && pr.bc_runs && pr.bc_fast)
		fail("broadcast IPI was fast but no NMI is effective");
	if (nmi_effective && pr.bc_runs && !pr.bc_fast)
		fail("NMI is effective but the broadcast IPI was not delivered with IRQs off");

	/* Exception-state / kprobe coverage. */
	if (!pr.irqflags_ok)
		fail("irqflags/exception-state round-trip test failed");
	if (pr.active) {
		if (pr.allint_clear_on_disable != 1)
			fail("local_irq_disable() masked NMIs (ALLINT set) with FEAT_NMI");
		if (pr.allint_reported != 1)
			fail("arch_irqs_disabled() ignored ALLINT");
		if (pr.allint_mask_ok != 1)
			fail("NMI was delivered while ALLINT was set (delta=%lld us)",
			     pr.allint_mask_delta_us);
	}
	if (pr.pseudo && pr.pmr_irq_disable != 1)
		fail("pseudo-NMI masking is not using PMR");
	if (pr.kprobe_rc != 0)
		fail("kprobe registration on vfs_read failed (%d)", pr.kprobe_rc);
	if (pr.kprobe_hits <= 0)
		fail("kprobe pre_handler never ran");
	if (pr.kprobe_msr_reject_rc == 0)
		fail("kprobe accepted an MSR ALLINT instruction");
	if (pr.kprobe_mrs_reject_rc == 0)
		fail("kprobe accepted an MRS ALLINT instruction");
	if (pr.random_kprobe_rc != 0)
		fail("kprobe on add_interrupt_randomness failed (%d)",
		     pr.random_kprobe_rc);
	if (pr.random_in_nmi != 0)
		fail("add_interrupt_randomness() called in NMI context (%d/%d)",
		     pr.random_in_nmi, pr.random_hits);

	/* Optional functional tests. */
	if (need_sysrq) {
		sysrq_bt = sysrq_test();
		report("sysrq_bt=%d", sysrq_bt);
	}

	if (need_perf) {
		if (perf_test(&perf_delta, &perf_count) == 0)
			perf_ok = perf_count > 0;
		else
			perf_ok = 0;
		report("perf_count=%llu perf_irq_delta=%d perf_ok=%d",
		       perf_count, perf_delta, perf_ok);
	}

	if (need_stress) {
		stress_ok = stress_test(&stress_pmu_delta, &stress_bt);
		report("stress_ok=%d stress_pmu_delta=%d stress_bt=%d",
		       stress_ok, stress_pmu_delta, stress_bt);
	}

	if (need_hotplug) {
		hotplug_ok = hotplug_test(&retest_fast);
		report("hotplug_ok=%d retest_fast=%d", hotplug_ok, retest_fast);
	}

	if (need_suspend) {
		suspend_ok = suspend_test(&retest_fast);
		report("suspend_ok=%d retest_fast=%d", suspend_ok, retest_fast);
	}

	if (need_efi) {
		efi_ok = efi_test();
		report("efi_ok=%d", efi_ok);
	}

	/* Run this last: it intentionally creates a hard lockup. */
	if (need_hardlockup) {
		hardlockup = hardlockup_test();
		report("hardlockup=%d", hardlockup);
	}

	/*
	 * Crash test: leave a CPU spinning with IRQs off, then panic so
	 * smp_send_stop() has to fall back to IPI_CPU_STOP_NMI.  This does
	 * not return; the runner inspects the panic log.
	 */
	if (need_crash) {
		write_file("/sys/module/nmi_probe/parameters/hold_ms", "6000");
		if (write_file(PROC_PATH, "s") < 0)
			report("crash_arm_failed=1");
		usleep(300000);
		report("crash_begin=1");
		fflush(stdout);
		write_file("/proc/sysrq-trigger", "c");
		for (;;)
			sleep(1);
	}

	/* Expectations. */
	if (auto_mode) {
		report("auto_mode=1 use_nmi=%d chip_nmi=%d external_nmi=%d nmi_effective=%d",
		       use_nmi, chip_nmi, external_nmi, nmi_effective);
		check("fast", pr.fast, 0);
		if (pr.bc_runs && pr.bc_fast != nmi_effective)
			fail("auto: bc_fast=%d but nmi_effective=%d",
			     pr.bc_fast, nmi_effective);
		if (pr.bc_runs && !pr.bc_cpus)
			fail("auto: broadcast ran on 0 CPUs");
	} else {
		check("supports", pr.supports, 0);
		check("active", pr.active, 0);
		check("pseudo", pr.pseudo, 0);
		check("gic_nmi", gic_nmi, 0);
		check("pseudo_log", pseudo_log, 0);
		check("arch_off_log", arch_off_log, 0);
		check("nmi_irqs", scan.total, 1);
		check("ipi_nmi", scan.ipi, 0);
		check("pmu_log", pmu_log, 0);
		check("fast", pr.fast, 0);
		check("bc_fast", pr.bc_fast, 0);
		check("bc_cpus", pr.bc_cpus, 1);
		check("sysrq_bt", sysrq_bt, 0);
		check("perf_ok", perf_ok, 0);
		check("stress_ok", stress_ok, 0);
		check("stress_pmu_delta", stress_pmu_delta, 0);
		check("stress_bt", stress_bt, 0);
		check("hotplug_ok", hotplug_ok, 0);
		check("retest_fast", retest_fast, 0);
		check("suspend_ok", suspend_ok, 0);
		check("hardlockup", hardlockup, 0);
		check("efi_ok", efi_ok, 0);
	}

	/* Pick up anything printed by the optional tests. */
	read_dmesg();
	check_dmesg_health();

	if (nr_fail)
		report("RESULT FAIL failures=%d", nr_fail);
	else
		report("RESULT PASS");

	report("shutdown=%s", nmi_shutdown);
	sync();
	sleep(1);
	if (!strcmp(nmi_shutdown, "reboot"))
		reboot(RB_AUTOBOOT);
	else if (!strcmp(nmi_shutdown, "halt"))
		reboot(RB_HALT_SYSTEM);
	else
		reboot(RB_POWER_OFF);
	sleep(2);
	return nr_fail ? 1 : 0;
}
