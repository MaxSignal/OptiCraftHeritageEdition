// NspireSystem for the headless desktop simulator (NSPIRE_HOST_SIM).
//
// Runs the exact Nspire port -- same PlatformConfig, tuning, lwjgl shims, nGL
// renderer and storage -- on a PC, so the port can be exercised without a
// calculator or an emulator ROM. Environment:
//
//   NSPIRE_SIM_APPDIR   directory standing in for /documents/ndless (game data,
//                       saves). Default: the executable's directory.
//   NSPIRE_SIM_OUT      where frames are written as PPM. Default: <appdir>/frames
//   NSPIRE_SIM_DUMP_EVERY  also dump every Nth presented frame (0 = off).
//   NSPIRE_SIM_SCRIPT   keypad script, one command per line, keyed on the
//                       presented-frame number:
//                         <frame> down <KEY>     hold a key (names: NspireKeys.cpp)
//                         <frame> up <KEY>
//                         <frame> tap <KEY>      press for two frames
//                         <frame> shot <name>    write <name>.ppm
//                         <frame> quit
//   NSPIRE_SIM_MAX_FRAMES  hard stop (default 100000).
//   NSPIRE_SIM_FRAME_MS    virtual clock: every clock in the process (std::chrono,
//                       gettimeofday, micros) advances this many milliseconds per
//                       presented frame instead of following real time -- runs
//                       the game at calculator-like frame rates (e.g. 250).
//   NSPIRE_SIM_SLOWDOWN    scaled clock: every clock reports real elapsed time
//                       multiplied by this factor, so per-frame time budgets
//                       (chunk builds, ticks) run out mid-way as they do on the
//                       calculator. Ignored when NSPIRE_SIM_FRAME_MS is set.
#if defined(NSPIRE_PLATFORM) && !defined(_TINSPIRE)

#include "nspire/NspireSystem.h"
#include "nspire/input/NspireKeys.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <malloc.h>
#include <ctime>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct ScriptEvent
{
    long frame;
    std::string command;
    std::string argument;
};

std::string g_appDir = ".";
std::string g_outDir;
std::vector<std::uint16_t> g_backBuffer;
std::vector<ScriptEvent> g_script;
std::size_t g_scriptPos = 0;
bool g_keys[NK_COUNT] = {};
std::vector<std::pair<long, int>> g_pendingReleases;
long g_frame = 0;
long g_dumpEvery = 0;
long g_maxFrames = 100000;
bool g_exit = false;
std::chrono::steady_clock::time_point g_start;
long g_frameMs = 0;              // 0: real time
std::uint64_t g_virtualUs = 0;   // virtual clock, advanced per frame and per read
constexpr std::uint64_t kVirtualEpochSec = 1790000000ull;

long g_slowdown = 0;              // 0: off
std::uint64_t g_realStartUs = 0;

std::uint64_t kernelMonotonicUs()
{
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
}

std::uint64_t scaledNowUs()
{
    return (kernelMonotonicUs() - g_realStartUs) * static_cast<std::uint64_t>(g_slowdown);
}

std::uint64_t virtualNowUs()
{
    if (g_slowdown > 0)
        return scaledNowUs();
    // Every read moves time forward a little, so code that spins until some
    // time has passed still terminates.
    return ++g_virtualUs;
}

const char* env(const char* name)
{
    const char* value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}

void writePpm(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", NspireSystem::kScreenWidth, NspireSystem::kScreenHeight);
    for (std::uint16_t c : g_backBuffer)
    {
        const unsigned char rgb[3] = {
            static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31),
            static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63),
            static_cast<unsigned char>((c & 31) * 255 / 31),
        };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

void loadScript(const char* path)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        ScriptEvent ev;
        if (!(ls >> ev.frame >> ev.command))
            continue;
        ls >> ev.argument;
        g_script.push_back(ev);
    }
}

void setKey(const std::string& name, bool down)
{
    const int key = nspireKeyFromName(name.c_str());
    if (key < 0)
    {
        std::fprintf(stderr, "[sim] unknown key '%s'\n", name.c_str());
        return;
    }
    g_keys[key] = down;
}

void runScript()
{
    for (auto it = g_pendingReleases.begin(); it != g_pendingReleases.end();)
    {
        if (it->first <= g_frame)
        {
            g_keys[it->second] = false;
            it = g_pendingReleases.erase(it);
        }
        else
            ++it;
    }
    while (g_scriptPos < g_script.size() && g_script[g_scriptPos].frame <= g_frame)
    {
        const ScriptEvent& ev = g_script[g_scriptPos++];
        if (ev.command == "down")
            setKey(ev.argument, true);
        else if (ev.command == "up")
            setKey(ev.argument, false);
        else if (ev.command == "tap")
        {
            setKey(ev.argument, true);
            const int key = nspireKeyFromName(ev.argument.c_str());
            if (key >= 0)
                g_pendingReleases.push_back({g_frame + 2, key});
        }
        else if (ev.command == "shot")
        {
            writePpm(g_outDir + "/" + ev.argument + ".ppm");
            std::fprintf(stderr, "[sim] frame %ld -> %s.ppm\n", g_frame, ev.argument.c_str());
        }
        else if (ev.command == "quit")
            g_exit = true;
    }
}
} // namespace

namespace NspireSystem
{
void initialize(int argc, char** argv)
{
    g_start = std::chrono::steady_clock::now();
    if (const char* dir = env("NSPIRE_SIM_APPDIR"))
        g_appDir = dir;
    else if (argc > 0 && argv && argv[0])
    {
        std::string path = argv[0];
        const std::size_t slash = path.find_last_of('/');
        g_appDir = slash == std::string::npos ? "." : path.substr(0, slash);
    }
    g_outDir = env("NSPIRE_SIM_OUT") ? env("NSPIRE_SIM_OUT") : g_appDir + "/frames";
    if (const char* every = env("NSPIRE_SIM_DUMP_EVERY"))
        g_dumpEvery = std::atol(every);
    if (const char* frameMs = env("NSPIRE_SIM_FRAME_MS"))
        g_frameMs = std::atol(frameMs);
    if (const char* slowdown = env("NSPIRE_SIM_SLOWDOWN"))
    {
        if (g_frameMs <= 0 && std::atol(slowdown) > 0)
        {
            g_realStartUs = kernelMonotonicUs();
            g_slowdown = std::atol(slowdown);
        }
    }
    if (const char* maxFrames = env("NSPIRE_SIM_MAX_FRAMES"))
        g_maxFrames = std::atol(maxFrames);
    if (const char* script = env("NSPIRE_SIM_SCRIPT"))
        loadScript(script);
    g_backBuffer.assign(kScreenWidth * kScreenHeight, 0);
    std::fprintf(stderr, "[sim] appdir=%s out=%s script=%zu events\n",
                 g_appDir.c_str(), g_outDir.c_str(), g_script.size());
}

void shutdown() {}

const std::string& appDir() { return g_appDir; }

std::uint64_t micros()
{
    if (g_frameMs > 0 || g_slowdown > 0)
        return virtualNowUs();
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<microseconds>(steady_clock::now() - g_start).count());
}

std::uint32_t millis() { return static_cast<std::uint32_t>(micros() / 1000u); }

void delayMs(std::uint32_t) {}

std::uint16_t* backBuffer() { return g_backBuffer.data(); }

void present()
{
    ++g_frame;
    if (g_frameMs > 0)
        g_virtualUs += static_cast<std::uint64_t>(g_frameMs) * 1000u;
    static const bool trace = env("NSPIRE_SIM_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "[frame %ld presented]\n", g_frame);
    if (g_dumpEvery > 0 && g_frame % g_dumpEvery == 0)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "/frame_%06ld.ppm", g_frame);
        writePpm(g_outDir + name);
    }
    if (g_frame >= g_maxFrames)
        g_exit = true;
}

void scanKeys() { runScript(); }

bool keyDown(int key)
{
    return key >= 0 && key < NK_COUNT && g_keys[key];
}

bool exitRequested() { return g_exit; }
void requestExit() { g_exit = true; }

long heapFreeKb() { return -1; }

std::size_t heapUsedBytes()
{
    return static_cast<std::size_t>(mallinfo2().uordblks);
}

std::size_t heapPeakBytes()
{
    static std::size_t peak = 0;
    peak = std::max(peak, heapUsedBytes());
    return peak;
}

unsigned heapFailures() { return 0; }
void* gameStackTop() { return nullptr; }
std::size_t gameStackUsedBytes() { return 0; }

void log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

void fatal(const std::string& message)
{
    std::fprintf(stderr, "[sim] FATAL: %s\n", message.c_str());
    std::exit(1);
}
} // namespace NspireSystem

// Clock interposition for NSPIRE_SIM_FRAME_MS. Defined in the executable, so
// they take precedence over libc's for libstdc++ (std::chrono) as well; with
// the virtual clock off they forward to the kernel.
extern "C" int clock_gettime(clockid_t id, struct timespec* ts)
{
    if ((g_frameMs > 0 || g_slowdown > 0) && ts != nullptr)
    {
        const std::uint64_t us = virtualNowUs();
        ts->tv_sec = static_cast<time_t>(kVirtualEpochSec + us / 1000000u);
        ts->tv_nsec = static_cast<long>((us % 1000000u) * 1000u);
        return 0;
    }
    return static_cast<int>(syscall(SYS_clock_gettime, id, ts));
}

extern "C" int gettimeofday(struct timeval* tv, void* tz)
{
    if ((g_frameMs > 0 || g_slowdown > 0) && tv != nullptr)
    {
        const std::uint64_t us = virtualNowUs();
        tv->tv_sec = static_cast<time_t>(kVirtualEpochSec + us / 1000000u);
        tv->tv_usec = static_cast<suseconds_t>(us % 1000000u);
        return 0;
    }
    return static_cast<int>(syscall(SYS_gettimeofday, tv, tz));
}

#endif
