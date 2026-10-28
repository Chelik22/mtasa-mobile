#pragma once

#if defined(_WIN32) && defined(MTA_CLIENT)
    #include "ExperimentalLabPaths.h"
    #include <bcrypt.h>
    #include <cstring>
    #include <limits>
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

    inline void NormalizeKnownPatches(std::vector<unsigned char>& bytes)
    {
        // Exact offsets are from loader/GameExecutablePatcher.cpp at a84e0c77.
        // Restore only its six supported patches; full SHA rejects every other
        // change. Do not accept a prefix hash or silently normalize code bytes.
        if (bytes.size() != executable_size || bytes[0] != 'M' || bytes[1] != 'Z' || std::memcmp(bytes.data() + 0x80, "PE\0\0\x4c\x01", 6) ||
            bytes[0x98] != 0x0b || bytes[0x99] != 0x01)
            throw std::runtime_error("Native executable is outside fixed PE32 contract");
        bytes[0x96] &= ~0x20;  // IMAGE_FILE_LARGE_ADDRESS_AWARE
        bytes[0xdf] &= ~0x01;  // IMAGE_DLLCHARACTERISTICS_NX_COMPAT
        constexpr unsigned char timestamp[]{0xca, 0x01, 0x71, 0x42};
        constexpr unsigned char checksum[]{0x89, 0xd6, 0xdb, 0x00};
        std::memcpy(bytes.data() + 0x88, timestamp, sizeof(timestamp));
        std::memcpy(bytes.data() + 0xd8, checksum, sizeof(checksum));
        auto* library = bytes.data() + 0x4a0c8a;
        if (!std::memcmp(library, "mtasa.dll\0", 10) || !std::memcmp(library, "mtasa_d.dll\0", 12))
            std::memcpy(library, "WINMM.dll\0\x02", 11);
        std::fill_n(bytes.data() + 0xf8, 8, 0);  // Export directory RVA/size.
        std::fill_n(bytes.data() + 0x4a1ab0, 144, 0);
        std::fill_n(bytes.data() + 0x4e1b00, 256, 0);
    }

    inline bool IsKnownExecutable(std::vector<unsigned char> bytes)
    {
        if (bytes.size() != executable_size)
            return false;
        if (Sha256(bytes) == canonical_hash)
            return true;
        NormalizeKnownPatches(bytes);
        return Sha256(bytes) == canonical_hash;
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
