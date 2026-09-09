/**
 * @file idpf_type_compat.h
 * @brief Minimal type compatibility layer for shared code compilation
 *
 * This header provides only the Linux-style type definitions needed by
 * shared code, without any kernel includes. It's designed to be force-included
 * via -include during compilation of shared code files.
 *
 * This file must NOT include any other headers to avoid dependency issues
 * during early preprocessing.
 */
#ifndef _IDPF_TYPE_COMPAT_H_
#define _IDPF_TYPE_COMPAT_H_

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

#ifndef __KERNEL__
#ifndef GENMASK
#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif

#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#endif
#endif /* !__KERNEL__ */

#ifndef static_assert
#define static_assert	_Static_assert
#endif

#ifndef struct_size_t
#define struct_size_t(type, member, count) \
	(__builtin_offsetof(type, member) + sizeof(((type *)0)->member[0]) * (count))
#endif

#ifndef VIRTCHNL2_EDT_SUPPORT
#define VIRTCHNL2_EDT_SUPPORT
#endif

#ifndef DEBUGFUNC
#define DEBUGFUNC(S) do { } while (0)
#endif

/* Indicate to idpf_osdep.h that types are already defined */
#define IDPF_TYPE_COMPAT_INCLUDED

#endif /* _IDPF_TYPE_COMPAT_H_ */
