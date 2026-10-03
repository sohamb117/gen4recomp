/* pc stand-in for the Metrowerks MSL <null.h>: just NULL.
 * Needed by lib/spl/src/spl_list.c, which includes <null.h> directly. */
#ifndef PC_MSL_NULL_H_
#define PC_MSL_NULL_H_

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#endif
