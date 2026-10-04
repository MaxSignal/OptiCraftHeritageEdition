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
