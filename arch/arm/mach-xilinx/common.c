/* arch/arm/mach-xilinx/common.c
 *
 * This file contains common code that is intended to be used across
 * boards so that it's not replicated.
 *
 *  Copyright (C) 2011 Xilinx
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/cpumask.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <asm/mach/map.h>
#include <asm/page.h>
#include <asm/hardware/gic.h>
#include <asm/hardware/cache-l2x0.h>

#include <mach/xilinx_soc.h>
#include <mach/clkdev.h>
#include "common.h"

/*
 * Clock function infrastructure.
 */
int clk_enable(struct clk *clk)
{
	return 0;
}

void clk_disable(struct clk *clk)
{
}

unsigned long clk_get_rate(struct clk *clk)
{
	return clk->rate;
}

/**
 * system_init - System specific initialization, intended to be called from
 *			board specific initialization.
 *
 **/
void __init xilinx_system_init(void)
{
#ifdef CONFIG_CACHE_L2X0
	/*
	 * 64KB way size, 8-way associativity, parity disabled
	 */
	l2x0_init(PL310_L2CC_BASE, 0x02060000, 0xF0F0FFFF);
#endif
}

/**
 * irq_init - Interrupt controller initialization for the GIC.
 *
 **/
void __init xilinx_irq_init(void)
{
	gic_init(0, 29, SCU_GIC_DIST_BASE, SCU_GIC_CPU_BASE);
}

/* The minimum devices needed to be mapped before the VM system is up and
 * running include the GIC, UART and Timer Counter.
 */

static struct map_desc io_desc[] __initdata = {
	{
		.virtual	= TTC0_VIRT,
		.pfn		= __phys_to_pfn(TTC0_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= SCU_PERIPH_VIRT,
		.pfn		= __phys_to_pfn(SCU_PERIPH_PHYS),
		.length		= SZ_8K,
		.type		= MT_DEVICE,
	}, {
		.virtual	= PL310_L2CC_VIRT,
		.pfn		= __phys_to_pfn(PL310_L2CC_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},

#ifdef CONFIG_DEBUG_LL
	{
		.virtual	= UART0_VIRT,
		.pfn		= __phys_to_pfn(UART0_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
#endif

};

/**
 * map_io - Create memory mappings needed for early I/O.
 *
 **/
void __init xilinx_map_io(void)
{
	iotable_init(io_desc, ARRAY_SIZE(io_desc));
}
