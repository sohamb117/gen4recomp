# Contributing

Decompilation work belongs upstream at
[pret/pokeplatinum](https://github.com/pret/pokeplatinum), not here. This fork
merges from upstream regularly, so a change accepted there arrives on its own.

What belongs here is the port: `pc/`, `3ds/` and `tools/armrec/`.

Two rules keep the merges cheap, and both are checked:

* **Nothing under `src/`, `lib/` or `subprojects/` is edited.** Where the port
  needs a game source to behave differently, it goes in `pc/patches/` as a
  unified diff against what the compile consumes. The build applies it into the
  build tree and the checked-out file never changes.
* **Nothing in the port is a guess.** Where a model could invent an answer for
  something untested, it aborts with a message naming what it does not know.

Before opening a pull request:

```sh
make -f pc/Makefile status     # link must be ok and nothing may fail to compile
python3 pc/tests/run_tests.py  # all tests pass
```

`pc/ci.sh all` runs the same thing the build gate runs, in one command, so you
can see a failure before pushing.
