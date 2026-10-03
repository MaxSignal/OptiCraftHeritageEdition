#pragma once

// Calculator keypad -> lwjgl Keyboard/Mouse events.
//
//   Letters, digits, space, del, tab, esc   the lwjgl key of the same name
//                                           (plus its character), so the
//                                           desktop bindings apply unchanged:
//                                           WASD walk, space jump, E inventory,
//                                           Q drop, 1-9 hotbar, esc pause.
//   shift                                   left shift (sneak; upper case)
//   ctrl                                    left control
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
// Move the software cursor (keyboard selection follows the pointer on consoles).
void setCursor(int x, int y);
// Calculator key label for a lwjgl key code, or null if it has none.
const char* keyLabelForLwjgl(int lwjglKey);
}
