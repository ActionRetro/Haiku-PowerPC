/*
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FBSD_MACHINE_GENERIC_BUS_H_
#define _FBSD_MACHINE_GENERIC_BUS_H_


#include <machine/_bus.h>


#define	BUS_SPACE_ALIGNED_POINTER(p, t) ALIGNED_POINTER(p, t)

#define	BUS_SPACE_MAXADDR_24BIT	0xFFFFFFUL
#define	BUS_SPACE_MAXADDR_32BIT 0xFFFFFFFFUL
#define	BUS_SPACE_MAXSIZE_24BIT	0xFFFFFFUL
#define	BUS_SPACE_MAXSIZE_32BIT	0xFFFFFFFFUL

#define	BUS_SPACE_MAXADDR 	0xFFFFFFFFFFFFFFFFUL
#define	BUS_SPACE_MAXSIZE 	0xFFFFFFFFFFFFFFFFUL

#define BUS_SPACE_INVALID_DATA	(~0)
#define BUS_SPACE_UNRESTRICTED	(~0)

// PCI device registers are little-endian. On a big-endian host the memory
// bus_space accessors must byte-swap (as FreeBSD does on sparc64/powerpc);
// on little-endian hosts these are no-ops. Stream accessors stay raw.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define _FBSD_BUS_SWAP16(x)	__builtin_bswap16(x)
#define _FBSD_BUS_SWAP32(x)	__builtin_bswap32(x)
#define _FBSD_BUS_SWAP64(x)	__builtin_bswap64(x)
#else
#define _FBSD_BUS_SWAP16(x)	(x)
#define _FBSD_BUS_SWAP32(x)	(x)
#define _FBSD_BUS_SWAP64(x)	(x)
#endif

// PowerPC needs an eieio between MMIO accesses: a plain volatile store/load
// only prevents *compiler* reordering, not the hardware write buffer, so a
// device register write immediately followed by a read (e.g. bwi_rf_read:
// write RF_CTRL, read RF_DATA) can race -- the read beats the write, giving a
// bus error / stale data. eieio enforces in-order execution of I/O accesses.
#if defined(__POWERPC__) || defined(__powerpc__) || defined(__ppc__)
#	define _FBSD_BUS_BARRIER()	__asm__ __volatile__("eieio" : : : "memory")
#else
#	define _FBSD_BUS_BARRIER()	__asm__ __volatile__("" : : : "memory")
#endif


static __inline u_int8_t
bus_space_read_1(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	_FBSD_BUS_BARRIER();
	return (*(volatile u_int8_t *)(handle + offset));
}


static __inline u_int16_t
bus_space_read_2(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	_FBSD_BUS_BARRIER();
	return _FBSD_BUS_SWAP16(*(volatile u_int16_t *)(handle + offset));
}


static __inline u_int32_t
bus_space_read_4(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	_FBSD_BUS_BARRIER();
	return _FBSD_BUS_SWAP32(*(volatile u_int32_t *)(handle + offset));
}


static __inline uint64_t
bus_space_read_8(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	_FBSD_BUS_BARRIER();
	return _FBSD_BUS_SWAP64(*(volatile uint64_t *)(handle + offset));
}


static __inline void
bus_space_write_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int8_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int8_t *)(bsh + offset) = value;
	_FBSD_BUS_BARRIER();
}


static __inline void
bus_space_write_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int16_t *)(bsh + offset) = _FBSD_BUS_SWAP16(value);
	_FBSD_BUS_BARRIER();
}


static __inline void
bus_space_write_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int32_t *)(bsh + offset) = _FBSD_BUS_SWAP32(value);
	_FBSD_BUS_BARRIER();
}


static __inline void
bus_space_write_8(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, uint64_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile uint64_t *)(bsh + offset) = _FBSD_BUS_SWAP64(value);
	_FBSD_BUS_BARRIER();
}


static __inline void
bus_space_read_region_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int8_t *addr, size_t count)
{
	for (; count > 0; offset += 1, addr++, count--)
		*addr = bus_space_read_1(tag, bsh, offset);
}


static __inline void
bus_space_read_region_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t *addr, size_t count)
{
	for (; count > 0; offset += 2, addr++, count--)
		*addr = bus_space_read_2(tag, bsh, offset);
}


static __inline void
bus_space_read_region_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t *addr, size_t count)
{
	for (; count > 0; offset += 4, addr++, count--)
		*addr = bus_space_read_4(tag, bsh, offset);
}


static __inline void
bus_space_write_multi_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int8_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_1(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_multi_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int16_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_2(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_multi_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int32_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_4(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int8_t *addr, size_t count)
{
	for (; count > 0; offset += 1, addr++, count--)
		bus_space_write_1(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int16_t *addr, size_t count)
{
	for (; count > 0; offset += 2, addr++, count--)
		bus_space_write_2(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int32_t *addr, size_t count)
{
	for (; count > 0; offset += 4, addr++, count--)
		bus_space_write_4(tag, bsh, offset, *addr);
}


static __inline void
bus_space_set_region_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int8_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_1(tag, bsh, offset, value);
}


static __inline void
bus_space_set_region_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_2(tag, bsh, offset, value);
}


static __inline void
bus_space_set_region_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_4(tag, bsh, offset, value);
}


static __inline int
bus_space_subregion(bus_space_tag_t t __unused, bus_space_handle_t bsh,
    bus_size_t offset, bus_size_t size __unused, bus_space_handle_t *nbshp)
{
	*nbshp = bsh + offset;
	return 0;
}

#define	BUS_SPACE_BARRIER_READ	0x01		/* force read barrier */
#define	BUS_SPACE_BARRIER_WRITE	0x02		/* force write barrier */


static __inline void
bus_space_barrier(bus_space_tag_t tag __unused, bus_space_handle_t bsh __unused,
	bus_size_t offset __unused, bus_size_t len __unused, int flags)
{
	__compiler_membar();
}


#include <machine/bus_dma.h>

/* Stream accessors are raw (no byte-swap), unlike the regular ones. */
#define	bus_space_read_stream_1(t, h, o)	bus_space_read_1((t), (h), (o))
#define	bus_space_read_stream_2(t, h, o) \
	(*(volatile u_int16_t *)((h) + (o)))
#define	bus_space_read_stream_4(t, h, o) \
	(*(volatile u_int32_t *)((h) + (o)))

#define	bus_space_read_multi_stream_1(t, h, o, a, c) \
	bus_space_read_multi_1((t), (h), (o), (a), (c))
#define	bus_space_read_multi_stream_2(t, h, o, a, c) \
	bus_space_read_multi_2((t), (h), (o), (a), (c))
#define	bus_space_read_multi_stream_4(t, h, o, a, c) \
	bus_space_read_multi_4((t), (h), (o), (a), (c))

#define	bus_space_write_stream_1(t, h, o, v) \
	bus_space_write_1((t), (h), (o), (v))
#define	bus_space_write_stream_2(t, h, o, v) \
	(*(volatile u_int16_t *)((h) + (o)) = (v))
#define	bus_space_write_stream_4(t, h, o, v) \
	(*(volatile u_int32_t *)((h) + (o)) = (v))

#define	bus_space_write_multi_stream_1(t, h, o, a, c) \
	bus_space_write_multi_1((t), (h), (o), (a), (c))
#define	bus_space_write_multi_stream_2(t, h, o, a, c) \
	bus_space_write_multi_2((t), (h), (o), (a), (c))
#define	bus_space_write_multi_stream_4(t, h, o, a, c) \
	bus_space_write_multi_4((t), (h), (o), (a), (c))

#define	bus_space_set_multi_stream_1(t, h, o, v, c) \
	bus_space_set_multi_1((t), (h), (o), (v), (c))
#define	bus_space_set_multi_stream_2(t, h, o, v, c) \
	bus_space_set_multi_2((t), (h), (o), (v), (c))
#define	bus_space_set_multi_stream_4(t, h, o, v, c) \
	bus_space_set_multi_4((t), (h), (o), (v), (c))

#define	bus_space_read_region_stream_1(t, h, o, a, c) \
	bus_space_read_region_1((t), (h), (o), (a), (c))
#define	bus_space_read_region_stream_2(t, h, o, a, c) \
	bus_space_read_region_2((t), (h), (o), (a), (c))
#define	bus_space_read_region_stream_4(t, h, o, a, c) \
	bus_space_read_region_4((t), (h), (o), (a), (c))

#define	bus_space_write_region_stream_1(t, h, o, a, c) \
	bus_space_write_region_1((t), (h), (o), (a), (c))
#define	bus_space_write_region_stream_2(t, h, o, a, c) \
	bus_space_write_region_2((t), (h), (o), (a), (c))
#define	bus_space_write_region_stream_4(t, h, o, a, c) \
	bus_space_write_region_4((t), (h), (o), (a), (c))

#define	bus_space_set_region_stream_1(t, h, o, v, c) \
	bus_space_set_region_1((t), (h), (o), (v), (c))
#define	bus_space_set_region_stream_2(t, h, o, v, c) \
	bus_space_set_region_2((t), (h), (o), (v), (c))
#define	bus_space_set_region_stream_4(t, h, o, v, c) \
	bus_space_set_region_4((t), (h), (o), (v), (c))

#define	bus_space_copy_region_stream_1(t, h1, o1, h2, o2, c) \
	bus_space_copy_region_1((t), (h1), (o1), (h2), (o2), (c))
#define	bus_space_copy_region_stream_2(t, h1, o1, h2, o2, c) \
	bus_space_copy_region_2((t), (h1), (o1), (h2), (o2), (c))
#define	bus_space_copy_region_stream_4(t, h1, o1, h2, o2, c) \
	bus_space_copy_region_4((t), (h1), (o1), (h2), (o2), (c))


#endif /* _FBSD_MACHINE_GENERIC_BUS_H_ */
