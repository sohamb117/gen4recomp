/*
 * pclaunch, a launcher for the PC port, so a downloaded zip is playable
 * without a terminal.
 *
 * A 64-bit SDL2 program beside pcview, for the same reason pcview is one: the
 * port is -m32 and SDL2 here is not. It owns no part of the port's behaviour;
 * everything it does is compose command lines the two programs already accept,
 * print them, and run them. A person who never opens this loses nothing but
 * the menu.
 *
 * It starts two programs, which is the one thing pc/play.sh also has to do.
 * The port publishes frames into a shared page and pcview presents them, so
 * PLAY starts the port with `--view NAME`, then starts `pcview NAME`, and
 * takes the port down when the window closes. There is no sleep between them:
 * pcview waits for the channel itself.
 *
 * What it offers is the display and pacing surface, because that is the part
 * with knobs a player reasonably wants. Every one is a flag one of the two
 * programs already has:
 *
 *   frame pacing     the port's --pace
 *   render scale     pcview --render-scale
 *   enhancement      pcview --filter (bilinear, sharp bilinear, EPX)
 *   screen size      pcview --scale
 *   wide layout      pcview --layout wide
 *   integer scaling  pcview --integer
 *   stretch          pcview --stretch
 *   fullscreen       pcview --fullscreen
 *   sound            pcview --no-audio
 *
 * The UI is drawn by hand into one streaming texture, from the bitmap font in
 * pclaunch_font.h, because a launcher must not grow dependencies the port's
 * own window does not have: SDL2 is already required and SDL_ttf is not.
 *
 * Settings persist in ~/.config/pokeplatinum-launcher.cfg, key and value per
 * line, written on every launch and on quit. The file is a convenience, not an
 * input to the port: deleting it costs the defaults.
 *
 * The path a person who never read this takes is two events long. If a ROM is
 * known, the selection starts on PLAY, so double-click, Enter, playing. If
 * none is findable, the file picker opens by itself, once, rather than the
 * menu presenting "(none found)" and waiting to be understood.
 *
 * $PCLAUNCH_PICKER overrides how that dialog is opened: any command whose
 * first line of stdout is the chosen path. It is the escape hatch for a
 * desktop with neither zenity nor kdialog, and it is what makes the picker
 * testable without a human clicking. `--keys a,b,c` pushes one keydown a frame
 * into SDL's queue and quits; `--dry-run` prints what it would run and starts
 * nothing.
 */

#include "pclaunch_font.h"
#include "pc_window_icon.h"

#include <SDL.h>
#if defined(_WIN32)
#include <SDL_syswm.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
/* mingw has dirent and stat; what differs is where the exe is, what a real
 * path is, where config lives, and how a child is made. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>    /* GetOpenFileNameA; LEAN_AND_MEAN leaves it out */
#define GAME_EXE "pokeplatinum.exe"
#define VIEW_EXE "pcview.exe"
#define PC_POPEN  _popen
#define PC_PCLOSE _pclose
/* _pclose hands back the child's exit code; pclose hands back a wait status. */
#define PC_EXITCODE(st) (st)
#else
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#define GAME_EXE "pokeplatinum"
#define VIEW_EXE "pcview"
#define PC_POPEN  popen
#define PC_PCLOSE pclose
#define PC_EXITCODE(st) (WIFEXITED(st) ? WEXITSTATUS(st) : -1)
#endif

#define CANVAS_W 640
#define CANVAS_H 600

#define MAX_ROMS 16
#define PATH_MAX_L 1024

/* ------------------------------------------------------------------ */
/* The canvas: fill rectangles and glyphs, nothing else                */
/* ------------------------------------------------------------------ */

static uint32_t canvas[CANVAS_W * CANVAS_H];

static void fill(int x, int y, int w, int h, uint32_t c) {
    int i, j;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            canvas[(size_t)(y + j) * CANVAS_W + x + i] = c;
        }
    }
}

static void glyph(int ch, int x, int y, int s, uint32_t c) {
    int gx, gy, i, j;

    if (ch < 32 || ch > 126) ch = '?';
    for (gy = 0; gy < LAUNCH_FONT_H; gy++) {
        unsigned bits = launch_font[ch - 32][gy];

        for (gx = 0; gx < LAUNCH_FONT_W; gx++) {
            if (!(bits & (0x80u >> gx))) continue;
            for (j = 0; j < s; j++) {
                for (i = 0; i < s; i++) {
                    int px = x + gx * s + i, py = y + gy * s + j;

                    if (px >= 0 && px < CANVAS_W && py >= 0 && py < CANVAS_H) {
                        canvas[(size_t)py * CANVAS_W + px] = c;
                    }
                }
            }
        }
    }
}

static void text(int x, int y, int s, uint32_t c, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int i;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (i = 0; buf[i] != '\0'; i++) {
        glyph((unsigned char)buf[i], x + i * LAUNCH_FONT_W * s, y, s, c);
    }
}

/* ------------------------------------------------------------------ */
/* The options                                                         */
/* ------------------------------------------------------------------ */

/*
 * Every option is a label and a ring of values; the ROM ring is filled at
 * startup by the scan below. Indices into this array are what the config
 * file stores, next to the ROM's full path (an index into a directory
 * listing would rot; a path does not).
 */
struct row {
    const char *label;
    const char *key;            /* config key, NULL = not persisted   */
    const char *vals[MAX_ROMS];
    int n;
    int idx;
    const char *help;
};

/*
 * The rows are exactly the flags the two programs have, and the ones diamond's
 * launcher offers that this one does not are absent rather than disabled:
 * `--hd3d`, a per-screen size, screen snapping and the two-window split are
 * that port's, and offering a control the port would ignore is worse than not
 * offering it.
 *
 * Two rows say "RESOLUTION" and they are not the same control either. 3D
 * detail is the port's: the 3D layer is rasterized at N times the pixels,
 * which is real detail the geometry always had. Render resolution is the
 * viewer's upscaler, which can only enlarge what it was given.
 *
 * Two rows say "ASPECT" and they are not the same control. Widescreen is the
 * port's: it widens the 3D camera, so the picture holds more world. Aspect
 * ratio is the viewer's: it decides what to do with the picture it was given,
 * whatever shape that is.
 */
enum {
    ROW_ROM, ROW_SAVE, ROW_PACE, ROW_WIDE, ROW_HD3D, ROW_RES, ROW_FX,
    ROW_LAYOUT,
    ROW_SIZE, ROW_INTSCALE, ROW_ASPECT, ROW_FULLSCREEN, ROW_SOUND,
    ROW_PLAY, ROW_QUIT, ROW_COUNT
};

static struct row rows[ROW_COUNT] = {
    [ROW_ROM] = { "Game", "rom", { "(none found)" }, 1, 0,
                  "the .nds to run; B browses, Backspace rescans, or drop a "
                  ".nds here" },
    [ROW_SAVE] = { "Save file", "save", { "Default (live.sav, beside this launcher)" }, 1, 0,
                   "where the cartridge save lives; B browses, Backspace "
                   "returns to default" },
    [ROW_PACE] = { "Frame pacing", "pace",
                   { "Console rate (60 fps)", "Unlimited" }, 2, 0,
                   "unlimited runs as fast as this machine can, which "
                   "breaks up the sound, because the game makes it faster "
                   "than it plays" },
    [ROW_WIDE] = { "Widescreen", "wide",
                   { "Off (4:3)", "Fill the window", "16:9", "16:10" }, 4, 0,
                   "widens the 3D camera rather than the pixels, more world, "
                   "same picture in the middle" },
    [ROW_HD3D] = { "3D detail", "hd3d",
                   { "Console (256x192)", "Double (512x384)",
                     "Triple (768x576)", "Quadruple (1024x768)" }, 4, 0,
                   "draws the 3D layer at N times the resolution; the 2D art "
                   "is the game's own and is not invented. Each step costs "
                   "the processor more, if the game runs slow, step back "
                   "down" },
    [ROW_RES] = { "Render resolution", "res",
                  { "256x192 (native)", "512x384 (2x)",
                    "768x576 (3x)", "1024x768 (4x)" }, 4, 0,
                  "what the picture is made at, the window scales it" },
    [ROW_FX] = { "Enhancement", "fx",
                 { "Off (crisp pixels)", "Bilinear",
                   "Sharp bilinear", "Scale2x (EPX)" }, 4, 0,
                 "how render pixels are made; EPX smooths pixel-art edges" },
    [ROW_LAYOUT] = { "Screen layout", "layout",
                     { "Smart", "Stacked (DS)", "Side by side" }, 3, 0,
                     "smart keeps the game large and grows the touch screen "
                     "when the game asks for the pen" },
    [ROW_SIZE] = { "Screen size", "size",
                   { "1x", "2x", "3x", "4x", "5x" }, 5, 1,
                   "how big the window opens; drag it any size after" },
    [ROW_INTSCALE] = { "Integer scaling", "int",
                       { "Off", "On" }, 2, 0,
                       "only whole multiples of a DS pixel; crisp, letterboxed" },
    [ROW_ASPECT] = { "Aspect ratio", "aspect",
                     { "Native (4:3)", "Stretch" }, 2, 0,
                     "stretch fills the window and gives up the proportions" },
    [ROW_FULLSCREEN] = { "Fullscreen", "fullscreen", { "Off", "On" }, 2, 0,
                         "F11 or Alt+Enter toggles it in the window too" },
    [ROW_SOUND] = { "Sound", "sound", { "On", "Off" }, 2, 0,
                    "the viewer plays it; pcview --list-audio-devices names "
                    "devices" },
    [ROW_PLAY] = { "PLAY", NULL, { "" }, 1, 0,
                   "starts the game with the settings above" },
    [ROW_QUIT] = { "Quit", NULL, { "" }, 1, 0, "" },
};

static char rom_paths[MAX_ROMS][PATH_MAX_L];
static char rom_names[MAX_ROMS][128];
static int rom_count;

/*
 * The save file. Empty means the port's own default, the ROM's path with
 * .nds swapped for .sav, and the row shows exactly one value. A custom
 * path (browsed, dropped, or read back from the config) adds a second ring
 * entry and composes --save. The file does not have to exist yet: the port
 * creates it on the game's first write.
 */
static char save_path[PATH_MAX_L];
static char save_label[144];

static void save_set(const char *path) {
    const char *base;

    if (path == NULL || path[0] == '\0' ||
        strlen(path) >= sizeof save_path) {
        return;
    }
    snprintf(save_path, sizeof save_path, "%s", path);
    base = strrchr(save_path, '/');
#if defined(_WIN32)
    {
        const char *b2 = strrchr(save_path, '\\');

        if (b2 != NULL && (base == NULL || b2 > base)) base = b2;
    }
#endif
    snprintf(save_label, sizeof save_label, "%s",
             base != NULL ? base + 1 : save_path);
    rows[ROW_SAVE].vals[1] = save_label;
    rows[ROW_SAVE].n = 2;
    rows[ROW_SAVE].idx = 1;
}

static void save_reset(void) {
    save_path[0] = '\0';
    rows[ROW_SAVE].n = 1;
    rows[ROW_SAVE].idx = 0;
}

/*
 * The window, kept here for one reason: a Windows file dialog needs an owner.
 * Without hwndOwner it is a top-level window of its own, and it opens BEHIND
 * the launcher, which looks exactly like pressing B doing nothing, because
 * the launcher is still drawing and still on top while a modal dialog nobody
 * can see waits for an answer.
 */
static SDL_Window *ui_window;

static char exe_dir[PATH_MAX_L];
static char status_line[256];
static uint32_t status_col = 0x00AAAAAAu;

/*
 * Also on stderr, always. The status line is one row of a window that is
 * hidden while the game runs and closed when it ends, so a launch that fails
 * writes its only explanation onto a surface nobody is looking at; which is
 * what "it kicks me back to the launcher" turned out to mean, with the reason
 * on screen for the few frames before the window went away. Anyone running
 * this from a terminal now gets the same sentence somewhere it stays.
 */
static void status(uint32_t col, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status_line, sizeof status_line, fmt, ap);
    va_end(ap);
    status_col = col;
    fprintf(stderr, "pclaunch: %s\n", status_line);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Finding things                                                      */
/* ------------------------------------------------------------------ */

static void find_exe_dir(const char *argv0) {
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir - 1);

    if (n == 0) snprintf(exe_dir, sizeof exe_dir, "%s",
                         argv0 != NULL ? argv0 : ".");
    else exe_dir[n] = '\0';
    {
        char *s;

        for (s = exe_dir + strlen(exe_dir); s > exe_dir; s--) {
            if (s[-1] == '\\' || s[-1] == '/') { s[-1] = '\0'; break; }
        }
    }
#else
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof exe_dir - 1);

    if (n > 0) {
        exe_dir[n] = '\0';
    } else if (argv0 != NULL) {
        snprintf(exe_dir, sizeof exe_dir, "%s", argv0);
    }
    {
        char *slash = strrchr(exe_dir, '/');

        if (slash != NULL) *slash = '\0';
        else snprintf(exe_dir, sizeof exe_dir, ".");
    }
#endif
}

static void rom_add(const char *path) {
#if defined(_WIN32)
    char *real = _fullpath(NULL, path, 0);
    struct stat st;

    if (real != NULL && stat(real, &st) != 0) { free(real); real = NULL; }
#else
    /* realpath allocates: handing it a buffer means owning PATH_MAX, and
     * glibc's fortify aborts the program for a buffer any smaller. */
    char *real = realpath(path, NULL);
#endif
    int i;

    if (real == NULL) return;
    for (i = 0; i < rom_count; i++) {
        if (strcmp(rom_paths[i], real) == 0) { free(real); return; }
    }
    if (rom_count >= MAX_ROMS || strlen(real) >= PATH_MAX_L) {
        free(real);
        return;
    }
    snprintf(rom_paths[rom_count], PATH_MAX_L, "%s", real);
    {
        const char *base = strrchr(real, '/');

        snprintf(rom_names[rom_count], sizeof rom_names[0], "%s",
                 base != NULL ? base + 1 : real);
    }
    free(real);
    rom_count++;
}

static void rom_scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;

    if (d == NULL) return;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        char path[PATH_MAX_L];

        if (len < 5 || strcmp(e->d_name + len - 4, ".nds") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        rom_add(path);
    }
    closedir(d);
}

/*
 * Where a .nds might be. The order is the order the ring offers them, so the
 * most likely answer comes first.
 *
 * The tree builds its own, which is the one thing this differs from a port of
 * a dump in. build/rom/pokeplatinum.us.nds is what `ninja -C build/rom`
 * produces, it sits beside build/pc where this program lives, and in a source
 * checkout it is almost always the file the player means. Beside the
 * executable comes first anyway, because that is where a zip puts one.
 */
static void rom_scan(void) {
    char up[PATH_MAX_L];
    const char *env = getenv("POKEPLATINUM_ROM");

    if (env != NULL && env[0] != '\0') rom_add(env);
    rom_scan_dir(exe_dir);
    snprintf(up, sizeof up, "%s/../rom", exe_dir);  /* build/pc -> build/rom */
    rom_scan_dir(up);
    snprintf(up, sizeof up, "%s/../..", exe_dir);   /* build/pc -> repo root */
    rom_scan_dir(up);
    rom_scan_dir(".");

    if (rom_count == 0) {
        rows[ROW_ROM].vals[0] = "(none found)";
        rows[ROW_ROM].n = 1;
    } else {
        int i;

        for (i = 0; i < rom_count; i++) rows[ROW_ROM].vals[i] = rom_names[i];
        rows[ROW_ROM].n = rom_count;
    }
}

/*
 * Take a path a person named, browsed, dropped, or read back from the
 * config, and make it the chosen ROM. Returns 1 if the list grew, 0 if the
 * path was unreadable or already there. Three callers did this by hand and a
 * fourth was wanted; one copy is one place for the ring bookkeeping to be
 * right.
 */
static int rom_adopt(const char *path) {
    int before = rom_count;

    rom_add(path);
    if (rom_count == before) return 0;
    rows[ROW_ROM].vals[rom_count - 1] = rom_names[rom_count - 1];
    rows[ROW_ROM].n = rom_count;
    rows[ROW_ROM].idx = rom_count - 1;
    return 1;
}

/*
 * Run one picker command and read the path it printed.
 *
 *   1  a path was chosen and `out` holds it
 *   0  the program ran and the person cancelled
 *  -1  the program is not on this machine, try the next one
 *
 * The three answers used to be two, and they have to be apart: the dialog
 * opens unprompted on a first run, so "you cancelled" and "there is no
 * dialog here, drag the file in instead" are different things to say. popen
 * runs the command through a shell, and a shell reports a command it could
 * not find as exit 127; which is the one status a real picker never
 * returns for a cancel (zenity uses 1, kdialog 1).
 */
static int run_picker(const char *cmd, char *out, size_t cap) {
    FILE *p = PC_POPEN(cmd, "r");
    size_t n;
    int st;

    if (p == NULL) return -1;
    out[0] = '\0';
    n = fgets(out, (int)cap, p) != NULL ? strlen(out) : 0;
    st = PC_PCLOSE(p);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[--n] = '\0';
    }
    if (n > 0) return 1;
    return PC_EXITCODE(st) == 127 ? -1 : 0;
}

/*
 * A file picker, with what this machine has. Windows has one in the box.
 * Elsewhere zenity and kdialog cover the common desktops; a machine with
 * neither still has drag-and-drop, which SDL delivers everywhere, and the
 * status line says so. $PCLAUNCH_PICKER outranks all of it, any command
 * whose first line of stdout is the chosen path; which is both the escape
 * hatch for an unusual desktop and how test_pclaunch drives this without a
 * human. Returns 1 with `out` filled, 0 for cancelled or unavailable.
 */
static int browse_file(const char *title, char *out, size_t cap) {
    const char *env = getenv("PCLAUNCH_PICKER");

    out[0] = '\0';
    if (env != NULL && env[0] != '\0') {
        int r = run_picker(env, out, cap);

        if (r < 0) {
            status(0x00FFAA66u, "$PCLAUNCH_PICKER did not run");
            return 0;
        }
        return r;
    }
#if defined(_WIN32)
    {
        OPENFILENAMEA of;

        memset(&of, 0, sizeof of);
        of.lStructSize = sizeof of;
        of.lpstrFile = out;
        of.nMaxFile = (DWORD)cap;
        of.lpstrTitle = title;
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        /* Owned by the launcher, so it opens in front of it and the taskbar
         * shows one window rather than two. */
        if (ui_window != NULL) {
            SDL_SysWMinfo wmi;

            SDL_VERSION(&wmi.version);
            if (SDL_GetWindowWMInfo(ui_window, &wmi)
                && wmi.subsystem == SDL_SYSWM_WINDOWS) {
                of.hwndOwner = wmi.info.win.window;
            }
        }
        return GetOpenFileNameA(&of) ? 1 : 0;
    }
#else
    {
        const char *cmds[2];
        static char cmd[PATH_MAX_L + 128];
        int c;

        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --title '%s' 2>/dev/null", title);
        cmds[0] = cmd;
        cmds[1] = "kdialog --getopenfilename . 2>/dev/null";
        for (c = 0; c < 2; c++) {
            int r = run_picker(cmds[c], out, cap);

            if (r >= 0) return r;       /* chose, or ran and was cancelled */
        }
        status(0x00FFAA66u, "no zenity or kdialog here, drag & drop the "
                            "file onto this window instead, or set "
                            "$PCLAUNCH_PICKER");
        return 0;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* The config file                                                     */
/* ------------------------------------------------------------------ */

static void cfg_path(char *out, size_t cap) {
#if defined(_WIN32)
    const char *app = getenv("APPDATA");

    snprintf(out, cap, "%s\\pokeplatinum-launcher.cfg",
             app != NULL && app[0] != '\0' ? app : ".");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(out, cap, "%s/pokeplatinum-launcher.cfg", xdg);
    } else {
        snprintf(out, cap, "%s/.config/pokeplatinum-launcher.cfg",
                 home != NULL ? home : ".");
    }
#endif
}

/*
 * Key bindings live in the config and are passed through, not chosen here.
 * The viewer owns what a key does; this program owns where a session's
 * settings are kept, so the bindings are stored beside everything else and
 * handed over on the command line. There is no rebinding screen: the file is
 * the interface, the syntax is in its own header comment, and an unreadable
 * binding is refused by the viewer with the name it did not understand
 * rather than silently dropped here.
 *
 * Saved back verbatim, so a hand-edited file survives the launcher writing
 * its own settings out.
 */
static char bind_spec[512];

static void cfg_save(void) {
    char path[PATH_MAX_L];
    FILE *f;
    int i;

    cfg_path(path, sizeof path);
    f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "# written by pclaunch; delete for defaults\n");
    fprintf(f, "# bind PAD=KEY[,PAD=KEY...] rebinds the keyboard --\n"
               "#   PAD is a, b, x, y, l, r, start, select or a direction,\n"
               "#   KEY is one of SDL's key names (z, Return, Left Shift).\n"
               "#   Anything not named keeps its default.\n");
    if (bind_spec[0] != '\0') {
        fprintf(f, "bind %s\n", bind_spec);
    }
    for (i = 0; i < ROW_COUNT; i++) {
        if (rows[i].key == NULL) continue;
        if (i == ROW_ROM) {
            if (rom_count > 0) {
                fprintf(f, "rom %s\n", rom_paths[rows[i].idx]);
            }
        } else if (i == ROW_SAVE) {
            /* A path, not an index, and only when one was chosen, so a
             * config with no `save` line means the default. */
            if (save_path[0] != '\0' && rows[i].idx == 1) {
                fprintf(f, "save %s\n", save_path);
            }
        } else {
            fprintf(f, "%s %d\n", rows[i].key, rows[i].idx);
        }
    }
    fclose(f);
}

static void cfg_load(void) {
    char path[PATH_MAX_L], line[PATH_MAX_L + 64];
    FILE *f;

    cfg_path(path, sizeof path);
    f = fopen(path, "r");
    if (f == NULL) return;
    while (fgets(line, sizeof line, f) != NULL) {
        char key[64];
        char val[PATH_MAX_L];
        int i;

        if (line[0] == '#' || sscanf(line, "%63s %1023[^\n]", key, val) != 2) {
            continue;
        }
        if (strcmp(key, "rom") == 0) {
            int found = 0;

            for (i = 0; i < rom_count; i++) {
                if (strcmp(rom_paths[i], val) == 0) {
                    rows[ROW_ROM].idx = i;
                    found = 1;
                }
            }
            /* A browsed ROM lives outside the scanned directories, so the
             * scan will not have it; add it back the way the browse did. */
            if (!found) rom_adopt(val);
            continue;
        }
        if (strcmp(key, "save") == 0) {
            save_set(val);
            continue;
        }
        if (strcmp(key, "bind") == 0) {
            snprintf(bind_spec, sizeof bind_spec, "%s", val);
            continue;
        }
        for (i = 0; i < ROW_COUNT; i++) {
            if (rows[i].key != NULL && strcmp(key, rows[i].key) == 0) {
                int v = atoi(val);

                if (v >= 0 && v < rows[i].n) rows[i].idx = v;
            }
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Composing the command line                                          */
/* ------------------------------------------------------------------ */

/*
 * Two command lines, because there are two programs. The port renders into a
 * shared page and pcview presents it; a -m32 port and a 64-bit SDL cannot meet
 * in a link, so they meet in shared memory and the launcher starts both. Every
 * flag below is one the receiving program already documents on its own --help.
 *
 * The strings a row contributes are literals or static buffers, so both arrays
 * stay valid until exec.
 */
static char arg_exe[PATH_MAX_L], arg_view_exe[PATH_MAX_L];
static char arg_save[PATH_MAX_L], arg_shots[PATH_MAX_L];
static char arg_rs[16], arg_scale[16];
static char chan_name[64];

/* A per-run channel, so two sessions cannot publish into one page. */
static void chan_make(void) {
#if defined(_WIN32)
    snprintf(chan_name, sizeof chan_name, "pplat-%lu",
             (unsigned long)GetCurrentProcessId());
#else
    snprintf(chan_name, sizeof chan_name, "pplat-%ld", (long)getpid());
#endif
}

/*
 * The exe cache dodge, on Windows only.
 *
 * Windows will serve a stale image for a rebuilt exe at a fixed path; this
 * project has measured it twice, once through a play session and once with
 * this launcher looking un-updated after a rebuild. The result is a session
 * that runs yesterday's binary and reports today's behaviour, which is the
 * worst shape a bug report can have.
 *
 * So the port is copied to a name nothing has cached before it is started,
 * the copies are pruned to the last few, and the name is printed, because a
 * session that cannot say which image it ran has the same problem in a
 * quieter form. On failure the fixed path is used and the reason said out
 * loud: a launcher that refuses to start because a copy failed would be worse
 * than one that runs a possibly-cached exe and admits it.
 *
 * POSIX needs none of this and gets none of it.
 */
#if defined(_WIN32)
#define PCLAUNCH_RING 3

/*
 * `keep` is the copy that is about to be RUN, and it is never deleted however
 * the sort comes out. It has to be named explicitly because the sort cannot
 * be trusted to protect it: CopyFile carries the SOURCE file's last-write
 * time onto the copy, so every pplay-*.exe in the folder shares one
 * timestamp, "newest first" cannot tell them apart, and on the fourth launch
 * from a folder the pruner deleted the image it had just made. CreateProcess
 * then failed on a file that no longer existed and the launcher came
 * straight back; "it doesn't start, it kicks me back to the launcher".
 * fresh_port_exe stamps the copy with the current time now, which makes the
 * ordering true again; this guard makes the deletion impossible either way.
 */
static void prune_copies(const char *keep) {
    WIN32_FIND_DATAA fd;
    char pat[PATH_MAX_L], path[PATH_MAX_L];
    struct { char name[64]; FILETIME t; } found[64];
    const char *keep_name = NULL;
    int n = 0, i, j;
    HANDLE h;

    if (keep != NULL) {
        const char *cut = strrchr(keep, '/');
        const char *bs = strrchr(keep, '\\');

        if (bs != NULL && (cut == NULL || bs > cut)) cut = bs;
        keep_name = cut != NULL ? cut + 1 : keep;
    }

    snprintf(pat, sizeof pat, "%s/pplay-*.exe", exe_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n == (int)(sizeof found / sizeof found[0])) break;
        snprintf(found[n].name, sizeof found[n].name, "%s", fd.cFileName);
        found[n].t = fd.ftLastWriteTime;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    /* Newest first, then delete everything past the ring. Failure is fine:
     * A copy still open by a running session cannot be removed, and the next
     * launch will get it. */
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && CompareFileTime(&found[j - 1].t, &found[j].t) < 0;
             j--) {
            char nm[64]; FILETIME t;
            memcpy(nm, found[j - 1].name, sizeof nm);
            t = found[j - 1].t;
            memcpy(found[j - 1].name, found[j].name, sizeof nm);
            found[j - 1].t = found[j].t;
            memcpy(found[j].name, nm, sizeof nm);
            found[j].t = t;
        }
    }
    for (i = PCLAUNCH_RING; i < n; i++) {
        if (keep_name != NULL && _stricmp(found[i].name, keep_name) == 0) {
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", exe_dir, found[i].name);
        DeleteFileA(path);
    }
}

/* Fills `out` with the exe to run: a fresh copy, or the fixed path if the
 * copy could not be made. */
static void fresh_port_exe(char *out, size_t cap) {
    char src[PATH_MAX_L];

    snprintf(src, sizeof src, "%s/%s", exe_dir, GAME_EXE);
    snprintf(out, cap, "%s/pplay-%lu.exe", exe_dir,
             (unsigned long)GetTickCount());
    if (!CopyFileA(src, out, FALSE)) {
        fprintf(stderr, "pclaunch: could not copy the game to a fresh name "
                        "(error %lu); running %s, which Windows may have "
                        "cached\n", (unsigned long)GetLastError(), GAME_EXE);
        snprintf(out, cap, "%s", src);
        return;
    }
    /* The copy is new; its timestamp says otherwise until this runs, and the
     * ring above sorts on it. See prune_copies. */
    {
        HANDLE h = CreateFileA(out, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft;
            SYSTEMTIME st;

            GetSystemTime(&st);
            if (SystemTimeToFileTime(&st, &ft)) {
                SetFileTime(h, NULL, NULL, &ft);
            }
            CloseHandle(h);
        }
    }
    prune_copies(out);
    fprintf(stderr, "pclaunch: running %s\n", out);
}
#endif

/* The port: what to run, where to save, and the channel to publish on. */
static int build_port_args(char **argv, int cap) {
    int a = 0;

#if defined(_WIN32)
    fresh_port_exe(arg_exe, sizeof arg_exe);
#else
    snprintf(arg_exe, sizeof arg_exe, "%s/%s", exe_dir, GAME_EXE);
#endif
    argv[a++] = arg_exe;
    argv[a++] = (char *)"--rom";
    argv[a++] = rom_paths[rows[ROW_ROM].idx];
    argv[a++] = (char *)"--view";
    argv[a++] = chan_name;

    /*
     * The default is the same file the play scripts use, live.sav beside
     * this program, and not the port's own default, which is beside the
     * ROM. Two defaults for one game means progress made through the
     * launcher is invisible to ./pc/play-win.sh and the other way round,
     * and the person who hits that has no way to guess why.
     */
    if (rows[ROW_SAVE].idx == 1 && save_path[0] != '\0') {
        argv[a++] = (char *)"--save";
        argv[a++] = save_path;
    } else {
        snprintf(arg_save, sizeof arg_save, "%s/live.sav", exe_dir);
        argv[a++] = (char *)"--save";
        argv[a++] = arg_save;
    }
    if (rows[ROW_HD3D].idx > 0) {
        static const char *const hd[] = { "1", "2", "3", "4" };

        argv[a++] = (char *)"--hd3d";
        argv[a++] = (char *)hd[rows[ROW_HD3D].idx];
    }
    /* --pace 1 is the port's default, so only the other case is passed: a
     * printed command line should say what was chosen, not restate defaults. */
    if (rows[ROW_PACE].idx == 1) {
        argv[a++] = (char *)"--pace";
        argv[a++] = (char *)"0";
    }
    /* Widescreen is the port's, not the viewer's: it is a wider camera, and
     * only the side that owns the projection can widen one. Off is the
     * default and stays off the command line. */
    if (rows[ROW_WIDE].idx != 0) {
        static const char *const specs[] = { "off", "auto", "16:9", "16:10" };

        argv[a++] = (char *)"--aspect";
        argv[a++] = (char *)specs[rows[ROW_WIDE].idx];
    }

    if (a >= cap) a = cap - 1;
    argv[a] = NULL;
    return a;
}

/* The viewer: the same channel, and the whole display surface. */
static int build_view_args(char **argv, int cap) {
    int a = 0;
    int rs = rows[ROW_RES].idx + 1;             /* 1..4 */
    int fx = rows[ROW_FX].idx;
    int scale = rows[ROW_SIZE].idx + 1;         /* 1..5 */

    if (rs < 1) rs = 1;
    if (rs > 4) rs = 4;
    if (scale < 1) scale = 1;
    if (scale > 5) scale = 5;

    snprintf(arg_view_exe, sizeof arg_view_exe, "%s/%s", exe_dir, VIEW_EXE);
    argv[a++] = arg_view_exe;
    argv[a++] = chan_name;

    /*
     * Screenshots go where the save goes. The viewer has no idea where that
     * is (it never sees a save path) and it should not: this program is
     * the one that decides where a session's files live, so it is the one
     * that says. Without this F12 writes into whatever directory the
     * launcher happened to be started from, which for a double-click is
     * nowhere a player would look.
     */
    {
        const char *sav = (rows[ROW_SAVE].idx == 1 && save_path[0] != '\0')
                          ? save_path : NULL;
        size_t n;

        if (sav != NULL) {
            const char *cut = strrchr(sav, '/');
#if defined(_WIN32)
            const char *bs = strrchr(sav, '\\');
            if (bs != NULL && (cut == NULL || bs > cut)) cut = bs;
#endif
            n = cut != NULL ? (size_t)(cut - sav) : 0;
        } else {
            n = 0;
        }
        if (n > 0 && n < sizeof arg_shots) {
            memcpy(arg_shots, sav, n);
            arg_shots[n] = '\0';
        } else {
            snprintf(arg_shots, sizeof arg_shots, "%s", exe_dir);
        }
        argv[a++] = (char *)"--shots";
        argv[a++] = arg_shots;
    }

    /* EPX below 2x has nothing to do, and the viewer would say so and pick 2
     * itself; picking it here keeps the printed command honest. */
    if (fx == 3 && rs < 2) rs = 2;
    /* Sharp bilinear is bilinear sampling of a replicated texture, so it needs
     * a render scale to replicate at. */
    if (fx == 2 && rs < 2) rs = 2;
    if (rs > 1) {
        snprintf(arg_rs, sizeof arg_rs, "%d", rs);
        argv[a++] = (char *)"--render-scale";
        argv[a++] = arg_rs;
    }
    if (fx == 1 || fx == 2) {
        argv[a++] = (char *)"--filter";
        argv[a++] = (char *)"linear";
    } else if (fx == 3) {
        argv[a++] = (char *)"--filter";
        argv[a++] = (char *)"scale2x";
    }

    /* Smart is the viewer's default, so only the other two are passed:
     * A printed command line should say what was chosen, not restate a
     * default. */
    if (rows[ROW_LAYOUT].idx == 1) {
        argv[a++] = (char *)"--layout";
        argv[a++] = (char *)"stacked";
    } else if (rows[ROW_LAYOUT].idx == 2) {
        argv[a++] = (char *)"--layout";
        argv[a++] = (char *)"wide";
    }

    snprintf(arg_scale, sizeof arg_scale, "%d", scale);
    argv[a++] = (char *)"--scale";
    argv[a++] = arg_scale;

    if (bind_spec[0] != '\0') {
        argv[a++] = (char *)"--bind";
        argv[a++] = bind_spec;
    }

    if (rows[ROW_INTSCALE].idx == 1)   argv[a++] = (char *)"--integer";
    if (rows[ROW_ASPECT].idx == 1)     argv[a++] = (char *)"--stretch";
    if (rows[ROW_FULLSCREEN].idx == 1) argv[a++] = (char *)"--fullscreen";
    if (rows[ROW_SOUND].idx == 1)      argv[a++] = (char *)"--no-audio";

    if (a >= cap) a = cap - 1;
    argv[a] = NULL;
    return a;
}

/* ------------------------------------------------------------------ */
/* Running the game                                                    */
/* ------------------------------------------------------------------ */

/*
 * The port creates /dev/shm/<channel> and removes it on a clean exit. A port
 * killed with the window still open does not get to, so the launcher sweeps
 * after itself: a stale page is a name the next session cannot reuse and a few
 * hundred KB nobody frees until reboot.
 */
static void shm_unlink_chan(void) {
#if !defined(_WIN32)
    char path[128];

    if (chan_name[0] == '\0') return;
    snprintf(path, sizeof path, "/dev/shm/%s", chan_name);
    unlink(path);
#endif
}

/*
 * PLAY: start the port, start the viewer, hide the menu, and come back when
 * the window closes.
 *
 * Both command lines are printed exactly as exec sees them, so a session is
 * reproducible without this program. There is no sleep between the two:
 * pcview waits for the channel itself, which is what makes starting them back
 * to back correct rather than lucky.
 *
 * The viewer is the one we wait on, and the port is taken down after it. The
 * window closing is what a player means by "done"; the port would otherwise
 * keep running with nothing presenting it, which is exactly the orphan
 * pc/play.sh traps for. The kill is unconditional and then reaped, so a port
 * that already exited on its own (a frame limit, a trap) is not a special
 * case.
 */
static int dry_run;             /* --dry-run: compose and print, start nothing */

static void print_cmd(const char *what, char **argv) {
    int i;

    fprintf(stderr, "pclaunch: %s:", what);
    for (i = 0; argv[i] != NULL; i++) fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
}

#if defined(_WIN32)
/* One command line, minimally quoted: a path with a space is the only
 * argument here that needs it, and no argument carries a quote. */
static int win_cmdline(char **argv, char *out, size_t cap) {
    int i, n = 0;

    for (i = 0; argv[i] != NULL; i++) {
        int sp = strchr(argv[i], ' ') != NULL;

        n += snprintf(out + n, cap - (size_t)n, "%s%s%s%s",
                      i > 0 ? " " : "", sp ? "\"" : "", argv[i], sp ? "\"" : "");
        if (n >= (int)cap - 2) return 0;
    }
    return 1;
}
#endif

static void play(SDL_Window *win) {
    char *pargv[64];
    char *vargv[64];
    int i;

    if (rom_count == 0) {
        status(0x00FF6666u, "no .nds found, press B to find one, or drag "
                            "one onto this window");
        return;
    }
    chan_make();
    build_port_args(pargv, 64);
    build_view_args(vargv, 64);
    cfg_save();

    print_cmd("port", pargv);
    print_cmd("view", vargv);

    if (dry_run) {
        /* Both lines on stdout, where a script reads them, and nothing
         * started. The channel name carries a pid, so a test matches on the
         * flags rather than on the whole line. */
        printf("would run:");
        for (i = 0; pargv[i] != NULL; i++) printf(" %s", pargv[i]);
        printf("\n");
        printf("would view:");
        for (i = 0; vargv[i] != NULL; i++) printf(" %s", vargv[i]);
        printf("\n");
        fflush(stdout);
        status(0x0088CC88u, "--dry-run: nothing started");
        (void)win;
        return;
    }

#if defined(_WIN32)
    {
        static char pcmd[2048], vcmd[2048];
        STARTUPINFOA si;
        PROCESS_INFORMATION ppi, vpi;
        DWORD code = 0;

        if (!win_cmdline(pargv, pcmd, sizeof pcmd) ||
            !win_cmdline(vargv, vcmd, sizeof vcmd)) {
            status(0x00FF6666u, "command line too long");
            return;
        }
        /*
         * The compatibility shim engine is kept out of the child, and this
         * is not a precaution; it is the bug that made the launcher look
         * broken while the same game started fine from a shell.
         *
         * A layer in `__COMPAT_LAYER` is inherited by every child, and a
         * shimmed process gets AcLayers.dll and apphelp.dll injected before
         * main(). Their heap is a pagefile-backed section of about twenty
         * megabytes, and Windows puts it low, measured here at 0x014B0000
         * through 0x028B1000, straight across the DS's ITCM at 0x01FF8000
         * and its main RAM at 0x02000000. The port maps the console's memory
         * at the console's own addresses, so it died on the first of them
         * with `VirtualAlloc failed for ITCM ... error 487` and the viewer,
         * finding no channel, exited 1. Setting the variable by hand on an
         * otherwise identical run from cmd.exe reproduces it exactly.
         *
         * Clearing it in this process clears it in the environment the
         * children inherit, which is the whole fix. Nothing here wants a
         * shim: both programs are built for the Windows they run on.
         */
        SetEnvironmentVariableA("__COMPAT_LAYER", NULL);

        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        memset(&ppi, 0, sizeof ppi);
        /*
         * Handles are inherited so that the port and the viewer print into
         * whatever the launcher was started from. A double-click has no
         * console and inherits nothing, which costs nothing; a run from a
         * terminal gets the children's own account of a failure, which is
         * what the message below promises.
         */
        if (!CreateProcessA(pargv[0], pcmd, NULL, NULL, TRUE, 0, NULL, NULL,
                            &si, &ppi)) {
            status(0x00FF6666u, "cannot start %s (error %lu)", pargv[0],
                   (unsigned long)GetLastError());
            return;
        }
        CloseHandle(ppi.hThread);

        memset(&vpi, 0, sizeof vpi);
        if (!CreateProcessA(vargv[0], vcmd, NULL, NULL, TRUE, 0, NULL, NULL,
                            &si, &vpi)) {
            status(0x00FF6666u, "the port started but %s did not (error %lu)",
                   vargv[0], (unsigned long)GetLastError());
            TerminateProcess(ppi.hProcess, 1);
            CloseHandle(ppi.hProcess);
            return;
        }
        CloseHandle(vpi.hThread);

        SDL_HideWindow(win);
        for (;;) {
            SDL_Event ev;

            if (WaitForSingleObject(vpi.hProcess, 0) == WAIT_OBJECT_0) break;
            while (SDL_PollEvent(&ev)) { /* stay responsive */ }
            SDL_Delay(100);
        }
        GetExitCodeProcess(vpi.hProcess, &code);
        CloseHandle(vpi.hProcess);
        TerminateProcess(ppi.hProcess, 0);
        WaitForSingleObject(ppi.hProcess, 2000);
        CloseHandle(ppi.hProcess);
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);

        if (code == 0) {
            status(0x0088CC88u, "run ended cleanly");
        } else {
            status(0x00FF6666u, "the viewer exited with code 0x%lX, its "
                                "console has the story", (unsigned long)code);
        }
    }
#else
    {
        pid_t pport, pview, got;
        int st = 0;

        pport = fork();
        if (pport < 0) {
            status(0x00FF6666u, "cannot fork: %s", strerror(errno));
            return;
        }
        if (pport == 0) {
            execv(pargv[0], pargv);
            fprintf(stderr, "pclaunch: cannot run %s: %s\n", pargv[0],
                    strerror(errno));
            _exit(127);
        }

        pview = fork();
        if (pview < 0) {
            status(0x00FF6666u, "cannot fork: %s", strerror(errno));
            kill(pport, SIGTERM);
            waitpid(pport, NULL, 0);
            return;
        }
        if (pview == 0) {
            execv(vargv[0], vargv);
            fprintf(stderr, "pclaunch: cannot run %s: %s\n", vargv[0],
                    strerror(errno));
            _exit(127);
        }

        SDL_HideWindow(win);
        for (;;) {
            SDL_Event ev;

            got = waitpid(pview, &st, WNOHANG);
            if (got == pview || (got < 0 && errno == ECHILD)) break;
            while (SDL_PollEvent(&ev)) { /* stay responsive, ignore all of it */ }
            SDL_Delay(100);
        }
        /* The window is gone; nothing is presenting the port. */
        kill(pport, SIGTERM);
        waitpid(pport, NULL, 0);
        shm_unlink_chan();
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);

        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            status(0x0088CC88u, "run ended cleanly");
        } else if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
            status(0x00FF6666u, "cannot start the viewer, is %s beside this "
                                "program?", VIEW_EXE);
        } else if (WIFEXITED(st)) {
            status(0x00FF6666u, "the viewer exited with status %d, its "
                                "stderr has the story", WEXITSTATUS(st));
        } else if (WIFSIGNALED(st)) {
            status(0x00FF6666u, "the viewer died on signal %d", WTERMSIG(st));
        }
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Drawing the menu                                                    */
/* ------------------------------------------------------------------ */

#define ROW_Y0     96
#define ROW_H      30
#define LABEL_X    32
#define VALUE_X    290
#define VALUE_W    (CANVAS_W - VALUE_X - 32)

static int row_at(int my) {
    int r = (my - ROW_Y0) / ROW_H;

    if (my < ROW_Y0 || r < 0 || r >= ROW_COUNT) return -1;
    return r;
}

static void draw_menu(int sel) {
    int i;

    fill(0, 0, CANVAS_W, CANVAS_H, 0x00101418u);
    fill(0, 0, CANVAS_W, 64, 0x00182028u);
    text(LABEL_X, 14, 2, 0x00E8E8F0u, "POKEMON PLATINUM");
    text(LABEL_X, 42, 1, 0x008899AAu,
         "arrows pick, Enter changes, B browses, Backspace resets, "
         "Esc quits");

    for (i = 0; i < ROW_COUNT; i++) {
        int y = ROW_Y0 + i * ROW_H;
        int on = i == sel;
        uint32_t lab = on ? 0x00FFFFFFu : 0x00AABBCCu;
        uint32_t val = on ? 0x00FFE080u : 0x00C8B060u;

        if (on) fill(LABEL_X - 12, y - 4, CANVAS_W - 2 * (LABEL_X - 12),
                     ROW_H - 4, 0x00202A34u);

        if (i == ROW_PLAY) {
            uint32_t c = rom_count == 0 ? 0x00667766u
                       : on ? 0x0090FF90u : 0x0060C060u;

            fill(LABEL_X - 12, y - 4, 140, ROW_H - 4,
                 on ? 0x00243424u : 0x001A241Au);
            text(LABEL_X + 34, y, 1, c, "PLAY");
            continue;
        }
        if (i == ROW_QUIT) {
            text(LABEL_X + 34, y, 1, on ? 0x00FF9090u : 0x00AA7070u, "Quit");
            continue;
        }

        text(LABEL_X, y, 1, lab, "%s", rows[i].label);
        /* Dim a row the current settings make inert, rather than hiding it
         * and making the menu jump under the cursor. Integer scaling is what
         * stretch overrides: the viewer cannot both fill the window and keep
         * whole pixels, and it resolves that in favour of stretch. */
        if (i == ROW_INTSCALE && rows[ROW_ASPECT].idx == 1) val = 0x00555555u;
        if (i == ROW_RES && rows[ROW_FX].idx >= 2 && rows[ROW_RES].idx == 0) {
            text(VALUE_X, y, 1, val, "512x384 (2x, this filter needs it)");
        } else {
            text(VALUE_X, y, 1, val, "< %s >", rows[i].vals[rows[i].idx]);
        }
    }

    {
        int hy = ROW_Y0 + ROW_COUNT * ROW_H + 8;

        if (sel >= 0 && sel < ROW_COUNT && rows[sel].help[0] != '\0') {
            text(LABEL_X, hy, 1, 0x00778899u, "%s", rows[sel].help);
        }
        fill(0, CANVAS_H - 28, CANVAS_W, 28, 0x00182028u);
        text(LABEL_X, CANVAS_H - 22, 1, status_col, "%s", status_line);
    }
}

static void cycle(int r, int dir) {
    if (r < 0 || r >= ROW_COUNT || rows[r].n <= 1) return;
    rows[r].idx = (rows[r].idx + dir + rows[r].n) % rows[r].n;
}

/* ------------------------------------------------------------------ */
/* Driving the menu from a script                                      */
/* ------------------------------------------------------------------ */

/*
 * `--keys return` pushes one keydown per frame into SDL's own queue and then
 * quits, so the handler under test is the handler a person's fingers reach,
 * not a restatement of it in a test. With SDL_VIDEODRIVER=dummy that runs
 * with no display, which is what makes "press nothing but Enter and be
 * playing" something a test can assert rather than something a run can
 * claim.
 *
 * An unknown name is a hard error: a typo that silently pressed nothing would
 * turn every assertion after it vacuous.
 */
static const struct { const char *name; int sym; } key_names[] = {
    { "up", SDLK_UP }, { "down", SDLK_DOWN },
    { "left", SDLK_LEFT }, { "right", SDLK_RIGHT },
    { "return", SDLK_RETURN }, { "space", SDLK_SPACE },
    { "b", SDLK_b }, { "backspace", SDLK_BACKSPACE },
    { "escape", SDLK_ESCAPE },
};

#define MAX_SCRIPT 64
static int script[MAX_SCRIPT];
static int script_n, script_i;

static int script_parse(const char *list) {
    const char *p = list;

    while (*p != '\0' && script_n < MAX_SCRIPT) {
        const char *comma = strchr(p, ',');
        size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
        size_t k;
        int found = 0;

        for (k = 0; k < sizeof key_names / sizeof key_names[0]; k++) {
            if (strlen(key_names[k].name) == len &&
                strncmp(key_names[k].name, p, len) == 0) {
                script[script_n++] = key_names[k].sym;
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "pclaunch: --keys: no key named '%.*s'\n",
                    (int)len, p);
            return 0;
        }
        if (comma == NULL) break;
        p = comma + 1;
    }
    return 1;
}

static void usage(void) {
    size_t k;

    printf("pclaunch, the launcher for the PC port of Pokemon Platinum.\n"
           "\n"
           "Run it with no arguments: pick your own .nds dump once, press\n"
           "Enter, play. Everything below is for scripts and tests.\n"
           "\n"
           "  --print-args [key=N ...]  print the command the menu would run\n"
           "                            and exit; savefile=PATH also works\n"
           "  --keys a,b,c              press these, one per frame, then quit\n"
           "  --dry-run                 PLAY prints `would run: ...` and\n"
           "                            starts nothing\n"
           "  --help                    this\n"
           "\n"
           "  $POKEPLATINUM_ROM a ROM to offer first\n"
           "  $PCLAUNCH_PICKER  the command that opens the file dialog; its\n"
           "                    first line of stdout is the chosen path\n"
           "\n"
           "Key names for --keys:");
    for (k = 0; k < sizeof key_names / sizeof key_names[0]; k++) {
        printf(" %s", key_names[k].name);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int sel;
    int quit = 0;
    int i;

    find_exe_dir(argv[0]);
    rom_scan();
    cfg_load();

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    /* The command this menu would run, printed and nothing started, how a
     * script or a test reads the composition without a display. Optional
     * `key=N` pairs override the saved settings first. */
    if (argc > 1 && strcmp(argv[1], "--print-args") == 0) {
        char *args[64];
        int i, j;

        for (i = 2; i < argc; i++) {
            char key[64];
            int v;

            /* savefile=PATH carries a path, not a ring index. */
            if (strncmp(argv[i], "savefile=", 9) == 0) {
                save_set(argv[i] + 9);
                continue;
            }
            if (sscanf(argv[i], "%63[^=]=%d", key, &v) != 2) continue;
            for (j = 0; j < ROW_COUNT; j++) {
                if (rows[j].key != NULL && strcmp(key, rows[j].key) == 0 &&
                    v >= 0 && v < rows[j].n) {
                    rows[j].idx = v;
                }
            }
        }
        if (rom_count == 0) {
            snprintf(rom_paths[0], PATH_MAX_L, "(no rom)");
            rom_count = 1;
        }
        chan_make();
        build_port_args(args, 64);
        for (i = 0; args[i] != NULL; i++) {
            printf("%s%s", i > 0 ? " " : "", args[i]);
        }
        printf("\n");
        build_view_args(args, 64);
        for (i = 0; args[i] != NULL; i++) {
            printf("%s%s", i > 0 ? " " : "", args[i]);
        }
        printf("\n");
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            if (!script_parse(argv[++i])) return 2;
        } else {
            fprintf(stderr, "pclaunch: %s? see --help\n", argv[i]);
            return 2;
        }
    }

    /*
     * Where the cursor starts is the whole of "double-click, Enter,
     * playing". On the ROM row Enter cycles the ring, so a person who did
     * exactly what the README says would get a different ROM and no game,
     * with PLAY eleven rows down. With a dump already known there is nothing to
     * configure, so start on the button. With none, start where the fix is.
     */
    sel = rom_count > 0 ? ROW_PLAY : ROW_ROM;

    if (rom_count > 0) {
        status(0x0088AACCu, "Enter plays %s, arrows for the settings above",
               rom_names[rows[ROW_ROM].idx]);
    } else {
        status(0x00FF6666u, "no .nds dump found, choose your own");
    }

    /*
     * Windows display scaling. Without this the compositor treats the
     * window as 96-DPI and bitmap-stretches it to the monitor's real
     * scale, which blurs every pixel however sharply it was rendered;
     * the classic "why does raising the resolution make it softer".
     * Per-monitor awareness makes the window's pixels the monitor's.
     * Ignored everywhere that is not Windows.
     */
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "pclaunch: SDL cannot open a display: %s\n"
                        "  The port itself runs headless: see "
                        "./pokeplatinum --help\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("Pok\xc3\xa9mon Platinum", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, CANVAS_W, CANVAS_H, 0);
    ui_window = win;
    if (win == NULL) {
        fprintf(stderr, "pclaunch: cannot open a window: %s\n",
                SDL_GetError());
        SDL_Quit();
        return 1;
    }
    pc_set_window_icon(win);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                      SDL_RENDERER_PRESENTVSYNC);
    if (ren == NULL) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    tex = ren != NULL
        ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888,
                            SDL_TEXTUREACCESS_STREAMING, CANVAS_W, CANVAS_H)
        : NULL;
    if (tex == NULL) {
        fprintf(stderr, "pclaunch: cannot draw: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /*
     * The other half: a first run with nothing findable opens the picker
     * by itself rather than showing "(none found)" and waiting to be
     * understood. Once, after the window exists so the dialog has something
     * to sit in front of, and only when there is genuinely nothing, a
     * remembered ROM means this never fires again, which is the "asked for
     * once" in the task.
     */
    if (rom_count == 0) {
        char picked[PATH_MAX_L];

        fprintf(stderr, "pclaunch: no .nds dump found; asking for one.\n");
        if (browse_file("Choose a Pokemon Platinum .nds",
                        picked, sizeof picked)) {
            if (rom_adopt(picked)) {
                status(0x0088CC88u, "Enter plays %s",
                       rom_names[rom_count - 1]);
                sel = ROW_PLAY;
                cfg_save();       /* asked once means remembered from now on */
            } else {
                status(0x00FF6666u, "cannot read %s", picked);
            }
        } else if (status_line[0] == '\0' ||
                   strncmp(status_line, "no .nds", 7) == 0) {
            /* browse_file leaves its own message when there was no dialog to
             * open; only speak for it when it did open and was cancelled. */
            status(0x00FFAA66u, "no dump chosen, press B to try again, or "
                                "drag a .nds onto this window");
        }
    }

    while (!quit) {
        SDL_Event ev;

        /*
         * --keys: one scripted keydown per frame into SDL's own queue, then
         * quit. Pushing rather than calling the handler directly is the
         * point; the path under test is the path a person's fingers take.
         */
        if (script_n > 0) {
            if (script_i < script_n) {
                SDL_Event k;

                memset(&k, 0, sizeof k);
                k.type = SDL_KEYDOWN;
                k.key.keysym.sym = script[script_i++];
                SDL_PushEvent(&k);
            } else {
                quit = 1;
            }
        }

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = 1;
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_UP:
                    sel = (sel + ROW_COUNT - 1) % ROW_COUNT;
                    break;
                case SDLK_DOWN:
                    sel = (sel + 1) % ROW_COUNT;
                    break;
                case SDLK_LEFT: cycle(sel, -1); break;
                case SDLK_RIGHT: cycle(sel, +1); break;
                case SDLK_b:
                    /* Browse, for the two rows that name a file. */
                    if (sel == ROW_ROM || sel == ROW_SAVE) {
                        char picked[PATH_MAX_L];

                        if (browse_file(sel == ROW_ROM
                                        ? "Choose a .nds dump"
                                        : "Choose a save file",
                                        picked, sizeof picked)) {
                            if (sel == ROW_ROM) {
                                if (rom_adopt(picked)) {
                                    status(0x0088CC88u, "ROM: %s",
                                           rom_names[rom_count - 1]);
                                } else if (rom_count > 0) {
                                    status(0x00FFAA66u, "already in the "
                                           "list, or not a readable file");
                                }
                            } else {
                                save_set(picked);
                                status(0x0088CC88u, "save file: %s",
                                       save_label);
                            }
                        }
                    }
                    break;
                case SDLK_BACKSPACE:
                case SDLK_DELETE:
                    /* Back to the default. */
                    if (sel == ROW_ROM) {
                        rows[ROW_ROM].idx = 0;
                        status(0x0088AACCu, "ROM: back to the first one "
                               "found by the scan");
                    } else if (sel == ROW_SAVE) {
                        save_reset();
                        status(0x0088AACCu, "save: back to the default, "
                               "beside the ROM");
                    }
                    break;
                case SDLK_RETURN:
                case SDLK_SPACE:
                    if (sel == ROW_PLAY) play(win);
                    else if (sel == ROW_QUIT) quit = 1;
                    else cycle(sel, +1);
                    break;
                default: break;
                }
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN) {
                int r = row_at(ev.button.y);

                if (r >= 0) {
                    sel = r;
                    if (r == ROW_PLAY) play(win);
                    else if (r == ROW_QUIT) quit = 1;
                    else cycle(r, ev.button.button == SDL_BUTTON_RIGHT
                                  ? -1 : +1);
                }
            }
            /*
             * A scripted run ignores the pointer. --keys exists so a test can
             * drive this program, and the pointer sitting anywhere over the
             * window moved the selection out from under the next scripted
             * key: measured on Windows, where a real mouse exists and the
             * motion event arrives in the same poll burst as the injected
             * one. Under SDL's dummy driver there is no pointer and the bug
             * was invisible, which is why the Windows launcher had never
             * been drivable at all.
             */
            if (ev.type == SDL_MOUSEMOTION && script_n == 0) {
                int r = row_at(ev.motion.y);

                if (r >= 0) sel = r;
            }
            if (ev.type == SDL_DROPFILE && ev.drop.file != NULL) {
                /* A dropped .nds is a ROM; anything else is a save file.
                 * The one gesture that works on every machine, dialogs or
                 * not. */
                size_t len = strlen(ev.drop.file);

                if (len > 4 &&
                    strcmp(ev.drop.file + len - 4, ".nds") == 0) {
                    if (rom_adopt(ev.drop.file)) {
                        status(0x0088CC88u, "ROM: %s",
                               rom_names[rom_count - 1]);
                        /* If they are still on the ROM row; which is where
                         * a launcher with no dump leaves them, the one
                         * thing missing has just arrived, so put the cursor
                         * on PLAY rather than making them find it. Anyone
                         * who has navigated away keeps their place. */
                        if (sel == ROW_ROM) sel = ROW_PLAY;
                    }
                } else {
                    save_set(ev.drop.file);
                    status(0x0088CC88u, "save file: %s", save_label);
                }
                SDL_free(ev.drop.file);
            }
        }

        draw_menu(sel);
        SDL_UpdateTexture(tex, NULL, canvas, CANVAS_W * (int)sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    cfg_save();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
