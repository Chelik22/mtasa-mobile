#pragma once

#include "PositionSnapshot.h"
#include <memory>
#include <string>

namespace mobile::position
{
    // No public constructor or 'verified' boolean. Opening binds an actual loaded
    // ELF mapping to an inode and a complete file fingerprint in this process.
    class AndroidModule final : private Memory
    {
    public:
        static std::unique_ptr<AndroidModule> Open(std::string& reason);
        ~AndroidModule();
        AndroidModule(const AndroidModule&) = delete;
        AndroidModule& operator=(const AndroidModule&) = delete;
        AndroidModule(AndroidModule&&) = delete;
        AndroidModule& operator=(AndroidModule&&) = delete;
        Result         ReadUnverifiedCandidate(Candidate& output) const;

    private:
        AndroidModule(int fd, std::uint64_t bias, std::uint64_t device, std::uint64_t inode);
        bool          Read(std::uint64_t address, void* output, std::size_t bytes) const override;
        bool          MappingStillPresent() const;
        int           fd_;
        std::uint64_t bias_, device_, inode_;
        std::int64_t  modifiedSeconds_{}, modifiedNanos_{}, changedSeconds_{}, changedNanos_{};
    };
}
