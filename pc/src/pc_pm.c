/*
 * The power-management IC, as a register file.
 *
 * PM traffic is an ARM7 SPI conversation; every entry point in the SDK's
 * pm.c funnels through PMi_ReadRegister(Async)/PMi_WriteRegister(Async).
 * Overriding that funnel with an 8-register model gives the whole PM
 * surface real behavior: writes stick, reads return them, and the async
 * forms complete synchronously and run their callback with SUCCESS,
 * the same completed-immediately schedule as the rest of the port.
 *
 * Reset defaults (gbatek, pmic control register): both backlights on,
 * sound amplifier enabled, the state a booted console is actually in
 * by the time game code asks. PM_GetBackLight at boot reads exactly this.
 */
#include <nitro/spi/ARM9/pm.h>

#define PC_PMIC_REGS 8

/* reg 0 (control): bit0 sound amp on, bit2 lower backlight, bit3 upper. */
static u16 sPmicReg[PC_PMIC_REGS] = { 0x000D, 0, 0, 0, 0, 0, 0, 0 };

u32 PMi_ReadRegisterAsync(u16 registerAddr, u16 *buffer,
                          PMCallback callback, void *arg)
{
    *buffer = sPmicReg[registerAddr % PC_PMIC_REGS];
    if (callback) {
        callback(PM_RESULT_SUCCESS, arg);
    }
    return PM_SUCCESS;
}

u32 PMi_ReadRegister(u16 registerAddr, u16 *buffer)
{
    return PMi_ReadRegisterAsync(registerAddr, buffer, NULL, NULL);
}

u32 PMi_WriteRegisterAsync(u16 registerAddr, u16 data,
                           PMCallback callback, void *arg)
{
    sPmicReg[registerAddr % PC_PMIC_REGS] = data;
    if (callback) {
        callback(PM_RESULT_SUCCESS, arg);
    }
    return PM_SUCCESS;
}

u32 PMi_WriteRegister(u16 registerAddr, u16 data)
{
    return PMi_WriteRegisterAsync(registerAddr, data, NULL, NULL);
}

/*
 * The two PXI word builders pm.c declares `inline` with no `extern` and no
 * `static`. C99 says a plain `inline` definition provides no external symbol,
 * so a call the compiler does not inline needs one from somewhere else,
 * and pm.c is compiled with inlining off on purpose, because the port
 * overrides SDK functions in that file and a same-TU call that got inlined
 * could not be preempted (pc/Makefile says which five objects and why).
 *
 * gcc still folds these two: they are one expression each and it inlines them
 * below its `-fno-inline-functions` line. Clang does not, so an Android build
 * came out with PMi_MakeData1 and PMi_MakeData2 undefined, which links,
 * because a shared object may have undefined symbols, and then fails at
 * dlopen, because Android's linker binds everything up front.
 *
 * So the external definitions live here. They are the same expressions pm.c
 * gives: a start/end bit, a sequence index, a command byte and a payload,
 * packed into the PXI word the ARM7 side reads. Unused on any toolchain that
 * folded the calls away.
 */
u32 PMi_MakeData1(u32 bit, u32 seq, u32 command, u32 data)
{
    return bit | (seq << SPI_PXI_INDEX_SHIFT) | (command << 8) | (data & 0xff);
}

u32 PMi_MakeData2(u32 bit, u32 seq, u32 data)
{
    return bit | (seq << SPI_PXI_INDEX_SHIFT) | (data & 0xffff);
}
