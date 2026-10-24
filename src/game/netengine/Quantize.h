#pragma once

#include "Packet.h"

#include <cmath>
#include <cstdint>

// Fixed-point encodings of continuous stream fields (positions, angles).
// Streams carry only what the receiver can tell apart: 1 cm and 2pi/65536
// rad are below anything rendered or simulated, at a fraction of a float's
// size.
namespace net
{
    // World coordinate as a signed 24-bit centimetre count (+-83.88 km).
    // Out of range clamps; NaN encodes as 0.
    static constexpr const int32_t kMaxCentimetres = 0x7FFFFF;

    inline bool WriteCentimetres24(Writer& w, float metres)
    {
        float cm = std::floor(metres * 100.0f + 0.5f);
        int32_t v = !(cm == cm) ? 0
                  : cm > static_cast<float>(kMaxCentimetres) ? kMaxCentimetres
                  : cm < -static_cast<float>(kMaxCentimetres) ? -kMaxCentimetres
                  : static_cast<int32_t>(cm);
        uint32_t u = static_cast<uint32_t>(v) & 0xFFFFFFu;
        return w.WriteU8(static_cast<uint8_t>(u & 0xFF)) && w.WriteU16(static_cast<uint16_t>(u >> 8));
    }

    inline bool ReadCentimetres24(Reader& r, float& metres)
    {
        uint8_t lo = 0;
        uint16_t hi = 0;
        if (!r.ReadU8(lo) || !r.ReadU16(hi)) return false;
        uint32_t u = static_cast<uint32_t>(lo) | static_cast<uint32_t>(hi) << 8;
        int32_t v = (u & 0x800000u) ? static_cast<int32_t>(u | 0xFF000000u) : static_cast<int32_t>(u);
        metres = static_cast<float>(v) / 100.0f;
        return true;
    }

    // Angle (any range, radians) as 2pi/65536 units; decodes into (-pi, pi].
    inline uint16_t EncodeAngle16(float rad)
    {
        const float kTwoPi = 6.28318531f;
        if (!(rad == rad)) return 0;
        float a = std::fmod(rad, kTwoPi);
        if (a < 0.0f) a += kTwoPi;
        return static_cast<uint16_t>(static_cast<uint32_t>(std::floor(a / kTwoPi * 65536.0f + 0.5f)) & 0xFFFFu);
    }

    inline float DecodeAngle16(uint16_t v)
    {
        const float kTwoPi = 6.28318531f;
        const float kPi = 3.14159265f;
        float a = static_cast<float>(v) * (kTwoPi / 65536.0f);
        return a > kPi ? a - kTwoPi : a;
    }
}
