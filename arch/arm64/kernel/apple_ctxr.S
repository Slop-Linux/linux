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

/* Both callers run with the MMU off; no identity mapping is needed here. */
	.pushsection ".text", "ax"
	.balign 8
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
	finalise_el2_state
	isb
	ret
SYM_FUNC_END(apple_ctxr_prepare_el1)

	.balign 8
SYM_DATA_START(apple_ctxr_mailbox)
	.quad 0
SYM_DATA_END(apple_ctxr_mailbox)
SYM_DATA_START(apple_ctxr_trampoline_end)
SYM_DATA_END(apple_ctxr_trampoline_end)
	.popsection
