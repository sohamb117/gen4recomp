#!/usr/bin/env python3
"""Drive the station corpus: mint each recipe, boot it, and check what happened.

    $ python3 pc/tests/pc_corpus.py                 # every station
    $ python3 pc/tests/pc_corpus.py jubilife league # some of them
    $ python3 pc/tests/pc_corpus.py --pin           # rewrite the pinned digests

A station is a `.recipe` (what save to mint, in the game's own names) beside a
`.spec` (how to drive it and what must be true afterwards). Both are text and
neither is a save: the save is regenerated from the recipe every time, so
nothing binary is ever committed and a station cannot drift from the code that
made it.

The spec is `key: value` lines. Everything is optional except `frames`:

    input:   pc/replays/lab-continue.txt   the script both this and the oracle drive
    frames:  3200                          how long the run is
    digest:  ABCDEF0123456789              the final frame, pinned
    map:     3                             what the save must say afterwards
    money:   12345
    badges:  1
    party:   2
    name:    JUBILIF
    rtc:     2009-03-22 21:00:00           re-base the deterministic clock
    rtc2:    2009-03-22 06:00:00           a second boot, same save, other hour
    digest2: ABCDEF0123456789              the final frame of that second boot
    poketch: app=POKETCH_APPID_CALCULATOR  what the lower screen is showing
    friendship: 0=255                      a party slot's friendship byte
    battle:  wild 25 5                     start this fight once the field is up
    battle-at: 2600                        at this frame
    battle2: wild 16 5                     a second fight after the first ends
    battle2-at: 5200                       at this frame (default: field settles)
    result:  1                             and the resultMask it must end with
    save-at: 4200                          write the save again at this frame
    pos:     304,520                       where the player must be standing
    used:    LOCATION_EVENT_USED_CUT       what the game recorded doing
    flag:    FLAG_X=0                      what the clock's daily events left
    var:     VAR_X=1234                    behind, by the game's own names
    honey:   0=1080                        minutes left on honey tree 0
    berry:   0.growthStage=2               one member of one berry patch
    form:    0=SHAYMIN_FORM_LAND           a party slot's form
    use:     0 ITEM_RARE_CANDY             use a bag item on a party member
    use-at:  3200                          at this frame
    species: 0=SPECIES_METAPOD             what a party slot IS afterwards
    egg:     1=0                           and whether it is still an egg
    dex:     SPECIES_METAPOD=caught        caught, seen or no
    daycare: state=DAYCARE_EGG_WAITING     what the day care holds
    move:    0.0=MOVE_THUNDERBOLT   a party slot's move slot, after a TM
    option:  textSpeed=OPTIONS_TEXT_SPEED_FAST   what the options screen wrote
    box:     stored=2 0.0=SPECIES_TURTWIG   what is in the PC's boxes
    poffin:  filled=1 0.type=POFFIN_TYPE_FOUL   what came out of the cooking pot
    contest: CONTEST_RANK_NORMAL CONTEST_TYPE_COOL 0  enter this Super Contest
    contest-at: 2600                        at this frame, with that party slot
    placement: 1                            and the place it must finish in
    condition: 0.cool=60 0.sheen=30         what a poffin left on the entry
    ribbon:  0.MON_DATA_SUPER_COOL_RIBBON=1   what winning one leaves behind
    underground: enter                      after the Explorer Kit arrives
    underground-at: 5600                    skip Roark; `mine` starts the dig
    sphere:  filled=1 0.type=SPHERE_PRISM   the Underground bags
    trap:    filled=1 0.id=TRAP_SMOKE
    good:    filled=1 0.id=UG_GOOD_TV
    secret-base: active=1 x=72 z=437        a captured base's entrance

A station with a `battle`, A `use` OR A `save-at` is checked after the run, not
BEFORE. The port writes the save again, when the battle ends, or at the frame
`save-at` names, so the party HP, the experience, the money, the dex and the
player's position in it are what the run DID rather than what it started from.
A station with none of them is checked before it boots, where the save is all
there is.

`used` is the one assert a digest cannot make, and it is why a field-move
station is worth having. Every field move ends its script by recording a
journal event, so `used` names the events the run must produce; a station whose
player walked past its tree draws the same overworld, pins the same kind of
digest and would otherwise pass. `pos` is the other half: the tile the player
ends on is the proof the obstacle is gone, the water was surfed onto or the
boulder moved.

Why a digest is not enough, and why every spec also asserts through the save
reader: a digest that is stable and wrong passes exactly as quietly as one that
is right. The two walls the save lab hit both left every field in the save
correct and only showed up on the screen, and a station that checked only the
screen would have the mirror-image blind spot.

A pinned digest is only believed after two identical runs and one under an
empty environment, which is the standard the state digest is held to. `--pin`
does all three before it writes anything.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS = os.path.join(ROOT, "pc", "tests", "corpus")
BINARY = os.environ.get("PC_BIN") or os.path.join(ROOT, "build", "pc", "pokeplatinum")
LABDIR = os.path.join(ROOT, "build", "pc", "lab")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_lab   # noqa: E402
import pc_save  # noqa: E402

DEFAULTS = {"input": "pc/replays/lab-continue.txt", "frames": "3200"}


def stations():
    return sorted(f[:-len(".recipe")] for f in os.listdir(CORPUS)
                  if f.endswith(".recipe"))


def read_spec(name):
    spec = dict(DEFAULTS)
    path = os.path.join(CORPUS, name + ".spec")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line or ":" not in line:
                    continue
                k, v = line.split(":", 1)
                spec[k.strip()] = v.strip()
    return spec


def write_spec(name, spec, note=None):
    path = os.path.join(CORPUS, name + ".spec")
    order = ["input", "frames", "rtc", "rtc2", "rtc-diff", "battle", "battle-at",
             "battle2", "battle2-at", "use", "use-at",
             "contest", "contest-at", "underground", "underground-at",
             "save-at", "digest", "digest2", "result",
             "placement", "ribbon", "condition", "used", "fished", "flag", "var",
             "honey", "berry", "form", "species", "egg", "move", "dex",
             "daycare", "box", "poffin", "option",
             "sphere", "trap", "good", "secret-base",
             "poketch", "friendship",
             "name", "map", "pos", "money",
             "badges", "party"]
    with open(path, "w") as f:
        if note:
            f.write("# %s\n#\n" % note)
        f.write("# Pinned after two identical runs and one under an empty\n"
                "# environment. Regenerate with pc/tests/pc_corpus.py --pin.\n")
        for k in order:
            if k in spec:
                f.write("%-8s %s\n" % (k + ":", spec[k]))
        for k in sorted(set(spec) - set(order)):
            f.write("%-8s %s\n" % (k + ":", spec[k]))


COVDIR = os.path.join(ROOT, "build", "pc", "cov")


def boot(sav, spec, out, env=None, cov=None):
    """Run the port from a station save; return (digest, why, stderr).

    `cov` names a file the port appends its script coverage to. It is off by
    default and switched on by the caller that wants it, because a pin measures
    the same station three times and coverage counted three times is not three
    times the coverage; the merge tool would sum it and the number would be
    honest but useless.

    The stderr is returned rather than stashed on the function. It used to be
    `boot.last_stderr`, which is one slot shared by every caller, and that had
    two costs. It made run_station unsafe to run concurrently, and the four
    fast stations are independent runs that the suite was driving one after
    another, which is why test_corpus was the longest test in the suite and
    therefore the whole suite's wall clock. It was also already subtly wrong
    under --pin: the reproducibility boots overwrite the slot, so the battle
    result recorded at pin time was read out of the `bare` run's stderr rather
    than the run whose digest was being pinned.
    """
    base = env if env is not None else \
        {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    frames = spec["frames"]
    e = dict(base)
    e.update({"PC_SAVE": sav, "PC_FRAMES": frames, "PC_PACE": "0",
              "PC_INPUT": os.path.join(ROOT, spec["input"]),
              "PC_DUMP_FRAMES": out, "PC_DUMP_FROM": frames})
    if cov:
        e["PC_SCRIPT_COV"] = cov
    if spec.get("rtc"):
        # The clock is an input, so a time of day is a
        # station's property like its map is. Minting is unaffected: what the
        # save holds does not depend on the hour, only what the run draws does.
        e["PC_RTC"] = spec["rtc"]
    if spec.get("battle"):
        e["PC_LAB_BATTLE"] = spec["battle"]
        e["PC_LAB_BATTLE_AT"] = spec.get("battle-at", "2600")
        e["PC_LAB_BATTLE_SAVE"] = "1"
    if spec.get("battle2"):
        e["PC_LAB_BATTLE2"] = spec["battle2"]
        if spec.get("battle2-at"):
            e["PC_LAB_BATTLE2_AT"] = spec["battle2-at"]
    if spec.get("contest"):
        # Written by the game's own names and resolved here, like `use`.
        e["PC_LAB_CONTEST"] = " ".join(
            str(pc_lab.resolve(t, "spec contest:")) for t in spec["contest"].split())
        e["PC_LAB_CONTEST_AT"] = spec.get("contest-at", "2600")
    if spec.get("underground"):
        e["PC_LAB_UNDERGROUND"] = spec["underground"]
        e["PC_LAB_UNDERGROUND_AT"] = spec.get("underground-at", "5600")
    if spec.get("save-at"):
        e["PC_LAB_SAVE_AT"] = spec["save-at"]
    if spec.get("use"):
        # Written by name in the spec and resolved here, the way a recipe's
        # arguments are: `use: 0 ITEM_RARE_CANDY` rather than `0 50`.
        e["PC_LAB_USE_ITEM"] = " ".join(
            str(pc_lab.resolve(t, "spec use:")) for t in spec["use"].split())
        e["PC_LAB_USE_AT"] = spec.get("use-at", "3200")
    # Always on. It is stderr only; the run it describes is byte-identical
    # with and without it, and a station that forgot to ask for it would be
    # a station whose `used` assert quietly had nothing to read.
    e["PC_TRACE_JOURNAL"] = "1"
    # Same bargain for the fishing trace, which is what a `fished` assert reads.
    e["PC_TRACE_FISH"] = "1"
    r = subprocess.run([BINARY], env=e, capture_output=True, text=True,
                       timeout=3600)
    if r.returncode != 0:
        return None, "the port exited %d: %s" % (r.returncode, r.stderr[-400:]), r.stderr
    manifest = os.path.join(out, "frames.txt")
    if not os.path.exists(manifest):
        return None, "no frame manifest", r.stderr
    with open(manifest) as f:
        for line in f:
            cols = line.split()
            if cols and cols[0].lstrip("0") in (frames.lstrip("0"), ""):
                if cols[0] == frames.zfill(6):
                    return cols[-1], None, r.stderr
    return None, "frame %s was never dumped" % frames, r.stderr


def boot_rtc2(sav, spec, out, env=None):
    """The same station at the other clock. Display-only: no end-of-run save.

    A clock app that actually draws the time produces a different digest at
    06:00 than at 21:00; one that does not (a kitchen timer sitting at 0:00)
    produces the same digest twice. Both answers are pins. The save is not
    rewritten so the first boot's semantic asserts stay the ones that count.
    """
    spec2 = dict(spec)
    spec2["rtc"] = spec["rtc2"]
    spec2.pop("save-at", None)
    return boot(sav, spec2, out, env=env)


def check_save(name, sav, spec):
    """Every semantic assert the spec carries, through the save reader."""
    save = pc_save.Save(sav)
    problems = []
    if save.result != "ok":
        problems.append("the save arbitrates as %s (%s)" % (save.result, save.errors))
    have = {"map": save.position[0], "money": save.money, "badges": save.badges,
            "party": len(save.party), "name": save.name,
            # The tile the run ended on, which for a field-move station is the
            # whole claim: on the far side of a tree that was cut, on water
            # that was surfed onto, past a boulder that moved.
            "pos": "%d,%d" % (save.position[1], save.position[2])}
    for key, value in have.items():
        if key not in spec:
            continue
        want = spec[key]
        if key not in ("name", "pos"):
            want = int(want, 0)
        if value != want:
            problems.append("%s is %r, the spec says %r" % (key, value, want))
    for i, mon in enumerate(save.party):
        if not mon["checksum_ok"]:
            problems.append("party slot %d fails its own checksum" % i)
    problems += check_state(spec, save)
    return save, problems


def _pairs(spec, key):
    """`k=v k=v` out of one spec line, with the value resolved by name."""
    for field in spec[key].replace(",", " ").split():
        if "=" not in field:
            raise SystemExit("pc_corpus: %s: %r is not k=v" % (key, field))
        k, v = field.split("=", 1)
        yield k, pc_lab.resolve(v, "spec %s:" % key)


def check_state(spec, save):
    """The clock-driven state a station asserts, through the save reader.

    This is what a daily or per-minute system leaves behind, and none of it is
    on screen. The field runs sub_020559DC on every map load, works out how
    long the save has been shut, and hands the difference to the daily events
    (which clear the daily flags and re-roll the lottery) and to the per-minute
    ones (berry growth, honey-tree countdown, Shaymin's form). Every one of
    them writes into the save and nothing else, so a station that boots an old
    save on a later clock and reads these back observes the system directly,
    no menu, no conversation, nothing pressed.

    Flags, vars and forms are written by the game's own names on both sides;
    `berry` takes `patch.member`, `honey` a tree index and `form` a party slot.
    """
    problems = []
    for key, get, what in (
        ("flag", lambda k: int(save.flag(pc_lab.resolve(k, "spec flag:"))),
         "flag %s"),
        ("var", lambda k: save.var(pc_lab.resolve(k, "spec var:")), "var %s"),
        ("honey", lambda k: save.honey_trees[int(k, 0)]["minutesRemaining"],
         "honey tree %s"),
        ("form", lambda k: save.party[int(k, 0)]["form"], "party slot %s form"),
        ("berry", lambda k: save.berry_patches[int(k.split(".")[0], 0)][k.split(".")[1]],
         "berry patch %s"),
        # The growth loop. `species` and `egg` are what an evolution or a hatch
        # DID, the one thing a digest of the overworld afterwards cannot see,
        # because the player is standing where they were with the same picture
        # around them either way.
        ("species", lambda k: save.party[int(k, 0)]["species"],
         "party slot %s species"),
        ("egg", lambda k: int(save.party[int(k, 0)]["is_egg"]),
         "party slot %s egg flag"),
        # `slot.moveslot`, which is what a TM teaching leaves behind and the
        # only place the move-overwrite prompt's answer is visible: the party
        # menu draws the same four rows whichever move was replaced.
        ("move", lambda k: save.party[int(k.split(".")[0], 0)]["moves"][
            int(k.split(".")[1], 0)], "party slot %s move"),
        # The options screen writes nothing else and draws nothing that
        # survives it, so this is the whole assert for that station.
        ("option", lambda k: save.options[k], "the %s option"),
        # `stored`, `current`, or `box.slot` for the species in one slot.
        # Without the last two, "the party shrank" is all a deposit station
        # would claim, and releasing a Pokemon shrinks it just as well.
        ("box", lambda k: (save.boxes["boxes"][int(k.split(".")[0], 0)]
                           [int(k.split(".")[1], 0)] or {"species": 0})["species"]
                          if "." in k else save.boxes[k], "the PC's %s"),
        ("daycare", lambda k: save.daycare[k], "the day care's %s"),
        # `filled`, or `slot.attribute` for one poffin's type, level or any of
        # its five flavours. The cooking application draws the poffin it made
        # on a screen the run leaves immediately, so the case is the only
        # place a station can see what an hour of stirring produced.
        ("poffin", lambda k: (save.poffins["slots"][int(k.split(".")[0], 0)]
                              [k.split(".")[1]] if "." in k
                              else save.poffins[k]), "the poffin case's %s"),
        # `slot.stat` for one party member's condition or sheen, the numbers
        # a poffin moves and the visual round is scored on.
        ("condition", lambda k: save.party[int(k.split(".")[0], 0)][k.split(".")[1]],
         "party slot %s"),
        # `slot.MON_DATA_SUPER_*_RIBBON`, one bit of block C's ribbon mask.
        # Winning a contest is the only thing that sets one, and it is the
        # only difference between a run that placed first and one that did
        # not: both end on the same map with the same party.
        ("ribbon", lambda k: (save.party[int(k.split(".")[0], 0)]["ribbons_super"]
                              >> (pc_lab.resolve(k.split(".")[1], "spec ribbon:")
                                  - pc_lab.resolve("MON_DATA_SUPER_COOL_RIBBON",
                                                   "spec ribbon:"))) & 1,
         "party slot %s"),
        # The Underground bags and the captured base. `filled` is occupied
        # slots; `slot.type` / `slot.size` is one sphere; `slot.id` is one
        # trap or good. `secret-base` is what SecretBase_SetEntrance wrote.
        ("sphere", lambda k: (save.underground["spheres"][int(k.split(".")[0], 0)]
                              [k.split(".")[1]] if "." in k
                              else save.underground["sphere_" + k]),
         "the sphere bag's %s"),
        ("trap", lambda k: (save.underground["traps"][int(k.split(".")[0], 0)]
                            if "." in k
                            else save.underground["trap_" + k]),
         "the trap bag's %s"),
        ("good", lambda k: (save.underground["goods"][int(k.split(".")[0], 0)]
                            if "." in k
                            else save.underground["good_" + k]),
         "the goods bag's %s"),
        ("secret-base", lambda k: save.underground["base"][k],
         "the secret base's %s"),
        # The Poketch page. `app` is the selected App ID, `steps` is the
        # pedometer, `alarm` / `alarm-hour` / `alarm-minute` what the alarm
        # app wrote, `color` the colour changer, `calendar-month` and
        # `calendar-marks` what a tap on the calendar left behind.
        ("poketch", lambda k: save.poketch[k], "poketch %s"),
        # A party slot's friendship byte, what the friendship checker
        # draws, and the only number that checker reads.
        ("friendship", lambda k: save.party[int(k, 0)]["friendship"],
         "party slot %s friendship"),
    ):
        if key not in spec:
            continue
        for name, want in _pairs(spec, key):
            got = get(name)
            if got != want:
                problems.append("%s is %d, the spec says %d"
                                % (what % name, got, want))

    # The dex is the other half of an evolution or a hatch, and its answer is a
    # word rather than a number: a species can be seen without being caught.
    if "dex" in spec:
        for field in spec["dex"].replace(",", " ").split():
            name, want = field.split("=", 1)
            got = save.dex(pc_lab.resolve(name, "spec dex:"))
            if got != want:
                problems.append("the dex says %s is %s, the spec says %s"
                                % (name, got, want))
    return problems


def check_used(spec, stderr):
    """The journal events the run had to produce, against the ones it did.

    Names are the game's own (LOCATION_EVENT_USED_CUT), resolved out of the
    generated headers the same way a recipe's are, so there is no second table
    of event numbers here.
    """
    want = [w for w in spec["used"].replace(",", " ").split() if w]
    got = []
    for line in stderr.split("\n"):
        if not line.startswith("pc-journal:"):
            continue
        for field in line.split():
            if field.startswith("event="):
                got.append(int(field.split("=", 1)[1]))
    problems = []
    for w in want:
        n = pc_lab.resolve(w, "spec used:")
        if n not in got:
            problems.append("the run never recorded %s, the journal events it "
                            "did record were %s" % (w, got or "none"))
    return problems


def check_fished(spec, stderr):
    """The fishing states the run had to pass through, against the ones it did.

    The words are the trace's own, wait, none, bite, caught, early, away,
    and `caught` is the one that carries the station. A rod's bite window opens
    at an RNG-chosen frame and closes 15 to 45 ticks later, so a press that
    misses it reads `early` or `away`, leaves the player standing on the same
    tile facing the same water, and draws a picture close enough to the pinned
    one that only this assert separates them.
    """
    want = [w for w in spec["fished"].replace(",", " ").split() if w]
    got = [line.split()[3] for line in stderr.split("\n")
           if line.startswith("pc-fish:")]
    return ["the run never reached `%s`; the fishing states it did reach "
            "were %s" % (w, got or "none") for w in want if w not in got]


def run_station(name, tmp, pin=False, quiet=False, cov=False):
    recipe = os.path.join(CORPUS, name + ".recipe")
    spec = read_spec(name)
    sav = os.path.join(LABDIR, name + ".sav")
    pc_lab.mint(recipe, sav)

    # A battle station is asserted AFTER its fight, and so is a station that
    # names a `save-at`: in both cases the port rewrites this same file while
    # the run goes, and that later state is the one worth checking. A station
    # with neither has nothing later to look at.
    fighting = bool(spec.get("battle"))
    if spec.get("use") and not spec.get("save-at"):
        # Without one the station would assert the save its recipe minted;
        # which is the state before the item was used, so every assert about
        # what the item DID would be checked against the wrong save and pass.
        raise SystemExit("pc_corpus: %s: a `use` station needs a `save-at`" % name)
    rewritten = fighting or bool(spec.get("save-at"))
    save, problems = (None, []) if rewritten else check_save(name, sav, spec)

    covfile = None
    if cov:
        os.makedirs(COVDIR, exist_ok=True)
        covfile = os.path.join(COVDIR, name + ".cov")
        if os.path.exists(covfile):
            os.unlink(covfile)

    digest, why, log = boot(sav, spec, os.path.join(tmp, name), cov=covfile)
    if digest is None:
        problems.append(why)
        return spec, save or pc_save.Save(sav), problems

    if fighting:
        report = [l for l in log.split("\n")
                  if l.startswith("pc_lab: battle over")]
        want_n = 2 if spec.get("battle2") else 1
        if len(report) < want_n:
            # The one failure a digest cannot see: a station whose fight never
            # started or never ended pins a picture of the overworld and passes.
            problems.append("the battle never finished, the port printed %d "
                            "'battle over' line(s) in %s frames, wanted %d"
                            % (len(report), spec["frames"], want_n))
        elif "result" in spec:
            got = report[-1].rsplit("=", 1)[-1].strip()
            if got != spec["result"]:
                problems.append("the battle ended resultMask=%s, the spec says %s"
                                % (got, spec["result"]))
    if spec.get("contest"):
        report = [l for l in log.split("\n")
                  if l.startswith("pc_lab: contest over")]
        if not report:
            # The same failure a battle station guards against: a contest that
            # never finished leaves the port drawing the stage, which digests
            # as stably as the finished one.
            problems.append("the contest never finished, the port printed no "
                            "'contest over' line in %s frames" % spec["frames"])
        elif "placement" in spec:
            got = report[-1].rsplit("=", 1)[-1].strip()
            if got != spec["placement"]:
                problems.append("the contest ended placement=%s, the spec says %s"
                                % (got, spec["placement"]))
    if spec.get("underground"):
        kind = spec["underground"]
        if kind == "mine":
            report = [l for l in log.split("\n")
                      if l.startswith("pc_lab: mining started")]
            if not report:
                problems.append("the digging minigame never started, the port"
                                " printed no 'mining started' line in %s frames"
                                % spec["frames"])
        else:
            report = [l for l in log.split("\n")
                      if l.startswith("pc_lab: underground enter")]
            if not report:
                problems.append("the underground hook never ran, the port"
                                " printed no 'underground enter' line in %s"
                                " frames" % spec["frames"])

    if rewritten:
        if spec.get("save-at") and not [
                l for l in log.split("\n")
                if l.startswith("pc_lab: end-of-run save")]:
            # Without this the station falls back to asserting the save its
            # recipe minted, which is the state it was trying to move away
            # from; it would pass by checking the wrong thing.
            problems.append("no end-of-run save was written: %s"
                            % (" / ".join(l for l in log.split("\n")
                                          if l.startswith("pc_lab:")) or "nothing said"))
        save, more = check_save(name, sav, spec)
        problems += more

    if spec.get("use") and not [l for l in log.split("\n")
                                if l.startswith("pc_lab: item ")]:
        # The same failure a battle station guards against: an item that was
        # never used leaves the overworld looking exactly as it should.
        problems.append("the item was never used, the port printed no 'item'"
                        " line in %s frames" % spec["frames"])

    if spec.get("used"):
        problems += check_used(spec, log)

    if spec.get("fished"):
        problems += check_fished(spec, log)

    digest2 = None
    if spec.get("rtc2") and digest is not None:
        # Display-only. The first boot already wrote any save-at the station
        # asked for; this one must not overwrite it.
        if rewritten:
            pc_lab.mint(recipe, sav)
        digest2, why_rtc2, _ = boot_rtc2(sav, spec, os.path.join(tmp, name + "-rtc2"))
        if digest2 is None:
            problems.append("rtc2: %s" % why_rtc2)
        elif spec.get("rtc-diff") and digest2 == digest:
            problems.append("rtc and rtc2 produced the same frame, the "
                            "clock is not on the screen")

    if pin:
        # The standard: two identical runs plus one with nothing in the
        # environment. A digest pinned on one run is a digest nobody has any
        # reason to believe.
        # Re-mint between runs for a station that rewrites its save. Its boot
        # writes the file again as it goes, so a second boot of the same file
        # would start from the state the first one left, three runs of three
        # different things, reported as a reproducibility failure.
        if rewritten:
            pc_lab.mint(recipe, sav)
        again, why2, _ = boot(sav, spec, os.path.join(tmp, name + "-again"))
        if rewritten:
            pc_lab.mint(recipe, sav)
        bare, why3, _ = boot(sav, spec, os.path.join(tmp, name + "-bare"), env={})
        if again != digest or bare != digest:
            problems.append("not reproducible: %s / %s / %s (%s %s)"
                            % (digest, again, bare, why2 or "", why3 or ""))
        else:
            spec["digest"] = digest
            spec.setdefault("name", save.name)
            spec.setdefault("map", str(save.position[0]))
            if spec.get("save-at"):
                spec.setdefault("pos", "%d,%d" % (save.position[1], save.position[2]))
            spec.setdefault("money", str(save.money))
            spec.setdefault("badges", str(save.badges))
            spec.setdefault("party", str(len(save.party)))
            if spec.get("contest") and "placement" not in spec:
                report = [l for l in log.split("\n")
                          if l.startswith("pc_lab: contest over")]
                if report:
                    spec["placement"] = report[-1].rsplit("=", 1)[-1].strip()
            if fighting and "result" not in spec:
                # `log`, not the stderr of the reproducibility boots above:
                # The result being recorded belongs to the run whose digest
                # is being pinned.
                report = [l for l in log.split("\n")
                          if l.startswith("pc_lab: battle over")]
                if report:
                    spec["result"] = report[-1].rsplit("=", 1)[-1].strip()
            if spec.get("rtc2") and digest2 is not None:
                if rewritten:
                    pc_lab.mint(recipe, sav)
                again2, why2b, _ = boot_rtc2(
                    sav, spec, os.path.join(tmp, name + "-rtc2-again"))
                if rewritten:
                    pc_lab.mint(recipe, sav)
                bare2, why3b, _ = boot_rtc2(
                    sav, spec, os.path.join(tmp, name + "-rtc2-bare"), env={})
                if again2 != digest2 or bare2 != digest2:
                    problems.append("rtc2 not reproducible: %s / %s / %s (%s %s)"
                                    % (digest2, again2, bare2,
                                       why2b or "", why3b or ""))
                else:
                    spec["digest2"] = digest2
            first = open(recipe).readline().strip().lstrip("# ")
            write_spec(name, spec, first)
    elif "digest" in spec and spec["digest"] != digest:
        problems.append("the final frame is %s, pinned at %s"
                        % (digest, spec["digest"]))
    elif "digest" not in spec:
        problems.append("no pinned digest (run --pin)")
    if (not pin and spec.get("rtc2") and digest2 is not None
            and "digest2" in spec and spec["digest2"] != digest2):
        problems.append("the rtc2 frame is %s, pinned at %s"
                        % (digest2, spec["digest2"]))
    elif not pin and spec.get("rtc2") and "digest2" not in spec:
        problems.append("rtc2 has no pinned digest (run --pin)")

    if not quiet:
        print("  %-14s map %-4s $%-6d %d badge(s) %d mon  %s%s"
              % (name, save.position[0], save.money, save.badges,
                 len(save.party), digest,
                 "" if not problems else "  <-- " + problems[0]))
    return spec, save, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("station", nargs="*")
    ap.add_argument("--pin", action="store_true",
                    help="measure and write the pinned digests")
    ap.add_argument("--repin-failed", action="store_true",
                    help="check first, then re-pin only the stations that "
                         "moved; a check is one boot and a pin is three, so "
                         "re-pinning a corpus that mostly did not move costs "
                         "three times what it needs to")
    ap.add_argument("--tmp", default=os.path.join(ROOT, "build", "pc", "corpus"))
    ap.add_argument("--cov", action="store_true",
                    help="also record which field-script commands each station "
                         "executed, into build/pc/cov/ for pc_scrcov.py")
    args = ap.parse_args()

    want = args.station or stations()
    os.makedirs(args.tmp, exist_ok=True)
    failed = {}
    print("corpus: %d station(s)%s" % (len(want), ", pinning" if args.pin else ""))
    for name in want:
        _spec, _save, problems = run_station(name, args.tmp, pin=args.pin,
                                             cov=args.cov)
        if problems:
            failed[name] = problems

    if args.repin_failed and failed:
        moved = sorted(failed)
        print("re-pinning %d station(s) that moved: %s"
              % (len(moved), ", ".join(moved)))
        for name in moved:
            _spec, _save, problems = run_station(name, args.tmp, pin=True)
            if not problems:
                del failed[name]
            else:
                failed[name] = problems
    print("%d of %d clean" % (len(want) - len(failed), len(want)))
    for name, problems in failed.items():
        for p in problems:
            print("  %s: %s" % (name, p))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
