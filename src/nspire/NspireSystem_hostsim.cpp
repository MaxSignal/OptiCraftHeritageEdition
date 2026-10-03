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
#if defined(NSPIRE_PLATFORM) && !defined(_TINSPIRE)

#include "nspire/NspireSystem.h"
#include "nspire/input/NspireKeys.h"

#include <chrono>
#include <cstdarg>
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
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<microseconds>(steady_clock::now() - g_start).count());
}

std::uint32_t millis() { return static_cast<std::uint32_t>(micros() / 1000u); }

void delayMs(std::uint32_t) {}

std::uint16_t* backBuffer() { return g_backBuffer.data(); }

void present()
{
    ++g_frame;
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

#endif
