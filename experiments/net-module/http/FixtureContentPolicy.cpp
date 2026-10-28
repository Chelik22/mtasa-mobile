#ifdef _WIN32
    #include <windows.h>
    #include <bcrypt.h>
#else
    #error "Fixture policy proof currently targets the Win32 component"
#endif
#include "FixtureContentPolicy.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace experimental_http
{
    namespace
    {
        std::string utf8(const std::filesystem::path& path)
        {
            const auto value = path.generic_u8string();
            return std::string(reinterpret_cast<const char*>(value.data()), value.size());
        }
        bool relative_key(const std::filesystem::path& path)
        {
            if (path.empty() || path.is_absolute() || path.has_root_path())
                return false;
            for (const auto& component : path)
                if (component == "." || component == "..")
                    return false;
            return true;
        }
        bool within(const std::filesystem::path& path, const std::filesystem::path& root)
        {
            const auto relative = path.lexically_relative(root);
            return relative == "." || relative_key(relative);
        }
        bool fail(std::string& error, const char* reason)
        {
            error = reason;
            return false;
        }
        struct Algorithm
        {
            BCRYPT_ALG_HANDLE value = nullptr;
            ~Algorithm()
            {
                if (value)
                    BCryptCloseAlgorithmProvider(value, 0);
            }
        };
        struct Hash
        {
            BCRYPT_HASH_HANDLE value = nullptr;
            ~Hash()
            {
                if (value)
                    BCryptDestroyHash(value);
            }
        };
        struct File
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~File()
            {
                if (value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }
        };
        struct Inspection
        {
            std::string                  sha256, source;
            std::array<unsigned char, 8> prefix{};
            size_t                       prefix_size = 0;
        };
        Inspection inspect(const std::filesystem::path& path, size_t bytes, FixtureFormat format)
        {
            File file;
            // Refuse reparse points and concurrent writers during the actual hash.
            file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file.value == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot open fixture temporary for a stable read");
            BY_HANDLE_FILE_INFORMATION info{};
            LARGE_INTEGER              size{};
            if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
                GetFileType(file.value) != FILE_TYPE_DISK || !GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
                static_cast<unsigned long long>(size.QuadPart) != bytes)
                throw std::runtime_error("Fixture temporary type/size changed");
            Algorithm algorithm;
            if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                throw std::runtime_error("BCrypt SHA256 provider failed");
            DWORD object_size = 0, result_size = 0;
            if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0 ||
                !object_size || object_size > 1024 * 1024)
                throw std::runtime_error("BCrypt SHA256 object size failed");
            std::vector<unsigned char> object(object_size);
            Hash                       hash;
            if (BCryptCreateHash(algorithm.value, &hash.value, object.data(), object_size, nullptr, 0, 0) < 0)
                throw std::runtime_error("BCrypt SHA256 hash creation failed");
            Inspection result;
            if (format == FixtureFormat::LuaSourceUtf8)
                result.source.reserve(bytes);
            std::array<unsigned char, 16384> buffer;
            size_t                           read = 0;
            while (read < bytes)
            {
                DWORD count = 0;
                if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(std::min(buffer.size(), bytes - read)), &count, nullptr) || !count)
                    throw std::runtime_error("Fixture temporary read failed/truncated");
                if (!read)
                {
                    result.prefix_size = std::min<size_t>(count, result.prefix.size());
                    std::copy_n(buffer.data(), result.prefix_size, result.prefix.data());
                }
                if (BCryptHashData(hash.value, buffer.data(), count, 0) < 0)
                    throw std::runtime_error("BCrypt SHA256 update failed");
                if (format == FixtureFormat::LuaSourceUtf8)
                    result.source.append(reinterpret_cast<const char*>(buffer.data()), count);
                read += count;
            }
            std::array<unsigned char, 32> digest{};
            if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
                throw std::runtime_error("BCrypt SHA256 finish failed");
            const char digits[] = "0123456789abcdef";
            for (const auto byte : digest)
            {
                result.sha256 += digits[byte >> 4];
                result.sha256 += digits[byte & 15];
            }
            return result;
        }
    }

    FixtureContentPolicy::FixtureContentPolicy(std::filesystem::path cache_root, std::vector<FixtureEntry> entries)
        : root_(std::filesystem::canonical(cache_root))
    {
        if (!std::filesystem::is_directory(root_) || entries.empty() || entries.size() > 128)
            throw std::invalid_argument("An existing fixture cache root and 1..128 manifest entries are required");
        for (auto& entry : entries)
        {
            const auto path = std::filesystem::u8path(entry.destination_relative);
            if (!relative_key(path) || path.filename().empty() || utf8(path.lexically_normal()) != entry.destination_relative ||
                entry.destination_relative.size() > 4096 || entry.destination_relative.find_first_of("\\:") != std::string::npos || entry.url.size() > 8192 ||
                !(entry.url.starts_with("http://") || entry.url.starts_with("https://")) || entry.raw_sha256.size() != 64 ||
                entry.max_bytes > 256 * 1024 * 1024 || (entry.format != FixtureFormat::LuaSourceUtf8 && entry.format != FixtureFormat::BinaryAsset) ||
                (entry.format == FixtureFormat::LuaSourceUtf8 && entry.max_bytes > 1024 * 1024))
                throw std::invalid_argument("Invalid fixture manifest entry/path/URL/bound/format");
            for (char& byte : entry.raw_sha256)
            {
                if (byte >= 'A' && byte <= 'F')
                    byte += 'a' - 'A';
                if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
                    throw std::invalid_argument("Invalid fixture SHA256");
            }
            auto key = entry.destination_relative;
            if (!entries_.emplace(std::move(key), std::move(entry)).second)
                throw std::invalid_argument("Duplicate fixture manifest path");
        }
    }

    bool FixtureContentPolicy::operator()(const FileValidationContext& context, std::string& error) const
    {
        error.clear();
        try
        {
            if (!context.destination.is_absolute() || !context.temporary.is_absolute())
                return fail(error, "Fixture context paths must be absolute");
            const auto destination = context.destination.lexically_normal();
            const auto relative = destination.lexically_relative(root_);
            if (!relative_key(relative))
                return fail(error, "Fixture destination escapes the configured cache root");
            const auto found = entries_.find(utf8(relative));
            if (found == entries_.end())
                return fail(error, "Fixture destination is absent from the trusted manifest");
            const auto& entry = found->second;
            if (context.url != entry.url || context.effective_url != entry.url)
                return fail(error, "Fixture requested/effective URL does not match the manifest");
            if (context.http_status < 200 || context.http_status > 299)
                return fail(error, "Fixture did not receive a successful HTTP status");
            if (context.bytes_received > entry.max_bytes)
                return fail(error, "Fixture response exceeds its manifest bound");
            const auto parent = std::filesystem::weakly_canonical(destination.parent_path());
            if (!within(parent, root_))
                return fail(error, "Fixture destination parent resolves outside the cache root");
            if (parent != destination.parent_path())
                return fail(error, "Fixture destination parent must not use a filesystem alias");
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(context.temporary)))
                return fail(error, "Fixture temporary cannot be a symlink");
            const auto temporary = std::filesystem::canonical(context.temporary);
            if (temporary.parent_path() != parent || temporary.filename() == destination.filename())
                return fail(error, "Fixture temporary is not a distinct adjacent file");
            const auto actual = inspect(temporary, context.bytes_received, entry.format);
            if (actual.sha256 != entry.raw_sha256)
                return fail(error, "Fixture raw SHA256 does not match the trusted manifest");
            const auto&  prefix = actual.prefix;
            const size_t offset = actual.prefix_size >= 3 && prefix[0] == 0xef && prefix[1] == 0xbb && prefix[2] == 0xbf ? 3 : 0;
            if ((actual.prefix_size >= 2 && prefix[0] == 'M' && prefix[1] == 'Z') ||
                (actual.prefix_size >= 4 && prefix[0] == 0x7f && prefix[1] == 'E' && prefix[2] == 'L' && prefix[3] == 'F'))
                return fail(error, "Native executable magic is outside fixture asset policy");
            if (actual.prefix_size >= offset + 4 && prefix[offset] == 0x1b && prefix[offset + 1] == 'L' && prefix[offset + 2] == 'u' &&
                prefix[offset + 3] == 'a')
                return fail(error, "Compiled Lua is outside fixture policy");
            if (entry.format == FixtureFormat::LuaSourceUtf8)
            {
                if (actual.prefix_size > offset && prefix[offset] == 0x1b)
                    return fail(error, "Source Lua cannot begin with an ESC chunk marker");
                if (actual.source.find('\0') != std::string::npos)
                    return fail(error, "Source Lua contains NUL bytes");
                if (!actual.source.empty() &&
                    !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, actual.source.data(), static_cast<int>(actual.source.size()), nullptr, 0))
                    return fail(error, "Source Lua is not valid UTF8");
            }
            return true;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }
}
