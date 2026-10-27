#include "MtaBitStream.h"
#include <iostream>

using experimental_net::MtaBitStream;
void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
struct PairSync final : ISyncStructure
{
    unsigned int value = 0;
    bool         Read(NetBitStreamInterface& stream) override { return stream.Read(value); }
    void         Write(NetBitStreamInterface& stream) const override { stream.Write(value); }
};
int main()
{
    try
    {
        MtaBitStream           storage;
        NetBitStreamInterface& stream = storage;
        stream.Write(ElementID{12345});
        stream.WriteString(std::string_view("headless"));
        stream.WriteStr(std::string_view("witness"));
        stream.Write(static_cast<long>(-42));
        stream.WriteCompressed(static_cast<unsigned int>(300));
        stream.WriteBit(true);
        PairSync sent;
        sent.value = 0xfedcba98;
        stream.Write(&sent);
        ElementID    id;
        std::string  a, b;
        long         number = 0;
        unsigned int compressed = 0;
        bool         flag = false;
        PairSync     received;
        check(stream.Read(id) && id.Value() == 12345, "MTA element helper");
        check(stream.ReadString(a) && a == "headless" && stream.ReadStr(b) && b == "witness", "MTA string helpers");
        check(stream.Read(number) && number == -42 && stream.ReadCompressed(compressed) && compressed == 300, "MTA primitive helpers");
        check(stream.ReadBit(flag) && flag && stream.Read(&received) && received.value == sent.value, "MTA sync delegation");
        check(!stream.Read(compressed), "MTA EOF");
        stream.Reset();
        stream.WriteVector(0, 3, 4);
        float x = 0, y = 0, z = 0;
        check(stream.ReadVector(x, y, z) && std::abs(x) < 0.001f && std::abs(y - 3) < 0.001f && std::abs(z - 4) < 0.001f, "MTA vector");
        stream.Reset();
        stream.Write(5.0f);
        stream.WriteNormVector(0, 0, 0);
        check(!stream.ReadVector(x, y, z) && stream.GetReadOffsetAsBits() == 0, "nonzero magnitude with zero direction");
        stream.Reset();
        stream.Write(0.0f);
        stream.WriteNormVector(1, 0, 0);
        check(!stream.ReadVector(x, y, z) && stream.GetReadOffsetAsBits() == 0, "zero magnitude with nonzero direction");
        stream.Reset();
        stream.WriteOrthMatrix(0, -1, 0, 1, 0, 0, 0, 0, 1);
        float m[9]{};
        check(stream.ReadOrthMatrix(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]), "MTA matrix read");
        const float expected[9]{0, -1, 0, 1, 0, 0, 0, 0, 1};
        for (int i = 0; i < 9; ++i)
            check(std::abs(m[i] - expected[i]) < 0.0001f, "MTA matrix orientation");
        auto* ref = new MtaBitStream;
        ref->AddRef();
        check(ref->Release() == 1, "MTA AddRef");
        check(ref->Release() == 0, "MTA Release");
        std::cout << "PASS actual NetBitStreamInterface helpers and reference-count lifetime; cross-DLL/server integration NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
