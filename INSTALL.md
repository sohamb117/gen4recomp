# Building

## Packages

On Debian or Ubuntu:

```sh
sudo apt-get install -y bison flex g++ gcc-arm-none-eabi git make \
    ninja-build pkg-config python3 wget xz-utils libpng-dev \
    binutils gcc-multilib libc6-dev-i386 libsdl2-dev
```

The first line is the decompilation's own list, and the second is the port's.
`libpng-dev` is easy to miss because three of the asset tools take it as a
native dependency, so without it the ROM build stops at `meson setup` before
anything is compiled. `gcc-multilib` and `libc6-dev-i386` are there because
the port is a 32-bit build; `libsdl2-dev` is only needed for the window and
the launcher, and the port itself never links SDL.

`pc/ci.sh packages` prints the same list, and `pc/ci.sh tools` prints which
program comes from which package, so a build machine can install exactly what
the gate uses.

macOS and MSYS2 can build the ROM. The PC port needs a Linux toolchain that can
target 32-bit x86, so on Windows use WSL.

## The ROM

```sh
make BUILD=build/rom    # configure, build, compare against the real cartridge
```

`BUILD=build/rom` matters: the port looks for the cartridge at
`build/rom/pokeplatinum.us.nds`, and a plain `make` would leave it in
`build/` instead. Set `PC_ROM` if you want it somewhere else.

The first run downloads the compiler and the SDK through meson wraps, so it
needs network access and takes a few minutes. When it finishes, that file
matches the cartridge byte for byte.

## The PC port

```sh
make -f pc/Makefile -j$(nproc)
```

That leaves `build/pc/pokeplatinum`. It reads the ROM you just built, so build
the ROM first.

To play:

```sh
./pc/play.sh                  # a window, sound, a save beside the build
./pc/play.sh --new            # start over
./pc/play.sh --help           # the rest of the options
```

`play.sh` starts two programs, because the port is 32-bit and the host's SDL2
is not: they meet through a shared memory page instead of through a link. The
launcher, `build/pc/pclaunch`, does the same thing from a menu.

Every input to the port is an environment variable, and every variable has a
matching flag. `build/pc/pokeplatinum --help` lists them.

Other targets:

```sh
make -f pc/Makefile status         # what compiled, what linked, what is missing
make -f pc/Makefile test           # the test suite
make -f pc/Makefile pcview         # the viewer on its own
make -f pc/Makefile dist           # a zip that unpacks and runs
```

## The Windows build

```sh
make -f pc/Makefile.win -j$(nproc) status
make -f pc/Makefile.win viewer launcher
```

Needs `i686-w64-mingw32-gcc`. This is the same pipeline retargeted, not a
second copy of it.

## The ARM build

```sh
make -f pc/Makefile.arm -j$(nproc) status
make -f pc/Makefile.arm run ARGS='--help'
```

Needs `arm-linux-gnueabihf-gcc`, and `qemu-arm` if you want `run`. Retargeted
the same way the Windows build is.

## The Android build

```sh
make -f pc/Makefile.android -j$(nproc) status
```

Builds `build/pc-android/libpokeplatinum.so`. Needs an NDK, r27c by default
under `~/.local/opt/android-ndk-r27c`. Set `NDK=` to point at another one.

It is armeabi-v7a and never arm64-v8a, because eight-byte pointers move every
guest struct offset and the save format with them. The output is a shared
object rather than an executable, since an app is a library the framework
loads and places. `ANDROID_API` is a floor rather than a target and 26 is the
highest of three: 21 is where non-PIE was refused, 24 is where
`_FILE_OFFSET_BITS=64` is honoured, and 26 is where AAudio arrives.

Whether the guest map still lands at its fixed addresses once the framework
has placed the library is a question for a device, and `pc/handheld_probe.c`
answers it.

This builds the library and nothing more. There is no Java and no Gradle here,
so wrapping it in an app happens outside this repo.

## The 3DS build

```sh
make -f 3ds/Makefile
```

Needs devkitARM with libctru and citro3d. `make -f 3ds/Makefile status` says
what it found. `make -f 3ds/Makefile card SD=/path/to/sd` stages a card.

## Tests

```sh
python3 pc/tests/run_tests.py              # everything
python3 pc/tests/run_tests.py determinism  # one test
```

Tests that need the built binary skip themselves when it is not there. Run
`pc/ci.sh all` to do what the build gate does: the ROM, the port, the tests and
a reach measurement, in one command.
