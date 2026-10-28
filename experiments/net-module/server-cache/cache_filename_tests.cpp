#include "CacheFilenamePolicy.h"
#include <chrono>
#include <fstream>
#include <iostream>

using experimental_server_cache::CacheFilenamePolicy;
namespace fs = std::filesystem;
namespace
{
    void Check(bool value, const char* why)
    {
        if (!value)
            throw std::runtime_error(why);
    }
    std::string Utf8(const fs::path& path)
    {
        const auto text = path.generic_u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }
    struct Temporary
    {
        fs::path parent = fs::canonical(fs::temp_directory_path());
        fs::path root = parent / ("mta-server-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Temporary() { Check(fs::create_directory(root), "exclusive test directory"); }
        ~Temporary()
        {
            // Only this freshly created immediate child can ever be recursively cleaned up.
            if (root.parent_path() == parent && root.filename().string().starts_with("mta-server-cache-"))
            {
                std::error_code ignored;
                fs::remove_all(root, ignored);
            }
        }
    };
    void SetRoot(const std::optional<std::string>& root)
    {
#ifdef _WIN32
        if (root)
        {
            const auto path = fs::u8path(*root);
            Check(SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_SERVER_HTTP_CACHE_ROOT", path.c_str()) != FALSE, "set own process env");
        }
        else
        {
            const bool removed = SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_SERVER_HTTP_CACHE_ROOT", nullptr) != FALSE;
            Check(removed || GetLastError() == ERROR_ENVVAR_NOT_FOUND, "clear own process env");
        }
#else
        Check(root ? setenv(experimental_server_cache::environment, root->c_str(), 1) == 0 : unsetenv(experimental_server_cache::environment) == 0,
              "own process env");
#endif
    }
    void Contracts()
    {
        Temporary  temp;
        const auto cache = temp.root / "resource-cache";
        const auto outside = temp.root / "resource-cache-other";
        fs::create_directory(cache);
        fs::create_directory(outside);
        CacheFilenamePolicy policy(Utf8(cache));
        Check(policy.Ready(), policy.Error().c_str());
        Check(!CacheFilenamePolicy{}.Allows(Utf8(cache / "http-client-files/mobile-world-fixture/http-witness.txt")), "absent config accepted");
        Check(!CacheFilenamePolicy(Utf8(temp.root / "missing")).Ready(), "missing root accepted");
        Check(!CacheFilenamePolicy("relative/cache").Ready(), "relative root accepted");
        Check(!CacheFilenamePolicy(Utf8(cache) + "/../resource-cache").Ready(), "traversal root accepted");
        for (auto key : experimental_server_cache::fixture_files)
        {
            const auto file = cache / fs::u8path(key);
            Check(!fs::exists(file), "positive fixture unexpectedly exists");
            Check(policy.Allows(Utf8(file)), "pre-copy missing destination rejected");
            Check(!fs::exists(file.parent_path()), "validator created directories");
            fs::create_directories(file.parent_path());
            std::ofstream(file) << "MZ filename policy is not content approval";
            Check(policy.Allows(Utf8(file)), "regular allowed filename rejected");
        }
        const auto allowed = cache / "http-client-files/mobile-world-fixture/http-witness.txt";
        for (const auto& bad : std::array<std::string, 12>{
                 Utf8(outside / "http-client-files/mobile-world-fixture/http-witness.txt"), Utf8(cache / "http-client-files/mobile-world-fixture/client.lua"),
                 Utf8(cache / "http-client-files-no-client-cache/mobile-world-fixture/http-witness.txt"),
                 Utf8(cache / "http-client-files/mobile-world-fixture/HTTP-WITNESS.TXT"), Utf8(cache / "http-client-files/mobile-world-fixture/other.txt"),
                 Utf8(cache) + "/./http-client-files/mobile-world-fixture/http-witness.txt",
                 Utf8(cache) + "//http-client-files/mobile-world-fixture/http-witness.txt", Utf8(allowed) + "/", Utf8(allowed) + ":stream", Utf8(allowed) + ".",
                 Utf8(allowed) + " ", "http-client-files/mobile-world-fixture/http-witness.txt"})
            Check(!policy.Allows(bad), "outside/alias/unlisted candidate accepted");
        Check(!policy.Allows(static_cast<const char*>(nullptr)), "null accepted");
        Check(!policy.Allows(std::string(4097, 'a')), "oversized candidate accepted");
        Check(!policy.Allows(Utf8(cache) + "/\xc0\xaf"), "invalid UTF8 accepted");
        for (const auto bad : {"../file", "a/./file", "a/../file", "a//file", "a/file.", "a/file ", "a/NUL.txt", "a/COM1", "a/PROGRA~1", "a/file:stream"})
        {
            const std::array<std::string_view, 1> entry{bad};
            Check(!CacheFilenamePolicy(Utf8(cache), entry).Ready(), "unsafe allowlist entry accepted");
        }
        const std::array<std::string_view, 2> aliases{"a/file", "A/FILE"};
        Check(!CacheFilenamePolicy(Utf8(cache), aliases).Ready(), "case duplicate entries accepted");
        const auto unicode = temp.root / fs::u8path("\xd0\xba\xd1\x8d\xd1\x88");
        fs::create_directory(unicode);
        CacheFilenamePolicy utf8(Utf8(unicode));
        Check(utf8.Ready() && utf8.Allows(Utf8(unicode / "http-client-files/mobile-world-fixture/http-witness.txt")), "UTF8 root misread as ACP");
        SetRoot(std::nullopt);
        auto absent = CacheFilenamePolicy::FromEnvironment();
        Check(!absent.Ready(), "absent env enabled policy");
        SetRoot(Utf8(cache));
        auto frozen = CacheFilenamePolicy::FromEnvironment();
        SetRoot(Utf8(outside));
        Check(frozen.Ready() && frozen.Allows(Utf8(allowed)), "captured root changed with env");
        Check(!frozen.Allows(Utf8(outside / "http-client-files/mobile-world-fixture/http-witness.txt")), "changed env widened existing policy");
        fs::remove(allowed);
        fs::create_directory(allowed);
        Check(!policy.Allows(Utf8(allowed)), "directory final target accepted");
        fs::remove_all(cache / "http-client-files");
        std::ofstream(cache / "http-client-files") << "ancestor file";
        Check(!policy.Allows(Utf8(allowed)), "regular file ancestor accepted");
        SetRoot(std::nullopt);
        std::cout << "PASS cache filename/root/allowlist/UTF8/missing target/env freeze contracts; content/game NOT RUN\n";
    }
    int Links()
    {
        Temporary  temp;
        const auto cache = temp.root / "resource-cache";
        const auto outside = temp.root / "outside";
        fs::create_directory(cache);
        fs::create_directory(outside);
        CacheFilenamePolicy policy(Utf8(cache));
        std::error_code     error;
        fs::create_directory_symlink(outside, cache / "http-client-files", error);
        if (error)
        {
            std::cout << "SKIP native symlink fixture unavailable: " << error.message() << '\n';
            return 77;
        }
        Check(!policy.Allows(Utf8(cache / "http-client-files/mobile-world-fixture/http-witness.txt")), "symlink ancestor accepted");
        Check(!CacheFilenamePolicy(Utf8(cache / "http-client-files")).Ready(), "symlink root accepted");
        fs::remove(cache / "http-client-files");
        fs::create_directories(cache / "http-client-files/mobile-world-fixture");
        std::ofstream(outside / "file") << "outside";
        fs::create_symlink(outside / "file", cache / "http-client-files/mobile-world-fixture/http-witness.txt");
        Check(!policy.Allows(Utf8(cache / "http-client-files/mobile-world-fixture/http-witness.txt")), "symlink final target accepted");
        fs::remove(cache / "http-client-files/mobile-world-fixture/http-witness.txt");
        std::cout << "PASS cache root/ancestor/final symlink rejection; no writes outside test temp\n";
        return 0;
    }
}  // namespace
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "links")
            return Links();
        Contracts();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
