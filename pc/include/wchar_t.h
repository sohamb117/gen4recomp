/* pc stand-in for the Metrowerks MSL <wchar_t.h>.
 *
 * On the DS toolchain wchar_t is 16 bits; g2di_SplitChar.c casts UTF-16
 * string pointers to `const wchar_t *`, so the width is load-bearing,
 * do NOT let the host's 32-bit wchar_t leak in here.  The guard macros
 * below are the ones gcc's <stddef.h> honours, so a later stddef.h
 * inclusion will not redefine the type. */
#ifndef PC_MSL_WCHAR_T_H_
#define PC_MSL_WCHAR_T_H_

#if !defined(__cplusplus) && !defined(_WCHAR_T) && !defined(_WCHAR_T_DEFINED)
#define _WCHAR_T
#define _WCHAR_T_
#define _WCHAR_T_DEFINED
#define __WCHAR_T__
typedef unsigned short wchar_t;
#endif

#endif
