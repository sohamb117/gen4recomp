/*
 * The ARM7's power-management responder on PXI tag 8, for Diamond/Pearl.
 *
 * Platinum needs none: its SDK (4.2) reaches the PMIC through
 * PMi_ReadRegister/PMi_WriteRegister, which pc_pm.c answers directly. D's
 * SDK (3.2, arm9/asm/SPI_pm.s, recompiled) has those too (and pc_pm.c
 * replaces them the same way), but also talks to the ARM7 in raw PXI words:
 * PM_SendUtilityCommandAsync (backlight, LCD power, amp, amp gain:
 * command 0x63), the LED pattern get/set (0x67/0x66) and the sleep sequence
 * (0x60/0x61). Each takes the PM lock and the caller then spins in
 * PMi_WaitBusy until PMi_CommonCallback, the tag's receive callback,
 * releases it. With no responder pc_pxi.c drops the words and that spin
 * never ends.
 *
 * The wire format, from SPI_pm.s: a command is one or more 32-bit words;
 * the first carries the start bit (0x02000000) and the command in bits 8-14,
 * the last carries the end bit (0x01000000); a one-word command has both
 * (PM_GetLEDPatternAsync sends 0x03006700). PMi_CommonCallback reads the
 * reply as command = bits 8-14, result = bits 0-7, and for 0x67 stores the
 * low byte as the pattern.
 *
 * The answer is the one the ARM7 gives a command it carried out: the
 * command echoed with result 0 (PM_RESULT_SUCCESS). A desktop has no PMIC
 * whose state could disagree; the registers pc_pm.c models are the ones a
 * game reads back, through PMi_ReadRegister, which never reaches this tag.
 * The LED pattern reads back as 0 (PM_LED_PATTERN_NONE).
 */
#include <nitro.h>
#include <nitro/pxi.h>

#define PC_DP_PM_START_BIT 0x02000000u
#define PC_DP_PM_END_BIT   0x01000000u
#define PC_DP_PM_CMD_MASK  0x00007F00u
#define PC_DP_PM_RESULT_SUCCESS 0u

extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
extern void pc_pxi_reply(int tag, u32 data);

static u32 sCommand;

static void pm_respond(u32 data)
{
    if (data & PC_DP_PM_START_BIT) {
        sCommand = data & PC_DP_PM_CMD_MASK;
    }
    if (data & PC_DP_PM_END_BIT) {
        pc_pxi_reply(PXI_FIFO_TAG_PM, sCommand | PC_DP_PM_RESULT_SUCCESS);
    }
}

void pc_dp_pm_init(void)
{
    pc_pxi_set_responder(PXI_FIFO_TAG_PM, pm_respond);
}
