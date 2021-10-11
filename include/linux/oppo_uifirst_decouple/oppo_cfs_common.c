/**********************************************************************************
* Copyright (c), 2008-2019 , Guangdong OPPO Mobile Comm Corp., Ltd.
* VENDOR_EDIT
* File: oppo_cfs_common.c
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
#include <linux/jiffies.h>
#include <trace/events/sched.h>
#include <../kernel/sched/sched.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <../fs/proc/internal.h>

#include "oppo_cfs_common.h"

#ifndef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
extern unsigned long cpu_util(int cpu);
#endif

int ux_min_sched_delay_granularity;     /*ux thread delay upper bound(ms)*/
int ux_max_dynamic_exist = 1000;        /*ux dynamic max exist time(ms)*/
int ux_max_dynamic_granularity = 32;
int ux_min_migration_delay = 10;        /*ux min migration delay time(ms)*/
int ux_max_over_thresh = 2000; /* ms */
#define S2NS_T 1000000
#define CAMERA_PROVIDER_NAME "provider@2.4-se"

static int entity_before(struct sched_entity *a,
			struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) < 0;
}

static int entity_over(struct sched_entity *a,
			struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) > (s64)ux_max_over_thresh * S2NS_T;
}

extern const struct sched_class fair_sched_class;
/* return true: this task can be treated as a ux task, which means it will be sched first and so on */
inline bool test_task_ux(struct task_struct *task)
{
	u64 now = 0;

	if (!sysctl_uifirst_enabled)
		return false;

	if (task && task->sched_class != &fair_sched_class)
		return false;

	if (task && task->static_ux)
		return true;

	now = jiffies_to_nsecs(jiffies);
	if (task && atomic64_read(&task->dynamic_ux) &&
		(now - task->dynamic_ux_start) <= (u64)ux_max_dynamic_granularity * S2NS_T)
		return true;

	return false;
}

void enqueue_ux_thread(struct rq *rq, struct task_struct *p)
{
	struct list_head *pos, *n;
	bool exist = false;

	if (!rq || !p || !list_empty(&p->ux_entry)) {
		return;
	}
	p->enqueue_time = rq->clock;
	if (test_task_ux(p)) {
		list_for_each_safe(pos, n, &rq->ux_thread_list) {
			if (pos == &p->ux_entry) {
				exist = true;
				break;
			}
		}
		if (!exist) {
			list_add_tail(&p->ux_entry, &rq->ux_thread_list);
			get_task_struct(p);
		}
	}
}

void dequeue_ux_thread(struct rq *rq, struct task_struct *p)
{
	struct list_head *pos, *n;
	u64 now =  jiffies_to_nsecs(jiffies);

	if (!rq || !p) {
		return;
	}
	p->enqueue_time = 0;
	if (!list_empty(&p->ux_entry)) {
		if (atomic64_read(&p->dynamic_ux) && (now - p->dynamic_ux_start) > (u64)ux_max_dynamic_exist * S2NS_T) {
			atomic64_set(&p->dynamic_ux, 0);
		}
		list_for_each_safe(pos, n, &rq->ux_thread_list) {
			if (pos == &p->ux_entry) {
				list_del_init(&p->ux_entry);
				put_task_struct(p);
				return;
			}
		}
	}
}

static struct task_struct *pick_first_ux_thread(struct rq *rq)
{
	struct list_head *ux_thread_list = &rq->ux_thread_list;
	struct list_head *pos = NULL;
	struct list_head *n = NULL;
	struct task_struct *temp = NULL;
	struct task_struct *leftmost_task = NULL;
	list_for_each_safe(pos, n, ux_thread_list) {
		temp = list_entry(pos, struct task_struct, ux_entry);
		/*ensure ux task in current rq cpu otherwise delete it*/
		if (unlikely(task_cpu(temp) != rq->cpu)) {
			//ux_warn("task(%s,%d,%d) does not belong to cpu%d", temp->comm, task_cpu(temp), temp->policy, rq->cpu);
			list_del_init(&temp->ux_entry);
			continue;
		}
		if (!test_task_ux(temp)) {
			continue;
		}
		if (leftmost_task == NULL) {
			leftmost_task = temp;
		} else if (entity_before(&temp->se, &leftmost_task->se)) {
			leftmost_task = temp;
		}
	}

	return leftmost_task;
}

void pick_ux_thread(struct rq *rq, struct task_struct **p, struct sched_entity **se)
{
	struct task_struct *ori_p;
	struct task_struct *key_task;
	struct sched_entity *key_se;
	if (!rq || !p || !se) {
		return;
	}
	ori_p = *p;
	if (ori_p && !ori_p->static_ux && !atomic64_read(&ori_p->dynamic_ux)) {
		if (!list_empty(&rq->ux_thread_list)) {
			key_task = pick_first_ux_thread(rq);
			/* in case that ux thread keep running too long */
			if (key_task && entity_over(&key_task->se, &ori_p->se))
				return;

			if (key_task) {
				key_se = &key_task->se;
				if (key_se && (rq->clock >= key_task->enqueue_time) &&
				rq->clock - key_task->enqueue_time >= ((u64)ux_min_sched_delay_granularity * S2NS_T)) {
					*p = key_task;
					*se = key_se;
				}
			}
		}
	}
}

#define DYNAMIC_UX_SEC_WIDTH   8
#define DYNAMIC_UX_MASK_BASE   0x00000000ff

#define dynamic_ux_offset_of(type) (type * DYNAMIC_UX_SEC_WIDTH)
#define dynamic_ux_mask_of(type) ((u64)(DYNAMIC_UX_MASK_BASE) << (dynamic_ux_offset_of(type)))
#define dynamic_ux_get_bits(value, type) ((value & dynamic_ux_mask_of(type)) >> dynamic_ux_offset_of(type))
#define dynamic_ux_value(type, value) ((u64)value << dynamic_ux_offset_of(type))


bool test_dynamic_ux(struct task_struct *task, int type)
{
	u64 dynamic_ux;
	if (!task) {
		return false;
	}
	dynamic_ux = atomic64_read(&task->dynamic_ux);
	return dynamic_ux_get_bits(dynamic_ux, type) > 0;
}

static bool test_task_exist(struct task_struct *task, struct list_head *head)
{
	struct list_head *pos, *n;
	list_for_each_safe(pos, n, head) {
		if (pos == &task->ux_entry) {
			return true;
		}
	}
	return false;
}

inline void dynamic_ux_inc(struct task_struct *task, int type)
{
	atomic64_add(dynamic_ux_value(type, 1), &task->dynamic_ux);
}

inline void dynamic_ux_sub(struct task_struct *task, int type, int value)
{
	atomic64_sub(dynamic_ux_value(type, value), &task->dynamic_ux);
}

static void __dynamic_ux_dequeue(struct task_struct *task, int type, int value)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0)
	unsigned long flags;
#else
	struct rq_flags flags;
#endif
	bool exist = false;
	struct rq *rq = NULL;
	u64 dynamic_ux = 0;

	rq = task_rq_lock(task, &flags);
	dynamic_ux = atomic64_read(&task->dynamic_ux);
	if (dynamic_ux <= 0) {
		task_rq_unlock(rq, task, &flags);
		return;
	}
	dynamic_ux_sub(task, type, value);
	dynamic_ux = atomic64_read(&task->dynamic_ux);
	if (dynamic_ux > 0) {
		task_rq_unlock(rq, task, &flags);
		return;
	}
	task->ux_depth = 0;

	exist = test_task_exist(task, &rq->ux_thread_list);
	if (exist) {
		list_del_init(&task->ux_entry);
		put_task_struct(task);
	}
	task_rq_unlock(rq, task, &flags);
}

void dynamic_ux_dequeue(struct task_struct *task, int type)
{
	if (!task || type >= DYNAMIC_UX_MAX) {
		return;
	}
	__dynamic_ux_dequeue(task, type, 1);
}
void dynamic_ux_dequeue_refs(struct task_struct *task, int type, int value)
{
	if (!task || type >= DYNAMIC_UX_MAX) {
		return;
	}
	__dynamic_ux_dequeue(task, type, value);
}

static void __dynamic_ux_enqueue(struct task_struct *task, int type, int depth)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0)
	unsigned long flags;
#else
	struct rq_flags flags;
#endif
	bool exist = false;
	struct rq *rq = NULL;

	rq = task_rq_lock(task, &flags);
/*
	if (task->sched_class != &fair_sched_class) {
		task_rq_unlock(rq, task, &flags);
		return;
	}
*/
	if (unlikely(!list_empty(&task->ux_entry))) {
		//ux_warn("task(%s,%d,%d) is already in another list", task->comm, task->pid, task->policy);
		task_rq_unlock(rq, task, &flags);
		return;
	}

	dynamic_ux_inc(task, type);
	task->dynamic_ux_start = jiffies_to_nsecs(jiffies);
	task->ux_depth = task->ux_depth > depth + 1 ? task->ux_depth : depth + 1;
	if (task->state == TASK_RUNNING && task->sched_class == &fair_sched_class) {
		exist = test_task_exist(task, &rq->ux_thread_list);
		if (!exist) {
			get_task_struct(task);
			list_add_tail(&task->ux_entry, &rq->ux_thread_list);
		}
	}
	task_rq_unlock(rq, task, &flags);
}

void dynamic_ux_enqueue(struct task_struct *task, int type, int depth)
{
	if (!task || type >= DYNAMIC_UX_MAX) {
		return;
	}
	__dynamic_ux_enqueue(task, type, depth);
}

inline bool test_task_ux_depth(int ux_depth)
{
	return ux_depth < UX_DEPTH_MAX;
}

inline bool test_set_dynamic_ux(struct task_struct *tsk)
{
	return tsk && (tsk->static_ux || atomic64_read(&tsk->dynamic_ux)) &&
		test_task_ux_depth(tsk->ux_depth);
}

void ux_init_rq_data(struct rq *rq)
{
	if (!rq) {
		return;
	}
	rq->active_ux_balance = 0;
	INIT_LIST_HEAD(&rq->ux_thread_list);
}

int ux_prefer_cpu[NR_CPUS] = {0};
void ux_init_cpu_data(void) {
	int i = 0;
	int min_cpu = 0, ux_cpu = 0;

	for (; i < NR_CPUS; ++i) {
		ux_prefer_cpu[i] = -1;
	}

	ux_cpu = cpumask_weight(topology_core_cpumask(min_cpu));
	if (ux_cpu == 0) {
		ux_warn("failed to init ux cpu data\n");
		return;
	}

	for (i = 0; i < NR_CPUS && ux_cpu < NR_CPUS; ++i) {
		ux_prefer_cpu[i] = ux_cpu++;
	}
}

bool test_ux_task_cpu(int cpu) {
	return (cpu >= ux_prefer_cpu[0]);
}

bool test_ux_prefer_cpu(struct task_struct *tsk, int cpu) {
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;

	if (tsk->pid == tsk->tgid) {
#ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
		return cpu >= rd->max_cap_orig_cpu;
#else
		return capacity_orig_of(cpu) >= rd->max_cpu_capacity.val;
#endif /*CONFIG_OPLUS_SYSTEM_KERNEL_QCOM*/
	}

	return (cpu >= ux_prefer_cpu[0]);
}

void find_ux_task_cpu(struct task_struct *tsk, int *target_cpu) {
	int i = 0;
	int temp_cpu = 0;
	struct rq *rq = NULL;

	for (i = (NR_CPUS - 1); i >= 0; --i) {
		temp_cpu = ux_prefer_cpu[i];
		if (temp_cpu <= 0 || temp_cpu >= NR_CPUS)
			continue;

		rq = cpu_rq(temp_cpu);
		if (!rq)
			continue;

		if (rq->curr->prio <= MAX_RT_PRIO)
			continue;

		if (!test_task_ux(rq->curr) && cpu_online(temp_cpu) && !cpu_isolated(temp_cpu)
			&& cpumask_test_cpu(temp_cpu, &tsk->cpus_allowed)) {
			*target_cpu = temp_cpu;
			return;
		}
	}
	return;
}

/*
 * taget cpu is
 *  unboost: the one in all domain, with lowest prio running task
 *  boost:   the one in power domain, with lowest prio running task which is not ux
*/
int set_ux_task_cpu_common_by_prio(struct task_struct *task, int *target_cpu, bool boost, bool prefer_idle)
{
	int i;
	int lowest_prio = INT_MIN;
	unsigned long lowest_prio_max_cap = 0;
	int ret = -1;

	for_each_cpu(i, cpu_active_mask) {
		unsigned long capacity_curr;
		struct task_struct *curr;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
		if (!cpumask_test_cpu(i, task->cpus_ptr) || cpu_isolated(i))
#else
		if (!cpumask_test_cpu(i, &task->cpus_allowed) || cpu_isolated(i))
#endif
			continue;

		if (prefer_idle && idle_cpu(i)) {
			*target_cpu = i;
			return 0;
		}

		capacity_curr = capacity_curr_of(i);
		curr = cpu_rq(i)->curr;

		if (!curr || !(curr->sched_class == &fair_sched_class))
			continue;

		if ((curr->prio > lowest_prio) || (boost && !test_task_ux(curr) && capacity_curr > lowest_prio_max_cap)) {
			lowest_prio = curr->prio;
			lowest_prio_max_cap = capacity_curr;
			*target_cpu = i;
			ret = 0;
		}
	}

	return ret;
}

#define MAX_LIMIT_PROCESS (5)
#define ULIMIT_PROCESS_TYPE (3)
#define CAMERA_MAIN_THREAD "com.oppo.camera"
#define CAMERA_SERVER_NAME "cameraserver"
#define CAMERA_PROVIDER_NAME "provider@2.4-se"
int sysctl_animation_type = 0;

bool is_limit_task(struct task_struct *task)
{
    bool ret = false;
    struct task_struct *grp_leader = task->group_leader;

    if (sysctl_animation_type != ULIMIT_PROCESS_TYPE)
        return false;

        if (grp_leader) {
        char *process_comm = grp_leader->comm;
        if (strstr(process_comm, CAMERA_MAIN_THREAD) ||
            strstr(process_comm, CAMERA_SERVER_NAME) ||
            strstr(process_comm, CAMERA_PROVIDER_NAME)
        )
            ret = true;
    }

    return ret;
}

bool set_limit_core_use_target_cpu(struct task_struct *task, int *target_cpu)
{
	bool res = false;

	if (should_limit_core_use(task)) {
		int i = 0;
		int core_limit = get_limit_core_use();
		unsigned int best_nr = UINT_MAX;
		struct cpumask allowed_mask;
		cpumask_clear(&allowed_mask);

		if (core_limit <= 0 || core_limit >= NR_CPUS)
			return false;

		for (i = 0; i < core_limit; ++i)
			cpumask_set_cpu(i, &allowed_mask);

		if (cpumask_test_cpu(*target_cpu, &allowed_mask))
			return true;

		for_each_cpu_and(i, &allowed_mask, &task->cpus_allowed) {
			if (!cpu_online(i) || cpu_isolated(i))
				continue;

			if (cpu_rq(i)->nr_running < best_nr) {
				best_nr = cpu_rq(i)->nr_running;
				*target_cpu = i;
				res = true;
			}
		}
	}

	return res;
}

#ifdef CONFIG_CAMERA_OPT
inline void set_camera_opt( struct task_struct *p)
{
	struct task_struct *grp_leader = p->group_leader;
	if (strstr(grp_leader->comm, CAMERA_PROVIDER_NAME) && strstr(p->comm, CAMERA_PROVIDER_NAME)) {
		p->camera_opt = 1;
		return ;
	}
}
#else
inline void set_camera_opt( struct task_struct *p)
{
	return;
}
#endif

void drop_ux_task_cpus(struct task_struct *p, struct cpumask *lowest_mask)
{
	unsigned int cpu = cpumask_first(lowest_mask);

	while (cpu < nr_cpu_ids) {
		/* unlocked access */
		struct task_struct *task = READ_ONCE(cpu_rq(cpu)->curr);

		if (task->static_ux == 1) {
			cpumask_clear_cpu(cpu, lowest_mask);
		}

		cpu = cpumask_next(cpu, lowest_mask);
	}
}

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static inline u64 max_vruntime(u64 max_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - max_vruntime);
	if (delta > 0)
		max_vruntime = vruntime;

	return max_vruntime;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/* An entity is a task if it doesn't "own" a runqueue */
#define oplus_entity_is_task(se)	(!se->my_q)
#else
#define oplus_entity_is_task(se)	(1)
#endif

void place_entity_adjust_ux_task(struct cfs_rq *cfs_rq, struct sched_entity *se, int initial)
{
	u64 vruntime = cfs_rq->min_vruntime;
	unsigned long thresh = sysctl_sched_latency;
	struct task_struct *se_task = NULL;

	if (unlikely(!sysctl_uifirst_enabled))
		return;

	if (!oplus_entity_is_task(se) || initial)
		return;

	se_task = task_of(se);
#ifdef CONFIG_CAMERA_OPT
	if (se_task->static_ux == 1 || (se_task->camera_opt == 1 && sysctl_camera_opt_enabled)) {
#else
        if (se_task->static_ux == 1) {
#endif
                vruntime -= 2 * thresh;
                se->vruntime = vruntime;
                return;
        }
/*
	if (!test_ux_task_type(se_task, UIFIRST_TYPE_LIGHT | UIFIRST_TYPE_HEAVY | UIFIRST_TYPE_ANIMATOR))
		return;

	if (test_ux_task_type(se_task, UIFIRST_TYPE_ANIMATOR)) {
		vruntime -= 2 * thresh;
		se->vruntime = vruntime;
		trace_oppo_tp_sched_change_ux(1, se_task->pid);
		return;
	}

	if (test_ux_task_type(se_task, UIFIRST_TYPE_LIGHT | UIFIRST_TYPE_HEAVY)) {
		vruntime -= sysctl_launcher_boost_enabled ? thresh : (thresh >> 1);
		se->vruntime = vruntime;
		trace_oppo_tp_sched_change_ux(2, se_task->pid);
		return;
	}
*/
}

bool should_ux_preempt_wakeup(struct task_struct *wake_task, struct task_struct *curr_task)
{
	bool wake_ux = false;
	bool curr_ux = false;

	if (!sysctl_uifirst_enabled)
		return false;

	wake_ux = test_task_ux(wake_task);
	curr_ux = test_task_ux(curr_task);

	/* ux can preemt cfs */
	if (wake_ux && !curr_ux)
		return true;

	/* animator ux can preemt un-animator */
	if ((wake_task->static_ux == 1) && (curr_task->static_ux != 1))
		return true;

	return false;
}

void kick_min_cpu_from_mask(struct cpumask *lowest_mask)
{
	unsigned int cpu = cpumask_first(lowest_mask);
	while(cpu < nr_cpu_ids) {
		if (cpu < ux_prefer_cpu[0]) {
			cpumask_clear_cpu(cpu, lowest_mask);
		}
		cpu = cpumask_next(cpu, lowest_mask);
	}
}
bool ux_skip_sync_wakeup(struct task_struct *task, int *sync)
{
	bool ret = false;

	if (test_dynamic_ux(task, DYNAMIC_UX_BINDER)) {
		*sync = 0;
		ret = true;
	}

	return ret;
}

/*
 * add for create proc node: proc/pid/task/pid/static_ux
*/
bool is_special_entry(struct dentry *dentry, const char* special_proc)
{
	const unsigned char *name;
	if (NULL == dentry || NULL == special_proc)
		return false;

	name = dentry->d_name.name;
	if (NULL != name && !strncmp(special_proc, name, 32))
		return true;
	else
		return false;
}

static int proc_static_ux_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;
	p = get_proc_task(inode);
	if (!p) {
		return -ESRCH;
	}
	task_lock(p);
	seq_printf(m, "%d\n", p->static_ux);
	task_unlock(p);
	put_task_struct(p);
	return 0;
}

static int proc_static_ux_open(struct inode* inode, struct file *filp)
{
	return single_open(filp, proc_static_ux_show, inode);
}

static ssize_t proc_static_ux_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct task_struct *task;
	char buffer[PROC_NUMBUF];
	int err, static_ux;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count)) {
		return -EFAULT;
	}

	err = kstrtoint(strstrip(buffer), 0, &static_ux);
	if(err) {
		return err;
	}
	task = get_proc_task(file_inode(file));
	if (!task) {
		return -ESRCH;
	}

	task->static_ux = static_ux;

	put_task_struct(task);
	return count;
}

#ifdef CONFIG_CAMERA_OPT
static int proc_camera_opt_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;
	p = get_proc_task(inode);
	if (!p) {
		return -ESRCH;
	}
	task_lock(p);
	seq_printf(m, "%d\n", p->camera_opt);
	task_unlock(p);
	put_task_struct(p);
	return 0;
}

static int proc_camera_opt_open(struct inode* inode, struct file *filp)
{
	return single_open(filp, proc_camera_opt_show, inode);
}

static ssize_t proc_camera_opt_read(struct file* file, char __user *buf,
		                size_t count, loff_t *ppos)
{
	char buffer[128];
	struct task_struct *task = NULL;
	int camera_opt = -1;
	size_t len = 0;
	task = get_proc_task(file_inode(file));
	if (!task) {
		return -ESRCH;
	}
	camera_opt = task->camera_opt;
	put_task_struct(task);
	len = snprintf(buffer, sizeof(buffer), "is_opt:%d\n", camera_opt);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

const struct file_operations proc_camera_opt_operations = {
	.open	        = proc_camera_opt_open,
	.read           = proc_camera_opt_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};
#endif

static ssize_t proc_static_ux_read(struct file* file, char __user *buf,
							    size_t count, loff_t *ppos)
{
	char buffer[128];
	struct task_struct *task = NULL;
	int static_ux = -1;
	size_t len = 0;
	task = get_proc_task(file_inode(file));
	if (!task) {
		return -ESRCH;
	}
	static_ux = task->static_ux;
	put_task_struct(task);

	len = snprintf(buffer, sizeof(buffer), "static=%d dynamic=%d(bi:%d fu:%d rw:%d mu:%d)\n",
		static_ux, atomic64_read(&task->dynamic_ux),
		test_dynamic_ux(task, DYNAMIC_UX_BINDER), test_dynamic_ux(task, DYNAMIC_UX_FUTEX),
		test_dynamic_ux(task, DYNAMIC_UX_RWSEM), test_dynamic_ux(task, DYNAMIC_UX_MUTEX));

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

const struct file_operations proc_static_ux_operations = {
	.open		= proc_static_ux_open,
	.write		= proc_static_ux_write,
	.read		= proc_static_ux_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
