/*
 * perfmon.c: perfmon2 global initialization functions
 *
 * This file implements the perfmon2 interface which
 * provides access to the hardware performance counters
 * of the host processor.
 *
 *
 * The initial version of perfmon.c was written by
 * Ganesh Venkitachalam, IBM Corp.
 *
 * Then it was modified for perfmon-1.x by Stephane Eranian and
 * David Mosberger, Hewlett Packard Co.
 *
 * Version Perfmon-2.x is a complete rewrite of perfmon-1.x
 * by Stephane Eranian, Hewlett Packard Co.
 *
 * Copyright (c) 1999-2006 Hewlett-Packard Development Company, L.P.
 * Contributed by Stephane Eranian <eranian@hpl.hp.com>
 *                David Mosberger-Tang <davidm@hpl.hp.com>
 *
 * More information about perfmon available at:
 * 	http://www.hpl.hp.com/research/linux/perfmon
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307 USA
 */
#include <linux/kernel.h>
#include <linux/perfmon_kern.h>
#include "perfmon_priv.h"

/*
 * external variables
 */
DEFINE_PER_CPU(struct task_struct *, pmu_owner);
DEFINE_PER_CPU(struct pfm_context  *, pmu_ctx);
DEFINE_PER_CPU(u64, pmu_activation_number);

int perfmon_disabled;	/* >0 if perfmon is disabled */

/*
 * global initialization routine, executed only once
 */
int __init pfm_init(void)
{
	PFM_LOG("version %u.%u", PFM_VERSION_MAJ, PFM_VERSION_MIN);

	if (pfm_init_ctx())
		goto error_disable;

	if (pfm_init_fs())
		goto error_disable;

	/*
	 * one time, arch-specific global initialization
	 */
	if (pfm_arch_init())
		goto error_disable;

	return 0;

error_disable:
	PFM_ERR("perfmon is disabled due to initialization error");
	perfmon_disabled = 1;
	return -1;
}

/*
 * must use subsys_initcall() to ensure that the perfmon2 core
 * is initialized before any PMU description module when they are
 * compiled in.
 */
subsys_initcall(pfm_init);
