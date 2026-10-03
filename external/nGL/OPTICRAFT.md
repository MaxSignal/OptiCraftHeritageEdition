# nGL in OptiCraft

Vendored from https://github.com/Vogtinator/nGL at commit `f154107` (GPLv3, see
`LICENSE`): `gl.h`, `gl.cpp`, `fix.h`, `fastmath.h`, `fastmath.cpp`,
`triangle.inc.h`. The TI-Nspire port (`cmake/nspire.cmake`) builds them with the
configuration in `src/nspire/render/glconfig.h`.

The local changes are produced by `tools/patch_ngl.py` in the Nspire port
repository (re-runnable against a pristine checkout):

- headless host build (SDL 1.2 only with `NGL_HOST_SDL`);
- `NGLRasterState ngl_raster`: depth test / depth write / colour write switches,
  a 50% blend, a flat per-triangle colour multiply for textured triangles and an
  additive fog colour -- the fixed-function state OptiCraft's RenderAPI needs;
- texel fetches wrap with the power-of-two mask instead of reading past the
  bitmap at u == width;
- half-open spans, so pixels on an edge shared by two triangles are drawn once
  (blended quads otherwise showed a dark seam along the diagonal).

OptiCraft never uses nGL's matrix/perspective path: `src/platform/RenderAPI_NGL.cpp`
transforms, clips and projects itself and calls `nglDrawTriangleZClipped()` with
screen-space vertices.

**Licensing note:** OptiCraft Heritage is GPLv2 (no "or later" grant in the
repository) and nGL is GPLv3. Distributing a binary that combines them needs the
OptiCraft authors' agreement; building it for your own calculator does not.
