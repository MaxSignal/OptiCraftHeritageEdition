#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "platform/StdThread.h"

class NetworkManager;

// net.minecraft.src.NetworkReaderThread
class NetworkReaderThread
{
public:
    NetworkReaderThread(NetworkManager *networkmanager, const std::string &name);

    void start();
    bool isAlive() const;
    void join();
    void run();

    NetworkManager *netManager;

private:
    std::string threadName;
    PlatformStdThread thread;
    std::atomic_bool alive{false};
};
