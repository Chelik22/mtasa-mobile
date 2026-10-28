#include "ApprovedReadFile.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
int main()
{
    const auto directory =
        std::filesystem::temp_directory_path() / ("mta-approved-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "asset.bin";
    try
    {
        require(std::filesystem::create_directory(directory), "create isolated test directory");
        struct Cleanup
        {
            std::filesystem::path path, directory;
            ~Cleanup()
            {
                std::error_code error;
                std::filesystem::remove(path, error);
                std::filesystem::remove(directory, error);
            }
        } cleanup{path, directory};
        const std::string approved("a\0b", 3);
        {
            std::ofstream file(path, std::ios::binary);
            file.write(approved.data(), approved.size());
        }
        const auto                            name = path.u8string();
        const std::string                     filename(name.begin(), name.end());
        experimental_client::ApprovedReadFile reader(
            [&](const auto& actual_path, std::span<const unsigned char> bytes)
            {
                return actual_path == std::filesystem::canonical(path) && bytes.size() == approved.size() &&
                       std::memcmp(bytes.data(), approved.data(), bytes.size()) == 0;
            });
        require(!reader.FOpen(filename.c_str(), "wb+", true), "write mode must fail before truncation");
        require(std::filesystem::file_size(path) == 3, "rejected write leaves existing resource intact");
        require(!reader.FOpen(filename.c_str(), "rb", false), "validation cannot be bypassed");
        require(reader.FOpen(filename.c_str(), "rb", true), "approved actual resource opens");
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "replacement";
        }
        char data[4]{};
        require(reader.FRead(data, sizeof(data)) == 3 && std::memcmp(data, approved.data(), 3) == 0, "read retains approved bytes after path replacement");
        require(reader.FEof(), "short read records EOF");
        reader.FSeek(0, SEEK_SET);
        require(!reader.FEof() && reader.FTell() == 0, "seek clears EOF");
        reader.FSeek(0, SEEK_END);
        require(reader.FTell() == 3, "SDK file-size seek sees snapshot length");
        require(!reader.FOpen(filename.c_str(), "rb", true), "replacement fails content policy on reopen");
        require(reader.FTell() == -1 && reader.FEof(), "failed reopen closes old snapshot");
        experimental_client::ApprovedReadFile without_policy({});
        require(!without_policy.FOpen(filename.c_str(), "rb", true), "missing policy never accepts content");
        std::cout << "PASS approved SDK file snapshot; CNet/game/Lua integration NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
