#pragma once

// The opt-in laboratory must not reuse an installed game's settings. Keep this
// implementation out of public SDK/PCH headers so ordinary builds retain their
// existing ABI and incremental compilation dependencies.
#if defined(_WIN32) && defined(MTA_CLIENT)
    #include <windows.h>
    #include <algorithm>
    #include <array>
    #include <atomic>
    #include <cstdint>
    #include <filesystem>
    #include <map>
    #include <optional>
    #include <stdexcept>
    #include <string>
    #include <string_view>
    #include <vector>

namespace experimental_lab
{
    namespace fs = std::filesystem;
    inline constexpr wchar_t                          environment[] = L"MTA_EXPERIMENTAL_LAB_ROOT";
    inline constexpr std::array<std::wstring_view, 9> children{L"app",           L"gta",  L"data",     L"common", L"documents",
                                                               L"local-appdata", L"temp", L"settings", L"logs"};

    inline void RequireDirectory(const fs::path& path)
    {
        fs::path current = path.root_path();
        for (const auto& part : path.relative_path())
        {
            if (part == L"." || part == L"..")
                throw std::runtime_error("Lab paths cannot contain relative traversal components");
            current /= part;
            const DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Lab path must be an existing directory without reparse points");
        }
    }

    struct Context
    {
        bool     enabled = false;
        fs::path root;
        fs::path Child(const wchar_t* name) const
        {
            if (!enabled || !name || std::find(children.begin(), children.end(), std::wstring_view(name)) == children.end())
                throw std::runtime_error("Lab paths are not enabled");
            const fs::path path = root / name;
            RequireDirectory(path);
            return path;
        }
    };

    inline const Context& GetContext()
    {
        // Every module captures the inherited process environment once. Changing
        // it later cannot move an already-open module's settings to another root.
        static const Context context = []
        {
            SetLastError(ERROR_SUCCESS);
            const DWORD required = GetEnvironmentVariableW(environment, nullptr, 0);
            if (!required && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                return Context{};
            if (!required || required > 32768)
                throw std::runtime_error("Invalid explicit MTA_EXPERIMENTAL_LAB_ROOT");
            std::wstring value(required, L'\0');
            const DWORD  copied = GetEnvironmentVariableW(environment, value.data(), required);
            if (!copied || copied >= required)
                throw std::runtime_error("Lab root changed while being captured");
            value.resize(copied);
            const fs::path input(value);
            if (!input.is_absolute() || value.rfind(L"\\\\", 0) == 0)
                throw std::runtime_error("Lab root must be an absolute local path");
            RequireDirectory(input);
            Context result{true, fs::canonical(input)};
            for (const auto child : children)
                result.Child(child.data());
            return result;
        }();
        return context;
    }

    inline std::string Utf8(const fs::path& path)
    {
        const auto value = path.u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }

    inline bool Same(std::wstring_view left, std::wstring_view right)
    {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    inline bool RegistryPathWithin(std::wstring_view path, std::wstring_view parent)
    {
        return path.size() >= parent.size() && Same(path.substr(0, parent.size()), parent) && (path.size() == parent.size() || path[parent.size()] == L'\\');
    }

    struct Handle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        Handle() = default;
        explicit Handle(HANDLE handle) : value(handle) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle()
        {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
        void Regular() const
        {
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(value, FileAttributeTagInfo, &info, sizeof(info)) ||
                (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                throw std::runtime_error("Lab settings require a regular non-reparse file");
        }
    };

    struct SettingsLock
    {
        fs::path directory = GetContext().Child(L"settings");
        Handle file{CreateFileW((directory / L"registry.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        OVERLAPPED range{};
        SettingsLock()
        {
            file.Regular();
            if (!LockFileEx(file.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range))
                throw std::runtime_error("Cannot lock lab settings");
        }
        ~SettingsLock() { UnlockFileEx(file.value, 0, 1, 0, &range); }
    };

    struct KeyLess
    {
        bool operator()(const std::wstring& left, const std::wstring& right) const
        {
            return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
        }
    };
    using Settings = std::map<std::wstring, std::string, KeyLess>;
    inline constexpr std::size_t max_file = 8 * 1024 * 1024, max_value = 1024 * 1024, max_key = 32768, max_entries = 4096;

    inline std::wstring Key(HKEY hive, std::wstring_view path, std::wstring_view name)
    {
        const wchar_t* prefix = hive == HKEY_LOCAL_MACHINE ? L"HKLM\\" : hive == HKEY_CURRENT_USER ? L"HKCU\\" : nullptr;
        if (!prefix || path.size() + name.size() + 6 > max_key)
            throw std::runtime_error("Unsupported lab settings key");
        std::wstring result(prefix);
        result.append(path);
        result.push_back(L'\0');
        result.append(name);
        return result;
    }

    inline std::uint32_t Take32(const std::vector<unsigned char>& bytes, std::size_t& offset)
    {
        if (offset > bytes.size() || bytes.size() - offset < 4)
            throw std::runtime_error("Truncated lab settings");
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index)
            value |= std::uint32_t(bytes[offset++]) << (8 * index);
        return value;
    }
    inline void Put32(std::vector<unsigned char>& bytes, std::uint32_t value)
    {
        for (unsigned index = 0; index < 4; ++index)
            bytes.push_back(static_cast<unsigned char>(value >> (8 * index)));
    }

    inline Settings Load(const fs::path& directory)
    {
        const fs::path path = directory / L"registry.bin";
        Handle         file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND)
            return {};
        file.Regular();
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 12 || size.QuadPart > static_cast<LONGLONG>(max_file))
            throw std::runtime_error("Invalid lab settings size");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD                      copied = 0;
        if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &copied, nullptr) || copied != bytes.size() ||
            !std::equal(bytes.begin(), bytes.begin() + 8, "MTALAB01"))
            throw std::runtime_error("Invalid lab settings data");
        std::size_t offset = 8;
        const auto  count = Take32(bytes, offset);
        if (count > max_entries)
            throw std::runtime_error("Too many lab settings");
        Settings settings;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const auto key_size = Take32(bytes, offset), value_size = Take32(bytes, offset);
            if (!key_size || key_size > max_key || value_size > max_value || key_size > (bytes.size() - offset) / 2 ||
                value_size > bytes.size() - offset - key_size * 2)
                throw std::runtime_error("Invalid lab settings record");
            std::wstring key;
            key.reserve(key_size);
            for (std::uint32_t unit = 0; unit < key_size; ++unit)
            {
                key.push_back(static_cast<wchar_t>(bytes[offset] | (std::uint16_t(bytes[offset + 1]) << 8)));
                offset += 2;
            }
            std::string value(reinterpret_cast<const char*>(bytes.data() + offset), value_size);
            offset += value_size;
            if (!settings.emplace(std::move(key), std::move(value)).second)
                throw std::runtime_error("Duplicate lab settings key");
        }
        if (offset != bytes.size())
            throw std::runtime_error("Trailing lab settings bytes");
        return settings;
    }

    inline void Save(const fs::path& directory, const Settings& settings)
    {
        if (settings.size() > max_entries)
            throw std::runtime_error("Too many lab settings");
        std::vector<unsigned char> bytes{'M', 'T', 'A', 'L', 'A', 'B', '0', '1'};
        Put32(bytes, static_cast<std::uint32_t>(settings.size()));
        for (const auto& [key, value] : settings)
        {
            if (key.empty() || key.size() > max_key || value.size() > max_value || bytes.size() + 8 + key.size() * 2 + value.size() > max_file)
                throw std::runtime_error("Lab settings limit exceeded");
            Put32(bytes, static_cast<std::uint32_t>(key.size()));
            Put32(bytes, static_cast<std::uint32_t>(value.size()));
            for (wchar_t unit : key)
            {
                bytes.push_back(static_cast<unsigned char>(unit));
                bytes.push_back(static_cast<unsigned char>(unit >> 8));
            }
            bytes.insert(bytes.end(), value.begin(), value.end());
        }
        static std::atomic<unsigned long> sequence{};
        const fs::path temporary = directory / (L"registry." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount()) + L"." +
                                                std::to_wstring(sequence++) + L".tmp");
        bool           created = false;
        try
        {
            {
                Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                created = file.value != INVALID_HANDLE_VALUE;
                file.Regular();
                DWORD written = 0;
                if (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
                    !FlushFileBuffers(file.value))
                    throw std::runtime_error("Cannot write lab settings");
            }
            if (!MoveFileExW(temporary.c_str(), (directory / L"registry.bin").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot publish lab settings");
        }
        catch (...)
        {
            if (created)
                DeleteFileW(temporary.c_str());
            throw;
        }
    }

    inline std::optional<std::string> ReadSetting(HKEY hive, std::wstring_view path, std::wstring_view name)
    {
        SettingsLock lock;
        const auto   settings = Load(lock.directory);
        const auto   found = settings.find(Key(hive, path, name));
        return found == settings.end() ? std::nullopt : std::optional<std::string>(found->second);
    }
    inline void WriteSetting(HKEY hive, std::wstring_view path, std::wstring_view name, const std::string& value)
    {
        if (value.size() > max_value)
            throw std::runtime_error("Lab settings value limit exceeded");
        SettingsLock lock;
        auto         settings = Load(lock.directory);
        const auto   key = Key(hive, path, name);
        const auto   old = settings.find(key);
        if (old != settings.end() && old->second == value)
            return;
        settings[key] = value;
        Save(lock.directory, settings);
    }
    inline bool DeleteSettingsKey(HKEY hive, std::wstring_view path)
    {
        SettingsLock lock;
        auto         settings = Load(lock.directory);
        const auto   prefix = Key(hive, path, L"");
        const auto   key_path = std::wstring_view(prefix).substr(0, prefix.size() - 1);
        for (const auto& [key, value] : settings)
        {
            const auto stored_path = std::wstring_view(key).substr(0, key.find(L'\0'));
            if (stored_path.size() > key_path.size() && RegistryPathWithin(stored_path, key_path))
                return false;  // RegDeleteKey does not remove a key containing subkeys.
        }
        bool removed = false;
        for (auto entry = settings.begin(); entry != settings.end();)
        {
            if (Same(std::wstring_view(entry->first).substr(0, entry->first.find(L'\0')), key_path))
            {
                entry = settings.erase(entry);
                removed = true;
            }
            else
                ++entry;
        }
        if (removed)
            Save(lock.directory, settings);
        return removed;
    }
}
#endif
