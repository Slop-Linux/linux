// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple T8132 CPU start and stop through m1n1's RVBAR mailbox
 * (enable-method "apple,rvbar").
 *
 * iBoot locks every core's RVBAR to m1n1's vectors. m1n1 keeps its vectors,
 * a reset dispatcher and a mailbox in a page it reserves for us: a core that
 * comes out of reset finds its {MPIDR | valid, entry} slot and jumps to the
 * entry at EL2 with the MMU off. With CTXR active, Linux replaces that
 * dispatcher with an in-window entry which drops to EL1. A first start goes through
 * m1n1's spin table (cpu-release-addr), where it is parked from boot; later
 * starts, after the OS powered it off, and the boot CPU's wake from S2R go
 * through the mailbox. Linux sets the mailbox magic when it maps it, so a
 * core that resets without a slot (powered off, woken early) waits in the
 * dispatcher's WFE instead of entering m1n1, whose memory Linux now owns.
 *
 * Core power follows macOS 27.0 (26A428). The "Core" platform function
 * (ADT function-enable_core, mask 1 << cpu-id) reaches
 * ApplePMGR::configMiscCores (kernelcache fffffe0009b947f4). On J713 the pmgr
 * "clusters" property is {6, 4} with no shifts, so the core bits are packed
 * per cluster with a stride of the largest cluster (initDriver
 * fffffe0009b7bf7c..bfa8): a core's bit is cluster * stride + core
 * (fffffe0009b94b88..bcc). Power on writes the packed bit to +0x4 and
 * 1 << core to +0x8 + 4 * cluster (fffffe0009b94bd0..c84); power off writes
 * the packed bit to +0x0 (fffffe0009b94c8c..ecc).
 *
 * A secondary going down for system sleep runs PE_cpu_machine_quiesce
 * (fffffe000c56df5c): ApplePMGR::disableCPUCore (fffffe0009b953ac, the stop
 * bit above), then ml_arm_sleep (fffffe000be07d68). ml_arm_sleep's sleep
 * token, hold and arm64_prepare_for_sleep(1) belong to the system sleep and
 * are the S2R driver's (apple_rvbar_set_sleep_die); this file provides the
 * stop, the park's restart and the EL2 sleep request
 * (apple_rvbar_system_sleep). ml_arm_sleep unlocks the core's CoreSight
 * debug block (LAR +0xfb0, EDPRCR +0x310) only when
 * cpu_data->coresight_base[ED] is mapped (fffffe000be07e78..e90), which XNU
 * does only when /chosen effective-production-status-ap is 0
 * (fffffe000be07fc4..803c). J713 reads 1: no CoreSight write. Writing it
 * anyway raised an L2C error on that LAR (L2C_ERR_ADR 0x211310fb0,
 * cpu9 going down in S2R, 2026-10-04).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include <linux/smp.h>
#include <linux/suspend.h>
#include <linux/types.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/kexec.h>
#include <asm/mmu_context.h>
#include <asm/smp_apple_rvbar.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

#define RVBAR_MAILBOX_MAGIC	0x4b414c3252564241ULL
#define RVBAR_MAILBOX_SLOTS	24
#define RVBAR_SLOT_VALID	BIT_ULL(63)

struct rvbar_slot {
	u64 mpidr;
	u64 entry;
};

struct rvbar_mailbox {
	u64 magic;
	u64 reserved;
	struct rvbar_slot slots[RVBAR_MAILBOX_SLOTS];
};

#define CPU_START_STOP		0x0
#define CPU_START_SYS		0x4
#define CPU_START_CORE(cl)	(0x8 + 4 * (cl))

/* cpu-impl-reg + 0x100: non-zero low byte while the core is powered (m1n1 smp_stop_cpu). */
#define CPU_IMPL_PWR		0x100
#define CPU_IMPL_PWR_ON		GENMASK(7, 0)

/* arm64_prepare_for_sleep's system registers. */
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)
#define SYS_APL_CORE_OFF_EL2	sys_reg(3, 1, 15, 7, 4)
#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
/* Fast IPI request registers (irq-apple-aic). */
#define SYS_APL_IPI_RR_LOCAL_EL1	sys_reg(3, 5, 15, 0, 0)
#define SYS_APL_IPI_RR_GLOBAL_EL1	sys_reg(3, 5, 15, 0, 1)
#define IPI_RR_CPU		GENMASK(7, 0)
#define IPI_RR_CLUSTER		GENMASK(23, 16)

void apple_rvbar_retention_wfi(void);

static phys_addr_t rvbar_mailbox_pa, rvbar_cpu_start_pa;
static struct rvbar_mailbox __iomem *rvbar_mailbox;
static void __iomem *rvbar_cpu_start;
u32 apple_ctxr_active __ro_after_init;
static phys_addr_t rvbar_vectors_pa, ctxr_entry;
extern char apple_ctxr_trampoline[], apple_ctxr_trampoline_end[];
extern char apple_ctxr_mailbox[], apple_ctxr_el2_vectors[];
/* Bit stride of a cluster in the packed start/stop mask: its largest core count. */
static u32 rvbar_cluster_stride;

static struct {
	u32 cluster;
	u32 core;
	phys_addr_t impl_pa;
	void __iomem *impl;
	u64 release_addr;
	bool powered_off;
} rvbar_cpus[NR_CPUS];

static int __init apple_rvbar_cpu_init(unsigned int cpu)
{
	struct device_node *dn, *mb;
	u64 impl;
	int ret;

	if (!rvbar_mailbox_pa) {
		struct resource res;

		mb = of_find_compatible_node(NULL, NULL, "apple,rvbar-mailbox");
		if (!mb)
			return -ENODEV;
		ret = of_property_read_u64(mb, "apple,mailbox", &rvbar_mailbox_pa) ?:
		      of_property_read_u64(mb, "apple,cpu-start", &rvbar_cpu_start_pa);
		if (!ret && apple_ctxr_active) {
			ret = of_address_to_resource(mb, 0, &res);
			if (!ret)
				rvbar_vectors_pa = res.start;
		}
		of_node_put(mb);
		if (ret)
			return ret;
	}

	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;
	ret = of_property_read_u32_index(dn, "apple,pmgr-cpu", 0, &rvbar_cpus[cpu].cluster) ?:
	      of_property_read_u32_index(dn, "apple,pmgr-cpu", 1, &rvbar_cpus[cpu].core) ?:
	      of_property_read_u64(dn, "apple,cpu-impl-reg", &impl);
	of_property_read_u64(dn, "cpu-release-addr", &rvbar_cpus[cpu].release_addr);
	of_node_put(dn);
	rvbar_cpus[cpu].impl_pa = impl;
	rvbar_cluster_stride = max(rvbar_cluster_stride, rvbar_cpus[cpu].core + 1);
	return ret;
}

static int apple_ctxr_install_entry(void)
{
	size_t size = apple_ctxr_trampoline_end - apple_ctxr_trampoline;
	size_t mailbox_offset = apple_ctxr_mailbox - apple_ctxr_trampoline;
	void *vectors = memremap(rvbar_vectors_pa, SZ_16K, MEMREMAP_WB);
	__le32 branch = cpu_to_le32(0x14000000 | (SZ_4K / 4));
	__le64 mailbox = cpu_to_le64(rvbar_mailbox_pa);

	if (!vectors)
		return -ENOMEM;

	/*
	 * m1n1's mailbox ends below +0x1000; the remaining 12 KiB of its
	 * reserved vectors page is inside CTXR-D. The same entry handles
	 * initial spin-table release and RVBAR after power-off or S2R.
	 */
	memcpy(vectors + SZ_4K, apple_ctxr_trampoline, size);
	memcpy(vectors + SZ_4K + mailbox_offset, &mailbox, sizeof(mailbox));
	memcpy(vectors, &branch, sizeof(branch));
	flush_icache_range((unsigned long)vectors, (unsigned long)vectors + SZ_16K);
	memunmap(vectors);
	ctxr_entry = rvbar_vectors_pa + SZ_4K;
	/* The boot CPU's hyp stub hands its VBAR_EL2 to the in-window copy. */
	__hyp_set_vectors(ctxr_entry + (apple_ctxr_el2_vectors - apple_ctxr_trampoline));
	pr_info("Apple CTXR: EL1 CPU entry at %pa; VHE/KVM unavailable\n", &ctxr_entry);
	return 0;
}

static int apple_rvbar_cpu_prepare(unsigned int cpu)
{
	if (!rvbar_mailbox) {
		int ret;

		rvbar_mailbox = ioremap(rvbar_mailbox_pa, sizeof(*rvbar_mailbox));
		rvbar_cpu_start = ioremap(rvbar_cpu_start_pa, CPU_START_CORE(8));
		if (!rvbar_mailbox || !rvbar_cpu_start)
			return -ENOMEM;
		if (apple_ctxr_active) {
			ret = apple_ctxr_install_entry();
			if (ret)
				return ret;
		}
		/* From here every reset is the OS's: m1n1's memory belongs to Linux. */
		writeq(RVBAR_MAILBOX_MAGIC, &rvbar_mailbox->magic);
	}
	rvbar_cpus[cpu].impl = ioremap(rvbar_cpus[cpu].impl_pa, CPU_IMPL_PWR + 4);
	return rvbar_cpus[cpu].impl ? 0 : -ENOMEM;
}

/* Point @cpu's mailbox slot at @entry for its next reset. */
void apple_rvbar_set_entry(unsigned int cpu, phys_addr_t entry)
{
	struct rvbar_slot __iomem *slot = &rvbar_mailbox->slots[cpu];

	writeq_relaxed(entry, &slot->entry);
	writeq_relaxed(cpu_logical_map(cpu) | RVBAR_SLOT_VALID, &slot->mpidr);
	writeq(RVBAR_MAILBOX_MAGIC, &rvbar_mailbox->magic);
}
EXPORT_SYMBOL_GPL(apple_rvbar_set_entry);

/* Make @cpu come back from its next reset in cpu_resume (S2R). */
void apple_rvbar_set_resume_entry(unsigned int cpu)
{
	apple_rvbar_set_entry(cpu, __pa_symbol(cpu_resume));
}
EXPORT_SYMBOL_GPL(apple_rvbar_set_resume_entry);

/* First start: release the core from m1n1's spin table like spin-table does. */
static int apple_rvbar_spin_release(unsigned int cpu)
{
	__le64 __iomem *release = ioremap_cache(rvbar_cpus[cpu].release_addr, sizeof(*release));

	if (!release)
		return -ENOMEM;
	if (apple_ctxr_active)
		apple_rvbar_set_entry(cpu, __pa_symbol(secondary_entry));
	writeq_relaxed(apple_ctxr_active ? ctxr_entry : __pa_symbol(secondary_entry), release);
	dcache_clean_inval_poc((__force unsigned long)release,
			       (__force unsigned long)release + sizeof(*release));
	iounmap(release);
	sev();
	return 0;
}

/* The core's bit in the packed start (+0x4) and stop (+0x0) masks. */
static u32 apple_rvbar_core_bit(unsigned int cpu)
{
	return BIT(rvbar_cpus[cpu].cluster * rvbar_cluster_stride + rvbar_cpus[cpu].core);
}

static int apple_rvbar_cpu_boot(unsigned int cpu)
{
	u32 cluster = rvbar_cpus[cpu].cluster, core = rvbar_cpus[cpu].core;

	if (!rvbar_cpus[cpu].powered_off)
		return apple_rvbar_spin_release(cpu);

	apple_rvbar_set_entry(cpu, __pa_symbol(secondary_entry));
	/* A core that already woke waits in the dispatcher's WFE for its slot. */
	dsb(sy);
	sev();

	writel(apple_rvbar_core_bit(cpu), rvbar_cpu_start + CPU_START_SYS);
	writel(BIT(core), rvbar_cpu_start + CPU_START_CORE(cluster));

	/*
	 * A core that went down through arm64_prepare_for_sleep(0) is power-gated
	 * like an idle core and comes back through RVBAR on an interrupt; an
	 * immediate fast IPI wakes it (inferred from the idle wake path: PMGR start
	 * alone left E0 down on J713, 2026-10-03).
	 */
	if (MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 1) ==
	    MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1))
		write_sysreg_s(FIELD_PREP(IPI_RR_CPU, MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0)),
			       SYS_APL_IPI_RR_LOCAL_EL1);
	else
		write_sysreg_s(FIELD_PREP(IPI_RR_CPU, MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0)) |
			       FIELD_PREP(IPI_RR_CLUSTER, MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1)),
			       SYS_APL_IPI_RR_GLOBAL_EL1);
	isb();
	return 0;
}

/*
 * A powered-down core does not always lose power on J713 (impl +0x100 stays
 * active, cpu_kill -ETIMEDOUT, 2026-10-03). Woken by cpu_boot's IPI with its
 * slot filled, it then takes the reset path itself: MMU off through the idmap
 * (cpu_soft_restart, as kexec) and into the slot's entry at the kernel's EL.
 */
void apple_rvbar_restart_if_slot(void)
{
	struct rvbar_slot __iomem *slot = &rvbar_mailbox->slots[smp_processor_id()];
	typeof(cpu_soft_restart) *restart;
	u64 entry;

	if (!(readq_relaxed(&slot->mpidr) & RVBAR_SLOT_VALID))
		return;
	entry = readq_relaxed(&slot->entry);
	cpu_install_idmap();
	restart = (void *)__pa_symbol(cpu_soft_restart);
	restart(0, entry, 0, 0, 0);
}
EXPORT_SYMBOL_GPL(apple_rvbar_restart_if_slot);

/*
 * arm64_prepare_for_sleep(1) (fffffe000bc31640..bc31664), the system-sleep
 * request, which XNU makes at EL2: the HVC enters the CTXR-D vectors
 * (apple_ctxr_el2_vectors), which write s3_5_c15_c6_2 and WFI until the
 * power goes. The boot CPU's VBAR_EL2 points there from
 * apple_ctxr_install_entry, every other start's from the trampoline.
 */
void __noreturn apple_rvbar_system_sleep(void)
{
	asm volatile("hvc #0" ::: "memory");
	unreachable();
}
EXPORT_SYMBOL_GPL(apple_rvbar_system_sleep);

/* ml_arm_sleep on a secondary in system sleep, run by the S2R driver. */
static void (*apple_rvbar_sleep_die)(unsigned int cpu);

void apple_rvbar_set_sleep_die(void (*die)(unsigned int cpu))
{
	apple_rvbar_sleep_die = die;
}
EXPORT_SYMBOL_GPL(apple_rvbar_set_sleep_die);

/*
 * arm64_prepare_for_sleep(0) (fffffe000bc31610..bc3163c), a core power-down:
 * SIQ_CFG_EL1[1:0] = 3 and bit 0 of s3_1_c15_c7_4 cleared, each followed by
 * an isb, then WFI until the power goes, acknowledging the fast IPI after
 * each wake. s3_1_c15_c7_4 reads 0x6 on J713, bit 0 already clear.
 */
static void __noreturn apple_rvbar_core_off(void)
{
	sysreg_clear_set_s(SYS_APL_SIQ_CFG_EL1, 3, 3);
	isb();
	/* Mach-O EL1 cannot access CORE_OFF; the EL2 entry initialized it. */
	if (!apple_ctxr_active) {
		sysreg_clear_set_s(SYS_APL_CORE_OFF_EL2, BIT(0), 0);
		isb();
	}

	/*
	 * The core does not lose power here on J713: impl +0x100 reads 0f01d900
	 * like an idle core in retention, and a bare WFI woken by cpu_boot (the
	 * fast IPI, or the PMGR start alone) hung the SoC with the loop's
	 * registers cleared (2026-10-04).
	 */
	for (;;) {
		apple_rvbar_retention_wfi();
		write_sysreg_s(1, SYS_APL_IPI_SR_EL1);
		apple_rvbar_restart_if_slot();
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static bool apple_rvbar_cpu_can_disable(unsigned int cpu)
{
	return true;
}

static int apple_rvbar_cpu_disable(unsigned int cpu)
{
	return 0;
}

/* PE_cpu_machine_quiesce: ApplePMGR::disableCPUCore, then ml_arm_sleep. */
static void apple_rvbar_cpu_die(unsigned int cpu)
{
	rvbar_cpus[cpu].powered_off = true;
	/* Any reset before the next cpu_boot parks in the dispatcher. */
	writeq(0, &rvbar_mailbox->slots[cpu].mpidr);
	dsb(sy);
	writel(apple_rvbar_core_bit(cpu), rvbar_cpu_start + CPU_START_STOP);
	if (pm_suspend_target_state == PM_SUSPEND_MEM)
		apple_rvbar_sleep_die(cpu);
	apple_rvbar_core_off();
}

static int apple_rvbar_cpu_kill(unsigned int cpu)
{
	u32 pwr;

	/*
	 * AppleARMCPU::quiesceCPU (fffffe0008c6eafc) does not wait for the core
	 * to lose power; in S2R the SoC sleep takes it down. (The SError once
	 * blamed on returning at once was the CoreSight write, see above.)
	 */
	if (pm_suspend_target_state == PM_SUSPEND_MEM)
		return 0;
	return readl_poll_timeout(rvbar_cpus[cpu].impl + CPU_IMPL_PWR, pwr,
				  !(pwr & CPU_IMPL_PWR_ON), 100, 50000);
}
#endif

const struct cpu_operations smp_apple_rvbar_ops = {
	.name		= "apple,rvbar",
	.cpu_init	= apple_rvbar_cpu_init,
	.cpu_prepare	= apple_rvbar_cpu_prepare,
	.cpu_boot	= apple_rvbar_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = apple_rvbar_cpu_can_disable,
	.cpu_disable	= apple_rvbar_cpu_disable,
	.cpu_die	= apple_rvbar_cpu_die,
	.cpu_kill	= apple_rvbar_cpu_kill,
#endif
};
