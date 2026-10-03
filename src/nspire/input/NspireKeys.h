#pragma once

// Calculator keys the port reads. Platform-neutral ids so the host simulator
// can name them without libndls; NspireSystem_device.cpp maps each to its
// libndls t_key.
enum NspireKey : int
{
    NK_ESC, NK_HOME, NK_MENU, NK_TAB, NK_CTRL, NK_SHIFT, NK_DEL, NK_ENTER, NK_RET,
    NK_CLICK, NK_UP, NK_DOWN, NK_LEFT, NK_RIGHT, NK_SPACE,
    NK_A, NK_B, NK_C, NK_D, NK_E, NK_F, NK_G, NK_H, NK_I, NK_J, NK_K, NK_L, NK_M,
    NK_N, NK_O, NK_P, NK_Q, NK_R, NK_S, NK_T, NK_U, NK_V, NK_W, NK_X, NK_Y, NK_Z,
    NK_0, NK_1, NK_2, NK_3, NK_4, NK_5, NK_6, NK_7, NK_8, NK_9,
    NK_PERIOD, NK_COMMA, NK_MINUS, NK_PLUS, NK_MULTIPLY, NK_DIVIDE, NK_EQU,
    NK_LP, NK_RP, NK_NEGATIVE,
    NK_COUNT
};

// Script/debug names ("ESC", "A", "7", ...), indexed by NspireKey.
const char* nspireKeyName(int key);
int nspireKeyFromName(const char* name);
