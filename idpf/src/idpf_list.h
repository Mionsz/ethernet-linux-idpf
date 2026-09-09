/**
 * @file idpf_list.h
 * @brief FreeBSD backing for the shared code's OS-supplied list primitives.
 *
 * The shared tree (idpf/shared submodule) includes "idpf_list.h" from
 * idpf_type.h but never provides it: each OS port supplies its own. This
 * driver-owned version maps the shared LIST_*_TYPE / LIST_FOR_EACH_ENTRY*
 * names onto FreeBSD's <sys/queue.h> LIST(3) macros, and is pulled in by
 * idpf_osdep.h.
 */
#ifndef _IDPF_LIST_H_
#define _IDPF_LIST_H_

#include <sys/queue.h>

/** Get type of list entry structure.

   @param[in]   Type    Underlying data type.

   @return      Entry type.
**/
#define LIST_ENTRY_TYPE(Type)                 LIST_ENTRY(Type)

/** Get type of list head structure.

   @param[in]   List    List name.
   @param[in]   Type    Underlying data type.

   @return      Head type.
**/
#define LIST_HEAD_TYPE(List, Type)            LIST_HEAD(List, Type)

/** Iterate over a list.

   @param[in]   Pos     Loop iterator.
   @param[in]   Head    Head of the list.
   @param[in]   Type    Underlying data type (unused).
   @param[in]   List    Name of LIST_ENTRY_TYPE member in struct Type.
**/
/* Guarded: the CppUTest harness supplies its own list.h implementation. */
#ifndef LIST_FOR_EACH_ENTRY
#define LIST_FOR_EACH_ENTRY(Pos, Head, Type, List)			\
	LIST_FOREACH((Pos), (Head), List)
#endif

/** Iterate over a list, safe against removal of the current element.

   @param[in]   Pos     Loop iterator.
   @param[in]   Temp    Temporary iterator pointer.
   @param[in]   Head    Head of the list.
   @param[in]   Type    Underlying data type (unused).
   @param[in]   List    Name of LIST_ENTRY_TYPE member in struct Type.
**/
#ifndef LIST_FOR_EACH_ENTRY_SAFE
#define LIST_FOR_EACH_ENTRY_SAFE(Pos, Temp, Head, Type, List)		\
	LIST_FOREACH_SAFE((Pos), (Head), List, (Temp))
#endif

#endif /* _IDPF_LIST_H_ */
