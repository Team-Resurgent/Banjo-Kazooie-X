// xbox_glue.cpp -- what the parts Banjo-Kazooie-X takes from elsewhere need of the Xbox: new and delete on the
// clock, Xbox threads, and the strlen guard.
#include <xtl.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <new>
#include <stdlib.h>

#include "lusx_debug.h"
#include "lusx_log.h"

// new and delete as ever, on the clock: what the heap costs a tick, for the log (BenPort.cpp). Not atomic: the
// other threads' calls are rare, and a lost count is no matter here.
extern "C" {
uint64_t gLusxHeapCycles;
uint32_t gLusxHeapCalls;
}

static inline void* HeapNew(size_t size) {
    const uint64_t began = __builtin_ia32_rdtsc();
    void* p = malloc(size != 0 ? size : 1);
    gLusxHeapCycles += __builtin_ia32_rdtsc() - began;
    gLusxHeapCalls++;
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

static inline void HeapDelete(void* p) {
    const uint64_t began = __builtin_ia32_rdtsc();
    free(p);
    gLusxHeapCycles += __builtin_ia32_rdtsc() - began;
    gLusxHeapCalls++;
}

void* operator new(size_t size) {
    return HeapNew(size);
}
void* operator new[](size_t size) {
    return HeapNew(size);
}
void operator delete(void* p) noexcept {
    HeapDelete(p);
}
void operator delete[](void* p) noexcept {
    HeapDelete(p);
}
void operator delete(void* p, size_t) noexcept {
    HeapDelete(p);
}
void operator delete[](void* p, size_t) noexcept {
    HeapDelete(p);
}

// libultraship's resource manager calls this after each load (ResourceManager.cpp): 2Ship's port fed its audio there
// through a scene's long load. Lighthouse's audio has a thread of its own (src/port/Audio), which a load doesn't stop.
extern "C" void LusXbox_KeepAudioGoing(void) {
}

// 2Ship's audio thread (BenPort.cpp): above the game's, so a tick's synthesis runs in one piece as soon as
// it's asked for: one core, so it was cut into the renderer's work (and timings) otherwise
extern "C" unsigned LusXbox_ThreadId(void) {
    return (unsigned)GetCurrentThreadId();
}

// A thread with the Xbox's per-thread data, for one that may do file I/O: a std::thread has none, and CreateFile or
// anything else that sets the last error crashes on it.
static DWORD WINAPI ThreadEntry(LPVOID entry) {
    ((void (*)(void))entry)();
    return 0;
}

extern "C" void LusXbox_StartThread(void (*entry)(void), unsigned stackBytes) {
    CreateThread(NULL, stackBytes, ThreadEntry, (LPVOID)entry, 0, NULL);
}

// Lighthouse's threads (xbox/include/lhx_thread.h): an Xbox thread each, with flush-to-zero as the main thread has
struct LhxThreadStart {
    void (*entry)(void*);
    void* arg;
};

// the crash report's handler at the bottom of the thread's exception chain, as main() has it (xbox_main.cpp): with
// xbWatson attached, an exception on a thread without one reached the debugger with no report
extern "C" int __cdecl LusXbox_SehHandler(EXCEPTION_RECORD* er, void* frame, CONTEXT* c, void* dispatcher);

struct LhxSehRecord {
    LhxSehRecord* next;
    void* handler;
};

static DWORD WINAPI LhxThreadEntry(LPVOID param) {
    LhxThreadStart start = *(LhxThreadStart*)param;
    LhxSehRecord seh; // on this thread's stack, as the kernel requires, for as long as the thread runs
    uint32_t csr;

    free(param);
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8000;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    seh.handler = (void*)LusXbox_SehHandler;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(seh.next));
    __asm__ volatile("movl %0, %%fs:0" : : "r"(&seh) : "memory");
    start.entry(start.arg);
    __asm__ volatile("movl %0, %%fs:0" : : "r"(seh.next) : "memory");
    return 0;
}

extern "C" void* LusXbox_ThreadStart(void (*entry)(void*), void* arg, unsigned stackBytes) {
    LhxThreadStart* start = (LhxThreadStart*)malloc(sizeof(LhxThreadStart));
    HANDLE h;

    if (start == NULL) {
        return NULL;
    }
    start->entry = entry;
    start->arg = arg;
    h = CreateThread(NULL, stackBytes, LhxThreadEntry, start, 0, NULL);
    if (h == NULL) {
        free(start);
    }
    return h;
}

extern "C" void LusXbox_ThreadJoin(void* thread) {
    WaitForSingleObject((HANDLE)thread, INFINITE);
}

extern "C" void LusXbox_ThreadClose(void* thread) {
    CloseHandle((HANDLE)thread);
}

extern "C" void LusXbox_RaiseThisThread(void) {
    // and flush-to-zero for its SSE maths, as main() set for the game's thread: MXCSR is a thread's own,
    // and a denormal stalls the P3 for ~100 cycles (no DAZ: the Coppermine P3 lacks it)
    uint32_t csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8000;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

// a thread that works only while the game's wait (port/lhx_hdprefetch.cpp: HD textures read ahead)
extern "C" void LusXbox_LowerThisThread(void) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
}

// and its wait between looks at the stream
extern "C" void LusXbox_Sleep(unsigned ms) {
    Sleep(ms);
}

// a file written whole (the audio recording for the diagnosis: BenPort.cpp); true if it was
extern "C" int LusXbox_WriteFile(const char* path, const void* data, unsigned size) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    BOOL ok = WriteFile(h, data, size, &written, NULL);
    CloseHandle(h);
    return ok && written == size;
}

// strlen, wrapped at link time (tools/build.py: --wrap=strlen): a string pointer below 64 KB is never
// a string (the title's image starts at 0x10000) -- strlen(0x1) crashed after the title demo's Goron
// scene. Logged once with the caller's address (llvm-symbolizer --obj=build/Lus-X.exe), and taken as
// an empty string.
extern "C" size_t __real_strlen(const char* s);

extern "C" size_t __wrap_strlen(const char* s) {
    static volatile LONG sReported;

    if ((uintptr_t)s < 0x10000) {
        if (gLusxLogOn && InterlockedExchange((LONG*)&sReported, 1) == 0) {
            char line[128];
            snprintf(line, sizeof(line), "lus-x: strlen(%p) called from %p\n", (const void*)s,
                     __builtin_return_address(0));
            LusxLog_Send(line);
        }
        return 0;
    }
    return __real_strlen(s);
}
