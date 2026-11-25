// Serves an extracted disc tree through the runtime's sector-addressed disc interface.
// tools/extract_disc.py writes the manifest this reads; see docs/milestones/M03.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/ps2_disc_image.h"

// M10-09: the first file a read failed on after every retry (true), for the core to stop with it named
bool folderDiscUnreadable(std::string &path);

class FolderDisc final : public IPS2DiscImage
{
public:
    static constexpr uint32_t kSectorSize = 2048;
    static constexpr size_t kMaxOpenFiles = 8;

    // Null on failure, with a message naming the file or record that is wrong. root is
    // joined as text and passed to fopen, so it may be device-prefixed.
    static std::unique_ptr<FolderDisc> load(const std::string &root, std::string &error);
    ~FolderDisc() override;

    bool findFile(std::string_view ps2Path, uint32_t &lsn, uint32_t &sizeBytes, bool &isDirectory) const override;
    bool readBytes(uint64_t offset, size_t byteCount, void *destination) const override;
    uint32_t totalSectors() const override { return m_totalSectors; }
    bool listDirectory(std::string_view ps2Path, std::vector<DirEntry> &out) const override;

    // Upper-case key with no device, version or empty components. False for paths that
    // could escape the disc tree or are otherwise not a disc path.
    static bool normalize(std::string_view ps2Path, std::string &out);

    // Files replaced from root/mods/<disc path>, and the replacements refused at load.
    size_t modsApplied() const { return m_modsApplied; }
    const std::vector<std::string> &warnings() const { return m_warnings; }

    // Hashes every disc file (the disc copy, not a mod replacing it) and raw.bin against the
    // manifest's SHA-256 and returns one message per file that is unreadable, differs or has
    // no recorded hash; empty when the tree is intact. Load checks sizes only, so a same-size
    // corrupt file is found here. Reads the whole tree, so it is an explicit check.
    std::vector<std::string> verifyFiles() const;

    // Lower-case hex SHA-256 of a buffer.
    static std::string sha256Hex(const void *data, size_t size);

private:
    struct Entry
    {
        uint32_t lsn = 0;
        uint32_t sizeBytes = 0;
        bool isDirectory = false;
    };
    struct Extent
    {
        uint64_t begin = 0;
        uint64_t end = 0;     // sector-aligned, includes the zero padding after a file
        uint32_t file = 0;    // index into m_files, or kRawExtent
        uint64_t source = 0;  // byte offset of begin inside m_raw for raw extents
        uint64_t fileEnd = 0; // disc offset where the file's own bytes stop
    };
    struct File
    {
        std::string hostPath; // the mod's path when a mod replaces the file
        std::string discPath;
        std::string sha256;   // lower-case hex from the manifest, empty when not recorded
    };
    struct Slot
    {
        std::FILE *handle = nullptr;
        uint32_t file = 0;
        uint64_t lastUse = 0;
        bool busy = false;
    };
    static constexpr uint32_t kRawExtent = 0xffffffffu;

    FolderDisc() = default;
    bool parseManifest(const std::string &root, std::string &error);
    bool finishExtents(const std::string &root, std::string &error);
    Slot *acquire(uint32_t file) const;
    void release(Slot *slot) const;
    bool readFile(uint32_t file, uint64_t offset, size_t count, uint8_t *destination) const;

    uint32_t m_totalSectors = 0;
    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_map<std::string, std::vector<DirEntry>> m_children;
    std::vector<File> m_files;
    std::vector<Extent> m_extents; // sorted by begin, non-overlapping
    std::vector<uint8_t> m_raw;
    std::string m_rawPath;
    std::string m_rawSha256;
    size_t m_modsApplied = 0;
    std::vector<std::string> m_warnings;

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_released;
    mutable Slot m_slots[kMaxOpenFiles];
    mutable uint64_t m_clock = 0;
};
