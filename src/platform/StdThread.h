#pragma once

// std::thread for code that spawns helper threads directly (network, stats
// sync, downloads). On platforms without threads (PLATFORM_NO_THREADS: the
// TI-Nspire, whose libstdc++ is built without gthreads and so has no usable
// std::thread constructor) PlatformStdThread is an inert handle: constructing
// it with a callable does not run it, and it is never joinable. Every such
// thread in the codebase serves a feature those platforms do not have
// (multiplayer, online stats, skin downloads), so nothing waits on its work.
// Work the game does depend on is made synchronous at its source instead
// (see ThreadedFileIOBase, PlatformThread).

#include "platform/PlatformConfig.h"

#if PLATFORM_NO_THREADS

#include <thread>
#include <utility>

#include "platform/Log.h"

class PlatformStdThread
{
public:
    PlatformStdThread() noexcept = default;

    template <class Function, class... Args>
    explicit PlatformStdThread(Function&&, Args&&...)
    {
        MC_LOG_INFO("thread", "thread not started: this platform has no threads\n");
    }

    PlatformStdThread(PlatformStdThread&&) noexcept = default;
    PlatformStdThread& operator=(PlatformStdThread&&) noexcept = default;
    PlatformStdThread(const PlatformStdThread&) = delete;
    PlatformStdThread& operator=(const PlatformStdThread&) = delete;

    bool joinable() const noexcept { return false; }
    // Never equal to the running (only) thread's id.
    std::thread::id get_id() const noexcept { return std::thread::id(); }
    void join() {}
    void detach() {}
};

#else

#include <thread>
using PlatformStdThread = std::thread;

#endif
