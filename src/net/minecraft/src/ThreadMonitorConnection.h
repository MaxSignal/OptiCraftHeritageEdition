#pragma once

#include <thread>
#include "platform/StdThread.h"

class NetworkManager;

// net.minecraft.src.ThreadMonitorConnection
class ThreadMonitorConnection
{
public:
    explicit ThreadMonitorConnection(NetworkManager *networkManager);
    ~ThreadMonitorConnection();

    void start();
    void run();

private:
    NetworkManager *netManager;
    PlatformStdThread worker;
};
