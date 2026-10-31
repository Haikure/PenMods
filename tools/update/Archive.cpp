#include "Archive.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace penmods::update {
namespace {
constexpr uint64_t MaxArchive  = 256ULL * 1024 * 1024;
constexpr uint64_t MaxFile     = 192ULL * 1024 * 1024;
constexpr uint64_t MaxExpanded = 512ULL * 1024 * 1024;

uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
class Sha256 {
    std::array<uint32_t, 8>
        h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> buffer{};
    uint64_t                      length = 0;
    size_t                        used   = 0;
    void                          block(const unsigned char* p) {
        static constexpr uint32_t k[] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8)
                 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
            w[i] = w[i - 16] + (rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7]
                 + (rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10));
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], v = h[7];
        for (int i = 0; i < 64; ++i) {
            auto t = v + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            auto u = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            v      = g;
            g      = f;
            f      = e;
            e      = d + t;
            d      = c;
            c      = b;
            b      = a;
            a      = t + u;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += v;
    }

public:
    void add(const unsigned char* p, size_t n) {
        length += n;
        while (n) {
            auto count = std::min(n, buffer.size() - used);
            std::memcpy(buffer.data() + used, p, count);
            used += count;
            p    += count;
            n    -= count;
            if (used == 64) {
                block(buffer.data());
                used = 0;
            }
        }
    }
    std::string finish() {
        auto                bits  = length * 8;
        const unsigned char start = 0x80, zero = 0;
        add(&start, 1);
        while (used != 56) add(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; ++i) tail[7 - i] = static_cast<unsigned char>(bits >> (i * 8));
        add(tail, 8);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (auto v : h) out << std::setw(8) << v;
        return out.str();
    }
};
uint16_t u16(const std::vector<unsigned char>& b, size_t p) {
    if (p > b.size() || b.size() - p < 2) throw std::runtime_error("truncated ZIP");
    return b[p] | uint16_t(b[p + 1]) << 8;
}
uint32_t u32(const std::vector<unsigned char>& b, size_t p) { return u16(b, p) | uint32_t(u16(b, p + 2)) << 16; }
std::vector<unsigned char> load(const fs::path& path, uint64_t max) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    auto          length = in.tellg();
    if (!in || length < 0 || uint64_t(length) > max)
        throw std::runtime_error("missing or oversized archive: " + path.string());
    std::vector<unsigned char> result(static_cast<size_t>(length));
    in.seekg(0);
    if (!result.empty() && !in.read(reinterpret_cast<char*>(result.data()), length))
        throw std::runtime_error("archive read failed");
    return result;
}
} // namespace
std::string sha256(const std::vector<unsigned char>& bytes) {
    Sha256 h;
    h.add(bytes.data(), bytes.size());
    return h.finish();
}
std::string sha256File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot hash " + path.string());
    Sha256                           h;
    std::array<unsigned char, 65536> bytes;
    while (in) {
        in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        h.add(bytes.data(), in.gcount());
    }
    if (!in.eof()) throw std::runtime_error("hash read failed");
    return h.finish();
}
bool safeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.front() == '/' || path.find('\\') != std::string::npos
        || path.find('\0') != std::string::npos || path.find_first_of("*?[]") != std::string::npos)
        return false;
    size_t begin = 0;
    while (begin < path.size()) {
        auto end = path.find('/', begin);
        if (end == std::string::npos) end = path.size();
        auto part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos) return false;
        for (unsigned char c : part)
            if (c < 32 || c == 127) return false;
        begin = end + 1;
    }
    return true;
}
Archive::Archive(const fs::path& path) : mPath(fs::absolute(path)), mBytes(load(path, MaxArchive)) {
    if (mBytes.size() < 22) throw std::runtime_error("invalid ZIP end record");
    size_t end   = mBytes.size() - 22;
    size_t lower = mBytes.size() > 65557 ? mBytes.size() - 65557 : 0;
    while (u32(mBytes, end) != 0x06054b50 || end + 22 + u16(mBytes, end + 20) != mBytes.size()) {
        if (end == lower) throw std::runtime_error("ZIP end record not found");
        --end;
    }
    auto count       = u16(mBytes, end + 10);
    auto centralSize = u32(mBytes, end + 12);
    auto central     = u32(mBytes, end + 16);
    if (u16(mBytes, end + 4) || u16(mBytes, end + 6) || u16(mBytes, end + 8) != count || count == 65535 || count > 4096
        || uint64_t(central) + centralSize != end)
        throw std::runtime_error("unsupported ZIP layout");
    size_t                                     p     = central;
    uint64_t                                   total = 0;
    std::set<std::string>                      names;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (unsigned i = 0; i < count; ++i) {
        if (p + 46 > end || u32(mBytes, p) != 0x02014b50) throw std::runtime_error("invalid ZIP central record");
        auto flags = u16(mBytes, p + 8), method = u16(mBytes, p + 10), nl = u16(mBytes, p + 28),
             xl = u16(mBytes, p + 30), cl = u16(mBytes, p + 32);
        auto compressed = u32(mBytes, p + 20), size = u32(mBytes, p + 24), offset = u32(mBytes, p + 42),
             attr = u32(mBytes, p + 38);
        if (p + 46 + nl + xl + cl > end || u16(mBytes, p + 34) || (flags & ~uint16_t(0x080e)) || (flags & 1)
            || (method != 0 && method != 8) || size > MaxFile || compressed > MaxArchive)
            throw std::runtime_error("unsupported ZIP entry");
        std::string name(reinterpret_cast<const char*>(mBytes.data() + p + 46), nl);
        bool        directory = !name.empty() && name.back() == '/';
        if (!safeRelativePath(name) || !names.insert(directory ? name.substr(0, name.size() - 1) : name).second)
            throw std::runtime_error("unsafe or duplicate ZIP path");
        auto type = (attr >> 16) & 0170000;
        if (type && type != (directory ? 0040000 : 0100000))
            throw std::runtime_error("ZIP links and special files forbidden");
        if (directory && (size || compressed)) throw std::runtime_error("ZIP directory has data");
        if (uint64_t(offset) + 30 > central || u32(mBytes, offset) != 0x04034b50 || u16(mBytes, offset + 6) != flags
            || u16(mBytes, offset + 8) != method || u16(mBytes, offset + 26) != nl)
            throw std::runtime_error("ZIP local header mismatch");
        uint64_t data = uint64_t(offset) + 30 + nl + u16(mBytes, offset + 28);
        if (data > central || compressed > central - data || std::memcmp(mBytes.data() + offset + 30, name.data(), nl))
            throw std::runtime_error("ZIP local data mismatch");
        if (!(flags & 8)
            && (u32(mBytes, offset + 14) != u32(mBytes, p + 16) || u32(mBytes, offset + 18) != compressed
                || u32(mBytes, offset + 22) != size))
            throw std::runtime_error("ZIP local size mismatch");
        ranges.emplace_back(offset, data + compressed);
        total += size;
        if (total > MaxExpanded) throw std::runtime_error("ZIP expansion exceeds limit");
        mEntries.push_back(
            {name, u32(mBytes, p + 16), compressed, size, static_cast<uint32_t>(data), method, directory}
        );
        p += 46 + nl + xl + cl;
    }
    if (p != end) throw std::runtime_error("ZIP central size mismatch");
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second) throw std::runtime_error("overlapping ZIP entries");
    for (const auto& e : mEntries)
        if (!e.directory) {
            auto prefix = e.name + '/';
            for (const auto& other : mEntries)
                if (other.name.compare(0, prefix.size(), prefix) == 0)
                    throw std::runtime_error("ZIP file/directory collision");
        }
}
const Archive::Entry& Archive::entry(const std::string& name) const {
    for (const auto& e : mEntries)
        if (e.name == name && !e.directory) return e;
    throw std::runtime_error("missing ZIP entry: " + name);
}
std::vector<unsigned char> Archive::read(const Entry& e) const {
    std::vector<unsigned char> out(e.size);
    const auto*                data = mBytes.data() + e.dataOffset;
    if (e.method == 0) {
        if (e.compressedSize != e.size) throw std::runtime_error("stored ZIP size mismatch");
        std::copy(data, data + e.size, out.begin());
    } else {
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC) < 0) throw std::runtime_error("cannot create unzip pipe");
        auto pid = fork();
        if (pid < 0) {
            close(pipes[0]);
            close(pipes[1]);
            throw std::runtime_error("cannot fork unzip");
        }
        if (pid == 0) {
            dup2(pipes[1], STDOUT_FILENO);
            close(pipes[0]);
            close(pipes[1]);
            execl("/usr/bin/unzip", "unzip", "-p", mPath.c_str(), e.name.c_str(), static_cast<char*>(nullptr));
            execl("/bin/unzip", "unzip", "-p", mPath.c_str(), e.name.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(pipes[1]);
        size_t                           position = 0;
        bool                             overflow = false;
        std::array<unsigned char, 65536> bytes;
        for (;;) {
            auto n = ::read(pipes[0], bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                if (n < 0) overflow = true;
                break;
            }
            if (static_cast<size_t>(n) > out.size() - position) {
                overflow = true;
                break;
            }
            std::copy(bytes.begin(), bytes.begin() + n, out.begin() + position);
            position += n;
        }
        close(pipes[0]);
        if (overflow) kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (overflow || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || position != out.size())
            throw std::runtime_error("unzip data or size mismatch");
    }
    uint32_t crc = 0xffffffff;
    for (auto byte : out) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    if ((crc ^ 0xffffffff) != e.crc) throw std::runtime_error("ZIP CRC mismatch");
    return out;
}
} // namespace penmods::update
