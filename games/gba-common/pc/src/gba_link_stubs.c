/*
 * The link-cable boot libraries (multiboot.c and libgcnmultiboot.s, both
 * naked asm) have no partner to talk to on this machine: every attempt
 * reports that nothing answered, which the games handle as a missing
 * cable or GameCube.
 */
#include <stdint.h>

struct MultiBootParam;
struct GcmbStruct;

void MultiBootInit(struct MultiBootParam *mp) { (void)mp; }
int MultiBootMain(struct MultiBootParam *mp) {
    (void)mp;
    return 0;
}
void MultiBootStartMaster(struct MultiBootParam *mp, const uint8_t *srcp, int length, uint8_t palette_color,
                          int8_t palette_speed) {
    (void)mp, (void)srcp, (void)length, (void)palette_color, (void)palette_speed;
}
int MultiBootCheckComplete(struct MultiBootParam *mp) {
    (void)mp;
    return 0;
}

void GameCubeMultiBoot_Main(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_Init(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_Quit(void) {}
