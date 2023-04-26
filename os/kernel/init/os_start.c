/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * kernel/init/os_start.c
 *
 *   Copyright (C) 2007-2014 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include  <sys/types.h>
#include  <stdbool.h>
#include  <string.h>
#include  <assert.h>
#include  <debug.h>

#include  <tinyara/arch.h>
#include  <tinyara/binfmt/binfmt.h>
#include  <tinyara/compiler.h>
#include  <tinyara/sched.h>
#include  <tinyara/fs/fs.h>
#include  <tinyara/net/net.h>
#include  <tinyara/lib.h>
#include  <tinyara/mm/mm.h>
#include  <tinyara/mm/shm.h>
#include  <tinyara/kmalloc.h>
#include  <tinyara/init.h>
#include  <tinyara/pm/pm.h>
#include  <tinyara/mm/heap_regioninfo.h>
#ifdef CONFIG_DEBUG_SYSTEM
#include  <tinyara/debug/sysdbg.h>
#endif
#ifdef CONFIG_DRIVERS_OS_API_TEST
#include  <tinyara/os_api_test_drv.h>
#endif
#if defined(CONFIG_APP_BINARY_SEPARATION) && defined(CONFIG_ARCH_USE_MMU)
#include <tinyara/mmu.h>
#endif
#include <tinyara/sched_note.h>

#include  "sched/sched.h"
#include  "signal/signal.h"
#include  "wdog/wdog.h"
#include  "semaphore/semaphore.h"
#ifndef CONFIG_DISABLE_MQUEUE
#include  "mqueue/mqueue.h"
#endif
#ifndef CONFIG_DISABLE_PTHREAD
#include  "pthread/pthread.h"
#endif
#include  "clock/clock.h"
#include  "timer/timer.h"
#include  "irq/irq.h"
#ifdef HAVE_TASK_GROUP
#include  "group/group.h"
#endif
#include  "init/init.h"
#include  "debug/memdbg.h"
#ifdef CONFIG_LOG_DUMP
#include <tinyara/log_dump/log_dump_internal.h>
#endif

extern const uint32_t g_idle_topstack;

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* This is the set of all CPUs */

#define SCHED_ALL_CPUS		((1 << CONFIG_SMP_NCPUS) - 1)

/****************************************************************************
 * Private Type Declarations
 ****************************************************************************/

/****************************************************************************
 * Global Variables
 ****************************************************************************/

/* Task Lists ***************************************************************/
/* The state of a task is indicated both by the task_state field of the TCB
 * and by a series of task lists.  All of these tasks lists are declared
 * below. Although it is not always necessary, most of these lists are
 * prioritized so that common list handling logic can be used (only the
 * g_readytorun, the g_pendingtasks, and the g_waitingforsemaphore lists need
 * to be prioritized).
 */

/* This is the list of all tasks that are ready to run.  The head of this
 * list is the currently active task; the tail of this list is always the
 * IDLE task.  In the non-SMP case, the head of this list is the
 * currently active task and the tail of this list, the lowest priority
 * task, is always the IDLE task.
 */

volatile dq_queue_t g_readytorun;

/* In order to support SMP, the function of the g_readytorun list changes,
 * The g_readytorun is still used but in the SMP case it will contain only:
 *
 *  - Only tasks/threads that are eligible to run, but not currently running,
 *    and
 *  - Tasks/threads that have not been assigned to a CPU.
 *
 * Otherwise, the TCB will be retained in an assigned task list,
 * g_assignedtasks.  As its name suggests, on 'g_assignedtasks queue for CPU
 * 'n' would contain only tasks/threads that are assigned to CPU 'n'.  Tasks/
 * threads would be assigned a particular CPU by one of two mechanisms:
 *
 *  - (Semi-)permanently through an RTOS interfaces such as
 *    pthread_attr_setaffinity(), or
 *  - Temporarily through scheduling logic when a previously unassigned task
 *    is made to run.
 *
 * Tasks/threads that are assigned to a CPU via an interface like
 * pthread_attr_setaffinity() would never go into the g_readytorun list, but
 * would only go into the g_assignedtasks[n] list for the CPU 'n' to which
 * the thread has been assigned.  Hence, the g_readytorun list would hold
 * only unassigned tasks/threads.
 *
 * Like the g_readytorun list in in non-SMP case, each g_assignedtask[] list
 * is prioritized:  The head of the list is the currently active task on this
 * CPU.  Tasks after the active task are ready-to-run and assigned to this
 * CPU. The tail of this assigned task list, the lowest priority task, is
 * always the CPU's IDLE task.
 */

#ifdef CONFIG_SMP
volatile dq_queue_t g_assignedtasks[CONFIG_SMP_NCPUS];

/* g_running_tasks[] holds a references to the running task for each cpu.
 * It is valid only when up_interrupt_context() returns true.
 */

FAR struct tcb_s *g_running_tasks[CONFIG_SMP_NCPUS];

#else
//PORTNOTE: CONFIG_SMP_NCPUS should be dependant on CONFIG_SMP but Nuttx does
//not keep its usage under SMP, so I keep a single instance of the running
//tasks list when SMP is not defined
FAR struct tcb_s *g_running_tasks;

#endif

/* This is the list of all tasks that are ready-to-run, but cannot be placed
 * in the g_readytorun list because:  (1) They are higher priority than the
 * currently active task at the head of the g_readytorun list, and (2) the
 * currently active task has disabled pre-emption.
 */

volatile dq_queue_t g_pendingtasks;

/* This is the list of all tasks that are blocked waiting for a semaphore */

volatile dq_queue_t g_waitingforsemaphore;

/* This is the list of all tasks that are blocked waiting for a signal */

#ifndef CONFIG_DISABLE_SIGNALS
volatile dq_queue_t g_waitingforsignal;
#endif

/* This is the list of all tasks that are blocked waiting for a message
 * queue to become non-empty.
 */

#ifndef CONFIG_DISABLE_MQUEUE
volatile dq_queue_t g_waitingformqnotempty;
#endif

/* This is the list of all tasks that are blocked waiting for a message
 * queue to become non-full.
 */

#ifndef CONFIG_DISABLE_MQUEUE
volatile dq_queue_t g_waitingformqnotfull;
#endif

/* This is the list of all tasks that are blocking waiting for a page fill */

#ifdef CONFIG_PAGING
volatile dq_queue_t g_waitingforfill;
#endif

/* This is the list of all tasks that are blocking waiting to be unblocked another threads */

volatile dq_queue_t g_waitingforfin;

/* This the list of all tasks that have been initialized, but not yet
 * activated. NOTE:  This is the only list that is not prioritized.
 */

volatile dq_queue_t g_inactivetasks;

/* These are lists of dayed memory deallocations that need to be handled
 * within the IDLE loop or worker thread.  These deallocations get queued
 * by sched_kufree and sched_kfree() if the OS needs to deallocate memory
 * while it is within an interrupt handler.
 */

volatile sq_queue_t g_delayed_kufree;

volatile sq_queue_t g_delayed_kfree;

/* This gives number of alive tasks at any point of time in the system.
 * If the system is already running CONFIG_MAX_TASKS, Creating new
 * task is not supported.
 */
volatile uint8_t g_alive_taskcount;

/* This is the value of the last process ID assigned to a task */

volatile pid_t g_lastpid;

/* The following hash table is used for two things:
 *
 * 1. This hash table greatly speeds the determination of
 *    a new unique process ID for a task, and
 * 2. Is used to quickly map a process ID into a TCB.
 * It has the side effects of using more memory and limiting
 *
 * the number of tasks to CONFIG_MAX_TASKS.
 */

struct pidhash_s **g_pidhash;
volatile int g_npidhash;

/* This is a table of task lists.  This table is indexed by
 * the task state enumeration type (tstate_t) and provides
 * a pointer to the associated static task list (if there
 * is one) as well as a boolean indication as to if the list
 * is an ordered list or not.
 */

const struct tasklist_s g_tasklisttable[NUM_TASK_STATES] = {
	{NULL,                    false},	/* TSTATE_TASK_INVALID */
	{&g_pendingtasks,         true },	/* TSTATE_TASK_PENDING */
#ifdef CONFIG_SMP
	{&g_readytorun,           true},	/* TSTATE_TASK_READYTORUN */
	{g_assignedtasks,         true},	/* TSTATE_TASK_ASSIGNED */
	{g_assignedtasks,         true},	/* TSTATE_TASK_RUNNING */
#else
	{&g_readytorun,           true },	/* TSTATE_TASK_READYTORUN */
	{&g_readytorun,           true },	/* TSTATE_TASK_RUNNING */
#endif /* CONFIG_SMP */
	{&g_inactivetasks,        false},	/* TSTATE_TASK_INACTIVE */
	{&g_waitingforsemaphore,  true },	/* TSTATE_WAIT_SEM */
	{&g_waitingforfin,    true }		/* TSTATE_WAIT_FIN */
#ifndef CONFIG_DISABLE_SIGNALS
	,
	{&g_waitingforsignal,     false}	/* TSTATE_WAIT_SIG */
#endif
#ifndef CONFIG_DISABLE_MQUEUE
	,
	{&g_waitingformqnotempty, true },	/* TSTATE_WAIT_MQNOTEMPTY */
	{&g_waitingformqnotfull,  true }	/* TSTATE_WAIT_MQNOTFULL */
#endif
#ifdef CONFIG_PAGING
	,
	{&g_waitingforfill,       true }	/* TSTATE_WAIT_PAGEFILL */
#endif
};

/* This is the current initialization state.  The level of initialization
 * is only important early in the start-up sequence when certain OS or
 * hardware resources may not yet be available to the kernel logic.
 */

uint8_t g_os_initstate;  /* See enum nx_initstate_e */

/****************************************************************************
 * Private Variables
 ****************************************************************************/
/* This is the task control block for this thread of execution. This thread
 * of execution is the IDLE task.  NOTE:  the system boots into the IDLE
 * task.  The IDLE task spawns the user initialization task and that user
 * initialization task is responsible for bringing up the rest of the system.
 */

#ifdef CONFIG_SMP
static FAR struct task_tcb_s g_idletcb[CONFIG_SMP_NCPUS];
#else
static FAR struct task_tcb_s g_idletcb;
#endif

/* This is the name of the idle task */

//PORTNOTE: The block of code below can be refactored better I feel,
//Kept original for now
#if CONFIG_TASK_NAME_SIZE <= 0 || !defined(CONFIG_SMP)
#ifdef CONFIG_SMP
static const char g_idlename[] = "CPU Idle";
#else
static FAR const char g_idlename[] = "Idle Task";
#endif
#endif

/* This the IDLE idle threads argument list. */

#ifdef CONFIG_SMP
static FAR char *g_idleargv[CONFIG_SMP_NCPUS][2];
#else
static FAR char *g_idleargv[2];
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/
/****************************************************************************
 * Name: os_start
 *
 * Description:
 *   This function is called to initialize the operating system and to spawn
 *   the user initialization thread of execution.  This is the initial entry
 *   point into TinyAra
 *
 * Input Parameters:
 *   None
 *
 * Returned value:
 *   Does not return.
 *
 ****************************************************************************/

#ifndef CONFIG_SMP
void os_start(void)
{
	int i;

	slldbg("Entry\n");

	/* Initialize RTOS Data ************************************************** */
	/* Initialize all task lists */

	dq_init(&g_readytorun);
	dq_init(&g_pendingtasks);
	dq_init(&g_waitingforsemaphore);
#ifndef CONFIG_DISABLE_SIGNALS
	dq_init(&g_waitingforsignal);
#endif
#ifndef CONFIG_DISABLE_MQUEUE
	dq_init(&g_waitingformqnotfull);
	dq_init(&g_waitingformqnotempty);
#endif
#ifdef CONFIG_PAGING
	dq_init(&g_waitingforfill);
#endif
	dq_init(&g_inactivetasks);
	sq_init(&g_delayed_kufree);
#if (defined(CONFIG_BUILD_PROTECTED) || defined(CONFIG_BUILD_KERNEL)) && \
	 defined(CONFIG_MM_KERNEL_HEAP)
	sq_init(&g_delayed_kfree);
#endif

	/* Initialize the logic that determine unique process IDs. */

	g_lastpid = 0;
	for (i = 0; i < CONFIG_MAX_TASKS; i++) {
		g_pidhash[i]->tcb = NULL;
		g_pidhash[i]->pid = INVALID_PROCESS_ID;
	}

	/* Assign the process ID of ZERO to the idle task */

	g_pidhash[PIDHASH(0)]->tcb = &g_idletcb.cmn;
	g_pidhash[PIDHASH(0)]->pid = 0;

	/* Increment the g_alive_taskcount as first task "idle task" is created */
	g_alive_taskcount++;

	/* Initialize the IDLE task TCB *******************************************/
	/* Initialize a TCB for this thread of execution.  NOTE:  The default
	 * value for most components of the g_idletcb are zero.  The entire
	 * structure is set to zero.  Then only the (potentially) non-zero
	 * elements are initialized. NOTE:  The idle task is the only task in
	 * that has pid == 0 and sched_priority == 0.
	 */

	bzero((void *)&g_idletcb, sizeof(struct task_tcb_s));
	g_idletcb.cmn.task_state = TSTATE_TASK_RUNNING;
	g_idletcb.cmn.entry.main = (main_t)os_start;
	g_idletcb.cmn.flags = TCB_FLAG_TTYPE_KERNEL;

#if defined(CONFIG_APP_BINARY_SEPARATION) && defined(CONFIG_ARCH_USE_MMU)
	g_idletcb.cmn.app_id = 0;
	g_idletcb.cmn.pgtbl = mmu_get_os_l1_pgtbl();
	lldbg("Kernel Page table is at 0x%08x\n", g_idletcb.cmn.pgtbl);
#endif
	/* Set the IDLE task name */

#if CONFIG_TASK_NAME_SIZE > 0
	strncpy(g_idletcb.cmn.name, g_idlename, CONFIG_TASK_NAME_SIZE);
	g_idletcb.cmn.name[CONFIG_TASK_NAME_SIZE] = '\0';
#endif							/* CONFIG_TASK_NAME_SIZE */

	/* Configure the task name in the argument list.  The IDLE task does
	 * not really have an argument list, but this name is still useful
	 * for things like the TASH PS command.
	 *
	 * In the kernel mode build, the arguments are saved on the task's stack
	 * and there is no support that yet.
	 */

#if CONFIG_TASK_NAME_SIZE > 0
	g_idleargv[0]  = g_idletcb.cmn.name;
#else
	g_idleargv[0]  = (FAR char *)g_idlename;
#endif							/* CONFIG_TASK_NAME_SIZE */
	g_idleargv[1]  = NULL;
	g_idletcb.argv = g_idleargv;

	/* Fill the stack information to Idle task's tcb */

	g_idletcb.cmn.adj_stack_size = CONFIG_IDLETHREAD_STACKSIZE;
	g_idletcb.cmn.stack_alloc_ptr = (void *)(g_idle_topstack - CONFIG_IDLETHREAD_STACKSIZE);
	g_idletcb.cmn.adj_stack_ptr = (void *)(g_idle_topstack - 4);

	DEBUGASSERT(up_getsp() >= (uint32_t)g_idletcb.cmn.stack_alloc_ptr && up_getsp() <= (uint32_t)g_idletcb.cmn.adj_stack_ptr);

	/* Then add the idle task's TCB to the head of the ready to run list */

	dq_addfirst((FAR dq_entry_t *)&g_idletcb, (FAR dq_queue_t *)&g_readytorun);

	/* Initialize the processor-specific portion of the TCB */

	up_initial_state(&g_idletcb.cmn);

	/* Initialize RTOS facilities *********************************************
	 * Initialize the semaphore facility.  This has to be done very early
	 * because many subsystems depend upon fully functional semaphores.
	 */

	sem_initialize();


#if defined(CONFIG_MM_KERNEL_HEAP) || defined(CONFIG_MM_PGALLOC)
	/* Initialize the memory manager */

	{
		FAR void *heap_start;
		size_t heap_size;

#ifdef CONFIG_MM_KERNEL_HEAP
		/* Get the kernel-mode heap from the platform specific code and configure
		 * the kernel-mode memory allocator.
		 */

		up_allocate_kheap(&heap_start, &heap_size);
		if (kmm_initialize(heap_start, heap_size) != OK) {
			sdbg("ERROR : heap initialization is failed. heap_start : %x, heap_size : %u\n", heap_start, heap_size);
			PANIC();
		}
#endif

#ifdef CONFIG_MM_PGALLOC
		/* If there is a page allocator in the configuration, then get the page
		 * heap information from the platform-specific code and configure the
		 * page allocator.
		 */

		up_allocate_pgheap(&heap_start, &heap_size);
		mm_pginitialize(heap_start, heap_size);
#endif
	}
#endif

	up_add_kregion();

#ifdef CONFIG_APP_BINARY_SEPARATION
	mm_initialize_app_heap_q();
#endif

#if defined(CONFIG_SCHED_HAVE_PARENT) && defined(CONFIG_SCHED_CHILD_STATUS)
	/* Initialize tasking data structures */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (task_initialize != NULL)
#endif
	{
		task_initialize();
	}
#endif

	/* Initialize the interrupt handling subsystem (if included) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (irq_initialize != NULL)
#endif
	{
		irq_initialize();
	}

	/* Initialize the watchdog facility (if included in the link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (wd_initialize != NULL)
#endif
	{
		wd_initialize();
	}

#if CONFIG_NFILE_DESCRIPTORS > 0
	/* Initialize the file system (needed to support device drivers) */

	fs_initialize();
#endif

	/* Initialize the POSIX timer facility (if included in the link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (clock_initialize != NULL)
#endif
	{
		clock_initialize();
	}
#ifndef CONFIG_DISABLE_POSIX_TIMERS
#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (timer_initialize != NULL)
#endif
	{
		timer_initialize();
	}
#endif

#ifndef CONFIG_DISABLE_SIGNALS
	/* Initialize the signal facility (if in link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (sig_initialize != NULL)
#endif
	{
		sig_initialize();
	}
#endif

#ifndef CONFIG_DISABLE_MQUEUE
	/* Initialize the named message queue facility (if in link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (mq_initialize != NULL)
#endif
	{
		mq_initialize();
	}
#endif

#ifndef CONFIG_DISABLE_PTHREAD
	/* Initialize the thread-specific data facility (if in link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (pthread_initialize != NULL)
#endif
	{
		pthread_initialize();
	}
#endif

#ifdef CONFIG_NET
	/* Initialize the networking system.  Network initialization is
	 * performed in two steps:  (1) net_setup() initializes static
	 * configuration of the network support.  This must be done prior
	 * to registering network drivers by up_initialize().  This step
	 * cannot require upon any hardware-depending features such as
	 * timers or interrupts.
	 */

	net_setup();

#endif							/* CONFIG_NET */

	/* The processor specific details of running the operating system
	 * will be handled here.  Such things as setting up interrupt
	 * service routines and starting the clock are some of the things
	 * that are different for each  processor and hardware platform.
	 */

	up_initialize();

	/* Auto-mount Arch-independent File Sysytems */

	fs_auto_mount();

#ifdef CONFIG_DRIVERS_OS_API_TEST
	os_api_test_drv_register();
#endif

#if defined(CONFIG_DEBUG_SYSTEM)
	sysdbg_init();
#endif

#if defined(CONFIG_TTRACE)
	ttrace_init();
#endif

#ifdef CONFIG_MM_SHM
	/* Initialize shared memory support */

	shm_initialize();
#endif

	/* Initialize the C libraries.  This is done last because the libraries
	 * may depend on the above.
	 */

	lib_initialize();

#ifdef CONFIG_BINFMT_ENABLE
	/* Initialize the binfmt system */

	binfmt_initialize();
#endif

	/* IDLE Group Initialization **********************************************/
#ifdef HAVE_TASK_GROUP
	/* Allocate the IDLE group */

	DEBUGVERIFY(group_allocate(&g_idletcb, g_idletcb.cmn.flags));
#endif

#if CONFIG_NFILE_DESCRIPTORS > 0 || CONFIG_NSOCKET_DESCRIPTORS > 0
	/* Create stdout, stderr, stdin on the IDLE task.  These will be
	 * inherited by all of the threads created by the IDLE task.
	 */

	DEBUGVERIFY(group_setupidlefiles(&g_idletcb));
#endif

#ifdef HAVE_TASK_GROUP
	/* Complete initialization of the IDLE group.  Suppress retention
	 * of child status in the IDLE group.
	 */

	DEBUGVERIFY(group_initialize(&g_idletcb));
	g_idletcb.cmn.group->tg_flags = GROUP_FLAG_NOCLDWAIT;
#endif

#ifdef CONFIG_ARMV8M_TRUSTZONE
	up_init_secure_context();
#endif
	/* Bring Up the System ****************************************************/
	/* Create initial tasks and bring-up the system */

#ifdef CONFIG_PM
	/* We cannot enter low power state until boot complete */
	pm_stay(PM_IDLE_DOMAIN, PM_NORMAL);
#endif

#ifdef CONFIG_DEBUG_MM_WARN
	display_memory_information();
#endif

	DEBUGVERIFY(os_bringup());

	/* The IDLE Loop **********************************************************/
	/* When control is return to this point, the system is idle. */

	svdbg("Beginning Idle Loop\n");
	for (;;) {
		/* Perform garbage collection (if it is not being done by the worker
		 * thread).  This cleans-up memory de-allocations that were queued
		 * because they could not be freed in that execution context (for
		 * example, if the memory was freed from an interrupt handler).
		 */

#ifndef CONFIG_SCHED_WORKQUEUE
		/* We must have exclusive access to the memory manager to do this
		 * BUT the idle task cannot wait on a semaphore.  So we only do
		 * the cleanup now if we can get the semaphore -- this should be
		 * possible because if the IDLE thread is running, no other task is!
		 *
		 * WARNING: This logic could have undesirable side-effects if priority
		 * inheritance is enabled.  Imaginee the possible issues if the
		 * priority of the IDLE thread were to get boosted!  Moral: If you
		 * use priority inheritance, then you should also enable the work
		 * queue so that is done in a safer context.
		 */

		sched_garbagecollection();
#endif

		/* Perform any processor-specific idle state operations */

		up_idle();
	}
}

#else /* CONFIG_SMP */
//PORTNOTE: The below function is the implementation from Nuttx
//Instead of having a merged function, lets have a separate implementation as
//this is a very important function. But TizenRT function has diverged.
//Also, in general, names with "nx" have been changed to "os"
void os_start(void)
{
	int i;

	slldbg("Entry\n");

	/* Boot up is complete */

	g_os_initstate = OSINIT_BOOT;

	/* Initialize RTOS Data ***************************************************/

	/* Initialize the IDLE task TCB *******************************************/

	for (i = 0; i < CONFIG_SMP_NCPUS; i++) {
		FAR dq_queue_t *tasklist;

		/* Initialize a TCB for this thread of execution.  NOTE:  The default
		 * value for most components of the g_idletcb are zero.  The entire
		 * structure is set to zero.  Then only the (potentially) non-zero
		 * elements are initialized. NOTE:  The idle task is the only task in
		 * that has pid == 0 and sched_priority == 0.
		 */

		memset((void *)&g_idletcb[i], 0, sizeof(struct task_tcb_s));
		g_idletcb[i].cmn.pid = i;
		g_idletcb[i].cmn.task_state = TSTATE_TASK_RUNNING;

		/* Set the entry point.  This is only for debug purposes.  NOTE: that
		 * the start_t entry point is not saved.  That is acceptable, however,
		 * because it can be used only for restarting a task: The IDLE task
		 * cannot be restarted.
		 */

#ifdef CONFIG_SMP
		if (i > 0) {
			g_idletcb[i].cmn.start = os_idle_trampoline;
			g_idletcb[i].cmn.entry.main = (main_t)os_idle_trampoline;
		} else
#endif
		{
			g_idletcb[i].cmn.start = os_start;
			g_idletcb[i].cmn.entry.main = (main_t)os_start;
		}

		/* Set the task flags to indicate that this is a kernel thread and, if
		 * configured for SMP, that this task is locked to this CPU.
		 */

#ifdef CONFIG_SMP
		g_idletcb[i].cmn.flags = (TCB_FLAG_TTYPE_KERNEL | \
			TCB_FLAG_NONCANCELABLE | \
			TCB_FLAG_CPU_LOCKED);
		g_idletcb[i].cmn.cpu = i;

		/* Set the affinity mask to allow the thread to run on all CPUs.  No,
		 * this IDLE thread can only run on its assigned CPU.  That is
		 * enforced by the TCB_FLAG_CPU_LOCKED which overrides the affinity
		 * mask.  This is essential because all tasks inherit the affinity
		 * mask from their parent and, ultimately, the parent of all tasks is
		 * the IDLE task.
		 */

		g_idletcb[i].cmn.affinity = SCHED_ALL_CPUS;
#else
		g_idletcb[i].cmn.flags = (TCB_FLAG_TTYPE_KERNEL | \
				TCB_FLAG_NONCANCELABLE);
#endif

#if CONFIG_TASK_NAME_SIZE > 0
		/* Set the IDLE task name */

#ifdef CONFIG_SMP
		snprintf(g_idletcb[i].cmn.name, CONFIG_TASK_NAME_SIZE, "CPU%d IDLE", i);
#else
		strlcpy(g_idletcb[i].cmn.name, g_idlename, CONFIG_TASK_NAME_SIZE);
#endif

		/* Configure the task name in the argument list.  The IDLE task does
		 * not really have an argument list, but this name is still useful
		 * for things like the NSH PS command.
		 *
		 * In the kernel mode build, the arguments are saved on the task's
		 * stack and there is no support that yet.
		 */

		g_idleargv[i][0] = g_idletcb[i].cmn.name;
#else
		g_idleargv[i][0] = (FAR char *)g_idlename;
#endif /* CONFIG_TASK_NAME_SIZE */

		/* Then add the idle task's TCB to the head of the current ready to
		 * run list.
		 */

#ifdef CONFIG_SMP
		tasklist = TLIST_HEAD(TSTATE_TASK_RUNNING, i);
#else
		tasklist = TLIST_HEAD(TSTATE_TASK_RUNNING);
#endif
		dq_addfirst((FAR dq_entry_t *)&g_idletcb[i], tasklist);

		/* Mark the idle task as the running task */

		g_running_tasks[i] = &g_idletcb[i].cmn;
	}

	/* Task lists are initialized */

	g_os_initstate = OSINIT_TASKLISTS;

	/* Initialize RTOS facilities *********************************************/

	/* Initialize the semaphore facility.  This has to be done very early
	 * because many subsystems depend upon fully functional semaphores.
	 */

//PORTNOTE: nxsem_initialize changed to sem_initialize, check TizenRT name
	sem_initialize();

#if defined(MM_KERNEL_USRHEAP_INIT) || defined(CONFIG_MM_KERNEL_HEAP) || \
    defined(CONFIG_MM_PGALLOC)
	/* Initialize the memory manager */

	{
		FAR void *heap_start;
		size_t heap_size;

#ifdef MM_KERNEL_USRHEAP_INIT
		/* Get the user-mode heap from the platform specific code and configure
		 * the user-mode memory allocator.
		 */

		up_allocate_heap(&heap_start, &heap_size);
		kumm_initialize(heap_start, heap_size);
#endif

#ifdef CONFIG_MM_KERNEL_HEAP
		/* Get the kernel-mode heap from the platform specific code and
		 * configure the kernel-mode memory allocator.
		 */

		up_allocate_kheap(&heap_start, &heap_size);
		kmm_initialize(heap_start, heap_size);
#endif

#ifdef CONFIG_MM_PGALLOC
		/* If there is a page allocator in the configuration, then get the page
		 * heap information from the platform-specific code and configure the
		 * page allocator.
		 */

		up_allocate_pgheap(&heap_start, &heap_size);
		mm_pginitialize(heap_start, heap_size);
#endif
	}
#endif

#ifdef CONFIG_ARCH_HAVE_EXTRA_HEAPS
  /* Initialize any extra heap. */

	up_extraheaps_init();
#endif

#ifdef CONFIG_MM_IOB
  /* Initialize IO buffering */

	iob_initialize();
#endif

  /* Initialize the logic that determine unique process IDs. */

	g_npidhash = 4;
	while (g_npidhash <= CONFIG_SMP_NCPUS) {
		g_npidhash <<= 1;
	}

	g_pidhash = kmm_zalloc(sizeof(*g_pidhash) * g_npidhash);
	DEBUGASSERT(g_pidhash);

	/* IDLE Group Initialization **********************************************/

	for (i = 0; i < CONFIG_SMP_NCPUS; i++) {
		int hashndx;

		/* Assign the process ID(s) of ZERO to the idle task(s) */

		hashndx = PIDHASH(i);
		g_pidhash[hashndx] = &g_idletcb[i].cmn;

		/* Allocate the IDLE group */

		DEBUGVERIFY(group_allocate(&g_idletcb[i], g_idletcb[i].cmn.flags));
		g_idletcb[i].argv = &g_idleargv[i][0];

#ifdef CONFIG_SMP
		/* Create a stack for all CPU IDLE threads (except CPU0 which already
		 * has a stack).
		 */

		if (i > 0) {
			DEBUGVERIFY(up_cpu_idlestack(i, &g_idletcb[i].cmn, \
						CONFIG_IDLETHREAD_STACKSIZE));
		}
#endif

		/* Initialize the processor-specific portion of the TCB */

		up_initial_state(&g_idletcb[i].cmn);

		/* Initialize the thread local storage */

//PORTNOTE: We will not support tls.. tls_init_info(&g_idletcb[i].cmn);

		/* Complete initialization of the IDLE group.  Suppress retention
		 * of child status in the IDLE group.
		 */

		group_initialize(&g_idletcb[i]);
		g_idletcb[i].cmn.group->tg_flags = GROUP_FLAG_NOCLDWAIT | \
						   GROUP_FLAG_PRIVILEGED;
	}

	g_lastpid = CONFIG_SMP_NCPUS - 1;

	/* The memory manager is available */

	g_os_initstate = OSINIT_MEMORY;

#if defined(CONFIG_SCHED_HAVE_PARENT) && defined(CONFIG_SCHED_CHILD_STATUS)
	/* Initialize tasking data structures */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (task_initialize != NULL)
#endif
	{
		task_initialize();
	}
#endif

	/* Disables context switching because we need take the memory manager
	 * semaphore on this CPU so that it will not be available on the other
	 * CPUs until we have finished initialization.
	 */

	sched_lock();

	/* Initialize the file system (needed to support device drivers) */

	fs_initialize();

	/* Initialize the interrupt handling subsystem (if included) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (irq_initialize != NULL)
#endif
	{
		irq_initialize();
	}

	/* Initialize the POSIX timer facility (if included in the link) */

	#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (clock_initialize != NULL)
#endif
	{
		clock_initialize();
	}

#ifndef CONFIG_DISABLE_POSIX_TIMERS
#ifdef CONFIG_HAVE_WEAKFUNCTIONS
	if (timer_initialize != NULL)
#endif
	{
		timer_initialize();
	}
#endif

	/* Initialize the signal facility (if in link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
//PORTNOTE: nxsig_initialize changed to sig_initialize, check TizenRT name
	if (sig_initialize != NULL)
#endif
	{
		sig_initialize();
	}

#ifndef CONFIG_DISABLE_MQUEUE
	/* Initialize the named message queue facility (if in link) */

#ifdef CONFIG_HAVE_WEAKFUNCTIONS
//PORTNOTE: nxmq_initialize changed to mq_initialize, check TizenRT name
	if (mq_initialize != NULL)
#endif
	{
		mq_initialize();
	}
#endif

#ifdef CONFIG_NET
	/* Initialize the networking system */

	net_initialize();
#endif

#ifdef CONFIG_BINFMT_DISABLE
	/* Initialize the binfmt system */

	binfmt_initialize();
#endif

	/* Initialize Hardware Facilities *****************************************/

	/* The processor specific details of running the operating system
	 * will be handled here.  Such things as setting up interrupt
	 * service routines and starting the clock are some of the things
	 * that are different for each  processor and hardware platform.
	 */

	up_initialize();

	#ifdef CONFIG_BOARD_EARLY_INITIALIZE
	/* Call the board-specific up_initialize() extension to support
	 * early initialization of board-specific drivers and resources
	 * that cannot wait until board_late_initialize.
	 */

	board_early_initialize();
#endif

	/* Hardware resources are now available */

	g_os_initstate = OSINIT_HARDWARE;

	/* Setup for Multi-Tasking ************************************************/

	/* Announce that the CPU0 IDLE task has started */

	sched_note_start(&g_idletcb[0].cmn);

	/* Initialize stdio for the IDLE task of each CPU */

	for (i = 0; i < CONFIG_SMP_NCPUS; i++) {
		if (i > 0) {
			/* Clone stdout, stderr, stdin from the CPU0 IDLE task. */

			DEBUGVERIFY(group_setuptaskfiles(&g_idletcb[i]));
		} else {
			/* Create stdout, stderr, stdin on the CPU0 IDLE task.  These
			 * will be inherited by all of the threads created by the CPU0
			 * IDLE task.
			 */

			DEBUGVERIFY(group_setupidlefiles(&g_idletcb[i]));
		}
	}

#ifdef CONFIG_SMP
	/* Start all CPUs *********************************************************/

	/* A few basic sanity checks */

	DEBUGASSERT(this_cpu() == 0);

	/* Then start the other CPUs */

	DEBUGVERIFY(os_smp_start());

#endif /* CONFIG_SMP */

	/* Bring Up the System ****************************************************/

	/* The OS is fully initialized and we are beginning multi-tasking */

	g_os_initstate = OSINIT_OSREADY;

	/* Create initial tasks and bring-up the system */

	DEBUGVERIFY(os_bringup());
	/* Enter to idleloop */

	g_os_initstate = OSINIT_IDLELOOP;

	/* Let other threads have access to the memory manager */

	sched_unlock();

	/* The IDLE Loop **********************************************************/

	/* When control is return to this point, the system is idle. */

	slldbg("CPU0: Beginning Idle Loop\n");
	for (; ; ) {
		/* Perform any processor-specific idle state operations */

		up_idle();
	}
}
#endif /* CONFIG_SMP */
