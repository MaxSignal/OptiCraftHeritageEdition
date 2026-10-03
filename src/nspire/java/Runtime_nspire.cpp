// Runtime_nspire.cpp -- TI-Nspire implementation of java/Runtime.h.
//
// Ndless' malloc is the OS heap behind a syscall, with no statistics to read,
// so these report the budget the port is tuned for rather than a measurement.
// Only the F3 overlay consumes them.
#ifdef NSPIRE_PLATFORM

#include "java/Runtime.h"

#include "nspire/NspireSystem.h"

namespace
{
constexpr long_t kHeapBudget = 40L * 1024L * 1024L;
}

Runtime Runtime::instance;

Runtime &Runtime::getRuntime()
{
	return instance;
}

long_t Runtime::maxMemory()
{
	return kHeapBudget;
}

long_t Runtime::totalMemory()
{
	return kHeapBudget;
}

long_t Runtime::freeMemory()
{
	const long freeKb = NspireSystem::heapFreeKb();
	return freeKb >= 0 ? static_cast<long_t>(freeKb) * 1024 : kHeapBudget / 2;
}

#endif // NSPIRE_PLATFORM
