#include "platform/Input.h"

#include "nspire/input/NspireKeypad.h"

// The keypad reaches the game as lwjgl keyboard and mouse events
// (nspire/input/NspireKeypad.cpp). The one exception is a container screen:
// there the arrows, enter/click and menu act as a console pad for the slot
// navigator (ContainerSlotNavigator), so inventories need no pointer. Anywhere
// else the pad answers "not connected", which leaves menus to the lwjgl keys.
PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    PlatformTextInputSnapshot pad;
    if (port != 0 || !platformContainerNavigationActive() || NspireKeypad::pointerActive())
        return pad;
    pad.connected = true;
    pad.held = NspireKeypad::padHeld();
    pad.pressed = NspireKeypad::padPressed();
    return pad;
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
