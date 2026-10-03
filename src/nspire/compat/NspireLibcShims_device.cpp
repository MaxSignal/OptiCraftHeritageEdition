// Symbols the Ndless runtime does not provide but libstdc++/libgcc reference.
//
//   usleep/sleep        std::this_thread::sleep_for (msleep underneath)
//   symlink, readlink,  std::filesystem's link and resize operations; the
//   pathconf, truncate  calculator's filesystem has none of them
//   __atomic_*_N        std::atomic: ARMv5 has no exclusive
//                       access, so libgcc expects a libatomic. With a single
//                       thread and no preemption of game code, plain memory
//                       operations are atomic enough.
#if defined(NSPIRE_PLATFORM) && defined(_TINSPIRE)

#include <libndls.h>
// libndls turns sleep() into a compile error pointing at msleep(); this file
// is where the POSIX sleep() libstdc++ needs gets defined.
#undef sleep

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/types.h>

extern "C" {

int usleep(useconds_t usec)
{
    msleep(static_cast<unsigned>((usec + 999u) / 1000u));
    return 0;
}

unsigned sleep(unsigned seconds)
{
    msleep(seconds * 1000u);
    return 0;
}

int symlink(const char*, const char*)
{
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char*, char*, size_t)
{
    errno = ENOSYS;
    return -1;
}

long pathconf(const char*, int)
{
    errno = ENOSYS;
    return -1;
}

int truncate(const char*, off_t)
{
    errno = ENOSYS;
    return -1;
}

std::uint64_t __atomic_load_8(const volatile void* ptr, int)
{
    return *static_cast<const volatile std::uint64_t*>(ptr);
}

void __atomic_store_8(volatile void* ptr, std::uint64_t value, int)
{
    *static_cast<volatile std::uint64_t*>(ptr) = value;
}

std::uint64_t __atomic_exchange_8(volatile void* ptr, std::uint64_t value, int)
{
    volatile std::uint64_t* p = static_cast<volatile std::uint64_t*>(ptr);
    const std::uint64_t old = *p;
    *p = value;
    return old;
}

bool __atomic_compare_exchange_8(volatile void* ptr, void* expected, std::uint64_t desired, bool, int, int)
{
    volatile std::uint64_t* p = static_cast<volatile std::uint64_t*>(ptr);
    std::uint64_t* e = static_cast<std::uint64_t*>(expected);
    if (*p == *e)
    {
        *p = desired;
        return true;
    }
    *e = *p;
    return false;
}

#define NSPIRE_ATOMIC_FETCH_OP(name, op)                                             \
    std::uint64_t __atomic_fetch_##name##_8(volatile void* ptr, std::uint64_t v, int) \
    {                                                                                \
        volatile std::uint64_t* p = static_cast<volatile std::uint64_t*>(ptr);       \
        const std::uint64_t old = *p;                                                \
        *p = old op v;                                                               \
        return old;                                                                  \
    }                                                                                \
    std::uint64_t __atomic_##name##_fetch_8(volatile void* ptr, std::uint64_t v, int) \
    {                                                                                \
        volatile std::uint64_t* p = static_cast<volatile std::uint64_t*>(ptr);       \
        const std::uint64_t value = *p op v;                                         \
        *p = value;                                                                  \
        return value;                                                                \
    }

NSPIRE_ATOMIC_FETCH_OP(add, +)
NSPIRE_ATOMIC_FETCH_OP(sub, -)
NSPIRE_ATOMIC_FETCH_OP(and, &)
NSPIRE_ATOMIC_FETCH_OP(or, |)
NSPIRE_ATOMIC_FETCH_OP(xor, ^)

#undef NSPIRE_ATOMIC_FETCH_OP

// The same for the 1-, 2- and 4-byte cases: ARMv5TE has no LDREX/STREX, so GCC
// calls out for every std::atomic read-modify-write.
#define NSPIRE_ATOMIC_SIZED(N, T)                                                       \
    T __atomic_load_##N(const volatile void* ptr, int) { return *static_cast<const volatile T*>(ptr); } \
    void __atomic_store_##N(volatile void* ptr, T value, int) { *static_cast<volatile T*>(ptr) = value; } \
    T __atomic_exchange_##N(volatile void* ptr, T value, int)                           \
    {                                                                                   \
        volatile T* p = static_cast<volatile T*>(ptr);                                  \
        const T old = *p;                                                               \
        *p = value;                                                                     \
        return old;                                                                     \
    }                                                                                   \
    bool __atomic_compare_exchange_##N(volatile void* ptr, void* expected, T desired, bool, int, int) \
    {                                                                                   \
        volatile T* p = static_cast<volatile T*>(ptr);                                  \
        T* e = static_cast<T*>(expected);                                               \
        if (*p == *e) { *p = desired; return true; }                                    \
        *e = *p;                                                                        \
        return false;                                                                   \
    }                                                                                   \
    T __atomic_fetch_add_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); const T o = *p; *p = o + v; return o; } \
    T __atomic_fetch_sub_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); const T o = *p; *p = o - v; return o; } \
    T __atomic_fetch_and_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); const T o = *p; *p = o & v; return o; } \
    T __atomic_fetch_or_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); const T o = *p; *p = o | v; return o; } \
    T __atomic_fetch_xor_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); const T o = *p; *p = o ^ v; return o; } \
    T __atomic_add_fetch_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); *p = *p + v; return *p; } \
    T __atomic_sub_fetch_##N(volatile void* ptr, T v, int) { volatile T* p = static_cast<volatile T*>(ptr); *p = *p - v; return *p; }

NSPIRE_ATOMIC_SIZED(1, unsigned char)
NSPIRE_ATOMIC_SIZED(2, unsigned short)
NSPIRE_ATOMIC_SIZED(4, unsigned int)

#undef NSPIRE_ATOMIC_SIZED

} // extern "C"

#endif
