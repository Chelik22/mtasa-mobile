#pragma once
#include "ClientModule.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace experimental_client
{
    // Resource fileRead needs the actual SDK file interface. Retain the exact
    // bytes accepted by an explicit policy instead of reopening a mutable path
    // after validation. This lab reader grants no write/cache-trust capability.
    class ApprovedReadFile final : public CBinaryFileInterface
    {
    public:
        using Validator = std::function<bool(const std::filesystem::path&, std::span<const unsigned char>)>;
        explicit ApprovedReadFile(Validator validator, std::size_t max_bytes = 64 * 1024 * 1024) : validator_(std::move(validator)), max_bytes_(max_bytes) {}
        bool FOpen(const char* filename, const char* mode, bool validate) override
        {
            FClose();
            if (!filename || !mode || std::strcmp(mode, "rb") || !validate || !validator_ || max_bytes_ > static_cast<std::size_t>(INT_MAX))
                return false;
            try
            {
                const auto path = std::filesystem::canonical(std::filesystem::u8path(filename));
                if (!std::filesystem::is_regular_file(path))
                    return false;
                std::ifstream input(path, std::ios::binary | std::ios::ate);
                const auto    size = input.tellg();
                if (!input || size < 0 || static_cast<std::uintmax_t>(size) > max_bytes_)
                    return false;
                std::vector<unsigned char> candidate(static_cast<std::size_t>(size));
                input.seekg(0);
                if (!candidate.empty() && !input.read(reinterpret_cast<char*>(candidate.data()), static_cast<std::streamsize>(candidate.size())))
                    return false;
                if (!validator_(path, candidate))
                    return false;
                bytes_ = std::move(candidate);
                open_ = true;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        void FClose() override
        {
            std::vector<unsigned char>().swap(bytes_);
            position_ = 0;
            open_ = eof_ = false;
        }
        bool FEof() override { return !open_ || eof_; }
        void FFlush() override {}
        int  FTell() override { return open_ ? position_ : -1; }
        void FSeek(int offset, int origin) override
        {
            if (!open_ || (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END))
                return;
            const std::int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? position_ : static_cast<std::int64_t>(bytes_.size());
            const auto         next = base + offset;
            if (next < 0 || next > INT_MAX)
                return;
            position_ = static_cast<int>(next);
            eof_ = false;
        }
        int FRead(void* output, uint count) override
        {
            if (!open_ || (!output && count) || count > static_cast<uint>(INT_MAX))
                return 0;
            const auto remaining = static_cast<std::size_t>(position_) < bytes_.size() ? bytes_.size() - position_ : 0;
            const auto copied = std::min<std::size_t>(count, remaining);
            if (copied)
                std::memcpy(output, bytes_.data() + position_, copied);
            position_ += static_cast<int>(copied);
            if (copied < count)
                eof_ = true;
            return static_cast<int>(copied);
        }
        int FWrite(const void*, uint) override { return 0; }

    private:
        Validator                  validator_;
        std::size_t                max_bytes_;
        std::vector<unsigned char> bytes_;
        int                        position_ = 0;
        bool                       open_ = false, eof_ = false;
    };
}
