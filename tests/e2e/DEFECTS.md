# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

## D/P: the Vs. Seeker never readies a beaten trainer (suspected, blocks diamond/99-vs-seeker)

Seen: with the seeker charged (var 0x4033 = 100) and registered, Y next to the beaten Youngster Tristan on Route 202
always answers msg 486 #2 "The other Trainers don't appear to be ready for battle." and he never rematches.
35 tries, none readied: 9 idle delays before Y (0..60 frames), 16 different `PC_RTC` clocks, 5 walks back and
forth, 5 walks through the tall grass at (162..165, 819..821) including a wild battle. With a 50% roll per beaten
trainer the chance of that is about 2^-35.

Repro: `python3 tests/e2e/run.py --game diamond --systems --planned --only 99-vs-seeker` (start.recipe: flag 0x97F,
the seeker registered, var 0x4033 100, Tristan's trainer flag 0x551, the player at (166,816)); the sheet's
"seeker" shot shows the message.

What is known: the message is result 3 of the seeker task (ov05_021E1374.s case 4 -> scr_seq 0463 L_0059), set
when ov05_021E17A0 returns 0: every in-range trainer was beaten (TrainerFieldSystem_FlagCheck != 0) and failed
`LCRandom() % 100 < 50`. With Tristan unbeaten (no 0x551) the message does not come, so he is a candidate and the
flag check works. The recompiled roll (armrec ov05_021E1374.c, L__021E17DE) takes the remainder from
`_s32_div_f`'s r1 as the ARM code does. Not yet known: why LCRandom's value at the roll never lands below 50 (an
lldb trace of the `w2c_diamond_c2u0x24LCRandom` return inside `w2c_diamond_ov05_021E17A0` is the next step).

## D/P: the GTS connect screen waits with no time-out (observation, not blocking)

Seen: the GTS receptionist's trade path opens "Connecting to Nintendo Wi-Fi Connection... Please wait a moment..."
and nothing changes for 1800+ frames; B cancels it ("Reconnect to Nintendo WFC?"), NO returns to the field.
The port models an unconfigured console (games/platinum/pc/src/pc_dwc_auth.c). Whether a real unconfigured DS
shows an error code here instead of waiting is [INFERENCE: likely, unverified]. diamond/97-gts-offline uses B + NO.
