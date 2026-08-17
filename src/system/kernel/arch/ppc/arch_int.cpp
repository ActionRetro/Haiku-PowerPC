/*
 * Copyright 2003-2011, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Axel Dörfler <axeld@pinc-software.de>
 * 		Ingo Weinhold <bonefish@cs.tu-berlin.de>
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <interrupts.h>
#include <ksignal.h>
#include <ksyscalls.h>
#include <syscall_numbers.h>

#include <arch/smp.h>
#include <boot/kernel_args.h>
#include <device_manager.h>
#include <kscheduler.h>
#include <interrupt_controller.h>
#include <smp.h>
#include <thread.h>
#include <timer.h>
#include <user_debugger.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/kernel_cpp.h>
#include <vm/vm.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <PCI.h>

#include <string.h>


// defined in arch_exceptions.S
extern int __irqvec_start;
extern int __irqvec_end;

extern"C" void ppc_exception_tail(void);


// the exception contexts for all CPUs
static ppc_cpu_exception_context sCPUExceptionContexts[SMP_MAX_CPUS];


// An iframe stack used in the early boot process when we don't have
// threads yet.
struct iframe_stack gBootFrameStack;

// interrupt controller interface (initialized
// in arch_int_init_post_device_manager())
static struct interrupt_controller_module_info *sPIC;
static void *sPICCookie;


void
arch_int_enable_io_interrupt(int32 irq)
{
	if (!sPIC)
		return;

	// TODO: I have no idea, what IRQ type is appropriate.
	sPIC->enable_io_interrupt(sPICCookie, irq, IRQ_TYPE_LEVEL);
}


void
arch_int_disable_io_interrupt(int32 irq)
{
	if (!sPIC)
		return;

	sPIC->disable_io_interrupt(sPICCookie, irq);
}


/* arch_int_*_interrupts() and friends are in arch_asm.S */


static void
print_iframe(struct iframe *frame)
{
	dprintf("iframe at %p:\n", frame);
	dprintf("r0-r3:   0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r0, frame->r1, frame->r2, frame->r3);
	dprintf("r4-r7:   0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r4, frame->r5, frame->r6, frame->r7);
	dprintf("r8-r11:  0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r8, frame->r9, frame->r10, frame->r11);
	dprintf("r12-r15: 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r12, frame->r13, frame->r14, frame->r15);
	dprintf("r16-r19: 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r16, frame->r17, frame->r18, frame->r19);
	dprintf("r20-r23: 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r20, frame->r21, frame->r22, frame->r23);
	dprintf("r24-r27: 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r24, frame->r25, frame->r26, frame->r27);
	dprintf("r28-r31: 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", frame->r28, frame->r29, frame->r30, frame->r31);
	dprintf("     ctr 0x%08lx        xer 0x%08lx\n", frame->ctr, frame->xer);
	dprintf("      cr 0x%08lx         lr 0x%08lx\n", frame->cr, frame->lr);
	dprintf("   dsisr 0x%08lx        dar 0x%08lx\n", frame->dsisr, frame->dar);
	dprintf("    srr1 0x%08lx       srr0 0x%08lx\n", frame->srr1, frame->srr0);
}


// Set to 1 to trace every exception (very noisy - floods the serial
// console and the framebuffer, and slows the boot dramatically).
#define TRACE_PPC_EXCEPTIONS 0

// TEMP: resolve a user/kernel code or data address to its loaded image area
// (name + offset) so a fault pc/lr/dar can be symbolized despite ASLR.
static void
ppc_log_img(const char* label, addr_t addr)
{
	if (addr == 0) {
		dprintf("  %s=%08lx (null)\n", label, (unsigned long)addr);
		return;
	}
	VMAddressSpace* space = IS_KERNEL_ADDRESS(addr)
		? VMAddressSpace::GetKernel() : VMAddressSpace::GetCurrent();
	if (space == NULL) {
		dprintf("  %s=%08lx (no aspace)\n", label, (unsigned long)addr);
		return;
	}
	space->ReadLock();
	VMArea* area = space->LookupArea(addr);
	if (area != NULL) {
		dprintf("  %s=%08lx -> %s +%#lx (base %08lx)\n", label,
			(unsigned long)addr, area->name,
			(unsigned long)(addr - area->Base()), (unsigned long)area->Base());
	} else
		dprintf("  %s=%08lx -> (no area)\n", label, (unsigned long)addr);
	space->ReadUnlock();
	space->Put();
}


// Keep the per-CPU exception context's kernel_stack pointing just below the
// deepest live iframe. A nested USERLAND exception (e.g. an interrupt taken
// while a signal trampoline runs) then stacks BELOW the suspended outer frame
// instead of aliasing the fixed kernel_stack_top and clobbering it (the ppc
// nested-signal freeze/corruption). A nested userland exception implies the
// outer kernel activity already returned to user (rfi), so the space below its
// iframe is free to reuse.
extern "C" void ppc_sync_kernel_stack(Thread* thread);
void
ppc_sync_kernel_stack(Thread* thread)
{
	struct iframe_stack* st = &thread->arch_info.iframes;
	addr_t ks;
	if (st->index > 0)
		ks = (addr_t)st->frames[st->index - 1] - 64;
	else
		ks = (addr_t)thread->kernel_stack_top - 8;
	ppc_get_cpu_exception_context(smp_get_current_cpu())->kernel_stack = (void*)ks;
}


extern "C" void ppc_exception_entry(int vector, struct iframe *iframe);
void
ppc_exception_entry(int vector, struct iframe *iframe)
{
#if TRACE_PPC_EXCEPTIONS
	if (vector != 0x900 && vector != 0xc00) {
		dprintf("ppc_exception_entry: time %lld vector 0x%x, iframe %p, "
			"srr0: %p\n", system_time(), vector, iframe, (void*)iframe->srr0);
	}
#endif

	Thread *thread = thread_get_current_thread();

	// push iframe
	if (thread)
		ppc_push_iframe(&thread->arch_info.iframes, iframe);
	else
		ppc_push_iframe(&gBootFrameStack, iframe);
	if (thread)
		ppc_sync_kernel_stack(thread);

	// An exception taken from user mode is a kernel entry, just like a
	// syscall. Pair it with thread_at_kernel_entry/exit so pending signals are
	// delivered on the way back out; in particular the SIGSEGV that
	// vm_page_fault raises for an unrecoverable user fault must be delivered
	// here, otherwise the thread simply resumes the faulting instruction and
	// refaults forever (pinning the CPU in a "Bad port ID" debug-event loop).
	bool fromUserland = thread != NULL && (iframe->srr1 & (1 << 14)) != 0;
	if (fromUserland)
		thread_at_kernel_entry(system_time());

	switch (vector) {
		case 0x100: // system reset
			panic("system reset exception\n");
			break;
		case 0x200: // machine check
			panic("machine check exception\n");
			break;
		case 0x300: // DSI
		case 0x400: // ISI
		{
			bool kernelDebugger = debug_debugger_running();

			if (kernelDebugger) {
				// if this CPU or this thread has a fault handler,
				// we're allowed to be here
				cpu_ent* cpu = &gCPU[smp_get_current_cpu()];
				if (cpu->fault_handler != 0) {
					iframe->srr0 = cpu->fault_handler;
					iframe->r1 = cpu->fault_handler_stack_pointer;
					break;
				}
				Thread *thread = thread_get_current_thread();
				if (thread && thread->fault_handler != 0) {
					iframe->srr0 =
						reinterpret_cast<uintptr_t>(thread->fault_handler);
					break;
				}

				// otherwise, not really
				panic("page fault in debugger without fault handler! Touching "
					"address %p from ip %p\n", (void *)iframe->dar,
					(void *)iframe->srr0);
				break;
			} else if ((iframe->srr1 & MSR_EXCEPTIONS_ENABLED) == 0) {
				// if the interrupts were disabled, and we are not running the
				// kernel startup the page fault was not allowed to happen and
				// we must panic
				panic("page fault, but interrupts were disabled. Touching "
					"address %p from ip %p\n", (void *)iframe->dar,
					(void *)iframe->srr0);
				break;
			} else if (thread != NULL && thread->page_faults_allowed < 1) {
				panic("page fault not allowed at this place. Touching address "
					"%p from ip %p\n", (void *)iframe->dar,
					(void *)iframe->srr0);
			}

			enable_interrupts();

			addr_t newip;

			// On an instruction storage interrupt (0x400/ISI) the faulting
			// address is the instruction pointer in SRR0 - DAR is only set for
			// data storage interrupts (0x300/DSI). Likewise the store/load
			// status lives in DSISR only for DSI; an instruction fetch is
			// always a read.
			bool isInstructionFault = (vector == 0x400);
			addr_t faultAddress
				= isInstructionFault ? iframe->srr0 : iframe->dar;
			bool isWrite
				= !isInstructionFault && (iframe->dsisr & (1 << 25));

			// TEMP: log only faults that will NOT resolve -- i.e. the faulting
			// address is covered by no area in the relevant address space. That
			// skips the flood of ordinary demand-paging faults (the kernel
			// touching a valid, not-yet-mapped user page during team setup) and
			// keeps only genuine bad accesses: a user-mode null/wild pointer, or
			// a kernel dereference of a stripped pointer (the bit-0x80000000
			// crash) that vm_page_fault() is about to SIGSEGV or panic on.
			// Mirrors vm_page_fault()'s own address-space lookup.
			bool willFault;
			{
				VMAddressSpace* space = IS_KERNEL_ADDRESS(faultAddress)
					? VMAddressSpace::GetKernel() : VMAddressSpace::GetCurrent();
				if (space != NULL) {
					space->ReadLock();
					willFault = space->LookupArea(faultAddress) == NULL;
					space->ReadUnlock();
					space->Put();
				} else
					willFault = true;
			}
			if (willFault) {
				Thread* t = thread_get_current_thread();

				// PISMO bit-31 hunt: a supervisor-mode fault on a bit-31-clear
				// (physical-looking) address that no area backs is the stripped-
				// kernel-pointer crash. Panic FIRST with ONE compact line (no
				// preceding dprintf) so the culprit stays readable at the top of
				// the non-scrollable Pismo KDL screen.
				{
					cpu_ent* b31cpu = &gCPU[smp_get_current_cpu()];
					bool b31recoverable = b31cpu->fault_handler != 0
						|| (thread != NULL && thread->fault_handler != 0);
					if (!b31recoverable && (iframe->srr1 & (1 << 14)) == 0
							&& faultAddress >= 0x1000 && faultAddress < 0x80000000) {
						uint32 gpr[32] = {
							iframe->r0, iframe->r1, iframe->r2, iframe->r3,
							iframe->r4, iframe->r5, iframe->r6, iframe->r7,
							iframe->r8, iframe->r9, iframe->r10, iframe->r11,
							iframe->r12, iframe->r13, iframe->r14, iframe->r15,
							iframe->r16, iframe->r17, iframe->r18, iframe->r19,
							iframe->r20, iframe->r21, iframe->r22, iframe->r23,
							iframe->r24, iframe->r25, iframe->r26, iframe->r27,
							iframe->r28, iframe->r29, iframe->r30, iframe->r31 };
						int exact = -1, base = -1;
						for (int i = 0; i < 32; i++) {
							if (gpr[i] == (uint32)faultAddress)
								exact = i;
							if ((gpr[i] & 0x80000000) == 0 && gpr[i] >= 0x1000
									&& (uint32)faultAddress >= gpr[i]
									&& (uint32)faultAddress - gpr[i] < 0x10000)
								base = i;
						}
						panic("BIT31KERNEL pc=%08lx lr=%08lx dar=%08lx exact=r%d "
							"base=r%d baseval=%08lx", (unsigned long)iframe->srr0,
							(unsigned long)iframe->lr, (unsigned long)faultAddress,
							exact, base, base >= 0 ? (unsigned long)gpr[base] : 0UL);
					}
				}
				uint32 insn = 0;
				// Only read the faulting instruction from a plausibly-mapped
				// user address. A jump through a null/low function pointer
				// leaves srr0 in the first page (e.g. 0 when ctr==0), and
				// reading there would itself fault - turning a recoverable
				// user crash into a kernel panic. Skip the low page (and the
				// kernel half); insn stays 0 for those, and vm_page_fault()
				// below delivers the SIGSEGV cleanly.
				if (iframe->srr0 >= B_PAGE_SIZE && iframe->srr0 < 0x80000000)
					insn = *(volatile uint32*)(addr_t)iframe->srr0;
				dprintf("uf %s pc=%08lx a=%08lx insn=%08lx t=%ld(%s)\n",
					isInstructionFault ? "x" : (isWrite ? "w" : "r"),
					(unsigned long)iframe->srr0, (unsigned long)faultAddress,
					(unsigned long)insn,
					(long)(t ? t->id : -1), t ? t->name : "?");
				dprintf("  r0-7  %08x %08x %08x %08x %08x %08x %08x %08x\n",
					iframe->r0, iframe->r1, iframe->r2, iframe->r3, iframe->r4,
					iframe->r5, iframe->r6, iframe->r7);
				dprintf("  r8-15 %08x %08x %08x %08x %08x %08x %08x %08x\n",
					iframe->r8, iframe->r9, iframe->r10, iframe->r11, iframe->r12,
					iframe->r13, iframe->r14, iframe->r15);
				dprintf("  r16-23 %08x %08x %08x %08x %08x %08x %08x %08x\n",
					iframe->r16, iframe->r17, iframe->r18, iframe->r19, iframe->r20,
					iframe->r21, iframe->r22, iframe->r23);
				dprintf("  r24-31 %08x %08x %08x %08x %08x %08x %08x %08x lr=%08x\n",
					iframe->r24, iframe->r25, iframe->r26, iframe->r27, iframe->r28,
					iframe->r29, iframe->r30, iframe->r31, iframe->lr);
				ppc_log_img("pc ", iframe->srr0);
				ppc_log_img("lr ", iframe->lr);
				ppc_log_img("dar", faultAddress);
			}

			vm_page_fault(faultAddress, iframe->srr0,
				isWrite,
				isInstructionFault, // execute access?
				iframe->srr1 & (1 << 14), // was the system in user or supervisor
				&newip);
			if (newip != 0) {
				// the page fault handler wants us to modify the iframe to set the
				// IP the cpu will return to to be this ip
				iframe->srr0 = newip;
			}
 			break;
		}

		case 0x500: // external interrupt
		{
			if (!sPIC) {
				panic("ppc_exception_entry(): external interrupt although we "
					"don't have a PIC driver!");
				break;
			}

			int irq;
			while ((irq = sPIC->acknowledge_io_interrupt(sPICCookie)) >= 0) {
// TODO: correctly pass level-triggered vs. edge-triggered to the handler!
				io_interrupt_handler(irq, true);
			}
			break;
		}

		case 0x600: // alignment exception
			panic("alignment exception: unimplemented\n");
			break;
		case 0x700: // program exception (illegal/privileged insn, trap)
		{
			if ((iframe->srr1 & (1 << 14)) == 0) {
				// In supervisor mode this is a genuine kernel bug.
				print_iframe(iframe);
				panic("program exception in kernel mode: srr0 %p\n",
					(void*)iframe->srr0);
				break;
			}

			// A user thread executed an illegal/privileged instruction or a
			// trap. Raise SIGILL on it (delivered on the return-to-userland
			// path, which terminates the thread if it has no handler) rather
			// than panicking the whole kernel.
			dprintf("program exception (0x700) in user thread %" B_PRId32
				" \"%s\" at srr0 %p\n", thread->id, thread->name,
				(void*)iframe->srr0);
			enable_interrupts();
			struct sigaction action;
			if ((sigaction(SIGILL, NULL, &action) == 0
					&& action.sa_handler != SIG_DFL
					&& action.sa_handler != SIG_IGN)
				|| user_debug_exception_occurred(B_INVALID_OPCODE_EXCEPTION,
					SIGILL)) {
				Signal signal(SIGILL, ILL_ILLOPC, 0, thread->team->id);
				signal.SetAddress((void*)iframe->srr0);
				send_signal_to_thread(thread, signal, 0);
			}
			break;
		}
		case 0x800: // FP unavailable exception
			panic("FP unavailable exception: unimplemented\n");
			break;
		case 0x900: // decrementer exception
			timer_interrupt();
			break;
		case 0xc00: // system call
		{
			uint32 syscall = iframe->r0;
			uint64 returnValue = 0;

			if (syscall < (uint32)kSyscallCount) {
				// The SysV PPC ABI passes the first eight argument words in
				// r3-r10 and spills the remainder into the caller's parameter
				// save area at sp+8. 64-bit arguments occupy the next
				// consecutive register pair (no even-register alignment on
				// this ABI), so gathering the registers in order reproduces
				// exactly the packed argument buffer the dispatcher expects.
				uint32 args[20];
				uint32 regArgs[8] = {
					iframe->r3, iframe->r4, iframe->r5, iframe->r6,
					iframe->r7, iframe->r8, iframe->r9, iframe->r10
				};
				int argSize = kSyscallInfos[syscall].parameter_size;
				if (argSize > (int)sizeof(args))
					argSize = (int)sizeof(args);
				int regBytes = argSize < (int)sizeof(regArgs)
					? argSize : (int)sizeof(regArgs);
				memcpy(args, regArgs, regBytes);
				if (argSize > (int)sizeof(regArgs)) {
					if (user_memcpy((uint8*)args + sizeof(regArgs),
							(void*)(iframe->r1 + 8),
							argSize - sizeof(regArgs)) != B_OK) {
						iframe->r3 = (uint32)B_BAD_ADDRESS;
						break;
					}
				}

				enable_interrupts();

				syscall_dispatcher(syscall, (void*)args, &returnValue);

				disable_interrupts();

				// Store the return value per the 32-bit PPC SysV ABI: a
				// 32-bit result (status_t/ssize_t) is returned in r3, while a
				// 64-bit result (off_t, bigtime_t) uses the r3:r4 pair with
				// r3 = high word and r4 = low word. Previously only r3 was
				// written (with the low word) and r4 was left stale, so every
				// off_t/bigtime_t-returning syscall handed userland a garbage
				// 64-bit value (e.g. BFile::Seek() to a non-zero offset, which
				// broke callers that check the returned position).
				// _kern_restore_signal_frame is special: it has already
				// rebuilt the whole user iframe (including r4) from the signal
				// context and returns the restored r3, so it must only refresh
				// r3 and leave r4 alone - splitting it would clobber the
				// restored r4 and corrupt every signal return.
				if (kExtendedSyscallInfos[syscall].return_type.size > 4
					&& syscall != SYSCALL_RESTORE_SIGNAL_FRAME) {
					iframe->r3 = (uint32)(returnValue >> 32);
					iframe->r4 = (uint32)returnValue;
				} else {
					iframe->r3 = (uint32)returnValue;
				}
			} else {
				iframe->r3 = (uint32)B_BAD_VALUE;
			}
			break;
		}
		case 0xd00: // trace exception
			panic("trace exception: unimplemented\n");
			break;
		case 0xe00: // FP assist exception
			panic("FP assist exception: unimplemented\n");
			break;
		case 0xf00: // performance monitor exception
			panic("performance monitor exception: unimplemented\n");
			break;
		case 0xf20: // altivec unavailable exception
			// Returning from here just re-executes the faulting instruction,
			// so this must not be a silent "log and continue" - that hangs.
			// With MSR_VEC set for both kernel and threads this should never
			// fire; if it does, say why loudly.
			panic("altivec unavailable exception (gPPCHasAltiVec=%d): the "
				"vector unit is not enabled in this context\n",
				gPPCHasAltiVec ? 1 : 0);
			break;
		case 0x1000:
		case 0x1100:
		case 0x1200:
			panic("TLB miss exception: unimplemented\n");
			break;
		case 0x1300: // instruction address exception
			panic("instruction address exception: unimplemented\n");
			break;
		case 0x1400: // system management exception
			panic("system management exception: unimplemented\n");
			break;
		case 0x1600: // altivec assist exception
			// Raised for denormal handling in Java mode. Switching the vector
			// unit to non-Java (NJ) mode would avoid it entirely; until then,
			// note it rather than killing the machine.
			dprintf("altivec assist exception (denormal); ignored\n");
			break;
		case 0x1700: // thermal management exception
			panic("thermal management exception: unimplemented\n");
			break;
		default:
			dprintf("unhandled exception type 0x%x\n", vector);
			print_iframe(iframe);
			panic("unhandled exception type\n");
	}

	// thread is NULL for exceptions taken before thread_init() has run
	// (early boot startup, e.g. commpage_init() enabling a first
	// decrementer tick) - handled explicitly below just like the
	// gBootFrameStack case above; there is no post-interrupt callback or
	// scheduler to invoke yet, only an iframe to pop.
	if (thread != NULL) {
		cpu_status state = disable_interrupts();
		if (thread->post_interrupt_callback != NULL) {
			void (*callback)(void*) = thread->post_interrupt_callback;
			void* data = thread->post_interrupt_data;

			thread->post_interrupt_callback = NULL;
			thread->post_interrupt_data = NULL;

			restore_interrupts(state);

			callback(data);
		} else if (thread->cpu->invoke_scheduler) {
			SpinLocker schedulerLocker(thread->scheduler_lock);
			scheduler_reschedule(B_THREAD_READY);
			schedulerLocker.Unlock();
			restore_interrupts(state);
		}
	}

	// Return-to-userland: deliver any pending signals. This is where an
	// unrecoverable fault's SIGSEGV finally terminates the thread.
	if (fromUserland) {
		disable_interrupts();
		if ((thread->flags & (THREAD_FLAGS_SIGNALS_PENDING
				| THREAD_FLAGS_DEBUG_THREAD
				| THREAD_FLAGS_TRAP_FOR_CORE_DUMP)) != 0) {
			enable_interrupts();
			thread_at_kernel_exit();
		} else {
			thread_at_kernel_exit_no_signals();
		}
	}

	// pop iframe
	if (thread)
		ppc_pop_iframe(&thread->arch_info.iframes);
	else
		ppc_pop_iframe(&gBootFrameStack);
	if (thread)
		ppc_sync_kernel_stack(thread);
}


status_t
arch_int_init(kernel_args *args)
{
	return B_OK;
}


status_t
arch_int_init_post_vm(kernel_args *args)
{
	void *handlers = (void *)args->arch_args.exception_handlers.start;

	// The actual size of our own exception vector code, not whatever the
	// boot loader happened to record in kernel_args - see below, that value
	// has no reliable relationship to this in either of its code paths.
	// PPC hardware vectors exceptions to fixed offsets within the first two
	// physical pages (see the design comment atop arch_exceptions.S); the
	// generated __irqvec_start..__irqvec_end code is a bit under 6 KB,
	// i.e. genuinely needs both of those pages, not the one page a
	// same-sized-as-B_PAGE_SIZE assumption would provide.
	size_t handlersSize = ROUNDUP((addr_t)&__irqvec_end
		- (addr_t)&__irqvec_start, B_PAGE_SIZE);

	area_id exceptionArea;
	if (handlers == (void*)-1) {
		// The boot loader never found an existing Open Firmware mapping to
		// preserve for this (its own long-standing, acknowledged TODO - see
		// find_allocated_ranges() in the loader's mmu.cpp, which leaves this
		// exact sentinel value in place when its "physical_address <= 0x100"
		// heuristic for locating OF's exception vectors doesn't match -
		// true on at least QEMU/OpenBIOS, where OF's own vectors are high
		// up in physical memory instead). (void*)-1 also happens to satisfy
		// IS_KERNEL_ADDRESS() on ppc32 (>= KERNEL_BASE), so the remap logic
		// below would otherwise skip straight to requesting a B_EXACT_ADDRESS
		// B_ALREADY_WIRED area at literal address 0xffffffff - not an
		// address any real translation is ever going to occupy, and not
		// what B_ALREADY_WIRED (which asserts the mapping already exists)
		// is for in the first place. There is nothing to preserve here, so
		// allocate somewhere fresh instead - but *not* just anywhere: the
		// PPC CPU's hardware exception mechanism jumps to fixed *physical*
		// addresses at the bottom of memory regardless of where our code
		// for handling them virtually lives, so the physical location is
		// not a free choice here the way it is for ordinary kernel
		// allocations. B_CONTIGUOUS with a [0, 2 pages) physical
		// restriction forces exactly that placement; B_ANY_KERNEL_ADDRESS
		// alone (originally tried here) picked arbitrary physical memory
		// that real hardware exceptions could never actually reach,
		// producing a silent, unresponsive hang on the first one taken
		// after boot (no panic - the CPU vectors into whatever was already
		// sitting at physical address 0, not into our copied handler code)
		// rather than the crash a wrong *virtual* address would give.
		handlers = NULL;
		physical_address_restrictions physicalRestrictions = {};
		physicalRestrictions.low_address = 0;
		physicalRestrictions.high_address = handlersSize;
		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
		exceptionArea = vm_create_anonymous_area(VMAddressSpace::KernelID(),
			"exception_handlers", handlersSize, B_CONTIGUOUS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
			&virtualRestrictions, &physicalRestrictions, true, &handlers);
	} else {
		// We may need to remap the exception handler area into the kernel
		// address space.
		if (!IS_KERNEL_ADDRESS(handlers)) {
			addr_t address = (addr_t)handlers;
			status_t error = ppc_remap_address_range(&address, handlersSize,
				true);
			if (error != B_OK) {
				panic("arch_int_init_post_vm(): Failed to remap the "
					"exception handler area!");
				return error;
			}
			handlers = (void*)(address);
		}

		// create a region to map the irq vector code into (physical
		// address 0x0)
		exceptionArea = create_area("exception_handlers", &handlers,
			B_EXACT_ADDRESS, handlersSize, B_ALREADY_WIRED,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	}
	if (exceptionArea < B_OK)
		panic("arch_int_init2: could not create exception handler region\n");

	dprintf("exception handlers at %p\n", handlers);

	// copy the handlers into this area
	memcpy(handlers, &__irqvec_start, handlersSize);
	arch_cpu_sync_icache(handlers, handlersSize);

	// init the CPU exception contexts
	int cpuCount = smp_get_num_cpus();
	for (int i = 0; i < cpuCount; i++) {
		ppc_cpu_exception_context *context = ppc_get_cpu_exception_context(i);
		context->kernel_handle_exception = (void*)&ppc_exception_tail;
		context->exception_context = context;
		// kernel_stack is set when the current thread changes. At this point
		// we don't have threads yet.
	}

	// set the exception context for this CPU
	ppc_set_current_cpu_exception_context(ppc_get_cpu_exception_context(0));

	return B_OK;
}


status_t
arch_int_init_io(kernel_args* args)
{
	return B_OK;
}


template<typename ModuleInfo>
struct Module : DoublyLinkedListLinkImpl<Module<ModuleInfo> > {
	Module(ModuleInfo *module)
		: module(module)
	{
	}

	~Module()
	{
		if (module)
			put_module(((module_info*)module)->name);
	}

	ModuleInfo	*module;
};

typedef Module<interrupt_controller_module_info> PICModule;

struct PICModuleList : DoublyLinkedList<PICModule> {
	~PICModuleList()
	{
		while (PICModule *module = First()) {
			Remove(module);
			delete module;
		}
	}
};


// get_next_child_node() filters children through device_node::CompareTo(),
// which treats a NULL attribute list as "match nothing" (returns -1). Pass an
// empty (NULL-terminated) attribute list instead so every child matches.
static const device_attr kMatchAnyChild[] = { {} };

class DeviceTreeIterator {
public:
	DeviceTreeIterator(device_manager_info *deviceManager)
		: fDeviceManager(deviceManager),
		  fNode(NULL),
		  fParent(NULL)
	{
		Rewind();
	}

	~DeviceTreeIterator()
	{
		if (fParent != NULL)
			fDeviceManager->put_node(fParent);
		if (fNode != NULL)
			fDeviceManager->put_node(fNode);
	}

	void Rewind()
	{
		fNode = fDeviceManager->get_root_node();
	}

	bool HasNext() const
	{
		return (fNode != NULL);
	}

	device_node *Next()
	{
		if (fNode == NULL)
			return NULL;

		device_node *foundNode = fNode;

		// get first child
		device_node *child = NULL;
		if (fDeviceManager->get_next_child_node(fNode, kMatchAnyChild, &child)
				== B_OK) {
			// move to the child node
			if (fParent != NULL)
				fDeviceManager->put_node(fParent);
			fParent = fNode;
			fNode = child;

		// no more children; backtrack to find the next sibling
		} else {
			while (fParent != NULL) {
				if (fDeviceManager->get_next_child_node(fParent, kMatchAnyChild, &fNode)
						== B_OK) {
						// get_next_child_node() always puts the node
					break;
				}
				fNode = fParent;
				fParent = fDeviceManager->get_parent_node(fNode);
			}

			// if we hit the root node again, we're done
			if (fParent == NULL) {
				fDeviceManager->put_node(fNode);
				fNode = NULL;
			}
		}

		return foundNode;
	}

private:
	device_manager_info *fDeviceManager;
	device_node	*fNode;
	device_node	*fParent;
};


static void
get_interrupt_controller_modules(PICModuleList &list)
{
	const char *namePrefix = "interrupt_controllers/";
	size_t namePrefixLen = strlen(namePrefix);

	char name[B_PATH_NAME_LENGTH];
	size_t length;
	uint32 cookie = 0;
	while (get_next_loaded_module_name(&cookie, name, &(length = sizeof(name)))
			== B_OK) {
		// an interrupt controller module?
		if (length <= namePrefixLen
			|| strncmp(name, namePrefix, namePrefixLen) != 0) {
			continue;
		}

		// get the module
		interrupt_controller_module_info *moduleInfo;
		if (get_module(name, (module_info**)&moduleInfo) != B_OK)
			continue;

		// add it to the list
		PICModule *module = new(nothrow) PICModule(moduleInfo);
		if (!module) {
			put_module(((module_info*)moduleInfo)->name);
			continue;
		}
		list.Add(module);
	}
}


static bool
probe_pic_device(device_node *node, PICModuleList &picModules)
{
	for (PICModule *module = picModules.Head();
		 module;
		 module = picModules.GetNext(module)) {
		if (module->module->info.supports_device(node) > 0) {
			if (module->module->info.register_device(node) == B_OK)
				return true;
		}
	}

	return false;
}


status_t
arch_int_init_post_device_manager(struct kernel_args *args)
{
	// PPC is the only arch that never reserved its I/O interrupt vectors, so
	// every sVectors[] entry kept assigned_cpu == NULL. install_io_interrupt_
	// handler() tolerates that (it only dereferences assigned_cpu for
	// INTERRUPT_TYPE_IRQ vectors), but the per-interrupt load accounting in
	// update_int_load() unconditionally does atomic_add(&assigned_cpu->load,
	// ...) - a NULL deref (fault at 0x14, load's offset within irq_assignment)
	// the moment a vector accrues measurable load. Rare/fast IRQs (ATA) never
	// tripped it; a constantly-polling source (VIA-CUDA ADB autopoll) does
	// immediately. Reserve the whole vector space as IRQ up front, exactly like
	// the arm and riscv64 ports do in their controller init, so assigned_cpu is
	// always valid. arch_int_assign_to_cpu() is a no-op on ppc, so the
	// IRQ-balancing path this enables in install_io_interrupt_handler() simply
	// pins every vector to CPU 0.
	reserve_io_interrupt_vectors(NUM_IO_VECTORS, 0, INTERRUPT_TYPE_IRQ);

	// get the interrupt controller driver modules
	PICModuleList picModules;
	get_interrupt_controller_modules(picModules);
	if (picModules.IsEmpty()) {
		panic("arch_int_init_post_device_manager(): Found no PIC modules!");
		return B_ENTRY_NOT_FOUND;
	}

	// get the device manager module
	device_manager_info *deviceManager;
	status_t error = get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&deviceManager);
	if (error != B_OK) {
		panic("arch_int_init_post_device_manager(): Failed to get device "
			"manager: %s", strerror(error));
		return error;
	}
	Module<device_manager_info> _deviceManager(deviceManager);	// auto put

	// iterate through the device tree and probe the interrupt controllers
	DeviceTreeIterator iterator(deviceManager);
	while (device_node *node = iterator.Next())
		probe_pic_device(node, picModules);

	// iterate through the tree again and get an interrupt controller node
	iterator.Rewind();
	while (device_node *node = iterator.Next()) {
		uint16 pciBaseClass, pciSubType;
		if (deviceManager->get_attr_uint16(node, B_DEVICE_TYPE,
				&pciBaseClass, false) == B_OK &&
			deviceManager->get_attr_uint16(node, B_DEVICE_SUB_TYPE,
				&pciSubType, false) == B_OK) {
			bool isPIC = (pciBaseClass == PCI_base_peripheral)
				&& (pciSubType == PCI_pic);

			if (isPIC) {
				driver_module_info *driver;
				void *driverCookie;

				deviceManager->get_driver(node, (driver_module_info **)&driver, (void **)&driverCookie);

				sPIC = (interrupt_controller_module_info *)driver;
				sPICCookie = driverCookie;
				return B_OK;
			}
		}
	}

	// no PIC found
	panic("arch_int_init_post_device_manager(): Found no supported PIC!");

	return B_ENTRY_NOT_FOUND;
}


// #pragma mark -

struct ppc_cpu_exception_context *
ppc_get_cpu_exception_context(int cpu)
{
	return sCPUExceptionContexts + cpu;
}


void
ppc_set_current_cpu_exception_context(struct ppc_cpu_exception_context *context)
{
	// translate to physical address
	phys_addr_t physicalPage;
	addr_t inPageOffset = (addr_t)context & (B_PAGE_SIZE - 1);
	status_t error = vm_get_page_mapping(VMAddressSpace::KernelID(),
		(addr_t)context - inPageOffset, &physicalPage);
	if (error != B_OK) {
		panic("ppc_set_current_cpu_exception_context(): Failed to get physical "
			"address!");
		return;
	}

	// physicalPage is a 64-bit phys_addr_t even on ppc32. Passing the
	// 64-bit sum directly into the asm with a single "r" constraint makes
	// GCC allocate a register *pair* for it - and expand %0 to the pair's
	// first register, which holds the *high* word: always zero for any
	// physical address below 4 GB. SPRG0 - the pointer the exception
	// vector entry code loads its per-CPU context through - was therefore
	// being set to literal 0 on every boot, making the first exception
	// taken corrupt the vector code through a NULL context pointer (the
	// "self-corrupting exception vector / frozen PC=0x10 loop" chased
	// across this entire porting effort). Truncate to 32 bits explicitly
	// before it reaches the asm operand.
	uint32 contextPhysical = (uint32)physicalPage + inPageOffset;
	asm volatile("mtsprg0 %0" : : "r"(contextPhysical));
}


int32
arch_int_assign_to_cpu(int32 irq, int32 cpu)
{
	// Not yet supported.
	return 0;
}
