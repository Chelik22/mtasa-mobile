#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
    #include <windows.h>
#endif

namespace experimental_server_cache
{
    namespace fs = std::filesystem;
    inline constexpr char                            environment[] = "MTA_EXPERIMENTAL_SERVER_HTTP_CACHE_ROOT";
    inline constexpr std::array<std::string_view, 2> fixture_files{"http-client-files/mobile-world-fixture/http-witness.txt",
                                                                   "http-client-files-no-client-cache/mobile-world-fixture/client.lua"};

    class CacheFilenamePolicy
    {
    public:
        CacheFilenamePolicy() = default;
        explicit CacheFilenamePolicy(std::string_view root, std::span<const std::string_view> entries = fixture_files)
        {
            try
            {
                root_ = Absolute(root);
                if (root_ == root_.root_path() || !SafeTree(root_, true))
                    throw std::invalid_argument("cache root must be an existing non-reparse directory");
                const auto canonical = fs::canonical(root_);
                if (canonical != root_ || !SafeTree(canonical, true))
                    throw std::invalid_argument("cache root must already be canonical");
                if (entries.empty() || entries.size() > 128)
                    throw std::invalid_argument("cache allowlist count outside bounds");
                for (auto entry : entries)
                {
                    if (entry.empty() || entry.size() > 512 || entry.find('\\') != entry.npos || entry.find("//") != entry.npos || entry.back() == '/')
                        throw std::invalid_argument("invalid cache relative entry");
                    const auto path = FromUtf8(entry);
                    if (path.has_root_path() || !Segments(path) || path.lexically_normal() != path)
                        throw std::invalid_argument("cache entry is not portable relative path");
                    auto folded = std::string(entry);
                    // The experimental fixture manifest is ASCII. Fail closed for other filename alphabets.
                    for (auto& c : folded)
                    {
                        if (static_cast<unsigned char>(c) >= 128)
                            throw std::invalid_argument("fixture cache entry must be ASCII");
                        if (c >= 'A' && c <= 'Z')
                            c += 'a' - 'A';
                    }
                    if (std::find(folded_.begin(), folded_.end(), folded) != folded_.end())
                        throw std::invalid_argument("cache entry case alias/duplicate");
                    keys_.emplace_back(entry);
                    folded_.push_back(std::move(folded));
                }
                ready_ = true;
                error_.clear();
            }
            catch (const std::exception& error)
            {
                error_ = error.what();
            }
        }

        static CacheFilenamePolicy FromEnvironment()
        {
            try
            {
#ifdef _WIN32
                SetLastError(ERROR_SUCCESS);
                const DWORD length = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_SERVER_HTTP_CACHE_ROOT", nullptr, 0);
                if (!length)
                    return {};
                if (length > max_path_bytes + 1)
                    throw std::invalid_argument("cache root environment too long");
                std::wstring wide(length, L'\0');
                const DWORD  copied = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_SERVER_HTTP_CACHE_ROOT", wide.data(), length);
                if (!copied || copied >= length)
                    throw std::invalid_argument("cache root environment changed or empty");
                wide.resize(copied);
                const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
                if (count <= 0 || count > max_path_bytes)
                    throw std::invalid_argument("cache root is not bounded UTF-8");
                std::string value(count, '\0');
                if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), value.data(), count, nullptr, nullptr) !=
                    count)
                    throw std::invalid_argument("cache root UTF-8 conversion failed");
                return CacheFilenamePolicy(value);
#else
                const auto* value = std::getenv(environment);
                const auto  bounded = Bounded(value);
                return bounded ? CacheFilenamePolicy(*bounded) : CacheFilenamePolicy{};
#endif
            }
            catch (const std::exception& error)
            {
                CacheFilenamePolicy disabled;
                disabled.error_ = error.what();
                return disabled;
            }
        }

        bool               Ready() const noexcept { return ready_; }
        const std::string& Error() const noexcept { return error_; }
        bool               Allows(const char* filename) const noexcept
        {
            const auto bounded = Bounded(filename);
            return bounded && Allows(*bounded);
        }
        bool Allows(std::string_view filename) const noexcept
        {
            if (!ready_)
                return false;
            try
            {
                const auto path = Absolute(filename);
                const auto relative = path.lexically_relative(root_);
                if (relative.empty() || relative.has_root_path() || !Segments(relative))
                    return false;
                const auto        u8 = relative.generic_u8string();
                const std::string key(reinterpret_cast<const char*>(u8.data()), u8.size());
                if (std::find(keys_.begin(), keys_.end(), key) == keys_.end())
                    return false;
                // The destination may not exist yet: GenerateChecksums validates BEFORE FileCopy.
                // Inspect every existing ancestor, not canonical(destination), which requires the file.
                return SafeTree(root_, true) && SafeTree(path, false) && fs::weakly_canonical(path) == path;
            }
            catch (...)
            {
                return false;
            }
        }

    private:
        static constexpr std::size_t           max_path_bytes = 4096;
        static std::optional<std::string_view> Bounded(const char* text) noexcept
        {
            if (!text)
                return {};
            for (std::size_t size = 0; size <= max_path_bytes; ++size)
                if (!text[size])
                    return std::string_view(text, size);
            return {};
        }
        static bool Utf8(std::string_view text)
        {
            for (std::size_t i = 0; i < text.size();)
            {
                const auto lead = static_cast<unsigned char>(text[i++]);
                if (lead < 128)
                    continue;
                unsigned code = 0, remaining = 0, minimum = 0;
                if (lead >= 0xc2 && lead <= 0xdf)
                {
                    code = lead & 31;
                    remaining = 1;
                    minimum = 0x80;
                }
                else if (lead >= 0xe0 && lead <= 0xef)
                {
                    code = lead & 15;
                    remaining = 2;
                    minimum = 0x800;
                }
                else if (lead >= 0xf0 && lead <= 0xf4)
                {
                    code = lead & 7;
                    remaining = 3;
                    minimum = 0x10000;
                }
                else
                    return false;
                if (remaining > text.size() - i)
                    return false;
                while (remaining--)
                {
                    const auto next = static_cast<unsigned char>(text[i++]);
                    if ((next & 0xc0) != 0x80)
                        return false;
                    code = (code << 6) | (next & 63);
                }
                if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                    return false;
            }
            return true;
        }
        static fs::path FromUtf8(std::string_view text)
        {
            if (!Utf8(text) || text.find('\0') != text.npos)
                throw std::invalid_argument("invalid UTF-8 cache path");
#ifdef _WIN32
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (count <= 0)
                throw std::invalid_argument("cache UTF-8 conversion failed");
            std::wstring wide(count, L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), count) != count)
                throw std::invalid_argument("cache UTF-8 conversion changed");
            return fs::path(wide);
#else
            return fs::u8path(text.begin(), text.end());
#endif
        }
        static bool Segments(const fs::path& path)
        {
            for (const auto& component : path)
            {
                const auto  u8 = component.generic_u8string();
                std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
                if (name.empty() || name == "." || name == ".." || name.size() > 255 || name.back() == '.' || name.back() == ' ')
                    return false;
                for (const unsigned char c : name)
                    if (c < 32 || c == 127 || std::string_view("\\:<>\"|?*~").find(static_cast<char>(c)) != std::string_view::npos)
                        return false;
                auto base = name.substr(0, name.find('.'));
                while (!base.empty() && base.back() == ' ')
                    base.pop_back();
                for (auto& c : base)
                    if (c >= 'a' && c <= 'z')
                        c -= 'a' - 'A';
                if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$")
                    return false;
                if (base.starts_with("COM") || base.starts_with("LPT"))
                {
                    const auto digit = base.substr(3);
                    if ((digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') || digit == "\xc2\xb9" || digit == "\xc2\xb2" || digit == "\xc2\xb3")
                        return false;
                }
            }
            return true;
        }
        static fs::path Absolute(std::string_view raw)
        {
            if (raw.empty() || raw.size() > max_path_bytes)
                throw std::invalid_argument("cache path size outside bounds");
            std::string syntax(raw);
#ifdef _WIN32
            std::replace(syntax.begin(), syntax.end(), '\\', '/');
#else
            if (syntax.find('\\') != syntax.npos)
                throw std::invalid_argument("backslash cache alias");
#endif
            if (syntax.find("//") != syntax.npos || syntax.back() == '/')
                throw std::invalid_argument("cache separator alias");
            const auto path = FromUtf8(syntax);
            if (!path.is_absolute() || !Segments(path.relative_path()) || path.lexically_normal() != path)
                throw std::invalid_argument("cache path must be canonical absolute");
#ifdef _WIN32
            const auto drive = path.root_name().native();
            if (drive.size() != 2 || drive[1] != L':' || !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')))
                throw std::invalid_argument("cache path requires a local drive");
#endif
            return path;
        }
        static bool SafeTree(const fs::path& path, bool directory)
        {
            auto       current = path.root_path();
            const auto relative = path.relative_path();
            for (auto it = relative.begin(); it != relative.end(); ++it)
            {
                current /= *it;
                const bool last = std::next(it) == relative.end();
#ifdef _WIN32
                const auto attributes = GetFileAttributesW(current.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const auto error = GetLastError();
                    return !directory && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
                }
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    return false;
#endif
                std::error_code error;
                const auto      status = fs::symlink_status(current, error);
                if (error && error != std::errc::no_such_file_or_directory)
                    return false;
                if (status.type() == fs::file_type::not_found || error == std::errc::no_such_file_or_directory)
                    return !directory;
                if (fs::is_symlink(status))
                    return false;
                if (!last || directory)
                {
                    if (!fs::is_directory(status))
                        return false;
                }
                else if (!fs::is_regular_file(status))
                    return false;
            }
            return true;
        }
        fs::path                 root_;
        std::vector<std::string> keys_, folded_;
        std::string              error_ = "server HTTP cache root is not configured";
        bool                     ready_ = false;
    };
}  // namespace experimental_server_cache
