// NspireSystem on the calculator (Ndless).
//
// Clock: Ndless's newlib backend answers gettimeofday() from the RTC, which
// counts whole seconds, so every std::chrono clock in the game would advance in
// one-second steps and the 20 Hz tick loop would stall and then burst. The first
// SP804 timer at 0x900C0000 runs free here at 32768 Hz -- configured exactly the
// way nSDL's SDL_GetTicks does it on CX/CX II (clock gate on, 32 kHz source,
// 32-bit free-running), and restored on exit -- and backs both micros() and,
// through the linker's --wrap=_gettimeofday, newlib's own clock, which is what
// fixes System::nanoTime() and every direct std::chrono use in shared code.
#if defined(NSPIRE_PLATFORM) && defined(_TINSPIRE)

#include "nspire/NspireSystem.h"
#include "nspire/input/NspireKeys.h"

#include <libndls.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

namespace
{
volatile std::uint32_t* const kTimerLoad    = reinterpret_cast<std::uint32_t*>(0x900C0000);
volatile std::uint32_t* const kTimerValue   = reinterpret_cast<std::uint32_t*>(0x900C0004);
volatile std::uint32_t* const kTimerControl = reinterpret_cast<std::uint32_t*>(0x900C0008);
volatile std::uint32_t* const kTimerClock   = reinterpret_cast<std::uint32_t*>(0x900C0080);
volatile std::uint32_t* const kClockGates   = reinterpret_cast<std::uint32_t*>(0x900B0018);
constexpr std::uint32_t kTimerGateBit = 1u << 11;
// A frame never takes this long; a bigger step between two reads means the
// counter was touched behind our back, and is dropped rather than turned into
// a jump of hours.
constexpr std::uint32_t kMaxStepTicks = 32768u * 5u;
volatile std::uint32_t* const kRtcSeconds    = reinterpret_cast<std::uint32_t*>(0x90090000);
constexpr std::uint32_t kTimerHz = 32768;

std::uint32_t g_savedControl = 0;
std::uint32_t g_savedLoad = 0;
std::uint32_t g_savedClock = 0;
std::uint32_t g_savedGates = 0;
std::uint32_t g_lastValue = 0;
std::uint64_t g_ticks = 0;
std::uint32_t g_rtcBase = 0;
bool g_timerRunning = false;
bool g_exit = false;
std::string g_appDir = "/documents/ndless";
std::uint16_t* g_backBuffer = nullptr;
bool g_logStarted = false;

const t_key* keyTable()
{
    static const t_key table[NK_COUNT] = {
        KEY_NSPIRE_ESC, KEY_NSPIRE_HOME, KEY_NSPIRE_MENU, KEY_NSPIRE_TAB, KEY_NSPIRE_CTRL,
        KEY_NSPIRE_SHIFT, KEY_NSPIRE_DEL, KEY_NSPIRE_ENTER, KEY_NSPIRE_RET, KEY_NSPIRE_CLICK,
        KEY_NSPIRE_UP, KEY_NSPIRE_DOWN, KEY_NSPIRE_LEFT, KEY_NSPIRE_RIGHT, KEY_NSPIRE_SPACE,
        KEY_NSPIRE_A, KEY_NSPIRE_B, KEY_NSPIRE_C, KEY_NSPIRE_D, KEY_NSPIRE_E, KEY_NSPIRE_F,
        KEY_NSPIRE_G, KEY_NSPIRE_H, KEY_NSPIRE_I, KEY_NSPIRE_J, KEY_NSPIRE_K, KEY_NSPIRE_L,
        KEY_NSPIRE_M, KEY_NSPIRE_N, KEY_NSPIRE_O, KEY_NSPIRE_P, KEY_NSPIRE_Q, KEY_NSPIRE_R,
        KEY_NSPIRE_S, KEY_NSPIRE_T, KEY_NSPIRE_U, KEY_NSPIRE_V, KEY_NSPIRE_W, KEY_NSPIRE_X,
        KEY_NSPIRE_Y, KEY_NSPIRE_Z,
        KEY_NSPIRE_0, KEY_NSPIRE_1, KEY_NSPIRE_2, KEY_NSPIRE_3, KEY_NSPIRE_4,
        KEY_NSPIRE_5, KEY_NSPIRE_6, KEY_NSPIRE_7, KEY_NSPIRE_8, KEY_NSPIRE_9,
        KEY_NSPIRE_PERIOD, KEY_NSPIRE_COMMA, KEY_NSPIRE_MINUS, KEY_NSPIRE_PLUS,
        KEY_NSPIRE_MULTIPLY, KEY_NSPIRE_DIVIDE, KEY_NSPIRE_EQU, KEY_NSPIRE_LP, KEY_NSPIRE_RP,
        KEY_NSPIRE_NEGATIVE,
    };
    return table;
}

void startTimer()
{
    g_savedGates = *kClockGates;
    g_savedControl = *kTimerControl;
    g_savedLoad = *kTimerLoad;
    g_savedClock = *kTimerClock;
    *kClockGates = g_savedGates & ~kTimerGateBit;
    *kTimerControl = 0;
    *kTimerClock = 0xA;          // 32768 Hz source
    *kTimerLoad = 0xFFFFFFFFu;
    // Enable, free-running, interrupt off, no prescale, 32-bit.
    *kTimerControl = 0x82u;
    g_lastValue = *kTimerValue;
    g_ticks = 0;
    g_rtcBase = *kRtcSeconds;
    g_timerRunning = true;
}

void stopTimer()
{
    if (!g_timerRunning)
        return;
    *kTimerControl = 0;
    *kTimerLoad = g_savedLoad;
    *kTimerClock = g_savedClock;
    *kTimerControl = g_savedControl;
    *kClockGates = g_savedGates;
    g_timerRunning = false;
}

std::uint64_t ticks()
{
    if (!g_timerRunning)
        return 0;
    // Down-counter: elapsed = previous - current, modulo 2^32.
    const std::uint32_t value = *kTimerValue;
    const std::uint32_t step = g_lastValue - value;
    if (step <= kMaxStepTicks)
        g_ticks += step;
    g_lastValue = value;
    return g_ticks;
}
} // namespace

// newlib reaches the clock through _gettimeofday; see the header comment.
extern "C" int __wrap__gettimeofday(struct timeval* tv, void* tz)
{
    if (tv)
    {
        const std::uint64_t us = NspireSystem::micros();
        tv->tv_sec = static_cast<time_t>(g_rtcBase + us / 1000000u);
        tv->tv_usec = static_cast<suseconds_t>(us % 1000000u);
    }
    if (tz)
        std::memset(tz, 0, sizeof(struct timezone));
    return 0;
}

namespace NspireSystem
{
void initialize(int argc, char** argv)
{
    startTimer();
    if (argc > 0 && argv && argv[0])
    {
        std::string path = argv[0];
        const std::size_t slash = path.find_last_of('/');
        if (slash != std::string::npos)
            g_appDir = path.substr(0, slash);
    }
    g_backBuffer = static_cast<std::uint16_t*>(std::calloc(kScreenWidth * kScreenHeight, sizeof(std::uint16_t)));
    if (g_backBuffer == nullptr)
        fatal("Out of memory allocating the framebuffer.");
}

void shutdown()
{
    std::free(g_backBuffer);
    g_backBuffer = nullptr;
    stopTimer();
}

const std::string& appDir() { return g_appDir; }

std::uint64_t micros()
{
    // ticks * 1e6 / 32768 == ticks * 15625 / 512, exact and overflow-free for
    // the ~1.8e5 years a 64-bit tick count covers.
    return ticks() * 15625u / 512u;
}

std::uint32_t millis()
{
    return static_cast<std::uint32_t>(ticks() * 1000u / kTimerHz);
}

void delayMs(std::uint32_t ms)
{
    if (ms)
        msleep(ms);
}

std::uint16_t* backBuffer() { return g_backBuffer; }

void present()
{
    lcd_blit(g_backBuffer, SCR_320x240_565);
}

void scanKeys() {}

bool keyDown(int key)
{
    if (key < 0 || key >= NK_COUNT)
        return false;
    return isKeyPressed(keyTable()[key]);
}

bool exitRequested() { return g_exit; }
void requestExit() { g_exit = true; }

extern "C" std::size_t nspire_heap_used();
extern "C" std::size_t nspire_heap_peak();
extern "C" unsigned nspire_heap_failures();

std::size_t heapUsedBytes() { return nspire_heap_used(); }
std::size_t heapPeakBytes() { return nspire_heap_peak(); }
unsigned heapFailures() { return nspire_heap_failures(); }

constexpr std::size_t kGameStackBytes = 1024 * 1024;
constexpr std::uint32_t kStackPaint = 0x5A17C0DEu;
std::uint32_t* g_gameStack = nullptr;

void* gameStackTop()
{
    // Allocated before anything else and never freed: the program returns
    // through crt0, which restores the OS stack itself.
    if (g_gameStack == nullptr)
    {
        g_gameStack = static_cast<std::uint32_t*>(std::malloc(kGameStackBytes));
        if (g_gameStack == nullptr)
            return nullptr;
        for (std::size_t i = 0; i < kGameStackBytes / 4; ++i)
            g_gameStack[i] = kStackPaint;
    }
    // 8-byte aligned, as the AAPCS requires at public interfaces.
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(g_gameStack + kGameStackBytes / 4) & ~std::uintptr_t(7));
}

std::size_t gameStackUsedBytes()
{
    if (g_gameStack == nullptr)
        return 0;
    std::size_t untouched = 0;
    while (untouched < kGameStackBytes / 4 && g_gameStack[untouched] == kStackPaint)
        ++untouched;
    return kGameStackBytes - untouched * 4;
}

long heapFreeKb()
{
    // The OS heap has no query; Runtime falls back to a fixed budget.
    return -1;
}

void log(const char* fmt, ...)
{
    // The OS commits a file to flash only when it is closed, so a hang loses
    // anything still open. Reopen and close per line: the log is a few lines
    // every few seconds, and the last ones are the ones that matter.
    // .tns so the file can be pulled off the calculator with TI's software.
    const std::string path = g_appDir + "/opticraft_log.txt.tns";
    std::FILE* file = std::fopen(path.c_str(), g_logStarted ? "a" : "w");
    if (file == nullptr)
        return;
    g_logStarted = true;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fclose(file);
}

void fatal(const std::string& message)
{
    log("FATAL: %s\n", message.c_str());
    // Hand the screen back to the OS before its dialog draws over it.
    lcd_init(SCR_TYPE_INVALID);
    show_msgbox("OptiCraft", message.c_str());
    shutdown();
    std::exit(1);
}
} // namespace NspireSystem

#endif
