#include "nspire/input/NspireKeys.h"

#include <cstring>

namespace
{
const char* const kNames[NK_COUNT] = {
    "ESC", "HOME", "MENU", "TAB", "CTRL", "SHIFT", "DEL", "ENTER", "RET",
    "CLICK", "UP", "DOWN", "LEFT", "RIGHT", "SPACE",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "PERIOD", "COMMA", "MINUS", "PLUS", "MULTIPLY", "DIVIDE", "EQU",
    "LP", "RP", "NEGATIVE",
};
}

const char* nspireKeyName(int key)
{
    return (key >= 0 && key < NK_COUNT) ? kNames[key] : "?";
}

int nspireKeyFromName(const char* name)
{
    for (int i = 0; i < NK_COUNT; ++i)
        if (std::strcmp(kNames[i], name) == 0)
            return i;
    return -1;
}
