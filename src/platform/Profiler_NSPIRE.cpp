#include "platform/Profiler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "nspire/NspireSystem.h"

// Accumulates the game's own phase probes (world tick phases, chunk load and
// generation, meshing, saving) so ClientProfilerBackend_NSPIRE can put the
// heaviest ones in its periodic log line. Phase names are string literals at
// the call sites, so the pointers are stable keys.
namespace
{
struct Phase
{
    const char* name;
    long long ns;
    unsigned count;
};

constexpr int kMaxPhases = 32;
Phase g_phases[kMaxPhases];
int g_phaseCount = 0;

void add(const char* name, long long ns)
{
    if (ns <= 0)
        return;
    for (int i = 0; i < g_phaseCount; ++i)
        if (g_phases[i].name == name || std::strcmp(g_phases[i].name, name) == 0)
        {
            g_phases[i].ns += ns;
            ++g_phases[i].count;
            return;
        }
    if (g_phaseCount < kMaxPhases)
        g_phases[g_phaseCount++] = {name, ns, 1};
}
}

// The heaviest phases since the last call, as "name=ms/count ..." (ms total).
void nspireProfileTakeTopPhases(char* out, std::size_t size, int top)
{
    std::sort(g_phases, g_phases + g_phaseCount, [](const Phase& a, const Phase& b) { return a.ns > b.ns; });
    std::size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < g_phaseCount && i < top && used + 1 < size; ++i)
        used += static_cast<std::size_t>(std::snprintf(out + used, size - used, "%s%s=%lld/%u",
                                                       i ? " " : "", g_phases[i].name,
                                                       g_phases[i].ns / 1000000LL, g_phases[i].count));
    g_phaseCount = 0;
}

std::uint32_t platformProfileRenderPhaseBegin() { return static_cast<std::uint32_t>(NspireSystem::micros()); }
void platformProfileRenderPhaseEnd(std::uint32_t start, PlatformRenderPhase phase)
{
    static const char* const kNames[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11"};
    const int index = static_cast<int>(phase);
    const char* name = (index >= 0 && index < 12) ? kNames[index] : "rX";
    add(name, static_cast<long long>(static_cast<std::uint32_t>(NspireSystem::micros()) - start) * 1000LL);
}
void platformProfileTickPhase(const char* name, long long ns) { add(name, ns); }
void platformProfileChunkBuild(long long ns, int) { add("chunkBuild", ns); }
void platformProfileChunkMeshPass(int, long long ns, int) { add("meshPass", ns); }
void platformProfileSnowColumn(bool, bool, int) {}
void platformProfilePopulatePhase(PlatformPopulatePhase, long long ns) { add("populatePhase", ns); }
void platformProfileChunkLoad(long long ns) { add("chunkLoad", ns); }
void platformProfilePopulate(long long ns) { add("populate", ns); }
void platformProfileGenerate(long long ns) { add("generate", ns); }
void platformProfileMesh(long long ns) { add("mesh", ns); }
void platformProfileUnloadSave(long long ns) { add("unloadSave", ns); }
void platformProfileTickUpdates(long long ns) { add("tickUpdates", ns); }
void platformProfileTickQueue(long long) {}
void platformProfileMobSpawn(long long ns) { add("mobSpawn2", ns); }
void platformProfileSaveWorldInfo(long long ns) { add("saveWorldInfo", ns); }
void platformProfileMapStorage(long long ns) { add("mapStorage", ns); }
void platformProfileChunkEvict(long long ns) { add("chunkEvict", ns); }
