#include "folder_disc.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <sys/stat.h>

namespace
{
    constexpr uint64_t kMaxRawBytes = 16u * 1024u * 1024u;

    bool parseNumber(std::string_view text, uint64_t &out)
    {
        const char *end = text.data() + text.size();
        const auto result = std::from_chars(text.data(), end, out);
        return !text.empty() && result.ec == std::errc{} && result.ptr == end;
    }

    std::vector<std::string_view> splitTabs(std::string_view line)
    {
        std::vector<std::string_view> fields;
        size_t start = 0;
        while (true)
        {
            const size_t tab = line.find('\t', start);
            if (tab == std::string_view::npos)
            {
                fields.push_back(line.substr(start));
                return fields;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
    }

    bool readWholeFile(const std::string &path, uint64_t limit, std::vector<uint8_t> &out)
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            return false;
        }
        out.clear();
        uint8_t buffer[16384];
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        {
            if (out.size() + count > limit)
            {
                std::fclose(file);
                return false;
            }
            out.insert(out.end(), buffer, buffer + count);
        }
        const bool ok = std::ferror(file) == 0;
        std::fclose(file);
        return ok;
    }

    bool seekTo(std::FILE *file, uint64_t offset)
    {
#if defined(_WIN32)
        return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
        return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }

    std::string parentOf(const std::string &path)
    {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    std::string leafOf(const std::string &path)
    {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    bool isSha256Hex(std::string_view text)
    {
        return text.size() == 64 &&
               std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    // FIPS 180-4 SHA-256, portable so the same code runs in the host tests and on the Switch.
    class Sha256
    {
    public:
        void update(const uint8_t *data, size_t size)
        {
            m_bytes += size;
            while (size != 0)
            {
                const size_t count = std::min(size, sizeof(m_buffer) - m_used);
                std::memcpy(m_buffer + m_used, data, count);
                m_used += count;
                data += count;
                size -= count;
                if (m_used == sizeof(m_buffer))
                {
                    block(m_buffer);
                    m_used = 0;
                }
            }
        }

        std::string finishHex()
        {
            const uint64_t bits = m_bytes * 8;
            const uint8_t one = 0x80, zero = 0;
            update(&one, 1);
            while (m_used != 56)
            {
                update(&zero, 1);
            }
            uint8_t length[8];
            for (int i = 0; i < 8; ++i)
            {
                length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
            }
            update(length, sizeof(length));
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string text;
            for (const uint32_t word : m_state)
            {
                for (int shift = 28; shift >= 0; shift -= 4)
                {
                    text.push_back(kDigits[(word >> shift) & 0xf]);
                }
            }
            return text;
        }

    private:
        static uint32_t rotate(uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }

        void block(const uint8_t *data)
        {
            static constexpr uint32_t kRound[64] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
            {
                w[i] = uint32_t(data[4 * i]) << 24 | uint32_t(data[4 * i + 1]) << 16 | uint32_t(data[4 * i + 2]) << 8 |
                       uint32_t(data[4 * i + 3]);
            }
            for (int i = 16; i < 64; ++i)
            {
                const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
            uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
            for (int i = 0; i < 64; ++i)
            {
                const uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
                const uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            m_state[0] += a;
            m_state[1] += b;
            m_state[2] += c;
            m_state[3] += d;
            m_state[4] += e;
            m_state[5] += f;
            m_state[6] += g;
            m_state[7] += h;
        }

        uint32_t m_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint8_t m_buffer[64] = {};
        size_t m_used = 0;
        uint64_t m_bytes = 0;
    };
}

bool FolderDisc::normalize(std::string_view path, std::string &out)
{
    out.clear();
    static constexpr std::string_view kDevice = "cdrom0:";
    if (path.size() >= kDevice.size())
    {
        bool device = true;
        for (size_t i = 0; i < kDevice.size(); ++i)
        {
            const char c = path[i];
            device = device && (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == kDevice[i];
        }
        if (device)
        {
            path.remove_prefix(kDevice.size());
        }
    }
    if (const size_t semi = path.find(';'); semi != std::string_view::npos)
    {
        const std::string_view version = path.substr(semi + 1);
        uint64_t number = 0;
        if (!parseNumber(version, number))
        {
            return false;
        }
        path = path.substr(0, semi);
    }
    size_t start = 0;
    while (start <= path.size())
    {
        size_t stop = start;
        while (stop < path.size() && path[stop] != '/' && path[stop] != '\\')
        {
            ++stop;
        }
        std::string_view part = path.substr(start, stop - start);
        start = stop + 1;
        if (part.empty())
        {
            continue;
        }
        if (part == "." || part == "..")
        {
            return false;
        }
        for (const char c : part)
        {
            if (static_cast<unsigned char>(c) < 0x20 || c == ':' || c == ';')
            {
                return false;
            }
        }
        if (!out.empty())
        {
            out.push_back('/');
        }
        for (const char c : part)
        {
            out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
    }
    return true;
}

bool FolderDisc::parseManifest(const std::string &root, std::string &error)
{
    const std::string manifestPath = root + "/.nxdisc/manifest.tsv";
    std::vector<uint8_t> bytes;
    if (!readWholeFile(manifestPath, 16u * 1024u * 1024u, bytes))
    {
        error = "cannot read " + manifestPath + "; run tools/extract_disc.py and copy .nxdisc to the SD card";
        return false;
    }
    const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    size_t position = 0, lineNumber = 0;
    const auto fail = [&](const std::string &why) {
        error = manifestPath + " line " + std::to_string(lineNumber) + ": " + why;
        return false;
    };
    while (position < text.size())
    {
        size_t end = text.find('\n', position);
        if (end == std::string_view::npos)
        {
            ++lineNumber;
            return fail("truncated manifest");
        }
        const std::string_view line = text.substr(position, end - position);
        position = end + 1;
        ++lineNumber;
        const std::vector<std::string_view> fields = splitTabs(line);
        if (lineNumber == 1)
        {
            if (line != "NXDISC\t1")
            {
                return fail("unsupported manifest version");
            }
            continue;
        }
        if (fields[0] == "sectors" && fields.size() == 2)
        {
            uint64_t sectors = 0;
            if (!parseNumber(fields[1], sectors) || sectors == 0 || sectors > 0x7fffffffu)
            {
                return fail("invalid sector count");
            }
            if (m_totalSectors != 0)
            {
                // Earlier entries were checked against the first count.
                return fail("duplicate sector count");
            }
            m_totalSectors = static_cast<uint32_t>(sectors);
        }
        else if ((fields[0] == "D" || fields[0] == "F") && fields.size() == 5)
        {
            if (m_totalSectors == 0)
            {
                return fail("entry before the sector count");
            }
            const bool directory = fields[0] == "D";
            uint64_t lba = 0, size = 0;
            if (!parseNumber(fields[1], lba) || !parseNumber(fields[2], size) || size > 0xffffffffu)
            {
                return fail("invalid extent");
            }
            if (!directory && fields[3] != "-" && !isSha256Hex(fields[3]))
            {
                return fail("invalid file hash");
            }
            const uint64_t sectors = (size + kSectorSize - 1) / kSectorSize;
            if (lba > m_totalSectors || sectors > m_totalSectors - lba)
            {
                return fail("extent past the end of the disc");
            }
            std::string key;
            if (!normalize(fields[4], key) || key.size() != fields[4].size() || (key.empty() && !directory))
            {
                return fail("unsafe path");
            }
            const std::string exact(fields[4]);
            if (key != exact || m_entries.count(key))
            {
                return fail(key != exact ? "path is not in disc form" : "duplicate path");
            }
            m_entries[key] = Entry{static_cast<uint32_t>(lba), static_cast<uint32_t>(size), directory};
            if (directory)
            {
                m_children.try_emplace(key);
            }
            else
            {
                Extent extent;
                extent.begin = lba * kSectorSize;
                extent.end = extent.begin + sectors * kSectorSize;
                extent.fileEnd = extent.begin + size;
                extent.file = static_cast<uint32_t>(m_files.size());
                m_extents.push_back(extent);
                m_files.push_back(File{root + "/" + exact, root + "/" + exact,
                                       fields[3] == "-" ? std::string() : std::string(fields[3])});
            }
            if (!key.empty())
            {
                const auto parent = m_children.find(parentOf(key));
                if (parent == m_children.end())
                {
                    return fail("entry before its parent directory");
                }
                parent->second.push_back(DirEntry{leafOf(key), static_cast<uint32_t>(lba), static_cast<uint32_t>(size), directory});
            }
        }
        else if (fields[0] == "raw_sha256" && fields.size() == 2)
        {
            if (!isSha256Hex(fields[1]) || !m_rawSha256.empty())
            {
                return fail(m_rawSha256.empty() ? "invalid raw hash" : "duplicate raw hash");
            }
            m_rawSha256 = std::string(fields[1]);
        }
        else if (fields[0] == "R" && fields.size() == 4)
        {
            uint64_t lba = 0, sectors = 0, offset = 0;
            if (!parseNumber(fields[1], lba) || !parseNumber(fields[2], sectors) || !parseNumber(fields[3], offset) ||
                sectors == 0 || lba > m_totalSectors || sectors > m_totalSectors - lba)
            {
                return fail("invalid raw extent");
            }
            Extent extent;
            extent.begin = lba * kSectorSize;
            extent.end = extent.begin + sectors * kSectorSize;
            extent.fileEnd = extent.end;
            extent.file = kRawExtent;
            extent.source = offset;
            m_extents.push_back(extent);
        }
        else if (fields[0] == "D" || fields[0] == "F" || fields[0] == "R" || fields.size() != 2)
        {
            return fail("malformed record");
        }
    }
    if (m_totalSectors == 0 || !m_entries.count(std::string()))
    {
        error = manifestPath + ": no sector count or root directory";
        return false;
    }
    return true;
}

bool FolderDisc::finishExtents(const std::string &root, std::string &error)
{
    std::sort(m_extents.begin(), m_extents.end(), [](const Extent &a, const Extent &b) { return a.begin < b.begin; });
    uint64_t position = 0, rawBytes = 0;
    struct stat modsInfo{};
    const bool mods = stat((root + "/mods").c_str(), &modsInfo) == 0 && S_ISDIR(modsInfo.st_mode);
    for (const Extent &extent : m_extents)
    {
        // A zero-length file holds no sectors, as in the extractor: it cannot overlap anything.
        if (extent.end > extent.begin)
        {
            if (extent.begin < position)
            {
                error = "manifest extents overlap at sector " + std::to_string(extent.begin / kSectorSize);
                return false;
            }
            position = extent.end;
        }
        if (extent.file == kRawExtent)
        {
            if (extent.source != rawBytes)
            {
                error = "manifest raw extents are out of order";
                return false;
            }
            rawBytes += extent.end - extent.begin;
        }
        else
        {
            struct stat info{};
            const std::string &path = m_files[extent.file].hostPath;
            if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            {
                error = "missing game file: " + path;
                return false;
            }
            if (static_cast<uint64_t>(info.st_size) != extent.fileEnd - extent.begin)
            {
                error = "wrong size for game file: " + path;
                return false;
            }
            if (mods)
            {
                // A different size would move every later LBA the game expects, so it is refused.
                const std::string modPath = root + "/mods/" + path.substr(root.size() + 1);
                struct stat modInfo{};
                if (stat(modPath.c_str(), &modInfo) == 0)
                {
                    if (S_ISREG(modInfo.st_mode) && modInfo.st_size == info.st_size)
                    {
                        m_files[extent.file].hostPath = modPath;
                        ++m_modsApplied;
                    }
                    else if (!S_ISREG(modInfo.st_mode))
                    {
                        m_warnings.push_back("ignored mod, not a regular file: " + modPath);
                    }
                    else
                    {
                        m_warnings.push_back("ignored mod, size differs from the disc file: " + modPath);
                    }
                }
            }
        }
    }
    // Reads find an extent by its start; empty extents would hide a file starting at the same sector.
    m_extents.erase(std::remove_if(m_extents.begin(), m_extents.end(),
                                   [](const Extent &extent) { return extent.end == extent.begin; }),
                    m_extents.end());
    if (rawBytes > kMaxRawBytes)
    {
        error = "manifest raw extents are too large";
        return false;
    }
    m_rawPath = root + "/.nxdisc/raw.bin";
    if (!readWholeFile(m_rawPath, kMaxRawBytes, m_raw) || m_raw.size() != rawBytes)
    {
        error = "missing or wrong size: " + m_rawPath;
        return false;
    }
    return true;
}

std::string FolderDisc::sha256Hex(const void *data, size_t size)
{
    Sha256 hash;
    hash.update(static_cast<const uint8_t *>(data), size);
    return hash.finishHex();
}

std::vector<std::string> FolderDisc::verifyFiles() const
{
    std::vector<std::string> problems;
    std::vector<uint8_t> buffer(1u << 20);
    for (const File &file : m_files)
    {
        if (file.sha256.empty())
        {
            problems.push_back("no recorded SHA-256 for game file: " + file.discPath);
            continue;
        }
        std::FILE *handle = std::fopen(file.discPath.c_str(), "rb");
        if (!handle)
        {
            problems.push_back("unreadable game file: " + file.discPath);
            continue;
        }
        Sha256 hash;
        size_t count = 0;
        while ((count = std::fread(buffer.data(), 1, buffer.size(), handle)) != 0)
        {
            hash.update(buffer.data(), count);
        }
        const bool readOk = std::ferror(handle) == 0;
        std::fclose(handle);
        if (!readOk)
        {
            problems.push_back("unreadable game file: " + file.discPath);
        }
        else if (hash.finishHex() != file.sha256)
        {
            problems.push_back("corrupt game file, SHA-256 differs from the manifest: " + file.discPath);
        }
    }
    // raw.bin is held in memory from load, so its loaded copy is what reads serve.
    if (m_rawSha256.empty())
    {
        problems.push_back("no recorded SHA-256 for " + m_rawPath);
    }
    else if (sha256Hex(m_raw.data(), m_raw.size()) != m_rawSha256)
    {
        problems.push_back("corrupt raw sectors, SHA-256 differs from the manifest: " + m_rawPath);
    }
    for (const std::string &problem : problems)
    {
        std::fprintf(stderr, "[folder-disc] %s\n", problem.c_str());
    }
    return problems;
}

std::unique_ptr<FolderDisc> FolderDisc::load(const std::string &root, std::string &error)
{
    std::unique_ptr<FolderDisc> disc(new FolderDisc());
    if (!disc->parseManifest(root, error) || !disc->finishExtents(root, error))
    {
        return nullptr;
    }
    return disc;
}

FolderDisc::~FolderDisc()
{
    for (Slot &slot : m_slots)
    {
        if (slot.handle)
        {
            std::fclose(slot.handle);
        }
    }
}

bool FolderDisc::findFile(std::string_view ps2Path, uint32_t &lsn, uint32_t &sizeBytes, bool &isDirectory) const
{
    std::string key;
    if (!normalize(ps2Path, key))
    {
        return false;
    }
    const auto found = m_entries.find(key);
    if (found == m_entries.end())
    {
        return false;
    }
    lsn = found->second.lsn;
    sizeBytes = found->second.sizeBytes;
    isDirectory = found->second.isDirectory;
    return true;
}

bool FolderDisc::listDirectory(std::string_view ps2Path, std::vector<DirEntry> &out) const
{
    out.clear();
    std::string key;
    if (!normalize(ps2Path, key))
    {
        return false;
    }
    const auto found = m_children.find(key);
    if (found == m_children.end())
    {
        return false;
    }
    out = found->second;
    return true;
}

FolderDisc::Slot *FolderDisc::acquire(uint32_t file) const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true)
    {
        Slot *same = nullptr, *victim = nullptr;
        for (Slot &slot : m_slots)
        {
            if (slot.handle && slot.file == file)
            {
                same = &slot;
            }
            if (!slot.busy && (!victim || !slot.handle || (victim->handle && slot.lastUse < victim->lastUse)))
            {
                victim = &slot;
            }
        }
        if (same && same->busy)
        {
            m_released.wait(lock);
            continue;
        }
        Slot *chosen = same ? same : victim;
        if (!chosen)
        {
            m_released.wait(lock);
            continue;
        }
        if (!same)
        {
            if (chosen->handle)
            {
                std::fclose(chosen->handle);
            }
            chosen->handle = std::fopen(m_files[file].hostPath.c_str(), "rb");
            chosen->file = file;
            if (!chosen->handle)
            {
                return nullptr;
            }
        }
        chosen->busy = true;
        chosen->lastUse = ++m_clock;
        return chosen;
    }
}

void FolderDisc::release(Slot *slot) const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        slot->busy = false;
    }
    m_released.notify_all();
}

namespace
{
    // M10-09: VCS_NX_FAULT_READ=<part of a host path>:<after s>:<count> (nxrun.cfg env): after <after s> seconds of the
    // process, the next <count> reads of a file whose path contains that text fail as an I/O error would (nothing read,
    // the same "read failed" line). Off when unset.
    bool injectedReadFault(const std::string &path)
    {
        static const struct Spec
        {
            std::string part;
            double after = 0.0;
            int count = 0;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        } spec = [] {
            Spec s;
            if (const char *v = std::getenv("VCS_NX_FAULT_READ"))
            {
                const std::string text(v);
                const size_t a = text.find(':'), b = a == std::string::npos ? a : text.find(':', a + 1);
                if (a != std::string::npos && b != std::string::npos)
                {
                    s.part = text.substr(0, a);
                    s.after = std::atof(text.substr(a + 1, b - a - 1).c_str());
                    s.count = std::atoi(text.substr(b + 1).c_str());
                }
            }
            return s;
        }();
        static std::atomic<int> injected{0};
        if (spec.part.empty() || path.find(spec.part) == std::string::npos ||
            std::chrono::duration<double>(std::chrono::steady_clock::now() - spec.start).count() < spec.after)
            return false;
        const int n = injected.fetch_add(1);
        if (n >= spec.count)
            return false;
        std::fprintf(stderr, "[fault] read %d of %d refused: %s (VCS_NX_FAULT_READ)\n", n + 1, spec.count, path.c_str());
        return true;
    }
}

namespace
{
    std::mutex g_unreadableMutex;
    std::string g_unreadable; // the first file a read failed on after every retry (M10-09)
}

bool folderDiscUnreadable(std::string &path)
{
    std::lock_guard<std::mutex> lock(g_unreadableMutex);
    path = g_unreadable;
    return !path.empty();
}

bool FolderDisc::readFile(uint32_t file, uint64_t offset, size_t count, uint8_t *destination) const
{
    // M10-09: a failed read is retried (3 more attempts, 20 ms apart), as a drive retries: a transient SD error then
    // costs a short wait. A read failing every time is recorded (folderDiscUnreadable) so the core can stop with the
    // file named instead of the game waiting on a black screen for data that never comes (fault-read-img-docked).
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        if (attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bool ok = false;
        if (!injectedReadFault(m_files[file].hostPath))
        {
            Slot *slot = acquire(file);
            ok = slot && seekTo(slot->handle, offset) && std::fread(destination, 1, count, slot->handle) == count;
            if (slot)
            {
                release(slot);
            }
        }
        if (ok && attempt == 0)
            return true;
        std::fprintf(stderr, "[folder-disc] read %s: %s offset=%llu count=%zu (attempt %d of 4)\n",
                     ok ? "succeeded after a failure" : "failed", m_files[file].hostPath.c_str(),
                     static_cast<unsigned long long>(offset), count, attempt + 1);
        if (ok)
            return true;
    }
    {
        std::lock_guard<std::mutex> lock(g_unreadableMutex);
        if (g_unreadable.empty())
            g_unreadable = m_files[file].hostPath;
    }
    return false;
}

bool FolderDisc::readBytes(uint64_t offset, size_t byteCount, void *destination) const
{
    if (byteCount != 0 && !destination)
    {
        return false;
    }
    uint8_t *out = static_cast<uint8_t *>(destination);
    if (byteCount != 0)
    {
        std::memset(out, 0, byteCount);
    }
    // As vcs::IsoImage::readBytes: an offset at or past the end fails whatever the count.
    const uint64_t discBytes = uint64_t(m_totalSectors) * kSectorSize;
    if (offset >= discBytes)
    {
        return false;
    }
    if (byteCount == 0)
    {
        return true;
    }
    const bool complete = byteCount <= discBytes - offset;
    const uint64_t stop = complete ? offset + byteCount : discBytes;

    auto extent = std::upper_bound(m_extents.begin(), m_extents.end(), offset,
                                   [](uint64_t value, const Extent &item) { return value < item.begin; });
    if (extent != m_extents.begin())
    {
        --extent;
    }
    uint64_t position = offset;
    for (; extent != m_extents.end() && extent->begin < stop; ++extent)
    {
        const uint64_t begin = std::max(position, extent->begin);
        if (begin >= extent->end)
        {
            continue; // the search can land on the extent before offset
        }
        const uint64_t dataEnd = std::min(stop, extent->fileEnd);
        if (begin < dataEnd)
        {
            uint8_t *target = out + (begin - offset);
            const size_t count = static_cast<size_t>(dataEnd - begin);
            if (extent->file == kRawExtent)
            {
                std::memcpy(target, m_raw.data() + extent->source + (begin - extent->begin), count);
            }
            else if (!readFile(extent->file, begin - extent->begin, count, target))
            {
                std::memset(out, 0, byteCount);
                return false;
            }
        }
        position = extent->end; // gaps and file padding stay zero from the memset above
    }
    return complete;
}
