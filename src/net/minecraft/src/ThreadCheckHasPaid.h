#pragma once

#include <thread>
#include "platform/StdThread.h"

class Minecraft;

// net.minecraft.src.ThreadCheckHasPaid
class ThreadCheckHasPaid
{
public:
	ThreadCheckHasPaid(Minecraft *minecraft);
	~ThreadCheckHasPaid();

	void start();
	void run();

private:
	Minecraft *field_28146_a;
	PlatformStdThread worker;
};
