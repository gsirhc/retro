#include <gtest/gtest.h>

#include "machine.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kChip = 0x800;

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string sh_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

void consider(std::map<std::string, std::vector<uint8_t>>& files, const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    files[lower(p.filename().string())] = std::move(buf);
}

void collect_dir(std::map<std::string, std::vector<uint8_t>>& files, const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) consider(files, it->path());
    }
}

struct TmpDir {
    fs::path p;
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
};

bool extract_zip(const fs::path& zip, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return false;
    const std::string cmd =
        "python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' " +
        sh_quote(zip.string()) + " " + sh_quote(dest.string());
    return std::system(cmd.c_str()) == 0;
}

// lrescue.1-4 at $0000, lrescue.5-6 at $4000, 7643-1.cpu colour map.
std::optional<lunarrescue::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    lunarrescue::RomSet s;
    auto find = [&](const std::string& want, size_t n) -> const std::vector<uint8_t>* {
        for (const auto& [name, data] : files) {
            if ((name == want || ends_with(name, "/" + want)) && data.size() >= n) return &data;
        }
        return nullptr;
    };
    for (int i = 0; i < 6; i++) {
        const auto* chip = find("lrescue." + std::to_string(i + 1), kChip);
        if (!chip) return std::nullopt;
        uint8_t* dst = i < 4 ? s.program.data() + i * kChip : s.program_hi.data() + (i - 4) * kChip;
        std::copy_n(chip->begin(), kChip, dst);
    }
    const auto* prom = find("7643-1.cpu", s.prom.size());
    if (!prom) return std::nullopt;
    std::copy_n(prom->begin(), s.prom.size(), s.prom.begin());
    return s;
}

std::optional<lunarrescue::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    if (fs::is_regular_file(p) && ends_with(lower(p.filename().string()), ".zip")) {
        TmpDir tmp;
        tmp.p = fs::temp_directory_path() / ("lrescue-rom-" + std::to_string(std::rand()));
        if (!extract_zip(p, tmp.p)) return std::nullopt;
        collect_dir(files, tmp.p);
        return identify(files);
    }
    if (fs::is_directory(p)) collect_dir(files, p);
    else consider(files, p);
    return identify(files);
}

std::optional<lunarrescue::RomSet> load_user_set() {
    if (const char* env = std::getenv("LRESCUE_ROM"); env && *env) return load_path(env);
    if (auto set = load_path(CORE_USER_ROMS)) return set;
    if (const char* home = std::getenv("HOME"); home && *home) {
        if (auto set = load_path(fs::path(home) / "images" / "arcade" / "lrescue.zip")) return set;
        if (auto set = load_path(fs::path(home) / "Downloads" / "lrescue.zip")) return set;
    }
    return std::nullopt;
}

bool is_hwtest(const lunarrescue::Machine& m) {
    return m.ram[0] == 'T' && m.ram[1] == 'S' && m.ram[2] == 'T' && m.ram[3] == '1';
}

void run_frames(lunarrescue::Machine& m, int frames) {
    m.run_cycles(lunarrescue::kFrameCycles * frames);
}

#define NEED_USER_SET(set)                                                              \
    auto set = load_user_set();                                                         \
    if (!set) GTEST_SKIP() << "no local lrescue dump in roms/user/, LRESCUE_ROM, "      \
                              "~/images/arcade/lrescue.zip, or ~/Downloads/lrescue.zip"

}  // namespace

TEST(Play, UserRomSeedsTheHiscoreAndPaintsInColour) {
    NEED_USER_SET(set);
    lunarrescue::Machine m;
    m.load_roms(*set);
    m.reset();
    run_frames(m, 240);
    if (is_hwtest(m)) GTEST_SKIP() << "that dump is the generated self-test ROM";
    EXPECT_EQ(m.ram[0xF4], 0x00) << "high score low byte at $20F4";
    EXPECT_EQ(m.ram[0xF5], 0x05);
    std::array<uint32_t, lunarrescue::kUprightW * lunarrescue::kUprightH> rgb{};
    m.render(rgb.data());
    std::vector<uint32_t> colours;
    int lit = 0;
    for (uint32_t p : rgb) {
        if (!p) continue;
        lit++;
        if (std::find(colours.begin(), colours.end(), p) == colours.end()) colours.push_back(p);
    }
    EXPECT_GT(lit, 200);
    EXPECT_GE(colours.size(), 2u) << "the colour PROM tints the attract screen";
}

TEST(Play, UserRomCoinStartAndFireSound) {
    NEED_USER_SET(set);
    lunarrescue::Machine m;
    m.load_roms(*set);
    m.reset();
    run_frames(m, 120);
    if (is_hwtest(m)) GTEST_SKIP() << "that dump is the generated self-test ROM";
    m.inputs.in1 &= uint8_t(~0x01);
    run_frames(m, 6);
    m.inputs.in1 |= 0x05;
    run_frames(m, 10);
    m.inputs.in1 &= uint8_t(~0x04);
    run_frames(m, 600);
    m.audio.clear();
    float peak = 0;
    for (int i = 0; i < 120; i++) {
        m.inputs.in1 ^= 0x10;
        m.inputs.in2 ^= 0x10;
        run_frames(m, 4);
        for (float s : m.audio) peak = std::max(peak, s < 0 ? -s : s);
        m.audio.clear();
    }
    EXPECT_TRUE(m.sound.amp_enabled());
    EXPECT_GT(peak, 0.01f) << "a game in progress makes sound";
}
