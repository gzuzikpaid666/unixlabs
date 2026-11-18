#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace fs = std::filesystem;

class Sha1 {
public:
    Sha1() { reset(); }

    void update(const uint8_t* data, size_t len) {
        total_ += len;
        while (len > 0) {
            size_t n = std::min(len, sizeof(buf_) - blen_);
            std::memcpy(buf_ + blen_, data, n);
            blen_ += n; data += n; len -= n;
            if (blen_ == 64) { block(buf_); blen_ = 0; }
        }
    }

    std::string finish() {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (blen_ != 56) update(&zero, 1);
        uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(lenbuf, 8);

        char out[41];
        for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        return std::string(out, 40);
    }

private:
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t blen_ = 0;
    uint64_t total_ = 0;

    void reset() {
        h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE;
        h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
    }
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
                   (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
};

bool hashFile(const fs::path& p, std::string& result) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    Sha1 sha;
    std::vector<uint8_t> chunk(1 << 16);
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), chunk.size());
        std::streamsize n = in.gcount();
        if (n > 0) sha.update(chunk.data(), (size_t)n);
    }
    if (in.bad()) return false;
    result = sha.finish();
    return true;
}

struct Original {
    fs::path path;
    dev_t dev;
    ino_t ino;
};

bool replaceWithHardLink(const fs::path& orig, const fs::path& dup) {
    fs::path tmp = dup;
    tmp += ".dedup_tmp";

    std::error_code ec;
    fs::create_hard_link(orig, tmp, ec);
    if (ec) {
        std::cerr << "Не удалось создать ссылку: " << ec.message() << "\n";
        return false;
    }
    fs::rename(tmp, dup, ec);
    if (ec) {
        std::cerr << "Не удалось заменить файл: " << ec.message() << "\n";
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Использование: " << argv[0] << " <каталог>\n";
        return 1;
    }
    fs::path root = argv[1];
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::cerr << "Не каталог: " << root << "\n";
        return 1;
    }

    std::vector<fs::path> files;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code e2;
        auto st = it->symlink_status(e2);
        if (!e2 && fs::is_regular_file(st)) files.push_back(it->path());
    }
    std::cout << "Найдено файлов: " << files.size() << "\n";

    std::map<uintmax_t, std::vector<fs::path>> bySize;
    for (auto& f : files) {
        std::error_code e2;
        uintmax_t sz = fs::file_size(f, e2);
        if (!e2) bySize[sz].push_back(f);
    }

    size_t linked = 0;
    uintmax_t saved = 0;

    for (auto& [size, group] : bySize) {
        if (group.size() < 2) continue;

        std::map<std::string, Original> byHash; 

        for (auto& f : group) {
            std::string h;
            if (!hashFile(f, h)) {
                std::cerr << "Не удалось прочитать: " << f << "\n";
                continue;
            }
            struct stat s;
            if (stat(f.c_str(), &s) != 0) continue;

            auto found = byHash.find(h);
            if (found == byHash.end()) {
                byHash[h] = {f, s.st_dev, s.st_ino};
                continue;
            }

            const Original& orig = found->second;
            if (orig.dev == s.st_dev && orig.ino == s.st_ino) continue;
            if (orig.dev != s.st_dev) {
                std::cerr << "Разные файловые системы, пропуск: " << f << "\n";
                continue;
            }
            if (replaceWithHardLink(orig.path, f)) {
                std::cout << f.string() << "  ->  " << orig.path.string() << "\n";
                ++linked;
                saved += size;
            }
        }
    }

    std::cout << "Заменено дубликатов: " << linked
              << ", освобождено: " << saved << " байт\n";
    return 0;
}
