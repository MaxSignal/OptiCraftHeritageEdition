// NspireSystem on the calculator (Ndless).
//
// Clock: Ndless's newlib backend answers gettimeofday() from the RTC, which
// counts whole seconds, so every std::chrono clock in the game would advance in
// one-second steps and the 20 Hz tick loop would stall and then burst. The SP804
// at 0x900D0000 has two timers; libndls' msleep() owns the first, so the second
// (base + 0x20) runs free here at 32768 Hz and backs both micros() and, through
// the linker's --wrap=_gettimeofday, newlib's own clock -- which is what fixes
// System::nanoTime() and every direct std::chrono use in shared code at once.
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
volatile std::uint32_t* const kTimer2Load    = reinterpret_cast<std::uint32_t*>(0x900D0020);
volatile std::uint32_t* const kTimer2Value   = reinterpret_cast<std::uint32_t*>(0x900D0024);
volatile std::uint32_t* const kTimer2Control = reinterpret_cast<std::uint32_t*>(0x900D0028);
volatile std::uint32_t* const kRtcSeconds    = reinterpret_cast<std::uint32_t*>(0x90090000);
constexpr std::uint32_t kTimerHz = 32768;

std::uint32_t g_savedControl = 0;
std::uint32_t g_savedLoad = 0;
std::uint32_t g_lastValue = 0;
std::uint64_t g_ticks = 0;
std::uint32_t g_rtcBase = 0;
bool g_timerRunning = false;
bool g_exit = false;
std::string g_appDir = "/documents/ndless";
std::uint16_t* g_backBuffer = nullptr;
std::FILE* g_log = nullptr;

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
    g_savedControl = *kTimer2Control;
    g_savedLoad = *kTimer2Load;
    *kTimer2Control = 0;
    *kTimer2Load = 0xFFFFFFFFu;
    // Enable, free-running, interrupt off, no prescale, 32-bit.
    *kTimer2Control = 0b10000010u;
    g_lastValue = *kTimer2Value;
    g_ticks = 0;
    g_rtcBase = *kRtcSeconds;
    g_timerRunning = true;
}

void stopTimer()
{
    if (!g_timerRunning)
        return;
    *kTimer2Control = 0;
    *kTimer2Load = g_savedLoad;
    *kTimer2Control = g_savedControl;
    g_timerRunning = false;
}

std::uint64_t ticks()
{
    if (!g_timerRunning)
        return 0;
    // Down-counter: elapsed = previous - current, modulo 2^32.
    const std::uint32_t value = *kTimer2Value;
    g_ticks += static_cast<std::uint32_t>(g_lastValue - value);
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
    if (g_log)
    {
        std::fclose(g_log);
        g_log = nullptr;
    }
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

long heapFreeKb()
{
    // The OS heap has no query; Runtime falls back to a fixed budget.
    return -1;
}

void log(const char* fmt, ...)
{
    if (g_log == nullptr)
    {
        // .tns so the file can be pulled off the calculator with TI's software.
        const std::string path = g_appDir + "/opticraft_log.txt.tns";
        g_log = std::fopen(path.c_str(), "w");
        if (g_log == nullptr)
            return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fflush(g_log);
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
