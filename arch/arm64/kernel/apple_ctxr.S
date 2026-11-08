/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * T8132 / 26A428 Mach-O secondaries permit EL2 fetches in the CTXR-D
 * window and EL1 fetches outside it. CTXR-D covers the loaded region from
 * DeviceTree/TrustCache through the executable segment, not just TEXT_EXEC.
 * Copy this position-independent entry into the reserved m1n1 vectors page,
 * which lies within that window, before releasing CPUs.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/el2_setup.h>
#include <asm/kvm_arm.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>

/*
 * EL2 is relinquished permanently, including on the primary boot before
 * idreg overrides are parsed. Enable the hardware's EL1 facilities here;
 * Linux can still hide features with its ordinary EL1 capability policy.
 * Unlike finalise_el2_state's normal use, this copy cannot reference
 * kernel globals: it also runs at its relocated physical reset address.
 */
.purgem __check_override
.macro __check_override idreg, fld, width, pass, fail, tmp1, tmp2
	ubfx	\tmp1, \tmp1, #\fld, #\width
	cbnz	\tmp1, \pass
	b	\fail
.endm

/*
 * Both callers run with the MMU off; no identity mapping is needed here.
 * Aligned to 2 KiB here and copied to a 4 KiB boundary, the copy keeps
 * apple_ctxr_el2_vectors a valid VBAR_EL2.
 */
	.pushsection ".text", "ax"
	.balign 2048
SYM_CODE_START(apple_ctxr_trampoline)
	msr	daifset, #0xf
	msr	oslar_el1, xzr
	mrs	x9, mpidr_el1
	and	x9, x9, #0xffffff
	orr	x9, x9, #(1 << 63)
1:
	adr	x10, apple_ctxr_mailbox
	ldr	x10, [x10]
	add	x10, x10, #16
	mov	x11, #24
2:
	ldp	x12, x19, [x10], #16
	cmp	x12, x9
	b.eq	3f
	subs	x11, x11, #1
	b.ne	2b
4:
	wfe
	b	1b
3:
	cbz	x19, 4b
	dsb	sy
	init_el2_hcr HCR_HOST_NVHE_FLAGS | HCR_ATA
	init_el2_state
	bl	apple_ctxr_prepare_el1
	adr	x0, apple_ctxr_el2_vectors
	msr	vbar_el2, x0
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr_s	SYS_SCTLR_EL12, x0
	msr	elr_el2, x19
	mov	x0, #INIT_PSTATE_EL1
	msr	spsr_el2, x0
	mov	x0, xzr
	isb
	eret
SYM_CODE_END(apple_ctxr_trampoline)

SYM_FUNC_START(apple_ctxr_prepare_el1)
	/*
	 * CORE_OFF (arm64_prepare_for_sleep, 26A428) is accessible at EL2,
	 * but its EL1 read is UNDEFINED on both E and P cores in Mach-O boot.
	 * Initialize bit 0 before relinquishing EL2. Both core types read 0x6
	 * here, so this preserves their existing core power-down configuration.
	 */
	mrs_s	x0, sys_reg(3, 1, 15, 7, 4)
	bic	x0, x0, #1
	msr_s	sys_reg(3, 1, 15, 7, 4), x0
	finalise_el2_state
	isb
	ret
SYM_FUNC_END(apple_ctxr_prepare_el1)

/*
 * EL2 vectors inside CTXR-D. Their one entry, synchronous from EL1, is the
 * HVC of apple_rvbar_system_sleep: arm64_prepare_for_sleep(1) as XNU runs it
 * at EL2 (26A428 K:fffffe000bc31640..bc31664): s3_5_c15_c6_2 |= bit 0 |
 * bit 63 with no isb, then dsb sy; isb; wfi forever, acknowledging the fast
 * IPI (s3_5_c15_c1_1 = 1) after each wake. The only exit is the power loss
 * of system sleep and the reset through RVBAR. No register is kept across
 * WFI, which may clear them.
 */
	.balign 2048
SYM_CODE_START(apple_ctxr_el2_vectors)
	.rept	8
	b	.
	.balign	0x80
	.endr
	b	apple_ctxr_el2_system_sleep	// synchronous, lower EL, AArch64
	.balign	0x80
	.rept	7
	b	.
	.balign	0x80
	.endr
SYM_CODE_END(apple_ctxr_el2_vectors)

SYM_CODE_START_LOCAL(apple_ctxr_el2_system_sleep)
	mrs_s	x1, sys_reg(3, 5, 15, 6, 2)
	orr	x1, x1, #1
	orr	x1, x1, #(1 << 63)
	msr_s	sys_reg(3, 5, 15, 6, 2), x1
1:	dsb	sy
	isb
	wfi
	mov	x9, #1
	msr_s	sys_reg(3, 5, 15, 1, 1), x9
	b	1b
SYM_CODE_END(apple_ctxr_el2_system_sleep)

	.balign 8
SYM_DATA_START(apple_ctxr_mailbox)
	.quad 0
SYM_DATA_END(apple_ctxr_mailbox)
SYM_DATA_START(apple_ctxr_trampoline_end)
SYM_DATA_END(apple_ctxr_trampoline_end)
	.popsection
