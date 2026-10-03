#pragma once

// nGL build configuration for OptiCraft (nGL includes "glconfig.h").
//
// Textured triangles with nGL's black-is-transparent variant, Z clipping on.
// OptiCraft never feeds geometry through nGL's own matrix/perspective path (it
// transforms, clips and projects in NglBackend and hands nGL screen-space
// triangles), so the matrix stack only has to exist.
#define TEXTURE_SUPPORT
#define Z_CLIPPING
#define CLIP_PLANE 25
#define MATRIX_STACK_SIZE 4
