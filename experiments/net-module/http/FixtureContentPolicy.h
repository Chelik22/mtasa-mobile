#pragma once
#include "HttpManager.h"
#include <map>
#include <vector>
#include <span>

namespace experimental_http
{
    enum class FixtureFormat
    {
        LuaSourceUtf8,
        BinaryAsset
    };
    struct FixtureEntry
    {
        std::string   destination_relative;
        std::string   url;
        std::string   raw_sha256;
        size_t        max_bytes = 0;
        FixtureFormat format = FixtureFormat::BinaryAsset;
    };
    // A caller-owned, trusted fixture manifest. This does not reproduce private
    // netc content checks or turn a server-supplied digest into a trust anchor.
    class FixtureContentPolicy
    {
    public:
        FixtureContentPolicy(std::filesystem::path cache_root, std::vector<FixtureEntry> entries);
        bool operator()(const FileValidationContext&, std::string& error) const;
        // The caller already captured canonical identity and retained these exact
        // bytes. No filesystem reopen/HTTP context is involved in this check.
        bool ValidateReadSnapshot(const std::filesystem::path& canonical_path, std::span<const unsigned char>, std::string& error) const;

    private:
        std::filesystem::path               root_;
        std::map<std::string, FixtureEntry> entries_;
        const FixtureEntry*                 entry_for_path(const std::filesystem::path&, std::string& error) const;
    };
}
