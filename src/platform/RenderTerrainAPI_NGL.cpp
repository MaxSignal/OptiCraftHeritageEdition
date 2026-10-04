// Terrain chunk handles on nGL.
//
// The Nspire runs the Wii's section-build state machine (PLATFORM_HANDLE_TERRAIN),
// which compiles each section pass into a staging handle and swaps it live when
// the pass is complete. Here a handle is a captured RAM mesh plus the section
// translation a display list would have applied (NglBackend mesh store), and a
// "chunk batch" is simply replaying the handles in order.
#include "platform/RenderTerrainAPI.h"
#include "platform/RenderAPI.h"

#include "nspire/render/NglBackend.h"

int renderTerrainCreateChunkHandle() { return NglBackend::createMesh(); }
void renderTerrainDestroyChunkHandle(int handle) { NglBackend::destroyMesh(handle); }
void renderTerrainClearChunkHandle(int handle) { NglBackend::clearMesh(handle); }
void renderTerrainSwapChunkHandles(int liveHandle, int stagingHandle) { NglBackend::swapMeshes(liveHandle, stagingHandle); }
bool renderTerrainBeginChunkBatch(int) { return true; }
bool renderTerrainAppendChunk(int handle)
{
    NglBackend::drawMesh(handle);
    return true;
}
void renderTerrainEndChunkBatch() {}
void renderTerrainCaptureCamera() {}
void renderTerrainSetViewerPosition(double x, double y, double z) { NglBackend::setViewer(x, y, z); }
// Terrain fog is the regular fixed-function fog state, already set by the game.
void renderTerrainSetFog(RenderFogMode, float, float, float, float, float, float, float) {}
void renderTerrainSetEarlyDepth(bool) {}
bool renderTerrainSortOpaqueFaces(const std::vector<int_t>&, std::vector<int_t>&, int, int) { return false; }
bool renderTerrainBeginPass(int texture, RenderTerrainPass)
{
    renderBindTexture(texture);
    return true;
}
void renderTerrainEndPass(RenderTerrainPass) {}
std::size_t renderTerrainLiveBytes() { return NglBackend::meshBytes(); }
std::size_t renderTerrainStagingBytes() { return 0; }

bool renderTerrainCaptureFrame(RenderTerrainFrame& out) { out = RenderTerrainFrame{}; return false; }
RenderTerrainDrawResult renderTerrainDrawSection(const RenderTerrainFrame&, const RenderTerrainSectionView&, const RenderTerrainFallbackDraw&) { return {}; }

void renderTerrainCacheInit(RenderTerrainBackendCache& cache) { cache.initialized = true; }
void renderTerrainCacheDestroy(RenderTerrainBackendCache& cache) { cache.initialized = false; }
void renderTerrainCacheReset(RenderTerrainBackendCache&) {}
void renderTerrainCacheRelease(RenderTerrainBackendCache&) {}
std::size_t renderTerrainCacheRamBytes(const RenderTerrainBackendCache&) { return 0; }
void renderTerrainCacheRamBreakdown(const RenderTerrainBackendCache&, RenderTerrainCacheRamBreakdown&) {}
bool renderTerrainCacheSortFaces(RenderTerrainBackendCache&, const int_t*, int_t*, int_t) { return false; }
bool renderTerrainCacheBuildOpaque(RenderTerrainBackendCache&, const int_t*, std::size_t, int_t, int_t, bool, bool, bool) { return false; }
bool renderTerrainCacheOpaqueValid(const RenderTerrainBackendCache&) { return false; }
int_t renderTerrainCacheOpaqueVertexCount(const RenderTerrainBackendCache&) { return 0; }
const void* renderTerrainCacheFaceGroups(const RenderTerrainBackendCache&) { return nullptr; }
const void* renderTerrainCacheOpaqueMesh(const RenderTerrainBackendCache&) { return nullptr; }

bool renderTerrainIsGreedyCube(Block*) { return false; }
bool renderTerrainGreedyMeshFace(ChunkCache&, int, int, int, int, int, int, int) { return false; }

// WorldRendererWii.cpp (shared through PLATFORM_HANDLE_TERRAIN) resets the GX
// terrain vertex state after replaying extra CTM meshes. nGL has no such state.
void wii_native_invalidate_terrain_pass() {}
