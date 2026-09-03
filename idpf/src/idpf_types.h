/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_TYPES_H_
#define _IDPF_TYPES_H_

/*
 * The shared code spells the fixed-width types the Linux way, so the driver
 * headers that carry shared definitions use them too.  virtchnl2.h and
 * virtchnl2_lan_desc.h keep their own self-contained copies of this block
 * because they are also consumed outside the kernel build; the shared guard
 * means whichever header is reached first defines the types.
 */
#ifndef IDPF_TYPE_COMPAT_INCLUDED
#define IDPF_TYPE_COMPAT_INCLUDED

/* Basic integer types - these are compiler built-ins, no includes needed */
typedef __UINT8_TYPE__		u8;
typedef __INT8_TYPE__		s8;
typedef __UINT16_TYPE__		u16;
typedef __INT16_TYPE__		s16;
typedef __UINT32_TYPE__		u32;
typedef __INT32_TYPE__		s32;
typedef __UINT64_TYPE__		u64;
typedef __INT64_TYPE__		s64;

/* Little-endian types used by shared code */
#define __le16		u16
#define __le32		u32
#define __le64		u64

/* Big-endian types used by shared code */
#define __be16		u16
#define __be32		u32
#define __be64		u64

/* Basic macros needed by shared code */
#ifndef BIT
#define BIT(a) (1UL << (a))
#endif

#ifndef BIT_ULL
#define BIT_ULL(a) (1ULL << (a))
#endif

#ifndef BITS_PER_LONG
#define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#endif

#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG (__SIZEOF_LONG_LONG__ * 8)
#endif

#ifndef GENMASK
#define GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif

#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) \
    (((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#endif

#ifndef DEBUGFUNC
#define DEBUGFUNC(S) do { } while (0)
#endif

#endif /* IDPF_TYPE_COMPAT_INCLUDED */

#endif /* _IDPF_TYPES_H_ */
