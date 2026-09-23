#include <gtest/gtest.h>

#include "machine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

bool is_zip_name(const std::string& n) {
    auto lower = n;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return lower.size() >= 4 && lower.substr(lower.size() - 4) == ".zip";
}

std::string sh_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

bool extract_zip(const fs::path& zip, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return false;
    const std::string cmd =
        "python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' " +
        sh_quote(zip.string()) + " " + sh_quote(dest.string());
    return std::system(cmd.c_str()) == 0;
}

bool has_main(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "3200a.bin", ec);
}

struct Hold {
    fs::path dir;
    fs::path tmp;
    ~Hold() {
        if (!tmp.empty()) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
        }
    }
};

std::optional<Hold> open_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    Hold h;
    if (fs::is_regular_file(p, ec) && is_zip_name(p.filename().string())) {
        h.tmp = fs::temp_directory_path() / ("galaga-rom-" + std::to_string(std::rand()));
        if (!extract_zip(p, h.tmp)) return std::nullopt;
        h.dir = h.tmp;
    } else if (fs::is_directory(p, ec)) {
        h.dir = p;
    } else {
        return std::nullopt;
    }
    if (!has_main(h.dir)) return std::nullopt;
    return h;
}

std::optional<Hold> find_dump() {
    if (const char* env = std::getenv("GALAGA_ROM"); env && *env) {
        if (auto h = open_path(env)) return h;
    }
    if (auto h = open_path(CORE_USER_ROMS)) return h;
    if (const char* home = std::getenv("HOME"); home && *home) {
        fs::path root(home);
        if (auto h = open_path(root / "images" / "arcade" / "galagamw.zip")) return h;
        if (auto h = open_path(root / "Downloads" / "galagamw.zip")) return h;
    }
    return std::nullopt;
}

bool read_file(const fs::path& path, uint8_t* dst, size_t n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(dst), std::streamsize(n));
    return in.gcount() == std::streamsize(n);
}

}  // namespace

TEST(Machine, UserRomInsertsCoinWhenLocalDumpPresent) {
    auto hold = find_dump();
    if (!hold) {
        GTEST_SKIP() << "no local galagamw dump in roms/user/, GALAGA_ROM, "
                        "~/images/arcade/galagamw.zip, or ~/Downloads/galagamw.zip";
    }
    const fs::path& dir = hold->dir;
    galaga::RomSet set;
    if (!read_file(dir / "3200a.bin", set.main.data(), 0x1000) ||
        !read_file(dir / "3300b.bin", set.main.data() + 0x1000, 0x1000) ||
        !read_file(dir / "3400c.bin", set.main.data() + 0x2000, 0x1000) ||
        !read_file(dir / "3500d.bin", set.main.data() + 0x3000, 0x1000) ||
        !read_file(dir / "3600e.bin", set.sub.data(), 0x1000) ||
        !read_file(dir / "3700g.bin", set.sound.data(), 0x1000)) {
        GTEST_SKIP() << "incomplete galagamw set in " << dir;
    }
    read_file(dir / "2600j.bin", set.tiles.data(), set.tiles.size());
    read_file(dir / "2800l.bin", set.sprites.data(), 0x1000);
    read_file(dir / "2700k.bin", set.sprites.data() + 0x1000, 0x1000);
    read_file(dir / "prom-5.5n", set.palette.data(), set.palette.size());
    read_file(dir / "prom-4.2n", set.char_lut.data(), set.char_lut.size());
    read_file(dir / "prom-3.1c", set.sprite_lut.data(), set.sprite_lut.size());
    read_file(dir / "prom-1.1d", set.wave.data(), set.wave.size());
    if (read_file(dir / "51xx.bin", set.mcu51.data(), set.mcu51.size())) set.has51 = true;
    if (read_file(dir / "54xx.bin", set.mcu54.data(), set.mcu54.size())) set.has54 = true;

    galaga::Machine m;
    m.load_roms(set);
    m.reset();
    m.run_cycles(galaga::kCpuHz);
    m.inputs.in1 = 0x10;
    m.run_cycles(galaga::kCpuHz / 10);
    m.inputs.in1 = 0x04;
    m.run_cycles(galaga::kCpuHz);
    EXPECT_GT(m.frames, 60);
    EXPECT_FALSE(m.watchdog_reset);
}
