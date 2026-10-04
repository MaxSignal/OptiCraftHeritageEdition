#include "NoiseGeneratorPerlin.h"

#include "MathHelper.h"

#include <cstdint>
#include <cstring>

#ifndef PLATFORM_FIXED_TERRAIN_NOISE
#define PLATFORM_FIXED_TERRAIN_NOISE 0
#endif

namespace
{
int_t floorPerlinCoord(terrain_coord_real_t value)
{
#if PLATFORM_FLOAT_TERRAIN_NOISE
    return MathHelper::floor_float(value);
#else
    return MathHelper::floor_double(value);
#endif
}

#if PLATFORM_FLOAT_TERRAIN_NOISE
// grad()/getBlockMetadata() select between float operands based on bits of a
// pseudo-random hash (the low 4 bits of a permutation-table entry), so the
// branch a `? :` compiles to is essentially unpredictable on real hardware --
// worst case for the EE's simple in-order pipeline, called ~4 times per unique
// noise cell. whenSet/whenClear are returned bit-for-bit unchanged (not
// reconstructed from arithmetic), so this cannot introduce any rounding
// difference versus the branchy form it replaces.
inline float selectBits(std::uint32_t mask, float whenSet, float whenClear)
{
    std::uint32_t a, b;
    std::memcpy(&a, &whenSet, sizeof(a));
    std::memcpy(&b, &whenClear, sizeof(b));
    const std::uint32_t r = (a & mask) | (b & ~mask);
    float result;
    std::memcpy(&result, &r, sizeof(result));
    return result;
}

// 0 or 1 -> 0x00000000 or 0xFFFFFFFF, via two's-complement negation.
inline std::uint32_t maskFromBit(int_t bit)
{
    return static_cast<std::uint32_t>(-static_cast<std::int32_t>(bit));
}
#endif
}

NoiseGeneratorPerlin::NoiseGeneratorPerlin()
{
    Random r;
    *this = NoiseGeneratorPerlin(r);
}

NoiseGeneratorPerlin::NoiseGeneratorPerlin(Random &random)
{
#if PLATFORM_FLOAT_TERRAIN_NOISE
    xCoord = static_cast<perlin_real_t>(random.nextDoubleFloat()) * 256.0f;
    yCoord = static_cast<perlin_real_t>(random.nextDoubleFloat()) * 256.0f;
    zCoord = static_cast<perlin_real_t>(random.nextDoubleFloat()) * 256.0f;
#else
    xCoord = random.nextDouble() * 256.0;
    yCoord = random.nextDouble() * 256.0;
    zCoord = random.nextDouble() * 256.0;
#endif
    permutations.fill(0);
    for (int_t i = 0; i < 256; i++)
        permutations[i] = i;
    for (int_t j = 0; j < 256; j++)
    {
        int_t k = random.nextInt(256 - j) + j;
        int_t l = permutations[j];
        permutations[j] = permutations[k];
        permutations[k] = l;
        permutations[j + 256] = permutations[j];
    }
}

perlin_real_t NoiseGeneratorPerlin::lerp(perlin_real_t d, perlin_real_t d1, perlin_real_t d2)
{
    return d1 + d * (d2 - d1);
}

perlin_real_t NoiseGeneratorPerlin::getBlockMetadata(int_t i, perlin_real_t d, perlin_real_t d1)
{
#if PLATFORM_FLOAT_TERRAIN_NOISE
    const int_t j = i & 0xf;
    const std::uint32_t b0 = maskFromBit(j & 1);
    const std::uint32_t b1 = maskFromBit((j >> 1) & 1);
    const std::uint32_t b2 = maskFromBit((j >> 2) & 1);
    const std::uint32_t b3 = maskFromBit((j >> 3) & 1);
    const std::uint32_t below4 = ~(b2 | b3);      // j < 4
    const std::uint32_t is12or14 = b3 & b2 & ~b0;  // j == 12 or j == 14

    const float term1 = selectBits(b3, 0.0f, d);
    const float term2 = selectBits(below4, 0.0f, selectBits(is12or14, d, d1));
    const float signedTerm1 = selectBits(b0, -term1, term1);
    const float signedTerm2 = selectBits(b1, -term2, term2);
    return signedTerm1 + signedTerm2;
#else
    int_t j = i & 0xf;
    perlin_real_t d2 = (float)(1 - ((j & 8) >> 3)) * d;
    perlin_real_t d3 = j >= 4 ? (j != 12 && j != 14 ? d1 : d) : 0.0f;
    return ((j & 1) != 0 ? -d2 : d2) + ((j & 2) != 0 ? -d3 : d3);
#endif
}

perlin_real_t NoiseGeneratorPerlin::grad(int_t i, perlin_real_t d, perlin_real_t d1, perlin_real_t d2)
{
#if PLATFORM_FLOAT_TERRAIN_NOISE
    const int_t j = i & 0xf;
    const std::uint32_t b0 = maskFromBit(j & 1);
    const std::uint32_t b1 = maskFromBit((j >> 1) & 1);
    const std::uint32_t b2 = maskFromBit((j >> 2) & 1);
    const std::uint32_t b3 = maskFromBit((j >> 3) & 1);
    const std::uint32_t below4 = ~(b2 | b3);      // j < 4
    const std::uint32_t is12or14 = b3 & b2 & ~b0;  // j == 12 or j == 14

    const float u = selectBits(b3, d1, d);
    const float v = selectBits(below4, d1, selectBits(is12or14, d, d2));
    const float signedU = selectBits(b0, -u, u);
    const float signedV = selectBits(b1, -v, v);
    return signedU + signedV;
#else
    int_t j = i & 0xf;
    perlin_real_t d3 = j >= 8 ? d1 : d;
    perlin_real_t d4 = j >= 4 ? (j != 12 && j != 14 ? d2 : d) : d1;
    return ((j & 1) != 0 ? -d3 : d3) + ((j & 2) != 0 ? -d4 : d4);
#endif
}

perlin_real_t NoiseGeneratorPerlin::getDropItemId(perlin_real_t d, perlin_real_t d1)
{
    return generateNoise(d, d1, (perlin_real_t)0.0f);
}

perlin_real_t NoiseGeneratorPerlin::generateNoise(perlin_real_t d, perlin_real_t d1, perlin_real_t d2)
{
    const terrain_coord_real_t d3 = (terrain_coord_real_t)d + (terrain_coord_real_t)xCoord;
    const terrain_coord_real_t d4 = (terrain_coord_real_t)d1 + (terrain_coord_real_t)yCoord;
    const terrain_coord_real_t d5 = (terrain_coord_real_t)d2 + (terrain_coord_real_t)zCoord;
    int_t i = floorPerlinCoord(d3);
    int_t j = floorPerlinCoord(d4);
    int_t k = floorPerlinCoord(d5);
    int_t l = i & 0xff, i1 = j & 0xff, j1 = k & 0xff;
    perlin_real_t fx = (perlin_real_t)(d3 - i);
    perlin_real_t fy = (perlin_real_t)(d4 - j);
    perlin_real_t fz = (perlin_real_t)(d5 - k);
    perlin_real_t d6 = fx*fx*fx*(fx*(fx*6.0f-15.0f)+10.0f);
    perlin_real_t d7 = fy*fy*fy*(fy*(fy*6.0f-15.0f)+10.0f);
    perlin_real_t d8 = fz*fz*fz*(fz*(fz*6.0f-15.0f)+10.0f);
    int_t k1 = permutations[l] + i1;
    int_t l1 = permutations[k1] + j1;
    int_t i2 = permutations[k1+1] + j1;
    int_t j2 = permutations[l+1] + i1;
    int_t k2 = permutations[j2] + j1;
    int_t l2 = permutations[j2+1] + j1;
    return lerp(d8,
        lerp(d7,
            lerp(d6, grad(permutations[l1],fx,fy,fz), grad(permutations[k2],fx-1.0f,fy,fz)),
            lerp(d6, grad(permutations[i2],fx,fy-1.0f,fz), grad(permutations[l2],fx-1.0f,fy-1.0f,fz))),
        lerp(d7,
            lerp(d6, grad(permutations[l1+1],fx,fy,fz-1.0f), grad(permutations[k2+1],fx-1.0f,fy,fz-1.0f)),
            lerp(d6, grad(permutations[i2+1],fx,fy-1.0f,fz-1.0f), grad(permutations[l2+1],fx-1.0f,fy-1.0f,fz-1.0f))));
}

#if PLATFORM_FIXED_TERRAIN_NOISE
// Integer noise for CPUs without an FPU (TI-Nspire): sample coordinates are
// 64-bit Q16 (an octave's wrapped coordinate stays below 2^24), the fractional
// parts, fade curve, gradients and interpolation 32-bit Q16 with 64-bit
// products. Only the final accumulation into the float buffer is floating
// point. Results differ from the float path in the last bits, so a seed's
// terrain shifts slightly; the shapes are the same.
namespace
{
using fixed64_t = std::int64_t;

fixed64_t toFixed64(float v)
{
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu);
    if (exponent == 0)
        return 0;
    const std::int64_t mantissa = static_cast<std::int64_t>((bits & 0x7FFFFFu) | 0x800000u);
    const int shift = exponent - 127 - 23 + 16;
    std::int64_t magnitude;
    if (shift >= 39)
        magnitude = std::int64_t(1) << 62;
    else if (shift >= 0)
        magnitude = mantissa << shift;
    else if (shift > -24)
        magnitude = mantissa >> -shift;
    else
        magnitude = 0;
    return (bits >> 31) ? -magnitude : magnitude;
}

inline std::int32_t fadeFixed(std::int32_t t)
{
    // t^3 (t (6t - 15) + 10), Q16.
    const std::int64_t inner = ((static_cast<std::int64_t>(t) * (6 * t - 15 * 65536)) >> 16) + 10 * 65536;
    const std::int64_t t3 = (((static_cast<std::int64_t>(t) * t) >> 16) * t) >> 16;
    return static_cast<std::int32_t>((t3 * inner) >> 16);
}

inline std::int32_t lerpFixed(std::int32_t t, std::int32_t a, std::int32_t b)
{
    return a + static_cast<std::int32_t>((static_cast<std::int64_t>(t) * (b - a)) >> 16);
}

inline std::int32_t gradFixed(int_t hash, std::int32_t x, std::int32_t y, std::int32_t z)
{
    const int_t h = hash & 0xf;
    const std::int32_t u = h < 8 ? x : y;
    const std::int32_t v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

// getBlockMetadata(): the y = 0 gradient of the 2D (single-layer) case.
inline std::int32_t grad2Fixed(int_t hash, std::int32_t x, std::int32_t z)
{
    const int_t h = hash & 0xf;
    const std::int32_t u = (h & 8) ? 0 : x;
    const std::int32_t v = h < 4 ? 0 : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
}
#endif

void NoiseGeneratorPerlin::getCanSpawnHere(TerrainNoiseBuffer &ad, perlin_real_t d, perlin_real_t d1, perlin_real_t d2,
                                      int_t i, int_t j, int_t k,
                                      perlin_real_t d3, perlin_real_t d4, perlin_real_t d5, perlin_real_t d6)
{
#if PLATFORM_FIXED_TERRAIN_NOISE
    {
        const int_t* p = permutations.data();
        const fixed64_t originX = toFixed64(static_cast<float>(d)) + toFixed64(static_cast<float>(xCoord));
        const fixed64_t originY = toFixed64(static_cast<float>(d1)) + toFixed64(static_cast<float>(yCoord));
        const fixed64_t originZ = toFixed64(static_cast<float>(d2)) + toFixed64(static_cast<float>(zCoord));
        const fixed64_t stepX = toFixed64(static_cast<float>(d3));
        const fixed64_t stepY = toFixed64(static_cast<float>(d4));
        const fixed64_t stepZ = toFixed64(static_cast<float>(d5));
        // amplitude / 65536 folds the Q16 result back to the buffer's scale.
        const terrain_noise_real_t amp = static_cast<terrain_noise_real_t>((perlin_real_t)1.0f / d6 / 65536.0f);
        constexpr std::int32_t one = 65536;
        int_t index = 0;
        if (j == 1)
        {
            fixed64_t cx = originX;
            for (int_t ix = 0; ix < i; ix++, cx += stepX)
            {
                const int_t xi = static_cast<int_t>(cx >> 16) & 0xff;
                const std::int32_t fx = static_cast<std::int32_t>(cx & 0xFFFF);
                const std::int32_t sx = fadeFixed(fx);
                fixed64_t cz = originZ;
                for (int_t iz = 0; iz < k; iz++, cz += stepZ)
                {
                    const int_t zi = static_cast<int_t>(cz >> 16) & 0xff;
                    const std::int32_t fz = static_cast<std::int32_t>(cz & 0xFFFF);
                    const std::int32_t sz = fadeFixed(fz);
                    const int_t a = p[p[xi]] + zi;
                    const int_t b = p[p[xi + 1]] + zi;
                    const std::int32_t n0 = lerpFixed(sx, grad2Fixed(p[a], fx, fz), gradFixed(p[b], fx - one, 0, fz));
                    const std::int32_t n1 = lerpFixed(sx, gradFixed(p[a + 1], fx, 0, fz - one), gradFixed(p[b + 1], fx - one, 0, fz - one));
                    ad[index++] += static_cast<terrain_noise_real_t>(lerpFixed(sz, n0, n1)) * amp;
                }
            }
            return;
        }
        int_t lastY = -1;
        std::int32_t c00 = 0, c10 = 0, c01 = 0, c11 = 0;
        fixed64_t cx = originX;
        for (int_t ix = 0; ix < i; ix++, cx += stepX)
        {
            const int_t xi = static_cast<int_t>(cx >> 16) & 0xff;
            const std::int32_t fx = static_cast<std::int32_t>(cx & 0xFFFF);
            const std::int32_t sx = fadeFixed(fx);
            fixed64_t cz = originZ;
            for (int_t iz = 0; iz < k; iz++, cz += stepZ)
            {
                const int_t zi = static_cast<int_t>(cz >> 16) & 0xff;
                const std::int32_t fz = static_cast<std::int32_t>(cz & 0xFFFF);
                const std::int32_t sz = fadeFixed(fz);
                fixed64_t cy = originY;
                for (int_t iy = 0; iy < j; iy++, cy += stepY)
                {
                    const int_t yi = static_cast<int_t>(cy >> 16) & 0xff;
                    const std::int32_t fy = static_cast<std::int32_t>(cy & 0xFFFF);
                    const std::int32_t sy = fadeFixed(fy);
                    if (iy == 0 || yi != lastY)
                    {
                        lastY = yi;
                        const int_t a = p[xi] + yi, aa = p[a] + zi, ab = p[a + 1] + zi;
                        const int_t b = p[xi + 1] + yi, ba = p[b] + zi, bb = p[b + 1] + zi;
                        c00 = lerpFixed(sx, gradFixed(p[aa], fx, fy, fz), gradFixed(p[ba], fx - one, fy, fz));
                        c10 = lerpFixed(sx, gradFixed(p[ab], fx, fy - one, fz), gradFixed(p[bb], fx - one, fy - one, fz));
                        c01 = lerpFixed(sx, gradFixed(p[aa + 1], fx, fy, fz - one), gradFixed(p[ba + 1], fx - one, fy, fz - one));
                        c11 = lerpFixed(sx, gradFixed(p[ab + 1], fx, fy - one, fz - one), gradFixed(p[bb + 1], fx - one, fy - one, fz - one));
                    }
                    const std::int32_t n = lerpFixed(sz, lerpFixed(sy, c00, c10), lerpFixed(sy, c01, c11));
                    ad[index++] += static_cast<terrain_noise_real_t>(n) * amp;
                }
            }
        }
        return;
    }
#endif
    const terrain_coord_real_t baseX = (terrain_coord_real_t)d;
    const terrain_coord_real_t baseY = (terrain_coord_real_t)d1;
    const terrain_coord_real_t baseZ = (terrain_coord_real_t)d2;
    const terrain_coord_real_t scaleX = (terrain_coord_real_t)d3;
    const terrain_coord_real_t scaleY = (terrain_coord_real_t)d4;
    const terrain_coord_real_t scaleZ = (terrain_coord_real_t)d5;
    const terrain_coord_real_t coordX = (terrain_coord_real_t)xCoord;
    const terrain_coord_real_t coordY = (terrain_coord_real_t)yCoord;
    const terrain_coord_real_t coordZ = (terrain_coord_real_t)zCoord;

    if (j == 1)
    {
        int_t j3 = 0;
        perlin_real_t amp = (perlin_real_t)1.0f / d6;
        for (int_t i4 = 0; i4 < i; i4++)
        {
            terrain_coord_real_t d14 = baseX + (terrain_coord_real_t)i4 * scaleX + coordX;
            int_t j4 = floorPerlinCoord(d14);
            int_t k4 = j4 & 0xff;
            perlin_real_t fx = (perlin_real_t)(d14 - j4);
            perlin_real_t d17 = fx*fx*fx*(fx*(fx*6.0f-15.0f)+10.0f);
            for (int_t l4 = 0; l4 < k; l4++)
            {
                terrain_coord_real_t d19 = baseZ + (terrain_coord_real_t)l4 * scaleZ + coordZ;
                int_t j5 = floorPerlinCoord(d19);
                int_t l5 = j5 & 0xff;
                perlin_real_t fz = (perlin_real_t)(d19 - j5);
                perlin_real_t d21 = fz*fz*fz*(fz*(fz*6.0f-15.0f)+10.0f);
                int_t l  = permutations[k4] + 0;
                int_t j1 = permutations[l] + l5;
                int_t k1 = permutations[k4+1] + 0;
                int_t l1 = permutations[k1] + l5;
                perlin_real_t d9 = lerp(d17, getBlockMetadata(permutations[j1],fx,fz), grad(permutations[l1],fx-1.0f,0.0f,fz));
                perlin_real_t d11 = lerp(d17, grad(permutations[j1+1],fx,0.0f,fz-1.0f), grad(permutations[l1+1],fx-1.0f,0.0f,fz-1.0f));
                ad[j3++] += (terrain_noise_real_t)(lerp(d21, d9, d11) * amp);
            }
        }
        return;
    }
    int_t i1 = 0;
    perlin_real_t amp = (perlin_real_t)1.0f / d6;
    int_t i2 = -1;
    perlin_real_t d13=0, d15=0, d16=0, d18=0;
    for (int_t i5 = 0; i5 < i; i5++)
    {
        terrain_coord_real_t d20 = baseX + (terrain_coord_real_t)i5 * scaleX + coordX;
        int_t k5 = floorPerlinCoord(d20);
        int_t i6 = k5 & 0xff;
        perlin_real_t fx = (perlin_real_t)(d20 - k5);
        perlin_real_t d22 = fx*fx*fx*(fx*(fx*6.0f-15.0f)+10.0f);
        for (int_t j6 = 0; j6 < k; j6++)
        {
            terrain_coord_real_t d24 = baseZ + (terrain_coord_real_t)j6 * scaleZ + coordZ;
            int_t k6 = floorPerlinCoord(d24);
            int_t l6 = k6 & 0xff;
            perlin_real_t fz = (perlin_real_t)(d24 - k6);
            perlin_real_t d25 = fz*fz*fz*(fz*(fz*6.0f-15.0f)+10.0f);
            for (int_t i7 = 0; i7 < j; i7++)
            {
                terrain_coord_real_t d26 = baseY + (terrain_coord_real_t)i7 * scaleY + coordY;
                int_t j7 = floorPerlinCoord(d26);
                int_t k7 = j7 & 0xff;
                perlin_real_t fy = (perlin_real_t)(d26 - j7);
                perlin_real_t d27 = fy*fy*fy*(fy*(fy*6.0f-15.0f)+10.0f);
                if (i7 == 0 || k7 != i2)
                {
                    i2 = k7;
                    int_t j2 = permutations[i6]+k7, k2=permutations[j2]+l6, l2=permutations[j2+1]+l6;
                    int_t i3 = permutations[i6+1]+k7, k3=permutations[i3]+l6, l3=permutations[i3+1]+l6;
                    d13 = lerp(d22, grad(permutations[k2],fx,fy,fz), grad(permutations[k3],fx-1.0f,fy,fz));
                    d15 = lerp(d22, grad(permutations[l2],fx,fy-1.0f,fz), grad(permutations[l3],fx-1.0f,fy-1.0f,fz));
                    d16 = lerp(d22, grad(permutations[k2+1],fx,fy,fz-1.0f), grad(permutations[k3+1],fx-1.0f,fy,fz-1.0f));
                    d18 = lerp(d22, grad(permutations[l2+1],fx,fy-1.0f,fz-1.0f), grad(permutations[l3+1],fx-1.0f,fy-1.0f,fz-1.0f));
                }
                ad[i1++] += (terrain_noise_real_t)(lerp(d25, lerp(d27,d13,d15), lerp(d27,d16,d18)) * amp);
            }
        }
    }
}
