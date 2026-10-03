#pragma once

#include <thread>
#include "platform/StdThread.h"

class NetworkManager;

// net.minecraft.src.NetworkMasterThread
class NetworkMasterThread
{
public:
    NetworkMasterThread(NetworkManager *networkmanager);

    void start();
    void run();

    NetworkManager *netManager;

private:
    PlatformStdThread thread;
};
