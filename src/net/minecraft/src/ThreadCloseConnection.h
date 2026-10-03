#pragma once

#include <thread>
#include "platform/StdThread.h"

class NetworkManager;

// net.minecraft.src.ThreadCloseConnection
class ThreadCloseConnection
{
public:
	ThreadCloseConnection(NetworkManager *networkmanager);
	~ThreadCloseConnection();

	void start();
	void run();

private:
	NetworkManager *field_28109_a;
	PlatformStdThread worker;
};
