// SMP.1 + SMP.3
// spinlock + mask interrupts
#include "config.h"
#include "errno.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "globals.h"
#include "kernel.h"
#include "mm/slab.h"
#include "util/debug.h"
#include "util/printf.h"
#include "util/string.h"
#include "util/time.h"
#include <drivers/screen.h>
#include <fs/vfs_syscall.h>
#include <main/apic.h>

/*==========
 * Variables
 *=========*/

/*
 * Global variable that maintains the current process
 */
proc_t *curproc CORE_SPECIFIC_DATA;

/*
 * Global list of all processes (except for the idle process) and its lock
 */
static list_t proc_list = LIST_INITIALIZER(proc_list);
static spinlock_t proc_list_lock = SPINLOCK_INITIALIZER(proc_list_lock);

/*
 * Allocator for process descriptors
 */
static slab_allocator_t *proc_allocator = NULL;

/*
 * Statically allocated idle process
 * Each core has its own idleproc, so the idleproc is stored in static memory
 * rather than in the global process list
 */
proc_t idleproc CORE_SPECIFIC_DATA;

/*
 * Pointer to the init process
 */
static proc_t *proc_initproc = NULL;

/*===============
 * System startup
 *==============*/

/*
 * Initializes the allocator for process descriptors.
 */
void proc_init()
{
    proc_allocator = slab_allocator_create("proc", sizeof(proc_t));
    KASSERT(proc_allocator);
}

/*
 * Initializes idleproc for the current core. Sets initial values for curproc
 * and curthr.
 */
void proc_idleproc_init()
{
    proc_t *proc = &idleproc;

    proc->p_pid = 0;
    list_init(&proc->p_threads);
    list_init(&proc->p_children);
    proc->p_pproc = NULL;

    list_link_init(&proc->p_child_link);
    list_link_init(&proc->p_list_link);

    spinlock_init(&proc->p_children_lock);

    proc->p_status = 0;
    proc->p_state = PROC_RUNNING;

    memset(&proc->p_wait, 0, sizeof(ktqueue_t)); // should not be used

    proc->p_pml4 = pt_get();
    #ifdef __VM__
        proc->p_vmmap = vmmap_create();
    #endif  

    proc->p_cwd = NULL;

    memset(proc->p_files, 0, sizeof(proc->p_files));

    char name[8];
    snprintf(name, sizeof(name), "idle%ld", curcore.kc_id);
    strncpy(proc->p_name, name, PROC_NAME_LEN);
    proc->p_name[PROC_NAME_LEN - 1] = '\0';

    dbg(DBG_PROC, "created %s\n", proc->p_name);
    curproc = &idleproc;
    curthr = NULL;
}

/*=================
 * Helper functions
 *================*/

/*
 * Gets the next available process ID (pid).
 */
static pid_t next_pid = 1;
static pid_t _proc_getid()
{
    spinlock_lock(&proc_list_lock);
    pid_t pid = next_pid;
restart:
    list_iterate(&proc_list, p, proc_t, p_list_link)
    {
        if (p->p_pid == pid)
        {
            pid = pid + 1 == PROC_MAX_COUNT ? 1 : pid + 1;
            if (pid == next_pid)
            {
                spinlock_unlock(&proc_list_lock);
                return -1;
            }
            else
            {
                goto restart;
            }
        }
    }
    next_pid = pid + 1 == PROC_MAX_COUNT ? 1 : pid + 1;
    KASSERT(pid);
    spinlock_unlock(&proc_list_lock);
    return pid;
}

/*
 * Searches the global process list for the process descriptor corresponding to
 * a pid.
 */
proc_t *proc_lookup(pid_t pid)
{
    if (pid == 0)
    {
        return &idleproc;
    }
    spinlock_lock(&proc_list_lock);
    list_iterate(&proc_list, p, proc_t, p_list_link)
    {
        if (p->p_pid == pid)
        {
            spinlock_unlock(&proc_list_lock);
            return p;
        }
    }
    spinlock_unlock(&proc_list_lock);
    return NULL;
}

/*==========
 * Functions
 *=========*/

/*
 * Creates a new process with the given name.
 * Returns the newly created process, or null on failure.
 *
 * Hints:
 * Use _proc_getid() to get a new pid.
 * Allocate a new proc_t with the process slab allocator (proc_allocator).
 * Use pt_create() to create a new page table (p_pml4).
 * If the newly created process is the init process (i.e. the generated PID 
 * matches the init process's PID, given by the macro PID_INIT), set the 
 * global proc_initproc to the created process.
 * 
 * There is some setup to be done for VFS and VM - remember to return to this
 * function! For VFS, clone and ref the files from curproc. For VM, clone the
 * vmmap from curproc. 
 * 
 * Be sure to free resources appropriately if proc_create() fails midway!
 */
proc_t *proc_create(const char *name)
{
    proc_t *new_proc;
    new_proc=slab_obj_alloc(proc_allocator); // Allocated new space for new_proc
    new_proc->p_pid=_proc_getid(); // Get a new id
    if(new_proc->p_pid==-1){ // ERROR check
        slab_obj_free(proc_allocator, new_proc); // Free resources
        return NULL;
    }
    strcpy(new_proc->p_name,name); //Initialize the name TODO: Check it
    new_proc->p_pml4=pt_create(); // New page table
    list_init(&new_proc->p_threads); // Initialize two lists
    list_init(&new_proc->p_children); 
    new_proc->p_pproc=curproc; // set the parent process, which is current process
    
    list_link_init(&new_proc->p_child_link); // Initialize two list link
    list_link_init(&new_proc->p_list_link);
    list_insert_tail(&proc_list,&new_proc->p_list_link); // Insert the new_proc into the proc_list
    list_insert_tail(&curproc->p_children,&new_proc->p_child_link); // Insert it into parents' children list(current process)

    spinlock_init(&new_proc->p_children_lock); // Initialize spin lock
    new_proc->p_status=0; 
    new_proc->p_state=PROC_RUNNING;
    sched_queue_init(&new_proc->p_wait);

    if(new_proc->p_pid==PID_INIT){ // If the new proc is the init process
        proc_initproc=new_proc;
    }

   // Initialize VFS part
    for(int i=0;i<NFILES;i++){
        new_proc->p_files[i]=curproc->p_files[i];   // Copy the file from current process to new process
        if(new_proc->p_files[i]!=NULL)  {
            fref(new_proc->p_files[i]);     // Increment the refcount is the file exist
        }
    }
    new_proc->p_cwd=curproc->p_cwd;     // Set the current working directory
    if(new_proc->p_cwd!=NULL){
        vref(new_proc->p_cwd);
    }

    // Initialize VM part
    new_proc->p_brk=curproc->p_brk;     // Set the break
    new_proc->p_start_brk=curproc->p_start_brk;
    new_proc->p_vmmap=vmmap_clone(curproc->p_vmmap);    // Set the vmmap
    new_proc->p_vmmap->vmm_proc = new_proc;
    // NOT_YET_IMPLEMENTED("PROCS: proc_create");
    return new_proc;
}

/*
 * Helper for proc_thread_exiting() that cleans up resources from the current
 * process in preparation for its destruction (which occurs later via proc_destroy()). 
 * Reparents child processes to the init process, or initiates Weenix shutdown 
 * if the current process is the init process.
 *
 * Hints:
 * You won't have much to clean up until VFS and VM -- remember to revisit this
 * function later!
 * **VFS/VM** - there may be some repeat code in proc_destroy()). The initial process
 * does not have a parent process and thus cleans itself up, hence why we need to cleanup 
 * here as well. 
 * 
 * Remember to set the state and status of the process.
 * The init process' PID is given by PID_INIT
 * Use initproc_finish() to shutdown Weenix when cleaning up the init process.
 */
void proc_cleanup(long status)
{
    curproc->p_state=PROC_DEAD;
    curproc->p_status=status;         // Set current process's state and status
    for(int i=0;i<NFILES;i++){
        do_close(i);                // Clean up all the files
    }
    if(curproc->p_cwd){
        vput(&curproc->p_cwd);
    }
    if(curproc->p_vmmap){
        vmmap_destroy(&curproc->p_vmmap);
    }

    if(curproc->p_pid==PID_INIT){  // If the current process is initial process       
       initproc_finish(); // Shup down the weenix
    }
    else{
        list_iterate(&curproc->p_children,p_child,proc_t,p_child_link){ // Loop the child list
            list_remove(&p_child->p_child_link); // Remove children in current process's children link
            p_child->p_pproc=proc_initproc;
            list_insert_tail(&proc_initproc->p_children,&(p_child->p_child_link)); // Add the children into 
                                                                                 // initial process's list                                                                                  
        }
    }
    // NOT_YET_IMPLEMENTED("PROCS: proc_cleanup");
}

/*
 * Cleans up the current process and the current thread, broadcasts on its
 * parent's p_wait, then forces a context switch. After this, the process is
 * essentially dead -- this function does not return. The parent must eventually
 * finish destroying the process.
 *
 * Hints:
 * Use proc_cleanup() to clean up the current process (you should pass (long)retval
 * as the status argument).
 * Remember to set the exit state and return value of the current thread after calling
 * proc_cleanup(), as this may block and cause the thread's state to be overwritten. 
 * The context switch should be performed by a call to sched_switch().
 */
void proc_thread_exiting(void *retval)
{
    proc_cleanup((long)retval); // Clean up the current process   
    curthr->kt_state=KT_EXITED; // Set the exited state
    curthr->kt_retval=retval; 
    sched_broadcast_on(&curproc->p_pproc->p_wait); // Wake the parent of the process up   
    sched_switch(0,0); // Force the process die, and don't put current thread 
    // into queue, so set argument as 0,0
    // NOT_YET_IMPLEMENTED("PROCS: proc_thread_exiting");
}

/*
 * Cancels all the threads of proc. This should never be called on curproc.
 * 
 * Hints:
 * The status argument should be passed to kthread_cancel() as the retval.
 */
void proc_kill(proc_t *proc, long status)
{
    if(proc!=curproc) { // Make sure that it is not current process
      proc->p_status=status; // Set the status of the process
      list_iterate(&proc->p_threads,p_thr,kthread_t,kt_plink){ // Iterate the process's thread list
            kthread_cancel(p_thr,(void *)status); // Cancel all the thread
      }            // TODO: Not sure if kthread_cancel is correct
    }
    // NOT_YET_IMPLEMENTED("PROCS: proc_kill");
}

/*
 * Kills all processes that are not curproc and not a direct child of idleproc (i.e.,
 * the init process), then kills the current process.
 *
 * Hints:
 * The PID of the idle process is given by PID_IDLE.
 * Processes should be killed with a status of -1.
 * Use do_exit() to kill the current process.
 */
void proc_kill_all()
{
    list_iterate(&proc_list,iter_proc,proc_t,p_list_link){ // Loop the list of all processes
           if(iter_proc->p_pproc!=&idleproc){ // If it's parents is not idleproc
                proc_kill(iter_proc,-1); // Cancel the process
           }
    }
    do_exit(-1); // Kill the current process
    // NOT_YET_IMPLEMENTED("PROCS: proc_kill_all");
}

/*
 * Destroy / free everything from proc. Be sure to remember reference counting
 * when working on VFS.
 *
 * In contrast with proc_cleanup() (in which a process begins to clean itself up), this 
 * will be called on proc by some other process to complete its cleanup. 
 * I.e., the process we are destroying should not be curproc.
 */
void proc_destroy(proc_t *proc)
{
    spinlock_lock(&proc_list_lock);
    list_remove(&proc->p_list_link); // Remove it from process list
    spinlock_unlock(&proc_list_lock);

    list_iterate(&proc->p_threads, thr, kthread_t, kt_plink)
    {
        kthread_destroy(thr);
    }

#ifdef __VFS__
    for (int fd = 0; fd < NFILES; fd++)
    {
        if (proc->p_files[fd])
            fput(proc->p_files + fd);
    }
    if (proc->p_cwd)
    {
        vput(&proc->p_cwd);
    }
#endif

#ifdef __VM__
    if (proc->p_vmmap)
        vmmap_destroy(&proc->p_vmmap);
#endif

    dbg(DBG_THR, "destroying P%d\n", proc->p_pid);

    KASSERT(proc->p_pml4);
    pt_destroy(proc->p_pml4);

    slab_obj_free(proc_allocator, proc);
}

/*=============
 * System calls
 *============*/

/*
 * Waits for a child process identified by pid to exit. Finishes destroying the
 * process and optionally returns the child's status in status.
 *
 * If pid is a positive integer, tries to clean up the process specified by pid.
 * If pid is -1, cleans up any child process of curproc that exits.
 *
 * Returns the pid of the child process that exited, or error cases:
 *  - ENOTSUP: pid is 0, a negative number not equal to -1,
 *      or options are specified (options does not equal 0)
 *  - ECHILD: pid is a positive integer but not a child of curproc, or
 *      pid is -1 and the process has no children
 *
 * Hints:
 * Use sched_sleep_on() to be notified of a child process exiting.
 * Destroy an exited process by removing it from any lists and calling
 * proc_destroy(). Remember to set status (if it was provided) to the child's
 * status before destroying the process.
 * If waiting on a specific child PID, wakeups from other exiting child
 * processes should be ignored.
 * If waiting on any child (-1), do_waitpid can return when *any* child has exited,
 * it does not have to return the one that exited earliest. // TODO: Not sure what this mean
 */
pid_t do_waitpid(pid_t pid, int *status, int options)
// Need to return value to status, this is why we deference it, which means that we can change the content inside
// this address. In addition, a function cannnot return two variables, so we set status here
{
    if((pid<=0&&pid!=-1)||options!=0){
        return -ENOTSUP; 
    }
    if(pid==-1&& list_empty(&curproc->p_children)){ // If the process has no children
       return -ECHILD;
    }
    pid_t find_pid=-1;  // Set the initial value of children's pid
    if(pid>0){
    while(1){
       list_iterate(&curproc->p_children,iter_chil,proc_t,p_child_link){    
           if(iter_chil->p_pid==pid){ // Clean up certain process with certain pid   
                find_pid=iter_chil->p_pid; // Get the pid of the children 
           if(iter_chil->p_state==PROC_DEAD){
               *status= iter_chil->p_status;   // Set the status before destroy
               list_remove(&iter_chil->p_child_link); // Remove it from parents children link, since we 
                // didn't do it in proc_destory
               proc_destroy(iter_chil); // Destroy the child process 
               return find_pid;
           }
           else{
            sched_sleep_on(&curproc->p_wait,&curproc->p_children_lock); // Put parents into its own wait queue
           }
           }
        }
            if(find_pid==-1){ // If we didn't find a specified pid in the children list
               return -ECHILD;
           }
    }
        // TODO: Figure out how to return error condition
    }
    else if(pid==-1){
        while(1){
        list_iterate(&curproc->p_children,iter_chil,proc_t,p_child_link){
        if(iter_chil->p_state==PROC_DEAD){
           *status= iter_chil->p_status;   // Set the status before destroy
           find_pid=iter_chil->p_pid;
           list_remove(&iter_chil->p_child_link);
           proc_destroy(iter_chil);
           return find_pid;
        }
        }   
        if(find_pid==-1){  // If we cannot find a terminated child, put parents into sleep firstly
            sched_sleep_on(&curproc->p_wait,&curproc->p_children_lock); // Put parents into its own wait queue
        }
    }
}
    return find_pid;
   // NOT_YET_IMPLEMENTED("PROCS: do_waitpid");
}

/*
 * Wrapper around kthread_exit.
 */
void do_exit(long status)
{
    kthread_exit((void *)status); 
    // NOT_YET_IMPLEMENTED("PROCS: do_exit");
}

/*==========
 * Debugging
 *=========*/

size_t proc_info(const void *arg, char *buf, size_t osize)
{
    const proc_t *p = (proc_t *)arg;
    size_t size = osize;
    proc_t *child;

    KASSERT(NULL != p);
    KASSERT(NULL != buf);

    iprintf(&buf, &size, "pid:          %i\n", p->p_pid);
    iprintf(&buf, &size, "name:         %s\n", p->p_name);
    if (NULL != p->p_pproc)
    {
        iprintf(&buf, &size, "parent:       %i (%s)\n", p->p_pproc->p_pid,
                p->p_pproc->p_name);
    }
    else
    {
        iprintf(&buf, &size, "parent:       -\n");
    }

    if (list_empty(&p->p_children))
    {
        iprintf(&buf, &size, "children:     -\n");
    }
    else
    {
        iprintf(&buf, &size, "children:\n");
    }
    list_iterate(&p->p_children, child, proc_t, p_child_link)
    {
        iprintf(&buf, &size, "     %i (%s)\n", child->p_pid, child->p_name);
    }

    iprintf(&buf, &size, "status:       %ld\n", p->p_status);
    iprintf(&buf, &size, "state:        %i\n", p->p_state);

#ifdef __VFS__
#ifdef __GETCWD__
    if (NULL != p->p_cwd)
    {
        char cwd[256];
        lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
        iprintf(&buf, &size, "cwd:          %-s\n", cwd);
    }
    else
    {
        iprintf(&buf, &size, "cwd:          -\n");
    }
#endif /* __GETCWD__ */
#endif

#ifdef __VM__
    iprintf(&buf, &size, "start brk:    0x%p\n", p->p_start_brk);
    iprintf(&buf, &size, "brk:          0x%p\n", p->p_brk);
#endif

    return size;
}

size_t proc_list_info(const void *arg, char *buf, size_t osize)
{
    size_t size = osize;

    KASSERT(NULL == arg);
    KASSERT(NULL != buf);

#if defined(__VFS__) && defined(__GETCWD__)
    iprintf(&buf, &size, "%5s %-13s %-18s %-s\n", "PID", "NAME", "PARENT",
            "CWD");
#else
    iprintf(&buf, &size, "%5s %-13s %-s\n", "PID", "NAME", "PARENT");
#endif

    list_iterate(&proc_list, p, proc_t, p_list_link)
    {
        char parent[64];
        if (NULL != p->p_pproc)
        {
            snprintf(parent, sizeof(parent), "%3i (%s)", p->p_pproc->p_pid,
                     p->p_pproc->p_name);
        }
        else
        {
            snprintf(parent, sizeof(parent), "  -");
        }

#if defined(__VFS__) && defined(__GETCWD__)
        if (NULL != p->p_cwd)
        {
            char cwd[256];
            lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
            iprintf(&buf, &size, " %3i  %-13s %-18s %-s\n", p->p_pid, p->p_name,
                    parent, cwd);
        }
        else
        {
            iprintf(&buf, &size, " %3i  %-13s %-18s -\n", p->p_pid, p->p_name,
                    parent);
        }
#else
        iprintf(&buf, &size, " %3i  %-13s %-s\n", p->p_pid, p->p_name, parent);
#endif
    }
    return size;
}
