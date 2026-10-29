#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif
#include "AndroidSelfMemory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

// Reuse the repository implementation; no crypto or package dependency is added.
#include "sha2.hpp"

namespace mobile::position
{
    namespace
    {
        constexpr off_t                         kFileBytes = 8943088;
        constexpr std::array<unsigned char, 32> kDigest = {0x4c, 0x6a, 0x74, 0x45, 0xe3, 0x0b, 0x27, 0xaf, 0xdd, 0xa7, 0x81, 0x30, 0x2e, 0x4d, 0xb9, 0xba,
                                                           0xc8, 0x9c, 0x28, 0xfc, 0x11, 0x81, 0xb6, 0x8b, 0x1e, 0xef, 0x16, 0xf8, 0x4d, 0x6a, 0x28, 0x2e};

        struct Loaded
        {
            std::string   path;
            std::uint64_t bias{};
            unsigned      count{};
            bool          zeroLoad{};
        };

        int FindModule(dl_phdr_info* info, std::size_t, void* context)
        {
            const std::string_view path(info->dlpi_name ? info->dlpi_name : "");
            if (path.substr(path.find_last_of('/') + 1) != "libGame.so")
                return 0;
            auto& result = *static_cast<Loaded*>(context);
            ++result.count;
            result.path = path;
            result.bias = info->dlpi_addr;
            for (unsigned i = 0; i < info->dlpi_phnum; ++i)
            {
                const auto& phdr = info->dlpi_phdr[i];
                if (phdr.p_type == PT_LOAD && phdr.p_offset == 0 && phdr.p_vaddr == 0 && phdr.p_filesz >= sizeof(Elf64_Ehdr))
                    result.zeroLoad = true;
            }
            return 0;
        }

        bool SameFile(const struct stat& a, const struct stat& b)
        {
            return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
                   a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
        }

        bool Copy(std::uint64_t address, void* output, std::size_t bytes)
        {
            if (!output || bytes == 0 || bytes > 4096 || address < 4096 || address >= (std::uint64_t{1} << 56) || bytes > (std::uint64_t{1} << 56) - address)
                return false;
            iovec local{output, bytes};
            iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), bytes};
            // The kernel reports an inaccessible/removed page instead of dereferencing it
            // in this process. SELinux/seccomp/ENOSYS and partial copies fail closed.
            return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
        }

        bool Mapping(std::uint64_t bias, std::uint64_t device, std::uint64_t inode)
        {
            FILE* maps = std::fopen("/proc/self/maps", "re");
            if (!maps)
                return false;
            char line[4096];
            bool found = false;
            while (std::fgets(line, sizeof(line), maps))
            {
                unsigned long long begin, end, offset, actualInode;
                unsigned           majorDevice, minorDevice;
                char               permissions[5]{};
                if (std::sscanf(line, "%llx-%llx %4s %llx %x:%x %llu", &begin, &end, permissions, &offset, &majorDevice, &minorDevice, &actualInode) == 7 &&
                    begin == bias && end > begin && end - begin >= sizeof(Elf64_Ehdr) && offset == 0 && permissions[0] == 'r' && actualInode == inode &&
                    static_cast<std::uint64_t>(makedev(majorDevice, minorDevice)) == device)
                    found = true;
            }
            const bool complete = std::feof(maps) && !std::ferror(maps);
            std::fclose(maps);
            return found && complete;
        }

        bool CodeAnchor(int fd, std::uint64_t bias)
        {
            // Compare against the already fingerprinted file, not copied game bytes in Git.
            constexpr std::uint64_t rva = 0x4af7d4;
            constexpr std::size_t   length = 68;
            Elf64_Ehdr              header{};
            if (pread(fd, &header, sizeof(header), 0) != sizeof(header) || header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
                header.e_phnum > 128 || header.e_phoff > kFileBytes || std::uint64_t{header.e_phnum} * sizeof(Elf64_Phdr) > kFileBytes - header.e_phoff)
                return false;
            for (unsigned i = 0; i < header.e_phnum; ++i)
            {
                Elf64_Phdr phdr{};
                if (pread(fd, &phdr, sizeof(phdr), header.e_phoff + i * sizeof(phdr)) != sizeof(phdr))
                    return false;
                if (phdr.p_type != PT_LOAD || !(phdr.p_flags & PF_X) || rva < phdr.p_vaddr || rva - phdr.p_vaddr > phdr.p_filesz ||
                    length > phdr.p_filesz - (rva - phdr.p_vaddr))
                    continue;
                const std::uint64_t delta = rva - phdr.p_vaddr;
                if (phdr.p_offset > kFileBytes || delta > kFileBytes - phdr.p_offset || length > kFileBytes - phdr.p_offset - delta ||
                    bias >= (std::uint64_t{1} << 56) - rva)
                    return false;
                std::array<unsigned char, length> file{}, live{};
                return pread(fd, file.data(), length, phdr.p_offset + delta) == length && Copy(bias + rva, live.data(), length) && file == live;
            }
            return false;
        }
    }

    AndroidModule::AndroidModule(int fd, std::uint64_t bias, std::uint64_t device, std::uint64_t inode) : fd_(fd), bias_(bias), device_(device), inode_(inode)
    {
    }

    AndroidModule::~AndroidModule()
    {
        close(fd_);
    }

    std::unique_ptr<AndroidModule> AndroidModule::Open(std::string& reason)
    {
        reason.clear();
#if !defined(__ANDROID__) || !defined(__aarch64__)
        reason = "requires Android ARM64; translated process ABI must still be ARM64";
        return {};
#else
        static_assert(sizeof(void*) == 8 && sizeof(float) == 4);
        Loaded loaded;
        dl_iterate_phdr(FindModule, &loaded);
        if (loaded.count != 1 || !loaded.bias || !loaded.zeroLoad)
        {
            reason = "unique zero-based ELF64 libGame mapping unavailable";
            return {};
        }
        // APK-backed, deleted, relative and linker synthetic names need a distinct
        // verifier. Never hash a similarly named extracted copy as their identity.
        if (loaded.path.empty() || loaded.path.front() != '/' || loaded.path.find('!') != std::string::npos)
        {
            reason = "packed or non-file-backed library mapping unsupported";
            return {};
        }
        const int fd = open(loaded.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        auto      fail = [&](const char* message) -> std::unique_ptr<AndroidModule>
        {
            if (fd >= 0)
                close(fd);
            reason = message;
            return {};
        };
        struct stat before
        {
        }, after{};
        if (fd < 0 || fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size != kFileBytes || !Mapping(loaded.bias, before.st_dev, before.st_ino))
            return fail("library inode/size/mapping does not match open file");
        Elf64_Ehdr header{}, liveHeader{};
        if (pread(fd, &header, sizeof(header), 0) != sizeof(header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
            header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_AARCH64 || header.e_type != ET_DYN ||
            !Copy(loaded.bias, &liveHeader, sizeof(liveHeader)) || std::memcmp(&header, &liveHeader, sizeof(header)))
            return fail("file/live ELF64 AArch64 identity mismatch or unreadable");
        sha256_ctx hash;
        sha256_init(&hash);
        std::array<unsigned char, 32768> bytes;
        for (off_t offset = 0; offset < kFileBytes;)
        {
            const auto count = static_cast<std::size_t>(std::min<off_t>(bytes.size(), kFileBytes - offset));
            if (pread(fd, bytes.data(), count, offset) != static_cast<ssize_t>(count))
                return fail("incomplete library fingerprint read");
            sha256_update(&hash, bytes.data(), static_cast<unsigned>(count));
            offset += count;
        }
        std::array<unsigned char, 32> digest;
        sha256_final(&hash, digest.data());
        if (digest != kDigest || fstat(fd, &after) || !SameFile(before, after))
            return fail("pinned fingerprint mismatch or file changed");
        auto result = std::unique_ptr<AndroidModule>(new AndroidModule(fd, loaded.bias, before.st_dev, before.st_ino));
        result->modifiedSeconds_ = before.st_mtim.tv_sec;
        result->modifiedNanos_ = before.st_mtim.tv_nsec;
        result->changedSeconds_ = before.st_ctim.tv_sec;
        result->changedNanos_ = before.st_ctim.tv_nsec;
        if (!result->MappingStillPresent())
        {
            reason = "module mapping or code changed during verification";
            return {};
        }
        return result;
#endif
    }

    bool AndroidModule::MappingStillPresent() const
    {
        struct stat status
        {
        };
        return fstat(fd_, &status) == 0 && status.st_size == kFileBytes && static_cast<std::uint64_t>(status.st_dev) == device_ &&
               static_cast<std::uint64_t>(status.st_ino) == inode_ && status.st_mtim.tv_sec == modifiedSeconds_ && status.st_mtim.tv_nsec == modifiedNanos_ &&
               status.st_ctim.tv_sec == changedSeconds_ && status.st_ctim.tv_nsec == changedNanos_ && Mapping(bias_, device_, inode_) && CodeAnchor(fd_, bias_);
    }

    bool AndroidModule::Read(std::uint64_t address, void* output, std::size_t bytes) const
    {
        return Copy(address, output, bytes);
    }

    Result AndroidModule::ReadUnverifiedCandidate(Candidate& output) const
    {
        output = {};
        if (!MappingStillPresent())
            return Result::invalid_binding;
        Candidate    candidate;
        const Result result = ReadCandidate(*this, bias_, candidate);
        if (!MappingStillPresent())
            return Result::invalid_binding;
        if (result == Result::candidate_unverified)
            output = candidate;
        return result;
    }
}
