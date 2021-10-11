/**********************************************************************************
* Copyright (c), 2008-2019 , Guangdong OPPO Mobile Comm Corp., Ltd.
* VENDOR_EDIT
* File: oppo_cfs_mutex.c
* Description: UI First
* Version: 2.0
* Date: 2019-10-01
* Author: Liujie.Xie@TECH.BSP.Kernel.Sched
* ------------------------------ Revision History: --------------------------------
* <version>           <date>                <author>                            <desc>
* Revision 1.0        2019-05-22       Liujie.Xie@TECH.BSP.Kernel.Sched      Created for UI First
* Revision 2.0        2019-10-01       Liujie.Xie@TECH.BSP.Kernel.Sched      Add for UI First 2.0
***********************************************************************************/

#include <linux/version.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "oppo_cfs_common.h"

#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
static void mutex_list_add_ux(struct list_head *entry, struct list_head *head)
{
	struct list_head *pos = NULL;
	struct list_head *n = NULL;
	struct mutex_waiter *waiter = NULL;
	list_for_each_safe(pos, n, head) {
		waiter = list_entry(pos, struct mutex_waiter, list);
		if (!test_task_ux(waiter->task)) {
			list_add(entry, waiter->list.prev);
			return;
		}
	}
	if (pos == head) {
		list_add_tail(entry, head);
	}
}

void mutex_list_add(struct task_struct *task, struct list_head *entry, struct list_head *head, struct mutex *lock)
{
	bool is_ux = test_task_ux(task);
	if (!entry || !head || !lock) {
		return;
	}
	if (is_ux && !lock->ux_dep_task) {
		mutex_list_add_ux(entry, head);
	} else {
		list_add_tail(entry, head);
	}
}

void mutex_dynamic_ux_enqueue(struct mutex *lock, struct task_struct *task)
{
	bool is_ux = false;
	struct task_struct *owner = NULL;
	if (!lock) {
		return;
	}
	is_ux = test_set_dynamic_ux(task);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	owner = __mutex_owner(lock);
#else
	owner = lock->owner;
#endif
	if (is_ux && !lock->ux_dep_task && owner && !test_task_ux(owner)) {
		dynamic_ux_enqueue(owner, DYNAMIC_UX_MUTEX, task->ux_depth);
		lock->ux_dep_task = owner;
	}
}

void mutex_dynamic_ux_dequeue(struct mutex *lock, struct task_struct *task)
{
	if (lock && lock->ux_dep_task == task) {
		dynamic_ux_dequeue(task, DYNAMIC_UX_MUTEX);
		lock->ux_dep_task = NULL;
	}
}
#endif /* CONFIG_OPLUS_SYSTEM_KERNEL_QCOM */
