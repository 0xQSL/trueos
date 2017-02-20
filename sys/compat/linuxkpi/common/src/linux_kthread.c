#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/compat.h>

#include <sys/bus.h>
#include <sys/interrupt.h>
#include <sys/priority.h>
#include <sys/sched.h>

enum KTHREAD_BITS {
	KTHREAD_IS_PER_CPU = 0,
	KTHREAD_SHOULD_STOP,
	KTHREAD_SHOULD_PARK,
	KTHREAD_IS_PARKED,
};

#define to_kthread(t) (&(t)->kthread)

bool
kthread_should_stop(void)
{
	return (test_bit(KTHREAD_SHOULD_STOP, &current->kthread.flags));
}

bool
kthread_should_park(void)
{
	return (test_bit(KTHREAD_SHOULD_PARK, &current->kthread.flags));
}

int
kthread_park(struct task_struct *k)
{
	struct kthread *kthread = &k->kthread;
	int ret = -ENOSYS;

/* XXX we don't know the thread is live */
	if (kthread) {
		if (!test_bit(KTHREAD_IS_PARKED, &kthread->flags)) {
			set_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
			if (k != current) {
				wake_up_process(k);
				wait_for_completion(&kthread->parked);
			}
		}
		ret = 0;
	}
	return ret;
}

static void
__kthread_unpark(struct task_struct *k, struct kthread *kthread)
{
	clear_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
	if (test_and_clear_bit(KTHREAD_IS_PARKED, &kthread->flags))
		wake_up_state(k, TASK_PARKED);
}

void
kthread_unpark(struct task_struct *k)
{
	__kthread_unpark(k, &k->kthread);
}

static void
__kthread_parkme(struct kthread *self)
{	
	__set_current_state(TASK_PARKED);
	while (test_bit(KTHREAD_SHOULD_PARK, &self->flags)) {
		if (!test_and_set_bit(KTHREAD_IS_PARKED, &self->flags))
			complete(&self->parked);
		schedule();
		__set_current_state(TASK_PARKED);
	}
	clear_bit(KTHREAD_IS_PARKED, &self->flags);
	__set_current_state(TASK_RUNNING);
}


void
kthread_parkme(void)
{
	__kthread_parkme(to_kthread(current));
}

int
kthread_stop(struct task_struct *task)	
{
	struct thread *td;
	struct kthread *k = &task->kthread;
	int retval = 0;

	/* XXX we don't know the thread is live */
	td = task->task_thread;
	PROC_LOCK(td->td_proc);
	set_bit(KTHREAD_SHOULD_STOP, &task->kthread.flags);
	PROC_UNLOCK(td->td_proc);
	__kthread_unpark(task, k);
	wake_up_process(task);
	wait_for_completion(&k->exited);

	retval = task->task_ret;
	linux_free_current(task);

	return (retval);
}

struct task_struct *
linux_kthread_setup_and_run(struct thread *td, linux_task_fn_t *task_fn, void *arg)
{
	struct task_struct *task;

	linux_set_current(td);

	task = td->td_lkpi_task;
	task->task_fn = task_fn;
	task->task_data = arg;

	/* make sure the scheduler priority is raised */
	thread_lock(td);
	sched_prio(td, PI_SWI(SWI_NET));
	sched_add(td, SRQ_BORING);
	thread_unlock(td);

	return (task);
}

void
linux_kthread_fn(void *arg __unused)
{
	struct task_struct *task;
	struct thread *td;

	td = curthread;
	task = current;

	if (kthread_should_stop())
		goto skip;

	task->task_ret = task->task_fn(task->task_data);

	PROC_LOCK(td->td_proc);
	wakeup(task);
	PROC_UNLOCK(td->td_proc);

	if (kthread_should_stop()) {
skip:
		/* let kthread_stop() free data */
		td->td_lkpi_task = NULL;
		/* wakeup kthread_stop() */
		complete(&task->kthread.exited);
	}
	kthread_exit();
}
