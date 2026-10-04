# 64-bit wine, the only way to execute the Windows cross builds on a Mac
# (OrbStack runs this amd64 image under Rosetta). Console programs only:
# no X server, so the SDL app runs with SDL_VIDEO_DRIVER=dummy.
#
#   docker --context orbstack build --platform linux/amd64 \
#          -t nativeplat-wine -f tools/docker/wine.Dockerfile tools/docker
#   docker --context orbstack run --rm --platform linux/amd64 \
#          -v "$PWD:$PWD" -w "$PWD" nativeplat-wine wine64 build/win-core/tests/test_core.exe
#
# ~1.1 GB; the prefix is initialised at build time so runs start quickly.
FROM --platform=linux/amd64 debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends wine64 \
 && rm -rf /var/lib/apt/lists/*

# Debian's wine64 (without the recommended `wine` wrapper package) keeps its
# loader and server in /usr/lib/wine.
ENV PATH=/usr/lib/wine:$PATH WINEPREFIX=/wine WINEDEBUG=-all LANG=C.UTF-8
RUN wine64 wineboot --init && wineserver -w
