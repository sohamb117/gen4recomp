import sys
from collections import deque
L = open('/tmp/vg.txt').read().split('\n')
g = {}
for l in L[2:66]:
    try: z = int(l[:5])
    except ValueError: continue
    for i, c in enumerate(l[6:70]): g[(i - 20, z)] = c
OX, OZ = int(sys.argv[1]) if len(sys.argv) > 1 else 0, int(sys.argv[2]) if len(sys.argv) > 2 else 0
# obstacle i: row z, state segments (overlay_06.s ov06_0224F8B8 / ov06_0224F918), initial UNK_020F7E48
ROW = [9, 10, 10, 10, 12, 13, 14, 16, 16, 18, 19, 20]
SEG = [([(9,2),(12,1)],[(10,2),(13,1)]), ([(3,1),(5,2)],[(4,1),(6,2)]), ([(11,1),(13,2)],[(12,1),(14,2)]),
       ([(18,1),(20,1)],[(19,1),(21,1)]), ([(4,1)],[(5,4)]), ([(16,4)],[(20,1)]), ([(19,4)],[(22,1)]),
       ([(4,2),(7,1)],[(7,2),(10,1)]), ([(16,2),(19,1)],[(17,2),(20,1)]), ([(4,1)],[(5,4)]),
       ([(19,4)],[(22,1)]), ([(3,4)],[(8,4)])]
INIT = (0,0,1,1,1,0,0,1,0,1,0,1)
PUSH = [(12,20,1,11),(2,20,0,11),(18,19,0,10),(9,18,1,9),(21,16,1,8),(15,16,0,8),(11,16,1,7),(18,14,0,6),
        (15,13,0,5),(9,12,1,4),(22,10,1,3),(16,10,1,2),(2,10,0,1),(8,9,0,0)]
TRAINERS = {(5,19),(12,13),(12,19),(19,18),(13,24)}
D = {"UP": (0,-1), "DOWN": (0,1), "LEFT": (-1,0), "RIGHT": (1,0)}

def bar_blocked(x, z, st):
    tx, tz = x + OX, z + OZ
    for i in range(12):
        if ROW[i] == tz:
            for a, n in SEG[i][st[i]]:
                if a <= tx < a + n:
                    return True
    return False

def free(x, z, st):
    c = g.get((x, z))
    return c is not None and c not in '#?o' and (x, z) not in TRAINERS and 3 <= z <= 25 and not bar_blocked(x, z, st)

def moves(pos, st):
    x, z = pos
    for k, (dx, dz) in D.items():
        n = (x + dx, z + dz)
        if free(n[0], n[1], st):
            yield k, n, st
        else:  # a bump: push?
            for px, pz, kind, i in PUSH:
                if (px, pz) == (x + OX, z + OZ) and ((kind and k == "LEFT" and st[i] == 1) or (not kind and k == "RIGHT" and st[i] == 0)):
                    ns = list(st); ns[i] ^= 1; ns = tuple(ns)
                    yield "PUSH" + k, pos, ns

start = ((12, 25), INIT); goal = (12, 5)
prev = {start: None}; q = deque([start])
while q:
    cur = q.popleft()
    if cur[0] == goal:
        path = []
        while prev[cur]:
            cur, k = prev[cur]; path.append(k)
        path.reverse()
        print(len(path), " ".join(path)); break
    for k, n, st in moves(*cur):
        if (n, st) not in prev:
            prev[(n, st)] = (cur, k); q.append((n, st))
else:
    print("no path", len(prev))
