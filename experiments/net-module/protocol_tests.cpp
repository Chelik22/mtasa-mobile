#include "Protocol.h"
#include <iostream>
using namespace experimental_net;
void expect(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
template <class F>
void invalid(F action)
{
    bool rejected = false;
    try
    {
        action();
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    expect(rejected, "malformed frame accepted");
}
int main()
{
    try
    {
        Frame packet;
        packet.packet = 0x42;
        packet.ordering = 2;
        packet.bits = 5;
        packet.nonce = 0x0102030405060708ull;
        packet.generation = 9;
        packet.sequence = 11;
        packet.payload = {0x15};
        const std::vector<std::uint8_t> golden{'M', 'T', 'A', 'X', 1, 0, 2, 0x42, 3, 2, 0, 0,  5, 0, 0, 0,   8,
                                               7,   6,   5,   4,   3, 2, 1, 9,    0, 0, 0, 11, 0, 0, 0, 0x15};
        auto                            encoded = encode_frame(packet);
        expect(encoded == golden, "frame golden bytes");
        auto decoded = decode_frame(golden.data(), golden.size());
        expect(decoded.packet == 0x42 && decoded.bits == 5 && decoded.nonce == packet.nonce && decoded.payload == packet.payload, "frame decode");
        for (std::size_t size = 0; size < golden.size(); ++size)
            invalid([&] { decode_frame(golden.data(), size); });
        for (auto index : {0, 4, 6, 8, 9, 10, 12, 16, 24, 28, 32})
        {
            auto bad = golden;
            if (index == 8)
                bad[index] = 5;
            else if (index == 9)
                bad[index] = 4;
            else if (index == 12)
                bad[index] = 9;
            else if (index == 16)
                for (int i = 16; i < 24; ++i)
                    bad[i] = 0;
            else if (index == 24)
                for (int i = 24; i < 28; ++i)
                    bad[i] = 0;
            else if (index == 28)
                for (int i = 28; i < 32; ++i)
                    bad[i] = 0;
            else if (index == 32)
                bad[index] = 0x80;
            else
                bad[index] ^= 0xff;
            invalid([&] { decode_frame(bad.data(), bad.size()); });
        }
        auto hello = make_hello({"0123456789ABCDEF0123456789ABCDEF", "1.7.0-0.00000.0"});
        auto hello_bytes = encode_frame(hello);
        auto body = read_hello(decode_frame(hello_bytes.data(), hello_bytes.size()));
        expect(body.identity == "0123456789ABCDEF0123456789ABCDEF" && body.version == "1.7.0-0.00000.0", "hello identity/version");
        invalid([&] { make_hello({"not-a-serial", "version"}); });
        invalid([&] { make_hello({body.identity, std::string(64, 'a')}); });
        hello.nonce = 1;
        invalid([&] { encode_frame(hello); });
        Frame welcome;
        welcome.kind = FrameKind::welcome;
        welcome.nonce = 123;
        welcome.generation = 1;
        auto welcome_bytes = encode_frame(welcome);
        expect(decode_frame(welcome_bytes.data(), welcome_bytes.size()).nonce == 123, "welcome");
        welcome.sequence = 1;
        invalid([&] { encode_frame(welcome); });
        packet.bits = static_cast<std::uint32_t>(BitBuffer::max_bytes * 8);
        packet.payload.assign(BitBuffer::max_bytes, 0);
        auto maximum = encode_frame(packet);
        expect(maximum.size() == BitBuffer::max_bytes + 32, "payload maximum");
        maximum.push_back(0);
        invalid([&] { decode_frame(maximum.data(), maximum.size()); });
        std::cout << "PASS owned frame golden/malformed/session shape/hello bounds; transport and MTA join NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
