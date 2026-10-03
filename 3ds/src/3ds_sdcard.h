/*
 * 3ds/src/3ds_sdcard.h: opening a file on the SD card for writing.
 *
 * Why this is not just fopen(). A console that has only ever launched this
 * port out of its own RomFS has no sdmc:/3ds/pokeplatinum: the cartridge
 * fallback is what usually creates it, and the sdmc devoptab will not make a
 * directory for an fopen the way a POSIX one does not either. Every report
 * this port writes therefore has to be prepared to make its own path, and
 * there is more than one of them, the sound check and the frame-time
 * heartbeat, so the retry lives here rather than once per writer.
 *
 * No libctru: mkdir and fopen are the C library's, so a build machine runs
 * this file unchanged and the tests that cover the writers do too.
 */

#ifndef POKEPLATINUM_3DS_SDCARD_H
#define POKEPLATINUM_3DS_SDCARD_H

#include <stdio.h>

/*
 * fopen(path, "w"), and if that fails, make every directory on the way to it
 * and try once more. NULL if it still will not open, so a failure that is not
 * a missing directory is still a failure.
 */
FILE *sd_open_write(const char *path);

/*
 * Every directory on the way to `path`, made if it is not there; the last
 * component is taken to be the file and is never made. Exported for the save
 * image, which is opened with open() rather than fopen(), write-through to
 * a file descriptor, not a stream, so it cannot go through sd_open_write().
 */
void sd_make_dirs(const char *path);

#endif /* POKEPLATINUM_3DS_SDCARD_H */
