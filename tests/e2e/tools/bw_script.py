#!/usr/bin/env python3
"""bw_script.py - make Pokemon Black/White field data citable, read from the cartridge alone.

Black/White have no decomp (docs/BW_PLAN.md): every fact here comes from the player's ROM (stdlib only,
read-only), so the same commands work on Black (IRBO) and White (IRAO):

  zone headers   a/0/1/2   one member, a 48-byte record per zone id (`zone` prints one)
  zone events    a/1/2/5   member = header +0x16: bg events, objects, warps, triggers, level scripts
  scripts        a/0/5/7   member = header +0x06 (the zone's scripts); its text bank is header +0x0A
  story text     a/0/0/3   message banks (`msg NNN #i`); system text a/0/0/2 (species 70, items 54,
                           moves 203, locations 89, trainer names 190, trainer classes 191)
  trainers       a/0/9/2 (trdata, 20 bytes) and a/0/9/3 (trpoke)
  command table  overlay 10 (the field overlay) holds the script engine's opcode -> handler table
                 (Black 0x0217062C, White 0x0217064C: 609 Thumb handlers, count word at -0x54) and the
                 shared-script range table (Black 0x021701A8, White 0x021701C8: 46 x {first id, last id,
                 a/0/5/7 member, archive, a/0/0/3 bank}).

Cite facts printed by this tool as `scr NNNN @0xOFF` (a/0/5/7 member, byte offset), `zone_event ZZZ object k`
(also bg / warp / trigger / level), `zone ZZZ` (header), `msg NNNN #i` (a/0/0/3), `sysmsg NNN #i` (a/0/0/2),
`trdata #N`.

Script files: a header of s32 offsets (each relative to the end of its own field) ended by u16 0xFD13 (or by
the first script, scr 0865); entry k (1-based) is "script k". Script ids: 1..999 are the running zone's entries; others go through the range
table (2000.. shared scripts, 3000..6999 trainer scripts, ...). Level scripts: the a/0/5/7 member named by
header +0x08 (the event file's tail holds the same bytes), u16 type + u32 operand entries ended by u16 0; type 1's operand is the offset of a {var, value, script id}
table (checked on entry until one matches), the other types name a script directly.

The command table (OPTABLE) is DERIVED from the ROM: `derive` reads the handler table out of overlay 10 and,
from the generated assembly of the recompiler (games/ndsrec/build/pc-wasm/<game>/ndsrec/asm, or --asm),
follows every handler's paths and records which script-reading primitives it calls on the script context, in
order: ScriptReadU16 (sub_02011330: h), ScriptReadU32 (sub_0201134C: w), the var-pointer read
(ov10_02159AE8: p), the var-or-value read (ov10_02159B10: v, >= 0x4000 reads a var), inline byte reads of
the script pointer (b), and a u32 added to the script pointer (o: code offset, m: movement list, d: data).
Wait callbacks installed with sub_020113D0 are followed too (they read their operands when they finish).
Disassembly stops with an error at any unknown opcode, truncated instruction or out-of-file jump.
Command names (NAMES) are ours: `h` = what the handler visibly does, `u` = from how the scripts use it, cross-
checked where possible with public notes (PhoenixBound/b2w2-scripts cmd_table.xml); [INFERENCE] otherwise.

Conditions: 0x08 PushConst / 0x09 PushVar / 0x10 PushFlag push values, 0x11 Compare pops two and pushes the
result (0 <, 1 ==, 2 >, 3 <=, 4 >=, 5 !=, 6 or, 7 and; the left operand is the first pushed), and JumpIf/
CallIf 0x1F/0x20 with condition 0xFF pop it and branch when it is FALSE ("unless"). With conditions 0..5
they test the result byte of 0x17-0x1A Compare* (0 <, 1 ==, 2 >, 3 <=, 4 >=, 5 !=; table ov10 0x021705DC).
"""

import argparse
import os
import re
import signal
import struct
import sys
from functools import cache
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "ndsrec"))

ROM_NAMES = {
    "black": "Pokemon - Black Version (USA, Europe) (NDSi Enhanced).nds",
    "white": "Pokemon - White Version (USA, Europe) (NDSi Enhanced).nds",
}
# per game code: handler table, its count word, the shared-script range table, the condition table (overlay 10)
TABLES = {
    "IRBO": dict(handlers=0x0217062C, count=0x021705D8, ranges=0x021701A8, conds=0x021705DC),
    "IRAO": dict(handlers=0x0217064C, count=0x021705F8, ranges=0x021701C8, conds=0x021705FC),
}
VERSION_ID = {"IRBO": 21, "IRAO": 20}  # what 0xE0 GetVersion stores (Black 21, White 20)

# ----------------------------------------------------------------------------------------------
# The command table, as `derive --emit` prints it.  Columns: opcode, argument layout, handler, end flag.
# Layout letters: b u8, h u16 literal, v u16 var-or-value, p u16 var id, w u32, o s32 code offset,
# m s32 movement-list offset, d s32 data offset, '.' none, '-' no handler (null table entry).
# E: every path of the handler ends the script; e: some path does.
# ----------------------------------------------------------------------------------------------
OPTABLE = r"""
000 . ov10_02159240
001 . ov10_02159244
002 . ov10_02159248 E
003 h ov10_02159260
004 o ov10_02159284
005 . ov10_0215929C
006 v ov10_021592A8
007 v ov10_021592C0
008 h ov10_021593DC
009 v ov10_021593F0
00A p ov10_02159404
00B . ov10_02159420
00C . ov10_0215942C
00D . ov10_0215944C
00E . ov10_0215946C
00F . ov10_0215948C
010 v ov10_021594B0
011 h ov10_021594E0
012 pv ov10_02159854
013 pv ov10_02159874
014 bb ov10_021592D8
015 bw ov10_021592F0
016 bb ov10_0215930C
017 bb ov10_02159340
018 bb ov10_02159370
019 ph ov10_02159398
01A pp ov10_021593B8
01B h ov10_02159570
01C h ov10_021595CC
01D . ov10_02159644 E
01E o ov10_02159658
01F bo ov10_02159670
020 bo ov10_021596B4
021 h ov10_021596E8
022 p ov10_02159708
023 v ov10_02159728
024 v ov10_02159750
025 vp ov10_02159778
026 pv ov10_021597AC
027 pv ov10_021597CC
028 ph ov10_02159894
029 pp ov10_021598AC
02A pv ov10_021598C8
02B pv ov10_021597EC
02C pv ov10_0215980C
02D pv ov10_02159830
02E . ov10_021598E4
02F . ov10_021598EC
030 . ov10_02159934
031 . ov10_02159974
032 . ov10_02159998
033 h ov21_021B15C8
034 vh ov21_021B04D8
035 vh ov21_021B0530
036 . ov21_021B0624
037 v ov21_021B05AC
038 vb ov21_021B10D0
039 . ov21_021B10E8
03A hvvh ov21_021B1100
03B h ov21_021B11D8
03C vvvvv ov21_021B0D14
03D vvvv ov21_021B0D74
03E . ov21_021B0EDC
03F . ov21_021B14F4
040 vv ov21_021B063C
041 . ov21_021B07E0
042 . ov21_021B0730
043 hh ov21_021B12B0
044 . ov21_021B13E0
045 hbbh ov21_021B143C
046 . ov21_021B14DC
047 p ov21_021B0130
048 vvvvvv ov21_021B0DF0
049 vvvvvv ov21_021B0E74
04A vb ov21_021B10DC
04B . ov21_021B15B8
04C b ov21_021AF75C
04D bv ov21_021AF7C4
04E bvvb ov21_021AF7E4
04F bv ov21_021AF7D4
050 bv ov21_021AF844
051 bv ov21_021AF878
052 bv ov21_021AF928
053 bv ov21_021AF958
054 bv ov21_021AF9A4
055 bv ov21_021AF9F0
056 bv ov21_021AF8A8
057 bv ov21_021AF908
058 bv ov21_021AF918
059 bv ov21_021AFAC8
05A bv ov21_021AFAFC
05B bv ov21_021AFA38
05C bvv ov21_021AFA80
05D bbv ov10_0215D884
05E bv ov21_021AFB2C
05F bv ov21_021AFB5C
060 bv ov21_021AFBC4
061 bv ov21_021AFC18
062 bv ov21_021AFC28
063 bv ov21_021AFB90
064 vm ov21_021B1698
065 . ov21_021B16F0
066 pv ov21_021B1700
067 vpp ov21_021B176C
068 pp ov21_021B17B4
069 vvvvvv ov21_021B181C
06A vp ov21_021B19B0
06B v ov21_021B18E4
06C v ov21_021B18B8
06D vvvvv ov21_021B1950
06E p ov21_021B17F8
06F pp ov21_021B1CB0
070 ppvvv ov21_021B1C2C
071 ppp ov21_021B2070
072 vppp ov21_021B20B0
073 vv ov21_021B1738
074 . ov21_021B1BF4
075 h ov21_021B1D20
076 vvvv ov21_021B1D64
077 . ov21_021B1E80
078 p ov21_021B1F60
079 vvp ov21_021B19E0
07A v ov21_021B2104
07B vvvv ov21_021B1FB8
07C vvv ov21_021B2154
07D vvvv ov21_021B21AC
07E v ov21_021B2218
07F vp ov21_021AE9F0
080 v ov21_021AE948
081 . ov21_021AE994
082 vp ov21_021AEA3C
083 p ov21_021AE9CC
084 p ov21_021AEA8C
085 vvv ov21_021AEABC
086 vvvv ov21_021AEB3C
087 vvv ov21_021B0EF4
088 ppp ov21_021AEBB0
089 -
08A vp ov21_021AEC1C
08B v ov21_021AEC40
08C . ov10_0215B3AC E
08D p ov10_0215B3DC
08E . ov21_021AEC84
08F -
090 -
091 -
092 vp ov21_021AECC0
093 vp ov21_021AED00
094 vvvv ov21_021AED24
095 v ov10_0215B328
096 v ov10_0215B350
097 vp ov10_0215B378
098 h ov21_021AEDE0
099 -
09A -
09B hp ov21_021AEEC0
09C -
09D -
09E . ov21_021AEFD8
09F v ov21_021AF0BC
0A0 . ov21_021AEF00
0A1 v ov21_021AF0F8
0A2 vv ov21_021AF130
0A3 v ov21_021AF458
0A4 v ov21_021AF484
0A5 pv ov21_021AF4B0
0A6 v ov21_021AF294
0A7 . ov21_021AF2BC
0A8 . ov21_021AF2E4
0A9 h ov21_021AF2F4
0AA . ov21_021AF370
0AB vv ov21_021AF380
0AC . ov21_021AF448
0AD bbvbh ov21_021B01DC
0AE bbvbh ov21_021B024C
0AF hhv ov21_021B0344
0B0 . ov21_021B0394
0B1 . ov21_021B03BC
0B2 bbvbh ov21_021B02C8
0B3 hhhh ov21_021B2830
0B4 . ov21_021B2A90
0B5 vvp ov21_021AF544
0B6 vvp ov21_021AF594
0B7 vvp ov21_021AF5E4
0B8 vvp ov21_021AF634
0B9 vp ov21_021AF684
0BA vp ov21_021AF6C4
0BB vp ov21_021AF6E8
0BC p ov21_021B5128
0BD vp ov21_021AF71C
0BE hhhh ov21_021AFD44
0BF hhhh ov21_021AFE64
0C0 hhhhh ov21_021AFF7C
0C1 hhh ov21_021AFDAC
0C2 hhhh ov21_021AFCD8
0C3 . ov21_021AFECC
0C4 hhhhh ov21_021AFEFC
0C5 . ov21_021C459C
0C6 v ov21_021C4628
0C7 . ov21_021C4678
0C8 v ov21_021C46A4
0C9 . ov10_0215AA60
0CA p ov10_0215AA90
0CB pv ov10_0215AABC
0CC p ov10_0215AADC
0CD p ov10_0215AB18
0CE p ov10_0215AAF4
0CF p ov10_0215AB3C
0D0 pp ov10_0215AB60
0D1 pp ov10_0215AB98
0D2 p ov10_0215ABD0
0D3 p ov10_0215AD38
0D4 pp ov10_0215ABEC
0D5 pv ov10_0215AC3C
0D6 v ov10_0215AC68
0D7 p ov10_0215ACA8
0D8 vp ov10_0215AEB8
0D9 v ov10_0215AD50
0DA vvv ov10_0215AE34
0DB . ov10_0215AD7C
0DC vvvvv ov10_0215ADCC
0DD vp ov10_0215AF98
0DE vv ov10_0215AEE8
0DF vvp ov10_0215AF40
0E0 p ov10_0215AA50
0E1 p ov10_0215AC1C
0E2 p ov10_0215B028
0E3 . ov10_0215B068
0E4 v ov10_0215B084
0E5 pp ov10_0215B0B4
0E6 vp ov10_0215B104
0E7 v ov10_0215B138
0E8 vp ov10_0215B158
0E9 p ov21_021CFA88
0EA p ov10_0215B190
0EB p ov10_0215CE98
0EC . ov10_0215CEBC
0ED . ov10_0215CEE8
0EE p ov10_0215CFB4
0EF p ov10_0215CFD8
0F0 v ov10_0215CF54
0F1 v ov10_0215CF84
0F2 pv ov10_0215CFFC
0F3 pv ov10_0215D034
0F4 pv ov10_0215D0A4
0F5 pv ov10_0215D0D4
0F6 pv ov10_0215D110
0F7 p ov10_0215CF00
0F8 pv ov10_0215D06C
0F9 v ov10_0215C558
0FA v ov10_0215C584
0FB pv ov10_0215C5B0
0FC pv ov10_0215B704
0FD vvh ov10_0215B74C
0FE pv ov10_0215B7E8
0FF pv ov10_0215B824
100 p ov21_021B627C
101 pv ov10_0215B660
102 pv ov10_0215B6C0
103 pv ov10_0215B888
104 . ov10_0215B5A8
105 pvv ov21_021B631C
106 p ov21_021B63D4
107 ppv ov21_021B5B4C
108 pv ov10_0215BCA0
109 ppvv ov21_021B636C
10A pvv ov10_0215BCE8
10B vvv ov10_0215BD2C
10C pvvv ov10_0215BB50
10D pv ov10_0215B848
10E pvvvvvvvv ov10_0215BBD0
10F pvv ov10_0215C2D0
110 pvv ov10_0215C138
111 vvv ov10_0215C0D0
112 pv ov10_0215B5BC
113 pv ov10_0215BE24
114 vp ov10_0215B904
115 pvv ov10_0215BAB0
116 pv ov10_0215BAF4
117 vv ov10_0215BF30
118 vpp ov10_0215BA18
119 pvv ov10_0215C01C
11A pppv ov10_0215C064
11B ppv ov10_0215B558
11C vvv ov10_0215BF7C
11D v ov21_021B5B8C
11E v ov21_021B5BFC
11F vp ov21_021B5C80
120 vv ov21_021B65D8
121 pv ov10_0215B994
122 pvvv ov10_0215C180
123 pvvvvvvvv ov10_0215C200
124 vv ov21_021B2E08
125 . ov21_021B2AB0
126 vvvv ov21_021B2CE8
127 pvvv ov21_021B2D34
128 v ov21_021B2D98
129 vv ov21_021B2DBC
12A v ov21_021B2E60
12B vvv ov21_021B2C08
12C v ov21_021B2E3C
12D vvvv ov21_021B2CA0
12E vvv ov21_021B2C5C
12F v ov21_021B2ADC
130 . ov21_021B2B24
131 . ov21_021B2B5C
132 v ov21_021B2B94
133 v ov21_021B5A18
134 . ov21_021B69B0
135 . ov21_021CFA58
136 vv ov10_02166F1C
137 p ov10_02159A64
138 ppp ov10_0215ACC8
139 p ov10_0215F1C8
13A p ov21_021B4BEC
13B p ov10_0215F214
13C . ov10_0215F228
13D . ov10_0215F238
13E . ov10_0215F280
13F . ov21_021B52FC
140 . ov21_021B536C
141 . ov21_021B538C
142 . ov21_021B53A4
143 hhwwwwh ov21_021B53BC
144 h ov21_021B5498
145 . ov21_021B54FC
146 hh ov21_021B542C
147 h ov21_021B54D4
148 vvvvvvvv ov21_021B5528
149 vp ov21_021B2FD8
14A . ov10_0215C5EC
14B . ov10_0215C610
14C . ov10_0215C6F0
14D vp ov21_021B4FB0
14E vpp ov10_0215C72C
14F pv ov21_021B4D60
150 p ov10_0215C7E0
151 vv ov10_0215CB08
152 . ov21_021B4F68
153 p ov10_0215CBF0
154 vv ov10_0215CA64
155 v ov10_0215CAB4
156 v ov21_021B4EE0 E
157 . ov21_021CA57C
158 hv ov21_021CA544
159 . ov21_021CA598
15A hvvh ov21_021CA5C4
15B v ov21_021D1A04
15C . ov21_021D1A54
15D v ov21_021D1A74
15E . ov21_021D1AB0
15F p ov21_021B4C14
160 p ov21_021B4C34
161 . ov21_021B4C54
162 . ov21_021B4C98
163 . ov10_0215CC38
164 vv ov10_0215CC4C
165 v ov10_0215CCE8
166 . ov21_021B4CDC
167 vb ov10_0215D34C e
168 v ov10_0215D468
169 bvp ov10_0215D504
16A vbp ov10_0215D7D4
16B vvvp ov10_0215DB38
16C v ov10_0215D864
16D v ov10_0215DADC
16E pp ov10_0215DB00
16F . ov18_0217CF68
170 p ov18_0217FAE0
171 . ov18_0217CF88
172 . ov10_0215C95C
173 p ov18_0217F648
174 p ov18_0217FB2C
175 p ov18_0217F8A0
176 p ov18_0217F8D0
177 . ov18_0217F914
178 vvv ov21_021B503C
179 . ov21_021B5094
17A . ov10_0215B408 E
17B p ov10_0215B438
17C p ov21_021B50D0
17D . ov21_021B50F8
17E . ov48_021F4650
17F pp ov10_0215CD3C
180 h ov21_021B2380
181 h ov21_021B23B8
182 v ov10_0215CD80
183 . ov21_021B23F0
184 p ov21_021B240C
185 h ov21_021B2444
186 v ov10_0215CDB8
187 h ov21_021B247C
188 h ov21_021B24F4
189 h ov21_021B24B8
18A . ov21_021B268C
18B . ov21_021B269C
18C p ov21_021B252C
18D p ov21_021B2568
18E h ov21_021B25A4
18F pppppppp ov10_0215CDF0
190 h ov21_021B25DC
191 h ov21_021B2614
192 h ov21_021B2650
193 h ov21_021B26AC
194 h ov21_021B26E4
195 h ov21_021B271C
196 . ov10_0215CE70
197 hh ov21_021B273C
198 h ov21_021B277C
199 v ov21_021B27F8
19A vv ov21_021B27B4
19B v ov21_021B4F1C
19C v ov21_021D1B2C
19D . ov21_021D1B8C
19E h ov21_021D29F0
19F h ov21_021C812C
1A0 v ov21_021C8170
1A1 v ov21_021C81B4
1A2 v ov21_021B5610
1A3 v ov21_021B5650
1A4 v ov21_021B568C
1A5 . ov21_021C881C
1A6 . ov21_021C8848
1A7 hp ov21_021C8868
1A8 hh ov21_021C88AC
1A9 vhvp ov10_02166734
1AA vhhv ov21_021C88EC
1AB p ov10_021667E0
1AC . ov21_021C8960
1AD . ov21_021B2910
1AE . ov21_021B2940
1AF . ov21_021B2970
1B0 . ov21_021B29A0
1B1 . ov21_021B2AA0
1B2 . ov21_021B28E0
1B3 -
1B4 . ov21_021B2890
1B5 . ov21_021B29D0
1B6 . ov21_021B2A00
1B7 . ov21_021B2A30
1B8 . ov21_021B2A60
1B9 vp ov21_021B5700
1BA vp ov21_021B574C
1BB . ov21_021CC544
1BC v ov21_021B5154
1BD vp ov10_0216BF38
1BE vv ov21_021B654C
1BF pvv ov21_021B647C
1C0 . ov21_021CC570
1C1 bp ov21_021CC5D8
1C2 . ov20_02186BE4
1C3 . ov20_02186C44
1C4 vp ov20_02186D48
1C5 v ov20_02186CC4
1C6 ppp ov20_02186EB8
1C7 . ov20_02186E94
1C8 . ov20_02187098
1C9 pv ov20_02187000
1CA . ov10_02164778
1CB d ov21_021B6AE0
1CC . ov21_021B6AFC
1CD v ov21_021B6B6C
1CE pv ov21_021B5EB8
1CF vpp ov21_021B5DF8
1D0 . ov10_0215AFE0
1D1 p ov10_0215AFF4
1D2 . ov10_0215B014
1D3 hhh ov10_02164910
1D4 . ov10_02164994
1D5 . ov10_02164978
1D6 p ov21_021B6A78
1D7 v ov21_021B69D8
1D8 vp ov21_021CFEA0
1D9 v ov21_021CFF28
1DA vp ov21_021CFF78
1DB v ov21_021CFFDC
1DC vv ov21_021D0084
1DD hp ov10_02166980
1DE vp ov10_021669C0
1DF vhp ov21_021D00B8
1E0 vppp ov10_02166A00
1E1 p ov21_021CFEF0
1E2 v ov21_021D0014
1E3 p ov21_021D004C
1E4 vp ov21_021B60F4
1E5 vvp ov21_021B6164
1E6 vvp ov21_021B6124
1E7 vpp ov21_021B61E8
1E8 pv ov10_0215BFD4
1E9 pv ov10_0215C89C
1EA vvvv ov10_0215B214
1EB vvvv ov10_0215B26C
1EC vvvvv ov10_0215B2C4
1ED vppp ov10_02166D30
1EE vp ov21_021B6BDC
1EF vp ov10_0215E1C0
1F0 vv ov21_021CC614
1F1 pp ov21_021CC6A8
1F2 . ov21_021CC6FC
1F3 vvvv ov10_0215F3A4
1F4 . ov10_0215F2F0
1F5 . ov10_0215F318
1F6 vvv ov10_0215F33C
1F7 vvvvvv ov10_0215F424
1F8 . ov10_0215F4F4
1F9 . ov10_0216A3F8
1FA . ov10_0216A420
1FB v ov10_0216A440
1FC vvp ov10_0216A470
1FD vp ov10_0216A4CC
1FE vv ov10_0216A510
1FF . ov10_0216A560
200 v ov10_0216A594
201 v ov10_0216A5C4
202 p ov10_0216A5FC
203 p ov10_0216A634
204 pp ov10_0216A674
205 p ov10_0216A6B4
206 v ov10_0216A6EC
207 vv ov10_0216A72C
208 p ov10_0216A774
209 vvvp ov21_021D1670
20A vvvp ov21_021D16D8
20B vv ov21_021D179C
20C vvvp ov21_021D213C
20D vppp ov21_021D2468
20E vp ov21_021B716C
20F vvp ov21_021B70D8
210 vvvv ov10_0216B63C
211 . ov10_0216B6DC
212 p ov10_0216B578
213 ppp ov10_0216B5AC
214 vp ov10_0216B720
215 vp ov10_0216B81C
216 p ov10_0216B86C
217 p ov10_0216B60C
218 p ov21_021B4E58
219 p ov21_021B4E9C
21A b ov21_021AFC38
21B b ov21_021AFC88
21C vv ov10_0216B7B0
21D . ov10_0216B7F8
21E p ov10_0216B504
21F . ov10_0216B538
220 vvpp ov10_0216BD54
221 vvvvvv ov10_0216BBD4
222 vvvv ov10_0216BC88
223 v ov10_0216BCE4
224 v ov10_0216BD1C
225 vvv ov10_0216A7A0
226 v ov10_0216A884
227 vvvv ov21_021D2524
228 vv ov10_0216BEC0
229 vp ov21_021D2710
22A v ov10_0216AE10
22B vp ov10_0216AD70
22C vp ov10_0216ACCC
22D vv ov10_0216AD1C
22E . ov10_0216AC40
22F vp ov10_0216ABD8
230 v ov10_0216AC90
231 p ov21_021B51BC
232 vp ov10_0215B464
233 vp ov10_0215B488
234 vp ov21_021B7364
235 p ov21_021B73B8
236 pv ov21_021B62C8
237 vvp ov21_021B5CE8
238 p ov21_021B6590
239 p ov21_021CF7DC
23A vv ov21_021D2ADC
23B vp ov21_021D2B18
23C . ov10_0216CC30
23D vp ov21_021D2B54
23E vv ov21_021D2EDC
23F . ov21_021D2F44
240 v ov21_021C81F8
241 v ov21_021B1A20
242 vv ov21_021B6628
243 hh ov21_021B1334
244 p ov21_021B5008
245 vp ov21_021B66B8
246 p ov21_021D0114
247 vvvv ov21_021B229C
248 hh ov21_021AEE28
249 vvvv ov21_021B721C
24A vv ov21_021B72CC
24B h ov21_021AF044
24C p ov21_021B2040
24D vv ov21_021AF3D8
24E . ov10_0216F8B8
24F . ov10_0216F8C8
250 . ov10_0216F8DC
251 pvv ov10_0215D14C
252 . ov21_021B2FB0
253 vv ov21_021AEF10
254 v ov21_021AEF60
255 vvvvvv ov21_021D2F6C
256 vvv ov21_021D3018
257 . ov21_021D3068
258 . ov21_021D3090
259 h ov21_021AEE78
25A hhhhh ov21_021B0000
25B vvvv ov21_021B1DEC
25C pppppp ov10_0215F52C
25D p ov10_0215F2B8
25E . ov10_0215F2DC
25F . ov21_021AF1B0
260 . ov21_021B23E0
"""

# Names: opcode name basis [note].  basis h = the handler shows it, u = usage in the scripts (cross-checked
# with public notes where they exist).  Unnamed commands print as CmdNNN.
NAMES = r"""
000 Nop h
001 Nop1 h
002 End h
003 Wait h frames; installs a wait callback
004 Call h push the return address, jump
005 Return h
008 PushConst h
009 PushVar h pushes the var-or-value
00A PopVar h
00B Pop h
00C Add h stack
00D Sub h stack
00E Mul h stack
00F Div h stack
010 PushFlag h pushes CheckFlag (sub_020142E8)
011 Compare h pops right then left, pushes the comparison
012 AndVar h
013 OrVar h
014 LoadReg h
015 LoadRegWord h
016 CopyReg h
017 CompareRegReg h
018 CompareRegValue h
019 CompareVarValue h
01A CompareVarVar h
01B RunScript h starts script id in a new context (ov10_021589A0)
01C CallStd h starts script id and waits for it
01D EndStd h ends the script
01E Jump h
01F JumpIf h
020 CallIf h
023 SetFlag h sub_02014314
024 ClearFlag h sub_0201433C
025 CheckFlagToVar h
026 AddVar h
027 SubVar h
028 SetVar h literal
029 CopyVar h
02A SetVarValue h var-or-value
02B MulVar h
02C DivVar h
02D ModVar h
02E LockAll u
02F UnlockAll u
030 WaitMoment u
032 WaitButton u
034 MessageSystem u (msg, style): narration ("received ...")
038 MessageSimple u (msg, style)
03C Message u (bank 0x400, msg, object, position, style)
03D Message2 u (bank 0x400, msg, position, style)
03E CloseMessage u
03F CloseMessage2 u
043 Signpost u (msg, style)
048 MessageGender u (bank, msg male, msg female, object, position, style) [INFERENCE: gender]
049 MessageVersion u (bank, msg White, msg Black, object, position, style)
04C BufferPlayerName u (slot)
053 BufferPartyName u (slot, party index) [INFERENCE]
057 BufferSpeciesName u (slot, species)
064 ApplyMovement u (object, movement list); 0xFF = the player
065 WaitMovement u
06B AddObject u
06C RemoveObject u
06D SetObjectPosition u (object, x, y, z, dir) [INFERENCE]
074 FacePlayer u
085 TrainerBattle u (trainer, trainer 2, mode); story fights the player cannot lose use mode 1 [INFERENCE]
08C BlackOut u after a lost battle; ends the script [INFERENCE]
08D GetBattleResult u 1 = won
08E EndBattle u back to the field after a battle [INFERENCE]
098 PlayMusic u
09E FadeMusic u [INFERENCE]
0A6 PlaySound u
0A8 WaitSound u
0A9 PlayFanfare u
0AA WaitFanfare u
0B3 FadeScreen u
0B4 WaitFade u
0BE Warp u (zone, x, z, dir) [INFERENCE]
0BF Warp u (zone, x, z, dir) [INFERENCE]
0C0 Warp u [INFERENCE]
0C1 Warp u (zone, x, z) [INFERENCE]
0C2 Warp u (zone, x, z, dir)
0C4 Warp u (zone, x, y, z, dir) [INFERENCE]
0E0 GetVersion u (var): Black 21, White 20
0E1 GetPlayerGender u (var): 0 male
10C GivePokemon u (result var, species, form, level) [INFERENCE: form]
"""

SIZE = {"b": 1, "h": 2, "v": 2, "p": 2, "w": 4, "o": 4, "m": 4, "d": 4}
CONDS = {0: "<", 1: "==", 2: ">", 3: "<=", 4: ">=", 5: "!=", 6: "or", 7: "and"}
FLOW_TERM = {0x002, 0x005, 0x01D, 0x01E}  # End, Return, EndStd, Jump: no fall-through


class Op:
    def __init__(self, op, layout, handler, end):
        self.op, self.layout, self.handler, self.end = op, layout, handler, end
        self.name, self.basis, self.note = f"Cmd{op:03X}", "", ""

    @property
    def size(self):
        return 2 + sum(SIZE[k] for k in self.layout)


@cache
def ops():
    out = {}
    for line in OPTABLE.strip().splitlines():
        f = line.split()
        if f[1] == "-":
            continue
        out[int(f[0], 16)] = Op(int(f[0], 16), "" if f[1] == "." else f[1], f[2], f[3] if len(f) > 3 else "")
    for line in NAMES.strip().splitlines():
        f = line.split(None, 3)
        o = out.get(int(f[0], 16))
        if o:
            o.name, o.basis, o.note = f[1], f[2], f[3] if len(f) > 3 else ""
    return out


class DisasmError(Exception):
    pass


# ----------------------------------------------------------------------------------------------
# The cartridge
# ----------------------------------------------------------------------------------------------
class Rom:
    def __init__(self, path):
        self.path = path
        self.raw = Path(path).read_bytes()
        self.code = self.raw[12:16].decode("ascii", "replace")
        if self.code not in TABLES:
            raise SystemExit(f"{path}: game code {self.code} is not Black (IRBO) or White (IRAO)")
        self.game = "black" if self.code == "IRBO" else "white"
        r = self.raw
        self.fnt_off, _, self.fat_off, fat_size = struct.unpack_from("<IIII", r, 0x40)
        self.paths = {}
        self._walk(0xF000, "")
        self._narcs = {}

    def _walk(self, did, prefix):
        r = self.raw
        off, first, _ = struct.unpack_from("<IHH", r, self.fnt_off + 8 * (did & 0xFFF))
        p, fid = self.fnt_off + off, first
        while r[p]:
            n = r[p] & 0x7F
            name = r[p + 1:p + 1 + n].decode("latin1")
            sub = r[p] & 0x80
            p += 1 + n
            if sub:
                self._walk(struct.unpack_from("<H", r, p)[0], prefix + name + "/")
                p += 2
            else:
                self.paths[prefix + name] = fid
                fid += 1

    def file(self, path):
        a, b = struct.unpack_from("<II", self.raw, self.fat_off + 8 * self.paths[path])
        return self.raw[a:b]

    def narc(self, path):
        if path not in self._narcs:
            self._narcs[path] = parse_narc(self.file(path))
        return self._narcs[path]

    @cache
    def overlay(self, ovid):
        from nds import blz_decompress  # tools/ndsrec/nds.py
        ovt, size = struct.unpack_from("<II", self.raw, 0x50)
        for i in range(size // 32):
            e = struct.unpack_from("<8I", self.raw, ovt + 32 * i)
            if e[0] == ovid:
                a, b = struct.unpack_from("<II", self.raw, self.fat_off + 8 * e[6])
                data = self.raw[a:b]
                if (e[7] >> 24) & 1:
                    data = blz_decompress(data[:e[7] & 0xFFFFFF])
                return e[1], bytes(data)
        raise DisasmError(f"no overlay {ovid}")

    def ov10_u32(self, addr):
        ram, data = self.overlay(10)
        return struct.unpack_from("<I", data, addr - ram)[0]

    def ov10_bytes(self, addr, n):
        ram, data = self.overlay(10)
        return data[addr - ram:addr - ram + n]


def parse_narc(d):
    if d[:4] != b"NARC":
        raise DisasmError("not a NARC")
    o = struct.unpack_from("<H", d, 0xC)[0]
    size, n = struct.unpack_from("<IH", d, o + 4)
    ents = [struct.unpack_from("<II", d, o + 12 + 8 * i) for i in range(n)]
    o += size
    o += struct.unpack_from("<I", d, o + 4)[0]  # BTNF
    base = o + 8  # GMIF
    return [d[base + a:base + b] for a, b in ents]


ROM = None


def rom():
    return ROM


def default_rom(game):
    for d in (os.environ.get("NP_BW_ROMS"), REPO / "roms", REPO.parent / "nativeplat-bw" / "roms"):
        if d and (Path(d) / ROM_NAMES[game]).is_file():
            return str(Path(d) / ROM_NAMES[game])
    raise SystemExit(f"no {game} ROM: pass --rom or set NP_BW_ROMS to the directory holding {ROM_NAMES[game]!r}")


# ----------------------------------------------------------------------------------------------
# Text
# ----------------------------------------------------------------------------------------------
def unpack9(codes):
    """0xF100-packed text: 9-bit characters, least significant first, 0x1FF ends."""
    out, acc, nbits = [], 0, 0
    for c in codes:
        acc |= c << nbits
        nbits += 16
        while nbits >= 9:
            v = acc & 0x1FF
            acc >>= 9
            nbits -= 9
            if v == 0x1FF:
                return out
            out.append(v)
    return out


def decode_text(codes):
    out, j, n = [], 0, len(codes)
    while j < n:
        c = codes[j]
        if c == 0xFFFF:
            break
        if c == 0xFFFE:
            out.append("\\n")
        elif c == 0xF000 and j + 2 < n:
            cmd, argc = codes[j + 1], codes[j + 2]
            args = codes[j + 3:j + 3 + argc]
            out.append("{%04X%s}" % (cmd, "".join(" %d" % a for a in args)))
            j += 3 + argc
            continue
        elif c == 0xF100:
            out.append(decode_text(unpack9(codes[j + 1:])))
            break
        elif c in (0x2486, 0x2487):  # the font's PK / MN glyphs ("PKMN Trainer")
            out.append("PK" if c == 0x2486 else "MN")
        elif c == 0x246D:
            out.append("\u2642")
        elif c == 0x246E:
            out.append("\u2640")
        elif 0xD800 <= c < 0xE000 or c < 0x20:
            out.append("\\x%04X" % c)
        else:
            out.append(chr(c))
        j += 1
    return "".join(out)


def parse_bank(data):
    """A Gen 5 message bank (as features/ndsdata/src/gen5_text.c): XOR-keyed UTF-16 strings."""
    if len(data) < 16:
        return []
    n = struct.unpack_from("<H", data, 2)[0]
    sec = struct.unpack_from("<I", data, 12)[0]
    out = []
    for i in range(n):
        off, ln = struct.unpack_from("<IH", data, sec + 4 + 8 * i)
        key = (0x7C89 + i * 0x2983) & 0xFFFF
        codes = []
        for j in range(ln):
            codes.append(struct.unpack_from("<H", data, sec + off + 2 * j)[0] ^ key)
            key = ((key << 3) | (key >> 13)) & 0xFFFF
        out.append(decode_text(codes))
    return out


@cache
def story_bank(i):
    return parse_bank(rom().narc("a/0/0/3")[i])


@cache
def sys_bank(i):
    return parse_bank(rom().narc("a/0/0/2")[i])


def msg_text(bank, idx, width=None):
    if bank is None:
        return None
    b = story_bank(bank)
    if not 0 <= idx < len(b):
        return None
    t = b[idx]
    return t if width is None or len(t) <= width else t[:width - 3] + "..."


def sys_name(bank, i):
    b = sys_bank(bank)
    return b[i] if 0 <= i < len(b) and b[i] else f"#{i}"


def species_name(i):
    return sys_name(70, i)


def item_name(i):
    return sys_name(54, i)


def location_name(i):
    return sys_name(89, i)


# ----------------------------------------------------------------------------------------------
# Zones, events, trainers
# ----------------------------------------------------------------------------------------------
@cache
def zones():
    """Zone header fields (48 bytes each).  Named by what the tool uses and could check: +0x06 scripts,
    +0x08 level scripts (a/0/5/7 member that holds nothing but a u32 0 in most zones), +0x0A text bank,
    +0x16 events, +0x1A location name (bank 89; low 10 bits)."""
    zh = rom().narc("a/0/1/2")[0]
    out = []
    for z in range(len(zh) // 48):
        f = struct.unpack_from("<24H", zh, 48 * z)
        out.append(dict(id=z, raw=f, scripts=f[3], level=f[4], text=f[5], events=f[11], matrix=f[12],
                        name=f[13] & 0x3FF))
    return out


def zone_name(z):
    zs = zones()
    return location_name(zs[z]["name"]) if 0 <= z < len(zs) else f"zone {z}?"


def zone_label(z):
    return f"zone {z} ({zone_name(z)})"


@cache
def events(z):
    """bg events (20 bytes), objects (36), warps (20), triggers (22), then the level-script list."""
    e = rom().narc("a/1/2/5")[zones()[z]["events"]]
    if len(e) < 8:
        return dict(bg=[], objects=[], warps=[], triggers=[], level=[])
    size = struct.unpack_from("<I", e, 0)[0]
    nb, no, nw, nt = e[4:8]
    o = 8
    bg, obj, wp, tr = [], [], [], []
    for k in range(nb):
        script, kind, unk, x, xh, zz, zh_, y = struct.unpack_from("<HHIHHHHi", e, o)
        bg.append(dict(script=script, kind=kind, unk=unk, x=x, z=zz, y=y))
        o += 20
    for k in range(no):
        f = struct.unpack_from("<14HHHi", e, o)
        obj.append(dict(id=f[0], gfx=f[1], move=f[2], kind=f[3], flag=f[4], script=f[5], dir=f[6],
                        params=f[7:10], range=(f[10], f[11]), x=f[14], z=f[15], y=f[16], raw=e[o:o + 36]))
        o += 36
    for k in range(nw):
        f = struct.unpack_from("<HHBBHHHHHHH", e, o)
        wp.append(dict(dest=f[0], dest_warp=f[1], a=f[2], b=f[3], x=f[5], y=f[6], z=f[7], w=f[8], h=f[9],
                       raw=f))
        o += 20
    for k in range(nt):
        f = struct.unpack_from("<11H", e, o)
        tr.append(dict(script=f[0], value=f[1], var=f[2], x=f[5], z=f[6], w=f[7], h=f[8], raw=f))
        o += 22
    assert o == size + 4, (z, o, size)
    return dict(bg=bg, objects=obj, warps=wp, triggers=tr, level=parse_level(e, size + 4))


def parse_level(e, o):
    out, p = [], o
    while p + 2 <= len(e):
        kind = struct.unpack_from("<H", e, p)[0]
        if kind == 0:
            break
        arg = struct.unpack_from("<I", e, p + 2)[0]
        p += 6
        if kind == 1:
            q = p + arg
            table = []
            while q + 6 <= len(e):
                var, val, sid = struct.unpack_from("<HHH", e, q)
                if var == 0:
                    break
                table.append((var, val, sid))
                q += 6
            out.append(dict(kind=1, table=table))
        else:
            out.append(dict(kind=kind, script=arg))
    return out


@cache
def trainer(t):
    d = rom().narc("a/0/9/2")[t]
    p = rom().narc("a/0/9/3")[t]
    fmt, cls, btype, n = d[0], d[1], d[2], d[3]
    items = struct.unpack_from("<4H", d, 4)
    per = 8 + (2 if fmt & 2 else 0) + (8 if fmt & 1 else 0)
    mons = []
    for k in range(n):
        q = k * per
        if q + 8 > len(p):
            break
        lvl, sp, form = struct.unpack_from("<HHH", p, q + 2)
        m = dict(species=sp, level=lvl, form=form)
        r = q + 8
        if fmt & 2:
            m["item"] = struct.unpack_from("<H", p, r)[0]
            r += 2
        if fmt & 1:
            m["moves"] = struct.unpack_from("<4H", p, r)
        mons.append(m)
    return dict(id=t, cls=cls, name=sys_name(190, t), cls_name=sys_name(191, cls), battle=btype, mons=mons,
                items=[i for i in items if i])


def trainer_summary(t):
    try:
        tr = trainer(t)
    except IndexError:
        return f"trainer {t}?"
    party = ", ".join(f"{species_name(m['species'])} {m['level']}" for m in tr["mons"])
    kind = {0: "", 1: " double", 2: " triple", 3: " rotation"}.get(tr["battle"], f" type {tr['battle']}")
    return f"{tr['cls_name']} {tr['name']} (trdata #{t}{kind}): {party}"


# ----------------------------------------------------------------------------------------------
# Script ids
# ----------------------------------------------------------------------------------------------
@cache
def ranges():
    """The shared-script range table in overlay 10: (first, last, a/0/5/7 member, a/0/0/3 bank)."""
    t = TABLES[rom().code]["ranges"]
    out = []
    for k in range(46):
        lo, hi, f, arch, bank = struct.unpack("<5H", rom().ov10_bytes(t + 10 * k, 10))
        out.append((lo, hi, f, bank))
    return out


def resolve_sid(sid, zone=None):
    """(a/0/5/7 member, entry index (1-based), text bank, label) for a script id."""
    for lo, hi, f, bank in ranges():
        if lo <= sid <= hi:
            return f, sid - lo + 1, bank, f"shared {lo}-{hi}"
    if 1 <= sid < 1000 and zone is not None:
        z = zones()[zone]
        return z["scripts"], sid, z["text"], zone_label(zone)
    return None, None, None, "unknown range"


def describe_sid(sid, zone=None):
    f, idx, bank, label = resolve_sid(sid, zone)
    if f is None:
        return f"script id {sid} ({label})"
    return f"script id {sid} -> scr {f:04d} script {idx}" + (f" [{label}]" if zone is None or sid >= 1000 else "")


@cache
def file_users():
    """a/0/5/7 member -> (zones using it as scripts, zones using it as level scripts)."""
    sc, lv = {}, {}
    for z in zones():
        sc.setdefault(z["scripts"], []).append(z["id"])
        lv.setdefault(z["level"], []).append(z["id"])
    return sc, lv


def file_label(f):
    sc, lv = file_users()
    if f in sc:
        z = sc[f]
        return "scripts of " + ", ".join(zone_label(x) for x in z[:3]) + (" ..." if len(z) > 3 else "")
    if f in lv:
        return "level scripts of " + zone_label(lv[f][0])
    for lo, hi, ff, bank in ranges():
        if ff == f:
            return f"shared scripts {lo}-{hi} (text {bank})"
    return "unreferenced"


def default_bank(f):
    sc, _ = file_users()
    if f in sc:
        return zones()[sc[f][0]]["text"]
    for lo, hi, ff, bank in ranges():
        if ff == f:
            return bank
    return None


def default_zone(f):
    sc, _ = file_users()
    return sc[f][0] if f in sc else None


# ----------------------------------------------------------------------------------------------
# Disassembly
# ----------------------------------------------------------------------------------------------
class Inst:
    __slots__ = ("off", "op", "args", "size")

    def __init__(self, off, op, args, size):
        self.off, self.op, self.args, self.size = off, op, args, size


def script_header(d, f):
    offs, o = [], 0
    while True:
        if offs and o >= min(offs):  # scr 0865 (hidden items) has no 0xFD13: its code starts right after
            return offs, o
        if o + 2 > len(d):
            raise DisasmError(f"scr {f:04d}: no 0xFD13 header end")
        if struct.unpack_from("<H", d, o)[0] == 0xFD13:
            return offs, o + 2
        if o + 4 > len(d):
            raise DisasmError(f"scr {f:04d}: truncated header")
        t = o + 4 + struct.unpack_from("<i", d, o)[0]
        if not 0 <= t < len(d):
            raise DisasmError(f"scr {f:04d}: header entry {len(offs) + 1} points outside the file (0x{t:X})")
        offs.append(t)
        o += 4


def decode_at(d, off, f):
    if off + 2 > len(d):
        raise DisasmError(f"scr {f:04d} @0x{off:04X}: truncated opcode")
    op = struct.unpack_from("<H", d, off)[0]
    o = ops().get(op)
    if o is None:
        raise DisasmError(f"scr {f:04d} @0x{off:04X}: unknown opcode 0x{op:X}")
    p, args = off + 2, []
    for k in o.layout:
        n = SIZE[k]
        if p + n > len(d):
            raise DisasmError(f"scr {f:04d} @0x{off:04X}: {o.name} truncated")
        v = int.from_bytes(d[p:p + n], "little", signed=k in "omd")
        p += n
        if k in "omd":
            v = p + v
            if not 0 <= v < len(d):
                raise DisasmError(f"scr {f:04d} @0x{off:04X}: {o.name} target 0x{v:X} outside the file")
        args.append((k, v))
    return Inst(off, op, args, p - off)


def parse_movement(d, off, f):
    steps, o = [], off
    while True:
        if o + 4 > len(d):
            raise DisasmError(f"scr {f:04d}: movement list @0x{off:04X} has no end")
        c, n = struct.unpack_from("<HH", d, o)
        steps.append((c, n))
        o += 4
        if c == 0xFE:
            return steps


def parse_datalist(d, off, f):
    """0x1CB's operand: 10-byte records (five u16) ended by u16 0xFFFF (scr 0888)."""
    recs, o = [], off
    while True:
        if o + 2 > len(d):
            raise DisasmError(f"scr {f:04d}: data list @0x{off:04X} has no end")
        if struct.unpack_from("<H", d, o)[0] == 0xFFFF:
            return recs
        if o + 10 > len(d):
            raise DisasmError(f"scr {f:04d}: data list @0x{off:04X} truncated")
        recs.append(struct.unpack_from("<5H", d, o))
        o += 10


class Script:
    def __init__(self, f):
        self.f = f
        self.d = d = rom().narc("a/0/5/7")[f]
        self.entries, self.header_end = script_header(d, f)
        self.insts, self.movements, self.data = {}, {}, {}
        self.labels = {}
        for i, off in enumerate(self.entries):
            self.labels.setdefault(off, f"script_{i + 1}")
        todo = list(self.entries)
        while todo:
            off = todo.pop()
            while off not in self.insts:
                ins = decode_at(d, off, f)
                self.insts[off] = ins
                for k, v in ins.args:
                    if k == "o":
                        self.labels.setdefault(v, f"L_{v:04X}")
                        todo.append(v)
                    elif k == "m" and v not in self.movements:
                        self.movements[v] = parse_movement(d, v, f)
                        self.labels.setdefault(v, f"M_{v:04X}")
                    elif k == "d" and v not in self.data:
                        self.data[v] = parse_datalist(d, v, f)
                        self.labels.setdefault(v, f"D_{v:04X}")
                if ins.op in FLOW_TERM or ops()[ins.op].end == "E":
                    break
                off += ins.size

    def covered(self):
        cov = bytearray(len(self.d))
        cov[:self.header_end] = b"\1" * self.header_end
        for i in self.insts.values():
            cov[i.off:i.off + i.size] = b"\1" * i.size
        for m, steps in self.movements.items():
            cov[m:m + 4 * len(steps)] = b"\1" * (4 * len(steps))
        for t, recs in self.data.items():
            cov[t:t + 10 * len(recs) + 2] = b"\1" * (10 * len(recs) + 2)
        return cov

    def entry_index(self, off):
        return [i + 1 for i, o in enumerate(self.entries) if o == off]


def sweep(sc):
    """Classify the bytes no entry reaches: unreachable code (decodes cleanly to a terminator), movement
    lists (u16 command, u16 count, ended by 0xFE), zero padding, and anything else ("unknown")."""
    d, cov = sc.d, sc.covered()
    res = {"code": 0, "movement": 0, "pad": 0, "unknown": 0}
    unknown = []
    i = 0
    while i < len(d):
        if cov[i]:
            i += 1
            continue
        j = i
        while j < len(d) and not cov[j]:
            j += 1
        o = i
        while o < j:
            m = _movement_at(d, o, j)
            if m:
                res["movement"] += m
                o += m
                continue
            if d[o] == 0:  # alignment byte (movement lists sit on even offsets after odd-sized code)
                res["pad"] += 1
                o += 1
                continue
            c = _code_at(d, o, j)
            if c:
                res["code"] += c
                o += c
                continue
            res["unknown"] += j - o
            unknown.append((o, j))
            break
        i = j
    return res, unknown


def _movement_at(d, o, end):
    p = o
    while p + 4 <= end:
        c, n = struct.unpack_from("<HH", d, p)
        p += 4
        if c == 0xFE:
            return p - o
        if c > 0x100 or n > 0x80:
            return 0
    return 0


def _code_at(d, o, end):
    p = o
    while p + 2 <= end:
        op = struct.unpack_from("<H", d, p)[0]
        x = ops().get(op)
        if x is None or p + x.size > end:
            # code that stops before zero bytes up to the next reached byte or the file end (scr 0218 @0x01B3)
            return p - o if p > o and not any(d[p:end]) else 0
        p += x.size
        if op in FLOW_TERM or x.end == "E" or (p == end and end < len(d)):
            return p - o  # ends in a terminator, or falls through into reached code
    return p - o if p > o and not any(d[p:end]) else 0


@cache
def get_script(f):
    return Script(f)


# ----------------------------------------------------------------------------------------------
# Rendering
# ----------------------------------------------------------------------------------------------
MSG_ARG = {0x034: 0, 0x038: 0, 0x03C: 1, 0x03D: 1, 0x043: 0, 0x048: 1, 0x049: 1, 0x03A: 0, 0x03B: 0, 0x033: 0}
OBJ_ARG = {0x064: 0, 0x06B: 0, 0x06C: 0, 0x06D: 0}
WARPS = {0x0BE, 0x0BF, 0x0C0, 0x0C1, 0x0C2, 0x0C4}


def fmt_val(k, v, labels):
    if k in "omd":
        return labels.get(v, f"@0x{v:04X}")
    if k == "p" or (k == "v" and v >= 0x4000):
        return f"var 0x{v:X}"
    return f"0x{v:X}" if v > 9 else str(v)


def lit(k, v, known):
    """The value of a v argument when it is a literal or a var just set by SetVar."""
    if k in "hbw" or (k == "v" and v < 0x4000):
        return v
    if k == "v" and v in known:
        return known[v]
    return None


class Annotator:
    """Per-script state: the stack of pushed expressions, vars set to literals."""

    def __init__(self, bank, zone):
        self.bank, self.zone = bank, zone
        self.reset()

    def reset(self):
        self.stack, self.known = [], {}

    def note(self, ins, labels):
        op, a = ins.op, ins.args
        L = [lit(k, v, self.known) for k, v in a]
        notes = []
        if op == 0x008:
            self.stack.append(str(a[0][1]))
        elif op == 0x009:
            k, v = a[0]
            self.stack.append(f"var 0x{v:X}" if v >= 0x4000 else str(v))
        elif op == 0x010:
            k, v = a[0]
            self.stack.append(f"flag 0x{v:X}" if v < 0x4000 else f"flag[var 0x{v:X}]")
        elif op == 0x011:
            r = self.stack.pop() if self.stack else "?"
            l_ = self.stack.pop() if self.stack else "?"
            c = CONDS.get(a[0][1], f"cond{a[0][1]}")
            if c == "==" and r == "1" and l_.startswith("flag"):
                e = f"{l_} set"
            elif c == "==" and r == "0" and l_.startswith("flag"):
                e = f"{l_} clear"
            else:
                e = f"({l_} {c} {r})" if c in ("or", "and") else f"{l_} {c} {r}"
            self.stack.append(e)
        elif op in (0x01F, 0x020):
            c = a[0][1]
            verb = "goto" if op == 0x01F else "call"
            if c == 0xFF:
                e = self.stack.pop() if self.stack else "?"
                notes.append(f"unless {e}: {verb} {labels.get(a[1][1], hex(a[1][1]))}")
            else:
                notes.append(f"if last compare {CONDS.get(c, c)}: {verb}")
        elif op in (0x00A, 0x00B) and self.stack:
            self.stack.pop()
        elif op in (0x00C, 0x00D, 0x00E, 0x00F) and len(self.stack) >= 2:
            r, l_ = self.stack.pop(), self.stack.pop()
            self.stack.append(f"({l_} {'+-*/'[op - 0x00C]} {r})")
        if op in (0x023, 0x024, 0x025):
            k, v = a[0]
            notes.append(f"flag 0x{v:X}" if v < 0x4000 else f"flag in var 0x{v:X}")
        if op == 0x085 and L[0] is not None:
            notes.append(trainer_summary(L[0]))
            if L[1]:
                notes.append("with " + trainer_summary(L[1]))
        if op == 0x10C and L[1] is not None:
            notes.append(f"{species_name(L[1])}" + (f" lv {L[3]}" if L[3] is not None else ""))
        if op == 0x057 and L[1] is not None:
            notes.append(species_name(L[1]))
        if op in WARPS and L[0] is not None:
            notes.append(zone_label(L[0]))
        if op in (0x01B, 0x01C) and L[0] is not None:
            notes.append(describe_sid(L[0], self.zone))
            if L[0] == 2805 and 0x8000 in self.known:
                notes.append(f"give item {item_name(self.known[0x8000])} x{self.known.get(0x8001, '?')}")
        if op == 0x0E0:
            notes.append(f"Black 21, White 20 (this ROM: {VERSION_ID[rom().code]})")
        if op in OBJ_ARG and L[OBJ_ARG[op]] is not None:
            o = L[OBJ_ARG[op]]
            notes.append("the player" if o == 0xFF else f"object {o}")
        if op in MSG_ARG and L[MSG_ARG[op]] is not None:
            i = L[MSG_ARG[op]]
            if op in (0x048, 0x049):
                j = L[2]
                names = ("male", "female") if op == 0x048 else ("White", "Black")
                for nm, x in zip(names, (i, j)):
                    t = msg_text(self.bank, x, 90) if x is not None else None
                    notes.append(f"{nm}: msg {self.bank} #{x}" + (f' "{t}"' if t is not None else ""))
            else:
                t = msg_text(self.bank, i, 90)
                notes.append(f"msg {self.bank} #{i}" + (f' "{t}"' if t is not None else ""))
        # vars set to literals stay known until something writes them
        if op == 0x028:
            self.known[a[0][1]] = a[1][1]
        elif op == 0x02A and L[1] is not None:
            self.known[a[0][1]] = L[1]
        else:
            for k, v in a:
                if k == "p":
                    self.known.pop(v, None)
        return "; ".join(notes)


def render(sc, bank, zone, out):
    order = sorted(set(sc.insts) | set(sc.movements) | set(sc.data))
    ann = Annotator(bank, zone)
    prev_end = None
    for off in order:
        if prev_end is not None and off != prev_end:
            out.append("")
        if off in sc.labels:
            idx = sc.entry_index(off)
            out.append(f"{sc.labels[off]}:" + (f"   ; header entries {idx}" if len(idx) > 1 else ""))
            ann.reset()
        if off in sc.insts:
            ins = sc.insts[off]
            o = ops()[ins.op]
            args = ", ".join(fmt_val(k, v, sc.labels) for k, v in ins.args)
            note = ann.note(ins, sc.labels)
            out.append(f"  @0x{off:04X}  {ins.op:03X} {o.name}" + (f" {args}" if args else "")
                       + (f"   ; {note}" if note else ""))
            prev_end = off + ins.size
            if ins.op in (0x004, 0x01C, 0x020):
                ann.known = {}
        elif off in sc.data:
            recs = sc.data[off]
            out.append("  " + " ".join("{" + ",".join(f"0x{v:X}" for v in r) + "}" for r in recs)
                       + f" 0xFFFF   ; data list @0x{off:04X} (five u16 per record; 0xFFFF ends)")
            prev_end = off + 10 * len(recs) + 2
        else:
            steps = sc.movements[off]
            out.append("  " + " ".join(f"{c:02X}x{n}" if c != 0xFE else "end" for c, n in steps)
                       + f"   ; movement list @0x{off:04X} (u16 command, u16 count; 0xFE ends)")
            prev_end = off + 4 * len(steps)


# ----------------------------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------------------------
def resolve_zone(arg):
    try:
        z = int(arg, 10) if arg.isdigit() else int(arg, 0)
    except ValueError:
        hits = [x["id"] for x in zones() if arg.lower() in zone_name(x["id"]).lower()]
        if not hits:
            raise SystemExit(f"no zone named like {arg!r}")
        if len(hits) > 1:
            print(f"{arg!r}: zones {' '.join(map(str, hits))}; using {hits[0]}", file=sys.stderr)
        z = hits[0]
    if not 0 <= z < len(zones()):
        raise SystemExit(f"zone {z} out of range (0..{len(zones()) - 1})")
    return z


def cmd_zone(args):
    if args.zone is None:
        for z in zones():
            print(f"{z['id']:3d} {zone_name(z['id']):24s} scr {z['scripts']:04d} level {z['level']:04d}"
                  f" text {z['text']:3d} events {z['events']:3d}")
        return
    z = zones()[resolve_zone(args.zone)]
    print(f"zone {z['id']}: {zone_name(z['id'])} (sysmsg 89 #{z['name']})")
    print(f"  scripts scr {z['scripts']:04d}; level scripts scr {z['level']:04d}; text msg {z['text']};"
          f" events zone_event {z['events']}; header u16s {list(z['raw'])}")


def cmd_events(args):
    z = resolve_zone(args.zone)
    ev = events(z)
    print(f"zone_event {z} ({zone_name(z)}):")
    for k, b in enumerate(ev["bg"]):
        print(f"  bg {k}: ({b['x']},{b['z']}) y {b['y']} kind {b['kind']} -> {describe_sid(b['script'], z)}")
    for k, o in enumerate(ev["objects"]):
        print(f"  object {k}: id {o['id']} gfx 0x{o['gfx']:X} at ({o['x']},{o['z']}) y {o['y'] / 4096:g} dir {o['dir']}"
              f" move {o['move']} range {o['range']} kind {o['kind']} params {list(o['params'])}"
              + (f" hidden by flag 0x{o['flag']:X}" if o['flag'] else "")
              + f" -> {describe_sid(o['script'], z)}"
              + (f"; trainer {trainer_summary(o['script'] - 3000)} sight {o['params'][0]} [INFERENCE: id = script - 3000]"
                 if o["kind"] == 1 and 3000 <= o["script"] < 5000 else ""))
    for k, w in enumerate(ev["warps"]):
        print(f"  warp {k}: tile ({w['x'] // 16},{w['z'] // 16}) (x 0x{w['x']:X} z 0x{w['z']:X} units of 1/16"
              f" tile) {w['w']}x{w['h']} -> {zone_label(w['dest'])} warp {w['dest_warp']} (bytes {w['a']},{w['b']})")
    for k, t in enumerate(ev["triggers"]):
        print(f"  trigger {k}: ({t['x']},{t['z']}) {t['w']}x{t['h']} when var 0x{t['var']:X} == {t['value']}"
              f" -> {describe_sid(t['script'], z)}")
    lv = level_scripts(z)
    for k, l in enumerate(lv):
        if l["kind"] == 1:
            for var, val, sid in l["table"]:
                print(f"  level {k} (type 1, on entry): var 0x{var:X} == {val} -> {describe_sid(sid, z)}")
        else:
            print(f"  level {k} (type {l['kind']}): {describe_sid(l['script'], z)}")
    print(f"  (level scripts: scr {zones()[z]['level']:04d})")


@cache
def level_scripts(z):
    return parse_level(rom().narc("a/0/5/7")[zones()[z]["level"]], 0)


def resolve_target(arg):
    if arg.startswith("sid:"):
        f, idx, bank, label = resolve_sid(int(arg[4:], 0))
        if f is None:
            raise SystemExit(f"script id {arg[4:]}: {label}")
        return f
    if arg.startswith("zone:"):
        return zones()[resolve_zone(arg[5:])]["scripts"]
    return int(arg, 10) if arg.isdigit() else int(arg, 0)


def cmd_script(args):
    f = resolve_target(args.target)
    _, lv = file_users()
    if f in lv and not args.force:
        z = lv[f][0]
        print(f"scr {f:04d}: level scripts of {zone_label(z)}")
        for l in level_scripts(z):
            if l["kind"] == 1:
                for var, val, sid in l["table"]:
                    print(f"  type 1: var 0x{var:X} == {val} -> {describe_sid(sid, z)}")
            else:
                print(f"  type {l['kind']}: {describe_sid(l['script'], z)}")
        return
    sc = get_script(f)
    bank = args.msg if args.msg is not None else default_bank(f)
    out = [f"scr {f:04d}: {file_label(f)}; text msg {bank}; {len(sc.entries)} entries"]
    render(sc, bank, default_zone(f), out)
    print("\n".join(out))


def script_files():
    _, lv = file_users()
    return [f for f in range(len(rom().narc("a/0/5/7"))) if f not in lv]


def cmd_coverage(args):
    """Every script file of this ROM: decode from every header entry, then account for every byte."""
    files = script_files()
    _, lv = file_users()
    tot = {"bytes": 0, "header": 0, "reached": 0, "code": 0, "movement": 0, "pad": 0, "unknown": 0}
    errors, used, ninst, unk = [], {}, 0, []
    for f in files:
        try:
            sc = get_script(f)
        except DisasmError as e:
            errors.append(str(e))
            continue
        cov = sc.covered()
        tot["bytes"] += len(sc.d)
        tot["header"] += sc.header_end
        tot["reached"] += sum(cov) - sc.header_end
        ninst += len(sc.insts)
        for i in sc.insts.values():
            used[i.op] = used.get(i.op, 0) + 1
        r, u = sweep(sc)
        for k, v in r.items():
            tot[k] += v
        unk += [(f, a, b) for a, b in u]
    lvl_bad = 0
    for f in lv:
        d = rom().narc("a/0/5/7")[f]
        try:
            parse_level(d, 0)
        except struct.error:
            lvl_bad += 1
    print(f"{rom().game} ({rom().code}): a/0/5/7 has {len(rom().narc('a/0/5/7'))} members: {len(files)} script"
          f" files, {len(lv)} level-script files ({lvl_bad} malformed)")
    print(f"decoded {len(files) - len(errors)} script files: {ninst} instructions, {len(used)} distinct opcodes"
          f" of {len(ops())} in the table; decode errors: {len(errors)}")
    print(f"bytes: {tot['bytes']} = header {tot['header']} + reached code/movement {tot['reached']}"
          f" + unreachable code {tot['code']} + unreachable movement lists {tot['movement']}"
          f" + zero padding {tot['pad']} + unknown {tot['unknown']}")
    for e in errors:
        print("  ! " + e)
    for f, a, b in unk:
        print(f"  ? scr {f:04d} @0x{a:04X}-0x{b - 1:04X}: {rom().narc('a/0/5/7')[f][a:b][:24].hex(' ')}")
    if args.verbose:
        for op in sorted(used):
            print(f"  {op:03X} {ops()[op].name:24s} {used[op]}")
    if errors:
        sys.exit(1)


def iter_insts():
    for f in script_files():
        sc = get_script(f)
        for off in sorted(sc.insts):
            yield f, sc.insts[off]


def cmd_grep_flag(args):
    flag = int(args.flag, 0)
    for f, ins in iter_insts():
        if ins.op in (0x023, 0x024, 0x025, 0x010) and ins.args[0][1] == flag:
            print(f"  scr {f:04d} @0x{ins.off:04X} {ops()[ins.op].name}   [{file_label(f)}]")
    for z in range(len(zones())):
        for k, o in enumerate(events(z)["objects"]):
            if o["flag"] == flag:
                print(f"  zone_event {z} object {k} (gfx 0x{o['gfx']:X}) hidden by it   [{zone_label(z)}]")


def cmd_grep_var(args):
    var = int(args.var, 0)
    for f, ins in iter_insts():
        if any(k in "pv" and v == var for k, v in ins.args):
            a = ", ".join(fmt_val(k, v, {}) for k, v in ins.args)
            print(f"  scr {f:04d} @0x{ins.off:04X} {ops()[ins.op].name} {a}   [{file_label(f)}]")
    for z in range(len(zones())):
        for k, t in enumerate(events(z)["triggers"]):
            if t["var"] == var:
                print(f"  zone_event {z} trigger {k} ({t['x']},{t['z']}) == {t['value']}   [{zone_label(z)}]")
        for l in level_scripts(z):
            for v_, val, sid in l.get("table", []):
                if v_ == var:
                    print(f"  zone {z} level type 1: == {val} -> {describe_sid(sid, z)}   [{zone_label(z)}]")


def cmd_grep_op(args):
    want = int(args.op, 16) if re.fullmatch(r"(0x)?[0-9A-Fa-f]{1,3}", args.op) else None
    for f, ins in iter_insts():
        o = ops()[ins.op]
        if ins.op == want or o.name.lower() == args.op.lower():
            a = ", ".join(fmt_val(k, v, {}) for k, v in ins.args)
            print(f"  scr {f:04d} @0x{ins.off:04X} {ins.op:03X} {o.name} {a}   [{file_label(f)}]")


def cmd_trainer(args):
    t = int(args.trainer, 0)
    tr = trainer(t)
    print(trainer_summary(t))
    for m in tr["mons"]:
        print(f"  {species_name(m['species'])} lv {m['level']}"
              + (f" @{item_name(m['item'])}" if m.get("item") else "")
              + (f" moves {[sys_name(203, x) for x in m['moves'] if x]}" if m.get("moves") else ""))
    if tr["items"]:
        print("  items: " + ", ".join(item_name(i) for i in tr["items"]))
    if args.where:
        for f, ins in iter_insts():
            if ins.op == 0x085 and t in (ins.args[0][1], ins.args[1][1]):
                print(f"  scr {f:04d} @0x{ins.off:04X} TrainerBattle   [{file_label(f)}]")


def cmd_text(args):
    b = story_bank(args.bank) if not args.sys else sys_bank(args.bank)
    for i, s in enumerate(b):
        if args.idx is None or i == args.idx:
            print(f"#{i}: {s}")


def cmd_text_grep(args):
    rx = re.compile(args.regex, re.I)
    narc = "a/0/0/2" if args.sys else "a/0/0/3"
    for i in range(len(rom().narc(narc))):
        for j, s in enumerate(sys_bank(i) if args.sys else story_bank(i)):
            if rx.search(s):
                print(f"{'sysmsg' if args.sys else 'msg'} {i} #{j}: {s}")


def cmd_opcodes(args):
    for op, o in sorted(ops().items()):
        print(f"{op:03X} {o.name:22s} {o.layout or '.':10s} {o.handler}{' ' + o.end if o.end else ''}"
              + (f"   [{o.basis}] {o.note}" if o.basis else ""))


def cmd_resolve(args):
    print(describe_sid(int(args.sid, 0)))


def cmd_version_diff(args):
    """Black vs White: which ROM files differ, and the scripts that branch on the version."""
    other = Rom(args.other)
    for path in ("a/0/1/2", "a/1/2/5", "a/0/5/7", "a/0/0/3", "a/0/0/2", "a/0/9/2", "a/0/9/3"):
        a, b = rom().narc(path), other.narc(path)
        diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
        print(f"{path}: {len(a)} / {len(b)} members, {len(diff)} differ {diff[:20]}")
    print("scripts reading the version (0xE0 GetVersion) or picking a message by it (0x49):")
    for f, ins in iter_insts():
        if ins.op in (0x0E0, 0x049):
            print(f"  scr {f:04d} @0x{ins.off:04X} {ops()[ins.op].name}   [{file_label(f)}]")


# ----------------------------------------------------------------------------------------------
# derive: the command table from the ROM's handler table and the generated assembly
# ----------------------------------------------------------------------------------------------
def handler_table():
    t = TABLES[rom().code]
    n = rom().ov10_u32(t["count"])
    return [rom().ov10_u32(t["handlers"] + 4 * k) for k in range(n)]


def cmd_derive(args):
    import bw_derive  # tests/e2e/tools/bw_derive.py
    asm = args.asm or str(REPO / "games" / "ndsrec" / "build" / "pc-wasm" / rom().game / "ndsrec" / "asm" / "arm9")
    rows = bw_derive.derive(asm, handler_table(), rom().code)
    if args.emit:
        print("\n".join(rows))
        return
    want = {r.split()[0]: r for r in OPTABLE.strip().splitlines()}
    got = {r.split()[0]: r for r in rows}
    bad = [k for k in sorted(set(want) | set(got)) if want.get(k, "").split()[1:2] + want.get(k, "").split()[3:]
           != got.get(k, "").split()[1:2] + got.get(k, "").split()[3:]]
    print(f"{rom().game}: {len(got)} handlers derived from {asm}; layouts differing from OPTABLE: {len(bad)}")
    for k in bad:
        print(f"  OPTABLE {want.get(k)!r} vs derived {got.get(k)!r}")
    if bad:
        sys.exit(1)


def main(argv=None):
    global ROM
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", choices=("black", "white"), default="black")
    ap.add_argument("--rom", help="the cartridge (default: $NP_BW_ROMS or <repo>/roms, the run.py name)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("zone", help="zone header(s): scripts, level scripts, text bank, events, name")
    s.add_argument("zone", nargs="?", help="zone id or a name substring")
    s.set_defaults(fn=cmd_zone)
    s = sub.add_parser("events", help="bg events, objects, warps, triggers and level scripts of a zone")
    s.add_argument("zone")
    s.set_defaults(fn=cmd_events)
    s = sub.add_parser("script", help="disassemble an a/0/5/7 member: N, zone:Z, or sid:<script id>")
    s.add_argument("target")
    s.add_argument("--msg", type=int, help="text bank used to inline messages")
    s.add_argument("--force", action="store_true", help="decode a level-script file as commands")
    s.set_defaults(fn=cmd_script)
    s = sub.add_parser("trainer", help="a trainer's party (trdata id)")
    s.add_argument("trainer")
    s.add_argument("--where", action="store_true", help="also list the TrainerBattle commands using it")
    s.set_defaults(fn=cmd_trainer)
    s = sub.add_parser("grep-flag", help="every script/object using a flag")
    s.add_argument("flag")
    s.set_defaults(fn=cmd_grep_flag)
    s = sub.add_parser("grep-var", help="every script/trigger/level script using a var")
    s.add_argument("var")
    s.set_defaults(fn=cmd_grep_var)
    s = sub.add_parser("grep-op", help="every use of an opcode (hex id or name)")
    s.add_argument("op")
    s.set_defaults(fn=cmd_grep_op)
    s = sub.add_parser("text", help="print a message bank (a/0/0/3; --sys for a/0/0/2)")
    s.add_argument("bank", type=int)
    s.add_argument("idx", type=int, nargs="?")
    s.add_argument("--sys", action="store_true")
    s.set_defaults(fn=cmd_text)
    s = sub.add_parser("text-grep", help="search every message bank (regex, case-insensitive)")
    s.add_argument("regex")
    s.add_argument("--sys", action="store_true")
    s.set_defaults(fn=cmd_text_grep)
    s = sub.add_parser("opcodes", help="the command table: layout, handler, name and its basis")
    s.set_defaults(fn=cmd_opcodes)
    s = sub.add_parser("coverage", help="decode every script file; account for every byte; fail on errors")
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(fn=cmd_coverage)
    s = sub.add_parser("resolve", help="where a script id lives")
    s.add_argument("sid")
    s.set_defaults(fn=cmd_resolve)
    s = sub.add_parser("version-diff", help="this ROM vs the other version's ROM")
    s.add_argument("other", help="the other ROM")
    s.set_defaults(fn=cmd_version_diff)
    s = sub.add_parser("derive", help="re-derive the command table from the ROM + generated asm; diff OPTABLE")
    s.add_argument("--asm", help="the recompiler's arm9 asm dir")
    s.add_argument("--emit", action="store_true", help="print the derived table in OPTABLE's format")
    s.set_defaults(fn=cmd_derive)
    args = ap.parse_args(argv)
    ROM = Rom(args.rom or default_rom(args.game))
    args.fn(args)


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
