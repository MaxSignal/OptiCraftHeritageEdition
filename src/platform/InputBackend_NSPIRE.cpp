#include "platform/Input.h"

#include "nspire/input/NspireKeypad.h"

// The keypad reaches the game as lwjgl keyboard and mouse events
// (nspire/input/NspireKeypad.cpp); there is no gamepad and no on-screen
// keyboard, so every controller-shaped query answers "not connected".
PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    (void)port;
    return {};
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    (void)port;
    return {};
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    (void)port;
    return {};
}

int platformMenuPad()
{
    return 0;
}

bool platformMenuPointerActive()
{
    return NspireKeypad::pointerActive();
}

bool platformMenuCursorVisible()
{
    return true;
}

void platformSetMenuCursor(int x, int y)
{
    NspireKeypad::setCursor(x, y);
}

const PlatformKeyboardHints& platformKeyboardHints()
{
    static const PlatformKeyboardHints hints;
    return hints;
}

const char* platformInputDebugLine()
{
    return "";
}
