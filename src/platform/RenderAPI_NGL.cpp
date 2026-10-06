// RenderAPI on nGL: a software fixed-function pipeline for the TI-Nspire.
//
// The game drives a GL 1.x-shaped API. This file keeps that state machine in
// software -- three matrix stacks, two texture units, alpha test, blend, depth
// switches, fog, two directional lights and the 1.2.5 lightmap on unit 1 -- and
// does the vertex work itself: transform to clip space, clip against the near,
// far and guard-band planes, perspective divide, viewport, face culling. What
// reaches nGL is a screen-space triangle with texel-unit texture coordinates and
// a 16-bit depth, rasterised by nglDrawTriangleZClipped() with the per-draw
// state in ngl_raster (see external/nGL and tools/patch_ngl.py).
//
// Deliberate simplifications, all cheaper than the GL behaviour they replace:
//   * colour is flat per triangle: the average of its three lit vertices,
//     multiplied into the texels (RGB565, no per-pixel interpolation);
//   * texture coordinates interpolate affinely (nGL has no perspective
//     correction), which shows only on large near quads;
//   * alpha is one bit per texel (black = transparent in nGL), and blending is
//     a 50% average chosen per triangle from its vertex alpha;
//   * fog is evaluated once per triangle at its centroid;
//   * no mipmaps or filtering: nearest texel from level 0.
#include "platform/RenderAPI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gl.h"
#include "nspire/NspireSystem.h"
#include "nspire/render/NglBackend.h"
#include "nspire/render/NglFixed.h"
#include "platform/Profiler.h"

namespace
{
// ---------------------------------------------------------------------------
// Matrices (column-major, as GL)
// ---------------------------------------------------------------------------
struct Mat4
{
    float m[16];
};

Mat4 identityMatrix()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r.m[c * 4 + row] = a.m[0 * 4 + row] * b.m[c * 4 + 0] + a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                               a.m[2 * 4 + row] * b.m[c * 4 + 2] + a.m[3 * 4 + row] * b.m[c * 4 + 3];
    return r;
}

bool isIdentity(const Mat4& a)
{
    const Mat4 id = identityMatrix();
    return std::memcmp(a.m, id.m, sizeof(a.m)) == 0;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
struct NglTexture
{
    int width = 0;        // as uploaded; texture coordinates scale by these
    int height = 0;
    int stride = 0;       // power-of-two storage nGL indexes and wraps with
    int rows = 0;
    std::vector<COLOR> pixels;
    std::vector<std::uint8_t> rgba; // kept only for tiny textures (the lightmap)
    bool transparent = false;
    // Atlases (square, 16 x 16 tiles): 1 for a tile with transparent texels.
    // Terrain faces of the other tiles rasterise without the per-texel key test.
    std::vector<std::uint8_t> tileClear;
    std::vector<std::uint32_t> tileAverage; // 0xRRGGBB of each tile's opaque texels
    int tileShift = 0; // log2 of the tile size in texels
    TEXTURE desc{};
};

int nextPow2(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

COLOR toRgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<COLOR>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Light
{
    float dir[3] = {0.0f, 0.0f, 1.0f}; // eye space, normalised
    float diffuse[3] = {0.0f, 0.0f, 0.0f};
    float ambient[3] = {0.0f, 0.0f, 0.0f};
};

struct State
{
    // Modelview, projection, and one texture matrix per texture unit: the
    // 1.2.5 lightmap programs unit 1's texture matrix (scale 1/256), which must
    // not touch the terrain/item coordinates on unit 0.
    std::vector<Mat4> stacks[4];
    int matrixMode = 0; // 0 modelview, 1 projection, 2 texture (of activeUnit)

    bool texture2d[2] = {false, false};
    int boundTexture[2] = {0, 0};
    int activeUnit = 0;

    bool alphaTest = false;
    bool blend = false;
    bool depthTest = false;
    bool depthMask = true;
    bool colorMask = true;
    bool cullFace = false;
    bool cullBack = true;
    bool cullFront = false;
    bool fog = false;
    bool lighting = false;
    bool light[2] = {false, false};
    bool polygonOffset = false;
    float polygonOffsetUnits = 0.0f;
    RenderCompare depthFunc = RenderCompare::Less;
    RenderBlendFactor blendSrc = RenderBlendFactor::One;
    RenderBlendFactor blendDst = RenderBlendFactor::Zero;

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float lightmapCoord[2] = {240.0f, 240.0f};
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    RenderFogMode fogMode = RenderFogMode::Exp;
    float fogDensity = 1.0f;
    float fogStart = 0.0f;
    float fogEnd = 1.0f;
    float fogColor[3] = {0.0f, 0.0f, 0.0f};

    Light lights[2];
    float lightModelAmbient[3] = {0.2f, 0.2f, 0.2f};

    int viewport[4] = {0, 0, NspireSystem::kScreenWidth, NspireSystem::kScreenHeight};
};

State g_state;
NglBackend::Stats g_stats;
std::unordered_map<int, std::unique_ptr<NglTexture>> g_textures;
int g_nextTextureName = 1;
bool g_initialized = false;

// Low-resolution world pass (NglBackend::beginWorldPass).
#ifndef _TINSPIRE
const bool g_lowResWorld = std::getenv("NSPIRE_SIM_FULLRES") == nullptr;
#else
constexpr bool g_lowResWorld = true;
#endif
bool g_inWorldPass = false;
// The world pass is drawn at a third of the screen in each direction and
// scaled up 3x: 107x80 (the 321st column falls off the right edge).
constexpr int kWorldScale = 3;
constexpr int kWorldW = (NspireSystem::kScreenWidth + kWorldScale - 1) / kWorldScale;
constexpr int kWorldH = NspireSystem::kScreenHeight / kWorldScale;
std::vector<COLOR> g_lowResBuffer;
COLOR* g_target = nullptr; // the buffer nGL draws into
// Set when the world pass closes: the GUI drawn on top needs a clear depth
// buffer, but only if something depth-tested is drawn at all (a frame whose HUD
// comes from the cache draws nothing after the world).
bool g_guiDepthStale = false;

// Drawing a terrain section (drawStoredMesh): every quad samples one atlas tile.
bool g_terrainDraw = false;
// The face groups drawStoredMesh hands drawTerrainQuads, in quads from the
// start of the mesh, each walked from its nearest face (reverse: last first).
struct TerrainRun
{
    int first, count;
    bool reverse;
};
TerrainRun g_terrainRuns[6];
int g_terrainRunCount = 0;
// Eye depth (clip w, Q12) beyond which terrain faces are drawn flat (drawTriangle).
constexpr std::int32_t kFlatTerrainW = 7 << 12;

// HUD cache (NglBackend::hudBegin). The HUD is drawn into its own frame over a
// key colour every few frames; the frames between copy the non-key pixels of
// it onto the new world, row span by row span.
enum class HudMode
{
    Off,     // not inside hudBegin/hudEnd
    Live,    // drawing straight into the frame
    Capture, // drawing into g_hudBuffer
};
constexpr COLOR kHudKey = 0xF81F; // magenta, which no HUD texture uses
HudMode g_hudMode = HudMode::Off;
std::vector<COLOR> g_hudBuffer;
// Runs of HUD pixels in g_hudBuffer: start index | length << 17.
std::vector<std::uint32_t> g_hudRuns;
bool g_hudValid = false;
bool g_hudLive = false;      // the last HUD blended: keep drawing it live until it stops
bool g_hudSawBlend = false;  // a 50% blend was drawn since hudBegin
std::uint32_t g_hudKey = 0;
int g_hudAge = 0;

int currentStackIndex()
{
    return g_state.matrixMode == 2 ? 2 + g_state.activeUnit : g_state.matrixMode;
}

Mat4& currentMatrix()
{
    return g_state.stacks[currentStackIndex()].back();
}

NglTexture* findTexture(int name)
{
    auto it = g_textures.find(name);
    return it == g_textures.end() ? nullptr : it->second.get();
}

NglTexture* textureForUpload(int name)
{
    std::unique_ptr<NglTexture>& slot = g_textures[name];
    if (!slot)
        slot.reset(new NglTexture());
    return slot.get();
}

void allocateTexture(NglTexture& tex, int width, int height)
{
    tex.width = width;
    tex.height = height;
    tex.stride = nextPow2(std::max(width, 1));
    tex.rows = nextPow2(std::max(height, 1));
    tex.pixels.assign(static_cast<std::size_t>(tex.stride) * tex.rows, 0);
    tex.transparent = false;
    tex.tileShift = 0;
    std::vector<std::uint8_t>().swap(tex.tileClear);
    if (width == height && width >= 16 && (width & (width - 1)) == 0)
    {
        while ((16 << tex.tileShift) < width)
            ++tex.tileShift;
        tex.tileClear.assign(256, 1);
        tex.tileAverage.assign(256, 0x808080u);
    }
    if (width * height <= 32 * 32)
        tex.rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
    else
        std::vector<std::uint8_t>().swap(tex.rgba);
    tex.desc.width = static_cast<std::uint16_t>(tex.stride);
    tex.desc.height = static_cast<std::uint16_t>(tex.rows);
    tex.desc.has_transparency = true;
    tex.desc.transparent_color = 0;
    tex.desc.bitmap = tex.pixels.data();
}

void writeTexels(NglTexture& tex, int x0, int y0, int w, int h, const std::uint8_t* src)
{
    for (int y = 0; y < h; ++y)
    {
        const int ty = y0 + y;
        if (ty < 0 || ty >= tex.height)
            continue;
        for (int x = 0; x < w; ++x)
        {
            const int tx = x0 + x;
            if (tx < 0 || tx >= tex.width)
                continue;
            const std::uint8_t* p = src + (static_cast<std::size_t>(y) * w + x) * 4;
            COLOR c;
            if (p[3] < 128)
            {
                c = 0; // nGL's transparent texel
                tex.transparent = true;
            }
            else
            {
                c = toRgb565(p[0], p[1], p[2]);
                if (c == 0)
                    c = 0x0841; // keep opaque black distinguishable from "transparent"
            }
            tex.pixels[static_cast<std::size_t>(ty) * tex.stride + tx] = c;
            if (!tex.rgba.empty())
                std::memcpy(&tex.rgba[(static_cast<std::size_t>(ty) * tex.width + tx) * 4], p, 4);
        }
    }
    if (tex.tileClear.empty() || w <= 0 || h <= 0)
        return;
    // Rescan the tiles the upload touched.
    const int shift = tex.tileShift, size = 1 << shift;
    const int tx0 = std::max(x0, 0) >> shift, ty0 = std::max(y0, 0) >> shift;
    const int tx1 = std::min((x0 + w - 1) >> shift, 15), ty1 = std::min((y0 + h - 1) >> shift, 15);
    for (int tv = ty0; tv <= ty1; ++tv)
        for (int tu = tx0; tu <= tx1; ++tu)
        {
            bool clear = false;
            std::uint32_t sum[3] = {0, 0, 0}, opaque = 0;
            for (int y = 0; y < size; ++y)
            {
                const COLOR* row = &tex.pixels[static_cast<std::size_t>((tv << shift) + y) * tex.stride + (tu << shift)];
                for (int x = 0; x < size; ++x)
                {
                    const COLOR c = row[x];
                    if (c == 0)
                    {
                        clear = true;
                        continue;
                    }
                    sum[0] += ((c >> 11) & 31) << 3;
                    sum[1] += ((c >> 5) & 63) << 2;
                    sum[2] += (c & 31) << 3;
                    ++opaque;
                }
            }
            const std::size_t index = static_cast<std::size_t>(tv) * 16 + tu;
            tex.tileClear[index] = clear ? 1 : 0;
            if (opaque > 0)
                tex.tileAverage[index] = (sum[0] / opaque) << 16 | (sum[1] / opaque) << 8 | (sum[2] / opaque);
        }
}

// ---------------------------------------------------------------------------
// Vertex processing
// ---------------------------------------------------------------------------
// Normal-word tag of a merged terrain face (mergeStoredFaces): repeated tile.
constexpr std::uint32_t kTileTag = 0x80000000u;

struct ClipVertex
{
    std::int32_t x, y, z, w; // clip space, Q12
    std::int32_t u, v;       // texel units, Q8
    int r, g, b, a;          // lit colour, 0..255
    int fog;                 // fog visibility 0..256 (256 = clear)
    unsigned outcode;        // clip planes this vertex is outside of (bit per plane)
    std::uint32_t tileTag;   // merged terrain face: repeated atlas tile (kTileTag)
    VERTEX screen;           // projected nGL vertex, valid when outcode == 0
};

// A matrix in fixed point: the three linear columns and the bottom row in Q16,
// the translation column (rows 0-2) in Q12, which keeps a range of +-500k
// blocks there while rotations and scales keep 16 fractional bits.
struct FixedMatrix
{
    std::int32_t m[16];
};

constexpr int fixedFormat(int index)
{
    return (index >= 12 && index < 15) ? NglFixed::kPosShift : NglFixed::kMatShift;
}

FixedMatrix toFixed(const Mat4& a)
{
    FixedMatrix r;
    for (int i = 0; i < 16; ++i)
        r.m[i] = NglFixed::fromFloat(a.m[i], fixedFormat(i));
    return r;
}

// a * b, column-major, every term rescaled to the result entry's format.
FixedMatrix multiply(const FixedMatrix& a, const FixedMatrix& b)
{
    FixedMatrix r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            const int target = fixedFormat(c * 4 + row);
            std::int64_t sum = 0;
            for (int k = 0; k < 4; ++k)
            {
                const int shift = fixedFormat(k * 4 + row) + fixedFormat(c * 4 + k) - target;
                sum += (static_cast<std::int64_t>(a.m[k * 4 + row]) * b.m[c * 4 + k]) >> shift;
            }
            r.m[c * 4 + row] = static_cast<std::int32_t>(sum);
        }
    return r;
}

// m * translate(t), t in Q12: only the translation column changes.
void translateFixed(FixedMatrix& m, const std::int32_t t[3])
{
    for (int row = 0; row < 4; ++row)
    {
        const std::int64_t d = static_cast<std::int64_t>(m.m[row]) * t[0] +
                               static_cast<std::int64_t>(m.m[4 + row]) * t[1] +
                               static_cast<std::int64_t>(m.m[8 + row]) * t[2];
        // Linear terms are Q16 * Q12 = Q28: Q12 for rows 0-2, Q16 for row 3.
        m.m[12 + row] += static_cast<std::int32_t>(d >> (row < 3 ? 16 : 12));
    }
}

// Fixed-point copies of the modelview, projection and their product, rebuilt
// only when the game changed a matrix since the last draw.
unsigned g_matrixSerial[4] = {1, 1, 1, 1};
struct MatrixCache
{
    unsigned mvSerial = 0, projSerial = 0;
    FixedMatrix mv, proj, mvp;
    // Orthographic projection (the GUI): the product again with every term in
    // Q24. Q16 holds 2/320 only to 0.25% and the Q12 translations to 1/4096,
    // enough to put a glyph's edges a hair off their pixels; the rasteriser
    // then got span widths of 7 or 9 for 8-texel glyphs and the text came out
    // garbled. Exact here, a pixel coordinate lands on the pixel.
    bool ortho = false;
    std::int32_t projHi[16] = {}; // the orthographic projection in Q24
    std::int32_t mvpHi[16] = {};
} g_matrixCache;

constexpr int kHiShift = 24;

// proj (an orthographic projection, every term Q24) * mv (FixedMatrix: linear
// terms Q16, translation Q12, bottom row Q16), all in Q24. Integer products
// only: the GUI changes the modelview for nearly every element it draws.
void orthoProductHi(const std::int32_t proj[16], const FixedMatrix& mv, std::int32_t out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            std::int64_t sum = 0;
            for (int k = 0; k < 4; ++k)
            {
                const int shift = fixedFormat(c * 4 + k); // the mv term's fraction bits
                sum += (static_cast<std::int64_t>(proj[k * 4 + row]) * mv.m[c * 4 + k]) >> shift;
            }
            out[c * 4 + row] = static_cast<std::int32_t>(sum);
        }
}

void bumpCurrentMatrix() { ++g_matrixSerial[currentStackIndex()]; }

struct DrawSetup
{
    FixedMatrix mv;
    FixedMatrix mvp;
    bool ortho = false;          // positions from mvpHi (see MatrixCache)
    std::int32_t mvpHi[16] = {}; // Q24, translation included
    const NglTexture* texture = nullptr;
    const NglTexture* lightmap = nullptr;
    bool textureMatrix = false;
    std::int32_t texMatrix[6] = {}; // s' = m0 s + m2 t + m4, t' = m1 s + m3 t + m5 (Q16)
    int color[4] = {255, 255, 255, 255}; // current colour, 0..255
    int lightmapS = 240, lightmapT = 240;
    // Fog, per draw.
    bool fog = false;
    RenderFogMode fogMode = RenderFogMode::Exp;
    std::int32_t fogEnd = 0;               // Q12
    NglFixed::Reciprocal fogInvSpan{1, 0}; // 1 / (end - start)
    bool fogSpanValid = false;
    std::int32_t fogDensity = 0;           // Q16
    int fogColor[3] = {0, 0, 0};           // 0..255
    // Lighting in eye space (entities and items only).
    bool lighting = false;
    std::int32_t lightDir[2][3] = {};      // Q14 unit vectors
    std::int32_t lightDiffuse[2][3] = {};  // Q8
    std::int32_t ambient[3] = {};          // Q8
    bool lightOn[2] = {false, false};
    int constantLit[3] = {256, 256, 256};  // for meshes without per-vertex normals
    int texWidth = 0, texHeight = 0;
    int viewport[4] = {0, 0, 0, 0};
    int screenHeight = 0;                  // of the current target
    std::int32_t depthOffset = 0;          // Q8, polygon offset
};

std::vector<ClipVertex> g_vertexScratch;

float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

int toByte(float v)
{
    return clamp255((NglFixed::fromFloat(v, 8) * 255 + 128) >> 8);
}

// The 1.2.5 lightmap: unit 1 samples the 16x16 light texture at (block light,
// sky light), each scaled by 16 in the brightness word.
const std::uint8_t* lightmapTexel(const NglTexture* lightmap, int s, int t)
{
    int block = s >> 4;
    int sky = t >> 4;
    block = block < 0 ? 0 : (block > 15 ? 15 : block);
    sky = sky < 0 ? 0 : (sky > 15 ? 15 : sky);
    if (lightmap->width < 16 || lightmap->height < 16 || lightmap->rgba.empty())
        return nullptr;
    return &lightmap->rgba[(static_cast<std::size_t>(sky) * lightmap->width + block) * 4];
}

constexpr int kGuardBand = 2;

unsigned computeOutcode(std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t w)
{
    unsigned code = 0;
    if (z + w < 0) code |= 1u;                    // near
    if (w - z < 0) code |= 2u;                    // far
    const std::int32_t gw = kGuardBand * w;
    if (gw + x < 0) code |= 4u;                   // left (guard band)
    if (gw - x < 0) code |= 8u;                   // right
    if (gw + y < 0) code |= 16u;                  // bottom
    if (gw - y < 0) code |= 32u;                  // top
    return code;
}

int fogVisibility256(const DrawSetup& setup, std::int32_t distance)
{
    int v;
    switch (setup.fogMode)
    {
    case RenderFogMode::Linear:
        v = setup.fogSpanValid ? NglFixed::mulReciprocal(setup.fogEnd - distance, setup.fogInvSpan, 8) : 256;
        break;
    case RenderFogMode::Exp2:
    {
        const std::int32_t d = NglFixed::mulShift(setup.fogDensity, distance, 16);
        v = NglFixed::expNeg256(d > (64 << 12) ? (64 << 12) : NglFixed::mulShift(d, d, 12));
        break;
    }
    default:
        v = NglFixed::expNeg256(NglFixed::mulShift(setup.fogDensity, distance, 16));
        break;
    }
    return v < 0 ? 0 : (v > 256 ? 256 : v);
}

VERTEX project(const DrawSetup& setup, const ClipVertex& c)
{
    const NglFixed::Reciprocal rw = NglFixed::reciprocal(c.w);
    const std::int32_t ndcX = NglFixed::mulReciprocal(c.x, rw, 16);
    const std::int32_t ndcY = NglFixed::mulReciprocal(c.y, rw, 16);
    const std::int32_t ndcZ = NglFixed::mulReciprocal(c.z, rw, 16);
    const int* vp = setup.viewport;
    VERTEX out;
    out.x.value = (vp[0] << 8) + static_cast<std::int32_t>((static_cast<std::int64_t>(ndcX + 65536) * vp[2]) >> 9);
    out.y.value = ((setup.screenHeight - vp[1]) << 8) -
                  static_cast<std::int32_t>((static_cast<std::int64_t>(ndcY + 65536) * vp[3]) >> 9);
    std::int32_t depth = (ndcZ + 65536) >> 1; // 0..1 in Q16
    depth = depth < 0 ? 0 : (depth > 65536 ? 65536 : depth);
    std::int32_t zb = (32 << 8) + static_cast<std::int32_t>((static_cast<std::int64_t>(depth) * 65000) >> 8) + setup.depthOffset;
    if (zb < (26 << 8))
        zb = 26 << 8;
    out.z.value = zb;
    out.u.value = c.u;
    out.v.value = c.v;
    out.c = 0;
    return out;
}

// Lighting factor per colour channel, 0..256: ambient + sum(diffuse * N.L)
// for a normal already in eye space (Q14, not necessarily unit length).
void lightFactors(const DrawSetup& setup, const std::int32_t en[3], int lit[3])
{
    const std::uint64_t len2 = static_cast<std::uint64_t>(static_cast<std::int64_t>(en[0]) * en[0] +
                                                         static_cast<std::int64_t>(en[1]) * en[1] +
                                                         static_cast<std::int64_t>(en[2]) * en[2]);
    std::int32_t n[3] = {0, 0, 0};
    if (const std::uint32_t len = NglFixed::isqrt(len2))
    {
        const NglFixed::Reciprocal rl = NglFixed::reciprocal(static_cast<std::int32_t>(len));
        for (int i = 0; i < 3; ++i)
            n[i] = NglFixed::mulReciprocal(en[i], rl, 14);
    }
    std::int32_t acc[3] = {setup.ambient[0], setup.ambient[1], setup.ambient[2]};
    for (int i = 0; i < 2; ++i)
    {
        if (!setup.lightOn[i])
            continue;
        const std::int32_t d = (n[0] * setup.lightDir[i][0] + n[1] * setup.lightDir[i][1] + n[2] * setup.lightDir[i][2]) >> 20;
        if (d <= 0)
            continue; // d is N.L in Q8
        for (int c = 0; c < 3; ++c)
            acc[c] += (setup.lightDiffuse[i][c] * d) >> 8;
    }
    for (int c = 0; c < 3; ++c)
        lit[c] = acc[c] < 0 ? 0 : (acc[c] > 256 ? 256 : acc[c]);
}

void eyeNormal(const DrawSetup& setup, const std::int32_t n[3], std::int32_t en[3])
{
    // Q16 matrix * Q7 normal = Q23; keep Q14.
    const std::int32_t* m = setup.mv.m;
    for (int r = 0; r < 3; ++r)
        en[r] = static_cast<std::int32_t>((static_cast<std::int64_t>(m[r]) * n[0] + static_cast<std::int64_t>(m[4 + r]) * n[1] +
                                           static_cast<std::int64_t>(m[8 + r]) * n[2]) >> 9);
}

// The last normal processVertex lit within the current draw (0: none).
std::uint32_t g_litKey = 0;
int g_lit[3] = {256, 256, 256};

// `fixedData`: the vertex comes from a mesh this backend captured, whose
// positions are already Q12 and texture coordinates Q16 (see
// renderCaptureInterleaved); otherwise they are the Tessellator's floats.
void processVertex(const DrawSetup& setup, const RenderInterleavedMesh& mesh, const std::uint8_t* base, bool fixedData,
                   ClipVertex& out)
{
    std::int32_t px, py, pz;
    if (mesh.positionShort)
    {
        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(base);
        px = s[0] << NglFixed::kPosShift;
        py = s[1] << NglFixed::kPosShift;
        pz = s[2] << NglFixed::kPosShift;
    }
    else if (fixedData)
    {
        const std::int32_t* p = reinterpret_cast<const std::int32_t*>(base);
        px = p[0];
        py = p[1];
        pz = p[2];
    }
    else
    {
        const float* f = reinterpret_cast<const float*>(base);
        px = NglFixed::fromFloat(f[0], NglFixed::kPosShift);
        py = NglFixed::fromFloat(f[1], NglFixed::kPosShift);
        pz = NglFixed::fromFloat(f[2], NglFixed::kPosShift);
    }

    if (setup.ortho)
    {
        // Q24 matrix * Q12 position = Q36, rounded to Q12.
        const std::int32_t* h = setup.mvpHi;
        const auto row = [&](int r) {
            return static_cast<std::int32_t>((static_cast<std::int64_t>(h[r]) * px + static_cast<std::int64_t>(h[4 + r]) * py +
                                              static_cast<std::int64_t>(h[8 + r]) * pz +
                                              (static_cast<std::int64_t>(h[12 + r]) << NglFixed::kPosShift) +
                                              (std::int64_t(1) << (kHiShift - 1))) >> kHiShift);
        };
        out.x = row(0);
        out.y = row(1);
        out.z = row(2);
        out.w = row(3);
    }
    else
    {
        // Q16 matrix * Q12 position = Q28; >> 16 gives Q12 clip coordinates.
        const std::int32_t* m = setup.mvp.m;
        out.x = static_cast<std::int32_t>((static_cast<std::int64_t>(m[0]) * px + static_cast<std::int64_t>(m[4]) * py +
                                           static_cast<std::int64_t>(m[8]) * pz) >> 16) + m[12];
        out.y = static_cast<std::int32_t>((static_cast<std::int64_t>(m[1]) * px + static_cast<std::int64_t>(m[5]) * py +
                                           static_cast<std::int64_t>(m[9]) * pz) >> 16) + m[13];
        out.z = static_cast<std::int32_t>((static_cast<std::int64_t>(m[2]) * px + static_cast<std::int64_t>(m[6]) * py +
                                           static_cast<std::int64_t>(m[10]) * pz) >> 16) + m[14];
        out.w = static_cast<std::int32_t>((static_cast<std::int64_t>(m[3]) * px + static_cast<std::int64_t>(m[7]) * py +
                                           static_cast<std::int64_t>(m[11]) * pz) >> 16) + (m[15] >> 4);
    }

    if (setup.fog)
    {
        // Eye-plane distance (GL's default fog coordinate), not the radial
        // distance: three multiplies instead of nine and a square root.
        const std::int32_t* mv = setup.mv.m;
        std::int32_t ez = static_cast<std::int32_t>((static_cast<std::int64_t>(mv[2]) * px + static_cast<std::int64_t>(mv[6]) * py +
                                                     static_cast<std::int64_t>(mv[10]) * pz) >> 16) + mv[14];
        if (ez < 0)
            ez = -ez;
        out.fog = fogVisibility256(setup, ez);
    }
    else
        out.fog = 256;

    // Texture coordinates, in texels of level 0 (Q8).
    if (setup.texture != nullptr)
    {
        std::int32_t s = 0, t = 0; // Q16
        if (mesh.hasTexture)
        {
            if (fixedData)
            {
                const std::int32_t* uv = reinterpret_cast<const std::int32_t*>(base + mesh.texCoordOffset);
                s = uv[0];
                t = uv[1];
            }
            else
            {
                const float* uv = reinterpret_cast<const float*>(base + mesh.texCoordOffset);
                s = NglFixed::fromFloat(uv[0], 16);
                t = NglFixed::fromFloat(uv[1], 16);
            }
        }
        // A merged terrain face carries its repeated tile in the normal word
        // and texture coordinates in tile units (mergeStoredFaces).
        out.tileTag = 0;
        if (fixedData && !mesh.hasNormals)
        {
            const std::uint32_t tag = *reinterpret_cast<const std::uint32_t*>(base + mesh.normalOffset);
            if (tag & kTileTag)
                out.tileTag = tag;
        }
        if (out.tileTag != 0)
        {
            const int tile = setup.texWidth >> 4;
            out.u = static_cast<std::int32_t>((static_cast<std::int64_t>(s) * tile) >> 8);
            out.v = static_cast<std::int32_t>((static_cast<std::int64_t>(t) * tile) >> 8);
        }
        else
        {
            if (setup.textureMatrix)
            {
                const std::int32_t* tm = setup.texMatrix;
                const std::int32_t ns = NglFixed::mulShift(tm[0], s, 16) + NglFixed::mulShift(tm[2], t, 16) + tm[4];
                const std::int32_t nt = NglFixed::mulShift(tm[1], s, 16) + NglFixed::mulShift(tm[3], t, 16) + tm[5];
                s = ns;
                t = nt;
            }
            out.u = static_cast<std::int32_t>((static_cast<std::int64_t>(s) * setup.texWidth) >> 8);
            out.v = static_cast<std::int32_t>((static_cast<std::int64_t>(t) * setup.texHeight) >> 8);
        }
    }
    else
    {
        out.u = out.v = 0;
        out.tileTag = 0;
    }

    // Colour, in integers: vertex colour or current colour.
    if (mesh.hasColor)
    {
        const std::uint8_t* c = base + mesh.colorOffset;
        out.r = c[0];
        out.g = c[1];
        out.b = c[2];
        out.a = c[3];
    }
    else
    {
        out.r = setup.color[0];
        out.g = setup.color[1];
        out.b = setup.color[2];
        out.a = setup.color[3];
    }

    // Fixed-function lighting: Minecraft's two directional "standard item"
    // lights with colour material, i.e. colour * (ambient + sum(diffuse*N.L)).
    if (setup.lighting)
    {
        int lit[3] = {setup.constantLit[0], setup.constantLit[1], setup.constantLit[2]};
        if (mesh.hasNormals)
        {
            const std::int8_t* nb = reinterpret_cast<const std::int8_t*>(base + mesh.normalOffset);
            // A box face's four corners share a normal: light it once (the
            // normalisation is an integer square root).
            const std::uint32_t key = 0x1000000u | static_cast<std::uint8_t>(nb[0]) |
                                      static_cast<std::uint32_t>(static_cast<std::uint8_t>(nb[1])) << 8 |
                                      static_cast<std::uint32_t>(static_cast<std::uint8_t>(nb[2])) << 16;
            if (key == g_litKey)
            {
                lit[0] = g_lit[0];
                lit[1] = g_lit[1];
                lit[2] = g_lit[2];
            }
            else
            {
                const std::int32_t n[3] = {nb[0], nb[1], nb[2]};
                std::int32_t en[3];
                eyeNormal(setup, n, en);
                lightFactors(setup, en, lit);
                g_litKey = key;
                g_lit[0] = lit[0];
                g_lit[1] = lit[1];
                g_lit[2] = lit[2];
            }
        }
        out.r = (out.r * lit[0]) >> 8;
        out.g = (out.g * lit[1]) >> 8;
        out.b = (out.b * lit[2]) >> 8;
    }

    if (setup.lightmap != nullptr)
    {
        int s = setup.lightmapS, t = setup.lightmapT;
        if (mesh.hasBrightness)
        {
            const std::uint32_t b = *reinterpret_cast<const std::uint32_t*>(base + mesh.brightnessOffset);
            s = static_cast<int>(b & 0xFFFFu);
            t = static_cast<int>(b >> 16);
        }
        if (const std::uint8_t* lm = lightmapTexel(setup.lightmap, s, t))
        {
            out.r = (out.r * (lm[0] + 1)) >> 8;
            out.g = (out.g * (lm[1] + 1)) >> 8;
            out.b = (out.b * (lm[2] + 1)) >> 8;
        }
    }

    out.outcode = computeOutcode(out.x, out.y, out.z, out.w);
    if (out.outcode == 0)
        out.screen = project(setup, out);
}

// ---------------------------------------------------------------------------
// Clipping and rasterisation
// ---------------------------------------------------------------------------
// t in Q16.
ClipVertex lerpVertex(const ClipVertex& a, const ClipVertex& b, std::int32_t t)
{
    const auto lerp = [t](std::int32_t from, std::int32_t to) {
        return from + static_cast<std::int32_t>((static_cast<std::int64_t>(to - from) * t) >> 16);
    };
    ClipVertex r = a;
    r.x = lerp(a.x, b.x);
    r.y = lerp(a.y, b.y);
    r.z = lerp(a.z, b.z);
    r.w = lerp(a.w, b.w);
    r.u = lerp(a.u, b.u);
    r.v = lerp(a.v, b.v);
    return r;
}

// Signed distance of a vertex to one clip plane; >= 0 is inside.
std::int32_t planeDistance(const ClipVertex& v, int plane)
{
    switch (plane)
    {
    case 0: return v.z + v.w;                 // near
    case 1: return v.w - v.z;                 // far
    case 2: return kGuardBand * v.w + v.x;    // left (guard band)
    case 3: return kGuardBand * v.w - v.x;    // right
    case 4: return kGuardBand * v.w + v.y;    // bottom
    default: return kGuardBand * v.w - v.y;   // top
    }
}

int clipPolygon(ClipVertex* poly, int count, ClipVertex* scratch, unsigned planes)
{
    ClipVertex* in = poly;
    ClipVertex* out = scratch;
    for (int plane = 0; plane < 6 && count > 0; ++plane)
    {
        if ((planes & (1u << plane)) == 0)
            continue;
        int outCount = 0;
        for (int i = 0; i < count; ++i)
        {
            const ClipVertex& a = in[i];
            const ClipVertex& b = in[(i + 1) % count];
            const std::int32_t da = planeDistance(a, plane);
            const std::int32_t db = planeDistance(b, plane);
            if (da >= 0)
                out[outCount++] = a;
            if ((da >= 0) != (db >= 0))
            {
                const std::int32_t t = static_cast<std::int32_t>((static_cast<std::int64_t>(da) << 16) / (da - db));
                out[outCount++] = lerpVertex(a, b, t);
            }
        }
        std::swap(in, out);
        count = outCount;
    }
    if (in != poly)
        std::copy(in, in + count, poly);
    return count;
}

COLOR colorFrom(float r, float g, float b)
{
    return toRgb565(static_cast<std::uint8_t>(clamp01(r) * 255.0f),
                    static_cast<std::uint8_t>(clamp01(g) * 255.0f),
                    static_cast<std::uint8_t>(clamp01(b) * 255.0f));
}

COLOR color565(int r, int g, int b)
{
    return toRgb565(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
}

bool blendSkipsDraw()
{
    // Depth-equal multi-pass effects (enchantment glint): decoration this
    // renderer cannot do cheaply.
    return g_state.depthTest && g_state.depthFunc == RenderCompare::Equal;
}

// Signed doubled area in window space (y down) from nGL's 8.8 coordinates.
long long screenArea(const VERTEX& a, const VERTEX& b, const VERTEX& c)
{
    const long long x1 = static_cast<long long>(b.x.value) - a.x.value;
    const long long y1 = static_cast<long long>(b.y.value) - a.y.value;
    const long long x2 = static_cast<long long>(c.x.value) - a.x.value;
    const long long y2 = static_cast<long long>(c.y.value) - a.y.value;
    return x1 * y2 - x2 * y1;
}

bool culled(long long area)
{
    if (area == 0)
        return true;
    if (!g_state.cullFace)
        return false;
    // GL's counter-clockwise front face comes out with a negative signed area
    // once y points down.
    const bool front = area < 0;
    return (front && g_state.cullFront) || (!front && g_state.cullBack);
}

#ifndef _TINSPIRE
// Host-only triangle accounting (NSPIRE_SIM_TRISTATS): why submitted triangles die.
unsigned long g_triOutside = 0, g_triClipEmpty = 0, g_triCulled = 0, g_triDrawn = 0, g_triClipped = 0, g_triFaded = 0;
#endif
// What a triangle is drawn with: flat colour, raster state and whether it is
// drawn at all. Terrain quads compute this once for both of their triangles.
struct TriangleShade
{
    bool visible = false;
    bool blendHalf = false;
    bool flat = false;     // untextured, in the tile's average colour
    bool textured = false;
    COLOR fogAdd = 0;
    COLOR modulate = 0xFFFF;
    COLOR vertexColor = 0;
};

// `farW` is the smallest eye depth of the primitive's corners (flat shading
// of far terrain faces); r, g, b, a and fog are its average lit colour and fog.
TriangleShade shadeTriangle(const DrawSetup& setup, int r, int g, int b, int a, int fogVis, std::int32_t farW,
                            const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2)
{
    TriangleShade s;
    if (g_state.blend)
    {
        // A 50% blend is the only blend there is, so faint overlays (vignettes,
        // gradient washes) would come out far too strong -- and cost a
        // full-screen read-modify-write. Below ~30% they are left out.
        if (a < 77)
            return s;
        s.blendHalf = a < 217 ||
                      g_state.blendSrc == RenderBlendFactor::DstColor ||
                      (g_state.blendSrc == RenderBlendFactor::One && g_state.blendDst == RenderBlendFactor::One);
        // A blend into the HUD cache would mix with the key colour, not the world.
        if (s.blendHalf && g_state.colorMask)
            g_hudSawBlend = true;
    }
    if (g_state.alphaTest && a < 26)
        return s;

    // The atlas tile a terrain face samples (-1: unknown).
    int tile = -1;
    if (setup.texture != nullptr && !setup.texture->tileClear.empty())
    {
        int tu = -1, tv = -1;
        if (v0.tileTag != 0)
        {
            tu = static_cast<int>((v0.tileTag >> 4) & 0xF);
            tv = static_cast<int>(v0.tileTag & 0xF);
        }
        else if (g_terrainDraw)
        {
            const int shift = setup.texture->tileShift + 8;
            tu = ((v0.u + v1.u + v2.u) / 3) >> shift;
            tv = ((v0.v + v1.v + v2.v) / 3) >> shift;
        }
        if (tu >= 0 && tu < 16 && tv >= 0 && tv < 16)
            tile = tv * 16 + tu;
    }

    // Far terrain faces in one flat colour: the tile's average texel, lit. At
    // 107x80 a block seven blocks away is a few pixels across, too small for
    // its texture to show, and a flat span skips the texel fetch and the
    // per-pixel colour multiply.
    if (g_terrainDraw && tile >= 0 && farW > kFlatTerrainW && !setup.texture->tileClear[static_cast<std::size_t>(tile)])
    {
        const std::uint32_t avg = setup.texture->tileAverage[static_cast<std::size_t>(tile)];
        r = (r * static_cast<int>((avg >> 16) & 0xFF)) >> 8;
        g = (g * static_cast<int>((avg >> 8) & 0xFF)) >> 8;
        b = (b * static_cast<int>(avg & 0xFF)) >> 8;
        s.flat = true;
    }

    if (setup.fog)
    {
        const int f = 256 - fogVis;
        r = (r * fogVis) >> 8;
        g = (g * fogVis) >> 8;
        b = (b * fogVis) >> 8;
        if (f > 5)
            s.fogAdd = color565(setup.fogColor[0] * f >> 8, setup.fogColor[1] * f >> 8, setup.fogColor[2] * f >> 8);
    }

    s.textured = setup.texture != nullptr && !s.flat;
    if (s.textured)
    {
        if (r < 250 || g < 250 || b < 250)
            s.modulate = color565(r, g, b);
        bool keyed = setup.texture->transparent && (g_state.alphaTest || g_state.blend);
        // A terrain face samples one atlas tile: without transparent texels
        // there, the cheaper unkeyed span loop draws the same pixels.
        if (keyed && tile >= 0 && !setup.texture->tileClear[static_cast<std::size_t>(tile)])
            keyed = false;
        s.vertexColor = keyed ? TEXTURE_TRANSPARENT : 0;
    }
    else
        s.vertexColor = color565(r, g, b);
    s.visible = true;
    return s;
}

// Clips, culls and rasterises one triangle with a shade from shadeTriangle.
void rasterTriangle(const DrawSetup& setup, const TriangleShade& s, const ClipVertex& v0, const ClipVertex& v1,
                    const ClipVertex& v2)
{
    ++g_stats.trianglesSubmitted;
    if ((v0.outcode & v1.outcode & v2.outcode) != 0)
    {
#ifndef _TINSPIRE
        ++g_triOutside;
#endif
        return; // entirely outside one plane
    }

    // The polygon to rasterise, by pointer: the vertices' own projections on
    // the fast path (no copies or initialisation per triangle), the clipper's
    // output otherwise. The vertex scratch is not const storage, so setting the
    // per-triangle colour on a shared corner below is fine.
    static VERTEX s_clipped[9];
    VERTEX* nv[9];
    int count = 3;
    if ((v0.outcode | v1.outcode | v2.outcode) == 0)
    {
        // Fast path: all three corners projected already, no clipping.
        nv[0] = const_cast<VERTEX*>(&v0.screen);
        nv[1] = const_cast<VERTEX*>(&v1.screen);
        nv[2] = const_cast<VERTEX*>(&v2.screen);
    }
    else
    {
        ClipVertex poly[9] = {v0, v1, v2};
        ClipVertex scratch[9];
        count = clipPolygon(poly, 3, scratch, v0.outcode | v1.outcode | v2.outcode);
        if (count < 3)
        {
#ifndef _TINSPIRE
            ++g_triClipEmpty;
#endif
            return;
        }
#ifndef _TINSPIRE
        ++g_triClipped;
#endif
        for (int i = 0; i < count; ++i)
        {
            s_clipped[i] = project(setup, poly[i]);
            nv[i] = &s_clipped[i];
        }
    }

    if (culled(screenArea(*nv[0], *nv[1], *nv[2])))
    {
#ifndef _TINSPIRE
        ++g_triCulled;
#endif
        return;
    }
#ifndef _TINSPIRE
    ++g_triDrawn;
#endif

    ngl_raster.blend = s.blendHalf;
    ngl_raster.fog_add = s.fogAdd;
    ngl_raster.modulate = s.modulate;
    if (v0.tileTag != 0 && s.textured)
    {
        const int tile = setup.texWidth >> 4;
        const int tileU = static_cast<int>((v0.tileTag >> 4) & 0xF), tileV = static_cast<int>(v0.tileTag & 0xF);
        ngl_raster.tex_offset = tileU * tile + tileV * tile * setup.texture->stride;
        ngl_raster.tile_mask = tile - 1;
    }
    else
    {
        ngl_raster.tex_offset = 0;
        ngl_raster.tile_mask = 0;
    }
    ++g_stats.trianglesDrawn;
    for (int i = 0; i < count; ++i)
        nv[i]->c = s.vertexColor;
    if (s.flat)
        glBindTexture(nullptr);
    for (int i = 1; i + 1 < count; ++i)
        nglDrawTriangleZClipped(nv[0], nv[i], nv[i + 1]);
    if (s.flat)
        glBindTexture(&setup.texture->desc);
}

void drawTriangle(const DrawSetup& setup, const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2)
{
    if ((v0.outcode & v1.outcode & v2.outcode) != 0)
    {
        ++g_stats.trianglesSubmitted;
#ifndef _TINSPIRE
        ++g_triOutside;
#endif
        return; // entirely outside one plane
    }
    // Flat colour for the whole triangle, in integers.
    const TriangleShade s = shadeTriangle(setup, (v0.r + v1.r + v2.r) / 3, (v0.g + v1.g + v2.g) / 3,
                                          (v0.b + v1.b + v2.b) / 3, (v0.a + v1.a + v2.a) / 3,
                                          (v0.fog + v1.fog + v2.fog) / 3, std::min(v0.w, std::min(v1.w, v2.w)),
                                          v0, v1, v2);
    if (!s.visible)
    {
        ++g_stats.trianglesSubmitted;
        return;
    }
    rasterTriangle(setup, s, v0, v1, v2);
}

void drawLine(const DrawSetup& setup, const ClipVertex& a, const ClipVertex& b)
{
    if ((a.outcode & b.outcode) != 0)
        return;
    ClipVertex poly[9] = {a, b, b};
    ClipVertex scratch[9];
    // Clip as a degenerate triangle; the first and last survivors are the ends.
    const int count = clipPolygon(poly, 3, scratch, a.outcode | b.outcode);
    if (count < 2)
        return;
    const VERTEX s0 = project(setup, poly[0]);
    const VERTEX s1 = project(setup, poly[count - 1]);
    const COLOR c = color565(a.r, a.g, a.b);
    COLOR* fb = g_target;
    const int x0 = s0.x.toInteger<int>(), y0 = s0.y.toInteger<int>();
    const int x1 = s1.x.toInteger<int>(), y1 = s1.y.toInteger<int>();
    const int z0 = s0.z.toInteger<int>(), z1 = s1.z.toInteger<int>();
    const int dx = x1 - x0, dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy)) + 1;
    for (int i = 0; i <= steps; ++i)
    {
        const int x = x0 + dx * i / steps;
        const int y = y0 + dy * i / steps;
        if (x < 0 || y < 0 || x >= ngl_clip_w || y >= ngl_clip_h)
            continue;
        const int z = z0 + (z1 - z0) * i / steps - 64;
        if (g_state.depthTest && nglZBufferAt(x, y) <= GLFix(z))
            continue;
        fb[y * NspireSystem::kScreenWidth + x] = c;
    }
}

// Translation applied on top of the modelview for the stored mesh being drawn
// (terrain sections), in Q12, so drawing a section never touches the float
// matrix stack.
bool g_meshTranslate = false;
std::int32_t g_meshTranslation[3] = {};

bool prepareDraw(DrawSetup& setup)
{
    if (!g_initialized)
        return false;
    if (blendSkipsDraw())
        return false;
    if (!g_state.colorMask && !g_state.depthMask)
        return false;
    if (g_guiDepthStale && !g_inWorldPass && (g_state.depthTest || g_state.depthMask))
    {
        g_guiDepthStale = false;
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    MatrixCache& cache = g_matrixCache;
    const bool mvChanged = cache.mvSerial != g_matrixSerial[0];
    const bool projChanged = cache.projSerial != g_matrixSerial[1];
    if (mvChanged)
        cache.mv = toFixed(g_state.stacks[0].back());
    if (projChanged)
        cache.proj = toFixed(g_state.stacks[1].back());
    if (mvChanged || projChanged)
    {
        cache.mvp = multiply(cache.proj, cache.mv);
        cache.mvSerial = g_matrixSerial[0];
        cache.projSerial = g_matrixSerial[1];
        if (projChanged)
        {
            const Mat4& proj = g_state.stacks[1].back();
            cache.ortho = proj.m[3] == 0.0f && proj.m[7] == 0.0f && proj.m[11] == 0.0f && proj.m[15] == 1.0f;
            if (cache.ortho)
                for (int i = 0; i < 16; ++i)
                    cache.projHi[i] = NglFixed::fromFloat(proj.m[i], kHiShift);
        }
        if (cache.ortho)
            orthoProductHi(cache.projHi, cache.mv, cache.mvpHi);
    }
    setup.mv = cache.mv;
    setup.mvp = cache.mvp;
    setup.ortho = cache.ortho;
    if (setup.ortho)
        std::copy(cache.mvpHi, cache.mvpHi + 16, setup.mvpHi);
    if (g_meshTranslate)
    {
        translateFixed(setup.mv, g_meshTranslation);
        translateFixed(setup.mvp, g_meshTranslation);
        if (setup.ortho)
            for (int row = 0; row < 4; ++row)
                setup.mvpHi[12 + row] += static_cast<std::int32_t>(
                    (static_cast<std::int64_t>(setup.mvpHi[row]) * g_meshTranslation[0] +
                     static_cast<std::int64_t>(setup.mvpHi[4 + row]) * g_meshTranslation[1] +
                     static_cast<std::int64_t>(setup.mvpHi[8 + row]) * g_meshTranslation[2]) >> NglFixed::kPosShift);
    }

    setup.texture = nullptr;
    if (g_state.texture2d[0])
    {
        const NglTexture* tex = findTexture(g_state.boundTexture[0]);
        if (tex != nullptr && !tex->pixels.empty())
            setup.texture = tex;
    }
    if (setup.texture != nullptr)
    {
        setup.texWidth = setup.texture->width;
        setup.texHeight = setup.texture->height;
    }
    setup.lightmap = nullptr;
    if (g_state.texture2d[1])
    {
        const NglTexture* lm = findTexture(g_state.boundTexture[1]);
        if (lm != nullptr && !lm->rgba.empty())
            setup.lightmap = lm;
    }
    const Mat4& tm = g_state.stacks[2].back();
    setup.textureMatrix = !isIdentity(tm);
    if (setup.textureMatrix)
    {
        setup.texMatrix[0] = NglFixed::fromFloat(tm.m[0], 16);
        setup.texMatrix[1] = NglFixed::fromFloat(tm.m[1], 16);
        setup.texMatrix[2] = NglFixed::fromFloat(tm.m[4], 16);
        setup.texMatrix[3] = NglFixed::fromFloat(tm.m[5], 16);
        setup.texMatrix[4] = NglFixed::fromFloat(tm.m[12], 16);
        setup.texMatrix[5] = NglFixed::fromFloat(tm.m[13], 16);
    }
    for (int i = 0; i < 4; ++i)
        setup.color[i] = toByte(g_state.color[i]);
    setup.lightmapS = NglFixed::fromFloat(g_state.lightmapCoord[0], 0);
    setup.lightmapT = NglFixed::fromFloat(g_state.lightmapCoord[1], 0);
    std::copy(g_state.viewport, g_state.viewport + 4, setup.viewport);
    setup.screenHeight = NspireSystem::kScreenHeight;
    if (g_inWorldPass)
    {
        setup.viewport[0] = setup.viewport[0] * kWorldW / NspireSystem::kScreenWidth;
        setup.viewport[2] = setup.viewport[2] * kWorldW / NspireSystem::kScreenWidth;
        setup.viewport[1] = setup.viewport[1] * kWorldH / NspireSystem::kScreenHeight;
        setup.viewport[3] = setup.viewport[3] * kWorldH / NspireSystem::kScreenHeight;
        setup.screenHeight = kWorldH;
    }
    setup.depthOffset = g_state.polygonOffset ? NglFixed::fromFloat(g_state.polygonOffsetUnits, 8) * 8 : 0;

    setup.fog = g_state.fog;
    if (setup.fog)
    {
        setup.fogMode = g_state.fogMode;
        const std::int32_t start = NglFixed::fromFloat(g_state.fogStart, NglFixed::kPosShift);
        setup.fogEnd = NglFixed::fromFloat(g_state.fogEnd, NglFixed::kPosShift);
        setup.fogSpanValid = setup.fogEnd > start;
        if (setup.fogSpanValid)
            setup.fogInvSpan = NglFixed::reciprocal(setup.fogEnd - start);
        setup.fogDensity = NglFixed::fromFloat(g_state.fogDensity, 16);
        for (int c = 0; c < 3; ++c)
            setup.fogColor[c] = toByte(g_state.fogColor[c]);
    }

    setup.lighting = g_state.lighting;
    if (setup.lighting)
    {
        for (int c = 0; c < 3; ++c)
        {
            std::int32_t a = NglFixed::fromFloat(g_state.lightModelAmbient[c], 8);
            for (int i = 0; i < 2; ++i)
                if (g_state.light[i])
                    a += NglFixed::fromFloat(g_state.lights[i].ambient[c], 8);
            setup.ambient[c] = a;
        }
        for (int i = 0; i < 2; ++i)
        {
            setup.lightOn[i] = g_state.light[i];
            for (int c = 0; c < 3; ++c)
            {
                setup.lightDir[i][c] = NglFixed::fromFloat(g_state.lights[i].dir[c], 14);
                setup.lightDiffuse[i][c] = NglFixed::fromFloat(g_state.lights[i].diffuse[c], 8);
            }
        }
        // GL's current normal, for meshes without their own: lit once per draw.
        const std::int32_t n[3] = {NglFixed::fromFloat(g_state.normal[0], 7), NglFixed::fromFloat(g_state.normal[1], 7),
                                   NglFixed::fromFloat(g_state.normal[2], 7)};
        std::int32_t en[3];
        eyeNormal(setup, n, en);
        lightFactors(setup, en, setup.constantLit);
    }

    glBindTexture(setup.texture ? &setup.texture->desc : nullptr);
    ngl_raster.depth_test = g_state.depthTest && g_state.depthFunc != RenderCompare::Always;
    ngl_raster.depth_bias = (g_state.depthFunc == RenderCompare::LessEqual) ? 1u : 0u;
    ngl_raster.depth_write = g_state.depthMask;
    ngl_raster.color_write = g_state.colorMask;
    return true;
}

void finishDraw()
{
    ngl_raster = NGLRasterState();
}

// Terrain sections (drawStoredMesh): quads of section-local Q12 positions with
// Q16 texture coordinates, a colour and a lightmap brightness; no normals, no
// texture matrix, no lighting. processVertex's 64-bit products become 32-bit
// ones (the matrix in Q14, positions in Q8: a section spans 16 blocks), fog
// reads the eye depth from w, and each quad is drawn as soon as its four
// corners are done, so the vertex scratch never leaves the data cache.
bool drawTerrainQuads(const DrawSetup& setup, const RenderInterleavedMesh& mesh, const std::uint8_t* base)
{
    if (mesh.primitive != RenderPrimitive::Quads || mesh.positionShort || !mesh.hasTexture || mesh.hasNormals ||
        setup.texture == nullptr || setup.textureMatrix || setup.lighting)
        return false;
    const std::int32_t* m = setup.mvp.m;
    std::int32_t q[12];
    for (int i = 0; i < 12; ++i)
    {
        // |q| < 2^16 and |position| < 2^13 keep three products within 31 bits.
        if (m[i] >= (4 << 16) || m[i] <= -(4 << 16))
            return false;
        q[i] = m[i] >> 2;
    }
    const std::int32_t tx = m[12], ty = m[13], tz = m[14], tw = m[15] >> 4;
    // GL's fog distance is the eye-plane depth -z_eye, which a perspective
    // projection leaves in w.
    const FixedMatrix& proj = g_matrixCache.proj;
    if (setup.fog && !(proj.m[3] == 0 && proj.m[7] == 0 && proj.m[11] == -NglFixed::kOne16 && proj.m[15] == 0))
        return false;

    const int tile = setup.texWidth >> 4;
    const int texW = setup.texWidth, texH = setup.texHeight;
    const std::uint8_t* lightmap = nullptr;
    int lightmapWidth = 0;
    if (setup.lightmap != nullptr && setup.lightmap->width >= 16 && setup.lightmap->height >= 16 && !setup.lightmap->rgba.empty())
    {
        lightmap = setup.lightmap->rgba.data();
        lightmapWidth = setup.lightmap->width;
    }

    // Per corner: position, texture coordinates, clip codes.
    const auto vertex = [&](const std::uint8_t* p, ClipVertex& out) {
        const std::int32_t* pos = reinterpret_cast<const std::int32_t*>(p);
        const std::int32_t px = pos[0] >> 4, py = pos[1] >> 4, pz = pos[2] >> 4; // Q8
        out.x = ((q[0] * px + q[4] * py + q[8] * pz) >> 10) + tx;
        out.y = ((q[1] * px + q[5] * py + q[9] * pz) >> 10) + ty;
        out.z = ((q[2] * px + q[6] * py + q[10] * pz) >> 10) + tz;
        out.w = ((q[3] * px + q[7] * py + q[11] * pz) >> 10) + tw;

        const std::int32_t* uv = reinterpret_cast<const std::int32_t*>(p + mesh.texCoordOffset);
        const std::uint32_t tag = *reinterpret_cast<const std::uint32_t*>(p + mesh.normalOffset);
        if (tag & kTileTag)
        {
            out.tileTag = tag;
            out.u = (uv[0] * tile) >> 8;
            out.v = (uv[1] * tile) >> 8;
        }
        else
        {
            out.tileTag = 0;
            out.u = (uv[0] * texW) >> 8;
            out.v = (uv[1] * texH) >> 8;
        }
        out.outcode = computeOutcode(out.x, out.y, out.z, out.w);
    };

    // Per quad: a terrain face has one colour and one light value at all four
    // corners (no smooth lighting), and the triangles are shaded flat anyway.
    const auto litColour = [&](const std::uint8_t* p, int& r, int& g, int& b, int& a) {
        if (mesh.hasColor)
        {
            const std::uint8_t* c = p + mesh.colorOffset;
            r = c[0];
            g = c[1];
            b = c[2];
            a = c[3];
        }
        else
        {
            r = setup.color[0];
            g = setup.color[1];
            b = setup.color[2];
            a = setup.color[3];
        }
        if (lightmap != nullptr)
        {
            int s = setup.lightmapS, t = setup.lightmapT;
            if (mesh.hasBrightness)
            {
                const std::uint32_t bright = *reinterpret_cast<const std::uint32_t*>(p + mesh.brightnessOffset);
                s = static_cast<int>(bright & 0xFFFFu);
                t = static_cast<int>(bright >> 16);
            }
            int block = s >> 4, sky = t >> 4;
            block = block < 0 ? 0 : (block > 15 ? 15 : block);
            sky = sky < 0 ? 0 : (sky > 15 ? 15 : sky);
            const std::uint8_t* lm = lightmap + (sky * lightmapWidth + block) * 4;
            r = (r * (lm[0] + 1)) >> 8;
            g = (g * (lm[1] + 1)) >> 8;
            b = (b * (lm[2] + 1)) >> 8;
        }
    };

    // Linear fog is complete at its end distance; a quad wholly behind it would
    // only paint fog colour over the fog-coloured clear.
    const bool fogCull = setup.fog && setup.fogMode == RenderFogMode::Linear && setup.fogSpanValid;
    const std::int32_t fogFar = setup.fogEnd;
    const int stride = mesh.stride;
    const int n = mesh.count & ~3;
    ClipVertex v[4];
    const auto drawQuad = [&](int i) {
        const std::uint8_t* p = base + static_cast<std::size_t>(i) * stride;
        vertex(p, v[0]);
        vertex(p + stride, v[1]);
        vertex(p + 2 * stride, v[2]);
        vertex(p + 3 * stride, v[3]);
        if ((v[0].outcode & v[1].outcode & v[2].outcode & v[3].outcode) != 0 ||
            (fogCull && v[0].w >= fogFar && v[1].w >= fogFar && v[2].w >= fogFar && v[3].w >= fogFar))
        {
            g_stats.trianglesSubmitted += 2; // outside one plane, or wholly in the fog: not drawn
            return;
        }
        int r, g, b, a;
        litColour(p, r, g, b, a);
        int fog = 256;
        if (setup.fog)
        {
            const std::int32_t w = (v[0].w + v[1].w + v[2].w + v[3].w) >> 2;
            fog = fogVisibility256(setup, w < 0 ? -w : w);
        }
        const std::int32_t nearW = std::min(std::min(v[0].w, v[1].w), std::min(v[2].w, v[3].w));
        const TriangleShade shade = shadeTriangle(setup, r, g, b, a, fog, nearW, v[0], v[1], v[2]);
        if (!shade.visible)
        {
            g_stats.trianglesSubmitted += 2;
            return;
        }
        for (ClipVertex& c : v)
            if (c.outcode == 0)
                c.screen = project(setup, c);
        rasterTriangle(setup, shade, v[0], v[1], v[2]);
        rasterTriangle(setup, shade, v[0], v[2], v[3]);
    };
    if (g_terrainRunCount > 0)
    {
        for (int r = 0; r < g_terrainRunCount; ++r)
        {
            const TerrainRun& run = g_terrainRuns[r];
            if (run.reverse)
                for (int q = run.first + run.count - 1; q >= run.first; --q)
                    drawQuad(q * 4);
            else
                for (int q = run.first; q < run.first + run.count; ++q)
                    drawQuad(q * 4);
        }
    }
    else
        for (int i = 0; i < n; i += 4)
            drawQuad(i);
    return true;
}

bool drawMeshNow(const RenderInterleavedMesh& mesh, bool fixedData = false)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
        return false;
    if (mesh.primitive == RenderPrimitive::Points)
        return true;

    // Render sub-phase accounting for the status log ([phase] prep / vtx / tris):
    // one clock read per stage of a draw call, not per vertex or triangle.
    // Device only: the host simulator's virtual clock advances on every read.
#ifdef _TINSPIRE
    const std::uint64_t tPrep = NspireSystem::micros();
#endif
    DrawSetup setup;
    if (!prepareDraw(setup))
        return true;
#ifdef _TINSPIRE
    const std::uint64_t tVertices = NspireSystem::micros();
#endif
    ++g_stats.draws;

    const std::uint8_t* base = static_cast<const std::uint8_t*>(mesh.data) +
                               static_cast<std::size_t>(mesh.first) * mesh.stride;
    if (g_terrainDraw && fixedData && drawTerrainQuads(setup, mesh, base))
    {
#ifdef _TINSPIRE
        platformProfileTickPhase("prep", static_cast<long long>(tVertices - tPrep) * 1000LL);
        platformProfileTickPhase("terrain", static_cast<long long>(NspireSystem::micros() - tVertices) * 1000LL);
#endif
        finishDraw();
        return true;
    }
    g_litKey = 0; // the lights and the modelview are this draw's
    g_vertexScratch.resize(static_cast<std::size_t>(mesh.count));
    for (int i = 0; i < mesh.count; ++i)
        processVertex(setup, mesh, base + static_cast<std::size_t>(i) * mesh.stride, fixedData, g_vertexScratch[i]);
#ifdef _TINSPIRE
    const std::uint64_t tTriangles = NspireSystem::micros();
#endif

    const ClipVertex* v = g_vertexScratch.data();
    const int n = mesh.count;
#ifndef _TINSPIRE
    // Host simulator: NSPIRE_SIM_TRACE=1 logs every draw with its state.
    static const bool trace = std::getenv("NSPIRE_SIM_TRACE") != nullptr;
    // NSPIRE_SIM_DUMPTEX=<name>: write that texture's RGB565 storage once.
    static const char* dumpTex = std::getenv("NSPIRE_SIM_DUMPTEX");
    if (dumpTex != nullptr && setup.texture != nullptr && std::atoi(dumpTex) == g_state.boundTexture[0])
    {
        const NglTexture* t = setup.texture;
        if (std::FILE* f = std::fopen("texdump.ppm", "wb"))
        {
            std::fprintf(f, "P6\n%d %d\n255\n", t->stride, t->rows);
            for (COLOR c : t->pixels)
            {
                const unsigned char rgb[3] = {static_cast<unsigned char>(((c >> 11) & 31) << 3),
                                              static_cast<unsigned char>(((c >> 5) & 63) << 2),
                                              static_cast<unsigned char>((c & 31) << 3)};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
        dumpTex = nullptr;
    }
    static std::vector<COLOR> before;
    if (trace)
        before.assign(NspireSystem::backBuffer(), NspireSystem::backBuffer() + 320 * 240);
    if (trace)
        NspireSystem::log("[draw] prim=%d n=%d tex=%d(%dx%d) lm=%d depth=%d/%d blend=%d alpha=%d cull=%d "
                          "col=%.2f,%.2f,%.2f,%.2f v0clip=%.2f,%.2f,%.2f,%.2f\n",
                          static_cast<int>(mesh.primitive), n, g_state.texture2d[0] ? g_state.boundTexture[0] : -1,
                          setup.texture ? setup.texture->width : 0, setup.texture ? setup.texture->height : 0,
                          setup.lightmap != nullptr, g_state.depthTest, g_state.depthMask, g_state.blend,
                          g_state.alphaTest, g_state.cullFace, v[0].r / 255.0, v[0].g / 255.0, v[0].b / 255.0, v[0].a / 255.0,
                          v[0].x / 4096.0, v[0].y / 4096.0, v[0].z / 4096.0, v[0].w / 4096.0);
#endif
    switch (mesh.primitive)
    {
    case RenderPrimitive::Triangles:
        for (int i = 0; i + 2 < n; i += 3)
            drawTriangle(setup, v[i], v[i + 1], v[i + 2]);
        break;
    case RenderPrimitive::TriangleStrip:
        for (int i = 0; i + 2 < n; ++i)
        {
            if (i & 1)
                drawTriangle(setup, v[i + 1], v[i], v[i + 2]);
            else
                drawTriangle(setup, v[i], v[i + 1], v[i + 2]);
        }
        break;
    case RenderPrimitive::TriangleFan:
        for (int i = 1; i + 1 < n; ++i)
            drawTriangle(setup, v[0], v[i], v[i + 1]);
        break;
    case RenderPrimitive::Quads:
        for (int i = 0; i + 3 < n; i += 4)
        {
            drawTriangle(setup, v[i], v[i + 1], v[i + 2]);
            drawTriangle(setup, v[i], v[i + 2], v[i + 3]);
        }
        break;
    case RenderPrimitive::Lines:
        for (int i = 0; i + 1 < n; i += 2)
            drawLine(setup, v[i], v[i + 1]);
        break;
    case RenderPrimitive::LineStrip:
        for (int i = 0; i + 1 < n; ++i)
            drawLine(setup, v[i], v[i + 1]);
        break;
    case RenderPrimitive::LineLoop:
        for (int i = 0; i + 1 < n; ++i)
            drawLine(setup, v[i], v[i + 1]);
        if (n > 2)
            drawLine(setup, v[n - 1], v[0]);
        break;
    case RenderPrimitive::Points:
        break;
    }
#ifdef _TINSPIRE
    {
        const std::uint64_t tEnd = NspireSystem::micros();
        platformProfileTickPhase("prep", static_cast<long long>(tVertices - tPrep) * 1000LL);
        platformProfileTickPhase("vtx", static_cast<long long>(tTriangles - tVertices) * 1000LL);
        platformProfileTickPhase("tris", static_cast<long long>(tEnd - tTriangles) * 1000LL);
    }
#endif
    finishDraw();
#ifndef _TINSPIRE
    if (trace)
    {
        int changed = 0;
        for (int i = 0; i < 320 * 240; ++i)
            changed += before[i] != NspireSystem::backBuffer()[i];
        NspireSystem::log("[draw]   -> %d px changed (v0 out=%u scr=%.1f,%.1f,%.1f uv=%.1f,%.1f rgb=%d,%d,%d fog=%d)\n", changed,
                          v[0].outcode, v[0].screen.x.value / 256.0, v[0].screen.y.value / 256.0, v[0].screen.z.value / 256.0,
                          v[0].u / 256.0, v[0].v / 256.0, v[0].r, v[0].g, v[0].b, v[0].fog);
    }
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Mesh store (persistent meshes and terrain chunk handles)
// ---------------------------------------------------------------------------
// A mesh the backend keeps and replays many times (terrain sections, display
// list replacements) is converted once to the pipeline's fixed point:
// positions Q12, texture coordinates Q16 (drawMeshNow's fixedData). Captures
// stay float until here, since the shared terrain code still reads them
// (face sort, plane extents).
void convertToFixed(RenderCapturedMesh& mesh)
{
    for (int v = 0; v < mesh.vertexCount; ++v)
    {
        std::int32_t* d = mesh.raw.data() + static_cast<std::size_t>(v) * 8u;
        float f[5];
        std::memcpy(f, d, sizeof(f));
        if (!mesh.positionShort)
            for (int i = 0; i < 3; ++i)
                d[i] = NglFixed::fromFloat(f[i], NglFixed::kPosShift);
        if (mesh.hasTexture)
        {
            d[3] = NglFixed::fromFloat(f[3], 16);
            d[4] = NglFixed::fromFloat(f[4], 16);
        }
    }
}

struct StoredMesh
{
    RenderCapturedMesh mesh;
    std::int32_t translation[3] = {}; // Q12
    bool translated = false;
    // Terrain sections built with the face-direction sort (WiiMeshSort): quads
    // grouped +X,-X,+Y,-Y,+Z,-Z,other, with each group's plane extent in
    // section-local space and the section's world corner to bring the eye there.
    bool hasGroups = false;
    int groupQuads[RenderTerrainFaceGroups::kGroupCount] = {};
    std::int32_t planeMin[RenderTerrainFaceGroups::kGroupCount] = {}; // Q8
    std::int32_t planeMax[RenderTerrainFaceGroups::kGroupCount] = {};
    std::int32_t origin[3] = {};
};

// Interpolated eye for the terrain pass (renderTerrainSetViewerPosition).
bool g_eyeValid = false;
std::int32_t g_eye[3] = {}; // Q8 world coordinates
constexpr std::int32_t kFaceCullMargin = 128; // half a block, Q8

// Draws the stored mesh, leaving out every face-direction group whose faces all
// point away from the eye. Roughly half the opaque terrain never reaches the
// vertex stage this way.
bool drawStoredMesh(const StoredMesh& stored)
{
    if (stored.mesh.empty())
        return false;

    const std::int32_t eye[3] = {g_eye[0] - stored.origin[0], g_eye[1] - stored.origin[1], g_eye[2] - stored.origin[2]};
    if (stored.hasGroups && g_eyeValid && g_state.fog && g_state.fogMode == RenderFogMode::Linear)
    {
        // A section wholly behind the fog would only draw fog colour over the
        // fog-coloured clear. The fog is planar (eye depth), the test radial,
        // so allow for the frustum's corners.
        const std::int64_t reach = static_cast<std::int64_t>(NglFixed::fromFloat(g_state.fogEnd, 8)) * 5 / 4; // Q8
        const auto gap = [](std::int32_t e) -> std::int64_t {
            constexpr std::int32_t kSection = 16 << 8;
            return e < 0 ? -e : (e > kSection ? e - kSection : 0);
        };
        const std::int64_t gx = gap(eye[0]), gy = gap(eye[1]), gz = gap(eye[2]);
        if (gx * gx + gy * gy + gz * gz > reach * reach)
        {
            g_stats.trianglesSkipped += static_cast<unsigned long>(stored.mesh.vertexCount / 2);
            return true;
        }
    }
    RenderInterleavedMesh view;
    view.data = stored.mesh.raw.data();
    view.stride = stored.mesh.stride;
    view.primitive = stored.mesh.primitive;
    view.positionShort = stored.mesh.positionShort;
    view.hasTexture = stored.mesh.hasTexture;
    view.texCoordOffset = stored.mesh.texCoordOffset;
    view.hasColor = stored.mesh.hasColor;
    view.colorOffset = stored.mesh.colorOffset;
    view.hasNormals = stored.mesh.hasNormals;
    view.normalOffset = stored.mesh.normalOffset;
    view.hasBrightness = stored.mesh.hasBrightness;
    view.brightnessOffset = stored.mesh.brightnessOffset;

#ifndef _TINSPIRE
    static const bool noFaceCull = std::getenv("NSPIRE_SIM_NOFACECULL") != nullptr;
#else
    constexpr bool noFaceCull = false;
#endif
    if (!stored.hasGroups || !g_eyeValid || noFaceCull)
    {
        view.first = 0;
        view.count = stored.mesh.vertexCount;
        return drawMeshNow(view, true);
    }

    struct TerrainScope
    {
        TerrainScope() { g_terrainDraw = true; }
        ~TerrainScope() { g_terrainDraw = false; }
    } terrainScope;
    // One draw for all the axis groups that face the eye, each group walked
    // front to back (sortGroupByPlane): +axis faces are seen from above their
    // planes, so the nearest is the highest plane, and -axis faces the reverse.
    int quad = 0;
    bool drew = false;
    constexpr int kOtherGroup = RenderTerrainFaceGroups::kGroupCount - 1;
    g_terrainRunCount = 0;
    for (int g = 0; g < kOtherGroup; ++g)
    {
        const int count = stored.groupQuads[g];
        if (count <= 0)
            continue;
        const int axis = g >> 1;
        const bool visible = (g & 1) == 0 ? eye[axis] + kFaceCullMargin > stored.planeMin[g]
                                          : eye[axis] - kFaceCullMargin < stored.planeMax[g];
        if (visible)
            g_terrainRuns[g_terrainRunCount++] = {quad, count, (g & 1) == 0};
        else
            g_stats.trianglesSkipped += static_cast<unsigned long>(count) * 2u;
        quad += count;
    }
    if (g_terrainRunCount > 0)
    {
        view.first = 0;
        view.count = quad * 4;
        drew |= drawMeshNow(view, true);
        g_terrainRunCount = 0;
    }
    if (const int count = stored.groupQuads[kOtherGroup])
    {
        // Diagonal faces, i.e. mostly plants: the terrain build gives them one
        // side only (RenderBlocks::nspireOneSidedPlants), so draw both.
        const bool cull = g_state.cullFace;
        g_state.cullFace = false;
        view.first = quad * 4;
        view.count = count * 4;
        drew |= drawMeshNow(view, true);
        g_state.cullFace = cull;
        quad += count;
    }
    return drew || quad == 0;
}

// ---------------------------------------------------------------------------
// Greedy face merging for terrain sections
// ---------------------------------------------------------------------------
// The calculator's cost is per triangle (transform, clip, setup), and terrain
// is mostly flat runs of identical block faces. Within each face-direction
// group, unit faces on the same plane with the same tile, colour and light are
// merged into rectangles of up to kMergeMax x kMergeMax blocks. A merged quad
// carries its tile in the otherwise unused normal word (kTileTag) and texture
// coordinates in tile units; the rasteriser repeats the tile across it.
// Textures interpolate affinely, so large merged quads swim slightly when seen
// at a grazing angle -- accepted for the triangle count.
constexpr int kMergeMax = 8;

struct MergeFace
{
    std::int32_t plane; // Q12
    std::uint32_t colour;
    std::uint32_t light;
    int tileU, tileV;
    std::uint16_t pattern; // per vertex: corner (a, b) and tile corner (u, v), 4 bits each
    int a0, b0;
    int quad;              // index in the source group
    bool operator<(const MergeFace& o) const
    {
        if (plane != o.plane) return plane < o.plane;
        if (colour != o.colour) return colour < o.colour;
        if (light != o.light) return light < o.light;
        if (tileU != o.tileU) return tileU < o.tileU;
        if (tileV != o.tileV) return tileV < o.tileV;
        if (pattern != o.pattern) return pattern < o.pattern;
        if (b0 != o.b0) return b0 < o.b0;
        return a0 < o.a0;
    }
    bool sameSurface(const MergeFace& o) const
    {
        return plane == o.plane && colour == o.colour && light == o.light && tileU == o.tileU && tileV == o.tileV &&
               pattern == o.pattern;
    }
};

inline void setRawFloat(std::int32_t* v, int i, float f)
{
    std::memcpy(v + i, &f, sizeof(f));
}

// The stored mesh is still float here; the classification reads it as fixed
// point (NglFixed::fromFloat takes the bits apart), so none of it goes through
// the soft-float library. Positions Q12, texture coordinates Q16.
inline std::int32_t rawPos(const std::int32_t* v, int i)
{
    float f;
    std::memcpy(&f, v + i, sizeof(f));
    return NglFixed::fromFloat(f, NglFixed::kPosShift);
}

inline std::int32_t rawTex(const std::int32_t* v, int i)
{
    float f;
    std::memcpy(&f, v + i, sizeof(f));
    return NglFixed::fromFloat(f, 16);
}

// Within 0.01 of an integer `target`, in Q`shift`.
inline bool nearInt(std::int32_t v, int target, int shift)
{
    const std::int32_t d = v - (target << shift);
    const std::int32_t tolerance = (1 << shift) / 100;
    return d > -tolerance && d < tolerance;
}

// Classifies one quad of an axis group; false if it is not a plain unit block
// face with one tile, colour and light (slabs, cross plants, liquids, ...).
bool classifyFace(const std::int32_t* q, int axis, MergeFace& f)
{
    constexpr int kP = NglFixed::kPosShift;
    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    f.plane = rawPos(q, axis);
    f.colour = static_cast<std::uint32_t>(q[5]);
    f.light = static_cast<std::uint32_t>(q[7]);
    std::int32_t pa[4], pb[4], tu[4], tv[4];
    std::int32_t amin = 0x7FFFFFFF, bmin = 0x7FFFFFFF, umin = 0x7FFFFFFF, vmin = 0x7FFFFFFF;
    for (int k = 0; k < 4; ++k)
    {
        const std::int32_t* v = q + k * 8;
        if (rawPos(v, axis) != f.plane || static_cast<std::uint32_t>(v[5]) != f.colour ||
            static_cast<std::uint32_t>(v[7]) != f.light || v[6] != 0)
            return false;
        pa[k] = rawPos(v, a);
        pb[k] = rawPos(v, b);
        tu[k] = rawTex(v, 3) * 16; // tile units, Q16
        tv[k] = rawTex(v, 4) * 16;
        amin = std::min(amin, pa[k]);
        bmin = std::min(bmin, pb[k]);
        umin = std::min(umin, tu[k]);
        vmin = std::min(vmin, tv[k]);
    }
    f.a0 = (amin + (1 << (kP - 1))) >> kP;
    f.b0 = (bmin + (1 << (kP - 1))) >> kP;
    if (!nearInt(amin, f.a0, kP) || !nearInt(bmin, f.b0, kP) || f.a0 < 0 || f.a0 > 15 || f.b0 < 0 || f.b0 > 15)
        return false;
    f.tileU = (umin + (1 << 15)) >> 16;
    f.tileV = (vmin + (1 << 15)) >> 16;
    if (f.tileU < 0 || f.tileU > 15 || f.tileV < 0 || f.tileV > 15)
        return false;
    f.pattern = 0;
    unsigned seen = 0;
    for (int k = 0; k < 4; ++k)
    {
        const std::int32_t c[4] = {pa[k] - (f.a0 << kP), pb[k] - (f.b0 << kP), tu[k] - (f.tileU << 16),
                                   tv[k] - (f.tileV << 16)};
        const int shifts[4] = {kP, kP, 16, 16};
        int bits = 0;
        for (int i = 0; i < 4; ++i)
        {
            bits <<= 1;
            if (nearInt(c[i], 1, shifts[i]))
                bits |= 1;
            else if (!nearInt(c[i], 0, shifts[i]))
                return false;
        }
        seen |= 1u << (bits >> 2); // corner (a, b)
        f.pattern = static_cast<std::uint16_t>((f.pattern << 4) | bits);
    }
    return seen == 0xFu; // four distinct corners
}

// Merges the faces of one axis group; appends the result to `out`, returns the quad count.
int mergeGroup(const std::int32_t* src, int quads, int axis, std::vector<std::int32_t>& out)
{
    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    std::vector<MergeFace> faces;
    faces.reserve(static_cast<std::size_t>(quads));
    int emitted = 0;
    for (int i = 0; i < quads; ++i)
    {
        MergeFace f;
        const std::int32_t* q = src + static_cast<std::size_t>(i) * 32;
        if (classifyFace(q, axis, f))
        {
            f.quad = i;
            faces.push_back(f);
        }
        else
        {
            out.insert(out.end(), q, q + 32);
            ++emitted;
        }
    }
    std::sort(faces.begin(), faces.end());

    int grid[16][16];
    for (std::size_t start = 0; start < faces.size();)
    {
        std::size_t end = start + 1;
        while (end < faces.size() && faces[end].sameSurface(faces[start]))
            ++end;
        for (auto& row : grid)
            for (int& cell : row)
                cell = -1;
        for (std::size_t i = start; i < end; ++i)
            grid[faces[i].b0][faces[i].a0] = static_cast<int>(i);

        const MergeFace& surface = faces[start];
        // Which merged extent the tile's u and v run along: compare the tile
        // corners of the (0,0) and (1,0) block corners in the pattern.
        int cornerU[4] = {}, cornerV[4] = {};
        for (int k = 0; k < 4; ++k)
        {
            const int bits = (surface.pattern >> ((3 - k) * 4)) & 0xF;
            cornerU[bits >> 2] = (bits >> 1) & 1;
            cornerV[bits >> 2] = bits & 1;
        }
        // Corner index is (ca << 1) | cb: 0 = (0,0), 2 = (1,0).
        const bool uAlongA = cornerU[0] != cornerU[2];
        const bool vAlongA = cornerV[0] != cornerV[2];

        for (int bb = 0; bb < 16; ++bb)
            for (int aa = 0; aa < 16; ++aa)
            {
                const int first = grid[bb][aa];
                if (first < 0)
                    continue;
                int w = 1;
                while (aa + w < 16 && w < kMergeMax && grid[bb][aa + w] >= 0)
                    ++w;
                int h = 1;
                while (bb + h < 16 && h < kMergeMax)
                {
                    bool full = true;
                    for (int x = 0; x < w && full; ++x)
                        full = grid[bb + h][aa + x] >= 0;
                    if (!full)
                        break;
                    ++h;
                }
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        grid[bb + y][aa + x] = -1;

                const std::int32_t* q = src + static_cast<std::size_t>(faces[first].quad) * 32;
                ++emitted;
                if (w == 1 && h == 1)
                {
                    out.insert(out.end(), q, q + 32);
                    continue;
                }
                const std::uint32_t tag = kTileTag | (static_cast<std::uint32_t>(surface.tileU) << 4) |
                                          static_cast<std::uint32_t>(surface.tileV);
                // Small integers as floats from a table: no int-to-float calls.
                static const float kInt[17] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
                const int extentU = uAlongA ? w : h;
                const int extentV = vAlongA ? w : h;
                for (int k = 0; k < 4; ++k)
                {
                    std::int32_t v[8];
                    std::copy(q + k * 8, q + k * 8 + 8, v);
                    const int bits = (surface.pattern >> ((3 - k) * 4)) & 0xF;
                    const int ca = (bits >> 3) & 1, cb = (bits >> 2) & 1, cu = (bits >> 1) & 1, cv = bits & 1;
                    setRawFloat(v, a, kInt[aa + ca * w]);
                    setRawFloat(v, b, kInt[bb + cb * h]);
                    setRawFloat(v, 3, kInt[cu * extentU]); // tile units
                    setRawFloat(v, 4, kInt[cv * extentV]);
                    v[6] = static_cast<std::int32_t>(tag);
                    out.insert(out.end(), v, v + 8);
                }
            }
        start = end;
    }
    return emitted;
}

// Orders an axis group's quads by their plane, ascending, so drawStoredMesh
// can draw each group front to back from wherever the eye is: up close a face
// covers the whole low-resolution frame, and a nearer face drawn first turns
// the ones behind it into depth-test rejects instead of a second full fill.
void sortGroupByPlane(std::vector<std::int32_t>& quads, std::size_t start, int count, int axis)
{
    std::vector<std::pair<std::int32_t, int>> order(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        order[static_cast<std::size_t>(i)] = {rawPos(&quads[start + static_cast<std::size_t>(i) * 32], axis), i};
    std::stable_sort(order.begin(), order.end(),
                     [](const std::pair<std::int32_t, int>& a, const std::pair<std::int32_t, int>& b) { return a.first < b.first; });
    std::vector<std::int32_t> sorted(static_cast<std::size_t>(count) * 32);
    for (int i = 0; i < count; ++i)
        std::copy_n(&quads[start + static_cast<std::size_t>(order[static_cast<std::size_t>(i)].second) * 32], 32,
                    &sorted[static_cast<std::size_t>(i) * 32]);
    std::copy(sorted.begin(), sorted.end(), quads.begin() + static_cast<std::ptrdiff_t>(start));
}

// Rewrites a grouped terrain mesh (still float) with merged faces.
void mergeStoredFaces(StoredMesh& stored)
{
    RenderCapturedMesh& mesh = stored.mesh;
    std::vector<std::int32_t> out;
    out.reserve(mesh.raw.size());
    int quadStart = 0;
    int total = 0;
    for (int g = 0; g < RenderTerrainFaceGroups::kGroupCount; ++g)
    {
        const int quads = stored.groupQuads[g];
        const std::int32_t* src = mesh.raw.data() + static_cast<std::size_t>(quadStart) * 32;
        int emitted;
        const std::size_t outStart = out.size();
        if (g < 6 && quads > 1)
            emitted = mergeGroup(src, quads, g >> 1, out);
        else
        {
            out.insert(out.end(), src, src + static_cast<std::size_t>(quads) * 32);
            emitted = quads;
        }
        if (g < 6 && emitted > 1)
            sortGroupByPlane(out, outStart, emitted, g >> 1);
        quadStart += quads;
        stored.groupQuads[g] = emitted;
        total += emitted;
    }
    mesh.raw.swap(out);
    mesh.raw.shrink_to_fit();
    mesh.vertexCount = total * 4;
}

std::unordered_map<int, StoredMesh> g_meshes;
int g_nextMeshHandle = 1;
std::size_t g_meshBytes = 0;
} // namespace

// ---------------------------------------------------------------------------
// Fixed-point fog table
// ---------------------------------------------------------------------------
namespace NglFixed
{
namespace
{
constexpr int kExpSteps = 1024;             // over [0, 16), 1/64 per step
std::uint16_t g_expTable[kExpSteps + 1] = {};
}

std::uint32_t g_recipTable[256];

void initRecipTable()
{
    // Entry i covers vn in [(256 + i) * 64, (257 + i) * 64): its midpoint.
    for (int i = 0; i < 256; ++i)
        g_recipTable[i] = static_cast<std::uint32_t>(((1ull << 31) / static_cast<unsigned long long>((256 + i) * 64 + 32) + 1) >> 1);
}

void initExpTable()
{
    for (int i = 0; i <= kExpSteps; ++i)
        g_expTable[i] = static_cast<std::uint16_t>(std::exp(-i / 64.0) * 256.0 + 0.5);
    g_expTable[kExpSteps] = 0;
}

int expNeg256(std::int32_t xQ12)
{
    if (xQ12 <= 0)
        return 256;
    const std::int32_t index = xQ12 >> 6;
    if (index >= kExpSteps)
        return 0;
    // Linear between the two neighbouring steps.
    const int frac = xQ12 & 63;
    return (g_expTable[index] * (64 - frac) + g_expTable[index + 1] * frac) >> 6;
}
} // namespace NglFixed

// ---------------------------------------------------------------------------
// NglBackend
// ---------------------------------------------------------------------------
namespace NglBackend
{
void initialize()
{
    if (g_initialized)
        return;
    nglInit();
    nglSetBuffer(NspireSystem::backBuffer());
    g_target = NspireSystem::backBuffer();
    NglFixed::initExpTable();
    NglFixed::initRecipTable();
    for (auto& stack : g_state.stacks)
        stack.assign(1, identityMatrix());
    ngl_raster = NGLRasterState();
    g_initialized = true;
}

namespace
{
std::vector<COLOR> g_backdrop;
bool g_backdropValid = false;
bool g_holdFrame = false;
}

void present()
{
    endWorldPass(); // a frame that ended inside the world pass
    if (g_holdFrame)
    {
        g_holdFrame = false;
#ifdef _TINSPIRE
        return; // the LCD still shows this frame (the simulator still records it)
#endif
    }
    NspireSystem::present();
}

bool hasBackdrop() { return g_backdropValid; }

void saveBackdrop()
{
    const COLOR* frame = NspireSystem::backBuffer();
    g_backdrop.assign(frame, frame + static_cast<std::size_t>(NspireSystem::kScreenWidth) * NspireSystem::kScreenHeight);
    g_backdropValid = true;
}

bool restoreBackdrop()
{
    if (!g_backdropValid)
        return false;
    std::copy(g_backdrop.begin(), g_backdrop.end(), NspireSystem::backBuffer());
    return true;
}

void dropBackdrop()
{
    if (!g_backdropValid)
        return;
    g_backdropValid = false;
    std::vector<COLOR>().swap(g_backdrop);
}

void holdFrame() { g_holdFrame = true; }

void beginWorldPass()
{
    if (!g_initialized || !g_lowResWorld || g_inWorldPass)
        return;
    if (g_lowResBuffer.empty())
        g_lowResBuffer.assign(static_cast<std::size_t>(NspireSystem::kScreenWidth) * NspireSystem::kScreenHeight, 0);
    g_inWorldPass = true;
    g_target = g_lowResBuffer.data();
    nglSetBuffer(g_target);
    ngl_clip_w = kWorldW;
    ngl_clip_h = kWorldH;
}

void endWorldPass()
{
    if (!g_inWorldPass)
        return;
    g_inWorldPass = false;
    // 3x3 pixel replication of the top-left kWorldW x kWorldH into the frame.
    constexpr int kW = NspireSystem::kScreenWidth;
    const COLOR* src = g_lowResBuffer.data();
    COLOR* dst = NspireSystem::backBuffer();
    static_assert(kWorldScale == 3 && kWorldW * 3 == kW + 1, "the row loop below writes 106 source pixels in pairs plus one");
    for (int y = 0; y < kWorldH; ++y)
    {
        const COLOR* row = src + y * kW;
        COLOR* out = dst + kWorldScale * y * kW;
        // Two source pixels are six output pixels: three 32-bit stores.
        std::uint32_t* o = reinterpret_cast<std::uint32_t*>(out);
        for (int x = 0; x + 1 < kWorldW - 1; x += 2, o += 3)
        {
            const std::uint32_t c0 = row[x], c1 = row[x + 1];
            o[0] = c0 | c0 << 16;
            o[1] = c0 | c1 << 16;
            o[2] = c1 | c1 << 16;
        }
        const std::uint32_t last = row[kWorldW - 1];
        o[0] = last | last << 16; // the 107th pixel's first two columns end the row
        for (int k = 1; k < kWorldScale; ++k)
            std::memcpy(out + k * kW, out, kW * sizeof(COLOR));
    }
    g_target = dst;
    nglSetBuffer(dst);
    ngl_clip_w = kW;
    ngl_clip_h = NspireSystem::kScreenHeight;
    // The GUI drawn next depth-tests against full-resolution coordinates;
    // cleared by its first depth-tested draw (prepareDraw).
    g_guiDepthStale = true;
}

bool hudBegin(std::uint32_t stateKey, bool cacheable)
{
    if (!g_initialized || g_hudMode != HudMode::Off)
        return true;
    g_hudSawBlend = false;
    if (!cacheable || g_hudLive)
    {
        if (!cacheable)
            g_hudValid = false;
        g_hudMode = HudMode::Live;
        return true;
    }
    constexpr int kMaxAge = 12; // frames: hearts, tooltips and the like animate
    if (g_hudValid && stateKey == g_hudKey && ++g_hudAge < kMaxAge)
        return false; // hudEnd composites the cached HUD
    constexpr std::size_t kPixels = static_cast<std::size_t>(NspireSystem::kScreenWidth) * NspireSystem::kScreenHeight;
    if (g_hudBuffer.size() != kPixels)
        g_hudBuffer.assign(kPixels, kHudKey);
    else
        std::fill(g_hudBuffer.begin(), g_hudBuffer.end(), kHudKey);
    g_hudMode = HudMode::Capture;
    g_hudKey = stateKey;
    g_hudAge = 0;
    g_target = g_hudBuffer.data();
    nglSetBuffer(g_target);
    return true;
}

bool hudEnd()
{
    const HudMode mode = g_hudMode;
    g_hudMode = HudMode::Off;
    if (mode == HudMode::Live)
    {
        // Try the cache again once a frame's HUD drew without blending.
        g_hudLive = g_hudSawBlend;
        return false;
    }
    if (mode == HudMode::Capture)
    {
        g_target = NspireSystem::backBuffer();
        nglSetBuffer(g_target);
        if (g_hudSawBlend)
        {
            // Chat, fading text: the blend needs the world under it. Draw this
            // frame's HUD again, straight into the frame.
            g_hudValid = false;
            g_hudLive = true;
            g_hudMode = HudMode::Live;
            g_hudSawBlend = false;
            return true;
        }
        constexpr int kW = NspireSystem::kScreenWidth;
        g_hudRuns.clear();
        for (int y = 0; y < NspireSystem::kScreenHeight; ++y)
        {
            const COLOR* row = g_hudBuffer.data() + y * kW;
            for (int x = 0; x < kW;)
            {
                if (row[x] == kHudKey)
                {
                    ++x;
                    continue;
                }
                const int start = x;
                while (x < kW && row[x] != kHudKey)
                    ++x;
                g_hudRuns.push_back(static_cast<std::uint32_t>(y * kW + start) |
                                    static_cast<std::uint32_t>(x - start) << 17);
            }
        }
        g_hudValid = true;
    }
    if (!g_hudValid)
        return false;
    COLOR* dst = NspireSystem::backBuffer();
    const COLOR* src = g_hudBuffer.data();
    for (const std::uint32_t run : g_hudRuns)
    {
        const std::uint32_t start = run & 0x1FFFFu;
        std::memcpy(dst + start, src + start, (run >> 17) * sizeof(COLOR));
    }
    return false;
}

void shutdown()
{
    if (!g_initialized)
        return;
    nglUninit();
    g_textures.clear();
    g_meshes.clear();
    g_meshBytes = 0;
    g_initialized = false;
}

int createMesh()
{
    const int handle = g_nextMeshHandle++;
    g_meshes[handle];
    return handle;
}

void destroyMesh(int handle)
{
    auto it = g_meshes.find(handle);
    if (it == g_meshes.end())
        return;
    g_meshBytes -= it->second.mesh.byteSize();
    g_meshes.erase(it);
}

void clearMesh(int handle)
{
    auto it = g_meshes.find(handle);
    if (it == g_meshes.end())
        return;
    g_meshBytes -= it->second.mesh.byteSize();
    it->second.mesh.clear();
    std::vector<std::int32_t>().swap(it->second.mesh.raw);
}

void swapMeshes(int a, int b)
{
    auto ia = g_meshes.find(a);
    auto ib = g_meshes.find(b);
    if (ia == g_meshes.end() || ib == g_meshes.end())
        return;
    std::swap(ia->second, ib->second);
}

bool compileMesh(int handle, const RenderInterleavedMesh& mesh, float tx, float ty, float tz,
                 const RenderTerrainCompileInfo* info)
{
    auto it = g_meshes.find(handle);
    if (it == g_meshes.end())
        return false;
    StoredMesh& stored = it->second;
    g_meshBytes -= stored.mesh.byteSize();
    if (!renderCaptureInterleaved(mesh, stored.mesh, false))
    {
        stored.mesh.clear();
        return false;
    }
    stored.mesh.raw.shrink_to_fit();
    stored.translation[0] = NglFixed::fromFloat(tx, NglFixed::kPosShift);
    stored.translation[1] = NglFixed::fromFloat(ty, NglFixed::kPosShift);
    stored.translation[2] = NglFixed::fromFloat(tz, NglFixed::kPosShift);
    stored.translated = stored.translation[0] != 0 || stored.translation[1] != 0 || stored.translation[2] != 0;
    stored.hasGroups = false;
    if (info != nullptr && info->faceGroups != nullptr && mesh.primitive == RenderPrimitive::Quads && mesh.first == 0)
    {
        int total = 0;
        for (int g = 0; g < RenderTerrainFaceGroups::kGroupCount; ++g)
        {
            stored.groupQuads[g] = info->faceGroups->quadCount[g];
            stored.planeMin[g] = NglFixed::fromFloat(info->faceGroups->planeMin[g], 8);
            stored.planeMax[g] = NglFixed::fromFloat(info->faceGroups->planeMax[g], 8);
            total += stored.groupQuads[g];
        }
        stored.origin[0] = NglFixed::fromFloat(info->worldOriginX, 8);
        stored.origin[1] = NglFixed::fromFloat(info->worldOriginY, 8);
        stored.origin[2] = NglFixed::fromFloat(info->worldOriginZ, 8);
        stored.hasGroups = total * 4 == mesh.count;
    }
    if (stored.hasGroups)
        mergeStoredFaces(stored);
    convertToFixed(stored.mesh);
    g_meshBytes += stored.mesh.byteSize();
    return true;
}

bool drawMesh(int handle)
{
    auto it = g_meshes.find(handle);
    if (it == g_meshes.end() || it->second.mesh.empty())
        return false;
    const StoredMesh& stored = it->second;
    if (stored.translated)
    {
        g_meshTranslate = true;
        std::copy(stored.translation, stored.translation + 3, g_meshTranslation);
        drawStoredMesh(stored);
        g_meshTranslate = false;
        return true;
    }
    return drawStoredMesh(stored);
}

void setViewer(double x, double y, double z)
{
    g_eye[0] = NglFixed::fromFloat(static_cast<float>(x), 8);
    g_eye[1] = NglFixed::fromFloat(static_cast<float>(y), 8);
    g_eye[2] = NglFixed::fromFloat(static_cast<float>(z), 8);
    g_eyeValid = true;
}

std::size_t meshBytes()
{
    return g_meshBytes;
}

std::size_t textureBytes()
{
    std::size_t total = 0;
    for (const auto& entry : g_textures)
        if (entry.second)
            total += entry.second->pixels.size() * sizeof(COLOR) + entry.second->rgba.size();
    return total;
}

Stats takeStats()
{
#ifndef _TINSPIRE
    if (std::getenv("NSPIRE_SIM_TRISTATS"))
        std::fprintf(stderr, "[tris] outside=%lu clipEmpty=%lu backface/zero=%lu clipped=%lu drawn=%lu\n", g_triOutside, g_triClipEmpty,
                     g_triCulled, g_triClipped, g_triDrawn);
    g_triOutside = g_triClipEmpty = g_triCulled = g_triDrawn = g_triClipped = 0;
#endif
    const Stats s = g_stats;
    g_stats = Stats();
    return s;
}
} // namespace NglBackend

// ---------------------------------------------------------------------------
// RenderAPI: state
// ---------------------------------------------------------------------------
namespace
{
void setCapability(RenderCapability capability, bool enabled)
{
    switch (capability)
    {
    case RenderCapability::Texture2D: g_state.texture2d[g_state.activeUnit] = enabled; break;
    case RenderCapability::AlphaTest: g_state.alphaTest = enabled; break;
    case RenderCapability::Blend: g_state.blend = enabled; break;
    case RenderCapability::DepthTest: g_state.depthTest = enabled; break;
    case RenderCapability::CullFace: g_state.cullFace = enabled; break;
    case RenderCapability::Fog: g_state.fog = enabled; break;
    case RenderCapability::Lighting: g_state.lighting = enabled; break;
    case RenderCapability::Light0: g_state.light[0] = enabled; break;
    case RenderCapability::Light1: g_state.light[1] = enabled; break;
    case RenderCapability::PolygonOffsetFill: g_state.polygonOffset = enabled; break;
    case RenderCapability::ColorMaterial:
    case RenderCapability::Normalize:
    case RenderCapability::RescaleNormal:
        break;
    }
}
} // namespace

void renderEnable(RenderCapability capability) { setCapability(capability, true); }
void renderDisable(RenderCapability capability) { setCapability(capability, false); }

void renderBlendFunc(RenderBlendFactor source, RenderBlendFactor destination)
{
    g_state.blendSrc = source;
    g_state.blendDst = destination;
}

void renderDepthMask(bool enabled) { g_state.depthMask = enabled; }
void renderDepthFunc(RenderCompare function) { g_state.depthFunc = function; }
void renderAlphaFunc(RenderCompare, float) {}

void renderCullFace(RenderFace face)
{
    g_state.cullBack = face == RenderFace::Back || face == RenderFace::FrontAndBack;
    g_state.cullFront = face == RenderFace::Front || face == RenderFace::FrontAndBack;
}

void renderColorMask(bool red, bool green, bool blue, bool)
{
    g_state.colorMask = red || green || blue;
}

void renderBindTexture(int texture) { g_state.boundTexture[g_state.activeUnit] = texture; }

void renderSetActiveTextureUnit(int textureUnit)
{
    g_state.activeUnit = textureUnit == 0x84C1 ? 1 : 0;
}

void renderSetClientActiveTextureUnit(int) {}

void renderSetMultiTextureCoord(int textureUnit, float u, float v)
{
    if (textureUnit == 0x84C1)
    {
        g_state.lightmapCoord[0] = u;
        g_state.lightmapCoord[1] = v;
    }
}

void renderSetLightmapColors(const std::uint32_t* colors, int count)
{
    // Only the PS2 path calls this; the Nspire reads the lightmap texture.
    (void)colors;
    (void)count;
}

void renderColor4f(float r, float g, float b, float a)
{
    g_state.color[0] = r;
    g_state.color[1] = g;
    g_state.color[2] = b;
    g_state.color[3] = a;
}

void renderColor3f(float r, float g, float b) { renderColor4f(r, g, b, 1.0f); }

void renderNormal3f(float x, float y, float z)
{
    g_state.normal[0] = x;
    g_state.normal[1] = y;
    g_state.normal[2] = z;
}

// ---------------------------------------------------------------------------
// RenderAPI: textures
// ---------------------------------------------------------------------------
void renderGenerateTextures(int count, int* textures)
{
    if (count <= 0 || textures == nullptr)
        return;
    for (int i = 0; i < count; ++i)
    {
        while (g_textures.count(g_nextTextureName))
            ++g_nextTextureName;
        textures[i] = g_nextTextureName;
        g_textures[g_nextTextureName].reset(new NglTexture());
        ++g_nextTextureName;
    }
}

void renderDeleteTextures(int count, const int* textures)
{
    if (count <= 0 || textures == nullptr)
        return;
    for (int i = 0; i < count; ++i)
        g_textures.erase(textures[i]);
}

void renderTextureSubImageRgba(int level, int x, int y, int width, int height, const void* pixels)
{
    if (level != 0 || pixels == nullptr)
        return;
    NglTexture* tex = findTexture(g_state.boundTexture[g_state.activeUnit]);
    if (tex == nullptr || tex->pixels.empty())
        return;
    writeTexels(*tex, x, y, width, height, static_cast<const std::uint8_t*>(pixels));
}

void renderTextureImageRgba(int level, int width, int height, const void* pixels)
{
    if (level != 0 || width <= 0 || height <= 0)
        return;
    NglTexture* tex = textureForUpload(g_state.boundTexture[g_state.activeUnit]);
    if (tex->width != width || tex->height != height || tex->pixels.empty())
        allocateTexture(*tex, width, height);
    if (pixels != nullptr)
        writeTexels(*tex, 0, 0, width, height, static_cast<const std::uint8_t*>(pixels));
}

void renderTextureParameters(bool, bool, bool) {}
int renderGetMaxAnisotropy() { return 0; }
int renderGetMaxSamples() { return 0; }

bool renderTextureBeginUpload(int texture, int width, int height, int, bool, bool, bool, bool)
{
    renderBindTexture(texture);
    NglTexture* tex = textureForUpload(texture);
    if (tex->width != width || tex->height != height || tex->pixels.empty())
        allocateTexture(*tex, width, height);
    return true;
}

bool renderTextureIsValid(int texture)
{
    const NglTexture* tex = findTexture(texture);
    return tex != nullptr && !tex->pixels.empty();
}

void renderResetResources() { g_textures.clear(); }

// ---------------------------------------------------------------------------
// RenderAPI: fog, lights, misc state
// ---------------------------------------------------------------------------
void renderFogf(RenderFogParameter parameter, float value)
{
    switch (parameter)
    {
    case RenderFogParameter::Density: g_state.fogDensity = value; break;
    case RenderFogParameter::Start: g_state.fogStart = value; break;
    case RenderFogParameter::End: g_state.fogEnd = value; break;
    default: break;
    }
}

void renderFogi(RenderFogParameter parameter, RenderFogMode value)
{
    if (parameter == RenderFogParameter::Mode && value != RenderFogMode::EyeRadial)
        g_state.fogMode = value;
}

void renderFogColor(const float* values)
{
    if (values == nullptr)
        return;
    g_state.fogColor[0] = values[0];
    g_state.fogColor[1] = values[1];
    g_state.fogColor[2] = values[2];
}

void renderLightfv(int lightIndex, RenderLightParameter parameter, const float* values)
{
    if (values == nullptr || lightIndex < 0 || lightIndex > 1)
        return;
    Light& light = g_state.lights[lightIndex];
    switch (parameter)
    {
    case RenderLightParameter::Ambient:
        std::copy(values, values + 3, light.ambient);
        break;
    case RenderLightParameter::Diffuse:
        std::copy(values, values + 3, light.diffuse);
        break;
    case RenderLightParameter::Specular:
        break;
    case RenderLightParameter::Position:
    {
        // GL stores the position transformed by the modelview current at the
        // time of the call; Minecraft's lights are directional (w = 0).
        const float* m = g_state.stacks[0].back().m;
        float d[3] = {
            m[0] * values[0] + m[4] * values[1] + m[8] * values[2],
            m[1] * values[0] + m[5] * values[1] + m[9] * values[2],
            m[2] * values[0] + m[6] * values[1] + m[10] * values[2],
        };
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 1e-6f)
            for (float& c : d)
                c /= len;
        std::copy(d, d + 3, light.dir);
        break;
    }
    }
}

void renderLightModelAmbient(const float* values)
{
    if (values != nullptr)
        std::copy(values, values + 3, g_state.lightModelAmbient);
}

void renderColorMaterial(RenderFace, RenderColorMaterialMode) {}
void renderShadeModel(RenderShadeModel) {}

void renderClear(unsigned int mask)
{
    if (!g_initialized)
        return;
    int buffers = 0;
    if (mask & RenderClearMask::Color)
        buffers |= GL_COLOR_BUFFER_BIT;
    if (mask & RenderClearMask::Depth)
        buffers |= GL_DEPTH_BUFFER_BIT;
    nglSetColor(colorFrom(g_state.clearColor[0], g_state.clearColor[1], g_state.clearColor[2]));
    glClear(buffers);
}

void renderFinishGpu() {}
void renderSubmitFrame() {}

void renderClearColor(float r, float g, float b, float a)
{
    g_state.clearColor[0] = r;
    g_state.clearColor[1] = g;
    g_state.clearColor[2] = b;
    g_state.clearColor[3] = a;
}

void renderClearDepth(double) {}

void renderPolygonOffset(float, float units)
{
    g_state.polygonOffsetUnits = units;
}

void renderLineWidth(float) {}

void renderViewport(int x, int y, int width, int height)
{
    g_state.viewport[0] = x;
    g_state.viewport[1] = y;
    g_state.viewport[2] = width;
    g_state.viewport[3] = height;
}

void renderGetViewport(int* values)
{
    if (values != nullptr)
        std::copy(g_state.viewport, g_state.viewport + 4, values);
}

void renderGetMatrix(RenderMatrixQuery query, float* values)
{
    if (values == nullptr)
        return;
    const int index = query == RenderMatrixQuery::Projection ? 1 : query == RenderMatrixQuery::Texture ? 2 + g_state.activeUnit : 0;
    const Mat4& m = g_state.stacks[index].back();
    std::copy(m.m, m.m + 16, values);
}

const unsigned char* renderGetString(RenderStringQuery query)
{
    static const unsigned char vendor[] = "nGL";
    static const unsigned char renderer[] = "nGL software (TI-Nspire)";
    static const unsigned char version[] = "1.1";
    static const unsigned char extensions[] = "";
    switch (query)
    {
    case RenderStringQuery::Vendor: return vendor;
    case RenderStringQuery::Renderer: return renderer;
    case RenderStringQuery::Version: return version;
    case RenderStringQuery::Extensions: return extensions;
    }
    return extensions;
}

bool renderSupportsFeature(RenderFeature) { return false; }
unsigned int renderGetError() { return 0; }
void renderFogHint(RenderHintMode) {}

// ---------------------------------------------------------------------------
// RenderAPI: matrices
// ---------------------------------------------------------------------------
void renderMatrixMode(RenderMatrixMode mode)
{
    g_state.matrixMode = mode == RenderMatrixMode::Projection ? 1 : mode == RenderMatrixMode::Texture ? 2 : 0;
}

void renderLoadIdentity()
{
    currentMatrix() = identityMatrix();
    bumpCurrentMatrix();
}

void renderPushMatrix()
{
    std::vector<Mat4>& stack = g_state.stacks[currentStackIndex()];
    stack.push_back(stack.back());
}

void renderPopMatrix()
{
    std::vector<Mat4>& stack = g_state.stacks[currentStackIndex()];
    if (stack.size() > 1)
        stack.pop_back();
    bumpCurrentMatrix();
}

void renderTranslate(float x, float y, float z)
{
    Mat4& m = currentMatrix();
    for (int row = 0; row < 4; ++row)
        m.m[12 + row] += m.m[row] * x + m.m[4 + row] * y + m.m[8 + row] * z;
    bumpCurrentMatrix();
}

namespace
{
// sin over a full turn in 4096 steps, for renderRotate: model parts rotate
// about the axes several times each per entity per frame, and sinf/cosf, the
// axis normalisation and a full 4x4 product are all soft-float on the ARM9.
constexpr int kSinSteps = 4096;
float g_sinTable[kSinSteps];
bool g_sinTableReady = false;

void sinCosDegrees(float degrees, float& s, float& c)
{
    if (!g_sinTableReady)
    {
        for (int i = 0; i < kSinSteps; ++i)
            g_sinTable[i] = static_cast<float>(std::sin(i * (2.0 * 3.14159265358979323846 / kSinSteps)));
        g_sinTableReady = true;
    }
    const int index = static_cast<int>(degrees * (kSinSteps / 360.0f) + (degrees < 0.0f ? -0.5f : 0.5f));
    s = g_sinTable[index & (kSinSteps - 1)];
    c = g_sinTable[(index + kSinSteps / 4) & (kSinSteps - 1)];
}
}

void renderRotate(float angle, float x, float y, float z)
{
    // About one of the axes (nearly every call): only two columns change.
    const int axis = (y == 0.0f && z == 0.0f) ? 0 : (x == 0.0f && z == 0.0f) ? 1 : (x == 0.0f && y == 0.0f) ? 2 : -1;
    if (axis >= 0 && (x != 0.0f || y != 0.0f || z != 0.0f))
    {
        const float component = axis == 0 ? x : (axis == 1 ? y : z);
        float s, c;
        sinCosDegrees(component < 0.0f ? -angle : angle, s, c);
        // M * R, R the rotation about the axis: columns a, b are the two it mixes.
        const int a = axis == 0 ? 1 : 0;
        const int b = axis == 2 ? 1 : 2;
        // About Y the sign of the mix flips (right-handed: z x x = y).
        const float sab = axis == 1 ? -s : s;
        Mat4& m = currentMatrix();
        for (int row = 0; row < 4; ++row)
        {
            const float ma = m.m[a * 4 + row], mb = m.m[b * 4 + row];
            m.m[a * 4 + row] = c * ma + sab * mb;
            m.m[b * 4 + row] = c * mb - sab * ma;
        }
        bumpCurrentMatrix();
        return;
    }
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f)
        return;
    x /= len;
    y /= len;
    z /= len;
    const float rad = angle * 3.14159265358979f / 180.0f;
    const float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
    Mat4 r = identityMatrix();
    r.m[0] = t * x * x + c;
    r.m[1] = t * x * y + s * z;
    r.m[2] = t * x * z - s * y;
    r.m[4] = t * x * y - s * z;
    r.m[5] = t * y * y + c;
    r.m[6] = t * y * z + s * x;
    r.m[8] = t * x * z + s * y;
    r.m[9] = t * y * z - s * x;
    r.m[10] = t * z * z + c;
    currentMatrix() = multiply(currentMatrix(), r);
    bumpCurrentMatrix();
}

void renderScale(float x, float y, float z)
{
    Mat4& m = currentMatrix();
    for (int row = 0; row < 4; ++row)
    {
        m.m[row] *= x;
        m.m[4 + row] *= y;
        m.m[8 + row] *= z;
    }
    bumpCurrentMatrix();
}

void renderScaleDouble(double x, double y, double z)
{
    renderScale(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void renderFrustum(double left, double right, double bottom, double top, double nearValue, double farValue)
{
    Mat4 f{};
    f.m[0] = static_cast<float>(2.0 * nearValue / (right - left));
    f.m[5] = static_cast<float>(2.0 * nearValue / (top - bottom));
    f.m[8] = static_cast<float>((right + left) / (right - left));
    f.m[9] = static_cast<float>((top + bottom) / (top - bottom));
    f.m[10] = static_cast<float>(-(farValue + nearValue) / (farValue - nearValue));
    f.m[11] = -1.0f;
    f.m[14] = static_cast<float>(-2.0 * farValue * nearValue / (farValue - nearValue));
    currentMatrix() = multiply(currentMatrix(), f);
    bumpCurrentMatrix();
}

void renderOrtho(double left, double right, double bottom, double top, double nearValue, double farValue)
{
    Mat4 o = identityMatrix();
    o.m[0] = static_cast<float>(2.0 / (right - left));
    o.m[5] = static_cast<float>(2.0 / (top - bottom));
    o.m[10] = static_cast<float>(-2.0 / (farValue - nearValue));
    o.m[12] = static_cast<float>(-(right + left) / (right - left));
    o.m[13] = static_cast<float>(-(top + bottom) / (top - bottom));
    o.m[14] = static_cast<float>(-(farValue + nearValue) / (farValue - nearValue));
    currentMatrix() = multiply(currentMatrix(), o);
    bumpCurrentMatrix();
}

void renderSetLegacyPresentationGamma(bool) {}

bool renderCopyFramebufferToBoundTexture(int, int, int, int)
{
    return false;
}

// ---------------------------------------------------------------------------
// RenderAPI: geometry
// ---------------------------------------------------------------------------
bool renderDrawInterleaved(const RenderInterleavedMesh& mesh)
{
    return drawMeshNow(mesh);
}

bool renderCaptureInterleaved(const RenderInterleavedMesh& mesh, RenderCapturedMesh& out, bool append)
{
    if (mesh.data == nullptr || mesh.stride != 32 || mesh.count <= 0)
        return false;
    if (!append)
        out.clear();
    if (!out.empty() && out.stride != 32)
        return false;
    if (out.empty())
    {
        out.stride = 32;
        out.primitive = mesh.primitive;
        out.positionShort = mesh.positionShort;
        out.hasTexture = mesh.hasTexture;
        out.texCoordOffset = 12;
        out.hasColor = mesh.hasColor;
        out.colorOffset = 20;
        out.hasNormals = mesh.hasNormals;
        out.normalOffset = 24;
        out.hasBrightness = mesh.hasBrightness;
        out.brightnessOffset = 28;
    }
    else
    {
        out.hasTexture = out.hasTexture || mesh.hasTexture;
        out.hasColor = out.hasColor || mesh.hasColor;
        out.hasNormals = out.hasNormals || mesh.hasNormals;
        out.hasBrightness = out.hasBrightness || mesh.hasBrightness;
    }
    const std::int32_t* src = static_cast<const std::int32_t*>(mesh.data) + mesh.first * 8;
    const std::size_t base = out.raw.size();
    out.raw.insert(out.raw.end(), src, src + static_cast<std::size_t>(mesh.count) * 8u);
    for (int v = 0; v < mesh.count; ++v)
    {
        std::int32_t* dst = out.raw.data() + base + static_cast<std::size_t>(v) * 8u;
        if (!mesh.hasTexture) { dst[3] = 0; dst[4] = 0; }
        if (!mesh.hasColor) dst[5] = static_cast<std::int32_t>(0xFFFFFFFFu);
        if (!mesh.hasNormals) dst[6] = 0;
        if (!mesh.hasBrightness) dst[7] = 0;
    }
    out.vertexCount += mesh.count;
    return true;
}

bool renderDrawCaptured(const RenderCapturedMesh& mesh)
{
    if (mesh.empty())
        return false;
    RenderInterleavedMesh view;
    view.data = mesh.raw.data();
    view.stride = mesh.stride;
    view.count = mesh.vertexCount;
    view.primitive = mesh.primitive;
    view.positionShort = mesh.positionShort;
    view.hasTexture = mesh.hasTexture;
    view.texCoordOffset = mesh.texCoordOffset;
    view.hasColor = mesh.hasColor;
    view.colorOffset = mesh.colorOffset;
    view.hasNormals = mesh.hasNormals;
    view.normalOffset = mesh.normalOffset;
    view.hasBrightness = mesh.hasBrightness;
    view.brightnessOffset = mesh.brightnessOffset;
    return drawMeshNow(view);
}

int renderCreatePersistentMesh() { return NglBackend::createMesh(); }
void renderDestroyPersistentMesh(int handle) { NglBackend::destroyMesh(handle); }

bool renderCompilePersistentMesh(int handle, const RenderInterleavedMesh& mesh)
{
    return NglBackend::compileMesh(handle, mesh, 0.0f, 0.0f, 0.0f, nullptr);
}

bool renderDrawPersistentMesh(int handle) { return NglBackend::drawMesh(handle); }

bool renderCompileTerrainMesh(int handle, const RenderInterleavedMesh& mesh, const RenderTerrainCompileInfo& info)
{
    return NglBackend::compileMesh(handle, mesh, info.translateX, info.translateY, info.translateZ, &info);
}
