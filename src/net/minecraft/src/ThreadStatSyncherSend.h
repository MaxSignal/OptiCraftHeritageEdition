#pragma once

#include <map>
#include <thread>

#include "java/Type.h"
#include "platform/StdThread.h"

class StatBase;
class StatsSyncher;

// net.minecraft.src.ThreadStatSyncherSend
class ThreadStatSyncherSend
{
public:
	ThreadStatSyncherSend(StatsSyncher *statssyncher, std::map<StatBase*, int_t> map);
	~ThreadStatSyncherSend();

	void start();
	void run();

private:
	std::map<StatBase*, int_t> field_27233_a;
	StatsSyncher *field_27232_b;
	PlatformStdThread worker;
};
