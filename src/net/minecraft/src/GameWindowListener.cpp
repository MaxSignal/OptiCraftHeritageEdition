#include "platform/Log.h"
#include "GameWindowListener.h"

#include <cstdlib>
#include <iostream>

#include "Minecraft.h"
#include "platform/StdThread.h"

GameWindowListener::GameWindowListener(Minecraft *minecraft, PlatformStdThread *thread)
    : mc(minecraft), mcThread(thread)
{
}

void GameWindowListener::windowClosing()
{
    mc->shutdown();

    if (mcThread != nullptr && mcThread->joinable())
    {
        try
        {
            mcThread->join();
        }
        catch (...)
        {
            MC_LOG_WARN("client", "GameWindowListener: interrupted while joining mcThread\n");
        }
    }

    std::exit(0);
}
