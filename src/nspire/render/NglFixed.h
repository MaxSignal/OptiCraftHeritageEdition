#pragma once

// Integer helpers for the nGL vertex pipeline (RenderAPI_NGL.cpp). The ARM9 in
// the calculator has no FPU: every float operation is a libgcc call costing
// tens of cycles, while a 32x32->64 multiply (SMULL) takes a handful. The
// per-vertex and per-triangle work therefore runs on fixed-point integers:
//
//   Q12  positions, clip coordinates, eye depth (1/4096 of a block)
//   Q16  matrix rotation/scale terms, normalised texture coordinates, NDC
//   Q8   screen coordinates and texel coordinates (nGL's GLFix)
#include <cstdint>
#include <cstring>

namespace NglFixed
{
constexpr int kPosShift = 12;
constexpr int kMatShift = 16;
constexpr std::int32_t kOne16 = 1 << kMatShift;

// IEEE float to fixed point with `frac` fractional bits, by taking the bits
// apart instead of calling the soft-float converter. Truncates toward zero
// and saturates instead of overflowing.
inline std::int32_t fromFloat(float f, int frac)
{
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu);
    if (exponent == 0)
        return 0; // zero or denormal
    const std::int32_t mantissa = static_cast<std::int32_t>((bits & 0x7FFFFFu) | 0x800000u);
    const int shift = exponent - 127 - 23 + frac;
    std::int32_t magnitude;
    if (exponent == 0xFF || shift > 7)
        magnitude = 0x7FFFFFFF;
    else if (shift >= 0)
        magnitude = mantissa << shift;
    else if (shift > -24)
        magnitude = mantissa >> -shift;
    else
        magnitude = 0;
    return (bits >> 31) ? -magnitude : magnitude;
}

inline float toFloat(std::int32_t v, int frac)
{
    return static_cast<float>(v) / static_cast<float>(1 << frac);
}

// (a * b) >> shift with a 64-bit intermediate.
inline std::int32_t mulShift(std::int32_t a, std::int32_t b, int shift)
{
    return static_cast<std::int32_t>((static_cast<std::int64_t>(a) * b) >> shift);
}

// Reciprocal of a positive value as (r, k) with 1/v ~= r / 2^k: v is brought
// to 15 significant bits first, so the 32-bit division keeps ~15 bits of
// precision whatever v's magnitude.
struct Reciprocal
{
    std::uint32_t r;
    int k;
};

// 2^30 / v for v's top 8 bits (initRecipTable), refined by one Newton step:
// the ARM9 has no divide instruction, and this runs once per projected vertex.
extern std::uint32_t g_recipTable[256];
void initRecipTable();

inline Reciprocal reciprocal(std::int32_t v)
{
    if (v <= 0)
        v = 1;
    const int msb = 31 - __builtin_clz(static_cast<std::uint32_t>(v));
    const int sh = msb - 14; // move the top bit to bit 14
    const std::uint32_t vn = sh >= 0 ? static_cast<std::uint32_t>(v) >> sh : static_cast<std::uint32_t>(v) << -sh;
    // r0 is within 2^-9 of 2^30 / vn; r0 + r0 * (2^30 - vn * r0) / 2^30 within
    // one unit, and the last step makes it exactly the division's floor: at
    // w = 1.0 (every 2D draw) one unit short put GUI vertices a hair left of
    // their pixel, which changed glyph span widths and garbled the text.
    const std::uint32_t r0 = g_recipTable[(vn >> 6) - 256];
    const std::int32_t e = static_cast<std::int32_t>((1u << 30) - vn * r0);
    std::uint32_t r = r0 + static_cast<std::uint32_t>((static_cast<std::int32_t>(r0) * (e >> 7)) >> 23);
    if (vn * (r + 1) <= (1u << 30))
        ++r;
    else if (vn * r > (1u << 30))
        --r;
    return {r, 30 + sh};
}

// x * (1/v) with the result in Q`frac`, given 1/v as a Reciprocal.
inline std::int32_t mulReciprocal(std::int32_t x, const Reciprocal& rc, int frac)
{
    const int shift = rc.k - frac;
    const std::int64_t p = static_cast<std::int64_t>(x) * rc.r;
    if (shift >= 0)
        return static_cast<std::int32_t>(shift >= 63 ? (p < 0 ? -1 : 0) : (p >> shift));
    return static_cast<std::int32_t>(p << -shift);
}

inline std::uint32_t isqrt(std::uint64_t v)
{
    std::uint64_t result = 0;
    std::uint64_t bit = std::uint64_t(1) << 62;
    while (bit > v)
        bit >>= 2;
    while (bit != 0)
    {
        if (v >= result + bit)
        {
            v -= result + bit;
            result = (result >> 1) + bit;
        }
        else
            result >>= 1;
        bit >>= 2;
    }
    return static_cast<std::uint32_t>(result);
}

// exp(-x) for x in Q12, as 0..256 (fog visibility). Table over [0, 16).
void initExpTable();
int expNeg256(std::int32_t xQ12);
} // namespace NglFixed
