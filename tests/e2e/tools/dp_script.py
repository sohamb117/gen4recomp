#!/usr/bin/env python3
"""dp_script.py - make Pokemon Diamond/Pearl (pret/pokediamond) binary field data citable.

Reads the pokediamond tree under games/diamond (stdlib only, read-only):
  map table      arm9/src/map_header.c sMapHeaders (row = map id; names from include/constants/maps.h)
  field scripts  files/fielddata/script/scr_seq_release/narc_NNNN.bin
  level scripts  same NARC, the map header's level_scripts_bank file
  map events     files/fielddata/eventdata/zone_event_release/narc_NNNN.bin
  trainers       files/poketool/trainer/trdata.json (source of trdata/trpoke NARCs)
  text           files/msgdata/msg/narc_NNNN.gmm (source of the msg NARC)

Cite facts printed by this tool as  `scr_seq NNNN @0xOFF`, `zone_event NNNN object k`, `maps.h:LINE`,
`map_header.c:LINE`, `trdata.json #N`, `msg NNNN #i`.

The script command table (721 commands, ids 0x000-0x2D0) is the D/P table gScriptCmdTable
(arm9/asm/unk_02038C78.s:150); argument layouts were derived from each ScrCmd_* body (file:line in
`opcodes`). Disassembly stops with an error at any unknown opcode, truncated instruction or
out-of-file jump: it never silently desyncs.
"""

import argparse
import json
import re
import signal
import struct
import sys
import xml.etree.ElementTree as ET
from functools import cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3] / "games" / "diamond"

SCR_DIR = "files/fielddata/script/scr_seq_release"
EVT_DIR = "files/fielddata/eventdata/zone_event_release"
MSG_DIR = "files/msgdata/msg"
TRDATA = "files/poketool/trainer/trdata.json"

# ----------------------------------------------------------------------------------------------
# Script command table.  Columns: id name args source [decomp todo hint]
# Arg kinds: b=u8  h=u16 literal  v=u16 var-or-value (ScriptGetVar: >=0x4000 reads a var)
#            p=u16 var id (ScriptGetVarPointer)  w=u32  o=s32 code offset (rel. to next byte)
#            m=s32 movement-list offset  *=selector-dependent (see VAR_ARGS)  -=no args
# ----------------------------------------------------------------------------------------------
OPTABLE = r"""
000 Nop - scrcmd.c:413
001 Dummy - scrcmd.c:419
002 End - scrcmd.c:425
003 Wait hp scrcmd.c:431
004 LoadByte bb scrcmd.c:455
005 LoadWord bw scrcmd.c:462
006 LoadByteFromAddr bw scrcmd.c:469
007 WriteByteToAddr wb scrcmd.c:476
008 SetPtrByte wb scrcmd.c:483
009 CopyLocal bb scrcmd.c:490
00A CopyByte ww scrcmd.c:498
00B CompareLocalToLocal bb scrcmd.c:516
00C CompareLocalToValue bb scrcmd.c:524
00D CompareLocalToAddr bw scrcmd.c:532
00E CompareAddrToLocal wb scrcmd.c:540
00F CompareAddrToValue wb scrcmd.c:548
010 CompareAddrToAddr ww scrcmd.c:556
011 CompareVarToValue ph scrcmd.c:564
012 CompareVarToVar pp scrcmd.c:572
013 RunScript h scrcmd.c:580
014 RunScriptWait h scrcmd.c:592
015 RestartCurrentScript - scrcmd.c:617
016 GoTo o scrcmd.c:625
017 ObjectGoTo bo scrcmd.c:632
018 BgGoTo bo scrcmd.c:643
019 DirectionGoTo bo scrcmd.c:655
01A Call o scrcmd.c:667
01B Return - scrcmd.c:674
01C GoToIf bo scrcmd.c:680
01D CallIf bo scrcmd.c:691
01E SetFlag h scrcmd.c:702
01F ClearFlag h scrcmd.c:710
020 CheckFlag h scrcmd.c:718
021 CheckFlagVar pp scrcmd.c:726
022 SetFlagVar p scrcmd.c:735
023 SetTrainerFlag v scrcmd.c:743
024 ClearTrainerFlag v scrcmd.c:751
025 CheckTrainerFlag v scrcmd.c:759
026 AddVar pv scrcmd.c:767
027 SubVar pv scrcmd.c:775
028 SetVar ph scrcmd.c:783
029 CopyVar pp scrcmd.c:790
02A SetOrCopyVar pv scrcmd.c:798 better_name
02B Message b scrcmd.c:806 MessageAll?
02C Unk002C b scrcmd.c:908 Message?
02D Unk002D v scrcmd.c:921 MessageFromVar?_MessageFlex?
02E Unk002E v scrcmd.c:943 MessageWait?_MessageNoSkip?
02F Unk002F b scrcmd.c:960
030 WaitButtonAB - scrcmd.c:978
031 WaitButton - scrcmd.c:1011
032 WaitButtonABPad - scrcmd.c:1036
033 Unk0033 - scrcmd.c:1052 OpenMessageBox?
034 CloseMessageBox - scrcmd.c:1062
035 Unk0035 - scrcmd.c:1073 FreezeMessageBox?
036 CreateMessageBox bbhh scrcmd.c:1144
037 Unk0037 bh scrcmd.c:1173 SetTextBoard?_ColourMessageBox?
038 Unk0038 b scrcmd.c:1185 ShowMessageBox?
039 Unk0039 - scrcmd.c:1192 MessageBoxWait?_WaitMessageBox?
03A Unk003A bh scrcmd.c:1209 CreateMessageBoxText?
03B Unk003B h scrcmd.c:1265 CloseMessageBox?
03C Menu - scrcmd.c:1303
03D ScrollBg bbbbbb scrcmd.c:1083
03E YesNoMenu h scrcmd.c:1308
03F Unk003F - scrcmd.c:3433
040 Unk0040 bbbbp scrcmd.c:1350
041 Unk0041 bbbbp scrcmd.c:1366
042 Unk0042 bb scrcmd.c:1382
043 Unk0043 - scrcmd.c:1399
044 Unk0044 bbbbp scrcmd.c:1440
045 Unk0045 bbbbp scrcmd.c:1457
046 Unk0046 vvv scrcmd.c:1474 AddListOption?
047 Unk0047 - scrcmd.c:1483 ShowList?
048 Unk0048 b scrcmd.c:1490
049 PlayFanfare v scrcmd_sound.c:110
04A StopFanfare v scrcmd_sound.c:117
04B PlayFanfareWait v scrcmd_sound.c:124
04C PlayCry vv scrcmd_sound.c:142
04D PlayCryWait - scrcmd_sound.c:150
04E PlaySound h scrcmd_sound.c:166
04F PlaySoundWait - scrcmd_sound.c:174
050 PlayBgm h scrcmd_sound.c:28
051 StopBgm h scrcmd_sound.c:34
052 PlayDefaultBgm - scrcmd_sound.c:42
053 Unk0053 h scrcmd_sound.c:49 SetMusic?_SpecialMusic?
054 FadeOutBgm hh scrcmd_sound.c:55
055 FadeInBgm h scrcmd_sound.c:76
056 Unk0056 bb scrcmd_sound.c:86
057 Unk0057 h scrcmd_sound.c:97 PlayFieldBgm?
058 Unk0058 b scrcmd_sound.c:103
059 CheckChatotCry p scrcmd_sound.c:189
05A StartChatotRecord p scrcmd_sound.c:203
05B StopChatotRecord - scrcmd_sound.c:216
05C SaveChatotCry - scrcmd_sound.c:223
05D Unk005D - scrcmd_sound.c:231 LoadSpearPillarAudio?
05E Unk005E vm scrcmd.c:1509 ApplyMovement?
05F WaitForMovement - scrcmd.c:1583
060 LockAllEvents - scrcmd.c:1629
061 ReleaseAllEvents - scrcmd.c:1715
062 LockEvent h scrcmd.c:1720
063 ReleaseEvent h scrcmd.c:1727
064 AddEvent v scrcmd.c:1734
065 RemoveEvent v scrcmd.c:1745
066 LockCamera vv scrcmd.c:1752
067 ReleaseCamera - scrcmd.c:1766
068 FacePlayer - scrcmd.c:1775
069 GetPlayerPosition pp scrcmd.c:1788
06A GetEventPosition vpp scrcmd.c:1800
06B Unk006B vvv scrcmd.c:1820 CheckPersonPosition?
06C KeepEvent vb scrcmd.c:1835
06D SetEventMovement vh scrcmd.c:1842
06E EventStopFollowing - scrcmd.c:1862
06F GiveMoney w scrcmd_money.c:12
070 TakeMoneyImmediate w scrcmd_money.c:23 TakeMoney?
071 HasEnoughMoneyImmediate pw scrcmd_money.c:45 CanAffordMoney?
072 ShowMoneyBox vv scrcmd_money.c:81
073 HideMoneyBox - scrcmd_money.c:93
074 UpdateMoneyBox - scrcmd_money.c:101
075 ShowCoinBox vv scrcmd_coins.c:12
076 HideCoinBox - scrcmd_coins.c:25
077 UpdateCoinBox - scrcmd_coins.c:33
078 GetCoins p scrcmd_coins.c:41 Coins_GetValue_instead?
079 GiveCoins v scrcmd_coins.c:51
07A TakeCoinsImmediate v scrcmd_coins.c:61 Coins_Subtract_instead?
07B GiveItem vvp scrcmd_items.c:8
07C TakeItem vvp scrcmd_items.c:22
07D HasSpaceForItem vvp scrcmd_items.c:36
07E HasItem vvp scrcmd_items.c:50
07F ItemIdIsTMOrHM vp scrcmd_items.c:64
080 GetItemPocketId vp scrcmd_items.c:76
081 Unk0081 - scrcmd_items.c:88 DummyGiveItem?
082 Unk0082 - scrcmd_items.c:94 DummyHasItem?
083 GiveSecretBaseDecoration vvp scrcmd_underground.c:13
084 TakeSecretBaseDecoration vvp scrcmd_underground.c:26
085 HasSpaceForDecoration vvp scrcmd_underground.c:35
086 GetDecorationCount vvp scrcmd_underground.c:48
087 GiveUndergroundTrap vvp scrcmd_underground.c:57
088 TakeUndergroundTrap vvp scrcmd_underground.c:70
089 HasSpaceForTrap vvp scrcmd_underground.c:79
08A GetTrapCount vvp scrcmd_underground.c:88
08B GiveTreasure vvp scrcmd_underground.c:97
08C TakeTreasure vvp scrcmd_underground.c:108
08D HasSpaceForTreasure vvp scrcmd_underground.c:117
08E GetTreasureCount vvp scrcmd_underground.c:126
08F GiveUndergroundSphere vvp scrcmd_underground.c:135
090 TakeUndergroundSphere vvp scrcmd_underground.c:148
091 HasSpaceForSphere vvp scrcmd_underground.c:157
092 GetSphereCount vvp scrcmd_underground.c:166
093 GetSealCountFromId vp scrcmd.c:1875
094 GiveSeals vv scrcmd.c:1883
095 GetPokemonForm vp scrcmd.c:1892
096 GiveMon vvvp scrcmd_party.c:19 GivePokemon?
097 GiveEgg vv scrcmd_party.c:77
098 SetPartyMonMove vvv scrcmd_party.c:100 ReplacePartyPokemonMove?
099 PartyMonHasMove pvv scrcmd_party.c:112
09A FindPartyMonWithMove pv scrcmd_party.c:135 CheckMoveInParty?
09B Unk009B vp scrcmd.c:1991
09C DummySetWeather - scrcmd.c:2000
09D DummyInitWeather - scrcmd.c:2005
09E DummyUpdateWeather - scrcmd.c:2010
09F DummyGetMapPosition - scrcmd.c:2015
0A0 Unk00A0 - scrcmd_party.c:747 DummyCountPartyPokemon
0A1 RestoreOverworld - scrcmd.c:2051
0A2 Unk00A2 - scrcmd.c:2088
0A3 Unk00A3 - scrcmd.c:2093
0A4 GetDressupPortraitSlot p scrcmd.c:2098
0A5 Unk00A5 - scrcmd.c:2173
0A6 DressupPokemon vpv scrcmd.c:2178
0A7 ShowDressedPokemon hp scrcmd.c:2186
0A8 ShowContestPokemon hp scrcmd.c:2201
0A9 ShowSealCapsuleEditor - scrcmd.c:2251
0AA ShowTownMapScreen - scrcmd.c:2256
0AB ShowPCBoxScreen b scrcmd.c:2302
0AC Unk00AC - scrcmd.c:2313
0AD Unk00AD - scrcmd.c:2319
0AE Unk00AE - scrcmd.c:2324
0AF Unk00AF - scrcmd.c:2329
0B0 ShowEndGameScreen - scrcmd.c:2336
0B1 InitHallOfFame - scrcmd.c:2341
0B2 Unk00B2 vp scrcmd.c:2348
0B3 Unk00B3 p scrcmd.c:2361
0B4 StarterSelectionScreen - scrcmd.c:2367
0B5 EndStarterSelectionScreen - scrcmd.c:2377
0B6 Unk00B6 v scrcmd_7.c:27
0B7 Unk00B7 vp scrcmd_7.c:61
0B8 Unk00B8 p scrcmd_7.c:83
0B9 Unk00B9 vp scrcmd_7.c:92
0BA NamePlayerScreen p scrcmd.c:2407
0BB NamePokemonScreen vp scrcmd.c:2413
0BC FadeScreen hhhh scrcmd.c:2465
0BD WaitFadeScreen - scrcmd.c:2476
0BE Warp hhvvh scrcmd.c:2485
0BF RockClimb v scrcmd.c:2523
0C0 Surf v scrcmd.c:2530
0C1 Waterfall v scrcmd.c:2538
0C2 Fly hvv scrcmd.c:2545
0C3 Flash - scrcmd.c:2553
0C4 Defog - scrcmd.c:2560
0C5 Cut v scrcmd.c:2567
0C6 ApplyContestDress - scrcmd.c:2586
0C7 CheckBike p scrcmd.c:2591
0C8 RideBike b scrcmd.c:2601
0C9 CyclingRoad b scrcmd.c:2623
0CA GetPlayerState p scrcmd.c:2629
0CB SetPlayerState h scrcmd.c:2635
0CC ApplyPlayerState - scrcmd.c:2641
0CD GetPlayerName b scrcmd_names.c:29 BufferPlayerName?_TextPlayerName?
0CE GetRivalName b scrcmd_names.c:42 BufferRivalName?_TextRivalName?
0CF GetFriendName b scrcmd_names.c:53 BufferFriendName?_TextFriendName?
0D0 GetPokemonName bv scrcmd_names.c:64 BufferPartyPokemonName?_TextPartyPokemonName?
0D1 GetItemName bv scrcmd_names.c:78 BufferItemName?_TextItemName?
0D2 GetPocketName bv scrcmd_names.c:90 BufferPocketName?_TextPocketName?
0D3 GetTMHMMoveName bv scrcmd_names.c:102 BufferTMHMName?_BufferMachineName?_TextTMHMName?_TextMachineName?
0D4 GetMoveName bv scrcmd_names.c:115 BufferMoveName?_TextMoveName?
0D5 Unk00D5 bv scrcmd_names.c:127 BufferNumber?_TextNumber?
0D6 GetPokemonNickname bv scrcmd_names.c:155 BufferPartyPokemonNickname?_TextPartyPokemonNickname?
0D7 GetPoketchAppName bv scrcmd_names.c:185 BufferPoketchAppName?_TextPoketchAppName?
0D8 GetTrainerClassName bv scrcmd_names.c:196 BufferTrainerClassName?_TextTrainerClassName?
0D9 Unk00D9 b scrcmd_names.c:207 BufferPlayerTrainerClassName?_TextPlayerTrainerClassName?
0DA Unk00DA bvhb scrcmd_names.c:223 BufferPokemonSpeciesName?_TextPokemonSpeciesName?
0DB GetPlayerStarterName b scrcmd_names.c:247 BufferPlayerStarterSpecies?
0DC GetRivalStarterName b scrcmd_names.c:261 BufferRivalStarterSpecies?
0DD GetCounterpartStarterName b scrcmd_names.c:275 BufferFriendStarterSpecies?
0DE GetStarter p scrcmd.c:2654
0DF GetDecorationName bv scrcmd_names.c:289 BufferDecorationName?
0E0 GetUndergroundTrapName bv scrcmd_names.c:300 BufferUndergroundTrapName?
0E1 GetUndergroundItemName bv scrcmd_names.c:311 BufferUndergroundItemName?_BufferTreasureName?
0E2 GetMapName bv scrcmd_names.c:322 BufferMapName?
0E3 GetSwarmInfo pp scrcmd.c:2646
0E4 Unk00E4 p scrcmd_7.c:104
0E5 Unk00E5 vv scrcmd_7.c:113
0E6 TrainerMessage vv scrcmd.c:2660
0E7 Unk00E7 ppp scrcmd_7.c:142
0E8 Unk00E8 ppp scrcmd_7.c:173
0E9 Unk00E9 p scrcmd_7.c:204
0EA Unk00EA v scrcmd_7.c:214
0EB Unk00EB - scrcmd_7.c:220
0EC Unk00EC p scrcmd_7.c:226
0ED Unk00ED p scrcmd_7.c:236
0EE Unk00EE p scrcmd_7.c:256
0EF Unk00EF - scrcmd_7.c:265
0F0 Unk00F0 - scrcmd_7.c:274
0F1 Unk00F1 o scrcmd_7.c:283
0F2 Unk00F2 vvvh scrcmd.c:2677
0F3 Unk00F3 vvvh scrcmd.c:2700
0F4 Unk00F4 h scrcmd.c:2723
0F5 Unk00F5 h scrcmd.c:2728
0F6 Unk00F6 - scrcmd.c:2733
0F7 Unk00F7 - scrcmd.c:2742
0F8 Unk00F8 v scrcmd_3.c:58
0F9 Unk00F9 v scrcmd_3.c:66
0FA Unk00FA vvvv scrcmd_3.c:82
0FB Unk00FB v scrcmd_3.c:118
0FC Unk00FC vv scrcmd_3.c:131
0FD Unk00FD vv scrcmd_3.c:142
0FE Unk00FE vv scrcmd_3.c:153
0FF Unk00FF vv scrcmd_3.c:164
100 Unk0100 - scrcmd_3.c:175
101 Unk0101 - scrcmd_3.c:191
102 Unk0102 v scrcmd_3.c:199
103 Unk0103 v scrcmd_3.c:209
104 Unk0104 v scrcmd_3.c:219
105 Unk0105 p scrcmd_3.c:229
106 Unk0106 v scrcmd_3.c:238
107 Unk0107 p scrcmd_3.c:248
108 Unk0108 p scrcmd_3.c:257
109 Unk0109 p scrcmd_3.c:265
10A Unk010A vp scrcmd_3.c:274
10B Unk010B vp scrcmd_3.c:284
10C Unk010C p scrcmd_3.c:294
10D Unk010D p scrcmd_3.c:315
10E Unk010E v scrcmd_3.c:324
10F Unk010F p scrcmd_3.c:334
110 Unk0110 pppp scrcmd_3.c:303
111 Unk0111 v scrcmd_3.c:359
112 Unk0112 - scrcmd_3.c:368
113 Unk0113 - scrcmd_3.c:382
114 Unk0114 - scrcmd_3.c:388
115 Unk0115 p scrcmd_3.c:394
116 Unk0116 - scrcmd_3.c:414
117 Unk0117 - scrcmd_3.c:343
118 Unk0118 - scrcmd_3.c:351
119 CheckPartyForPokerus p scrcmd_party.c:488
11A GetPartyMonGender vp scrcmd_party.c:511
11B SetDynamicWarp vvvvv scrcmd.c:2747
11C GetDynamicWarpFloorNumber p scrcmd.c:2758
11D ShowCurrentFloorNumber bbp scrcmd.c:2765
11E CountSinnohDexSeen p scrcmd.c:2775
11F CountSinnohDexOwned p scrcmd.c:2782
120 CountNationalDexSeen p scrcmd.c:2789
121 CountNationalDexOwned p scrcmd.c:2796
122 DummyDexCheck - scrcmd.c:2803
123 GetDexEvaluationMessage bp scrcmd.c:2807
124 WildBattle vv scrcmd.c:2820
125 FirstBattle vv scrcmd.c:2836
126 CatchTutorial - scrcmd.c:2843
127 UpdateHoneyTree - scrcmd.c:2848
128 CheckHoneyTree p scrcmd.c:2853
129 HoneyTreeBattle - scrcmd.c:2860
12A StopHoneyTreeAnimation - scrcmd.c:2866
12B ShowSignatureScreen - scrcmd.c:2871
12C CheckSaveStatus p scrcmd.c:2877
12D SaveGame p scrcmd.c:2893
12E CheckPortraitSlot hp scrcmd.c:2216
12F CheckContestPortraitSlot hp scrcmd.c:2227
130 Unk0130 v scrcmd.c:2238
131 GivePoketch - scrcmd.c:2900
132 CheckPoketch p scrcmd.c:2905
133 UnlockPoketchApp v scrcmd.c:2912
134 CheckPoketchApp vp scrcmd.c:2920
135 Unk0135 v scrcmd.c:2929
136 Unk0136 - scrcmd.c:2944
137 Unk0137 p scrcmd.c:2949
138 Unk0138 p scrcmd.c:2956
139 Unk0139 h scrcmd.c:2963
13A Unk013A - scrcmd.c:3069
13B Unk013B - scrcmd.c:3075
13C Unk013C h scrcmd.c:2977
13D Unk013D - scrcmd.c:2994
13E Unk013E - scrcmd.c:2999
13F Unk013F hp scrcmd.c:3010
140 Unk0140 p scrcmd.c:3029
141 Unk0141 h scrcmd.c:3046
142 Unk0142 - scrcmd.c:3064
143 Unk0143 vv scrcmd.c:3082
144 Unk0144 h scrcmd.c:3090
145 Unk0145 h scrcmd.c:3108
146 Unk0146 vp scrcmd.c:3037
147 NormalMart v scrcmd_mart.c:349 Pokemart?
148 SpecialMart v scrcmd_mart.c:400
149 GoodsMart v scrcmd_mart.c:416
14A SealsMart v scrcmd_mart.c:431
14B DummyBlackout - scrcmd.c:3163
14C SetSpawn v scrcmd.c:3168
14D GetPlayerGender p scrcmd.c:3175
14E HealParty - scrcmd.c:3182
14F Unk014F - scrcmd.c:3188
150 Unk0150 - scrcmd.c:3192
151 Unk0151 - scrcmd.c:3211
152 Unk0152 h scrcmd.c:3216
153 Unk0153 - scrcmd.c:3127
154 Unk0154 - scrcmd.c:3132
155 Unk0155 vp scrcmd.c:3139
156 SetPlayerAvatar v scrcmd.c:3156
157 HasSinnohDex p scrcmd_flags.c:38
158 GiveSinnohDex - scrcmd_flags.c:48
159 HasRunningShoes p scrcmd_flags.c:57
15A GiveRunningShoes - scrcmd_flags.c:68
15B HasBadge vp scrcmd_flags.c:78
15C GiveBadge v scrcmd_flags.c:90
15D GetTotalEarnedBadges p scrcmd_flags.c:111 CountBadges?
15E HasBag p scrcmd_flags.c:101
15F GiveBag - scrcmd_flags.c:129
160 Unk0160 p scrcmd_flags.c:138 HasPartner?_CheckPartner?
161 Unk0161 - scrcmd_flags.c:148 GivePartner?_SetPartner?
162 Unk0162 - scrcmd_flags.c:157 RemovePartner?_ClearPartner?
163 Unk0163 p scrcmd_flags.c:166 GetSteps?_CheckSteps?_GetStepFlag?_CheckStepFlag?
164 Unk0164 - scrcmd_flags.c:176 SetStepFlag?
165 Unk0165 - scrcmd_flags.c:185 ClearStepFlag?
166 CheckGameCompleted p scrcmd_flags.c:194
167 SetGameCompleted - scrcmd_flags.c:204
168 PrepareDoorAnimation hhvvb scrcmd.c:3298
169 WaitDoorAnimation b scrcmd.c:3308
16A FreeDoorAnimation b scrcmd.c:3314
16B OpenDoorAnimation b scrcmd.c:3320
16C CloseDoorAnimation b scrcmd.c:3326
16D GetDaycarePokemonNames - scrcmd_daycare.c:22
16E GetDaycareStatus p scrcmd_daycare.c:33
16F InitPastoriaGym - scrcmd.c:3332
170 CheckPastoriaGymButton - scrcmd.c:3337
171 InitHearthomeGym - scrcmd.c:3342
172 MoveHearthomeGymElevator - scrcmd.c:3347
173 InitCanalaveGym - scrcmd.c:3352
174 InitVeilstoneGym - scrcmd.c:3357
175 InitSunyshoreGym b scrcmd.c:3362
176 RotateSunyshoreGymGear b scrcmd.c:3369
177 CountPartyMons p scrcmd_party.c:371
178 ShowBagScreen b scrcmd.c:2385
179 GetBagScreenSelection p scrcmd.c:2396
17A CheckPocketNotEmpty vp scrcmd.c:3222
17B GetBerryName bvv scrcmd_names.c:336 BufferBerryName?
17C GetNatureName bv scrcmd_names.c:350 BufferNatureName?
17D GetBerryTreeGrowth p scrcmd_berry_trees.c:19
17E GetBerryTreeType p scrcmd_berry_trees.c:28
17F GetBerryTreeMulch p scrcmd_berry_trees.c:37
180 GetBerryTreeWater p scrcmd_berry_trees.c:46
181 GetBerryTreeAmount p scrcmd_berry_trees.c:55
182 SetBerryTreeMulch v scrcmd_berry_trees.c:64
183 SetBerryTreeType v scrcmd_berry_trees.c:73 PlantBerryTree?
184 Unk0184 h scrcmd_berry_trees.c:85 SetBerryTreeWater/WaterBerryTree,_or_just_the_animation?
185 TakeBerryTreeBerries - scrcmd_berry_trees.c:104
186 SetEventDefaultPosition vvv scrcmd.c:3230
187 SetEventPosition vvvvv scrcmd.c:3238
188 SetEventDefaultMovement vv scrcmd.c:3250
189 SetEventDefaultDirection vv scrcmd.c:3257
18A SetWarpPosition vvv scrcmd.c:3264
18B SetBgEventPosition vvv scrcmd.c:3272
18C SetEventDirection vv scrcmd.c:3280
18D ShowWaitingIcon - scrcmd.c:1337
18E HideWaitingIcon - scrcmd.c:1344
18F Unk018F v scrcmd.c:3289
190 WaitButtonABTime v scrcmd.c:992
191 ChoosePokemonMenu - scrcmd.c:1902
192 UnionChoosePokemonMenu - scrcmd.c:1916
193 GetSelectedPartySlot p scrcmd.c:1922
194 Unk0194 vvvv scrcmd.c:1937
195 Unk0195 pp scrcmd.c:1949
196 Unk0196 v scrcmd.c:1969
197 Unk0197 p scrcmd.c:1978
198 GetPartyMonSpecies pp scrcmd_party.c:32 GetPartyPokemonSpecies?
199 CheckPartyMonOTID pp scrcmd_party.c:54 CheckPartyPokemonTraded?
19A CountPartyMons_OmitEggs p scrcmd_party.c:381
19B CountAvailablePartyMons_IgnoreSlot pv scrcmd_party.c:404
19C CountAvailablePartyAndPCMons p scrcmd_party.c:435
19D GetPartyEggCount p scrcmd_party.c:464
19E Unk019E vh scrcmd.c:3381
19F Unk019F v scrcmd.c:3401
1A0 Unk01A0 - scrcmd.c:3414
1A1 Unk01A1 bv scrcmd.c:3419
1A2 Unk01A2 bv scrcmd.c:3426
1A3 TakeMoneyAddress v scrcmd_money.c:34 TakeMoneyVar?
1A4 Unk01A4 pv scrcmd_daycare.c:66
1A5 Unk01A5 p scrcmd_daycare.c:172
1A6 Unk01A6 - scrcmd_daycare.c:178
1A7 Unk01A7 - scrcmd_daycare.c:184
1A8 DeleteDaycareEgg - scrcmd_daycare.c:45
1A9 GiveDaycareEgg - scrcmd_daycare.c:53
1AA Unk01AA pv scrcmd_daycare.c:81
1AB HasEnoughMoneyAddress pv scrcmd_money.c:63 CanAffordMoneyVar?
1AC HatchEgg - scrcmd.c:3376
1AD Unk01AD p scrcmd_daycare.c:190
1AE GetDaycareLevel pv scrcmd_daycare.c:95
1AF Unk01AF hvp scrcmd_daycare.c:108
1B0 Unk01B0 v scrcmd_daycare.c:122
1B1 HideEvent v scrcmd.c:3438
1B2 ShowEvent v scrcmd.c:3447
1B3 ShowMailbox - scrcmd.c:3456
1B4 CountMail p scrcmd.c:3461
1B5 Unk01B5 v scrcmd.c:3469
1B6 GetTimeOfDay p scrcmd.c:3475
1B7 Random pv scrcmd.c:3481
1B8 DummyRandom pv scrcmd.c:3488
1B9 GetPartyMonFriendship pv scrcmd_party.c:266
1BA AddPartyMonFriendship vv scrcmd_party.c:278
1BB SubtractPartyMonFriendship vv scrcmd_party.c:317
1BC Unk01BC vvvv scrcmd_daycare.c:135
1BD GetPlayerDirection p scrcmd.c:1813
1BE Unk01BE p scrcmd_daycare.c:150
1BF Unk01BF p scrcmd_daycare.c:161 SaveEggPID?
1C0 CheckPartyForSpecies pv scrcmd_party.c:616
1C1 CheckPokemonSizeRecord pv scrcmd.c:3495
1C2 SetPokemonSizeRecord v scrcmd.c:3503
1C3 BufferPartyPokemonSize vvv scrcmd.c:3510
1C4 BufferPokemonRecordSize vvv scrcmd.c:3519
1C5 InitPokemonRecordSize - scrcmd.c:3528
1C6 Unk01C6 v scrcmd_move_relearner.c:16 MoveInfo?
1C7 Unk01C7 p scrcmd_move_relearner.c:25 StoreMove?
1C8 CountPartyMonMoves pv scrcmd_party.c:522
1C9 ForgetPartyMonMove vv scrcmd_party.c:565
1CA GetPartyMonMove pvv scrcmd_party.c:578
1CB GetPokemonMoveName bvv scrcmd_names.c:387 BufferPartyPokemonMoveName?
1CC Unk01CC - scrcmd.c:3534
1CD Unk01CD vvvvv scrcmd.c:3545
1CE Unk01CE - scrcmd.c:3583
1CF GetSetStrength * scrcmd_flags.c:213 Strength?
1D0 GetSetFlash * scrcmd_flags.c:238 Flash?
1D1 GetSetDefog * scrcmd_flags.c:263 Defog
1D2 Unk01D2 vv scrcmd.c:3587
1D3 Unk01D3 vvp scrcmd.c:3594
1D4 Unk01D4 vvp scrcmd.c:3602
1D5 Unk01D5 v scrcmd.c:3610
1D6 Unk01D6 vp scrcmd.c:3616
1D7 Unk01D7 h scrcmd.c:2265
1D8 Unk01D8 p scrcmd.c:2273
1D9 Unk01D9 vv scrcmd.c:2287
1DA Unk01DA - asm/scrcmd_10.s:30
1DB Unk01DB hh asm/scrcmd_10.s:40
1DC Unk01DC - asm/scrcmd_10.s:63
1DD Unk01DD hvp asm/scrcmd_10.s:79
1DE Unk01DE vvpp asm/scrcmd_10.s:505
1DF Unk01DF p asm/scrcmd_10.s:564
1E0 Unk01E0 p asm/scrcmd_10.s:584
1E1 Unk01E1 vv asm/scrcmd_10.s:604
1E2 Unk01E2 vh asm/scrcmd_10.s:674
1E3 Unk01E3 pp asm/scrcmd_10.s:697
1E4 Unk01E4 p asm/scrcmd_10.s:732
1E5 IncrementGameStat h scrcmd.c:3673
1E6 GetGameStat hpp scrcmd.c:3680
1E7 SetGameStat hhhb scrcmd.c:3692
1E8 CheckSinnohDexComplete p scrcmd.c:3623
1E9 CheckNationalDexComplete p scrcmd.c:3633
1EA RegisterSinnohPokedex - scrcmd.c:3643
1EB RegisterNationalPokedex - scrcmd.c:3650
1EC Unk01EC - scrcmd.c:3657
1ED Unk01ED p scrcmd.c:3662
1EE GetPartyMonHeldItem pv scrcmd_party.c:591
1EF Unk01EF p scrcmd.c:3668
1F0 ResetPartyMonHeldItem v scrcmd_party.c:603
1F1 CountFossils p scrcmd_fossils.c:16
1F2 Unk01F2 - scrcmd_fossils.c:73
1F3 Unk01F3 - scrcmd_fossils.c:79
1F4 GetFossilPokemon pv scrcmd_fossils.c:32
1F5 GetFossilMinimumAmount ppv scrcmd_fossils.c:49
1F6 CountPartyMonsAtOrBelowLevel pv scrcmd_party.c:170
1F7 SurvivePsn pv scrcmd_party.c:159
1F8 TerminateOverworldProcess - scrcmd.c:2056
1F9 DebugWatch v scrcmd.c:449
1FA MessageFrom vv scrcmd.c:813 MessageAllFromNarc?
1FB MessageFrom2 vv scrcmd.c:823 MessageFromNarc?
1FC Unk01FC hhhh scrcmd.c:834
1FD Unk01FD hhhh scrcmd.c:845
1FE Unk01FE b scrcmd.c:857
1FF Unk01FF bvhb scrcmd.c:877
200 GetPreviousMapID p scrcmd.c:2510
201 GetCurrentMapID p scrcmd.c:2517
202 EnableDisableSafariZone b scrcmd.c:3713
203 BattleRoomWarp hhvvh scrcmd.c:2495
204 ExitBattleRoom - scrcmd.c:2505
205 ShowGeonetScreen - scrcmd.c:2245
206 UseGreatMarshBinoculars - scrcmd.c:3738
207 Unk0207 p scrcmd.c:2106
208 ShowPokemonPic vv scrcmd.c:2112
209 Unk0209 - scrcmd.c:2133
20A Unk020A p scrcmd.c:2157
20B Unk020B - scrcmd.c:2165
20C Unk020C - scrcmd.c:951
20D SpearPillarSequence bp scrcmd.c:3743
20E KeepSafariTrain - scrcmd.c:3750
20F MoveSafariTrain ph scrcmd.c:3755
210 CheckSafariTrainPosition hp scrcmd.c:3763
211 IgnoreHeights b scrcmd.c:3770
212 GetPartyMonNature pv scrcmd_party.c:216
213 FindPartyMonWithNature pv scrcmd_party.c:241 CheckNatureInParty
214 GetSpiritombTalkCounter p scrcmd.c:3776
215 ClearAmitySquareSteps - scrcmd_amity_square.c:98
216 CheckAmitySquareSteps p scrcmd_amity_square.c:107
217 GetAmitySquareAccessory pv scrcmd_amity_square.c:116
218 Unk0218 p scrcmd.c:3783
219 Unk0219 v scrcmd.c:3804
21A Unk021A p scrcmd.c:3811
21B Unk021B - scrcmd.c:3817
21C CreateRoamer b scrcmd.c:3822
21D UnionGroup * scrcmd_union.c:14
21E Unk021E - scrcmd_move_relearner.c:40
21F Unk021F pv scrcmd_move_relearner.c:46
220 Unk0220 - scrcmd_move_relearner.c:78
221 Unk0221 v scrcmd_move_relearner.c:84 RememberMove?
222 Unk0222 - scrcmd_move_relearner.c:110 DummyMoveCmd?
223 Unk0223 p scrcmd_move_relearner.c:116 RememberMoveResponse?_-_destroys_the_MoveRelearner_-_find_better_name
224 Unk0224 vv scrcmd_move_relearner.c:95 TeachMove?
225 Unk0225 p scrcmd_move_relearner.c:134 TeachMoveResponse?_-_destroys_the_MoveRelearner_-_find_better_name
226 NPCTradeInit b scrcmd.c:3828
227 NPCTradeGetOfferedSpecies p scrcmd.c:3835
228 NPCTradeGetRequestedSpecies p scrcmd.c:3842
229 NPCTradeExecute v scrcmd.c:3849
22A NPCTradeEnd - scrcmd.c:3856
22B UnlockForeignEntries - scrcmd.c:3862
22C UnlockGenderEntries - scrcmd.c:3867
22D NationalDex bp scrcmd.c:3872
22E CountPartyMonRibbons pv scrcmd_party.c:641
22F CountTotalPartyRibbons p scrcmd_party.c:661
230 PartyMonHasRibbon pvv scrcmd_party.c:694
231 GivePartyMonRibbon vv scrcmd_party.c:706
232 GetRibbonName bv scrcmd_names.c:403 BufferRibbonName?
233 GetTotalPokemonEVs pv scrcmd.c:3887
234 GetDayOfWeek p scrcmd.c:3903
235 Unk0235 * asm/scrcmd_9.s:36
236 Unk0236 v asm/scrcmd_9.s:220
237 Unk0237 * asm/scrcmd_9.s:264
238 Unk0238 vp asm/scrcmd_9.s:359
239 Unk0239 p scrcmd.c:3911
23A GetPokemonFootprint ppv scrcmd.c:3917
23B PokecenterHealAnimation v scrcmd.c:3928
23C ElevatorAnimation vv scrcmd.c:3934
23D ShipAnimation bbhhh scrcmd.c:3941
23E Unk023E * asm/scrcmd_12.s:50
23F Unk023F - scrcmd_25.c:5
240 Unk0240 - scrcmd_25.c:12
241 Unk0241 - scrcmd_25.c:19
242 Unk0242 - scrcmd_25.c:25
243 Unk0243 vpp scrcmd.c:2437
244 Unk0244 vppp scrcmd.c:2446
245 Unk0245 vv scrcmd.c:2457
246 GetGameVersion p scrcmd.c:3952
247 GetLeadingPartyMonSlot p scrcmd_party.c:349
248 GetPartyMonTypes ppv scrcmd_party.c:357
249 GiveWallpaper pvvvv scrcmd.c:3958
24A Unk024A p scrcmd.c:3983
24B Unk024B b scrcmd.c:3991 PreparePCAnimation?
24C Unk024C b scrcmd.c:3998 OpenPCAnimation?
24D Unk024D b scrcmd.c:4005 ClosePCAnimation?
24E Unk024E p asm/scrcmd_5.s:12
24F Unk024F pppv asm/scrcmd_5.s:35
250 Unk0250 - asm/scrcmd_5.s:216
251 Unk0251 bv scrcmd_names.c:169 BufferBoxPokemonNickname?_TextBoxPokemonNickname?
252 CountPCFreeSpace p scrcmd.c:4021
253 Unk0253 v scrcmd_13.c:24
254 Unk0254 p scrcmd_13.c:42
255 Unk0255 - scrcmd_13.c:60
256 Unk0256 vp scrcmd_13.c:84
257 AccessoriesShop - scrcmd_mart.c:439 Unsure_if_this_is_correct,_SDSME_has_it_as_SprtSave?
258 Unk0258 - scrcmd.c:4028
259 Unk0259 - scrcmd.c:4035
25A Unk025A v scrcmd.c:4041
25B Unk025B - scrcmd.c:4047
25C Unk025C - scrcmd.c:4052
25D Unk025D p scrcmd.c:4057
25E Unk025E - scrcmd.c:4067
25F Unk025F - scrcmd.c:4072
260 AddSpecialGameStat h scrcmd.c:4077
261 GetFashionName bv scrcmd_names.c:361 BufferAccessoryName?
262 CheckPokemonInParty vp scrcmd.c:4083
263 SetDeoxysForm v scrcmd.c:4090
264 CheckBurmyForms p scrcmd.c:4108
265 Unk0265 - scrcmd.c:4233
266 Unk0266 - scrcmd.c:4238
267 SlotMachine v scrcmd.c:4243
268 GetHour p scrcmd.c:4249
269 ShakeEvent vvvvv scrcmd.c:4255
26A BlinkEvent vvv scrcmd.c:4267
26B CheckRegis p scrcmd.c:4277
26C Unk026C p scrcmd.c:4283
26D MessageUnown h scrcmd.c:894
26E CheckGBACartridge p scrcmd.c:4314
26F ResetSpiritombTalkCounter - scrcmd.c:4320
270 Unk0270 vb scrcmd.c:4325 SetMatrixAlternativeMap
271 WriteWhiteRockInscription p scrcmd.c:2431
272 GetWhiteRockInscription b scrcmd_names.c:372 BufferWhiteRockInscription?
273 BufferContestBackgroundName bv scrcmd.c:4338
274 HasEnoughCoinsImmediate pw scrcmd_coins.c:81 CanAffordCoins?
275 Unk0275 p scrcmd.c:4346
276 CanGiveCoins pv scrcmd_coins.c:123
277 Unk0277 p scrcmd.c:4357
278 GetPartyMonLevel pv scrcmd_party.c:198
279 Unk0279 vv scrcmd.c:4363
27A UseSunyshoreBinoculars - scrcmd.c:4370
27B Unk027B - scrcmd.c:4375
27C Unk027C * asm/scrcmd_9.s:387
27D BufferRandomTrendySaying pv scrcmd.c:4380
27E Unk027E p scrcmd.c:4400
27F Unk027F p scrcmd.c:4394
280 Unk0280 bvbb scrcmd_names.c:139 BufferNumberSpecial?_need_more_info
281 GetPartyMonContestCondition vvp scrcmd_party.c:337
282 CheckBirthday p scrcmd.c:4407
283 SetVolume vv scrcmd_sound.c:238
284 CountSeenUnown p scrcmd.c:4419
285 Unk0285 vv scrcmd.c:4426
286 Unk0286 p scrcmd.c:4471
287 Unk0287 p scrcmd.c:4478
288 Unk0288 p scrcmd.c:4485
289 Unk0289 pbbbbbb scrcmd.c:4492
28A Unk028A p scrcmd.c:4514
28B CheckEventValidity bp scrcmd.c:4524
28C ShowPartyPokemonPic v scrcmd.c:2122
28D Unk028D - scrcmd.c:2139
28E Unk028E h scrcmd.c:2145
28F Unk028F p scrcmd.c:4533
290 Unk0290 v scrcmd.c:4554
291 Unk0291 pp scrcmd.c:4562
292 Unk0292 bp scrcmd.c:4578
293 Unk0293 p scrcmd.c:4610
294 Unk0294 bb asm/scrcmd_10.s:795
295 Unk0295 - asm/scrcmd_10.s:820
296 Unk0296 - asm/scrcmd_10.s:833
297 Unk0297 p asm/scrcmd_10.s:850
298 Unk0298 v asm/scrcmd_10.s:873
299 Unk0299 v asm/scrcmd_10.s:896
29A Unk029A vp asm/scrcmd_10.s:919
29B Unk029B vvpp asm/scrcmd_10.s:958
29C Unk029C vp scrcmd.c:3148
29D Unk029D vv scrcmd.c:1390
29E Unk029E vp scrcmd.c:4589
29F ShakeCamera v scrcmd.c:4617
2A0 Unk02A0 vvv scrcmd_7.c:130
2A1 Unk02A1 vvv scrcmd.c:1524
2A2 Unk02A2 v scrcmd.c:4639
2A3 Unk02A3 p scrcmd.c:4627
2A4 Unk02A4 p scrcmd.c:4633
2A5 OpenTradeScreen - scrcmd.c:1909
2A6 GetPrizeItemIdAndCost vpp scrcmd_prizes.c:29
2A7 Unk02A7 vp scrcmd.c:4649
2A8 TakeCoinsAddress p scrcmd_coins.c:71 TakeCoinsVar_instead?
2A9 HasEnoughCoinsAddress pp scrcmd_coins.c:102 CanAffordCoinsVar?
2AA CompareMysteryGiftEasyChatInput pvvvv scrcmd.c:4659
2AB Unk02AB p scrcmd.c:1868
2AC ActivateMysteryGift - scrcmd.c:4681
2AD GetEventMovement pv scrcmd.c:1850
2AE Unk02AE hp scrcmd_sound.c:18
2AF Unk02AF p scrcmd.c:4686
2B0 Unk02B0 - scrcmd.c:4694
2B1 Unk02B1 - scrcmd.c:4699
2B2 Unk02B2 - scrcmd.c:4704
2B3 GetSealName bv scrcmd_names.c:415 BufferSealName?
2B4 LockAllEvents2 - scrcmd.c:1678
2B5 Unk02B5 vvv scrcmd.c:4709
2B6 Unk02B6 vb scrcmd.c:4722
2B7 CheckPartyForBadEgg p scrcmd_party.c:719
2B8 Unk02B8 v asm/scrcmd_9.s:238
2B9 Unk02B9 - scrcmd.c:1415
2BA Unk02BA p scrcmd.c:3019
2BB Unk02BB - scrcmd.c:3205
2BC Unk02BC p scrcmd_7.c:246
2BD LegendaryBattle vv scrcmd.c:2828
2BE GetTrainerCardLevel p scrcmd.c:4731
2BF DummyRideBike - scrcmd.c:2617
2C0 Unk02C0 v scrcmd.c:929
2C1 ShowSaveStats - scrcmd.c:4738
2C2 HideSaveStats - scrcmd.c:4748
2C3 ScopeMode b scrcmd.c:4758
2C4 GetItemNameWithIndefArticle bv scrcmd_names.c:426 BufferIndefItemName?
2C5 GetItemNamePlural bv scrcmd_names.c:437 BufferPluralItemName?
2C6 GetDecorationNameWithArticle bv scrcmd_names.c:448 BufferIndefDecorationName?
2C7 GetUndergroundTrapNameWithArticle bv scrcmd_names.c:459 BufferIndefTrapName?
2C8 GetUndergroundItemNameWithArticle bv scrcmd_names.c:470 BufferIndefUndergroundItemName?_BufferIndefTreasureName?
2C9 Unk02C9 bvhb scrcmd_names.c:481 BufferIndefSpeciesName?
2CA Unk02CA b scrcmd_names.c:494 BufferIndefFriendStarterSpecies?
2CB GetFashionNameWithArticle bv scrcmd_names.c:506 BufferIndefAccessoryName?
2CC Unk02CC bv scrcmd_names.c:517 BufferIndefTrainerClassName?
2CD GetSealNamePlural bv scrcmd_names.c:528 BufferPluralSealName?
2CE CapitalizeFirstChar b scrcmd_names.c:539 CapitalizeText?
2CF Unk02CF b scrcmd.c:1497
2D0 Unk02D0 b scrcmd.c:1503
"""

# Commands whose argument list depends on a leading selector (derived from the bodies/asm).
VAR_ARGS = {
    0x1CF: ("b", {2: "p"}),  # GetSetStrength scrcmd_flags.c:213 (0 clear, 1 set, 2 get->var)
    0x1D0: ("b", {2: "p"}),  # GetSetFlash scrcmd_flags.c:238
    0x1D1: ("b", {2: "p"}),  # GetSetDefog scrcmd_flags.c:263
    0x21D: ("h", {0: "vp", 1: "vp", 2: "vv", 3: "vv", 4: "p", 5: "v", 6: ""}),  # UnionGroup scrcmd_union.c:14
    0x235: ("h", {0: "p", 1: "hpp", 2: "", 3: "vpp", 4: "pp", 5: "vvp", 6: "p"}),  # asm/scrcmd_9.s:36 jump table
    0x237: ("h", {0: "vpp", 1: "vvv"}),  # asm/scrcmd_9.s:274
    0x23E: ("h", {0: "", 1: "p", 2: "p", 3: "p", 4: "", 5: "pp", 6: "pp", 7: "", 8: ""}),  # asm/scrcmd_12.s:50
    0x27C: ("h", {0: "v", 1: "v", 2: "v"}),  # asm/scrcmd_9.s:397
}

# Readable names for a few decomp-unnamed commands, each read off the D/P body.
ALIASES = {
    0x005E: "ApplyMovement",  # scrcmd.c:1509 obj + offset to movement list (sub_0205AEA4)
    0x00E5: "TrainerBattle",  # scrcmd_7.c:113 SetupAndStartTrainerBattle(trainer1, trainer2, follower)
    0x02A0: "TrainerBattleWithPartner",  # scrcmd_7.c:130 (partner, trainer1, trainer2)
    0x00EB: "Blackout",  # scrcmd_7.c:220 CallTask_Blackout
    0x00EC: "GetBattleWon",  # scrcmd_7.c:226 IsBattleResultWin -> var
    0x00F0: "SetLastInteractedTrainerFlag",  # scrcmd_7.c:274
    0x00F1: "GoToIfLastInteractedTrainerFlagSet",  # scrcmd_7.c:283
}

# Opcodes after which control never falls through.
TERMINATORS = {0x002, 0x01B, 0x016}  # End, Return, GoTo
CALLS = {0x01A, 0x01D}

# sScriptConditionTable (arm9/asm/unk_02038C78.s:133) against Compare() (scrcmd.c:506): 0 a<b, 1 ==, 2 >
CONDITIONS = {0: "LT", 1: "EQ", 2: "GT", 3: "LE", 4: "GE", 5: "NE"}

# LoadScriptsAndMessagesByMapId (arm9/asm/unk_02038C78.s:1206-1586): script id -> (first id, scr_seq file, msg bank)
SCRIPT_ID_RANGES = [
    (10300, 977, 496), (10200, 373, 332), (10150, 1042, 562), (10100, 1041, 563), (10000, 375, 334),
    (9950, 376, 335), (9900, 365, 199), (9800, 206, 203), (9700, 387, 378), (9600, 377, 199),
    (9500, 464, 492), (9400, 391, 381), (9300, 372, 329), (9200, 388, 379), (9100, 0, 9),
    (9000, 207, 207), (8970, 390, 7), (8950, 463, 486), (8900, 389, 380), (8800, 462, 485),
    (8000, 374, 333), (7000, 370, 325), (5000, 1040, 199), (3000, 1040, 199), (2800, 378, 350),
    (2500, 1, 13), (2000, 205, 199), (1, None, None), (0, 369, 317),
]

TRAINER_FLAG_BASE = 0x550  # TrainerFieldSystem_FlagCheck: flag 0x550 + trainer id (unk_02038C78.s:2155)
HIDDEN_ITEM_FLAG_SUB = 0x1C66  # sub_02039694: hidden-item flag = bg script id - 7270 (unk_02038C78.s:2200)
NUM_FLAGS = 2912  # include/constants/flags.h
TEMP_FLAG_BASE = 0x4000

SPECIAL_VARS = {0x800C: "VAR_RESULT", 0x800D: "VAR_LAST_INTERACTED"}  # include/script.h:148-161

LEVEL_SCRIPT_TYPES = {1: "ON_FRAME_TABLE", 2: "ON_TRANSITION", 3: "ON_RESUME", 4: "ON_LOAD"}

MSG_ARG = {  # opcode -> index of the message-id argument (in the current script bank)
    0x02B: 0, 0x02C: 0, 0x02D: 0, 0x02E: 0, 0x02F: 0, 0x036: 0, 0x03A: 0, 0x26D: 0, 0x2C0: 0,
}


class DisasmError(Exception):
    pass


class Op:
    __slots__ = ("args", "hint", "id", "name", "src")

    def __init__(self, id_, name, args, src, hint):
        self.id, self.name, self.args, self.src, self.hint = id_, name, args, src, hint

    @property
    def display(self):
        if self.id in ALIASES:
            return f"{self.name}/{ALIASES[self.id]}"
        return self.name


def _load_ops():
    ops = {}
    for line in OPTABLE.strip().splitlines():
        parts = line.split(" ", 4)
        id_ = int(parts[0], 16)
        args = "" if parts[2] == "-" else parts[2]
        ops[id_] = Op(id_, parts[1], args, parts[3], parts[4].replace("_", " ") if len(parts) > 4 else "")
    return ops


OPS = _load_ops()


# ----------------------------------------------------------------------------------------------
# Tree access
# ----------------------------------------------------------------------------------------------
def root():
    return ROOT


@cache
def read_bytes(rel):
    return (root() / rel).read_bytes()


@cache
def constants(header):
    """name -> (value, line); value -> [names in file order] for include/constants/<header>.h"""
    by_name, by_val = {}, {}
    path = root() / "include" / "constants" / f"{header}.h"
    for ln, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", line)
        if m:
            v = int(m.group(2), 0)
            by_name[m.group(1)] = (v, ln)
            by_val.setdefault(v, []).append(m.group(1))
    return by_name, by_val


def const_name(header, value, prefix=""):
    return next((n for n in constants(header)[1].get(value, []) if n.startswith(prefix)), None)


def map_name(i):
    return const_name("maps", i, "MAP_") or f"map#{i}"


def trainer_name(i):
    return const_name("trainers", i, "TRAINER_") or f"trainer#{i}"


def item_name(i):
    return const_name("items", i, "ITEM_") or f"item#{i}"


def species_name(i):
    return const_name("species", i, "SPECIES_") or f"species#{i}"


def sprite_name(i):
    if 101 <= i <= 116:  # map_object.c:771 FieldSystem_ResolveObjectSpriteID -> var 0x4020+n
        return f"GFX_FROM_VAR_0x{0x4020 + i - 101:04X}"
    return const_name("sprites", i, "SPR") or f"gfx#{i}"  # sprites.h:134 spells SPRTIE_SUNGLASSES


def badge_name(i):
    return const_name("badge", i, "BADGE_") or f"badge#{i}"


def dir_name(i):
    return const_name("global_fieldmap", i, "DIR_") or str(i)


@cache
def map_table():
    """List of dicts, index = map id, from arm9/src/map_header.c sMapHeaders."""
    path = root() / "arm9/src/map_header.c"
    lines = path.read_text().splitlines()
    rows, inside = [], False
    fields = ["area_data", "move_model", "matrix", "scripts", "level_scripts", "msg_bank", "day_music",
              "night_music", "wild_encounters", "events", "mapsec", "weather", "camera", "map_type",
              "battle_bg", "bike", "running", "escape_rope", "fly"]
    for ln, line in enumerate(lines, 1):
        if "sMapHeaders[] = {" in line:
            inside = True
            continue
        if inside and line.startswith("};"):
            break
        if not inside:
            continue
        m = re.match(r"\s*\{(.*)\},\s*//\s*(\w+)", line)
        if not m:
            continue
        parts, depth, cur = [], 0, ""
        for ch in m.group(1):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            if ch == "," and depth == 0:
                parts.append(cur.strip())
                cur = ""
            else:
                cur += ch
        parts.append(cur.strip())
        row = {"line": ln, "comment": m.group(2)}
        for k, v in zip(fields, parts):
            row[k] = v
        for k in ("area_data", "move_model", "matrix", "scripts", "level_scripts", "msg_bank", "events"):
            nm = re.search(r"narc_(\d+)_bin", row[k])
            row[k] = int(nm.group(1)) if nm else int(row[k], 0)
        enc = row["wild_encounters"]
        em = re.match(r"ENCDATA\(\w*narc_(\d+)_bin,\s*\w*narc_(\d+)_bin\)", enc)
        row["wild_encounters"] = (int(em.group(1)), int(em.group(2))) if em else None
        rows.append(row)
    return rows


def resolve_map(arg):
    rows = map_table()
    if re.fullmatch(r"\d+", arg):
        i = int(arg)
    else:
        by_name, _ = constants("maps")
        if arg not in by_name:
            raise SystemExit(f"unknown map {arg!r} (names: include/constants/maps.h)")
        i = by_name[arg][0]
    if not 0 <= i < len(rows):
        raise SystemExit(f"map id {i} out of range (0..{len(rows) - 1})")
    return i


def maps_line(i):
    by_name, _ = constants("maps")
    name = map_name(i)
    return by_name[name][1] if name in by_name else None


@cache
def level_script_files():
    return {r["level_scripts"] for r in map_table()}


@cache
def file_users():
    """scr_seq file -> [(map id, role)], zone_event file -> [map id]"""
    scr, evt = {}, {}
    for i, r in enumerate(map_table()):
        scr.setdefault(r["scripts"], []).append((i, "scripts"))
        scr.setdefault(r["level_scripts"], []).append((i, "level_scripts"))
        evt.setdefault(r["events"], []).append(i)
    return scr, evt


def script_id_target(sid, mapid=None):
    """Resolve a script id like the engine: -> (scr_seq file, header index, msg bank, label)."""
    if sid == 0xFFFF:
        return None, None, None, "none (0xFFFF)"
    for base, fileno, bank in SCRIPT_ID_RANGES:
        if sid >= base:
            idx = sid - base
            if fileno is None:  # map-local scripts 1..1999
                if mapid is None:
                    return None, idx, None, f"map-local #{sid}"
                r = map_table()[mapid]
                return r["scripts"], idx, r["msg_bank"], f"{map_name(mapid)} #{sid}"
            label = f"std {base}+{idx}"
            if base in (3000, 5000):
                label = f"trainer {trainer_name(sid - 2999 if base == 3000 else sid - 4999)}"
                if base == 5000:
                    label += " (2nd of double)"
            elif base == 8000:
                label = f"hidden item {idx}"
            return fileno, idx, bank, label
    return None, None, None, "?"


@cache
def default_msg_bank(fileno):
    scr, _ = file_users()
    for mapid, role in scr.get(fileno, []):
        if role == "scripts":
            return map_table()[mapid]["msg_bank"], f"msg bank of {map_name(mapid)}"
    for base, f, bank in SCRIPT_ID_RANGES:
        if f == fileno and bank is not None:
            return bank, f"script-id range {base}+"
    return None, "no map/range uses this file"


# ----------------------------------------------------------------------------------------------
# Text (files/msgdata/msg/narc_NNNN.gmm, the msgenc source of the msg NARC)
# ----------------------------------------------------------------------------------------------
@cache
def msg_bank(bank):
    path = root() / MSG_DIR / f"narc_{bank:04d}.gmm"
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="replace").replace("&", "&amp;").replace("&amp;amp;", "&amp;")
    try:
        tree = ET.fromstring(text)
    except ET.ParseError:
        tree = ET.fromstring(path.read_text(encoding="utf-8", errors="replace"))
    out = {}
    for row in tree.iter("row"):
        idx = int(row.get("index"))
        lang = row.find("language")
        out[idx] = (lang.text or "") if lang is not None else ""
    return out


def msg_text(bank, idx, width=None):
    msgs = msg_bank(bank) if bank is not None else None
    if not msgs or idx not in msgs:
        return None
    t = msgs[idx].replace("\n", "\\n").replace("\r", "\\r").replace("\f", "\\f")
    if width and len(t) > width:
        t = t[: width - 3] + "..."
    return t


# ----------------------------------------------------------------------------------------------
# Trainers (files/poketool/trainer/trdata.json)
# ----------------------------------------------------------------------------------------------
@cache
def trainers():
    return json.loads((root() / TRDATA).read_text())["trdata"]


def trainer_summary(tid):
    t = trainers()[tid]
    party = ", ".join(f"{m['species'].replace('SPECIES_', '')} {m['level']}" for m in t["party"])
    return f"{t['class'].replace('TRAINER_CLASS_', '')} {t['name'].replace('{TRNAME}', '')}: {party}"


def resolve_trainer(arg):
    if re.fullmatch(r"\d+", arg):
        return int(arg)
    by_name, _ = constants("trainers")
    if arg in by_name:
        return by_name[arg][0]
    hits = [n for n in by_name if arg.upper() in n]
    if len(hits) == 1:
        return by_name[hits[0]][0]
    raise SystemExit(f"unknown/ambiguous trainer {arg!r}: {hits[:20]}")


# ----------------------------------------------------------------------------------------------
# Hidden items: UNK_020F2DB4 (arm9/asm/unk_02038C78.s:7), 8-byte rows {u16 item, u8 qty, u8, u16, u16 index}
# ----------------------------------------------------------------------------------------------
@cache
def hidden_items():
    text = (root() / "arm9/asm/unk_02038C78.s").read_text()
    m = re.search(r"UNK_020F2DB4:[^\n]*\n((?:\s*\.byte[^\n]*\n)+)", text)
    if not m:
        return {}
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    out = {}
    for off in range(0, len(data) - 7, 8):
        item, qty, unk, _pad, idx = struct.unpack_from("<HBBHH", data, off)
        out[idx] = (item, qty, unk)
    return out


# ----------------------------------------------------------------------------------------------
# Events
# ----------------------------------------------------------------------------------------------
def parse_events(fileno):
    """zone_event layout (unk_02034A28.s): u32 n + bg[n] 0x14, u32 n + obj[n] 0x20 (ObjectEvent,
    include/map_object.h:107), u32 n + warp[n] 0xC, u32 n + coord[n] 0x10."""
    d = read_bytes(f"{EVT_DIR}/narc_{fileno:04d}.bin")
    p = 0
    ev = {"bg": [], "object": [], "warp": [], "coord": []}

    def count():
        nonlocal p
        n = struct.unpack_from("<I", d, p)[0]
        p += 4
        return n

    for _ in range(count()):
        script, typ, x, z, y, facing = struct.unpack_from("<HHiiiH", d, p)
        ev["bg"].append({"off": p, "script": script, "type": typ, "x": x, "z": z, "y": y, "facing": facing})
        p += 0x14
    for _ in range(count()):
        (oid, gfx, mvt, ttype, flag, script, facing, p0, p1, p2, xr, zr, x, z, y) = struct.unpack_from(
            "<HHHHHHhHHHhhHHi", d, p)
        ev["object"].append({"off": p, "id": oid, "gfx": gfx, "movement": mvt, "trainer_type": ttype, "flag": flag,
                                 "script": script, "facing": facing, "param": (p0, p1, p2), "xrange": xr, "zrange": zr,
                                 "x": x, "z": z, "y": y})
        p += 0x20
    for _ in range(count()):
        x, z, dest, warp, unk = struct.unpack_from("<HHHHI", d, p)
        ev["warp"].append({"off": p, "x": x, "z": z, "dest": dest, "warp": warp, "unk": unk})
        p += 0xC
    for _ in range(count()):
        script, x, z, w, length, y, value, var = struct.unpack_from("<HHHHHHHH", d, p)
        ev["coord"].append({"off": p, "script": script, "x": x, "z": z, "w": w, "l": length, "y": y,
                            "value": value, "var": var})
        p += 0x10
    return ev


def describe_script_ref(sid, mapid):
    fileno, idx, _bank, label = script_id_target(sid, mapid)
    if fileno is None:
        return label
    return f"{label} -> scr_seq {fileno:04d} entry {idx}"


# ----------------------------------------------------------------------------------------------
# Level scripts (sub_0203989C/sub_02039914, unk_02038C78.s:2470-2614)
# ----------------------------------------------------------------------------------------------
def parse_level_scripts(fileno):
    d = read_bytes(f"{SCR_DIR}/narc_{fileno:04d}.bin")
    out, p = [], 0
    while p < len(d) and d[p] != 0:
        typ = d[p]
        if typ == 1:
            rel = struct.unpack_from("<I", d, p + 1)[0]
            q = p + 5 + rel
            table = []
            while q + 2 <= len(d):
                var = struct.unpack_from("<H", d, q)[0]
                if var == 0:
                    break
                value, sid = struct.unpack_from("<HH", d, q + 2)
                table.append({"off": q, "var": var, "value": value, "script": sid})
                q += 6
            out.append({"off": p, "type": typ, "table": table})
        else:
            out.append({"off": p, "type": typ, "script": struct.unpack_from("<H", d, p + 1)[0]})
        p += 5
    return out


# ----------------------------------------------------------------------------------------------
# Disassembler
# ----------------------------------------------------------------------------------------------
class Inst:
    __slots__ = ("args", "off", "op", "size")

    def __init__(self, off, op, args, size):
        self.off, self.op, self.args, self.size = off, op, args, size


def script_header(d, fileno):
    """Table of s32 offsets (relative to the end of each entry).  ScriptRunByIndex
    (unk_02038C78.s:1911) indexes it directly, so the 0xFD13 marker is optional: the table also
    ends where the first script starts."""
    entries, p = [], 0
    while True:
        if entries and p >= min(entries):
            return entries, p
        if p + 2 > len(d):
            raise DisasmError(f"scr_seq {fileno:04d}: header runs off the file (no 0xFD13, {len(d)} bytes)")
        if struct.unpack_from("<H", d, p)[0] == 0xFD13:
            return entries, p + 2
        if p + 4 > len(d):
            raise DisasmError(f"scr_seq {fileno:04d}: header entry {len(entries)} @0x{p:04X} truncated")
        rel = struct.unpack_from("<i", d, p)[0]
        tgt = p + 4 + rel
        if not 0 <= tgt < len(d):
            raise DisasmError(f"scr_seq {fileno:04d}: header entry {len(entries)} @0x{p:04X} points outside file")
        entries.append(tgt)
        p += 4


def decode_at(d, off, fileno):
    def need(n, what):
        if off_ + n > len(d):
            raise DisasmError(f"scr_seq {fileno:04d} @0x{off:04X}: {what} truncated at end of file")

    off_ = off
    need(2, "opcode")
    opid = struct.unpack_from("<H", d, off_)[0]
    off_ += 2
    op = OPS.get(opid)
    if op is None:
        raise DisasmError(f"scr_seq {fileno:04d} @0x{off:04X}: unknown opcode 0x{opid:04X} (table has 0x000-0x2D0)")
    kinds = op.args
    args = []

    def read(kind):
        nonlocal off_
        size = {"b": 1, "h": 2, "v": 2, "p": 2, "w": 4, "o": 4, "m": 4}[kind]
        need(size, f"{op.name} argument")
        if kind == "b":
            val = d[off_]
        elif kind in "hvp":
            val = struct.unpack_from("<H", d, off_)[0]
        elif kind == "w":
            val = struct.unpack_from("<I", d, off_)[0]
        else:
            val = off_ + 4 + struct.unpack_from("<i", d, off_)[0]  # absolute target
        off_ += size
        args.append((kind, val))

    if kinds == "*":
        sel_kind, cases = VAR_ARGS[opid]
        read(sel_kind)
        sel = args[0][1]
        for k in cases.get(sel, ""):
            read(k)
    else:
        for k in kinds:
            read(k)
    return Inst(off, opid, args, off_ - off)


def parse_movement(d, off, fileno):
    steps, p = [], off
    while True:
        if p + 4 > len(d):
            raise DisasmError(f"scr_seq {fileno:04d}: movement list @0x{off:04X} runs off the file")
        cmd, n = struct.unpack_from("<HH", d, p)
        steps.append((p, cmd, n))
        p += 4
        if cmd == 0xFE:
            return steps


class Script:
    def __init__(self, fileno):
        self.fileno = fileno
        self.d = read_bytes(f"{SCR_DIR}/narc_{fileno:04d}.bin")
        self.entries, self.header_end = script_header(self.d, fileno)
        self.insts, self.movements, self.labels = {}, {}, {}
        self._walk()

    def _walk(self):
        d, todo = self.d, []
        for i, tgt in enumerate(self.entries):
            self.labels.setdefault(tgt, f"script_{i + 1}")
            todo.append(tgt)
        while todo:
            off = todo.pop()
            while off not in self.insts:
                if off >= len(d):
                    raise DisasmError(f"scr_seq {self.fileno:04d}: fell off the end of the file at 0x{off:04X}")
                ins = decode_at(d, off, self.fileno)
                for k, v in ins.args:
                    if k == "o":
                        if not 0 <= v < len(d):
                            raise DisasmError(
                                f"scr_seq {self.fileno:04d} @0x{off:04X}: jump target 0x{v:X} outside file")
                        self.labels.setdefault(v, f"L_{v:04X}")
                        todo.append(v)
                    elif k == "m":
                        if not 0 <= v < len(d):
                            raise DisasmError(
                                f"scr_seq {self.fileno:04d} @0x{off:04X}: movement target 0x{v:X} outside file")
                        self.labels.setdefault(v, f"Move_{v:04X}")
                        self.movements[v] = parse_movement(d, v, self.fileno)
                self.insts[off] = ins
                if ins.op in TERMINATORS:
                    break
                off += ins.size
        self._check_overlaps()

    def _check_overlaps(self):
        """Every byte belongs to at most one instruction / movement list and none to the header:
        a wrong argument size anywhere would show up here or as an unknown opcode."""
        owner = {}
        spans = [(o, i.size, f"instruction @0x{o:04X}") for o, i in self.insts.items()]
        spans += [(o, 4 * len(s), f"movement list @0x{o:04X}") for o, s in self.movements.items()]
        for start, size, what in spans:
            if start < self.header_end:
                raise DisasmError(f"scr_seq {self.fileno:04d}: {what} overlaps the header")
            for b in range(start, start + size):
                if b in owner and owner[b] != what:
                    raise DisasmError(f"scr_seq {self.fileno:04d}: {what} overlaps {owner[b]} at 0x{b:04X}")
                owner[b] = what
        self.covered = len(owner)

    def entry_index(self, off):
        return [i + 1 for i, t in enumerate(self.entries) if t == off]


@cache
def get_script(fileno):
    return Script(fileno)


# ----------------------------------------------------------------------------------------------
# Rendering helpers
# ----------------------------------------------------------------------------------------------
def fmt_var(v):
    if v in SPECIAL_VARS:
        return f"{SPECIAL_VARS[v]}(0x{v:04X})"
    return f"0x{v:04X}"


def fmt_arg(kind, val, labels):
    if kind == "p":
        return fmt_var(val)
    if kind == "v":
        return fmt_var(val) if val >= 0x4000 else str(val)
    if kind in "om":
        return labels.get(val, f"0x{val:04X}")
    if kind == "w":
        return f"0x{val:X}"
    return str(val)


def flag_class(f):
    if f >= TEMP_FLAG_BASE:
        return "RAM temp flag, not saved (save_vars_flags.c:51)"
    if 1 <= f < 64:
        return "map-temp flag, cleared on map load (ResetTempFlagsAndVars)"
    if TRAINER_FLAG_BASE <= f < TRAINER_FLAG_BASE + len(trainers()):
        return f"trainer flag of {trainer_name(f - TRAINER_FLAG_BASE)}"
    if 0x960 <= f < 0x9B0:
        return "system flag (unk_0205EC84.c)"
    if f >= NUM_FLAGS:
        return "beyond NUM_FLAGS"
    return ""


def lit(kind, val, known=None):
    """Literal value of an arg, or the value a var provably holds (SetVar earlier in the same
    straight-line block), else None."""
    if kind in "bhw":
        return val
    if kind == "v" and val < 0x4000:
        return val
    if kind == "v" and known and val in known:
        return known[val]
    return None


def annotate(ins, bank, mapid, known=None, prev=None):
    op, a = ins.op, ins.args
    notes = []
    L = [lit(k, v, known) for k, v in a]
    if op in (0x01C, 0x01D):
        cond = CONDITIONS.get(a[0][1], a[0][1])
        if prev is not None and prev.op in (0x020, 0x025) and cond in ("EQ", "LT", "NE"):
            cond += " (flag set)" if cond == "EQ" else " (flag clear)"
        notes.append(f"if {cond}")
    if op in (0x01E, 0x01F, 0x020):
        c = flag_class(a[0][1])
        notes.append(f"flag 0x{a[0][1]:X}" + (f" [{c}]" if c else ""))
    if op in (0x023, 0x024, 0x025, 0x0E5, 0x2A0, 0x0E6):
        vals = L[:1] if op == 0x0E6 else L
        names = [trainer_name(x) for x in vals if x not in (None, 0)]
        if names:
            notes.append(", ".join(names))
        if op in (0x023, 0x024, 0x025) and L[0] is not None:
            notes.append(f"flag 0x{TRAINER_FLAG_BASE + L[0]:X}")
    if op in (0x07B, 0x07C, 0x07D, 0x07E) and L[0] is not None:
        notes.append(item_name(L[0]) + (f" x{L[1]}" if L[1] is not None else ""))
    if op in (0x096, 0x124, 0x125, 0x2BD) and L[0] is not None:
        notes.append(species_name(L[0]) + (f" lv{L[1]}" if L[1] is not None else ""))
    if op == 0x096 and L[2]:
        notes.append(f"holding {item_name(L[2])}")
    if op in (0x15B, 0x15C) and L[0] is not None:
        notes.append(badge_name(L[0]))
    if op in (0x0BE, 0x11B, 0x203) and L[0] is not None:
        notes.append(map_name(L[0]))
    if op in (0x013, 0x014):
        notes.append(describe_script_ref(a[0][1], mapid))
    if op in (0x05E, 0x1B1, 0x1B2, 0x064, 0x065) and L[0] is not None:
        notes.append(f"object {L[0]}")
    if op in MSG_ARG:
        v = L[MSG_ARG[op]]
        if v is not None:
            t = msg_text(bank, v)
            notes.append(f"msg {bank} #{v}: \"{t}\"" if t is not None else f"msg {bank} #{v}")
    if op in (0x1FA, 0x1FB) and L[0] is not None and L[1] is not None:
        t = msg_text(L[0], L[1])
        notes.append(f"msg {L[0]} #{L[1]}: \"{t}\"" if t is not None else f"msg {L[0]} #{L[1]}")
    return "; ".join(notes)


def render_script(sc, bank, mapid, out):
    order = sorted(set(sc.insts) | set(sc.movements))
    prev_end, prev, known = None, None, {}
    for off in order:
        if prev_end is not None and off != prev_end:
            out.append("")
        if off in sc.labels:
            idx = sc.entry_index(off)
            ents = [i - 1 for i in idx]
            if len(ents) > 8:
                ents = f"{len(ents)} entries: {ents[0]}..{ents[-1]}"
            extra = f"   ; header entries {ents}" if idx else ""
            out.append(f"{sc.labels[off]}:{extra}")
            known, prev = {}, None
        if off in sc.insts:
            ins = sc.insts[off]
            argtxt = ", ".join(fmt_arg(k, v, sc.labels) for k, v in ins.args)
            note = annotate(ins, bank, mapid, known, prev)
            line = f"  @0x{off:04X}  {ins.op:03X} {OPS[ins.op].display}" + (f" {argtxt}" if argtxt else "")
            if note:
                line += f"   ; {note}"
            out.append(line)
            prev_end, prev = off + ins.size, ins
            if ins.op == 0x028:  # SetVar var, literal
                known[ins.args[0][1]] = ins.args[1][1]
            else:
                for k, v in ins.args:
                    if k == "p":
                        known.pop(v, None)
            if ins.op in CALLS or ins.op in (0x013, 0x014):  # callee may rewrite vars
                known = {}
        else:
            steps = sc.movements[off]
            out.append("  " + " ".join(f"{c:02X}x{n}" if c != 0xFE else "end" for _, c, n in steps)
                       + f"   ; movement list @0x{off:04X} (u16 cmd, u16 count; 0xFE ends)")
            prev_end, prev, known = off + 4 * len(steps), None, {}


# ----------------------------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------------------------
def cmd_map(args):
    i = resolve_map(args.map)
    r = map_table()[i]
    ml = maps_line(i)
    print(f"{map_name(i)} = {i}   (maps.h:{ml}; map_header.c:{r['line']} // {r['comment']})")
    print(f"  scripts       scr_seq {r['scripts']:04d}")
    print(f"  level scripts scr_seq {r['level_scripts']:04d}")
    print(f"  text          msg {r['msg_bank']:04d}")
    print(f"  events        zone_event {r['events']:04d}")
    enc = r["wild_encounters"]
    print("  encounters    " + (f"d_enc_data {enc[0]:04d} (Diamond) / p_enc_data {enc[1]:04d} (Pearl)" if enc else "none"))
    print(f"  mapsec {r['mapsec']}  matrix {r['matrix']}  map_type {r['map_type']}  music {r['day_music']}/{r['night_music']}"
          f"  weather {r['weather']}  camera {r['camera']}  bike {r['bike']} rope {r['escape_rope']} fly {r['fly']}")
    ls = parse_level_scripts(r["level_scripts"])
    print(f"  level scripts (scr_seq {r['level_scripts']:04d}):" + ("" if ls else " none"))
    for e in ls:
        tn = LEVEL_SCRIPT_TYPES.get(e["type"], f"type{e['type']}")
        if e["type"] == 1:
            for t in e["table"]:
                val = fmt_var(t["value"]) if t["value"] >= 0x4000 else t["value"]
                print(f"    @0x{t['off']:04X} {tn}: when var {fmt_var(t['var'])} == {val} run #{t['script']}"
                      f" ({describe_script_ref(t['script'], i)})")
        else:
            print(f"    @0x{e['off']:04X} {tn}: run #{e['script']} ({describe_script_ref(e['script'], i)})")
    if r["scripts"] in level_script_files():
        print("  note: script file is also used as a level-script file")
    try:
        sc = get_script(r["scripts"])
        print(f"  {len(sc.entries)} scripts in scr_seq {r['scripts']:04d}")
    except DisasmError as e:
        print(f"  scr_seq {r['scripts']:04d}: {e}")


def resolve_script_arg(arg):
    """-> (fileno, msg bank, map id or None, entry index or None)"""
    m = re.fullmatch(r"(?:sid:)(\d+)", arg)
    if m:
        sid = int(m.group(1))
        fileno, idx, bank, _label = script_id_target(sid)
        if fileno is None:
            raise SystemExit(f"script id {sid} is map-local: use `script MAP_...`")
        return fileno, bank, None, idx + 1
    if re.fullmatch(r"\d+", arg):
        fileno = int(arg)
        bank, _ = default_msg_bank(fileno)
        scr, _ = file_users()
        maps = [mi for mi, role in scr.get(fileno, []) if role == "scripts"]
        return fileno, bank, (maps[0] if maps else None), None
    i = resolve_map(arg)
    r = map_table()[i]
    return r["scripts"], r["msg_bank"], i, None


def cmd_script(args):
    fileno, bank, mapid, only = resolve_script_arg(args.target)
    if args.msg is not None:
        bank = args.msg
    level_shaped = False
    if fileno not in referenced_script_files() and fileno not in level_script_files():
        try:
            get_script(fileno)
        except DisasmError:
            level_shaped = parse_level_scripts_ok(fileno)
    if (fileno in level_script_files() or level_shaped) and not args.force:
        scr, _ = file_users()
        users = [map_name(mi) for mi, role in scr.get(fileno, []) if role == "level_scripts"][:5]
        print(f"scr_seq {fileno:04d} is a level-script file ({', '.join(users) or 'no map header'});"
              f" decoded as such:")
        for e in parse_level_scripts(fileno):
            tn = LEVEL_SCRIPT_TYPES.get(e["type"], f"type{e['type']}")
            if e["type"] == 1:
                for t in e["table"]:
                    print(f"  @0x{t['off']:04X} {tn}: var {fmt_var(t['var'])} == {t['value']} -> #{t['script']}")
            else:
                print(f"  @0x{e['off']:04X} {tn}: #{e['script']}")
        return
    try:
        sc = get_script(fileno)
    except DisasmError as e:
        raise SystemExit(f"error: {e}")
    scr, _ = file_users()
    users = sorted({map_name(mi) for mi, role in scr.get(fileno, []) if role == "scripts"})
    print(f"scr_seq {fileno:04d}  ({len(sc.d)} bytes, {len(sc.entries)} scripts; msg bank "
          f"{bank if bank is not None else '?'}; used by {', '.join(users[:6]) or 'no map header'}"
          f"{' ...' if len(users) > 6 else ''})")
    if mapid is not None:
        refs = script_refs_from_events(mapid)
        for k in sorted(refs):
            print(f"  #{k} <- {', '.join(refs[k])}")
    out = []
    if only is not None:
        sub = Script.__new__(Script)
        sub.__dict__.update(sc.__dict__)
        tgt = sc.entries[only - 1]
        reach, todo = set(), [tgt]
        while todo:
            o = todo.pop()
            while o in sc.insts and o not in reach:
                reach.add(o)
                ins = sc.insts[o]
                for k, v in ins.args:
                    if k == "o":
                        todo.append(v)
                if ins.op in TERMINATORS:
                    break
                o += ins.size
        mv = {v for o in reach for k, v in sc.insts[o].args if k == "m"}
        sub.insts = {o: sc.insts[o] for o in reach}
        sub.movements = {o: sc.movements[o] for o in mv}
        sc = sub
    render_script(sc, bank, mapid, out)
    print("\n".join(out))


def script_refs_from_events(mapid):
    r = map_table()[mapid]
    refs = {}
    ev = parse_events(r["events"])
    for k, o in enumerate(ev["object"]):
        if 0 < o["script"] < 2000:
            refs.setdefault(o["script"], []).append(f"zone_event {r['events']:04d} object {k} (id {o['id']})")
    for k, b in enumerate(ev["bg"]):
        if 0 < b["script"] < 2000:
            refs.setdefault(b["script"], []).append(f"bg {k}")
    for k, c in enumerate(ev["coord"]):
        if 0 < c["script"] < 2000:
            refs.setdefault(c["script"], []).append(f"coord {k} (var {fmt_var(c['var'])}=={c['value']})")
    for e in parse_level_scripts(r["level_scripts"]):
        if e["type"] == 1:
            for t in e["table"]:
                refs.setdefault(t["script"], []).append(
                    f"level {LEVEL_SCRIPT_TYPES[1]} var {fmt_var(t['var'])}=={t['value']}")
        else:
            refs.setdefault(e["script"], []).append(f"level {LEVEL_SCRIPT_TYPES.get(e['type'], e['type'])}")
    return refs


def cmd_events(args):
    i = resolve_map(args.map)
    r = map_table()[i]
    ev = parse_events(r["events"])
    print(f"{map_name(i)} = {i}  zone_event {r['events']:04d}  (scripts scr_seq {r['scripts']:04d}, msg {r['msg_bank']})")
    print(f"objects ({len(ev['object'])}):")
    for k, o in enumerate(ev["object"]):
        sid = o["script"]
        extra = ""
        if o["trainer_type"]:
            extra = f" trainer_type {o['trainer_type']} sight {o['param'][0]}"
        flag = f" hidden_flag 0x{o['flag']:X}" if o["flag"] else ""
        print(f"  object {k}: id {o['id']} {sprite_name(o['gfx'])}({o['gfx']}) at ({o['x']},{o['z']})"
              + (f" y_raw={o['y']}" if o["y"] else "") +
              f" facing {dir_name(o['facing'])} movement {o['movement']} range ({o['xrange']},{o['zrange']}){extra}{flag}"
              f" script {sid} [{describe_script_ref(sid, i) if sid else 'none'}]")
        if 3000 <= sid < 5000 + 850 and (sid < 3000 + 850 or sid >= 5000):
            tid = sid - 2999 if sid < 5000 else sid - 4999
            if 0 < tid < len(trainers()):
                print(f"           {trainer_name(tid)} ({tid}) flag 0x{TRAINER_FLAG_BASE + tid:X}: {trainer_summary(tid)}")
    print(f"warps ({len(ev['warp'])}):")
    for k, w in enumerate(ev["warp"]):
        print(f"  warp {k}: ({w['x']},{w['z']}) -> {map_name(w['dest'])}({w['dest']}) warp {w['warp']}")
    print(f"coords ({len(ev['coord'])}):")
    for k, c in enumerate(ev["coord"]):
        print(f"  coord {k}: ({c['x']},{c['z']}) size {c['w']}x{c['l']} when var {fmt_var(c['var'])} == {c['value']}"
              f" run #{c['script']} [{describe_script_ref(c['script'], i)}]")
    print(f"bgs ({len(ev['bg'])}):")
    hid = hidden_items()
    for k, b in enumerate(ev["bg"]):
        extra = ""
        if b["type"] == 2 or 8000 <= b["script"] < 8800:
            hi = hid.get(b["script"] - 8000)
            extra = f" hidden item flag 0x{b['script'] - HIDDEN_ITEM_FLAG_SUB:X}"
            if hi:
                extra += f" {item_name(hi[0])} x{hi[1]}"
        print(f"  bg {k}: ({b['x']},{b['z']}) type {b['type']} facing {b['facing']} script {b['script']}"
              f" [{describe_script_ref(b['script'], i) if b['script'] else 'none'}]{extra}")


def cmd_trainer(args):
    tid = resolve_trainer(args.trainer)
    t = trainers()[tid]
    by_name, _ = constants("trainers")
    name = trainer_name(tid)
    line = by_name.get(name, (None, None))[1]
    print(f"{name} = {tid}  (trainers.h:{line}; trdata.json #{tid})")
    print(f"  class {t['class']}  name {t['name'].replace('{TRNAME}', '')}  type {t['type']}"
          f"  double {t['doubleBattle']}  items {t['items']}")
    print(f"  trainer flag 0x{TRAINER_FLAG_BASE + tid:X}; battle script id {2999 + tid}"
          f" (scr_seq 1040 entry {tid - 1}); double-battle 2nd-trainer id {4999 + tid}")
    for m in t["party"]:
        bits = [f"{m['species']} lv{m['level']}"]
        if m.get("item", "ITEM_NONE") != "ITEM_NONE":
            bits.append(f"@{m['item']}")
        if m.get("moves"):
            bits.append("[" + ", ".join(m["moves"]) + "]")
        bits.append(f"(difficulty {m['difficulty']})")
        print("   - " + " ".join(bits))
    if args.where:
        sid = 2999 + tid
        _, evt = file_users()
        for fileno in sorted(evt):
            ev = parse_events(fileno)
            for k, o in enumerate(ev["object"]):
                if o["script"] in (sid, 4999 + tid):
                    maps = ", ".join(map_name(m) for m in evt[fileno][:3])
                    print(f"  placed: zone_event {fileno:04d} object {k} ({maps}) at ({o['x']},{o['z']})"
                          f" facing {dir_name(o['facing'])} sight {o['param'][0]}")
        for fileno in iter_scripts():
            sc = get_script(fileno)
            for off in sorted(sc.insts):
                ins = sc.insts[off]
                if ins.op in (0x0E5, 0x2A0) and tid in [lit(k, v) for k, v in ins.args]:
                    print(f"  battle: scr_seq {fileno:04d} @0x{off:04X} {OPS[ins.op].display}"
                          f" {', '.join(fmt_arg(k, v, sc.labels) for k, v in ins.args)}   [{file_label(fileno)}]")


def referenced_script_files():
    scr, _ = file_users()
    refs = {f for f, users in scr.items() if any(role == "scripts" for _, role in users)}
    refs |= {f for _, f, _ in SCRIPT_ID_RANGES if f is not None}
    return refs


def all_files():
    return sorted(int(p.stem[5:]) for p in (root() / SCR_DIR).glob("narc_*.bin"))


def iter_scripts():
    """Command-script files: referenced as a map's scripts / a script-id range, plus unreferenced files
    that decode as scripts (unreferenced level-script-shaped files are skipped)."""
    refs, levels = referenced_script_files(), level_script_files()
    for fileno in all_files():
        if fileno in refs:
            yield fileno
        elif fileno not in levels:
            try:
                get_script(fileno)
            except DisasmError:
                continue
            yield fileno


def file_label(fileno):
    scr, _ = file_users()
    maps = [map_name(mi) for mi, role in scr.get(fileno, []) if role == "scripts"]
    if maps:
        return maps[0] + (f" +{len(maps) - 1}" if len(maps) > 1 else "")
    for base, f, _b in SCRIPT_ID_RANGES:
        if f == fileno:
            return f"script ids {base}+"
    return "-"


def cmd_grep_flag(args):
    flag = int(args.flag, 0)
    print(f"flag 0x{flag:X} {flag_class(flag)}")
    for fileno in iter_scripts():
        try:
            sc = get_script(fileno)
        except DisasmError as e:
            print(f"  ! {e}")
            continue
        for off in sorted(sc.insts):
            ins = sc.insts[off]
            note = None
            if ins.op in (0x01E, 0x01F, 0x020) and ins.args[0][1] == flag:
                note = ""
            tid = flag - TRAINER_FLAG_BASE
            if ins.op in (0x023, 0x024, 0x025) and lit(*ins.args[0]) == tid:
                note = ""
            if ins.op in (0x0E5, 0x2A0) and tid > 0 and tid in [lit(k, v) for k, v in ins.args]:
                note = "   (battle with this trainer)"
            if note is not None:
                print(f"  scr_seq {fileno:04d} @0x{off:04X} {OPS[ins.op].display}"
                      f" {', '.join(fmt_arg(k, v, sc.labels) for k, v in ins.args)}   [{file_label(fileno)}]{note}")
    _, evt = file_users()
    for fileno in sorted(evt):
        ev = parse_events(fileno)
        for k, o in enumerate(ev["object"]):
            if o["flag"] == flag:
                print(f"  zone_event {fileno:04d} object {k} (id {o['id']}, {sprite_name(o['gfx'])}) hidden_flag"
                      f" [{map_name(evt[fileno][0])}]")
            sid = o["script"]
            if flag >= TRAINER_FLAG_BASE and sid in (2999 + flag - TRAINER_FLAG_BASE, 4999 + flag - TRAINER_FLAG_BASE):
                print(f"  zone_event {fileno:04d} object {k} is the trainer owning this flag [{map_name(evt[fileno][0])}]")
        for k, b in enumerate(ev["bg"]):
            if 8000 <= b["script"] < 8800 and b["script"] - HIDDEN_ITEM_FLAG_SUB == flag:
                print(f"  zone_event {fileno:04d} bg {k} hidden item [{map_name(evt[fileno][0])}]")


def cmd_grep_var(args):
    var = int(args.var, 0)
    print(f"var {fmt_var(var)}")
    for fileno in iter_scripts():
        try:
            sc = get_script(fileno)
        except DisasmError as e:
            print(f"  ! {e}")
            continue
        for off in sorted(sc.insts):
            ins = sc.insts[off]
            if any((k in "pv" and v == var) for k, v in ins.args):
                print(f"  scr_seq {fileno:04d} @0x{off:04X} {OPS[ins.op].display}"
                      f" {', '.join(fmt_arg(k, v, sc.labels) for k, v in ins.args)}   [{file_label(fileno)}]")
    for mi, r in enumerate(map_table()):
        for e in parse_level_scripts(r["level_scripts"]):
            for t in e.get("table", []):
                if t["var"] == var or t["value"] == var:
                    print(f"  scr_seq {r['level_scripts']:04d} @0x{t['off']:04X} level ON_FRAME_TABLE: var == "
                          f"{t['value']} -> #{t['script']} [{map_name(mi)}]")
    _, evt = file_users()
    for fileno in sorted(evt):
        for k, c in enumerate(parse_events(fileno)["coord"]):
            if c["var"] == var:
                print(f"  zone_event {fileno:04d} coord {k} ({c['x']},{c['z']}) {c['w']}x{c['l']} when == {c['value']}"
                      f" run #{c['script']} [{', '.join(map_name(m) for m in evt[fileno][:3])}]")


def cmd_grep_op(args):
    want = args.op
    ids = [o.id for o in OPS.values() if want.lower() in (o.name.lower(), ALIASES.get(o.id, "").lower())]
    if not ids:
        try:
            ids = [int(want, 16)]
        except ValueError:
            raise SystemExit(f"unknown opcode {want!r}")
    for fileno in iter_scripts():
        try:
            sc = get_script(fileno)
        except DisasmError as e:
            print(f"  ! {e}")
            continue
        for off in sorted(sc.insts):
            ins = sc.insts[off]
            if ins.op in ids:
                print(f"  scr_seq {fileno:04d} @0x{off:04X} {OPS[ins.op].display}"
                      f" {', '.join(fmt_arg(k, v, sc.labels) for k, v in ins.args)}   [{file_label(fileno)}]")


def cmd_text(args):
    msgs = msg_bank(args.bank)
    if msgs is None:
        raise SystemExit(f"no msg bank {args.bank}")
    idxs = [args.idx] if args.idx is not None else sorted(msgs)
    for i in idxs:
        print(f"msg {args.bank:04d} #{i}: {msg_text(args.bank, i)}")


def cmd_text_grep(args):
    rx = re.compile(args.regex, re.IGNORECASE)
    for p in sorted((root() / MSG_DIR).glob("narc_*.gmm")):
        bank = int(p.stem[5:])
        msgs = msg_bank(bank) or {}
        for i in sorted(msgs):
            if rx.search(msgs[i]):
                print(f"msg {bank:04d} #{i}: {msg_text(bank, i, 160)}")


def cmd_opcodes(args):
    for o in sorted(OPS.values(), key=lambda o: o.id):
        args_ = o.args or "-"
        if o.id in VAR_ARGS:
            sel, cases = VAR_ARGS[o.id]
            args_ = f"{sel}+{{" + ", ".join(f"{k}:{v or '-'}" for k, v in cases.items()) + "}"
        hint = f"  (decomp todo: {o.hint})" if o.hint else ""
        print(f"{o.id:03X} {o.display:40s} {args_:24s} {o.src}{hint}")


def cmd_coverage(args):
    """Disassemble every script file; report decode errors and opcode usage."""
    refs, levels = referenced_script_files(), level_script_files()
    used, errors, nfiles, ninst, unref_ok, unref_bad = {}, [], 0, 0, [], []
    nbytes = ncovered = 0
    for fileno in all_files():
        if fileno in levels and fileno not in refs:
            continue
        try:
            sc = get_script(fileno)
        except DisasmError as e:
            (errors if fileno in refs else unref_bad).append((fileno, str(e)))
            continue
        if fileno not in refs:
            unref_ok.append(fileno)
        nfiles += 1
        ninst += len(sc.insts)
        nbytes += len(sc.d)
        ncovered += sc.covered + sc.header_end
        for ins in sc.insts.values():
            used[ins.op] = used.get(ins.op, 0) + 1
    print(f"scr_seq files: {len(all_files())}; level-script files (map headers): {len(levels)};"
          f" referenced script files: {len(refs)}")
    print(f"decoded script files: {nfiles} (incl. {len(unref_ok)} unreferenced); instructions: {ninst};"
          f" distinct opcodes used: {len(used)}/{len(OPS)}; bytes reached (header+code+movement):"
          f" {ncovered}/{nbytes} (rest: unreachable code, padding)")
    print(f"decode errors in referenced script files: {len(errors)}")
    for _, e in errors:
        print("  ! " + e)
    shaped = [f for f, _ in unref_bad if parse_level_scripts_ok(f)]
    print(f"unreferenced files that are not scripts: {len(unref_bad)} ({len(shaped)} parse as level scripts:"
          f" {' '.join(f'{f:04d}' for f in shaped)})")
    for f, e in unref_bad:
        if f not in shaped:
            print("  ? " + e)
    if args.verbose:
        for op in sorted(used):
            print(f"  {op:03X} {OPS[op].display:40s} {used[op]}")
    if errors:
        sys.exit(1)


def parse_level_scripts_ok(fileno):
    d = read_bytes(f"{SCR_DIR}/narc_{fileno:04d}.bin")
    try:
        ls = parse_level_scripts(fileno)
    except struct.error:
        return False
    end = max([e["off"] + 5 for e in ls] + [0])
    return len(d) > 0 and end < len(d) and d[end] == 0 and all(e["type"] in LEVEL_SCRIPT_TYPES for e in ls)

def cmd_version_diff(args):
    """Diamond vs Pearl: wild encounter banks (ENCDATA, map_header.c:20), scripts that branch on
    GetGameVersion (0x246), and DIAMOND/PEARL conditionals in code (e.g. honey-tree tables).
    Scripts/events/trainers/text have no Pearl files."""
    enc_maps = {}
    for mi, r in enumerate(map_table()):
        if r["wild_encounters"]:
            enc_maps.setdefault(r["wild_encounters"], []).append(map_name(mi))
    same = 0
    print("wild encounter banks that differ (files/fielddata/encountdata/{d,p}_enc_data/narc_NNNN.bin):")
    for (d_no, p_no), maps in sorted(enc_maps.items()):
        d = read_bytes(f"files/fielddata/encountdata/d_enc_data/narc_{d_no:04d}.bin")
        p = read_bytes(f"files/fielddata/encountdata/p_enc_data/narc_{p_no:04d}.bin")
        if d == p:
            same += 1
            continue
        diff = [i for i in range(min(len(d), len(p))) if d[i] != p[i]]
        print(f"  d_enc {d_no:04d} / p_enc {p_no:04d}: {len(diff)} bytes differ (first @0x{diff[0]:X})"
              f"  [{', '.join(maps[:4])}{' ...' if len(maps) > 4 else ''}]")
    print(f"  ({same} encounter banks identical)")
    print("scripts branching on GetGameVersion (0x246):")
    for fileno in iter_scripts():
        sc = get_script(fileno)
        for off in sorted(sc.insts):
            if sc.insts[off].op == 0x246:
                print(f"  scr_seq {fileno:04d} @0x{off:04X} GetGameVersion"
                      f" {', '.join(fmt_arg(k, v, sc.labels) for k, v in sc.insts[off].args)}   [{file_label(fileno)}]")
    print("version conditionals in code (.ifdef/#ifdef DIAMOND|PEARL; build/ excluded):")
    rx = re.compile(r"^\s*[.#]\s*ifn?def\s+(DIAMOND|PEARL)\b")
    for path in sorted(root().glob("arm9/**/*")):
        if path.suffix not in (".s", ".c", ".h", ".inc") or "build" in path.relative_to(root()).parts:
            continue
        for ln, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if rx.match(line):
                print(f"  {path.relative_to(root())}:{ln}: {line.strip()}")


def cmd_resolve(args):
    sid = int(args.sid, 0)
    fileno, idx, bank, label = script_id_target(sid)
    print(f"script id {sid}: {label}" + (f" -> scr_seq {fileno:04d} entry {idx} (label script_{idx + 1}), msg {bank}"
                                          if fileno is not None else ""))


def main(argv=None):
    global ROOT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", help="pokediamond tree (default: <repo>/games/diamond)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("map", help="map header: script/level-script/event/text ids")
    s.add_argument("map", help="MAP_NAME or id")
    s.set_defaults(fn=cmd_map)
    s = sub.add_parser("script", help="disassemble scr_seq file: narc index, MAP_NAME, or sid:<script id>")
    s.add_argument("target")
    s.add_argument("--msg", type=int, help="override msg bank used to inline text")
    s.add_argument("--force", action="store_true", help="decode a level-script file as commands")
    s.set_defaults(fn=cmd_script)
    s = sub.add_parser("events", help="objects/warps/coords/bgs of a map")
    s.add_argument("map")
    s.set_defaults(fn=cmd_events)
    s = sub.add_parser("trainer", help="trainer party (TRAINER_CONST, substring, or id)")
    s.add_argument("trainer")
    s.add_argument("--where", action="store_true", help="also list zone_event objects that use this trainer")
    s.set_defaults(fn=cmd_trainer)
    s = sub.add_parser("grep-flag", help="every script/event that sets/clears/checks a flag")
    s.add_argument("flag")
    s.set_defaults(fn=cmd_grep_flag)
    s = sub.add_parser("grep-var", help="every script/level script/coord event using a var")
    s.add_argument("var")
    s.set_defaults(fn=cmd_grep_var)
    s = sub.add_parser("grep-op", help="every use of an opcode (name, alias or hex id)")
    s.add_argument("op")
    s.set_defaults(fn=cmd_grep_op)
    s = sub.add_parser("text", help="print a msg bank (or one message)")
    s.add_argument("bank", type=int)
    s.add_argument("idx", type=int, nargs="?")
    s.set_defaults(fn=cmd_text)
    s = sub.add_parser("text-grep", help="search all msg banks (regex, case-insensitive)")
    s.add_argument("regex")
    s.set_defaults(fn=cmd_text_grep)
    s = sub.add_parser("opcodes", help="the D/P command table with argument layouts and sources")
    s.set_defaults(fn=cmd_opcodes)
    s = sub.add_parser("coverage", help="disassemble every script file; fail on any decode error")
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(fn=cmd_coverage)
    s = sub.add_parser("resolve", help="where a script id lives (std/trainer/hidden-item ranges)")
    s.add_argument("sid")
    s.set_defaults(fn=cmd_resolve)
    s = sub.add_parser("version-diff", help="Diamond vs Pearl: differing encounter banks, GetGameVersion users")
    s.set_defaults(fn=cmd_version_diff)
    args = ap.parse_args(argv)
    if args.root:
        ROOT = Path(args.root).resolve()
    args.fn(args)


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
