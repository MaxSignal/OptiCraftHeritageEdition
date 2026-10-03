#include "nspire/input/NspireKeypad.h"

#include <algorithm>
#include <cstdint>

#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"
#include "nspire/NspireSystem.h"
#include "nspire/input/NspireKeys.h"

namespace
{
using lwjgl::Keyboard::Key;

struct KeyMapping
{
    int nspireKey;
    int lwjglKey;
    char lower;
    char upper;
};

// Keys that become plain lwjgl key events. Arrows, enter, click, menu and home
// are handled separately below.
const KeyMapping kKeyMappings[] = {
    {NK_A, Key::KEY_A, 'a', 'A'}, {NK_B, Key::KEY_B, 'b', 'B'}, {NK_C, Key::KEY_C, 'c', 'C'},
    {NK_D, Key::KEY_D, 'd', 'D'}, {NK_E, Key::KEY_E, 'e', 'E'}, {NK_F, Key::KEY_F, 'f', 'F'},
    {NK_G, Key::KEY_G, 'g', 'G'}, {NK_H, Key::KEY_H, 'h', 'H'}, {NK_I, Key::KEY_I, 'i', 'I'},
    {NK_J, Key::KEY_J, 'j', 'J'}, {NK_K, Key::KEY_K, 'k', 'K'}, {NK_L, Key::KEY_L, 'l', 'L'},
    {NK_M, Key::KEY_M, 'm', 'M'}, {NK_N, Key::KEY_N, 'n', 'N'}, {NK_O, Key::KEY_O, 'o', 'O'},
    {NK_P, Key::KEY_P, 'p', 'P'}, {NK_Q, Key::KEY_Q, 'q', 'Q'}, {NK_R, Key::KEY_R, 'r', 'R'},
    {NK_S, Key::KEY_S, 's', 'S'}, {NK_T, Key::KEY_T, 't', 'T'}, {NK_U, Key::KEY_U, 'u', 'U'},
    {NK_V, Key::KEY_V, 'v', 'V'}, {NK_W, Key::KEY_W, 'w', 'W'}, {NK_X, Key::KEY_X, 'x', 'X'},
    {NK_Y, Key::KEY_Y, 'y', 'Y'}, {NK_Z, Key::KEY_Z, 'z', 'Z'},
    {NK_0, Key::KEY_0, '0', '0'}, {NK_1, Key::KEY_1, '1', '1'}, {NK_2, Key::KEY_2, '2', '2'},
    {NK_3, Key::KEY_3, '3', '3'}, {NK_4, Key::KEY_4, '4', '4'}, {NK_5, Key::KEY_5, '5', '5'},
    {NK_6, Key::KEY_6, '6', '6'}, {NK_7, Key::KEY_7, '7', '7'}, {NK_8, Key::KEY_8, '8', '8'},
    {NK_9, Key::KEY_9, '9', '9'},
    {NK_SPACE, Key::KEY_SPACE, ' ', ' '},
    {NK_DEL, Key::KEY_BACK, 0, 0},
    {NK_TAB, Key::KEY_TAB, 0, 0},
    {NK_ESC, Key::KEY_ESCAPE, 0, 0},
    {NK_RET, Key::KEY_RETURN, 0, 0},
    {NK_SHIFT, Key::KEY_LSHIFT, 0, 0},
    {NK_CTRL, Key::KEY_LCONTROL, 0, 0},
    {NK_PERIOD, Key::KEY_PERIOD, '.', '.'},
    {NK_COMMA, Key::KEY_COMMA, ',', ','},
    {NK_MINUS, Key::KEY_MINUS, '-', '_'},
    {NK_NEGATIVE, Key::KEY_MINUS, '-', '_'},
    {NK_PLUS, Key::KEY_ADD, '+', '+'},
    {NK_MULTIPLY, Key::KEY_MULTIPLY, '*', '*'},
    {NK_DIVIDE, Key::KEY_SLASH, '/', '/'},
    {NK_EQU, Key::KEY_EQUALS, '=', '='},
    {NK_LP, Key::KEY_NONE, '(', '('},
    {NK_RP, Key::KEY_NONE, ')', ')'},
};

// Camera turn rate expressed in mouse pixels per second (vanilla turns 0.15
// degrees per pixel at default sensitivity, so this is ~70 degrees/s). Time
// based, because the calculator renders a few frames per second at best and a
// per-frame step would make looking around depend on how busy the scene is.
constexpr float kLookPixelsPerSecond = 480.0f;
constexpr float kCursorPixelsPerSecond = 200.0f;
constexpr float kMaxStepSeconds = 0.25f;

bool g_prev[NK_COUNT] = {};
int g_width = 320;
int g_height = 240;
float g_cursorX = 160.0f;
float g_cursorY = 120.0f;
float g_motionCarryX = 0.0f;
float g_motionCarryY = 0.0f;
std::uint64_t g_lastPollUs = 0;
std::uint64_t g_repeatAtUs[NK_COUNT] = {};
bool g_leftFromEnter = false;
// Whether the pointer currently owns menu input (Wii model, see GuiScreen.cpp):
// ctrl+arrows or the touchpad click hand it to the pointer, the plain arrows
// and enter hand it back to keyboard selection.
bool g_pointerActive = false;
constexpr int kParkedCursor = -10000;

bool pressed(int key, const bool* now) { return now[key] && !g_prev[key]; }
bool released(int key, const bool* now) { return !now[key] && g_prev[key]; }

void emitKeys(const bool* now)
{
    const bool shift = now[NK_SHIFT];
    for (const KeyMapping& m : kKeyMappings)
    {
        if (pressed(m.nspireKey, now))
        {
            if (m.lwjglKey != Key::KEY_NONE)
                lwjgl::Keyboard::detail::pushKey(m.lwjglKey, true);
            const char c = shift ? m.upper : m.lower;
            if (c)
                lwjgl::Keyboard::detail::pushChar(c);
        }
        else if (released(m.nspireKey, now) && m.lwjglKey != Key::KEY_NONE)
            lwjgl::Keyboard::detail::pushKey(m.lwjglKey, false);
    }
}

void emitButtons(const bool* now, bool inMenu)
{
    const int x = static_cast<int>(g_cursorX);
    const int y = static_cast<int>(g_cursorY);
    // In a menu enter confirms the keyboard selection (GuiScreen's Java-UI
    // navigation); in the world it is the attack button, like the click.
    if (inMenu)
    {
        if (pressed(NK_ENTER, now))
            lwjgl::Keyboard::detail::pushKey(Key::KEY_RETURN, true);
        else if (released(NK_ENTER, now))
            lwjgl::Keyboard::detail::pushKey(Key::KEY_RETURN, false);
    }
    const bool leftNow = now[NK_CLICK] || (!inMenu && now[NK_ENTER]);
    const bool leftPrev = g_prev[NK_CLICK] || (g_leftFromEnter && g_prev[NK_ENTER]);
    if (leftNow != leftPrev)
        lwjgl::Mouse::detail::pushButton(0, leftNow, x, y);
    g_leftFromEnter = !inMenu && now[NK_ENTER];
    if (now[NK_MENU] != g_prev[NK_MENU])
        lwjgl::Mouse::detail::pushButton(1, now[NK_MENU], x, y);
}

// Menu navigation: the arrows are the lwjgl arrow keys, with key repeat so a
// held arrow walks through a list even at a few frames per second.
void emitMenuArrows(const bool* now)
{
    static const int arrows[4][2] = {
        {NK_UP, Key::KEY_UP}, {NK_DOWN, Key::KEY_DOWN}, {NK_LEFT, Key::KEY_LEFT}, {NK_RIGHT, Key::KEY_RIGHT},
    };
    const std::uint64_t nowUs = NspireSystem::micros();
    for (const auto& a : arrows)
    {
        if (pressed(a[0], now))
        {
            lwjgl::Keyboard::detail::pushKey(a[1], true);
            g_repeatAtUs[a[0]] = nowUs + 450000u;
        }
        else if (now[a[0]] && nowUs >= g_repeatAtUs[a[0]])
        {
            lwjgl::Keyboard::detail::pushKey(a[1], true);
            g_repeatAtUs[a[0]] = nowUs + 150000u;
        }
        else if (released(a[0], now))
            lwjgl::Keyboard::detail::pushKey(a[1], false);
    }
}

void emitMotion(const bool* now, bool inMenu, float dt)
{
    float dx = 0.0f, dy = 0.0f;
    if (now[NK_LEFT]) dx -= 1.0f;
    if (now[NK_RIGHT]) dx += 1.0f;
    if (now[NK_UP]) dy -= 1.0f;
    if (now[NK_DOWN]) dy += 1.0f;
    if (dx == 0.0f && dy == 0.0f)
    {
        g_motionCarryX = g_motionCarryY = 0.0f;
        return;
    }

    const float rate = inMenu ? kCursorPixelsPerSecond : kLookPixelsPerSecond;
    // At least two pixels per frame so a single tap always moves the cursor.
    const float step = std::max(rate * dt, 2.0f);
    g_motionCarryX += dx * step;
    g_motionCarryY += dy * step;
    const int stepX = static_cast<int>(g_motionCarryX);
    const int stepY = static_cast<int>(g_motionCarryY);
    g_motionCarryX -= stepX;
    g_motionCarryY -= stepY;
    if (stepX == 0 && stepY == 0)
        return;

    if (inMenu)
    {
        g_cursorX = std::min(std::max(g_cursorX + stepX, 0.0f), static_cast<float>(g_width - 1));
        g_cursorY = std::min(std::max(g_cursorY + stepY, 0.0f), static_cast<float>(g_height - 1));
    }
    lwjgl::Mouse::detail::pushMotion(static_cast<int>(g_cursorX), static_cast<int>(g_cursorY), stepX, stepY);
}
} // namespace

namespace NspireKeypad
{
void initialize(int screenWidth, int screenHeight)
{
    g_width = screenWidth;
    g_height = screenHeight;
    g_cursorX = screenWidth * 0.5f;
    g_cursorY = screenHeight * 0.5f;
    lwjgl::Mouse::setCursorPosition(kParkedCursor, kParkedCursor);
    g_lastPollUs = NspireSystem::micros();
}

void poll(bool inMenu)
{
    NspireSystem::scanKeys();
    bool now[NK_COUNT];
    for (int k = 0; k < NK_COUNT; ++k)
        now[k] = NspireSystem::keyDown(k);

    const std::uint64_t nowUs = NspireSystem::micros();
    float dt = static_cast<float>(nowUs - g_lastPollUs) * 1e-6f;
    g_lastPollUs = nowUs;
    if (dt > kMaxStepSeconds)
        dt = kMaxStepSeconds;

    emitKeys(now);
    emitButtons(now, inMenu);
    // Menus: arrows navigate, ctrl+arrows drive the pointer (inventories and
    // other mouse-only screens). In the world the arrows always look around.
    const bool wasPointerActive = g_pointerActive;
    if (now[NK_CLICK] || (now[NK_CTRL] && (now[NK_UP] || now[NK_DOWN] || now[NK_LEFT] || now[NK_RIGHT])))
        g_pointerActive = true;
    else if (inMenu && (pressed(NK_UP, now) || pressed(NK_DOWN, now) || pressed(NK_LEFT, now) ||
                        pressed(NK_RIGHT, now) || pressed(NK_ENTER, now)))
        g_pointerActive = false;
    // Screens hit-test the lwjgl pointer for hover whether or not it is drawn,
    // and a hovered button pins the keyboard selection. Park it off-screen
    // while the keyboard owns the menu; bring it back where it was.
    if (g_pointerActive && !wasPointerActive)
        lwjgl::Mouse::detail::pushMotion(static_cast<int>(g_cursorX), static_cast<int>(g_cursorY), 0, 0);
    else if (!g_pointerActive)
        lwjgl::Mouse::setCursorPosition(kParkedCursor, kParkedCursor);
    if (inMenu && !now[NK_CTRL])
    {
        emitMenuArrows(now);
        g_motionCarryX = g_motionCarryY = 0.0f;
    }
    else
        emitMotion(now, inMenu, dt);

    if (pressed(NK_HOME, now))
        NspireSystem::requestExit();

    std::copy(now, now + NK_COUNT, g_prev);
}

bool pointerActive()
{
    return g_pointerActive;
}

void setCursor(int x, int y)
{
    g_cursorX = static_cast<float>(std::min(std::max(x, 0), g_width - 1));
    g_cursorY = static_cast<float>(std::min(std::max(y, 0), g_height - 1));
    if (g_pointerActive)
        lwjgl::Mouse::detail::pushMotion(static_cast<int>(g_cursorX), static_cast<int>(g_cursorY), 0, 0);
}

const char* keyLabelForLwjgl(int lwjglKey)
{
    switch (lwjglKey)
    {
    case Key::KEY_LSHIFT: return "shift";
    case Key::KEY_LCONTROL: return "ctrl";
    case Key::KEY_BACK: return "del";
    case Key::KEY_RETURN: return "ret";
    case Key::KEY_ESCAPE: return "esc";
    case Key::KEY_TAB: return "tab";
    case Key::KEY_SPACE: return "space";
    // Desktop mouse "keys" (LWJGL encodes buttons as negative key codes).
    case -100: return "enter";
    case -99: return "menu";
    default: return nullptr;
    }
}
} // namespace NspireKeypad
