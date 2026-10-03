/*
 * NOTE:
 * This file is shared between ARM9 and ARM7
 * Do not put proc specific code in here
 * Thank You!
 */

/*
 * Do not include this file directly
 * Include OS_spinLock.h from the specific proc's lib
 */

#ifndef POKEDIAMOND_OS_SPINLOCK_SHARED_H
#define POKEDIAMOND_OS_SPINLOCK_SHARED_H

#include "nitro/types.h"

typedef volatile struct OSLockWord {
    u32 lockFlag;
    u16 ownerID;
    u16 extension;
} OSLockWord;

#endif //POKEDIAMOND_OS_SPINLOCK_SHARED_H
