#pragma once

#include <thread>
#include "platform/StdThread.h"

class Minecraft;

// net.minecraft.src.GameWindowListener
// Java: extends WindowAdapter, responds to window close event.
// SDL2 equivalent: window close is handled via SDL_QUIT in the event loop,
// but this class mirrors the Java structure for completeness.
class GameWindowListener
{
public:
    GameWindowListener(Minecraft *minecraft, PlatformStdThread *mcThread);

    void windowClosing();

    Minecraft   *mc;
    PlatformStdThread *mcThread;
};
