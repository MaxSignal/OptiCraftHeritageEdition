#include "platform/Diagnostics.h"

#include "nspire/NspireSystem.h"

const char* platformOomDiagnosticLine(int index)
{
    (void)index;
    return "";
}

void platformMemoryCheckpoint(const char* tag)
{
    NspireSystem::log("mem %s: %ld KB free\n", tag, NspireSystem::heapFreeKb());
}

void platformHardwareCheckpoint(const char* tag)
{
    (void)tag;
}

long platformHeapFreeKb()
{
    return NspireSystem::heapFreeKb();
}

void platformCaptureBadAlloc()
{
}
