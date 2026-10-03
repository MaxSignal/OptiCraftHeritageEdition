#pragma once

// Single-threaded stand-ins for the C++ thread-support types Ndless lacks.
//
// The Ndless toolchain builds GCC with --disable-threads, so libstdc++ has no
// gthreads: <mutex> and <condition_variable> declare no std::mutex or
// std::condition_variable at all, and std::thread has no usable constructor.
// The calculator has no threads to synchronise either, so shared code that
// guards data with a mutex is correct with a mutex that does nothing.
//
// This header is force-included into every translation unit of the Nspire
// build (cmake/nspire.cmake). It is inert wherever libstdc++ does have threads
// (the host simulator), so both builds see the same source.
//
//   std::mutex & co.          no-op locks (always acquired)
//   std::condition_variable   wait() returns at once; wait(lock, pred) returns
//                             with pred() as it stands -- there is no other
//                             thread that could ever make it true
//
// std::thread is left alone: construction sites go through PlatformStdThread
// (platform/StdThread.h) instead.

#if defined(__cplusplus) && defined(NSPIRE_PLATFORM)

#include <bits/c++config.h>

#if !defined(_GLIBCXX_HAS_GTHREADS)

#include <chrono>
#include <bits/std_mutex.h>
#include <bits/unique_lock.h>

namespace std
{
class mutex
{
public:
    constexpr mutex() noexcept = default;
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    void lock() {}
    bool try_lock() { return true; }
    void unlock() {}
};

class recursive_mutex : public mutex {};
class timed_mutex : public mutex
{
public:
    template <class Rep, class Period>
    bool try_lock_for(const chrono::duration<Rep, Period>&) { return true; }
    template <class Clock, class Duration>
    bool try_lock_until(const chrono::time_point<Clock, Duration>&) { return true; }
};
class recursive_timed_mutex : public timed_mutex {};

template <class... Mutexes>
class scoped_lock
{
public:
    explicit scoped_lock(Mutexes&...) {}
    explicit scoped_lock(adopt_lock_t, Mutexes&...) {}
    scoped_lock(const scoped_lock&) = delete;
    scoped_lock& operator=(const scoped_lock&) = delete;
};

enum class cv_status { no_timeout, timeout };

class condition_variable
{
public:
    condition_variable() noexcept = default;
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    void notify_one() noexcept {}
    void notify_all() noexcept {}

    template <class Lock>
    void wait(Lock&) {}

    template <class Lock, class Predicate>
    void wait(Lock&, Predicate) {}

    template <class Lock, class Rep, class Period>
    cv_status wait_for(Lock&, const chrono::duration<Rep, Period>&) { return cv_status::timeout; }

    template <class Lock, class Rep, class Period, class Predicate>
    bool wait_for(Lock&, const chrono::duration<Rep, Period>&, Predicate pred) { return pred(); }

    template <class Lock, class Clock, class Duration>
    cv_status wait_until(Lock&, const chrono::time_point<Clock, Duration>&) { return cv_status::timeout; }

    template <class Lock, class Clock, class Duration, class Predicate>
    bool wait_until(Lock&, const chrono::time_point<Clock, Duration>&, Predicate pred) { return pred(); }
};

using condition_variable_any = condition_variable;
} // namespace std

#endif // !_GLIBCXX_HAS_GTHREADS
#endif // __cplusplus && NSPIRE_PLATFORM
