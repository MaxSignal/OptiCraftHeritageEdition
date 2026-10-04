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
}

// ---------------------------------------------------------------------------
// Vertex processing
// ---------------------------------------------------------------------------
struct ClipVertex
{
    float x, y, z, w;   // clip space
    float u, v;         // texel units
    float fogZ;         // eye-space depth, for fog
    int r, g, b, a;     // lit colour, 0..255
    int fog;            // fog visibility 0..256 (256 = clear)
    unsigned outcode;   // clip planes this vertex is outside of (bit per plane)
    VERTEX screen;      // projected nGL vertex, valid when outcode == 0
};

struct DrawSetup
{
    Mat4 modelView;
    Mat4 mvp;
    const NglTexture* texture = nullptr;
    const NglTexture* lightmap = nullptr;
    bool textureMatrix = false;
    Mat4 texMatrix;
    int color[4] = {255, 255, 255, 255}; // current colour, 0..255
    // Lighting in eye space, set up once per draw (entities and items only).
    float lightDir[2][3] = {};
    float lightDiffuse[2][3] = {};
    float ambient[3] = {};
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
    return clamp255(static_cast<int>(v * 255.0f + 0.5f));
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

constexpr float kGuardBand = 2.0f;

unsigned computeOutcode(float x, float y, float z, float w)
{
    unsigned code = 0;
    if (z + w < 0.0f) code |= 1u;                 // near
    if (w - z < 0.0f) code |= 2u;                 // far
    const float gw = kGuardBand * w;
    if (gw + x < 0.0f) code |= 4u;                // left (guard band)
    if (gw - x < 0.0f) code |= 8u;                // right
    if (gw + y < 0.0f) code |= 16u;               // bottom
    if (gw - y < 0.0f) code |= 32u;               // top
    return code;
}

int fogVisibility256(float distance)
{
    float vis;
    switch (g_state.fogMode)
    {
    case RenderFogMode::Linear:
    {
        const float span = g_state.fogEnd - g_state.fogStart;
        vis = span <= 0.0f ? 1.0f : (g_state.fogEnd - distance) / span;
        break;
    }
    case RenderFogMode::Exp2:
    {
        const float d = g_state.fogDensity * distance;
        vis = std::exp(-d * d);
        break;
    }
    default:
        vis = std::exp(-g_state.fogDensity * distance);
        break;
    }
    const int v = static_cast<int>(vis * 256.0f);
    return v < 0 ? 0 : (v > 256 ? 256 : v);
}

VERTEX project(const ClipVertex& c)
{
    const float invW = 1.0f / c.w;
    const int* vp = g_state.viewport;
    const float sx = vp[0] + (c.x * invW * 0.5f + 0.5f) * vp[2];
    const float sy = static_cast<float>(NspireSystem::kScreenHeight) - (vp[1] + (c.y * invW * 0.5f + 0.5f) * vp[3]);
    float depth = clamp01(c.z * invW * 0.5f + 0.5f);
    float zb = 32.0f + depth * 65000.0f;
    if (g_state.polygonOffset)
        zb += g_state.polygonOffsetUnits * 8.0f;
    if (zb < 26.0f)
        zb = 26.0f;
    return VERTEX(GLFix(sx), GLFix(sy), GLFix(zb), GLFix(c.u), GLFix(c.v), 0);
}

void processVertex(const DrawSetup& setup, const RenderInterleavedMesh& mesh, const std::uint8_t* base, ClipVertex& out)
{
    float px, py, pz;
    if (mesh.positionShort)
    {
        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(base);
        px = s[0];
        py = s[1];
        pz = s[2];
    }
    else
    {
        const float* f = reinterpret_cast<const float*>(base);
        px = f[0];
        py = f[1];
        pz = f[2];
    }

    const float* m = setup.mvp.m;
    out.x = m[0] * px + m[4] * py + m[8] * pz + m[12];
    out.y = m[1] * px + m[5] * py + m[9] * pz + m[13];
    out.z = m[2] * px + m[6] * py + m[10] * pz + m[14];
    out.w = m[3] * px + m[7] * py + m[11] * pz + m[15];

    const float* mv = setup.modelView.m;
    if (g_state.fog)
    {
        // Eye-plane distance (GL's default fog coordinate), not the radial
        // distance: three multiplies instead of nine and a square root.
        const float ez = mv[2] * px + mv[6] * py + mv[10] * pz + mv[14];
        out.fogZ = ez < 0.0f ? -ez : ez;
        out.fog = fogVisibility256(out.fogZ);
    }
    else
    {
        out.fogZ = 0.0f;
        out.fog = 256;
    }

    // Texture coordinates, in texels of level 0.
    if (setup.texture != nullptr)
    {
        float s = 0.0f, t = 0.0f;
        if (mesh.hasTexture)
        {
            const float* uv = reinterpret_cast<const float*>(base + mesh.texCoordOffset);
            s = uv[0];
            t = uv[1];
        }
        if (setup.textureMatrix)
        {
            const float* tm = setup.texMatrix.m;
            const float ns = tm[0] * s + tm[4] * t + tm[12];
            const float nt = tm[1] * s + tm[5] * t + tm[13];
            s = ns;
            t = nt;
        }
        out.u = s * setup.texture->width;
        out.v = t * setup.texture->height;
    }
    else
        out.u = out.v = 0.0f;

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
    if (g_state.lighting)
    {
        float n[3] = {g_state.normal[0], g_state.normal[1], g_state.normal[2]};
        if (mesh.hasNormals)
        {
            const std::int8_t* nb = reinterpret_cast<const std::int8_t*>(base + mesh.normalOffset);
            n[0] = nb[0] * (1.0f / 127.0f);
            n[1] = nb[1] * (1.0f / 127.0f);
            n[2] = nb[2] * (1.0f / 127.0f);
        }
        float en[3] = {
            mv[0] * n[0] + mv[4] * n[1] + mv[8] * n[2],
            mv[1] * n[0] + mv[5] * n[1] + mv[9] * n[2],
            mv[2] * n[0] + mv[6] * n[1] + mv[10] * n[2],
        };
        const float len2 = en[0] * en[0] + en[1] * en[1] + en[2] * en[2];
        if (len2 > 1e-12f)
        {
            const float inv = 1.0f / std::sqrt(len2);
            en[0] *= inv;
            en[1] *= inv;
            en[2] *= inv;
        }
        float lit[3] = {setup.ambient[0], setup.ambient[1], setup.ambient[2]};
        for (int i = 0; i < 2; ++i)
        {
            if (!g_state.light[i])
                continue;
            const float d = std::max(0.0f, en[0] * setup.lightDir[i][0] + en[1] * setup.lightDir[i][1] +
                                               en[2] * setup.lightDir[i][2]);
            for (int c = 0; c < 3; ++c)
                lit[c] += setup.lightDiffuse[i][c] * d;
        }
        out.r = (out.r * toByte(lit[0])) / 255;
        out.g = (out.g * toByte(lit[1])) / 255;
        out.b = (out.b * toByte(lit[2])) / 255;
    }

    if (setup.lightmap != nullptr)
    {
        int s = static_cast<int>(g_state.lightmapCoord[0]), t = static_cast<int>(g_state.lightmapCoord[1]);
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
        out.screen = project(out);
}

// ---------------------------------------------------------------------------
// Clipping and rasterisation
// ---------------------------------------------------------------------------
ClipVertex lerpVertex(const ClipVertex& a, const ClipVertex& b, float t)
{
    ClipVertex r = a;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.w = a.w + (b.w - a.w) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    return r;
}

// Signed distance of a vertex to one clip plane; >= 0 is inside.
float planeDistance(const ClipVertex& v, int plane)
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
            const float da = planeDistance(a, plane);
            const float db = planeDistance(b, plane);
            if (da >= 0.0f)
                out[outCount++] = a;
            if ((da >= 0.0f) != (db >= 0.0f))
                out[outCount++] = lerpVertex(a, b, da / (da - db));
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

void drawTriangle(const DrawSetup& setup, const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2)
{
    ++g_stats.trianglesSubmitted;
    if ((v0.outcode & v1.outcode & v2.outcode) != 0)
        return; // entirely outside one plane

    // Flat colour for the whole triangle, in integers.
    int r = (v0.r + v1.r + v2.r) / 3;
    int g = (v0.g + v1.g + v2.g) / 3;
    int b = (v0.b + v1.b + v2.b) / 3;
    const int a = (v0.a + v1.a + v2.a) / 3;

    bool blendHalf = false;
    if (g_state.blend)
    {
        // A 50% blend is the only blend there is, so faint overlays (vignettes,
        // gradient washes) would come out far too strong -- and cost a
        // full-screen read-modify-write. Below ~30% they are left out.
        if (a < 77)
            return;
        blendHalf = a < 217 ||
                    g_state.blendSrc == RenderBlendFactor::DstColor ||
                    (g_state.blendSrc == RenderBlendFactor::One && g_state.blendDst == RenderBlendFactor::One);
    }
    if (g_state.alphaTest && a < 26)
        return;

    COLOR fogAdd = 0;
    if (g_state.fog)
    {
        const int vis = (v0.fog + v1.fog + v2.fog) / 3;
        const int f = 256 - vis;
        r = (r * vis) >> 8;
        g = (g * vis) >> 8;
        b = (b * vis) >> 8;
        if (f > 5)
            fogAdd = color565(toByte(g_state.fogColor[0]) * f >> 8, toByte(g_state.fogColor[1]) * f >> 8,
                              toByte(g_state.fogColor[2]) * f >> 8);
    }

    const bool textured = setup.texture != nullptr;
    COLOR vertexColor;
    COLOR modulate = 0xFFFF;
    if (textured)
    {
        if (r < 250 || g < 250 || b < 250)
            modulate = color565(r, g, b);
        const bool keyed = setup.texture->transparent && (g_state.alphaTest || g_state.blend);
        vertexColor = keyed ? TEXTURE_TRANSPARENT : 0;
    }
    else
        vertexColor = color565(r, g, b);

    VERTEX nv[9];
    int count = 3;
    if ((v0.outcode | v1.outcode | v2.outcode) == 0)
    {
        // Fast path: all three corners projected already, no clipping.
        nv[0] = v0.screen;
        nv[1] = v1.screen;
        nv[2] = v2.screen;
    }
    else
    {
        ClipVertex poly[9] = {v0, v1, v2};
        ClipVertex scratch[9];
        count = clipPolygon(poly, 3, scratch, v0.outcode | v1.outcode | v2.outcode);
        if (count < 3)
            return;
        for (int i = 0; i < count; ++i)
            nv[i] = project(poly[i]);
    }

    if (culled(screenArea(nv[0], nv[1], nv[2])))
        return;

    ngl_raster.blend = blendHalf;
    ngl_raster.fog_add = fogAdd;
    ngl_raster.modulate = modulate;
    ++g_stats.trianglesDrawn;
    for (int i = 0; i < count; ++i)
        nv[i].c = vertexColor;
    for (int i = 1; i + 1 < count; ++i)
        nglDrawTriangleZClipped(&nv[0], &nv[i], &nv[i + 1]);
}

void drawLine(const ClipVertex& a, const ClipVertex& b)
{
    if ((a.outcode & b.outcode) != 0)
        return;
    ClipVertex poly[9] = {a, b, b};
    ClipVertex scratch[9];
    // Clip as a degenerate triangle; the first and last survivors are the ends.
    const int count = clipPolygon(poly, 3, scratch, a.outcode | b.outcode);
    if (count < 2)
        return;
    const VERTEX s0 = project(poly[0]);
    const VERTEX s1 = project(poly[count - 1]);
    const COLOR c = color565(a.r, a.g, a.b);
    COLOR* fb = NspireSystem::backBuffer();
    const int x0 = s0.x.toInteger<int>(), y0 = s0.y.toInteger<int>();
    const int x1 = s1.x.toInteger<int>(), y1 = s1.y.toInteger<int>();
    const int z0 = s0.z.toInteger<int>(), z1 = s1.z.toInteger<int>();
    const int dx = x1 - x0, dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy)) + 1;
    for (int i = 0; i <= steps; ++i)
    {
        const int x = x0 + dx * i / steps;
        const int y = y0 + dy * i / steps;
        if (x < 0 || y < 0 || x >= NspireSystem::kScreenWidth || y >= NspireSystem::kScreenHeight)
            continue;
        const int z = z0 + (z1 - z0) * i / steps - 64;
        if (g_state.depthTest && nglZBufferAt(x, y) <= GLFix(z))
            continue;
        fb[y * NspireSystem::kScreenWidth + x] = c;
    }
}

bool prepareDraw(DrawSetup& setup)
{
    if (!g_initialized)
        return false;
    if (blendSkipsDraw())
        return false;
    if (!g_state.colorMask && !g_state.depthMask)
        return false;

    setup.modelView = g_state.stacks[0].back();
    setup.mvp = multiply(g_state.stacks[1].back(), setup.modelView);
    setup.texture = nullptr;
    if (g_state.texture2d[0])
    {
        const NglTexture* tex = findTexture(g_state.boundTexture[0]);
        if (tex != nullptr && !tex->pixels.empty())
            setup.texture = tex;
    }
    setup.lightmap = nullptr;
    if (g_state.texture2d[1])
    {
        const NglTexture* lm = findTexture(g_state.boundTexture[1]);
        if (lm != nullptr && !lm->rgba.empty())
            setup.lightmap = lm;
    }
    setup.texMatrix = g_state.stacks[2].back();
    setup.textureMatrix = !isIdentity(setup.texMatrix);
    for (int i = 0; i < 4; ++i)
        setup.color[i] = toByte(g_state.color[i]);
    if (g_state.lighting)
    {
        for (int c = 0; c < 3; ++c)
            setup.ambient[c] = g_state.lightModelAmbient[c] + g_state.lights[0].ambient[c] * g_state.light[0] +
                               g_state.lights[1].ambient[c] * g_state.light[1];
        for (int i = 0; i < 2; ++i)
            for (int c = 0; c < 3; ++c)
            {
                setup.lightDir[i][c] = g_state.lights[i].dir[c];
                setup.lightDiffuse[i][c] = g_state.lights[i].diffuse[c];
            }
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

bool drawMeshNow(const RenderInterleavedMesh& mesh)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
        return false;
    if (mesh.primitive == RenderPrimitive::Points)
        return true;

    DrawSetup setup;
    if (!prepareDraw(setup))
        return true;
    ++g_stats.draws;

    const std::uint8_t* base = static_cast<const std::uint8_t*>(mesh.data) +
                               static_cast<std::size_t>(mesh.first) * mesh.stride;
    g_vertexScratch.resize(static_cast<std::size_t>(mesh.count));
    for (int i = 0; i < mesh.count; ++i)
        processVertex(setup, mesh, base + static_cast<std::size_t>(i) * mesh.stride, g_vertexScratch[i]);

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
                          v[0].x, v[0].y, v[0].z, v[0].w);
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
            drawLine(v[i], v[i + 1]);
        break;
    case RenderPrimitive::LineStrip:
        for (int i = 0; i + 1 < n; ++i)
            drawLine(v[i], v[i + 1]);
        break;
    case RenderPrimitive::LineLoop:
        for (int i = 0; i + 1 < n; ++i)
            drawLine(v[i], v[i + 1]);
        if (n > 2)
            drawLine(v[n - 1], v[0]);
        break;
    case RenderPrimitive::Points:
        break;
    }
    finishDraw();
#ifndef _TINSPIRE
    if (trace)
    {
        int changed = 0;
        for (int i = 0; i < 320 * 240; ++i)
            changed += before[i] != NspireSystem::backBuffer()[i];
        NspireSystem::log("[draw]   -> %d px changed\n", changed);
    }
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Mesh store (persistent meshes and terrain chunk handles)
// ---------------------------------------------------------------------------
struct StoredMesh
{
    RenderCapturedMesh mesh;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    bool translated = false;
};

std::unordered_map<int, StoredMesh> g_meshes;
int g_nextMeshHandle = 1;
std::size_t g_meshBytes = 0;
} // namespace

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
    for (auto& stack : g_state.stacks)
        stack.assign(1, identityMatrix());
    ngl_raster = NGLRasterState();
    g_initialized = true;
}

void present()
{
    NspireSystem::present();
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

bool compileMesh(int handle, const RenderInterleavedMesh& mesh, float tx, float ty, float tz)
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
    g_meshBytes += stored.mesh.byteSize();
    stored.tx = tx;
    stored.ty = ty;
    stored.tz = tz;
    stored.translated = tx != 0.0f || ty != 0.0f || tz != 0.0f;
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
        const int savedMode = g_state.matrixMode;
        g_state.matrixMode = 0;
        renderPushMatrix();
        renderTranslate(stored.tx, stored.ty, stored.tz);
        renderDrawCaptured(stored.mesh);
        renderPopMatrix();
        g_state.matrixMode = savedMode;
        return true;
    }
    return renderDrawCaptured(stored.mesh);
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

void renderLoadIdentity() { currentMatrix() = identityMatrix(); }

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
}

void renderTranslate(float x, float y, float z)
{
    Mat4& m = currentMatrix();
    for (int row = 0; row < 4; ++row)
        m.m[12 + row] += m.m[row] * x + m.m[4 + row] * y + m.m[8 + row] * z;
}

void renderRotate(float angle, float x, float y, float z)
{
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
    return NglBackend::compileMesh(handle, mesh, 0.0f, 0.0f, 0.0f);
}

bool renderDrawPersistentMesh(int handle) { return NglBackend::drawMesh(handle); }

bool renderCompileTerrainMesh(int handle, const RenderInterleavedMesh& mesh, const RenderTerrainCompileInfo& info)
{
    return NglBackend::compileMesh(handle, mesh, info.translateX, info.translateY, info.translateZ);
}
