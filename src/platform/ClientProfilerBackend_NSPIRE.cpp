#include "platform/ClientProfilerBackend.h"

#include <algorithm>

#include "nspire/NspireSystem.h"
#include "nspire/render/NglBackend.h"

// One status line in opticraft_log.txt.tns every few seconds: where the frame
// time goes (ticks / world+GUI render / present), how many frames and ticks ran,
// how much the renderer drew, and the heap. It is the calculator's only
// profiler, and the last lines before a hang say what the game was doing.
void nspireProfileTakeTopPhases(char* out, std::size_t size, int top);

namespace
{
constexpr long long kIntervalNs = 5000000000LL;

long long g_windowStartNs = -1;
long long g_tickNs = 0;
long long g_renderNs = 0;
long long g_displayNs = 0;
long long g_lightingNs = 0;
long long g_frameNs = 0;
long long g_worstFrameNs = 0;
int g_frames = 0;
int g_ticks = 0;
int g_chunkUpdates = 0;

long long nowNs()
{
    return static_cast<long long>(NspireSystem::micros()) * 1000LL;
}
}

namespace ClientProfilerBackend
{
void frameBegin()
{
    if (g_windowStartNs < 0)
        g_windowStartNs = nowNs();
}

void ticks(long long ns, int) { g_tickNs += ns; }
void lighting(long long ns) { g_lightingNs += ns; }
void displayUpdate(long long ns) { g_displayNs += ns; }
void render(long long ns) { g_renderNs += ns; }

void frameEnd(long long frameNs, long long, long long, int ticksThisFrame, int chunkUpdates,
              World* world, RenderGlobal*)
{
    ++g_frames;
    g_ticks += ticksThisFrame;
    g_chunkUpdates += chunkUpdates;
    g_frameNs += frameNs;
    g_worstFrameNs = std::max(g_worstFrameNs, frameNs);

    const long long now = nowNs();
    if (g_windowStartNs < 0 || now - g_windowStartNs < kIntervalNs)
        return;

    const double seconds = (now - g_windowStartNs) / 1e9;
    const auto ms = [&](long long ns) { return g_frames ? ns / 1e6 / g_frames : 0.0; };
    const NglBackend::Stats stats = NglBackend::takeStats();
    NspireSystem::log("[stat] t=%lus fps=%.2f frame=%.0fms(max %.0f) tick=%.0f light=%.0f render=%.0f present=%.0f "
                      "ticks=%d chunkupd=%d draws=%lu tris=%lu/%lu skip=%lu world=%d heap=%luK peak=%luK fail=%u mesh=%luK tex=%luK\n",
                      static_cast<unsigned long>(NspireSystem::micros() / 1000000u),
                      g_frames / seconds, ms(g_frameNs), g_worstFrameNs / 1e6, ms(g_tickNs), ms(g_lightingNs),
                      ms(g_renderNs), ms(g_displayNs), g_ticks, g_chunkUpdates,
                      stats.draws, stats.trianglesDrawn, stats.trianglesSubmitted, stats.trianglesSkipped, world != nullptr,
                      static_cast<unsigned long>(NspireSystem::heapUsedBytes() / 1024),
                      static_cast<unsigned long>(NspireSystem::heapPeakBytes() / 1024),
                      NspireSystem::heapFailures(),
                      static_cast<unsigned long>(NglBackend::meshBytes() / 1024),
                      static_cast<unsigned long>(NglBackend::textureBytes() / 1024));

    char phases[640];
    nspireProfileTakeTopPhases(phases, sizeof(phases), 20);
    if (phases[0] != '\0')
        NspireSystem::log("[phase] %s\n", phases);

    g_windowStartNs = now;
    g_tickNs = g_renderNs = g_displayNs = g_lightingNs = g_frameNs = g_worstFrameNs = 0;
    g_frames = g_ticks = g_chunkUpdates = 0;
}
}
