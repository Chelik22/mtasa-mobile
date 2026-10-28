#include <ExperimentalNativeUserPaths.h>
#include <fstream>
#include <iostream>

namespace paths = experimental_lab::native_paths;
void Check(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
template <typename Function>
void Reject(Function function)
{
    bool rejected = false;
    try
    {
        function();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Check(rejected, "negative native contract accepted");
}

void Canonical(const wchar_t* path)
{
    auto original = paths::ReadExecutable(path);
    Check(paths::Sha256(original) == paths::canonical_hash, "fixture must be real canonical GTA bytes, read as data only");
    Check(paths::IsKnownExecutable(original), "canonical executable rejected");
    auto patched = original;
    patched[0x96] |= 0x20;
    patched[0xdf] |= 0x01;
    patched[0x8b] = 0x43;
    std::fill_n(patched.data() + 0xd8, 4, 0xff);
    std::memcpy(patched.data() + 0x4a0c8a, "mtasa.dll", 10);
    std::fill_n(patched.data() + 0xf8, 8, 0xff);
    std::fill_n(patched.data() + 0x4a1ab0, 144, 0xff);
    std::fill_n(patched.data() + 0x4e1b00, 256, 0xff);
    Check(paths::IsKnownExecutable(patched), "supported loader patch normalization rejected");
    std::memcpy(patched.data() + 0x4a0c8a, "mtasa_d.dll", 12);
    Check(paths::IsKnownExecutable(patched), "debug proxy name normalization rejected");
    patched[0x1000] ^= 1;
    Check(!paths::IsKnownExecutable(patched), "unknown code mutation bypassed full SHA");
    original.resize(40);
    Check(!paths::IsKnownExecutable(original), "truncated image accepted");
    std::cout << "PASS canonical/raw+6-source-patch normalization; unknown mutation rejected; GTA execution not_run\n";
}

void Filesystem()
{
    const auto& context = experimental_lab::GetContext();
    Check(context.enabled, "paths test requires private lab root");
    paths::Install(experimental_lab::Context{}, nullptr);
    const auto value = paths::PreparePaths(context);
    Check(!std::filesystem::exists(value.wide[0]), "prepare must have no filesystem writes");
    Check(paths::AnsiPath(std::wstring(255, L'a')).size() == 255, "256-byte NUL-inclusive bound rejected");
    Reject([] { paths::AnsiPath(std::wstring(256, L'a')); });
    const std::wstring unpaired(1, static_cast<wchar_t>(0xd800));
    Reject([&] { paths::AnsiPath(unpaired); });
    paths::CreateDirectories(value);
    std::array<std::array<char, 258>, 3> storage{};
    std::array<char*, 3>                 destination{};
    for (std::size_t i = 0; i < storage.size(); ++i)
    {
        storage[i].fill('!');
        destination[i] = storage[i].data() + 1;
    }
    paths::Publish(value, destination);
    for (std::size_t i = 0; i < storage.size(); ++i)
    {
        Check(storage[i].front() == '!' && storage[i].back() == '!', "native buffer boundary overwritten");
        Check(!std::memcmp(destination[i], value.bytes[i].data(), 256), "published native bytes differ");
    }
    const auto before = storage;
    auto       invalid = value;
    invalid.wide[2] /= L"missing";
    Reject([&] { paths::Publish(invalid, destination); });
    Check(storage == before, "late invalid directory caused partial buffer publish");
    std::filesystem::remove(value.wide[1]);
    {
        std::ofstream blocker(value.wide[1]);
        blocker << "regular file is not directory";
    }
    Reject([&] { paths::PreparePaths(context); });
    Reject([&] { paths::Publish(value, destination); });
    Check(storage == before, "regular file replacement caused partial publish");
    std::cout << "PASS private native paths/ACP/bounds/all-or-none publish; no GTA/registry\n";
}

char* __cdecl AfterRedirect()
{
    return const_cast<char*>("after");
}
void Redirect()
{
#if defined(_M_IX86)
    // Execute only this tiny owned test function in private VirtualAlloc memory.
    // No game instructions, image or DLL are executed by this regression.
    auto* page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(page != nullptr, "test allocation failed");
    try
    {
        std::array<unsigned char, 16> code{};
        code.fill(0x90);
        code[0] = 0xb8;
        const auto before = reinterpret_cast<std::uint32_t>("before");
        std::memcpy(code.data() + 1, &before, 4);
        code[5] = 0xc3;
        std::memcpy(page, code.data(), code.size());
        DWORD old = 0;
        Check(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old) != FALSE, "test execute protection failed");
        FlushInstructionCache(GetCurrentProcess(), page, code.size());
        auto function = reinterpret_cast<char*(__cdecl*)()>(page);
        Check(!std::strcmp(function(), "before"), "test function initial ABI wrong");
        auto wrong = code;
        wrong[15] ^= 1;
        Reject([&] { paths::Redirect(page, wrong, reinterpret_cast<const void*>(&AfterRedirect)); });
        Check(!std::memcmp(page, code.data(), code.size()), "mismatched expected bytes changed code");
        paths::Redirect(page, code, reinterpret_cast<const void*>(&AfterRedirect));
        Check(!std::strcmp(function(), "after"), "owned native detour did not redirect actual call");
        MEMORY_BASIC_INFORMATION info{};
        Check(VirtualQuery(page, &info, sizeof(info)) && info.Protect == PAGE_EXECUTE_READ, "detour did not restore executable protection");
    }
    catch (...)
    {
        VirtualFree(page, 0, MEM_RELEASE);
        throw;
    }
    VirtualFree(page, 0, MEM_RELEASE);
    std::cout << "PASS real owned x86 detour/expected-bytes refusal/protection restored; no GTA\n";
#else
    throw std::runtime_error("regression requires x86");
#endif
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        Check(argc >= 2, "missing mode");
        const std::wstring mode = argv[1];
        if (mode == L"canonical" && argc == 3)
            Canonical(argv[2]);
        else if (mode == L"paths")
            Filesystem();
        else if (mode == L"redirect")
            Redirect();
        else
            throw std::runtime_error("invalid mode");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
