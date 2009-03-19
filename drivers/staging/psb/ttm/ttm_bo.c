/**************************************************************************
 *
 * Copyright (c) 2006-2008 Tungsten Graphics, Inc., Cedar Park, TX., USA
 * All Rights Reserved.
 * Copyright (c) 2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstr�m <thomas-at-tungstengraphics-dot-com>
 */

#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/list.h>
#include "ttm_bo_driver.h"
#include "ttm_placement_common.h"

#define TTM_ASSERT_LOCKED(param)
#define TTM_DEBUG(fmt, arg...)
#define TTM_BO_HASH_ORDER 13

static int ttm_bo_setup_vm(struct ttm_buffer_object *bo);
static void ttm_bo_unmap_virtual(struct ttm_buffer_object *bo);
static int ttm_bo_swapout(struct ttm_mem_shrink *shrink);

static inline uint32_t ttm_bo_type_flags(unsigned type)
{
	return (1 << (type));
}

static void ttm_bo_release_list(struct kref *list_kref)
{
	struct ttm_buffer_object *bo =
	    container_of(list_kref, struct ttm_buffer_object, list_kref);
	struct ttm_bo_device *bdev = bo->bdev;

	BUG_ON(atomic_read(&bo->list_kref.refcount));
	BUG_ON(atomic_read(&bo->kref.refcount));
	BUG_ON(atomic_read(&bo->cpu_writers));
	BUG_ON(bo->sync_obj != NULL);
	BUG_ON(bo->mem.mm_node != NULL);
	BUG_ON(!list_empty(&bo->lru));
	BUG_ON(!list_empty(&bo->ddestroy));

	if (bo->ttm)
		ttm_tt_destroy(bo->ttm);
	if (bo->destroy)
		bo->destroy(bo);
	else {
		ttm_mem_global_free(bdev->mem_glob, bo->acc_size, false);
		kfree(bo);
	}
}

int ttm_bo_wait_unreserved(struct ttm_buffer_object *bo, bool interruptible)
{

	if (interruptible) {
		int ret = 0;

		ret = wait_event_interruptible(bo->event_queue,
					       atomic_read(&bo->reserved) == 0);
		if (unlikely(ret != 0))
			return -ERESTART;
	} else {
		wait_event(bo->event_queue, atomic_read(&bo->reserved) == 0);
	}
	return 0;
}

static void ttm_bo_add_to_lru(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man;

	BUG_ON(!atomic_read(&bo->reserved));

	if (!(bo->mem.flags & TTM_PL_FLAG_NO_EVICT)) {

		BUG_ON(!list_empty(&bo->lru));

		man = &bdev->man[bo->mem.mem_type];
		list_add_tail(&bo->lru, &man->lru);
		kref_get(&bo->list_kref);

		if (bo->ttm != NULL) {
			list_add_tail(&bo->swap, &bdev->swap_lru);
			kref_get(&bo->list_kref);
		}
	}
}

/*
 * Call with bdev->lru_lock and bdev->global->swap_lock held..
 */

static int ttm_bo_del_from_lru(struct ttm_buffer_object *bo)
{
	int put_count = 0;

	if (!list_empty(&bo->swap)) {
		list_del_init(&bo->swap);
		++put_count;
	}
	if (!list_empty(&bo->lru)) {
		list_del_init(&bo->lru);
		++put_count;
	}

	/*
	 * TODO: Add a driver hook to delete from
	 * driver-specific LRU's here.
	 */

	return put_count;
}

int ttm_bo_reserve_locked(struct ttm_buffer_object *bo,
			  bool interruptible,
			  bool no_wait, bool use_sequence, uint32_t sequence)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int ret;

	while (unlikely(atomic_cmpxchg(&bo->reserved, 0, 1) != 0)) {
		if (use_sequence && bo->seq_valid &&
			(sequence - bo->val_seq < (1 << 31))) {
			return -EAGAIN;
		}

		if (no_wait)
			return -EBUSY;

		spin_unlock(&bdev->lru_lock);
		ret = ttm_bo_wait_unreserved(bo, interruptible);
		spin_lock(&bdev->lru_lock);

		if (unlikely(ret))
			return ret;
	}

	if (use_sequence) {
		bo->val_seq = sequence;
		bo->seq_valid = true;
	} else {
		bo->seq_valid = false;
	}

	return 0;
}

static void ttm_bo_ref_bug(struct kref *list_kref)
{
	BUG();
}

int ttm_bo_reserve(struct ttm_buffer_object *bo,
		   bool interruptible,
		   bool no_wait, bool use_sequence, uint32_t sequence)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int put_count = 0;
	int ret;

	spin_lock(&bdev->lru_lock);
	ret = ttm_bo_reserve_locked(bo, interruptible, no_wait, use_sequence,
				    sequence);
	if (likely(ret == 0))
		put_count = ttm_bo_del_from_lru(bo);
	spin_unlock(&bdev->lru_lock);

	while (put_count--)
		kref_put(&bo->list_kref, ttm_bo_ref_bug);

	return ret;
}

void ttm_bo_unreserve(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;

	spin_lock(&bdev->lru_lock);
	ttm_bo_add_to_lru(bo);
	atomic_set(&bo->reserved, 0);
	wake_up_all(&bo->event_queue);
	spin_unlock(&bdev->lru_lock);
}

/*
 * Call bo->mutex locked.
 */

static int ttm_bo_add_ttm(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int ret = 0;
	uint32_t page_flags = 0;

	TTM_ASSERT_LOCKED(&bo->mutex);
	bo->ttm = NULL;

	switch (bo->type) {
	case ttm_bo_type_device:
	case ttm_bo_type_kernel:
		bo->ttm = ttm_tt_create(bdev, bo->num_pages << PAGE_SHIFT,
					page_flags, bdev->dummy_read_page);
		if (unlikely(bo->ttm == NULL))
			ret = -ENOMEM;
		break;
	case ttm_bo_type_user:
		bo->ttm = ttm_tt_create(bdev, bo->num_pages << PAGE_SHIFT,
					page_flags | TTM_PAGE_FLAG_USER,
					bdev->dummy_read_page);
		if (unlikely(bo->ttm == NULL))
			ret = -ENOMEM;
		break;

		ret = ttm_tt_set_user(bo->ttm, current,
				      bo->buffer_start, bo->num_pages);
		if (unlikely(ret != 0))
			ttm_tt_destroy(bo->ttm);
		break;
	default:
		printk(KERN_ERR "Illegal buffer object type\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ttm_bo_handle_move_mem(struct ttm_buffer_object *bo,
				  struct ttm_mem_reg *mem,
				  bool evict, bool interruptible, bool no_wait)
{
	struct ttm_bo_device *bdev = bo->bdev;
	bool old_is_pci = ttm_mem_reg_is_pci(bdev, &bo->mem);
	bool new_is_pci = ttm_mem_reg_is_pci(bdev, mem);
	struct ttm_mem_type_manager *old_man = &bdev->man[bo->mem.mem_type];
	struct ttm_mem_type_manager *new_man = &bdev->man[mem->mem_type];
	int ret = 0;

	if (old_is_pci || new_is_pci ||
	    ((mem->flags & bo->mem.flags & TTM_PL_MASK_CACHING) == 0))
		ttm_bo_unmap_virtual(bo);

	/*
	 * Create and bind a ttm if required.
	 */

	if (!(new_man->flags & TTM_MEMTYPE_FLAG_FIXED) && (bo->ttm == NULL)) {
		ret = ttm_bo_add_ttm(bo);
		if (ret)
			goto out_err;

		ret = ttm_tt_set_placement_caching(bo->ttm, mem->flags);
		if (ret)
			return ret;

		if (mem->mem_type != TTM_PL_SYSTEM) {
			ret = ttm_tt_bind(bo->ttm, mem);
			if (ret)
				goto out_err;
		}

		if (bo->mem.mem_type == TTM_PL_SYSTEM) {

			struct ttm_mem_reg *old_mem = &bo->mem;
			uint32_t save_flags = old_mem->flags;
			uint32_t save_proposed_flags = old_mem->proposed_flags;

			*old_mem = *mem;
			mem->mm_node = NULL;
			old_mem->proposed_flags = save_proposed_flags;
			ttm_flag_masked(&save_flags, mem->flags,
					TTM_PL_MASK_MEMTYPE);
			goto moved;
		}

	}

	if (!(old_man->flags & TTM_MEMTYPE_FLAG_FIXED) &&
	    !(new_man->flags & TTM_MEMTYPE_FLAG_FIXED))
		ret = ttm_bo_move_ttm(bo, evict, no_wait, mem);
	else if (bdev->driver->move)
		ret = bdev->driver->move(bo, evict, interruptible,
					 no_wait, mem);
	else
		ret = ttm_bo_move_memcpy(bo, evict, no_wait, mem);

	if (ret)
		goto out_err;

      moved:
	if (bo->priv_flags & TTM_BO_PRIV_FLAG_EVICTED) {
		ret = bdev->driver->invalidate_caches(bdev, bo->mem.flags);
		if (ret)
			printk(KERN_ERR "Can not flush read caches\n");
	}

	ttm_flag_masked(&bo->priv_flags,
			(evict) ? TTM_BO_PRIV_FLAG_EVICTED : 0,
			TTM_BO_PRIV_FLAG_EVICTED);

	if (bo->mem.mm_node)
		bo->offset = (bo->mem.mm_node->start << PAGE_SHIFT) +
		    bdev->man[bo->mem.mem_type].gpu_offset;

	return 0;

      out_err:
	new_man = &bdev->man[bo->mem.mem_type];
	if ((new_man->flags & TTM_MEMTYPE_FLAG_FIXED) && bo->ttm) {
		ttm_tt_unbind(bo->ttm);
		ttm_tt_destroy(bo->ttm);
		bo->ttm = NULL;
	}

	return ret;
}

static int ttm_bo_expire_sync_obj(struct ttm_buffer_object *bo,
				  bool allow_errors)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_driver *driver = bdev->driver;

	if (bo->sync_obj) {
		if (bdev->nice_mode) {
			unsigned long _end = jiffies + 3 * HZ;
			int ret;
			do {
				ret = ttm_bo_wait(bo, false, false, false);
				if (ret && allow_errors)
					return ret;

			} while (ret && !time_after_eq(jiffies, _end));

			if (bo->sync_obj) {
				bdev->nice_mode = false;
				printk(KERN_ERR "Detected probable GPU lockup. "
				       "Evicting buffer.\n");
			}
		}
		if (bo->sync_obj) {
			driver->sync_obj_unref(&bo->sync_obj);
			bo->priv_flags &= ~TTM_BO_PRIV_FLAG_MOVING;
		}
	}
	return 0;
}

/**
 * If bo idle, remove from delayed- and lru lists, and unref.
 * If not idle, and already on delayed list, do nothing.
 * If not idle, and not on delayed list, put on delayed list,
 *   up the list_kref and schedule a delayed list check.
 */

static void ttm_bo_cleanup_refs(struct ttm_buffer_object *bo, bool remove_all)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_driver *driver = bdev->driver;

	mutex_lock(&bo->mutex);

	if (bo->sync_obj && driver->sync_obj_signaled(bo->sync_obj,
						      bo->sync_obj_arg)) {
		driver->sync_obj_unref(&bo->sync_obj);
		bo->priv_flags &= ~TTM_BO_PRIV_FLAG_MOVING;
	}

	if (bo->sync_obj && remove_all)
		(void)ttm_bo_expire_sync_obj(bo, false);

	if (!bo->sync_obj) {
		int put_count;

		if (bo->ttm)
			ttm_tt_unbind(bo->ttm);
		spin_lock(&bdev->lru_lock);
		if (!list_empty(&bo->ddestroy)) {
			list_del_init(&bo->ddestroy);
			kref_put(&bo->list_kref, ttm_bo_ref_bug);
		}
		if (bo->mem.mm_node) {
			drm_mm_put_block(bo->mem.mm_node);
			bo->mem.mm_node = NULL;
		}
		put_count = ttm_bo_del_from_lru(bo);
		spin_unlock(&bdev->lru_lock);
		mutex_unlock(&bo->mutex);
		while (put_count--)
			kref_put(&bo->list_kref, ttm_bo_release_list);

		return;
	}

	spin_lock(&bdev->lru_lock);
	if (list_empty(&bo->ddestroy)) {
		spin_unlock(&bdev->lru_lock);
		driver->sync_obj_flush(bo->sync_obj, bo->sync_obj_arg);
		spin_lock(&bdev->lru_lock);
		if (list_empty(&bo->ddestroy)) {
			kref_get(&bo->list_kref);
			list_add_tail(&bo->ddestroy, &bdev->ddestroy);
		}
		spin_unlock(&bdev->lru_lock);
		schedule_delayed_work(&bdev->wq,
				      ((HZ / 100) < 1) ? 1 : HZ / 100);
	} else
		spin_unlock(&bdev->lru_lock);

	mutex_unlock(&bo->mutex);
	return;
}

/**
 * Traverse the delayed list, and call ttm_bo_cleanup_refs on all
 * encountered buffers.
 */

static int ttm_bo_delayed_delete(struct ttm_bo_device *bdev, bool remove_all)
{
	struct ttm_buffer_object *entry, *nentry;
	struct list_head *list, *next;
	int ret;

	spin_lock(&bdev->lru_lock);
	list_for_each_safe(list, next, &bdev->ddestroy) {
		entry = list_entry(list, struct ttm_buffer_object, ddestroy);
		nentry = NULL;

		/*
		 * Protect the next list entry from destruction while we
		 * unlock the lru_lock.
		 */

		if (next != &bdev->ddestroy) {
			nentry = list_entry(next, struct ttm_buffer_object,
					    ddestroy);
			kref_get(&nentry->list_kref);
		}
		kref_get(&entry->list_kref);

		spin_unlock(&bdev->lru_lock);
		ttm_bo_cleanup_refs(entry, remove_all);
		kref_put(&entry->list_kref, ttm_bo_release_list);
		spin_lock(&bdev->lru_lock);

		if (nentry) {
			bool next_onlist = !list_empty(next);
			kref_put(&nentry->list_kref, ttm_bo_release_list);

			/*
			 * Someone might have raced us and removed the
			 * next entry from the list. We don't bother restarting
			 * list traversal.
			 */

			if (!next_onlist)
				break;
		}
	}
	ret = !list_empty(&bdev->ddestroy);
	spin_unlock(&bdev->lru_lock);

	return ret;
}

static void ttm_bo_delayed_workqueue(struct work_struct *work)
{
	struct ttm_bo_device *bdev =
	    container_of(work, struct ttm_bo_device, wq.work);

	if (ttm_bo_delayed_delete(bdev, false)) {
		schedule_delayed_work(&bdev->wq,
				      ((HZ / 100) < 1) ? 1 : HZ / 100);
	}
}

static void ttm_bo_release(struct kref *kref)
{
	struct ttm_buffer_object *bo =
	    container_of(kref, struct ttm_buffer_object, kref);
	struct ttm_bo_device *bdev = bo->bdev;

	if (likely(bo->vm_node != NULL)) {
		rb_erase(&bo->vm_rb, &bdev->addr_space_rb);
		drm_mm_put_block(bo->vm_node);
	}
	write_unlock(&bdev->vm_lock);
	ttm_bo_cleanup_refs(bo, false);
	kref_put(&bo->list_kref, ttm_bo_release_list);
	write_lock(&bdev->vm_lock);
}

void ttm_bo_unref(struct ttm_buffer_object **p_bo)
{
	struct ttm_buffer_object *bo = *p_bo;
	struct ttm_bo_device *bdev = bo->bdev;

	*p_bo = NULL;
	write_lock(&bdev->vm_lock);
	kref_put(&bo->kref, ttm_bo_release);
	write_unlock(&bdev->vm_lock);
}

static int ttm_bo_evict(struct ttm_buffer_object *bo, unsigned mem_type,
			bool interruptible, bool no_wait)
{
	int ret = 0;
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_reg evict_mem;

	if (bo->mem.mem_type != mem_type)
		goto out;

	ret = ttm_bo_wait(bo, false, interruptible, no_wait);
	if (ret && ret != -ERESTART) {
		printk(KERN_ERR "Failed to expire sync object before "
		       "buffer eviction.\n");
		goto out;
	}

	BUG_ON(!atomic_read(&bo->reserved));

	evict_mem = bo->mem;
	evict_mem.mm_node = NULL;

	evict_mem.proposed_flags = bdev->driver->evict_flags(bo);
	BUG_ON(ttm_bo_type_flags(mem_type) & evict_mem.proposed_flags);

	ret = ttm_bo_mem_space(bo, &evict_mem, interruptible, no_wait);
	if (unlikely(ret != 0 && ret != -ERESTART)) {
		evict_mem.proposed_flags = TTM_PL_FLAG_SYSTEM;
		BUG_ON(ttm_bo_type_flags(mem_type) & evict_mem.proposed_flags);
		ret = ttm_bo_mem_space(bo, &evict_mem, interruptible, no_wait);
	}

	if (ret) {
		if (ret != -ERESTART)
			printk(KERN_ERR "Failed to find memory space for "
			       "buffer 0x%p eviction.\n", bo);
		goto out;
	}

	ret = ttm_bo_handle_move_mem(bo, &evict_mem, true, interruptible, no_wait);
	if (ret) {
		if (ret != -ERESTART)
			printk(KERN_ERR "Buffer eviction failed\n");
		goto out;
	}

	spin_lock(&bdev->lru_lock);
	if (evict_mem.mm_node) {
		drm_mm_put_block(evict_mem.mm_node);
		evict_mem.mm_node = NULL;
	}
	spin_unlock(&bdev->lru_lock);

	ttm_flag_masked(&bo->priv_flags, TTM_BO_PRIV_FLAG_EVICTED,
			TTM_BO_PRIV_FLAG_EVICTED);

      out:
	return ret;
}

/**
 * Repeatedly evict memory from the LRU for @mem_type until we create enough
 * space, or we've evicted everything and there isn't enough space.
 */
static int ttm_bo_mem_force_space(struct ttm_bo_device *bdev,
				  struct ttm_mem_reg *mem,
				  uint32_t mem_type,
				  bool interruptible, bool no_wait)
{
	struct drm_mm_node *node;
	struct ttm_buffer_object *entry;
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];
	struct list_head *lru;
	unsigned long num_pages = mem->num_pages;
	int put_count = 0;
	int ret;

      retry_pre_get:
	ret = drm_mm_pre_get(&man->manager);
	if (unlikely(ret != 0))
		return ret;

	spin_lock(&bdev->lru_lock);
	do {
		node = drm_mm_search_free(&man->manager, num_pages,
					  mem->page_alignment, 1);
		if (node)
			break;

		lru = &man->lru;
		if (list_empty(lru))
			break;

		entry = list_first_entry(lru, struct ttm_buffer_object, lru);
		kref_get(&entry->list_kref);

		ret =
		    ttm_bo_reserve_locked(entry, interruptible, no_wait, false, 0);

		if (likely(ret == 0))
			put_count = ttm_bo_del_from_lru(entry);

		spin_unlock(&bdev->lru_lock);

		if (unlikely(ret != 0))
			return ret;

		while (put_count--)
			kref_put(&entry->list_kref, ttm_bo_ref_bug);

		mutex_lock(&entry->mutex);
		ret = ttm_bo_evict(entry, mem_type, interruptible, no_wait);
		mutex_unlock(&entry->mutex);

		ttm_bo_unreserve(entry);

		kref_put(&entry->list_kref, ttm_bo_release_list);
		if (ret)
			return ret;

		spin_lock(&bdev->lru_lock);
	} while (1);

	if (!node) {
		spin_unlock(&bdev->lru_lock);
		return -ENOMEM;
	}

	node = drm_mm_get_block_atomic(node, num_pages, mem->page_alignment);
	if (unlikely(!node)) {
		spin_unlock(&bdev->lru_lock);
		goto retry_pre_get;
	}

	spin_unlock(&bdev->lru_lock);
	mem->mm_node = node;
	mem->mem_type = mem_type;
	return 0;
}

static bool ttm_bo_mt_compatible(struct ttm_mem_type_manager *man,
				 bool disallow_fixed,
				 uint32_t mem_type,
				 uint32_t mask, uint32_t * res_mask)
{
	uint32_t cur_flags = ttm_bo_type_flags(mem_type);

	if ((man->flags & TTM_MEMTYPE_FLAG_FIXED) && disallow_fixed)
		return false;

	if ((cur_flags & mask & TTM_PL_MASK_MEM) == 0)
		return false;

	if ((mask & man->available_caching) == 0)
		return false;
	if (mask & man->default_caching)
		cur_flags |= man->default_caching;
	else if (mask & TTM_PL_FLAG_CACHED)
		cur_flags |= TTM_PL_FLAG_CACHED;
	else if (mask & TTM_PL_FLAG_WC)
		cur_flags |= TTM_PL_FLAG_WC;
	else
		cur_flags |= TTM_PL_FLAG_UNCACHED;

	*res_mask = cur_flags;
	return true;
}

/**
 * Creates space for memory region @mem according to its type.
 *
 * This function first searches for free space in compatible memory types in
 * the priority order defined by the driver.  If free space isn't found, then
 * ttm_bo_mem_force_space is attempted in priority order to evict and find
 * space.
 */
int ttm_bo_mem_space(struct ttm_buffer_object *bo,
		     struct ttm_mem_reg *mem, bool interruptible, bool no_wait)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man;

	uint32_t num_prios = bdev->driver->num_mem_type_prio;
	const uint32_t *prios = bdev->driver->mem_type_prio;
	uint32_t i;
	uint32_t mem_type = TTM_PL_SYSTEM;
	uint32_t cur_flags = 0;
	bool type_found = false;
	bool type_ok = false;
	bool has_eagain = false;
	struct drm_mm_node *node = NULL;
	int ret;

	mem->mm_node = NULL;
	for (i = 0; i < num_prios; ++i) {
		mem_type = prios[i];
		man = &bdev->man[mem_type];

		type_ok = ttm_bo_mt_compatible(man,
					       bo->type == ttm_bo_type_user,
					       mem_type, mem->proposed_flags,
					       &cur_flags);

		if (!type_ok)
			continue;

		if (mem_type == TTM_PL_SYSTEM)
			break;

		if (man->has_type && man->use_type) {
			type_found = true;
			do {
				ret = drm_mm_pre_get(&man->manager);
				if (unlikely(ret))
					return ret;

				spin_lock(&bdev->lru_lock);
				node = drm_mm_search_free(&man->manager,
							  mem->num_pages,
							  mem->page_alignment,
							  1);
				if (unlikely(!node)) {
					spin_unlock(&bdev->lru_lock);
					break;
				}
				node = drm_mm_get_block_atomic(node,
							       mem->num_pages,
							       mem->
							       page_alignment);
				spin_unlock(&bdev->lru_lock);
			} while (!node);
		}
		if (node)
			break;
	}

	if ((type_ok && (mem_type == TTM_PL_SYSTEM)) || node) {
		mem->mm_node = node;
		mem->mem_type = mem_type;
		mem->flags = cur_flags;
		return 0;
	}

	if (!type_found)
		return -EINVAL;

	num_prios = bdev->driver->num_mem_busy_prio;
	prios = bdev->driver->mem_busy_prio;

	for (i = 0; i < num_prios; ++i) {
		mem_type = prios[i];
		man = &bdev->man[mem_type];

		if (!man->has_type)
			continue;

		if (!ttm_bo_mt_compatible(man,
					  bo->type == ttm_bo_type_user,
					  mem_type,
					  mem->proposed_flags, &cur_flags))
			continue;

		ret = ttm_bo_mem_force_space(bdev, mem, mem_type,
					     interruptible, no_wait);

		if (ret == 0 && mem->mm_node) {
			mem->flags = cur_flags;
			return 0;
		}

		if (ret == -ERESTART)
			has_eagain = true;
	}

	ret = (has_eagain) ? -ERESTART : -ENOMEM;
	return ret;
}

/*
 * Call bo->mutex locked.
 * Returns 1 if the buffer is currently rendered to or from. 0 otherwise.
 */

static int ttm_bo_busy(struct ttm_buffer_object *bo)
{
	void *sync_obj = bo->sync_obj;
	struct ttm_bo_driver *driver = bo->bdev->driver;

	if (sync_obj) {
		if (driver->sync_obj_signaled(sync_obj, bo->sync_obj_arg)) {
			driver->sync_obj_unref(&bo->sync_obj);
			bo->priv_flags &= ~TTM_BO_PRIV_FLAG_MOVING;
			return 0;
		}
		driver->sync_obj_flush(sync_obj, bo->sync_obj_arg);
		if (driver->sync_obj_signaled(sync_obj, bo->sync_obj_arg)) {
			driver->sync_obj_unref(&bo->sync_obj);
			bo->priv_flags &= ~TTM_BO_PRIV_FLAG_MOVING;
			return 0;
		}
		return 1;
	}
	return 0;
}

int ttm_bo_wait_cpu(struct ttm_buffer_object *bo, bool no_wait)
{
	int ret = 0;

	if ((atomic_read(&bo->cpu_writers) > 0) && no_wait)
		return -EBUSY;

	ret = wait_event_interruptible(bo->event_queue,
				       atomic_read(&bo->cpu_writers) == 0);

	if (ret == -ERESTARTSYS)
		ret = -ERESTART;

	return ret;
}

/*
 * bo->mutex locked.
 * Note that new_mem_flags are NOT transferred to the bo->mem.proposed_flags.
 */

int ttm_bo_move_buffer(struct ttm_buffer_object *bo, uint32_t new_mem_flags,
		       bool interruptible, bool no_wait)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int ret = 0;
	struct ttm_mem_reg mem;

	BUG_ON(!atomic_read(&bo->reserved));

	/*
	 * FIXME: It's possible to pipeline buffer moves.
	 * Have the driver move function wait for idle when necessary,
	 * instead of doing it here.
	 */

	ttm_bo_busy(bo);
	ret = ttm_bo_wait(bo, false, interruptible, no_wait);
	if (ret)
		return ret;

	mem.num_pages = bo->num_pages;
	mem.size = mem.num_pages << PAGE_SHIFT;
	mem.proposed_flags = new_mem_flags;
	mem.page_alignment = bo->mem.page_alignment;

	/*
	 * Determine where to move the buffer.
	 */

	ret = ttm_bo_mem_space(bo, &mem, interruptible, no_wait);
	if (ret)
		goto out_unlock;

	ret = ttm_bo_handle_move_mem(bo, &mem, false, interruptible, no_wait);

      out_unlock:
	if (ret && mem.mm_node) {
		spin_lock(&bdev->lru_lock);
		drm_mm_put_block(mem.mm_node);
		spin_unlock(&bdev->lru_lock);
	}
	return ret;
}

static int ttm_bo_mem_compat(struct ttm_mem_reg *mem)
{
	if ((mem->proposed_flags & mem->flags & TTM_PL_MASK_MEM) == 0)
		return 0;
	if ((mem->proposed_flags & mem->flags & TTM_PL_MASK_CACHING) == 0)
		return 0;

	return 1;
}

int ttm_buffer_object_validate(struct ttm_buffer_object *bo,
			       bool interruptible, bool no_wait)
{
	int ret;

	BUG_ON(!atomic_read(&bo->reserved));
	bo->mem.proposed_flags = bo->proposed_flags;

	TTM_DEBUG("Proposed flags 0x%08lx, Old flags 0x%08lx\n",
		  (unsigned long)bo->mem.proposed_flags,
		  (unsigned long)bo->mem.flags);

	/*
	 * Check whether we need to move buffer.
	 */

	if (!ttm_bo_mem_compat(&bo->mem)) {
		ret = ttm_bo_move_buffer(bo, bo->mem.proposed_flags,
					 interruptible, no_wait);
		if (ret) {
			if (ret != -ERESTART)
				printk(KERN_ERR "Failed moving buffer. "
				       "Proposed placement 0x%08x\n",
				       bo->mem.proposed_flags);
			if (ret == -ENOMEM)
				printk(KERN_ERR "Out of aperture space or "
				       "DRM memory quota.\n");
			return ret;
		}
	}

	/*
	 * We might need to add a TTM.
	 */

	if (bo->mem.mem_type == TTM_PL_SYSTEM && bo->ttm == NULL) {
		ret = ttm_bo_add_ttm(bo);
		if (ret)
			return ret;
	}
	/*
	 * Validation has succeeded, move the access and other
	 * non-mapping-related flag bits from the proposed flags to
	 * the active flags
	 */

	ttm_flag_masked(&bo->mem.flags, bo->proposed_flags,
			~TTM_PL_MASK_MEMTYPE);

	return 0;
}

int
ttm_bo_check_placement(struct ttm_buffer_object *bo,
		       uint32_t set_flags, uint32_t clr_flags)
{
	uint32_t new_mask = set_flags | clr_flags;

	if ((bo->type == ttm_bo_type_user) && (clr_flags & TTM_PL_FLAG_CACHED)) {
		printk(KERN_ERR
		       "User buffers require cache-coherent memory.\n");
		return -EINVAL;
	}

	if (!capable(CAP_SYS_ADMIN)) {
		if (new_mask & TTM_PL_FLAG_NO_EVICT) {
			printk(KERN_ERR "Need to be root to modify"
			       " NO_EVICT status.\n");
			return -EINVAL;
		}

		if ((clr_flags & bo->mem.flags & TTM_PL_MASK_MEMTYPE) &&
		    (bo->mem.flags & TTM_PL_FLAG_NO_EVICT)) {
			printk(KERN_ERR "Incompatible memory specification"
			       " for NO_EVICT buffer.\n");
			return -EINVAL;
		}
	}
	return 0;
}

int ttm_buffer_object_init(struct ttm_bo_device *bdev,
			   struct ttm_buffer_object *bo,
			   unsigned long size,
			   enum ttm_bo_type type,
			   uint32_t flags,
			   uint32_t page_alignment,
			   unsigned long buffer_start,
			   bool interruptible,
			   struct file *persistant_swap_storage,
			   size_t acc_size,
			   void (*destroy) (struct ttm_buffer_object *))
{
	int ret = 0;
	unsigned long num_pages;

	size += buffer_start & ~PAGE_MASK;
	num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (num_pages == 0) {
		printk(KERN_ERR "Illegal buffer object size.\n");
		return -EINVAL;
	}
	bo->destroy = destroy;

	mutex_init(&bo->mutex);
	mutex_lock(&bo->mutex);
	kref_init(&bo->kref);
	kref_init(&bo->list_kref);
	atomic_set(&bo->cpu_writers, 0);
	atomic_set(&bo->reserved, 1);
	init_waitqueue_head(&bo->event_queue);
	INIT_LIST_HEAD(&bo->lru);
	INIT_LIST_HEAD(&bo->ddestroy);
	INIT_LIST_HEAD(&bo->swap);
	bo->bdev = bdev;
	bo->type = type;
	bo->num_pages = num_pages;
	bo->mem.mem_type = TTM_PL_SYSTEM;
	bo->mem.num_pages = bo->num_pages;
	bo->mem.mm_node = NULL;
	bo->mem.page_alignment = page_alignment;
	bo->buffer_start = buffer_start & PAGE_MASK;
	bo->priv_flags = 0;
	bo->mem.flags = (TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED);
	bo->seq_valid = false;
	bo->persistant_swap_storage = persistant_swap_storage;
	bo->acc_size = acc_size;

	ret = ttm_bo_check_placement(bo, flags, 0ULL);
	if (unlikely(ret != 0))
		goto out_err;

	/*
	 * If no caching attributes are set, accept any form of caching.
	 */

	if ((flags & TTM_PL_MASK_CACHING) == 0)
		flags |= TTM_PL_MASK_CACHING;

	bo->proposed_flags = flags;
	bo->mem.proposed_flags = flags;

	/*
	 * For ttm_bo_type_device buffers, allocate
	 * address space from the device.
	 */

	if (bo->type == ttm_bo_type_device) {
		ret = ttm_bo_setup_vm(bo);
		if (ret)
			goto out_err;
	}

	ret = ttm_buffer_object_validate(bo, interruptible, false);
	if (ret)
		goto out_err;

	mutex_unlock(&bo->mutex);
	ttm_bo_unreserve(bo);
	return 0;

      out_err:
	mutex_unlock(&bo->mutex);
	ttm_bo_unreserve(bo);
	ttm_bo_unref(&bo);

	return ret;
}

static inline size_t ttm_bo_size(struct ttm_bo_device *bdev,
				 unsigned long num_pages)
{
	size_t page_array_size = (num_pages * sizeof(void *) + PAGE_SIZE - 1) &
	    PAGE_MASK;

	return bdev->ttm_bo_size + 2 * page_array_size;
}

int ttm_buffer_object_create(struct ttm_bo_device *bdev,
			     unsigned long size,
			     enum ttm_bo_type type,
			     uint32_t flags,
			     uint32_t page_alignment,
			     unsigned long buffer_start,
			     bool interruptible,
			     struct file *persistant_swap_storage,
			     struct ttm_buffer_object **p_bo)
{
	struct ttm_buffer_object *bo;
	int ret;
	struct ttm_mem_global *mem_glob = bdev->mem_glob;

	size_t acc_size =
	    ttm_bo_size(bdev, (size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	ret = ttm_mem_global_alloc(mem_glob, acc_size, false, false, false);
	if (unlikely(ret != 0))
		return ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);

	if (unlikely(bo == NULL)) {
		ttm_mem_global_free(mem_glob, acc_size, false);
		return -ENOMEM;
	}

	ret = ttm_buffer_object_init(bdev, bo, size, type, flags,
				     page_alignment, buffer_start,
				     interruptible,
				     persistant_swap_storage, acc_size, NULL);
	if (likely(ret == 0))
		*p_bo = bo;

	return ret;
}

static int ttm_bo_leave_list(struct ttm_buffer_object *bo,
			     uint32_t mem_type, bool allow_errors)
{
	int ret;

	mutex_lock(&bo->mutex);

	ret = ttm_bo_expire_sync_obj(bo, allow_errors);
	if (ret)
		goto out;

	if (bo->mem.mem_type == mem_type)
		ret = ttm_bo_evict(bo, mem_type, false, false);

	if (ret) {
		if (allow_errors) {
			goto out;
		} else {
			ret = 0;
			printk(KERN_ERR "Cleanup eviction failed\n");
		}
	}

      out:
	mutex_unlock(&bo->mutex);
	return ret;
}

static int ttm_bo_force_list_clean(struct ttm_bo_device *bdev,
				   struct list_head *head,
				   unsigned mem_type, bool allow_errors)
{
	struct ttm_buffer_object *entry;
	int ret;
	int put_count;

	/*
	 * Can't use standard list traversal since we're unlocking.
	 */

	spin_lock(&bdev->lru_lock);

	while (!list_empty(head)) {
		entry = list_first_entry(head, struct ttm_buffer_object, lru);
		kref_get(&entry->list_kref);
		ret = ttm_bo_reserve_locked(entry, false, false, false, 0);
		put_count = ttm_bo_del_from_lru(entry);
		spin_unlock(&bdev->lru_lock);
		while (put_count--)
			kref_put(&entry->list_kref, ttm_bo_ref_bug);
		BUG_ON(ret);
		ret = ttm_bo_leave_list(entry, mem_type, allow_errors);
		ttm_bo_unreserve(entry);
		kref_put(&entry->list_kref, ttm_bo_release_list);
		spin_lock(&bdev->lru_lock);
	}

	spin_unlock(&bdev->lru_lock);

	return 0;
}

int ttm_bo_clean_mm(struct ttm_bo_device *bdev, unsigned mem_type)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];
	int ret = -EINVAL;

	if (mem_type >= TTM_NUM_MEM_TYPES) {
		printk(KERN_ERR "Illegal memory type %d\n", mem_type);
		return ret;
	}

	if (!man->has_type) {
		printk(KERN_ERR "Trying to take down uninitialized "
		       "memory manager type %u\n", mem_type);
		return ret;
	}

	man->use_type = false;
	man->has_type = false;

	ret = 0;
	if (mem_type > 0) {
		ttm_bo_force_list_clean(bdev, &man->lru, mem_type, false);

		spin_lock(&bdev->lru_lock);
		if (drm_mm_clean(&man->manager)) {
			drm_mm_takedown(&man->manager);
		} else {
			ret = -EBUSY;
		}
		spin_unlock(&bdev->lru_lock);
	}

	return ret;
}

int ttm_bo_evict_mm(struct ttm_bo_device *bdev, unsigned mem_type)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];

	if (mem_type == 0 || mem_type >= TTM_NUM_MEM_TYPES) {
		printk(KERN_ERR "Illegal memory manager memory type %u.\n",
		       mem_type);
		return -EINVAL;
	}

	if (!man->has_type) {
		printk(KERN_ERR "Memory type %u has not been initialized.\n",
		       mem_type);
		return 0;
	}

	return ttm_bo_force_list_clean(bdev, &man->lru, mem_type, true);
}

int ttm_bo_init_mm(struct ttm_bo_device *bdev, unsigned type,
		   unsigned long p_offset, unsigned long p_size)
{
	int ret = -EINVAL;
	struct ttm_mem_type_manager *man;

	if (type >= TTM_NUM_MEM_TYPES) {
		printk(KERN_ERR "Illegal memory type %d\n", type);
		return ret;
	}

	man = &bdev->man[type];
	if (man->has_type) {
		printk(KERN_ERR
		       "Memory manager already initialized for type %d\n",
		       type);
		return ret;
	}

	ret = bdev->driver->init_mem_type(bdev, type, man);
	if (ret)
		return ret;

	ret = 0;
	if (type != TTM_PL_SYSTEM) {
		if (!p_size) {
			printk(KERN_ERR "Zero size memory manager type %d\n",
			       type);
			return ret;
		}
		ret = drm_mm_init(&man->manager, p_offset, p_size);
		if (ret)
			return ret;
	}
	man->has_type = true;
	man->use_type = true;
	man->size = p_size;

	INIT_LIST_HEAD(&man->lru);

	return 0;
}

int ttm_bo_device_release(struct ttm_bo_device *bdev)
{
	int ret = 0;
	unsigned i = TTM_NUM_MEM_TYPES;
	struct ttm_mem_type_manager *man;

	while (i--) {
		man = &bdev->man[i];
		if (man->has_type) {
			man->use_type = false;
			if ((i != TTM_PL_SYSTEM) && ttm_bo_clean_mm(bdev, i)) {
				ret = -EBUSY;
				printk(KERN_ERR "DRM memory manager type %d "
				       "is not clean.\n", i);
			}
			man->has_type = false;
		}
	}

	if (!cancel_delayed_work(&bdev->wq))
		flush_scheduled_work();

	while (ttm_bo_delayed_delete(bdev, true)) ;

	spin_lock(&bdev->lru_lock);
	if (list_empty(&bdev->ddestroy))
		TTM_DEBUG("Delayed destroy list was clean\n");

	if (list_empty(&bdev->man[0].lru))
		TTM_DEBUG("Swap list was clean\n");
	spin_unlock(&bdev->lru_lock);

	ttm_mem_unregister_shrink(bdev->mem_glob, &bdev->shrink);
	BUG_ON(!drm_mm_clean(&bdev->addr_space_mm));
	write_lock(&bdev->vm_lock);
	drm_mm_takedown(&bdev->addr_space_mm);
	write_unlock(&bdev->vm_lock);

	__free_page(bdev->dummy_read_page);
	return ret;
}

/*
 * This function is intended to be called on drm driver load.
 * If you decide to call it from firstopen, you must protect the call
 * from a potentially racing ttm_bo_driver_finish in lastclose.
 * (This may happen on X server restart).
 */

int ttm_bo_device_init(struct ttm_bo_device *bdev,
		       struct ttm_mem_global *mem_glob,
		       struct ttm_bo_driver *driver, uint64_t file_page_offset)
{
	int ret = -EINVAL;

	bdev->dummy_read_page = NULL;
	rwlock_init(&bdev->vm_lock);
	spin_lock_init(&bdev->lru_lock);

	bdev->driver = driver;
	bdev->mem_glob = mem_glob;

	memset(bdev->man, 0, sizeof(bdev->man));

	bdev->dummy_read_page = alloc_page(__GFP_ZERO | GFP_DMA32);
	if (unlikely(bdev->dummy_read_page == NULL)) {
		ret = -ENOMEM;
		goto out_err0;
	}

	/*
	 * Initialize the system memory buffer type.
	 * Other types need to be driver / IOCTL initialized.
	 */
	ret = ttm_bo_init_mm(bdev, TTM_PL_SYSTEM, 0, 0);
	if (unlikely(ret != 0))
		goto out_err1;

	bdev->addr_space_rb = RB_ROOT;
	ret = drm_mm_init(&bdev->addr_space_mm, file_page_offset, 0x10000000);
	if (unlikely(ret != 0))
		goto out_err2;

	INIT_DELAYED_WORK(&bdev->wq, ttm_bo_delayed_workqueue);
	bdev->nice_mode = true;
	INIT_LIST_HEAD(&bdev->ddestroy);
	INIT_LIST_HEAD(&bdev->swap_lru);
	bdev->dev_mapping = NULL;
	ttm_mem_init_shrink(&bdev->shrink, ttm_bo_swapout);
	ret = ttm_mem_register_shrink(mem_glob, &bdev->shrink);
	if (unlikely(ret != 0)) {
		printk(KERN_ERR "Could not register buffer object swapout.\n");
		goto out_err2;
	}
	return 0;
      out_err2:
	ttm_bo_clean_mm(bdev, 0);
      out_err1:
	__free_page(bdev->dummy_read_page);
      out_err0:
	return ret;
}

/*
 * buffer object vm functions.
 */

bool ttm_mem_reg_is_pci(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];

	if (!(man->flags & TTM_MEMTYPE_FLAG_FIXED)) {
		if (mem->mem_type == TTM_PL_SYSTEM)
			return false;

		if (man->flags & TTM_MEMTYPE_FLAG_CMA)
			return false;

		if (mem->flags & TTM_PL_FLAG_CACHED)
			return false;
	}
	return true;
}

int ttm_bo_pci_offset(struct ttm_bo_device *bdev,
		      struct ttm_mem_reg *mem,
		      unsigned long *bus_base,
		      unsigned long *bus_offset, unsigned long *bus_size)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];

	*bus_size = 0;
	if (!(man->flags & TTM_MEMTYPE_FLAG_MAPPABLE))
		return -EINVAL;

	if (ttm_mem_reg_is_pci(bdev, mem)) {
		*bus_offset = mem->mm_node->start << PAGE_SHIFT;
		*bus_size = mem->num_pages << PAGE_SHIFT;
		*bus_base = man->io_offset;
	}

	return 0;
}

/**
 * \c Kill all user-space virtual mappings of this buffer object.
 *
 * \param bo The buffer object.
 *
 * Call bo->mutex locked.
 */

void ttm_bo_unmap_virtual(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	loff_t offset = (loff_t) bo->addr_space_offset;
	loff_t holelen = ((loff_t) bo->mem.num_pages) << PAGE_SHIFT;

	if (!bdev->dev_mapping)
		return;

	unmap_mapping_range(bdev->dev_mapping, offset, holelen, 1);
}

static void ttm_bo_vm_insert_rb(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct rb_node **cur = &bdev->addr_space_rb.rb_node;
	struct rb_node *parent = NULL;
	struct ttm_buffer_object *cur_bo;
	unsigned long offset = bo->vm_node->start;
	unsigned long cur_offset;

	while (*cur) {
		parent = *cur;
		cur_bo = rb_entry(parent, struct ttm_buffer_object, vm_rb);
		cur_offset = cur_bo->vm_node->start;
		if (offset < cur_offset)
			cur = &parent->rb_left;
		else if (offset > cur_offset)
			cur = &parent->rb_right;
		else
			BUG();
	}

	rb_link_node(&bo->vm_rb, parent, cur);
	rb_insert_color(&bo->vm_rb, &bdev->addr_space_rb);
}

/**
 * ttm_bo_setup_vm:
 *
 * @bo: the buffer to allocate address space for
 *
 * Allocate address space in the drm device so that applications
 * can mmap the buffer and access the contents. This only
 * applies to ttm_bo_type_device objects as others are not
 * placed in the drm device address space.
 */

static int ttm_bo_setup_vm(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int ret;

      retry_pre_get:
	ret = drm_mm_pre_get(&bdev->addr_space_mm);
	if (unlikely(ret != 0))
		return ret;

	write_lock(&bdev->vm_lock);
	bo->vm_node = drm_mm_search_free(&bdev->addr_space_mm,
					 bo->mem.num_pages, 0, 0);

	if (unlikely(bo->vm_node == NULL)) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	bo->vm_node = drm_mm_get_block_atomic(bo->vm_node,
					      bo->mem.num_pages, 0);

	if (unlikely(bo->vm_node == NULL)) {
		write_unlock(&bdev->vm_lock);
		goto retry_pre_get;
	}

	ttm_bo_vm_insert_rb(bo);
	write_unlock(&bdev->vm_lock);
	bo->addr_space_offset = ((uint64_t) bo->vm_node->start) << PAGE_SHIFT;

	return 0;
      out_unlock:
	write_unlock(&bdev->vm_lock);
	return ret;
}

int ttm_bo_wait(struct ttm_buffer_object *bo,
		bool lazy, bool interruptible, bool no_wait)
{
	struct ttm_bo_driver *driver = bo->bdev->driver;
	void *sync_obj;
	void *sync_obj_arg;
	int ret = 0;

	while (bo->sync_obj) {
		if (driver->sync_obj_signaled(bo->sync_obj, bo->sync_obj_arg)) {
			driver->sync_obj_unref(&bo->sync_obj);
			bo->priv_flags &= ~TTM_BO_PRIV_FLAG_MOVING;
			goto out;
		}
		if (no_wait) {
			ret = -EBUSY;
			goto out;
		}
		sync_obj = driver->sync_obj_ref(bo->sync_obj);
		sync_obj_arg = bo->sync_obj_arg;
		mutex_unlock(&bo->mutex);
		ret = driver->sync_obj_wait(sync_obj, sync_obj_arg,
					    lazy, interruptible);

		mutex_lock(&bo->mutex);
		if (unlikely(ret != 0)) {
			driver->sync_obj_unref(&sync_obj);
			return ret;
		}

		if (bo->sync_obj == sync_obj) {
			driver->sync_obj_unref(&bo->sync_obj);
			bo->priv_flags &= ~TTM_BO_PRIV_FLAG_MOVING;
		}
		driver->sync_obj_unref(&sync_obj);
	}
      out:
	return 0;
}

void ttm_bo_unblock_reservation(struct ttm_buffer_object *bo)
{
	atomic_set(&bo->reserved, 0);
	wake_up_all(&bo->event_queue);
}

int ttm_bo_block_reservation(struct ttm_buffer_object *bo, bool interruptible,
			     bool no_wait)
{
	int ret;

	while (unlikely(atomic_cmpxchg(&bo->reserved, 0, 1) != 0)) {
		if (no_wait)
			return -EBUSY;
		else if (interruptible) {
			ret = wait_event_interruptible
			    (bo->event_queue, atomic_read(&bo->reserved) == 0);
			if (unlikely(ret != 0))
				return -ERESTART;
		} else {
			wait_event(bo->event_queue,
				   atomic_read(&bo->reserved) == 0);
		}
	}
	return 0;
}

int ttm_bo_synccpu_write_grab(struct ttm_buffer_object *bo, bool no_wait)
{
	int ret = 0;

	/*
	 * Using ttm_bo_reserve instead of ttm_bo_block_reservation
	 * makes sure the lru lists are updated.
	 */

	ret = ttm_bo_reserve(bo, true, no_wait, false, 0);
	if (unlikely(ret != 0))
		return ret;
	mutex_lock(&bo->mutex);
	ret = ttm_bo_wait(bo, false, true, no_wait);
	if (unlikely(ret != 0))
		goto out_err0;
	atomic_inc(&bo->cpu_writers);
      out_err0:
	mutex_unlock(&bo->mutex);
	ttm_bo_unreserve(bo);
	return ret;
}

void ttm_bo_synccpu_write_release(struct ttm_buffer_object *bo)
{
	if (atomic_dec_and_test(&bo->cpu_writers))
		wake_up_all(&bo->event_queue);
}

/**
 * A buffer object shrink method that tries to swap out the first
 * buffer object on the bo_global::swap_lru list.
 */

static int ttm_bo_swapout(struct ttm_mem_shrink *shrink)
{
	struct ttm_bo_device *bdev =
	    container_of(shrink, struct ttm_bo_device, shrink);
	struct ttm_buffer_object *bo;
	int ret = -EBUSY;
	int put_count;
	uint32_t swap_placement = (TTM_PL_FLAG_CACHED | TTM_PL_FLAG_SYSTEM);

	spin_lock(&bdev->lru_lock);
	while (ret == -EBUSY) {
		if (unlikely(list_empty(&bdev->swap_lru))) {
			spin_unlock(&bdev->lru_lock);
			return -EBUSY;
		}

		bo = list_first_entry(&bdev->swap_lru,
				      struct ttm_buffer_object, swap);
		kref_get(&bo->list_kref);

		/**
		 * Reserve buffer. Since we unlock while sleeping, we need
		 * to re-check that nobody removed us from the swap-list while
		 * we slept.
		 */

		ret = ttm_bo_reserve_locked(bo, false, true, false, 0);
		if (unlikely(ret == -EBUSY)) {
			spin_unlock(&bdev->lru_lock);
			ttm_bo_wait_unreserved(bo, false);
			kref_put(&bo->list_kref, ttm_bo_release_list);
			spin_lock(&bdev->lru_lock);
		}
	}

	BUG_ON(ret != 0);
	put_count = ttm_bo_del_from_lru(bo);
	spin_unlock(&bdev->lru_lock);

	while (put_count--)
		kref_put(&bo->list_kref, ttm_bo_ref_bug);

	/**
	 * Wait for GPU, then move to system cached.
	 */

	mutex_lock(&bo->mutex);
	ret = ttm_bo_wait(bo, false, false, false);
	if (unlikely(ret != 0))
		goto out;

	if ((bo->mem.flags & swap_placement) != swap_placement) {
		struct ttm_mem_reg evict_mem;

		evict_mem = bo->mem;
		evict_mem.mm_node = NULL;
		evict_mem.proposed_flags =
		    TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED;
		evict_mem.flags = TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED;
		evict_mem.mem_type = TTM_PL_SYSTEM;

		ret = ttm_bo_handle_move_mem(bo, &evict_mem, true, false, false);
		if (unlikely(ret != 0))
			goto out;
	}

	ttm_bo_unmap_virtual(bo);

	/**
	 * Swap out. Buffer will be swapped in again as soon as
	 * anyone tries to access a ttm page.
	 */

	ret = ttm_tt_swapout(bo->ttm, bo->persistant_swap_storage);
      out:
	mutex_unlock(&bo->mutex);

	/**
	 *
	 * Unreserve without putting on LRU to avoid swapping out an
	 * already swapped buffer.
	 */

	atomic_set(&bo->reserved, 0);
	wake_up_all(&bo->event_queue);
	kref_put(&bo->list_kref, ttm_bo_release_list);
	return ret;
}

void ttm_bo_swapout_all(struct ttm_bo_device *bdev)
{
	while (ttm_bo_swapout(&bdev->shrink) == 0) ;
}
