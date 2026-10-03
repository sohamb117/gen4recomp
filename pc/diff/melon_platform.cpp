/*
 * melonDS's Platform layer, for a headless oracle.
 *
 * Why this file exists at all. MelonDS's core is a library with no I/O of its
 * own: `src/Platform.h` declares about seventy functions the frontend owes it:
 * files, threads, logging, cameras, microphones, wifi, and the only
 * implementation in that repository is `frontend/qt_sdl/Platform.cpp`, which
 * drags in Qt. The differential runner needs the emulator and none of the
 * frontend, so it supplies its own.
 *
 * What is real and what is a stub, and the rule for telling them apart.
 * Everything the ARM9 can *observe* is implemented; everything it cannot is a
 * stub that returns "nothing there". File I/O is real because the cart is read
 * through it. Threads, semaphores and mutexes are real because the 3D
 * renderer's soft rasteriser may use them. The camera, the microphone, the
 * local multiplayer transport and the AAC decoder are stubs, and each one is
 * a *silent* stub deliberately: this is an oracle, and an oracle that printed
 * a warning per frame would bury the one line that matters. A stub that the
 * game reaches would show up as a divergence, which is the whole point of the
 * tool.
 *
 * DETERMINISM. Two things here could make a run irreproducible and both are
 * pinned. GetMSCount/GetUSCount return a counter that advances by a fixed step
 * per call rather than the host clock, the core uses them only for frame
 * pacing and logging, neither of which reaches guest memory, but a host clock
 * in this file would be exactly the leak the port's own determinism test
 * exists to catch on the other side. And WriteDateTime is ignored: the RTC
 * is set from --rtc, and letting the guest write it back would make the
 * reported time depend on something other than the epoch.
 *
 * LICENCE. melonDS is GPLv3-or-later and this file is written against its
 * headers, so the differential runner is a GPLv3 work, which this port
 * already is, because pc/hw/ carries melonDS-derived hardware models.
 * Nothing melonDS owns is copied into this repository; the core is built
 * from the checkout you point MELONDS at.
 */

#include "Platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>

namespace melonDS::Platform
{

/* ------------------------------------------------------------------ */
/* Stopping                                                            */
/* ------------------------------------------------------------------ */

/*
 * The core calls this when it decides it cannot go on, GBA mode, a bad
 * exception region, a power-off. The host has to know, because "the emulator
 * stopped early" and "the emulator ran N frames" are different traces and
 * only one of them is a valid oracle. melon_host.cpp reads this flag.
 */
int melon_stop_reason = -1;

void SignalStop(StopReason reason, void* /*userdata*/)
{
    melon_stop_reason = (int)reason;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

/*
 * A FileHandle is a FILE*. The core never looks inside one, and the header
 * says so, so the cast is the interface rather than a shortcut.
 */
struct FileHandle { FILE* fp; };

static FileHandle* wrap(FILE* fp)
{
    if (!fp) return nullptr;
    FileHandle* f = new FileHandle;
    f->fp = fp;
    return f;
}

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    const char* m;

    if (mode & FileMode::Write)
    {
        if (mode & FileMode::Preserve)
            m = (mode & FileMode::Read) ? "r+b" : "r+b";
        else
            m = (mode & FileMode::Read) ? "w+b" : "wb";
    }
    else
    {
        m = "rb";
    }
    if (mode & FileMode::NoCreate)
    {
        /* Only open something that is already there. */
        FILE* probe = fopen(path.c_str(), "rb");
        if (!probe) return nullptr;
        fclose(probe);
    }
    return wrap(fopen(path.c_str(), m));
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(path, mode);
}

bool FileExists(const std::string& name)
{
    FILE* f = fopen(name.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool LocalFileExists(const std::string& name) { return FileExists(name); }

bool CheckFileWritable(const std::string& /*path*/) { return false; }
bool CheckLocalFileWritable(const std::string& /*path*/) { return false; }

bool CloseFile(FileHandle* file)
{
    if (!file) return false;
    bool ok = fclose(file->fp) == 0;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle* file) { return file && feof(file->fp) != 0; }

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return file && fgets(str, count, file->fp) != nullptr;
}

u64 FilePosition(FileHandle* file) { return file ? (u64)ftell(file->fp) : 0; }

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int o = SEEK_SET;
    if (origin == FileSeekOrigin::Current) o = SEEK_CUR;
    else if (origin == FileSeekOrigin::End) o = SEEK_END;
    return file && fseek(file->fp, (long)offset, o) == 0;
}

void FileRewind(FileHandle* file) { if (file) rewind(file->fp); }

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? (u64)fread(data, (size_t)size, (size_t)count, file->fp) : 0;
}

bool FileFlush(FileHandle* file) { return file && fflush(file->fp) == 0; }

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? (u64)fwrite(data, (size_t)size, (size_t)count, file->fp) : 0;
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list ap;
    int n;
    if (!file) return 0;
    va_start(ap, fmt);
    n = vfprintf(file->fp, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : (u64)n;
}

u64 FileLength(FileHandle* file)
{
    long pos, end;
    if (!file) return 0;
    pos = ftell(file->fp);
    fseek(file->fp, 0, SEEK_END);
    end = ftell(file->fp);
    fseek(file->fp, pos, SEEK_SET);
    return (u64)end;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

/*
 * To stderr, and prefixed, because stdout is where the trace goes when the
 * host is asked for one on a pipe. The core is chatty at Debug level about
 * things a boot does normally (unmapped VRAM reads, SPI commands), so only
 * Warn and above get through unless the host asks for more.
 */
int melon_log_level = (int)LogLevel::Warn;

void Log(LogLevel level, const char* fmt, ...)
{
    va_list ap;
    if ((int)level < melon_log_level) return;
    fputs("melon: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Threads and synchronisation                                         */
/* ------------------------------------------------------------------ */

struct Thread { std::thread t; };

Thread* Thread_Create(std::function<void()> func)
{
    Thread* th = new Thread;
    th->t = std::thread(func);
    return th;
}

void Thread_Free(Thread* thread)
{
    if (!thread) return;
    if (thread->t.joinable()) thread->t.join();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread && thread->t.joinable()) thread->t.join();
}

struct Semaphore
{
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* sema) { delete sema; }

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> lk(sema->m);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> lk(sema->m);
    sema->cv.wait(lk, [&] { return sema->count > 0; });
    sema->count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(sema->m);
    if (timeout_ms <= 0)
    {
        if (sema->count <= 0) return false;
    }
    else if (!sema->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [&] { return sema->count > 0; }))
    {
        return false;
    }
    sema->count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    std::lock_guard<std::mutex> lk(sema->m);
    sema->count += count;
    sema->cv.notify_all();
}

struct Mutex { std::mutex m; };

Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->m.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->m.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->m.try_lock(); }

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

/*
 * Not the host clock. See the header: the core uses these for pacing and for
 * log timestamps, and a real clock here is the one thing in this file that
 * could make two runs of the same script disagree.
 */
static u64 fake_us = 0;

u64 GetMSCount() { fake_us += 1000; return fake_us / 1000; }
u64 GetUSCount() { fake_us += 1000; return fake_us; }

/* ------------------------------------------------------------------ */
/* Saves, firmware and the clock                                       */
/* ------------------------------------------------------------------ */

/*
 * Discarded rather than written. A save file would make the oracle depend on
 * a previous run of itself, which is the same mistake as letting the port
 * read a host clock: the trace has to be a function of --rom, --rtc and
 * --input and nothing else, exactly as the port's is.
 */
void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

/* ------------------------------------------------------------------ */
/* Things this console has and this game does not use                  */
/* ------------------------------------------------------------------ */

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*)
{
    if (frame) memset(frame, 0, (size_t)width * (size_t)height * 4);
}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}
