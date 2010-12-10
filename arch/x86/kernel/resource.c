#include <linux/ioport.h>
#include <linux/pnp.h>
#include <asm/e820.h>

#ifdef CONFIG_PNP
static bool resource_conflict(struct resource *res, resource_size_t start,
			      resource_size_t end)
{
	/*
	 * Return true if and only if "res" conflicts with the [start-end]
	 * range.
	 */
	return res->start <= end && res->end >= start;
}

static void resource_split(struct resource *res, resource_size_t start,
			   resource_size_t end, struct resource *low,
			   struct resource *high)
{
	/*
	 * If "res" conflicts with [start-end], split "res" into the
	 * part below "start" (low) and the part above "end" (high),
	 * either (or both) of which may be empty.
	 *
	 * If there's no conflict, return the entire "res" as "low".
	 */
	*low = *res;
	low->start = res->start;
	low->end = res->start - 1;	/* default to empty (size 0) */

	*high = *res;
	high->end = res->end;
	high->start = res->end + 1;	/* default to empty (size 0) */

	if (!resource_conflict(res, start, end)) {
		low->end = res->end;
		return;
	}

	if (res->start < start)
		low->end = start - 1;

	if (res->end > end)
		high->start = end + 1;
}

static void pnp_remove_reservations(struct resource *avail)
{
	unsigned long type = resource_type(avail);
	struct pnp_dev *dev;
	int i;
	struct resource *res, low, high;

	/*
	 * Clip the available region to avoid PNP devices.  The PNP
	 * resources really should be in the resource map to begin with,
	 * but there are still some issues preventing that.
	 */
	pnp_for_each_dev(dev) {
		i = 0;
		res = pnp_get_resource(dev, type, i++);
		while (res) {
			if (!(res->flags & IORESOURCE_WINDOW)) {
				resource_split(avail, res->start, res->end,
					       &low, &high);
				if (resource_size(&low) > resource_size(&high))
					*avail = low;
				else
					*avail = high;
			}

			res = pnp_get_resource(dev, type, i++);
		}
	}
}
#endif

void arch_remove_reservations(struct resource *avail)
{
	/*
	 * Trim out the area reserved for BIOS (low 1MB).  We could also remove
	 * E820 "reserved" areas here.
	 */
	if (avail->flags & IORESOURCE_MEM) {
		if (avail->start < BIOS_END)
			avail->start = BIOS_END;
	}

#ifdef CONFIG_PNP
	pnp_remove_reservations(avail);
#endif
}
