#pragma once

#include <cstdint>

// Calculator keypad -> lwjgl Keyboard/Mouse events.
//
//   Letters, digits, space, del, tab, esc   the lwjgl key of the same name
//                                           (plus its character), so the
//                                           desktop bindings apply unchanged:
//                                           WASD walk, space jump, E inventory,
//                                           Q drop, 1-9 hotbar, esc pause.
//   shift                                   left shift (sneak; upper case)
//   ctrl                                    left control
//   ctrl + 1..9, 0                          F1..F10 (F2 screenshot, F3 debug)
//   arrows / touchpad edges                 in game: mouse motion (camera look)
//                                           in menus: the arrow keys (selection)
//   ctrl + arrows (menus)                   the software cursor
//   enter                                   in game: left mouse button (break)
//                                           in menus: return (activate selection)
//   touchpad click                          left mouse button
//   menu                                    right mouse button (place / use)
//   ret                                     return (confirm text fields)
//   home                                    close request (saves and quits)
namespace NspireKeypad
{
void initialize(int screenWidth, int screenHeight);
// Once per presented frame. inMenu decides what the arrows drive.
void poll(bool inMenu);
// True while the pointer (ctrl+arrows, touchpad click) rather than keyboard
// selection owns menu input.
bool pointerActive();
// This frame's keypad as PLATFORM_TEXT_* pad bits (held / newly pressed).
std::uint32_t padHeld();
std::uint32_t padPressed(); // consumes
// Creative inventory tab steps from ( and ) since the last call (consumes).
int consumePageSteps();
// True when a key was down, or went up, in a poll since the last call
// (consumes): nothing on a screen over the world can have changed otherwise.
bool takeActivity();
// Move the software cursor (keyboard selection follows the pointer on consoles).
void setCursor(int x, int y);
// Calculator key label for a lwjgl key code, or null if it has none.
const char* keyLabelForLwjgl(int lwjglKey);
}
