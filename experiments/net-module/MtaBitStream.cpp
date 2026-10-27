#include "MtaBitStream.h"

namespace experimental_net
{
    void MtaBitStream::WriteVector(float x, float y, float z)
    {
        const double length = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
        if (!std::isfinite(length) || length > std::numeric_limits<float>::max())
            throw std::invalid_argument("invalid vector magnitude");
        Write(static_cast<float>(length));
        WriteNormVector(x, y, z);
    }
    bool MtaBitStream::ReadVector(float& x, float& y, float& z)
    {
        const int start = GetReadOffsetAsBits();
        float     length = 0, a = 0, b = 0, c = 0;
        if (!Read(length) || !std::isfinite(length) || length < 0 || !ReadNormVector(a, b, c) || ((length == 0) != (a == 0 && b == 0 && c == 0)))
        {
            SetReadOffsetAsBits(start);
            return false;
        }
        x = a * length;
        y = b * length;
        z = c * length;
        return true;
    }
    void MtaBitStream::WriteOrthMatrix(float a, float b, float c, float d, float e, float f, float g, float h, float i)
    {
        const std::array<float, 9> matrix{a, b, c, d, e, f, g, h, i};
        for (float value : matrix)
            if (!std::isfinite(value))
                throw std::invalid_argument("nonfinite matrix");
        auto dot = [](float a0, float a1, float a2, float b0, float b1, float b2) { return a0 * b0 + a1 * b1 + a2 * b2; };
        if (std::abs(dot(a, b, c, a, b, c) - 1) > 0.002f || std::abs(dot(d, e, f, d, e, f) - 1) > 0.002f || std::abs(dot(g, h, i, g, h, i) - 1) > 0.002f ||
            std::abs(dot(a, b, c, d, e, f)) > 0.002f || std::abs(dot(a, b, c, g, h, i)) > 0.002f || std::abs(dot(d, e, f, g, h, i)) > 0.002f ||
            std::abs(a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g) - 1) > 0.003f)
            throw std::invalid_argument("matrix is not a proper rotation");
        float       w = 0, x = 0, y = 0, z = 0;
        const float trace = a + e + i;
        if (trace > 0)
        {
            const float s = std::sqrt(trace + 1) * 2;
            w = 0.25f * s;
            x = (h - f) / s;
            y = (c - g) / s;
            z = (d - b) / s;
        }
        else if (a > e && a > i)
        {
            const float s = std::sqrt(1 + a - e - i) * 2;
            w = (h - f) / s;
            x = 0.25f * s;
            y = (b + d) / s;
            z = (c + g) / s;
        }
        else if (e > i)
        {
            const float s = std::sqrt(1 + e - a - i) * 2;
            w = (c - g) / s;
            x = (b + d) / s;
            y = 0.25f * s;
            z = (f + h) / s;
        }
        else
        {
            const float s = std::sqrt(1 + i - a - e) * 2;
            w = (d - b) / s;
            x = (c + g) / s;
            y = (f + h) / s;
            z = 0.25f * s;
        }
        WriteNormQuat(w, x, y, z);
    }
    bool MtaBitStream::ReadOrthMatrix(float& a, float& b, float& c, float& d, float& e, float& f, float& g, float& h, float& i)
    {
        float w = 0, x = 0, y = 0, z = 0;
        if (!ReadNormQuat(w, x, y, z))
            return false;
        a = 1 - 2 * (y * y + z * z);
        b = 2 * (x * y - z * w);
        c = 2 * (x * z + y * w);
        d = 2 * (x * y + z * w);
        e = 1 - 2 * (x * x + z * z);
        f = 2 * (y * z - x * w);
        g = 2 * (x * z - y * w);
        h = 2 * (y * z + x * w);
        i = 1 - 2 * (x * x + y * y);
        return true;
    }
}  // namespace experimental_net
