// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "config.h"

#ifdef HAVE_DL_ITERATE_PHDR
#include <link.h>
#endif

#include "runtime.h"
#include "arch.h"
#include "defs.h"
#include "malloc.h"
#include "go-type.h"

#ifdef USING_SPLIT_STACK

/* FIXME: These are not declared anywhere.  */

extern void __splitstack_getcontext(void *context[10]);

extern void __splitstack_setcontext(void *context[10]);

extern void *__splitstack_makecontext(size_t, void *context[10], size_t *);

extern void * __splitstack_resetcontext(void *context[10], size_t *);

extern void *__splitstack_find(void *, void *, size_t *, void **, void **,
			       void **);

extern void __splitstack_block_signals (int *, int *);

extern void __splitstack_block_signals_context (void *context[10], int *,
						int *);

#endif

#ifndef PTHREAD_STACK_MIN
# define PTHREAD_STACK_MIN 8192
#endif

#if defined(USING_SPLIT_STACK) && defined(LINKER_SUPPORTS_SPLIT_STACK)
# define StackMin PTHREAD_STACK_MIN
#else
# define StackMin ((sizeof(char *) < 8) ? 2 * 1024 * 1024 : 4 * 1024 * 1024)
#endif

uintptr runtime_stacks_sys;

static void gtraceback(G*);

#ifdef __rtems__
#define __thread
#endif

static __thread G *g;

#ifndef SETCONTEXT_CLOBBERS_TLS

static inline void
initcontext(void)
{
}

static inline void
fixcontext(ucontext_t *c __attribute__ ((unused)))
{
}

#else

# if defined(__x86_64__) && defined(__sun__)

// x86_64 Solaris 10 and 11 have a bug: setcontext switches the %fs
// register to that of the thread which called getcontext.  The effect
// is that the address of all __thread variables changes.  This bug
// also affects pthread_self() and pthread_getspecific.  We work
// around it by clobbering the context field directly to keep %fs the
// same.

static __thread greg_t fs;

static inline void
initcontext(void)
{
	ucontext_t c;

	getcontext(&c);
	fs = c.uc_mcontext.gregs[REG_FSBASE];
}

static inline void
fixcontext(ucontext_t* c)
{
	c->uc_mcontext.gregs[REG_FSBASE] = fs;
}

# elif defined(__NetBSD__)

// NetBSD has a bug: setcontext clobbers tlsbase, we need to save
// and restore it ourselves.

static __thread __greg_t tlsbase;

static inline void
initcontext(void)
{
	ucontext_t c;

	getcontext(&c);
	tlsbase = c.uc_mcontext._mc_tlsbase;
}

static inline void
fixcontext(ucontext_t* c)
{
	c->uc_mcontext._mc_tlsbase = tlsbase;
}

# elif defined(__sparc__)

static inline void
initcontext(void)
{
}

static inline void
fixcontext(ucontext_t *c)
{
	/* ??? Using 
	     register unsigned long thread __asm__("%g7");
	     c->uc_mcontext.gregs[REG_G7] = thread;
	   results in
	     error: variable ‘thread’ might be clobbered by \
		‘longjmp’ or ‘vfork’ [-Werror=clobbered]
	   which ought to be false, as %g7 is a fixed register.  */

	if (sizeof (c->uc_mcontext.gregs[REG_G7]) == 8)
		asm ("stx %%g7, %0" : "=m"(c->uc_mcontext.gregs[REG_G7]));
	else
		asm ("st %%g7, %0" : "=m"(c->uc_mcontext.gregs[REG_G7]));
}

# else

#  error unknown case for SETCONTEXT_CLOBBERS_TLS

# endif

#endif

// ucontext_arg returns a properly aligned ucontext_t value.  On some
// systems a ucontext_t value must be aligned to a 16-byte boundary.
// The g structure that has fields of type ucontext_t is defined in
// Go, and Go has no simple way to align a field to such a boundary.
// So we make the field larger in runtime2.go and pick an appropriate
// offset within the field here.
static ucontext_t*
ucontext_arg(void** go_ucontext)
{
	uintptr_t p = (uintptr_t)go_ucontext;
	size_t align = __alignof__(ucontext_t);
	if(align > 16) {
		// We only ensured space for up to a 16 byte alignment
		// in libgo/go/runtime/runtime2.go.
		runtime_throw("required alignment of ucontext_t too large");
	}
	p = (p + align - 1) &~ (uintptr_t)(align - 1);
	return (ucontext_t*)p;
}

// We can not always refer to the TLS variables directly.  The
// compiler will call tls_get_addr to get the address of the variable,
// and it may hold it in a register across a call to schedule.  When
// we get back from the call we may be running in a different thread,
// in which case the register now points to the TLS variable for a
// different thread.  We use non-inlinable functions to avoid this
// when necessary.

G* runtime_g(void) __attribute__ ((noinline, no_split_stack));

G*
runtime_g(void)
{
	return g;
}

M* runtime_m(void) __attribute__ ((noinline, no_split_stack));

M*
runtime_m(void)
{
	if(g == nil)
		return nil;
	return g->m;
}

// Set g.
void
runtime_setg(G* gp)
{
	g = gp;
}

void runtime_newosproc(M *)
  __asm__(GOSYM_PREFIX "runtime.newosproc");

// Start a new thread.
void
runtime_newosproc(M *mp)
{
	pthread_attr_t attr;
	sigset_t clear, old;
	pthread_t tid;
	int tries;
	int ret;

	if(pthread_attr_init(&attr) != 0)
		runtime_throw("pthread_attr_init");
	if(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
		runtime_throw("pthread_attr_setdetachstate");

	// Block signals during pthread_create so that the new thread
	// starts with signals disabled.  It will enable them in minit.
	sigfillset(&clear);

#ifdef SIGTRAP
	// Blocking SIGTRAP reportedly breaks gdb on Alpha GNU/Linux.
	sigdelset(&clear, SIGTRAP);
#endif

	sigemptyset(&old);
	pthread_sigmask(SIG_BLOCK, &clear, &old);

	for (tries = 0; tries < 20; tries++) {
		ret = pthread_create(&tid, &attr, runtime_mstart, mp);
		if (ret != EAGAIN) {
			break;
		}
		runtime_usleep((tries + 1) * 1000); // Milliseconds.
	}

	pthread_sigmask(SIG_SETMASK, &old, nil);

	if (ret != 0) {
		runtime_printf("pthread_create failed: %d\n", ret);
		runtime_throw("pthread_create");
	}
}

// First function run by a new goroutine.  This replaces gogocall.
static void
kickoff(void)
{
	void (*fn)(void*);
	void *param;

	if(g->traceback != nil)
		gtraceback(g);

	fn = (void (*)(void*))(g->entry);
	param = g->param;
	g->entry = 0;
	g->param = nil;
	fn(param);
	runtime_goexit1();
}

// Switch context to a different goroutine.  This is like longjmp.
void runtime_gogo(G*) __attribute__ ((noinline));
void
runtime_gogo(G* newg)
{
#ifdef USING_SPLIT_STACK
	__splitstack_setcontext(&newg->stackcontext[0]);
#endif
	g = newg;
	newg->fromgogo = true;
	fixcontext(ucontext_arg(&newg->context[0]));
	setcontext(ucontext_arg(&newg->context[0]));
	runtime_throw("gogo setcontext returned");
}

// Save context and call fn passing g as a parameter.  This is like
// setjmp.  Because getcontext always returns 0, unlike setjmp, we use
// g->fromgogo as a code.  It will be true if we got here via
// setcontext.  g == nil the first time this is called in a new m.
void runtime_mcall(void (*)(G*)) __attribute__ ((noinline));
void
runtime_mcall(void (*pfn)(G*))
{
	M *mp;
	G *gp;
#ifndef USING_SPLIT_STACK
	void *afterregs;
#endif

	// Ensure that all registers are on the stack for the garbage
	// collector.
	__builtin_unwind_init();

	gp = g;
	mp = gp->m;
	if(gp == mp->g0)
		runtime_throw("runtime: mcall called on m->g0 stack");

	if(gp != nil) {

#ifdef USING_SPLIT_STACK
		__splitstack_getcontext(&g->stackcontext[0]);
#else
		// We have to point to an address on the stack that is
		// below the saved registers.
		gp->gcnextsp = &afterregs;
#endif
		gp->fromgogo = false;
		getcontext(ucontext_arg(&gp->context[0]));

		// When we return from getcontext, we may be running
		// in a new thread.  That means that g may have
		// changed.  It is a global variables so we will
		// reload it, but the address of g may be cached in
		// our local stack frame, and that address may be
		// wrong.  Call the function to reload the value for
		// this thread.
		gp = runtime_g();
		mp = gp->m;

		if(gp->traceback != nil)
			gtraceback(gp);
	}
	if (gp == nil || !gp->fromgogo) {
#ifdef USING_SPLIT_STACK
		__splitstack_setcontext(&mp->g0->stackcontext[0]);
#endif
		mp->g0->entry = (uintptr)pfn;
		mp->g0->param = gp;

		// It's OK to set g directly here because this case
		// can not occur if we got here via a setcontext to
		// the getcontext call just above.
		g = mp->g0;

		fixcontext(ucontext_arg(&mp->g0->context[0]));
		setcontext(ucontext_arg(&mp->g0->context[0]));
		runtime_throw("runtime: mcall function returned");
	}
}

// mcall called from Go code.
void gomcall(FuncVal *)
  __asm__ (GOSYM_PREFIX "runtime.mcall");

void
gomcall(FuncVal *fv)
{
	runtime_mcall((void*)fv->fn);
}

// Goroutine scheduler
// The scheduler's job is to distribute ready-to-run goroutines over worker threads.
//
// The main concepts are:
// G - goroutine.
// M - worker thread, or machine.
// P - processor, a resource that is required to execute Go code.
//     M must have an associated P to execute Go code, however it can be
//     blocked or in a syscall w/o an associated P.
//
// Design doc at http://golang.org/s/go11sched.

extern bool* runtime_getCgoHasExtraM()
  __asm__ (GOSYM_PREFIX "runtime.getCgoHasExtraM");
extern G* allocg(void)
  __asm__ (GOSYM_PREFIX "runtime.allocg");

Sched*	runtime_sched;
int32	runtime_ncpu;

bool	runtime_isarchive;

static void exitsyscall0(G*);
static bool exitsyscallfast(void);

extern void mstart1(void)
  __asm__(GOSYM_PREFIX "runtime.mstart1");
extern void stopm(void)
  __asm__(GOSYM_PREFIX "runtime.stopm");
extern void handoffp(P*)
  __asm__(GOSYM_PREFIX "runtime.handoffp");
extern void wakep(void)
  __asm__(GOSYM_PREFIX "runtime.wakep");
extern void stoplockedm(void)
  __asm__(GOSYM_PREFIX "runtime.stoplockedm");
extern void schedule(void)
  __asm__(GOSYM_PREFIX "runtime.schedule");
extern void execute(G*, bool)
  __asm__(GOSYM_PREFIX "runtime.execute");
extern G* gfget(P*)
  __asm__(GOSYM_PREFIX "runtime.gfget");
extern void acquirep(P*)
  __asm__(GOSYM_PREFIX "runtime.acquirep");
extern P* releasep(void)
  __asm__(GOSYM_PREFIX "runtime.releasep");
extern void incidlelocked(int32)
  __asm__(GOSYM_PREFIX "runtime.incidlelocked");
extern void globrunqput(G*)
  __asm__(GOSYM_PREFIX "runtime.globrunqput");
extern P* pidleget(void)
  __asm__(GOSYM_PREFIX "runtime.pidleget");

bool runtime_isstarted;

// Used to determine the field alignment.

struct field_align
{
  char c;
  Hchan *p;
};

void getTraceback(G*, G*) __asm__(GOSYM_PREFIX "runtime.getTraceback");

// getTraceback stores a traceback of gp in the g's traceback field
// and then returns to me.  We expect that gp's traceback is not nil.
// It works by saving me's current context, and checking gp's traceback field.
// If gp's traceback field is not nil, it starts running gp.
// In places where we call getcontext, we check the traceback field.
// If it is not nil, we collect a traceback, and then return to the
// goroutine stored in the traceback field, which is me.
void getTraceback(G* me, G* gp)
{
#ifdef USING_SPLIT_STACK
	__splitstack_getcontext(&me->stackcontext[0]);
#endif
	getcontext(ucontext_arg(&me->context[0]));

	if (gp->traceback != nil) {
		runtime_gogo(gp);
	}
}

// Do a stack trace of gp, and then restore the context to
// gp->dotraceback.

static void
gtraceback(G* gp)
{
	Traceback* traceback;

	traceback = gp->traceback;
	gp->traceback = nil;
	if(gp->m != nil && gp->m != g->m)
		runtime_throw("gtraceback: m is not nil");
	gp->m = traceback->gp->m;
	traceback->c = runtime_callers(1, traceback->locbuf,
		sizeof traceback->locbuf / sizeof traceback->locbuf[0], false);
	gp->m = nil;
	runtime_gogo(traceback->gp);
}

// Called to set up the context information for a new M.

void mstartInitContext(G*, void*)
	__asm__(GOSYM_PREFIX "runtime.mstartInitContext");

void
mstartInitContext(G *gp, void *stack __attribute__ ((unused)))
{
	initcontext();

	// Record top of stack for use by mcall.
	// Once we call schedule we're never coming back,
	// so other calls can reuse this stack space.
#ifdef USING_SPLIT_STACK
	__splitstack_getcontext(&gp->stackcontext[0]);
#else
	gp->gcinitialsp = stack;
	// Setting gcstacksize to 0 is a marker meaning that gcinitialsp
	// is the top of the stack, not the bottom.
	gp->gcstacksize = 0;
	gp->gcnextsp = stack;
#endif

	// Save the currently active context.  This will return
	// multiple times via the setcontext call in mcall.
	getcontext(ucontext_arg(&gp->context[0]));

	if(gp->traceback != nil) {
		// Got here from getTraceback.
		// I'm not sure this ever actually happens--getTraceback
		// may always go to the getcontext call in mcall.
		gtraceback(gp);
	}

	if(gp->entry != 0) {
		// Got here from mcall.
		void (*pfn)(G*) = (void (*)(G*))gp->entry;
		G* gp1 = (G*)gp->param;
		gp->entry = 0;
		gp->param = nil;
		pfn(gp1);
		*(int*)0x21 = 0x21;
	}

	// Initial call to getcontext--starting thread.

#ifdef USING_SPLIT_STACK
	{
		int dont_block_signals = 0;
		__splitstack_block_signals(&dont_block_signals, nil);
	}
#endif

	mstart1();
}

typedef struct CgoThreadStart CgoThreadStart;
struct CgoThreadStart
{
	M *m;
	G *g;
	uintptr *tls;
	void (*fn)(void);
};

void setGContext(void) __asm__ (GOSYM_PREFIX "runtime.setGContext");

// setGContext sets up a new goroutine context for the current g.
void
setGContext()
{
	int val;
	G *gp;

	initcontext();
	gp = g;
	gp->entry = 0;
	gp->param = nil;
#ifdef USING_SPLIT_STACK
	__splitstack_getcontext(&gp->stackcontext[0]);
	val = 0;
	__splitstack_block_signals(&val, nil);
#else
	gp->gcinitialsp = &val;
	gp->gcstack = nil;
	gp->gcstacksize = 0;
	gp->gcnextsp = &val;
#endif
	getcontext(ucontext_arg(&gp->context[0]));

	if(gp->entry != 0) {
		// Got here from mcall.
		void (*pfn)(G*) = (void (*)(G*))gp->entry;
		G* gp1 = (G*)gp->param;
		gp->entry = 0;
		gp->param = nil;
		pfn(gp1);
		*(int*)0x22 = 0x22;
	}
}

void makeGContext(G*, byte*, uintptr)
	__asm__(GOSYM_PREFIX "runtime.makeGContext");

// makeGContext makes a new context for a g.
void
makeGContext(G* gp, byte* sp, uintptr spsize) {
	ucontext_t *uc;

	uc = ucontext_arg(&gp->context[0]);
	getcontext(uc);
	uc->uc_stack.ss_sp = sp;
	uc->uc_stack.ss_size = (size_t)spsize;
	makecontext(uc, kickoff, 0);
}

// The goroutine g is about to enter a system call.
// Record that it's not using the cpu anymore.
// This is called only from the go syscall library and cgocall,
// not from the low-level system calls used by the runtime.
//
// Entersyscall cannot split the stack: the runtime_gosave must
// make g->sched refer to the caller's stack segment, because
// entersyscall is going to return immediately after.

void runtime_entersyscall(int32) __attribute__ ((no_split_stack));
static void doentersyscall(uintptr, uintptr)
  __attribute__ ((no_split_stack, noinline));

void
runtime_entersyscall(int32 dummy __attribute__ ((unused)))
{
	// Save the registers in the g structure so that any pointers
	// held in registers will be seen by the garbage collector.
	getcontext(ucontext_arg(&g->gcregs[0]));

	// Do the work in a separate function, so that this function
	// doesn't save any registers on its own stack.  If this
	// function does save any registers, we might store the wrong
	// value in the call to getcontext.
	//
	// FIXME: This assumes that we do not need to save any
	// callee-saved registers to access the TLS variable g.  We
	// don't want to put the ucontext_t on the stack because it is
	// large and we can not split the stack here.
	doentersyscall((uintptr)runtime_getcallerpc(&dummy),
		       (uintptr)runtime_getcallersp(&dummy));
}

static void
doentersyscall(uintptr pc, uintptr sp)
{
	// Disable preemption because during this function g is in _Gsyscall status,
	// but can have inconsistent g->sched, do not let GC observe it.
	g->m->locks++;

	// Leave SP around for GC and traceback.
#ifdef USING_SPLIT_STACK
	{
	  size_t gcstacksize;
	  g->gcstack = __splitstack_find(nil, nil, &gcstacksize,
					 &g->gcnextsegment, &g->gcnextsp,
					 &g->gcinitialsp);
	  g->gcstacksize = (uintptr)gcstacksize;
	}
#else
	{
		void *v;

		g->gcnextsp = (byte *) &v;
	}
#endif

	g->syscallsp = sp;
	g->syscallpc = pc;

	g->atomicstatus = _Gsyscall;

	if(runtime_atomicload(&runtime_sched->sysmonwait)) {  // TODO: fast atomic
		runtime_lock(&runtime_sched->lock);
		if(runtime_atomicload(&runtime_sched->sysmonwait)) {
			runtime_atomicstore(&runtime_sched->sysmonwait, 0);
			runtime_notewakeup(&runtime_sched->sysmonnote);
		}
		runtime_unlock(&runtime_sched->lock);
	}

	g->m->mcache = nil;
	((P*)(g->m->p))->m = 0;
	runtime_atomicstore(&((P*)g->m->p)->status, _Psyscall);
	if(runtime_atomicload(&runtime_sched->gcwaiting)) {
		runtime_lock(&runtime_sched->lock);
		if (runtime_sched->stopwait > 0 && runtime_cas(&((P*)g->m->p)->status, _Psyscall, _Pgcstop)) {
			if(--runtime_sched->stopwait == 0)
				runtime_notewakeup(&runtime_sched->stopnote);
		}
		runtime_unlock(&runtime_sched->lock);
	}

	g->m->locks--;
}

// The same as runtime_entersyscall(), but with a hint that the syscall is blocking.
void
runtime_entersyscallblock(int32 dummy __attribute__ ((unused)))
{
	P *p;

	g->m->locks++;  // see comment in entersyscall

	// Leave SP around for GC and traceback.
#ifdef USING_SPLIT_STACK
	{
	  size_t gcstacksize;
	  g->gcstack = __splitstack_find(nil, nil, &gcstacksize,
					 &g->gcnextsegment, &g->gcnextsp,
					 &g->gcinitialsp);
	  g->gcstacksize = (uintptr)gcstacksize;
	}
#else
	g->gcnextsp = (byte *) &p;
#endif

	// Save the registers in the g structure so that any pointers
	// held in registers will be seen by the garbage collector.
	getcontext(ucontext_arg(&g->gcregs[0]));

	g->syscallpc = (uintptr)runtime_getcallerpc(&dummy);
	g->syscallsp = (uintptr)runtime_getcallersp(&dummy);

	g->atomicstatus = _Gsyscall;

	p = releasep();
	handoffp(p);
	if(g->isbackground)  // do not consider blocked scavenger for deadlock detection
		incidlelocked(1);

	g->m->locks--;
}

// The goroutine g exited its system call.
// Arrange for it to run on a cpu again.
// This is called only from the go syscall library, not
// from the low-level system calls used by the runtime.
void
runtime_exitsyscall(int32 dummy __attribute__ ((unused)))
{
	G *gp;

	gp = g;
	gp->m->locks++;  // see comment in entersyscall

	if(gp->isbackground)  // do not consider blocked scavenger for deadlock detection
		incidlelocked(-1);

	gp->waitsince = 0;
	if(exitsyscallfast()) {
		// There's a cpu for us, so we can run.
		((P*)gp->m->p)->syscalltick++;
		gp->atomicstatus = _Grunning;
		// Garbage collector isn't running (since we are),
		// so okay to clear gcstack and gcsp.
#ifdef USING_SPLIT_STACK
		gp->gcstack = nil;
#endif
		gp->gcnextsp = nil;
		runtime_memclr(&gp->gcregs[0], sizeof gp->gcregs);
		gp->syscallsp = 0;
		gp->m->locks--;
		return;
	}

	gp->m->locks--;

	// Call the scheduler.
	runtime_mcall(exitsyscall0);

	// Scheduler returned, so we're allowed to run now.
	// Delete the gcstack information that we left for
	// the garbage collector during the system call.
	// Must wait until now because until gosched returns
	// we don't know for sure that the garbage collector
	// is not running.
#ifdef USING_SPLIT_STACK
	gp->gcstack = nil;
#endif
	gp->gcnextsp = nil;
	runtime_memclr(&gp->gcregs[0], sizeof gp->gcregs);

	gp->syscallsp = 0;

	// Note that this gp->m might be different than the earlier
	// gp->m after returning from runtime_mcall.
	((P*)gp->m->p)->syscalltick++;
}

static bool
exitsyscallfast(void)
{
	G *gp;
	P *p;

	gp = g;

	// Freezetheworld sets stopwait but does not retake P's.
	if(runtime_sched->stopwait) {
		gp->m->p = 0;
		return false;
	}

	// Try to re-acquire the last P.
	if(gp->m->p && ((P*)gp->m->p)->status == _Psyscall && runtime_cas(&((P*)gp->m->p)->status, _Psyscall, _Prunning)) {
		// There's a cpu for us, so we can run.
		gp->m->mcache = ((P*)gp->m->p)->mcache;
		((P*)gp->m->p)->m = (uintptr)gp->m;
		return true;
	}
	// Try to get any other idle P.
	gp->m->p = 0;
	if(runtime_sched->pidle) {
		runtime_lock(&runtime_sched->lock);
		p = pidleget();
		if(p && runtime_atomicload(&runtime_sched->sysmonwait)) {
			runtime_atomicstore(&runtime_sched->sysmonwait, 0);
			runtime_notewakeup(&runtime_sched->sysmonnote);
		}
		runtime_unlock(&runtime_sched->lock);
		if(p) {
			acquirep(p);
			return true;
		}
	}
	return false;
}

// runtime_exitsyscall slow path on g0.
// Failed to acquire P, enqueue gp as runnable.
static void
exitsyscall0(G *gp)
{
	M *m;
	P *p;

	m = g->m;
	gp->atomicstatus = _Grunnable;
	gp->m = nil;
	m->curg = nil;
	runtime_lock(&runtime_sched->lock);
	p = pidleget();
	if(p == nil)
		globrunqput(gp);
	else if(runtime_atomicload(&runtime_sched->sysmonwait)) {
		runtime_atomicstore(&runtime_sched->sysmonwait, 0);
		runtime_notewakeup(&runtime_sched->sysmonnote);
	}
	runtime_unlock(&runtime_sched->lock);
	if(p) {
		acquirep(p);
		execute(gp, false);  // Never returns.
	}
	if(m->lockedg) {
		// Wait until another thread schedules gp and so m again.
		stoplockedm();
		execute(gp, false);  // Never returns.
	}
	stopm();
	schedule();  // Never returns.
}

void syscall_entersyscall(void)
  __asm__(GOSYM_PREFIX "syscall.Entersyscall");

void syscall_entersyscall(void) __attribute__ ((no_split_stack));

void
syscall_entersyscall()
{
  runtime_entersyscall(0);
}

void syscall_exitsyscall(void)
  __asm__(GOSYM_PREFIX "syscall.Exitsyscall");

void syscall_exitsyscall(void) __attribute__ ((no_split_stack));

void
syscall_exitsyscall()
{
  runtime_exitsyscall(0);
}

// Allocate a new g, with a stack big enough for stacksize bytes.
G*
runtime_malg(bool allocatestack, bool signalstack, byte** ret_stack, uintptr* ret_stacksize)
{
	uintptr stacksize;
	G *newg;
	byte* unused_stack;
	uintptr unused_stacksize;
#if USING_SPLIT_STACK
	int dont_block_signals = 0;
	size_t ss_stacksize;
#endif

	if (ret_stack == nil) {
		ret_stack = &unused_stack;
	}
	if (ret_stacksize == nil) {
		ret_stacksize = &unused_stacksize;
	}
	newg = allocg();
	if(allocatestack) {
		stacksize = StackMin;
		if(signalstack) {
			stacksize = 32 * 1024; // OS X wants >= 8K, GNU/Linux >= 2K
#ifdef SIGSTKSZ
			if(stacksize < SIGSTKSZ)
				stacksize = SIGSTKSZ;
#endif
		}

#if USING_SPLIT_STACK
		*ret_stack = __splitstack_makecontext(stacksize,
						      &newg->stackcontext[0],
						      &ss_stacksize);
		*ret_stacksize = (uintptr)ss_stacksize;
		__splitstack_block_signals_context(&newg->stackcontext[0],
						   &dont_block_signals, nil);
#else
                // In 64-bit mode, the maximum Go allocation space is
                // 128G.  Our stack size is 4M, which only permits 32K
                // goroutines.  In order to not limit ourselves,
                // allocate the stacks out of separate memory.  In
                // 32-bit mode, the Go allocation space is all of
                // memory anyhow.
		if(sizeof(void*) == 8) {
			void *p = runtime_SysAlloc(stacksize, &mstats()->other_sys);
			if(p == nil)
				runtime_throw("runtime: cannot allocate memory for goroutine stack");
			*ret_stack = (byte*)p;
		} else {
			*ret_stack = runtime_mallocgc(stacksize, 0, FlagNoProfiling|FlagNoGC);
			runtime_xadd(&runtime_stacks_sys, stacksize);
		}
		*ret_stacksize = (uintptr)stacksize;
		newg->gcinitialsp = *ret_stack;
		newg->gcstacksize = (uintptr)stacksize;
#endif
	}
	return newg;
}

void resetNewG(G*, void **, uintptr*)
  __asm__(GOSYM_PREFIX "runtime.resetNewG");

// Reset stack information for g pulled out of the cache to start a
// new goroutine.
void
resetNewG(G *newg, void **sp, uintptr *spsize)
{
#ifdef USING_SPLIT_STACK
  int dont_block_signals = 0;
  size_t ss_spsize;

  *sp = __splitstack_resetcontext(&newg->stackcontext[0], &ss_spsize);
  *spsize = ss_spsize;
  __splitstack_block_signals_context(&newg->stackcontext[0],
				     &dont_block_signals, nil);
#else
  *sp = newg->gcinitialsp;
  *spsize = newg->gcstacksize;
  if(*spsize == 0)
    runtime_throw("bad spsize in resetNewG");
  newg->gcnextsp = *sp;
#endif
}

void
runtime_Breakpoint(void)
{
	runtime_breakpoint();
}

static struct {
	uint32 lock;
	int32 hz;
} prof;

static void System(void) {}
static void GC(void) {}

// Called if we receive a SIGPROF signal.
void
runtime_sigprof()
{
	M *mp = g->m;
	int32 n, i;
	bool traceback;
	uintptr pcbuf[TracebackMaxFrames];
	Location locbuf[TracebackMaxFrames];
	Slice stk;

	if(prof.hz == 0)
		return;

	if(mp == nil)
		return;

	// Profiling runs concurrently with GC, so it must not allocate.
	mp->mallocing++;

	traceback = true;

	if(mp->mcache == nil)
		traceback = false;

	n = 0;

	if(runtime_atomicload(&runtime_in_callers) > 0) {
		// If SIGPROF arrived while already fetching runtime
		// callers we can have trouble on older systems
		// because the unwind library calls dl_iterate_phdr
		// which was not recursive in the past.
		traceback = false;
	}

	if(traceback) {
		n = runtime_callers(0, locbuf, nelem(locbuf), false);
		for(i = 0; i < n; i++)
			pcbuf[i] = locbuf[i].pc;
	}
	if(!traceback || n <= 0) {
		n = 2;
		pcbuf[0] = (uintptr)runtime_getcallerpc(&n);
		if(mp->gcing || mp->helpgc)
			pcbuf[1] = (uintptr)GC;
		else
			pcbuf[1] = (uintptr)System;
	}

	if (prof.hz != 0) {
		stk.__values = &pcbuf[0];
		stk.__count = n;
		stk.__capacity = n;

		// Simple cas-lock to coordinate with setcpuprofilerate.
		while (!runtime_cas(&prof.lock, 0, 1)) {
			runtime_osyield();
		}
		if (prof.hz != 0) {
			runtime_cpuprofAdd(stk);
		}
		runtime_atomicstore(&prof.lock, 0);
	}

	mp->mallocing--;
}

// Arrange to call fn with a traceback hz times a second.
void
runtime_setcpuprofilerate_m(int32 hz)
{
	// Force sane arguments.
	if(hz < 0)
		hz = 0;

	// Disable preemption, otherwise we can be rescheduled to another thread
	// that has profiling enabled.
	g->m->locks++;

	// Stop profiler on this thread so that it is safe to lock prof.
	// if a profiling signal came in while we had prof locked,
	// it would deadlock.
	runtime_resetcpuprofiler(0);

	while (!runtime_cas(&prof.lock, 0, 1)) {
		runtime_osyield();
	}
	prof.hz = hz;
	runtime_atomicstore(&prof.lock, 0);

	runtime_lock(&runtime_sched->lock);
	runtime_sched->profilehz = hz;
	runtime_unlock(&runtime_sched->lock);

	if(hz != 0)
		runtime_resetcpuprofiler(hz);

	g->m->locks--;
}

// Return whether we are waiting for a GC.  This gc toolchain uses
// preemption instead.
bool
runtime_gcwaiting(void)
{
	return runtime_sched->gcwaiting;
}
