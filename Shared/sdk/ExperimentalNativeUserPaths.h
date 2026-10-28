#pragma once

#if defined(_WIN32) && defined(MTA_CLIENT)
    #include "ExperimentalLabPaths.h"
    #include <bcrypt.h>
    #include <cstring>
    #ifdef _MSC_VER
        #pragma comment(lib, "bcrypt.lib")
    #endif

// GTA reads Personal itself: redirecting SharedUtil paths cannot isolate these
// native writes. This opt-in supports one audited executable, never arbitrary GTA.
namespace experimental_lab::native_paths
{
    inline constexpr std::size_t                   executable_size = 0x00db7a00;
    inline constexpr std::uintptr_t                image_base = 0x400000, routine = 0x744fb0;
    inline constexpr std::array<unsigned char, 16> expected_code{0x83, 0xec, 0x0c, 0x8d, 0x44, 0x24, 0x00, 0x50,
                                                                 0xe9, 0x4a, 0x18, 0xcc, 0xff, 0x6a, 0x00, 0x68};
    inline constexpr std::array<std::uintptr_t, 3> buffers{0xc92368, 0xc92268, 0xc92168};
    inline constexpr std::array<unsigned char, 32> canonical_hash{0xf6, 0x3f, 0xa6, 0x23, 0xd1, 0x4e, 0x17, 0x0d, 0x0a, 0xe9, 0xe7,
                                                                  0x03, 0x21, 0x86, 0x66, 0x9c, 0xf3, 0xa1, 0x77, 0x79, 0x3b, 0xe2,
                                                                  0x1c, 0x48, 0x24, 0x04, 0x3f, 0x02, 0x79, 0xbd, 0x50, 0x6a};

    inline auto Sha256(const std::vector<unsigned char>& bytes) -> std::array<unsigned char, 32>
    {
        BCRYPT_ALG_HANDLE             algorithm = nullptr;
        BCRYPT_HASH_HANDLE            hash = nullptr;
        std::array<unsigned char, 32> result{};
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Cannot open SHA256 provider");
        const auto cleanup = [&]
        {
            if (hash)
                BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
        };
        try
        {
            if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0 ||
                BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0 ||
                BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
                throw std::runtime_error("Cannot hash native executable");
        }
        catch (...)
        {
            cleanup();
            throw;
        }
        cleanup();
        return result;
    }

    inline bool IsKnownExecutable(const std::vector<unsigned char>& bytes)
    {
        // Full raw-image hashes, derived from canonical bytes plus the exact
        // pinned ApplyPatches combinations. No masked/zeroed regions: arbitrary
        // export padding, timestamp or checksum changes must also be rejected.
        if (bytes.size() != executable_size)
            return false;
        constexpr std::array<std::string_view, 9> allowed{
            "f63fa623d14e170d0ae9e7032186669cf3a177793be21c4824043f0279bd506a", "f3d07cca3375af2fe4d19ff4035c444f2d4fd5a9c07fa78ed834bd6756d5b37c",
            "5b01125db4b0bf04592bd22ddc69245eb391e9b626a950525f95cfb08f676cce", "77485627b4ef17f92819318050d501e171c7ab84ceffe5091b973b9e29f9cc98",
            "1c8e082037446cd239eed9c924afff8ced3891cd9a426fdd1c9317671cfcf4b9", "c57460a2e242c1ac3d05054d477c582eeec9a085ce582ff5640925938f58f052",
            "5cd22ed63ffb628a2538a29139cfeee4a1a96224b84fa89fa598bf67f316873c", "416cdc04f19b317393a6dfc404be28449e6dd38bfc63cc3cccc10e5fb51f3eae",
            "2f046967c782658f15dfe1524d3d3a0696f90d09b5a602cc40d3a52990135ffc",
        };
        const auto  hash = Sha256(bytes);
        std::string text;
        for (const auto byte : hash)
        {
            text += "0123456789abcdef"[byte >> 4];
            text += "0123456789abcdef"[byte & 15];
        }
        return std::find(allowed.begin(), allowed.end(), text) != allowed.end();
    }

    inline auto ReadExecutable(const fs::path& path) -> std::vector<unsigned char>
    {
        RequireDirectory(path.parent_path());
        // Deny concurrent write/delete while reading the bytes being approved.
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        file.Regular();
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart != executable_size)
            throw std::runtime_error("Unexpected native executable length");
        std::vector<unsigned char> bytes(executable_size);
        DWORD                      actual = 0;
        if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &actual, nullptr) || actual != bytes.size())
            throw std::runtime_error("Cannot read full native executable");
        return bytes;
    }

    inline auto AnsiPath(const fs::path& path) -> std::string
    {
        const auto& wide = path.native();
        const UINT  page = GetACP();
        BOOL        substituted = FALSE;
        const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        auto*       defaulted = page == CP_UTF8 ? nullptr : &substituted;
        const int   size = WideCharToMultiByte(page, flags, wide.c_str(), -1, nullptr, 0, nullptr, defaulted);
        if (size <= 0 || size > 256 || substituted)
            throw std::runtime_error("Native user path exceeds 256 bytes or cannot round-trip ACP");
        std::string value(size, '\0');
        if (WideCharToMultiByte(page, flags, wide.c_str(), -1, value.data(), size, nullptr, defaulted) != size || substituted)
            throw std::runtime_error("Cannot encode native user path");
        std::wstring roundtrip(wide.size() + 1, '\0');
        const int    decoded = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, value.data(), size, roundtrip.data(), static_cast<int>(roundtrip.size()));
        if (decoded != static_cast<int>(roundtrip.size()) || std::wstring_view(roundtrip.data(), roundtrip.size() - 1) != wide)
            throw std::runtime_error("Native user path ACP conversion changed its identity");
        value.pop_back();
        return value;
    }

    struct Paths
    {
        std::array<fs::path, 3>              wide;
        std::array<std::array<char, 256>, 3> bytes{};
    };
    inline Paths PreparePaths(const Context& context)
    {
        const auto base = context.Child(L"documents") / L"GTA San Andreas User Files";
        Paths      result{{base, base / L"Gallery", base / L"User Tracks"}, {}};
        for (std::size_t i = 0; i < result.wide.size(); ++i)
        {
            const auto text = AnsiPath(result.wide[i]);
            std::memcpy(result.bytes[i].data(), text.c_str(), text.size() + 1);
            if (fs::exists(result.wide[i]))
                RequireDirectory(result.wide[i]);
        }
        return result;
    }

    inline void CreateDirectories(const Paths& paths)
    {
        for (const auto& path : paths.wide)
        {
            RequireDirectory(path.parent_path());
            if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Cannot create private native user directory");
            RequireDirectory(path);
        }
    }

    inline void Publish(const Paths& paths, const std::array<char*, 3>& destinations)
    {
        // Recheck before every use so a later reparse replacement cannot silently
        // move a native save path outside the already-approved lab hierarchy.
        for (const auto& path : paths.wide)
            RequireDirectory(path);
        for (std::size_t i = 0; i < destinations.size(); ++i)
            std::memcpy(destinations[i], paths.bytes[i].data(), paths.bytes[i].size());
    }

    inline Paths         installed_paths;
    inline char* __cdecl UserDirectory() noexcept
    {
        try
        {
            Publish(installed_paths, {reinterpret_cast<char*>(buffers[0]), reinterpret_cast<char*>(buffers[1]), reinterpret_cast<char*>(buffers[2])});
            return reinterpret_cast<char*>(buffers[0]);
        }
        catch (...)
        {
            TerminateProcess(GetCurrentProcess(), 0x4c414250);
            ExitProcess(0x4c414250);
        }
    }

    inline void RequireImageRegion(std::uintptr_t address, std::size_t size, bool writable)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
            info.AllocationBase != reinterpret_cast<void*>(image_base) || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            address < reinterpret_cast<std::uintptr_t>(info.BaseAddress) ||
            size > info.RegionSize - (address - reinterpret_cast<std::uintptr_t>(info.BaseAddress)) || (writable && info.Protect != PAGE_READWRITE))
            throw std::runtime_error("Native user path image region mismatch");
    }

    inline void Redirect(void* address, const std::array<unsigned char, 16>& expected, const void* replacement)
    {
        if (std::memcmp(address, expected.data(), expected.size()))
            throw std::runtime_error("Native user path instructions differ");
        std::array<unsigned char, 7> jump{0xe9, 0, 0, 0, 0, 0x90, 0x90};
        const auto                   delta = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(replacement)) -
                           static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address) + 5);
        std::memcpy(jump.data() + 1, &delta, sizeof(delta));
        DWORD protection = 0;
        if (!VirtualProtect(address, jump.size(), PAGE_EXECUTE_READWRITE, &protection))
            throw std::runtime_error("Cannot protect native path detour");
        std::memcpy(address, jump.data(), jump.size());
        DWORD      ignored = 0;
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, jump.size()) != FALSE;
        const bool restored = VirtualProtect(address, jump.size(), protection, &ignored) != FALSE;
        if (!flushed || !restored)
        {
            // The caller must terminate its owned child on *any* failure. Try to
            // restore original instructions first; never continue with half a patch.
            DWORD temporary = 0;
            if (VirtualProtect(address, jump.size(), PAGE_EXECUTE_READWRITE, &temporary))
            {
                std::memcpy(address, expected.data(), jump.size());
                FlushInstructionCache(GetCurrentProcess(), address, jump.size());
                VirtualProtect(address, jump.size(), protection, &ignored);
            }
            throw std::runtime_error("Cannot commit native path detour; process must terminate");
        }
    }

    inline void Install(const Context& context, HMODULE image)
    {
        if (!context.enabled)
            return;
    #if defined(_M_IX86)
        if (image != reinterpret_cast<HMODULE>(image_base))
            throw std::runtime_error("Native path hook only supports the fixed x86 image base");
        // Match loader/Utils.cpp GetGameLaunchDirectory, not the assets directory.
        const fs::path             executable = context.Child(L"data") / L"GTA San Andreas" / L"gta_sa.exe";
        std::array<wchar_t, 32768> actual{};
        const DWORD                length = GetModuleFileNameW(image, actual.data(), static_cast<DWORD>(actual.size()));
        if (!length || length >= actual.size() || !Same(actual.data(), executable.native()) || !IsKnownExecutable(ReadExecutable(executable)))
            throw std::runtime_error("Native path hook executable is not the approved lab copy");
        RequireImageRegion(routine, expected_code.size(), false);
        if (std::memcmp(reinterpret_cast<void*>(routine), expected_code.data(), expected_code.size()))
            throw std::runtime_error("Native path hook expected instructions not present");
        for (const auto address : buffers)
        {
            RequireImageRegion(address, 256, true);
            const auto* begin = reinterpret_cast<const unsigned char*>(address);
            if (std::any_of(begin, begin + 256, [](unsigned char byte) { return byte != 0; }))
                throw std::runtime_error("Native path hook is too late: user buffers already initialized");
        }
        Paths paths = PreparePaths(context);
        CreateDirectories(paths);
        installed_paths = std::move(paths);
        Redirect(reinterpret_cast<void*>(routine), expected_code, reinterpret_cast<const void*>(&UserDirectory));
    #else
        (void)image;
        throw std::runtime_error("Native path hook requires x86");
    #endif
    }
}
#endif
