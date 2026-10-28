#pragma once

#include "BitBuffer.h"
#include <bit>
#include <cstring>
#include <SharedUtil.IntTypes.h>
#include <SharedUtil.Defines.h>
#include <SharedUtil.Misc.h>
#include <SharedUtil.Math.h>
#include <net/bitstream.h>

namespace experimental_net
{
    // The public MTA interface remains unchanged. This adapter is for the explicitly
    // owned protocol only; it is not a decoder for official RakNet/netc traffic.
    class MtaBitStream final : public NetBitStreamInterface
    {
    public:
        static_assert(std::endian::native == std::endian::little, "MTA raw partial-bit fields require a little-endian host");
        static_assert(sizeof(float) == 4 && sizeof(double) == 8 && std::numeric_limits<float>::is_iec559);
        explicit MtaBitStream(unsigned short version = static_cast<unsigned short>(eBitStreamVersion::Latest)) : buffer_(version) {}
        MtaBitStream(const void* data, std::size_t bytes, unsigned short version) : buffer_(data, bytes, bytes * 8, version) {}
        MtaBitStream(const void* data, std::size_t bytes, std::size_t bits, unsigned short version) : buffer_(data, bytes, bits, version) {}
        ~MtaBitStream() override = default;
        using NetBitStreamInterface::Read;
        using NetBitStreamInterface::ReadBit;
        using NetBitStreamInterface::ReadBits;
        using NetBitStreamInterface::Write;
        using NetBitStreamInterface::WriteBits;

        int  GetReadOffsetAsBits() override { return static_cast<int>(buffer_.offset()); }
        void SetReadOffsetAsBits(int offset) override
        {
            if (offset < 0)
                throw std::out_of_range("negative MTA bit offset");
            buffer_.seek(static_cast<std::size_t>(offset));
        }
        void Reset() override { buffer_.clear(); }
        void ResetReadPointer() override { buffer_.rewind(); }

#define MTA_SCALAR(Type) \
    void Write(const Type& value) override \
    { \
        buffer_.write_scalar(value); \
    } \
    bool Read(Type& value) override \
    { \
        return buffer_.read_scalar(value); \
    }
        MTA_SCALAR(unsigned char)
        MTA_SCALAR(char)
        MTA_SCALAR(unsigned short)
        MTA_SCALAR(short)
        MTA_SCALAR(unsigned int)
        MTA_SCALAR(int)
        MTA_SCALAR(float)
        MTA_SCALAR(double)
#undef MTA_SCALAR
        void Write(const char* source, int bytes) override
        {
            if (bytes < 0)
                throw std::invalid_argument("negative MTA write length");
            buffer_.write_bits(source, static_cast<std::size_t>(bytes) * 8);
        }
        bool Read(char* destination, int bytes) override
        {
            return bytes >= 0 && buffer_.read_bits(destination, static_cast<std::size_t>(bytes) * 8);
        }
        void Write(const ISyncStructure* value) override
        {
            if (!value)
                throw std::invalid_argument("null sync structure");
            value->Write(*this);
        }
        bool Read(ISyncStructure* value) override
        {
            return value && value->Read(*this);
        }

#define MTA_COMPRESSED_UNSIGNED(Type) \
    void WriteCompressed(const Type& value) override \
    { \
        buffer_.write_varuint(value); \
    } \
    bool ReadCompressed(Type& value) override \
    { \
        return buffer_.read_varuint(value); \
    }
        MTA_COMPRESSED_UNSIGNED(unsigned char)
        MTA_COMPRESSED_UNSIGNED(unsigned short)
        MTA_COMPRESSED_UNSIGNED(unsigned int)
#undef MTA_COMPRESSED_UNSIGNED
#define MTA_COMPRESSED_SIGNED(Type) \
    void WriteCompressed(const Type& value) override \
    { \
        buffer_.write_varint(value); \
    } \
    bool ReadCompressed(Type& value) override \
    { \
        return buffer_.read_varint(value); \
    }
        MTA_COMPRESSED_SIGNED(short)
        MTA_COMPRESSED_SIGNED(int)
#undef MTA_COMPRESSED_SIGNED
        // Plain char is a byte on this API; its signedness differs between ARM and PC.
        void WriteCompressed(const char& value) override
        {
            buffer_.write_varuint(static_cast<unsigned char>(value));
        }
        bool ReadCompressed(char& value) override
        {
            unsigned char byte = 0;
            if (!buffer_.read_varuint(byte))
                return false;
            std::memcpy(&value, &byte, 1);
            return true;
        }
        void WriteCompressed(const float& value) override
        {
            Write(value);
        }
        void WriteCompressed(const double& value) override
        {
            Write(value);
        }
        bool ReadCompressed(float& value) override
        {
            return Read(value);
        }
        bool ReadCompressed(double& value) override
        {
            return Read(value);
        }
        bool ReadCompressed(unsigned long& value)
        {
            return NetBitStreamInterface::ReadCompressed(value);
        }
        bool ReadCompressed(long& value)
        {
            return NetBitStreamInterface::ReadCompressed(value);
        }
        void WriteCompressed(unsigned long value)
        {
            NetBitStreamInterface::WriteCompressed(value);
        }
        void WriteCompressed(long value)
        {
            NetBitStreamInterface::WriteCompressed(value);
        }

        void WriteBits(const char* value, unsigned int bits) override
        {
            buffer_.write_bits(value, bits);
        }
        bool ReadBits(char* value, unsigned int bits) override
        {
            return buffer_.read_bits(value, bits);
        }
        void WriteBit(bool value) override
        {
            buffer_.write_bit(value);
        }
        bool ReadBit() override
        {
            bool value = false;
            buffer_.read_bit(value);
            return value;
        }
        void WriteNormVector(float x, float y, float z) override
        {
            buffer_.write_unit(std::array<float, 3>{x, y, z});
        }
        bool ReadNormVector(float& x, float& y, float& z) override
        {
            std::array<float, 3> value{};
            if (!buffer_.read_unit(value))
                return false;
            x = value[0];
            y = value[1];
            z = value[2];
            return true;
        }
        void WriteVector(float x, float y, float z) override;
        bool ReadVector(float& x, float& y, float& z) override;
        void WriteNormQuat(float w, float x, float y, float z) override
        {
            buffer_.write_unit(std::array<float, 4>{w, x, y, z});
        }
        bool ReadNormQuat(float& w, float& x, float& y, float& z) override
        {
            std::array<float, 4> value{};
            if (!buffer_.read_unit(value))
                return false;
            w = value[0];
            x = value[1];
            y = value[2];
            z = value[3];
            return true;
        }
        void WriteOrthMatrix(float, float, float, float, float, float, float, float, float) override;
        bool ReadOrthMatrix(float&, float&, float&, float&, float&, float&, float&, float&, float&) override;
        int  GetNumberOfBitsUsed() const override
        {
            return static_cast<int>(buffer_.bits());
        }
        int GetNumberOfBytesUsed() const override
        {
            return static_cast<int>(buffer_.bytes());
        }
        int GetNumberOfUnreadBits() const override
        {
            return static_cast<int>(buffer_.remaining());
        }
        void AlignWriteToByteBoundary() const override
        {
            buffer_.align_write();
        }
        void AlignReadToByteBoundary() const override
        {
            buffer_.align_read();
        }
        unsigned char* GetData() const override
        {
            return buffer_.data();
        }
        unsigned short Version() const override
        {
            return buffer_.version();
        }

    private:
        mutable BitBuffer buffer_;
    };
}  // namespace experimental_net
