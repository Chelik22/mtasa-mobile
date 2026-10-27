#include "wire.hpp"

#include <iostream>

using namespace mobile_net;

void check(bool condition, const char* label)
{
    if (!condition)
        throw std::runtime_error(label);
}

template <class Operation>
void rejects(Operation operation)
{
    try
    {
        operation();
    }
    catch (const std::invalid_argument&)
    {
        return;
    }
    throw std::runtime_error("malformed message accepted");
}

int main()
{
    try
    {
        const Message message{Type::probe, 0x0102030405060708ULL, 0x11223344, 0x55667788, "hi"};
        // Literal bytes are independent of the encoder, including all big-endian fields.
        const std::vector<std::uint8_t> golden{0x4d, 0x4e, 0x45, 0x54, 0x00, 0x01, 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                               0x08, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x00, 0x00, 0x02, 0x68, 0x69};
        check(encode(message) == golden, "golden encode");
        const auto read = decode(golden.data(), golden.size());
        check(read.type == Type::probe && read.session == 0x0102030405060708ULL && read.generation == 0x11223344 && read.sequence == 0x55667788 &&
                  read.payload == "hi",
              "golden decode");
        for (std::size_t size = 0; size < golden.size(); ++size)
            rejects([&] { decode(golden.data(), size); });
        rejects([] { decode(nullptr, header_size); });
        for (const std::size_t field : {std::size_t{0}, std::size_t{5}, std::size_t{7}, std::size_t{27}})
        {
            auto corrupt = golden;
            corrupt[field] = 0xff;
            rejects([&] { decode(corrupt.data(), corrupt.size()); });
        }
        for (const auto range : {std::pair<std::size_t, std::size_t>{8, 16}, {16, 20}, {20, 24}})
        {
            auto corrupt = golden;
            for (auto i = range.first; i < range.second; ++i)
                corrupt[i] = 0;
            rejects([&] { decode(corrupt.data(), corrupt.size()); });
        }
        auto trailing = golden;
        trailing.push_back(0);
        rejects([&] { decode(trailing.data(), trailing.size()); });
        Message    boundary{Type::hello, 1, 1, 1, std::string(max_payload, '\0')};
        const auto full = encode(boundary);
        check(decode(full.data(), full.size()).payload.size() == max_payload, "max payload");
        boundary.payload.push_back('x');
        rejects([&] { encode(boundary); });
        auto oversized = full;
        oversized.push_back(0);
        rejects([&] { decode(oversized.data(), oversized.size()); });
        for (Message invalid : {Message{Type::hello, 0, 1, 1, ""}, Message{Type::hello, 1, 0, 1, ""}, Message{Type::hello, 1, 1, 0, ""},
                                Message{static_cast<Type>(4), 1, 1, 1, ""}})
            rejects([&] { encode(invalid); });
        std::cout << "{\"test\":\"wire_contract\",\"passed\":true,\"golden_vectors\":1,\"truncated_prefixes\":" << golden.size() << "}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
