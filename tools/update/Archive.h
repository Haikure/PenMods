#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace penmods::update {
namespace fs = std::filesystem;

std::string sha256(const std::vector<unsigned char>& bytes);
std::string sha256File(const fs::path& path);
bool        safeRelativePath(const std::string& path);

// Parse and validate the complete ZIP before creating any payload file. ZIP64,
// links, encryption, duplicate names and ambiguous local headers are rejected.
class Archive {
public:
    struct Entry {
        std::string name;
        uint32_t    crc;
        uint32_t    compressedSize;
        uint32_t    size;
        uint32_t    dataOffset;
        uint16_t    method;
        bool        directory;
    };
    explicit Archive(const fs::path& path);
    const std::vector<Entry>&  entries() const { return mEntries; }
    std::vector<unsigned char> read(const Entry& entry) const;
    const Entry&               entry(const std::string& name) const;

private:
    fs::path                   mPath;
    std::vector<unsigned char> mBytes;
    std::vector<Entry>         mEntries;
};
} // namespace penmods::update
