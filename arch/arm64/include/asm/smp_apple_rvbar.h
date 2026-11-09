/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_SMP_APPLE_RVBAR_H
#define __ASM_SMP_APPLE_RVBAR_H

#include <linux/types.h>

void apple_rvbar_set_entry(unsigned int cpu, phys_addr_t entry);
void apple_rvbar_set_resume_entry(unsigned int cpu);
void apple_rvbar_restart_if_slot(void);
void __noreturn apple_rvbar_system_sleep(void);
void apple_rvbar_set_sleep_die(void (*die)(unsigned int cpu));
phys_addr_t apple_rvbar_s2r_dbg_pa(void);

#endif
