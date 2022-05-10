#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "stdlib.h"

struct cpu cpus[NCPU];
  // static char *states[] = {
  // [UNUSED]    "unused",
  // [SLEEPING]  "sleep ",
  // [RUNNABLE]  "runble",
  // [RUNNING]   "run   ",
  // [ZOMBIE]    "zombie"
  // };
struct proc proc[NPROC];

int unused_list = -1;
struct spinlock unused_list_lock;
int zombie_list = -1;
struct spinlock zombie_list_lock;
int sleeping_list = -1;
struct spinlock sleeping_list_lock;


struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

extern uint64 cas(volatile void *addr , int expected , int newval);

extern void push(int *head, struct proc *p, struct spinlock *lock);
extern int pop(int *head, struct spinlock *list_lock);
extern int remove(int *head, struct proc* p, struct spinlock *list_lock);

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{

  //initialize CPU's runnable lists and locks
  struct cpu *c;
  for(c = cpus; c < &cpus[NCPU];c++)
  {
    initlock(&c->list_lock, "runnable_list");
    c->runnable_list = -1;
  }

  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  int index = 0;
  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    initlock(&p->node_lock, "list_lock");
    p->kstack = KSTACK((int) (p - proc));
    p->index = index;
    p->next = -1;
    push(&unused_list, p, &unused_list_lock);
    p->state = UNUSED;
    index++;
  }
  initlock(&unused_list_lock, "unused_list");
  initlock(&zombie_list_lock, "zombie_list");
  initlock(&sleeping_list_lock, "sleeping_list");
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  do {
    pid = nextpid;
  } while(cas(&nextpid, pid, pid + 1));
  
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  int i = pop(&unused_list, &unused_list_lock);
  if(i == -1)
    return 0;
  
  struct proc *p = &proc[i];
  acquire(&p->lock);

  p->pid = allocpid();
  p->state = USED;
  p->cpu_num = 0;
  p->next = -1;
  //printf("alloc: proc %d cpu %d state %s\n", p->index, p->cpu_num, states[p->state]);

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  //printf("freeproc: proc %d state %s\n", p->index, states[p->state]);
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->cpu_num = -1;
  remove(&zombie_list,p, &zombie_list_lock);
  push(&unused_list, p, &unused_list_lock);
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  struct cpu *c = &cpus[0];
  //printf("userinit: proc %d\n", p->index);
  push(&c->runnable_list, p, &c->list_lock);
  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  //printf("fork : from %d state %s to %d state %s\n", p->index, states[p->state], np->index, states[np->state]);
  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  np->cpu_num = p->cpu_num;
  release(&wait_lock);

  acquire(&np->lock);
  struct cpu *c = &cpus[np->cpu_num];
  push(&c->runnable_list, np, &c->list_lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  push(&zombie_list, p, &zombie_list_lock);
  p->state = ZOMBIE;
  //printf("exit: proc %d cpu %d state %s\n", p->index, p->cpu_num, states[p->state]);
  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    int i = pop(&c->runnable_list, &c->list_lock);
    if(i != -1) 
    {
      p = &proc[i];
      //printf("sched: proc %d cpu %d state %s\n", p->index, p->cpu_num, states[p->state]);
      acquire(&p->lock);
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      p->state = RUNNING;
      c->proc = p;
      swtch(&c->context, &p->context);
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  struct cpu *c = &cpus[p->cpu_num];
  //printf("yield: proc %d cpu %d state %s\n", p->index, p->cpu_num, states[p->state]);
  push(&c->runnable_list, p, &c->list_lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  //printf("sleep: proc %d cpu %d state %s\n", p->index, p->cpu_num, states[p->state]);
  push(&sleeping_list, p, &sleeping_list_lock);
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  acquire(&sleeping_list_lock);
  if(sleeping_list == -1)
  {
    release(&sleeping_list_lock);
    return;
  }
  struct proc *p1 = &proc[sleeping_list];
  acquire(&p1->lock);
  acquire(&p1->node_lock);
  while(p1->chan == chan)
  {
    sleeping_list = p1->next;
    release(&sleeping_list_lock);
    p1->next = -1;
    release(&p1->node_lock);
    struct cpu *c = &cpus[p1->cpu_num];
    //printf("wakup: proc %d cpu %d state %s\n", p1->index, p1->cpu_num, states[p1->state]);
    push(&c->runnable_list, p1, &c->list_lock);
    p1->state = RUNNABLE;
    release(&p1->lock);
    if(sleeping_list == -1)
      return;
    p1 = &proc[sleeping_list];
    acquire(&p1->lock);
    acquire(&p1->node_lock);
    acquire(&sleeping_list_lock);
  }
  release(&sleeping_list_lock);
  struct proc *p2;
  while(p1->next != -1)
  {
    p2 = &proc[p1->next];
    acquire(&p2->lock);
    acquire(&p2->node_lock);
    if(p2->chan == chan)
    {
      p1->next = p2->next;
      p2->next = -1;
      release(&p2->node_lock);
      struct cpu *c = &cpus[p2->cpu_num];
      //printf("wakup: proc %d cpu %d state %s\n", p1->index, p1->cpu_num, states[p1->state]);
      push(&c->runnable_list, p2, &c->list_lock);
      p2->state = RUNNABLE;
      release(&p2->lock);
    }
    else
    {
      release(&p1->node_lock);
      release(&p1->lock);
      p1 = p2;
    }
  }
  release(&p1->node_lock);
  release(&p1->lock);
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
set_cpu(int cpu_num)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  yield();
  struct cpu *old_c = &cpus[p->cpu_num];
  if(cpu_num < 0 || cpu_num > NCPU - 1)
  {
    release(&p->lock);
    return -1;
  }
  struct cpu *new_c = &cpus[cpu_num];
  remove(&old_c->runnable_list, p, &old_c->list_lock);
  push(&new_c->runnable_list, p, &new_c->list_lock);
  p->cpu_num = cpu_num;
  release(&p->lock);
  return 0;
}

int
get_cpu()
{
  if(!myproc()->cpu_num)
    return -1;
  return myproc()->cpu_num;
}

void push(int *head, struct proc *p, struct spinlock *list_lock)
{
  acquire(list_lock);
  acquire(&p->node_lock);
  //printf("push list: %x proc: %d\n", head, p->index);
  p->next = *head;
  *head = p->index;
  release(&p->node_lock);
  release(list_lock);
}

int pop(int *head, struct spinlock *list_lock)
{
  acquire(list_lock);
  int first = *head;
  if(first == -1)
  {
    release(list_lock);
    return -1;  
  }
  struct proc *p1 = &proc[first];
  acquire(&p1->node_lock);
  if(p1->next == -1)
  {
    *head = p1->next;
    p1->next = -1;
    release(&p1->node_lock);
    release(list_lock);
    return p1->index;
  }
  release(list_lock);
  struct proc *p2 = &proc[p1->next];
  acquire(&p2->node_lock);
  while(p2->next != -1)
  {
    release(&p1->node_lock);
    p1 = p2;
    p2 = &proc[p2->next];
    acquire(&p2->node_lock);
  }
  p1->next = p2->next;
  release(&p2->node_lock);
  release(&p1->node_lock);
  //printf("pop list: %x proc: %d\n", head, p2->index);
  return p2->index;
}
 
int remove(int *head, struct proc *p, struct spinlock *list_lock)
{
  acquire(list_lock);
  int first = *head;
  if(first == -1)
  {
    release(list_lock);
    return -1;  
  }
  struct proc *p1 = &proc[first];
  acquire(&p1->node_lock);
  if(p1->index == p->index)
  {
    //printf("p->next is %d\n", p->next);
    *head = p1->next;
    p1->next = -1;
    release(&p1->node_lock);
    release(list_lock);
    return p->index;
  }
  release(list_lock);
  int second = p1->next;
  if(second == -1)
    return -1;
  struct proc *p2 = &proc[second];
  acquire(&p2->node_lock);
  while(p2->index != -1)
  {
    if (p2->index == p->index)
    {
      p1->next = p2->next;
      p2->next = -1;
      break;
    }
    release(&p1->node_lock);
    p1 = p2;
    p2 = &proc[p2->next];
    acquire(&p2->node_lock);
  }
  release(&p2->node_lock);
  release(&p1->node_lock);
  return p2->next;
}


