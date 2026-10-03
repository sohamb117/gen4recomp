# Replays

Input scripts. Each line is a frame number and what is held from that frame
until a later line replaces it.

    # comment and blank lines are ignored; frames must not decrease
    150 keys START            hold START from frame 150
    156 keys none             release everything
    400 keys A B              chords are one line
    520 touch 128 96          pen down at (128,96) on the lower screen
    540 release               pen up

Frame-addressed rather than time-addressed, so a script means the same thing on
a machine running at five frames a second and on one running at two hundred.
The 3DS port reads these same files unchanged.

```sh
./pc/play.sh --input pc/replays/new-game.txt     # watch one
PC_RECORD_INPUT=out.txt ./pc/play.sh             # record one
```

`new-game.txt` is a cold boot through the intro to a saved game. The `lab-*`
scripts drive one situation each and are what the test suite's stations use.
