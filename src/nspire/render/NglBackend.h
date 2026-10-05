#pragma once

// Glue between OptiCraft's fixed-function RenderAPI and nGL.
//
// RenderAPI_NGL.cpp emulates the GL 1.x state machine the game was written
// against (matrix stacks, texture units, alpha test, blend, fog, two lights,
// the 1.2.5 lightmap) in software, transforms and clips every primitive itself,
// and hands nGL screen-space triangles to rasterise into the RGB565 back buffer
// NspireSystem owns. RenderTerrainAPI_NGL.cpp keeps terrain sections as
// captured RAM meshes behind the chunk-handle API the Wii path uses.

#include <cstddef>
#include <cstdint>

#include "platform/RenderAPI.h"

namespace NglBackend
{
void initialize();
void present();
void shutdown();

// Mesh store shared by persistent meshes and terrain chunk handles. A handle
// owns one captured mesh plus the translation its display list would have
// applied before the geometry.
int createMesh();
void destroyMesh(int handle);
void clearMesh(int handle);
void swapMeshes(int a, int b);
bool compileMesh(int handle, const RenderInterleavedMesh& mesh, float tx, float ty, float tz,
                 const RenderTerrainCompileInfo* info);
// Eye position for face-direction culling of terrain sections.
void setViewer(double x, double y, double z);
bool drawMesh(int handle);
std::size_t meshBytes();
std::size_t textureBytes();

// The 3D world pass of a frame. While it is open, drawing goes to a
// one-third-resolution target (107x80, a ninth of the pixels to fill); closing it scales
// that up 3x into the frame and clears depth for the GUI drawn on top.
void beginWorldPass();
void endWorldPass();

// The in-game HUD, cached: drawing it costs as much as half the terrain, and it
// rarely changes between frames.
//   if (hudBegin(key, cacheable)) { draw; if (hudEnd()) { draw; hudEnd(); } }
//   else hudEnd();
// hudBegin returns false when the HUD cached under the same `stateKey` is
// recent enough: hudEnd then copies it onto the frame. Otherwise the HUD is
// drawn into the cache (or, when not `cacheable` or while it uses blending,
// straight into the frame). hudEnd returns true when the HUD drawn into the
// cache turned out to blend with what is under it: the caller draws it once
// more, live.
bool hudBegin(std::uint32_t stateKey, bool cacheable);
bool hudEnd();

// Draw counters since the last call (ClientProfilerBackend_NSPIRE logs them).
struct Stats
{
    unsigned long draws = 0;
    unsigned long trianglesSubmitted = 0;
    unsigned long trianglesDrawn = 0;
    unsigned long trianglesSkipped = 0; // culled by face direction before the vertex stage
};
Stats takeStats();
}
