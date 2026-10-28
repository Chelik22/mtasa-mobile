#include <windows.h>
#include "FixtureContentPolicy.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace experimental_http;
void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
void write(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    require(file.good(), "fixture test file write");
}
std::span<const unsigned char> as_bytes(const std::string& bytes)
{
    return {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()};
}
int main()
{
    try
    {
        const auto base = std::filesystem::canonical(std::filesystem::temp_directory_path());
        const auto root = base / ("mta-g4-fixture-policy-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        require(!std::filesystem::exists(root) && std::filesystem::create_directory(root), "exclusive fixture test root");
        struct Cleanup
        {
            std::filesystem::path root, base;
            ~Cleanup()
            {
                std::error_code error;
                const auto      resolved = std::filesystem::weakly_canonical(root, error);
                if (!error && resolved.parent_path() == base && resolved.filename() == root.filename())
                    std::filesystem::remove_all(resolved, error);
            }
        } cleanup{root, base};
        const auto            temporary = root / "opaque-owned-temp.part";
        const std::string     url = "http://127.0.0.1:1/resource/client.lua";
        const std::string     lua = "return 1\n";
        const std::string     lua_hash = "0805bfdc02e872ed322a4a4e440ae985f3a4335f4dd59f3d373dfaf2a68a4a3c";
        const FixtureEntry    entry{"client.lua", url, lua_hash, 1024, FixtureFormat::LuaSourceUtf8};
        FixtureContentPolicy  policy(root, {entry});
        FileValidationContext context{temporary, root / "client.lua", url, url, 200, lua.size()};
        std::string           error;
        write(temporary, lua);
        require(policy(context, error) && error.empty(), "actual BCrypt SHA256/UTF8 source positive");
        write(root / "client.lua", "changed on disk after capture");
        require(policy.ValidateReadSnapshot(root / "client.lua", as_bytes(lua), error), "retained bytes must not reopen/hash a changed disk file");
        std::filesystem::remove(root / "client.lua");
        require(policy.ValidateReadSnapshot(root / "client.lua", as_bytes(lua), error), "snapshot remains valid after original file removal");
        require(!policy.ValidateReadSnapshot(root / "other.lua", as_bytes(lua), error), "snapshot path must match a manifest entry");
        const std::string wrong_snapshot = "return 2\n";
        require(!policy.ValidateReadSnapshot(root / "client.lua", as_bytes(wrong_snapshot), error), "changed retained bytes must fail raw digest");
        auto bad = context;
        bad.destination = root / "other.lua";
        require(!policy(bad, error), "mismatched destination binding");
        bad = context;
        bad.destination = base / "escape.lua";
        require(!policy(bad, error), "root escape must fail");
        bad = context;
        bad.url = "http://127.0.0.1:1/wrong";
        require(!policy(bad, error), "requested URL binding");
        bad = context;
        bad.effective_url = "http://127.0.0.1:1/redirected";
        require(!policy(bad, error), "effective URL binding");
        bad = context;
        bad.http_status = 404;
        require(!policy(bad, error), "modern HTTP success does not bypass fixture status policy");
        bad = context;
        --bad.bytes_received;
        require(!policy(bad, error), "actual file size must match transfer context");
        bad = context;
        bad.bytes_received = 1025;
        require(!policy(bad, error), "entry body bound");
        std::filesystem::create_directory(root / "other");
        write(root / "other/temp.part", lua);
        bad = context;
        bad.temporary = root / "other/temp.part";
        require(!policy(bad, error), "nonadjacent temporary binding");
        write(temporary, "return 2\n");
        require(!policy(context, error) && error.find("SHA256") != std::string::npos, "changed source hash is rejected");
        write(temporary, "return 1\r\n");
        context.bytes_received = 10;
        require(!policy(context, error), "raw hash cannot normalize CRLF to LF");
        auto crlf = entry;
        crlf.raw_sha256 = "4644671b1faf8551a0d6d6d3165298ceae48b759132598281b082dd985ec6237";
        require(FixtureContentPolicy(root, {crlf})(context, error), "matching checkout CRLF raw digest succeeds");
        auto compiled = entry;
        compiled.raw_sha256 = "fd5e1a168e4468e5cc3d46a993a4a9962ef69f931b3041b4210de50dfe6b573e";
        const std::string chunk = "\xef\xbb\xbf\x1bLua";
        write(temporary, chunk);
        context.bytes_received = chunk.size();
        require(!FixtureContentPolicy(root, {compiled})(context, error) && error.find("Compiled Lua") != std::string::npos,
                "matching hash cannot bypass BOM/compiled chunk rejection");
        require(!FixtureContentPolicy(root, {compiled}).ValidateReadSnapshot(root / "client.lua", as_bytes(chunk), error),
                "snapshot uses the same compiled-source rejection");
        auto native = entry;
        native.format = FixtureFormat::BinaryAsset;
        native.raw_sha256 = "78a83403fcb3b7a3fc1df1abbeb6dfad652c01e561efd9ba6985f7c75c44245a";
        write(temporary, "MZfixture");
        context.bytes_received = 9;
        require(!FixtureContentPolicy(root, {native})(context, error) && error.find("Native executable") != std::string::npos,
                "native executable fixture magic is rejected even with matching hash");
        auto binary = entry;
        binary.format = FixtureFormat::BinaryAsset;
        binary.raw_sha256 = "84cb49c57cb7a9c1e96119d49967fa3144bc4c610beb042a4ec66b9a3120497c";
        const std::string asset(
            "\x00\xff\x10"
            "abc",
            6);
        write(temporary, asset);
        context.bytes_received = asset.size();
        require(FixtureContentPolicy(root, {binary})(context, error), "binary assets are not constrained to source text");
        require(FixtureContentPolicy(root, {binary}).ValidateReadSnapshot(root / "client.lua", as_bytes(asset), error),
                "binary snapshot uses the same manifest/hash rules");
        binary.format = FixtureFormat::LuaSourceUtf8;
        require(!FixtureContentPolicy(root, {binary})(context, error), "source-specific NUL/UTF8 policy uses entry type");
        auto invalid_utf8 = entry;
        invalid_utf8.raw_sha256 = "4ce4b594686dc731d6b5c4da53e49eb5c107bb8bff7c3f7cb345882e37ed932d";
        write(temporary, std::string("--\xff", 3));
        context.bytes_received = 3;
        require(!FixtureContentPolicy(root, {invalid_utf8})(context, error), "matching hash cannot bypass source UTF8 requirement");
        auto empty = entry;
        empty.max_bytes = 0;
        empty.raw_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        write(temporary, "");
        context.bytes_received = 0;
        require(FixtureContentPolicy(root, {empty})(context, error), "empty file and zero maximum have precise byte semantics");
        bool duplicate = false;
        try
        {
            FixtureContentPolicy invalid(root, {entry, entry});
        }
        catch (const std::invalid_argument&)
        {
            duplicate = true;
        }
        require(duplicate, "duplicate manifest path rejected");
        auto traversal = entry;
        traversal.destination_relative = "../client.lua";
        bool escaped = false;
        try
        {
            FixtureContentPolicy invalid(root, {traversal});
        }
        catch (const std::invalid_argument&)
        {
            escaped = true;
        }
        require(escaped, "manifest traversal rejected before file validation");
        for (const auto& alias : {"client.lua.", "client.lua ", "folder./client.lua", "folder /client.lua", "bad?.lua", "bad*.lua", "bad<.lua", "bad|.lua",
                                  "NUL.txt", "con.lua", "COM1.dat"})
        {
            auto invalid = entry;
            invalid.destination_relative = alias;
            bool rejected = false;
            try
            {
                FixtureContentPolicy invalid_policy(root, {invalid});
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            require(rejected, "nonportable Windows alias rejected in each segment");
        }
        auto upper = entry;
        upper.destination_relative = "Client.lua";
        bool folded_duplicate = false;
        try
        {
            FixtureContentPolicy invalid(root, {entry, upper});
        }
        catch (const std::invalid_argument&)
        {
            folded_duplicate = true;
        }
        require(folded_duplicate, "Windows case-insensitive duplicate rejected");
        std::cout << "PASS actual Win32 fixture manifest path/URL/raw SHA256/format policy; engine checksum/private netc/G4 NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
