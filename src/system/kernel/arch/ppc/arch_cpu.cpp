/*
 * Copyright 2003-2005, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <KernelExport.h>

#include <arch_platform.h>
#include <arch/cpu.h>
#include <arch/thread.h>
#include <boot/kernel_args.h>

static bool sHasTlbia;

void ppc_detect_altivec(void);

status_t
arch_cpu_preboot_init_percpu(kernel_args *args, int curr_cpu)
{
	// enable FPU
	set_msr(get_msr() | MSR_FP_AVAILABLE);

	// Enable AltiVec per-CPU, for the same reason as the FPU: this runs on
	// every processor, and ppc_context_switch() executes stvx/lvx in kernel
	// context on whichever CPU it happens to run on. Doing this only once on
	// the boot CPU would leave a second processor taking an "AltiVec
	// unavailable" exception on its first switch. Each CPU reads its own PVR.
	ppc_detect_altivec();
	if (gPPCHasAltiVec)
		set_msr(get_msr() | MSR_VEC_AVAILABLE);

	// The current thread must be NULL for all CPUs till we have threads.
	// Some boot code relies on this.
	arch_thread_set_current_thread(NULL);

	return B_OK;
}


bool gPPCHasAltiVec = false;


/*!	Decides whether this CPU has an AltiVec unit, from the processor version in
	the upper 16 bits of the PVR. Only the 74xx (G4) and 970 (G5) families do;
	601/603/604/750 (G3) do not, and a vector instruction there traps.
*/
void
ppc_detect_altivec(void)
{
	uint16 version = (uint16)(get_pvr() >> 16);

	switch (version) {
		case MPC7400:		/* 7400/7410 - the original G4 */
		case MPC7410:
		case MPC7447A:
		case MPC7448:
		case MPC7450:
		case MPC7455:
		case MPC7457:
			gPPCHasAltiVec = true;
			break;
		default:
			gPPCHasAltiVec = false;
			break;
	}

}


/*!	Logs what ppc_detect_altivec() found. Kept OUT of the detector itself: that
	runs from arch_cpu_preboot_init_percpu(), which is far too early for
	dprintf() - doing it there hangs the boot before any output appears.
*/
static void
ppc_report_altivec(void)
{
	dprintf("ppc: PVR 0x%08" B_PRIx32 " (version 0x%04x): AltiVec %s\n",
		get_pvr(), (uint16)(get_pvr() >> 16),
		gPPCHasAltiVec ? "PRESENT" : "absent");
}


status_t
arch_cpu_init(kernel_args *args)
{
	ppc_report_altivec();
	// TODO: Let the boot loader put that info into the kernel args
	// (property "tlbia" in the CPU node).
	sHasTlbia = false;

	return B_OK;
}


status_t
arch_cpu_init_post_vm(kernel_args *args)
{
	return B_OK;
}

status_t
arch_cpu_init_percpu(kernel_args *args, int curr_cpu)
{
        //detect_cpu(curr_cpu);

        // we only support one on ppc anyway at the moment...
	//XXX: WRITEME
        return 0;
}

status_t
arch_cpu_init_post_modules(kernel_args *args)
{
	return B_OK;
}

#define CACHELINE 32

void
arch_cpu_sync_icache(void *address, size_t len)
{
	// NOTE: len is unsigned (size_t), so it must never be used directly as
	// a decrementing loop counter - the final subtraction can undershoot
	// zero and wrap around to a huge positive value instead of stopping,
	// turning this into a runaway loop that walks off the end of the
	// intended range into unmapped memory. Use signed locals for both
	// countdowns instead (mirrors the equivalent fix in arch_elf.cpp's
	// sync_icache_for_relocation()).
	int off = (unsigned int)address & (CACHELINE - 1);
	int remaining = (int)(len + off);

	char *p = (char *)address - off;
	do {
		asm volatile ("dcbst 0,%0" :: "r"(p));
		p += CACHELINE;
	} while ((remaining -= CACHELINE) > 0);
	asm volatile ("sync");

	remaining = (int)(len + off);
	p = (char *)address - off;
	do {
		asm volatile ("icbi 0,%0" :: "r"(p));
		p += CACHELINE;
	} while ((remaining -= CACHELINE) > 0);
	asm volatile ("sync");
	isync();
}


void
arch_cpu_memory_read_barrier(void)
{
// WARNING PPC: is it model-dependant ?
	asm volatile ("lwsync");
}


void
arch_cpu_memory_write_barrier(void)
{
// WARNING PPC: is it model-dependant ?
	asm volatile ("isync");
	asm volatile ("eieio");
}


void
arch_cpu_invalidate_tlb_range(intptr_t, addr_t start, addr_t end)
{
	asm volatile("sync");
	while (start < end) {
		asm volatile("tlbie %0" :: "r" (start));
		asm volatile("eieio");
		asm volatile("sync");
		start += B_PAGE_SIZE;
	}
	asm volatile("tlbsync");
	asm volatile("sync");
}


void
arch_cpu_invalidate_tlb_list(intptr_t, addr_t pages[], int num_pages)
{
	int i;

	asm volatile("sync");
	for (i = 0; i < num_pages; i++) {
		asm volatile("tlbie %0" :: "r" (pages[i]));
		asm volatile("eieio");
		asm volatile("sync");
	}
	asm volatile("tlbsync");
	asm volatile("sync");
}


void
arch_cpu_global_tlb_invalidate()
{
	if (sHasTlbia) {
		ppc_sync();
		tlbia();
		ppc_sync();
	} else {
		addr_t address = 0;
		unsigned long i;

		ppc_sync();
		for (i = 0; i < 0x100000; i++) {
			tlbie(address);
			eieio();
			ppc_sync();

			address += B_PAGE_SIZE;
		}
		tlbsync();
		ppc_sync();
	}
}


void
arch_cpu_user_tlb_invalidate(intptr_t)
{
	arch_cpu_global_tlb_invalidate();
}


// TODO: all functions that use fault handlers need to be implemented
// in assembly due to problems passing in label addresses in gcc4.

status_t
arch_cpu_user_memcpy(void *to, const void *from, size_t size,
	addr_t *faultHandler)
{
	char *tmp = (char *)to;
	char *s = (char *)from;
	addr_t oldFaultHandler = *faultHandler;

// TODO: This doesn't work correctly with gcc 4 anymore!
	if (ppc_set_fault_handler(faultHandler, (addr_t)&&error))
		goto error;

	while (size--)
		*tmp++ = *s++;

	*faultHandler = oldFaultHandler;
	return 0;

error:
	*faultHandler = oldFaultHandler;
	return B_BAD_ADDRESS;
}


/**	\brief Copies at most (\a size - 1) characters from the string in \a from to
 *	the string in \a to, NULL-terminating the result.
 *
 *	\param to Pointer to the destination C-string.
 *	\param from Pointer to the source C-string.
 *	\param size Size in bytes of the string buffer pointed to by \a to.
 *
 *	\return strlen(\a from).
 */

ssize_t
arch_cpu_user_strlcpy(char *to, const char *from, size_t size, addr_t *faultHandler)
{
	int from_length = 0;
	addr_t oldFaultHandler = *faultHandler;

// TODO: This doesn't work correctly with gcc 4 anymore!
	if (ppc_set_fault_handler(faultHandler, (addr_t)&&error))
		goto error;

	if (size > 0) {
		to[--size] = '\0';
		// copy
		for ( ; size; size--, from_length++, to++, from++) {
			if ((*to = *from) == '\0')
				break;
		}
	}
	// count any leftover from chars
	while (*from++ != '\0')
		from_length++;

	*faultHandler = oldFaultHandler;
	return from_length;

error:
	*faultHandler = oldFaultHandler;
	return B_BAD_ADDRESS;
}


status_t
arch_cpu_user_memset(void *s, char c, size_t count, addr_t *faultHandler)
{
	char *xs = (char *)s;
	addr_t oldFaultHandler = *faultHandler;

// TODO: This doesn't work correctly with gcc 4 anymore!
	if (ppc_set_fault_handler(faultHandler, (addr_t)&&error))
		goto error;

	while (count--)
		*xs++ = c;

	*faultHandler = oldFaultHandler;
	return 0;

error:
	*faultHandler = oldFaultHandler;
	return B_BAD_ADDRESS;
}


status_t
arch_cpu_shutdown(bool reboot)
{
	PPCPlatform::Default()->ShutDown(reboot);
	return B_ERROR;
}


// The purpose of this function is to trick the compiler. When setting the
// page_handler to a label that is obviously (to the compiler) never used,
// it may reorganize the control flow, so that the labeled part is optimized
// away.
// By invoking the function like this
//
//	if (ppc_set_fault_handler(faultHandler, (addr_t)&&error))
//		goto error;
//
// the compiler has to keep the labeled code, since it can't guess the return
// value of this (non-inlinable) function. At least in my tests it worked that
// way, and I hope it will continue to work like this in the future.
//
bool
ppc_set_fault_handler(addr_t *handlerLocation, addr_t handler)
{
// TODO: This doesn't work correctly with gcc 4 anymore!
	*handlerLocation = handler;
	return false;
}
