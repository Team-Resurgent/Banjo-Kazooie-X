/* lhx_thread.h -- Banjo-Kazooie-X: a thread as Lighthouse uses std::thread (its game tick, the N64 threads it runs as
 * threads of their own, its timers), on the Xbox's own threads (xbox/src/xbox_glue.cpp). A std::thread here has none
 * of the Xbox's per-thread data, and file I/O on one (a resource the game loads, a save) crashes; an Xbox thread has
 * it, and SSE's flush-to-zero set as the main thread has (a denormal costs the Pentium III ~100 cycles).
 *
 * Each says its stack's size: an Xbox thread's stack is committed whole, on a 64 MB console. Lighthouse's files use
 * LHX_THREAD for the type and LHX_THREAD_STACK(bytes) before a callable to start one. */
#pragma once

#include <exception>
#include <functional>
#include <utility>

extern "C" {
void* LusXbox_ThreadStart(void (*entry)(void*), void* arg, unsigned stackBytes);
void LusXbox_ThreadJoin(void* thread);
void LusXbox_ThreadClose(void* thread);
}

namespace lhx {

class Thread {
  public:
    Thread() = default;

    template <typename F> Thread(unsigned stackBytes, F&& entry) {
        auto* fn = new std::function<void()>(std::forward<F>(entry));
        mThread = LusXbox_ThreadStart(&Run, fn, stackBytes);
        if (mThread == nullptr) {
            delete fn;
        }
    }

    Thread(Thread&& other) noexcept : mThread(other.mThread) {
        other.mThread = nullptr;
    }

    Thread& operator=(Thread&& other) noexcept {
        if (mThread != nullptr) { // as std::thread: a joinable thread isn't overwritten
            std::terminate();
        }
        mThread = other.mThread;
        other.mThread = nullptr;
        return *this;
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    ~Thread() {
        if (mThread != nullptr) {
            std::terminate();
        }
    }

    bool joinable() const {
        return mThread != nullptr;
    }

    void join() {
        LusXbox_ThreadJoin(mThread);
        LusXbox_ThreadClose(mThread);
        mThread = nullptr;
    }

    void detach() {
        LusXbox_ThreadClose(mThread);
        mThread = nullptr;
    }

  private:
    static void Run(void* arg) {
        auto* fn = static_cast<std::function<void()>*>(arg);
        (*fn)();
        delete fn;
    }

    void* mThread = nullptr;
};

} // namespace lhx

#define LHX_THREAD lhx::Thread
#define LHX_THREAD_STACK(bytes) (bytes),
