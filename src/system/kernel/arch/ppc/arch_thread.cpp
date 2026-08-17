/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Axel Dörfler <axeld@pinc-software.de>
 * 		Ingo Weinhold <bonefish@cs.tu-berlin.de>
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <arch/cpu.h>
#include <commpage.h>
#include <arch/int.h>
#include <interrupts.h>
#include <smp.h>
#include <arch/thread.h>
#include <boot/stage2.h>
#include <kernel.h>
#include <thread.h>
#include <ksignal.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>
//#include <arch/vm_translation_map.h>

#include <string.h>

// Valid initial arch_thread state. We just memcpy() it when initializing
// a new thread structure.
static struct arch_thread sInitialState;

// Helper function for thread creation, defined in arch_asm.S.
extern "C" void ppc_kernel_thread_root();


void
ppc_push_iframe(struct iframe_stack *stack, struct iframe *frame)
{
	ASSERT(stack->index < IFRAME_TRACE_DEPTH);
	stack->frames[stack->index++] = frame;
}


void
ppc_pop_iframe(struct iframe_stack *stack)
{
	ASSERT(stack->index > 0);
	stack->index--;
}


/**	Returns the current iframe structure of the running thread.
 *	This function must only be called in a context where it's actually
 *	sure that such iframe exists; ie. from syscalls, but usually not
 *	from standard kernel threads.
 */
static struct iframe *
ppc_get_current_iframe(void)
{
	Thread *thread = thread_get_current_thread();

	ASSERT(thread->arch_info.iframes.index >= 0);
	return thread->arch_info.iframes.frames[thread->arch_info.iframes.index - 1];
}


/** \brief Returns the current thread's topmost (i.e. most recent)
 *  userland->kernel transition iframe (usually the first one, save for
 *  interrupts in signal handlers).
 *  \return The iframe, or \c NULL, if there is no such iframe (e.g. when
 *          the thread is a kernel thread).
 */
struct iframe *
ppc_get_user_iframe(void)
{
	Thread *thread = thread_get_current_thread();
	int i;

	for (i = thread->arch_info.iframes.index - 1; i >= 0; i--) {
		struct iframe *frame = thread->arch_info.iframes.frames[i];
		if (frame->srr1 & MSR_PRIVILEGE_LEVEL)
			return frame;
	}

	return NULL;
}


// #pragma mark -


status_t
arch_thread_init(struct kernel_args *args)
{
	// Initialize the static initial arch_thread state (sInitialState).
	// Currently nothing to do, i.e. zero initialized is just fine.

	return B_OK;
}


status_t
arch_team_init_team_struct(Team *team, bool kernel)
{
	// Nothing to do. The structure is empty.
	return B_OK;
}


status_t
arch_thread_init_thread_struct(Thread *thread)
{
	// set up an initial state (stack & fpu)
	memcpy(&thread->arch_info, &sInitialState, sizeof(struct arch_thread));

	return B_OK;
}


void
arch_thread_init_kthread_stack(Thread* thread, void* _stack, void* _stackTop,
	void (*function)(void*), const void* data)
{
	addr_t *kstackTop = (addr_t *)_stackTop;

	// space for frame pointer and return address, and stack frames must be
	// 16 byte aligned
	kstackTop -= 2;
	kstackTop = (addr_t*)((addr_t)kstackTop & ~0xf);

	// AltiVec save area: v20-v31 + VSCR + VRSAVE = 224 bytes, which
	// ppc_context_switch() restores from ABOVE the general frame (it carves
	// this off first, while r1 is still 16-byte aligned). It must be reserved
	// here too, or the first switch into a brand new thread pops a vector area
	// that was never pushed and reads straight off the top of the stack.
	// Reserved unconditionally: on a CPU with no vector unit the switch skips
	// the block and these bytes simply go unused, which costs nothing and
	// keeps this frame layout independent of the CPU it ends up running on.
	kstackTop -= 224 / sizeof(addr_t);
	memset(kstackTop, 0, 224);

	// LR, CR, r2, r3, r13-r31, f13-f31, as restored by ppc_context_switch()
	kstackTop -= 23 + 2 * 19;
	memset(kstackTop, 0, (23 + 2 * 19) * sizeof(addr_t));

	// Let LR point directly at the thread's entry function, and smuggle
	// "data" into r3 (the first argument register in the PowerPC SVR4
	// calling convention) - when ppc_context_switch() restores this brand
	// new thread for the first time, its closing "blr" branches straight
	// into function(data). This mirrors every other working architecture's
	// implementation of this function (e.g. arch/arm/arch_thread.cpp); the
	// old three-function ppc_kernel_thread_root() trampoline this function
	// used to target is a leftover from a since-removed thread-creation
	// API and no longer matches this function's (function, data) signature.
	kstackTop[0] = (addr_t)function;
	kstackTop[3] = (addr_t)data;

	// save this stack position
	thread->arch_info.sp = (void *)kstackTop;
}


status_t
arch_thread_init_tls(Thread *thread)
{
	thread->user_local_storage =
		thread->user_stack_base + thread->user_stack_size;
	return B_OK;
}


extern "C" void ppc_sync_kernel_stack(Thread* thread);


void
arch_thread_context_switch(Thread *t_from, Thread *t_to)
{
	// Record the new thread's kernel stack in this CPU's exception context.
	// The exception entry code (ppc_exception_tail) reads context->kernel_stack
	// to know which stack to switch to when an exception is taken from user
	// mode. (The previous EAR write was dead - nothing ever read EAR back.)
	ppc_sync_kernel_stack(t_to);

    // switch the asids if we need to
	if (t_to->team->address_space != NULL) {
		// the target thread has is user space
		if (t_from->team != t_to->team) {
			// switching to a new address space
			ppc_translation_map_change_asid(
				t_to->team->address_space->TranslationMap());
		}
	}

	ppc_context_switch(&t_from->arch_info.sp, t_to->arch_info.sp);
}


void
arch_thread_dump_info(void *info)
{
	struct arch_thread *at = (struct arch_thread *)info;

	dprintf("\tsp: %p\n", at->sp);
}


extern "C" void ppc_enter_userspace(struct iframe *frame);

status_t
arch_thread_enter_userspace(Thread *thread, addr_t entry, void *arg1, void *arg2)
{
	addr_t stackTop = thread->user_stack_base + thread->user_stack_size;

	// Make sure this CPU's exception context points at our kernel stack, so
	// the first syscall/interrupt taken from user mode lands on a valid stack.
	ppc_sync_kernel_stack(thread);

	// Build a synthetic iframe describing the initial user state and let the
	// standard exception-return path (rfi) drop us into user mode.
	struct iframe frame;
	memset(&frame, 0, sizeof(frame));

	frame.srr0 = entry;
	frame.srr1 = MSR_PRIVILEGE_LEVEL | MSR_EXCEPTIONS_ENABLED
		| MSR_FP_AVAILABLE | MSR_MACHINE_CHECK_ENABLED
		/* AltiVec, but only where the unit exists - see gPPCHasAltiVec */
		| (gPPCHasAltiVec ? MSR_VEC_AVAILABLE : 0)
		| MSR_INST_ADDRESS_TRANSLATION | MSR_DATA_ADDRESS_TRANSLATION;

	// 16-byte align the user stack and lay down an empty initial frame
	// (back chain NULL) per the SysV ABI.
	stackTop &= ~0xf;
	stackTop -= 16;
	*(uint32*)stackTop = 0;

	frame.r1 = stackTop;					// user stack pointer
	frame.r2 = thread->user_local_storage;	// TLS pointer (ppc32)
	frame.r3 = (uint32)arg1;
	frame.r4 = (uint32)arg2;

	// When the thread's top function (libroot's thread_entry()) returns, its
	// blr branches to whatever we leave in LR. Point it at the commpage
	// thread-exit stub so the thread cleanly calls exit_thread() with its
	// return value (in r3) rather than branching to a null LR and faulting
	// at address 0.
	addr_t commPageAddress = (addr_t)thread->team->commpage_address;
	if (commPageAddress != 0) {
		frame.lr = ((addr_t*)commPageAddress)[COMMPAGE_ENTRY_PPC_THREAD_EXIT]
			+ commPageAddress;
	}

	disable_interrupts();
	ppc_enter_userspace(&frame);

	// never reached
	return B_ERROR;
}


bool
arch_on_signal_stack(Thread *thread)
{
	struct iframe* frame = ppc_get_user_iframe();
	if (frame == NULL)
		return false;

	return frame->r1 >= thread->signal_stack_base
		&& frame->r1 < thread->signal_stack_base + thread->signal_stack_size;
}


static uint8*
get_signal_stack(Thread* thread, struct iframe* frame, struct sigaction* action,
	size_t spaceNeeded)
{
	// use the alternate signal stack if we should and can
	if (thread->signal_stack_enabled && (action->sa_flags & SA_ONSTACK) != 0
		&& (frame->r1 < thread->signal_stack_base
			|| frame->r1 >= thread->signal_stack_base
				+ thread->signal_stack_size)) {
		addr_t stackTop = thread->signal_stack_base + thread->signal_stack_size;
		return (uint8*)ROUNDDOWN(stackTop - spaceNeeded, 16);
	}

	return (uint8*)ROUNDDOWN(frame->r1 - spaceNeeded, 16);
}


status_t
arch_setup_signal_frame(Thread *thread, struct sigaction *sa,
	struct signal_frame_data *signalFrameData)
{
	struct iframe* frame = ppc_get_user_iframe();
	if (frame == NULL) {
		panic("arch_setup_signal_frame(): No user iframe!");
		return B_ERROR;
	}

	// save the volatile register state into the signal context (vregs)
	mcontext_t& regs = signalFrameData->context.uc_mcontext;
	regs.pc = frame->srr0;
	regs.r0 = frame->r0;
	regs.r1 = frame->r1;
	regs.r2 = frame->r2;
	regs.r3 = frame->r3;
	regs.r4 = frame->r4;
	regs.r5 = frame->r5;
	regs.r6 = frame->r6;
	regs.r7 = frame->r7;
	regs.r8 = frame->r8;
	regs.r9 = frame->r9;
	regs.r10 = frame->r10;
	regs.r11 = frame->r11;
	regs.r12 = frame->r12;
	regs.f0 = frame->f0;
	regs.f1 = frame->f1;
	regs.f2 = frame->f2;
	regs.f3 = frame->f3;
	regs.f4 = frame->f4;
	regs.f5 = frame->f5;
	regs.f6 = frame->f6;
	regs.f7 = frame->f7;
	regs.f8 = frame->f8;
	regs.f9 = frame->f9;
	regs.f10 = frame->f10;
	regs.f11 = frame->f11;
	regs.f12 = frame->f12;
	regs.f13 = frame->f13;
	regs.fpscr = frame->fpscr;
	regs.ctr = frame->ctr;
	regs.xer = frame->xer;
	regs.cr = frame->cr;
	regs.msr = frame->srr1;
	regs.lr = frame->lr;
	// fill in the stack info and the syscall-restart return value
	signal_get_user_stack(frame->r1, &signalFrameData->context.uc_stack);
	signalFrameData->syscall_restart_return_value = frame->r3;

	// Reserve a 16-byte linkage area below the signal_frame_data blob so the
	// handler trampoline (a normal ppc function) can store its return address
	// at sp+4 without clobbering the data.
	size_t dataSize = (sizeof(*signalFrameData) + 15) & ~(size_t)15;
	uint8* userStack = get_signal_stack(thread, frame, sa, dataSize + 16);
	addr_t dataAddress = (addr_t)userStack + 16;

	// copy the signal frame data onto the user stack
	status_t error = user_memcpy((void*)dataAddress, signalFrameData,
		sizeof(*signalFrameData));
	if (error != B_OK)
		return error;

	// write a stack back chain pointing at the interrupted frame, so a stack
	// walk through the signal handler terminates cleanly
	uint32 backChain = frame->r1;
	error = user_memcpy(userStack, &backChain, sizeof(backChain));
	if (error != B_OK)
		return error;

	// look up the commpage signal handler trampoline
	addr_t commpageAddress = (addr_t)thread->team->commpage_address;
	addr_t handlerAddress
		= ((addr_t*)commpageAddress)[COMMPAGE_ENTRY_PPC_SIGNAL_HANDLER]
			+ commpageAddress;

	// redirect the thread into the trampoline: r3 = &signal_frame_data,
	// r1 = new stack, pc = trampoline
	frame->r1 = (addr_t)userStack;
	frame->r3 = dataAddress;
	frame->srr0 = handlerAddress;
	frame->lr = handlerAddress;

	return B_OK;
}


int64
arch_restore_signal_frame(struct signal_frame_data* signalFrameData)
{
	struct iframe* frame = ppc_get_user_iframe();
	if (frame == NULL) {
		panic("arch_restore_signal_frame(): No user iframe!");
		return 0;
	}

	mcontext_t& regs = signalFrameData->context.uc_mcontext;
	frame->srr0 = regs.pc;
	frame->r0 = regs.r0;
	frame->r1 = regs.r1;
	frame->r2 = regs.r2;
	frame->r3 = regs.r3;
	frame->r4 = regs.r4;
	frame->r5 = regs.r5;
	frame->r6 = regs.r6;
	frame->r7 = regs.r7;
	frame->r8 = regs.r8;
	frame->r9 = regs.r9;
	frame->r10 = regs.r10;
	frame->r11 = regs.r11;
	frame->r12 = regs.r12;
	frame->f0 = regs.f0;
	frame->f1 = regs.f1;
	frame->f2 = regs.f2;
	frame->f3 = regs.f3;
	frame->f4 = regs.f4;
	frame->f5 = regs.f5;
	frame->f6 = regs.f6;
	frame->f7 = regs.f7;
	frame->f8 = regs.f8;
	frame->f9 = regs.f9;
	frame->f10 = regs.f10;
	frame->f11 = regs.f11;
	frame->f12 = regs.f12;
	frame->f13 = regs.f13;
	frame->fpscr = regs.fpscr;
	frame->ctr = regs.ctr;
	frame->xer = regs.xer;
	frame->cr = regs.cr;
	frame->lr = regs.lr;
		// note: srr1 (MSR) is deliberately NOT restored from the
		// user-supplied context - the current iframe already holds a
		// valid user-mode MSR and importing an arbitrary one would let a
		// signal handler escalate privilege.
	return frame->r3;
}



/**	Saves everything needed to restore the frame in the child fork in the
 *	arch_fork_arg structure to be passed to arch_restore_fork_frame().
 *	Also makes sure to return the right value.
 */

void
arch_store_fork_frame(struct arch_fork_arg *arg)
{
	arg->iframe = *ppc_get_user_iframe();
	arg->iframe.r3 = 0;
}


/** Restores the frame from a forked team as specified by the provided
 *	arch_fork_arg structure.
 *	Needs to be called from within the child team, ie. instead of
 *	arch_thread_enter_uspace() as thread "starter".
 *	This function does not return to the caller, but will enter userland
 *	in the child team at the same position where the parent team left of.
 */

void
arch_restore_fork_frame(struct arch_fork_arg *arg)
{
	Thread* thread = thread_get_current_thread();

	ppc_sync_kernel_stack(thread);

	struct iframe frame = arg->iframe;
	disable_interrupts();
	ppc_enter_userspace(&frame);
}

