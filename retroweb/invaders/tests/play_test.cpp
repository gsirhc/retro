#include <gtest/gtest.h>

#include "machine.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// MAME invaders: H, G, F, E at $0000, $0800, $1000, $1800.
std::optional<invaders::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    invaders::RomSet s;
    for (const auto& [name, data] : files) {
        if ((name == "program.bin" || name == "invaders.bin") && data.size() >= s.program.size()) {
            std::copy_n(data.begin(), s.program.size(), s.program.begin());
            return s;
        }
    }
    const char* chip[4] = {"h", "g", "f", "e"};
    for (int i = 0; i < 4; i++) {
        const std::string loose = std::string("invaders.") + chip[i];
        const std::string mame = std::string("m739") + chip[i] + "." + chip[i] + "1";
        const std::vector<uint8_t>* found = nullptr;
        for (const auto& [name, data] : files) {
            if ((name == loose || ends_with(name, mame)) && data.size() >= kChip) found = &data;
        }
        if (!found) return std::nullopt;
        std::copy_n(found->begin(), kChip, s.program.begin() + i * kChip);
    }
    return s;
}

std::optional<invaders::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    if (fs::is_regular_file(p) && ends_with(lower(p.filename().string()), ".zip")) {
        TmpDir tmp;
        tmp.p = fs::temp_directory_path() / ("invaders-rom-" + std::to_string(std::rand()));
        if (!extract_zip(p, tmp.p)) return std::nullopt;
        collect_dir(files, tmp.p);
        return identify(files);
    }
    if (fs::is_directory(p)) collect_dir(files, p);
    else consider(files, p);
    return identify(files);
}

std::optional<invaders::RomSet> load_user_set() {
    if (const char* env = std::getenv("INVADERS_ROM"); env && *env) return load_path(env);
    if (auto set = load_path(CORE_USER_ROMS)) return set;
    if (const char* home = std::getenv("HOME"); home && *home) {
        if (auto set = load_path(fs::path(home) / "images" / "arcade" / "invaders.zip")) return set;
        if (auto set = load_path(fs::path(home) / "Downloads" / "invaders.zip")) return set;
    }
    return std::nullopt;
}

bool is_hwtest(const invaders::Machine& m) {
    return m.ram[0] == 'T' && m.ram[1] == 'S' && m.ram[2] == 'T' && m.ram[3] == '1';
}

bool run_frames(invaders::Machine& m, int frames) {
    m.run_cycles(invaders::kFrameCycles * frames);
    return !m.watchdog_reset;
}

#define NEED_USER_SET(set)                                                              \
    auto set = load_user_set();                                                         \
    if (!set) GTEST_SKIP() << "no local invaders dump in roms/user/, INVADERS_ROM, "    \
                              "~/images/arcade/invaders.zip, or ~/Downloads/invaders.zip"

}  // namespace

TEST(Play, UserRomAttractPaintsAndCoinStartsAGame) {
    NEED_USER_SET(set);
    invaders::Machine m;
    m.load_roms(*set);
    m.reset();
    ASSERT_TRUE(run_frames(m, 240)) << "watchdog tripped during attract";
    if (is_hwtest(m)) GTEST_SKIP() << "that dump is the generated self-test ROM";
    std::array<uint32_t, invaders::kUprightW * invaders::kUprightH> rgb{};
    m.render(rgb.data());
    int lit = 0;
    for (uint32_t p : rgb) lit += p ? 1 : 0;
    EXPECT_GT(lit, 200);

    m.inputs.in1 &= uint8_t(~0x01);
    ASSERT_TRUE(run_frames(m, 6));
    m.inputs.in1 |= 0x01;
    ASSERT_TRUE(run_frames(m, 30));
    EXPECT_EQ(m.ram[0xEB], 0x01) << "numCoins ($20EB) after one coin";
    m.inputs.in1 |= 0x04;
    ASSERT_TRUE(run_frames(m, 10));
    m.inputs.in1 &= uint8_t(~0x04);
    ASSERT_TRUE(run_frames(m, 180));
    EXPECT_EQ(m.ram[0xEF], 0x01) << "gameMode ($20EF) after 1P start";
    EXPECT_EQ(m.ram[0xEB], 0x00);
}

TEST(Play, UserRomFiresAndSounds) {
    NEED_USER_SET(set);
    invaders::Machine m;
    m.load_roms(*set);
    m.reset();
    ASSERT_TRUE(run_frames(m, 120));
    if (is_hwtest(m)) GTEST_SKIP() << "that dump is the generated self-test ROM";
    m.inputs.in1 &= uint8_t(~0x01);
    ASSERT_TRUE(run_frames(m, 6));
    m.inputs.in1 |= 0x05;
    ASSERT_TRUE(run_frames(m, 10));
    m.inputs.in1 &= uint8_t(~0x04);
    ASSERT_TRUE(run_frames(m, 240));
    m.audio.clear();
    float peak = 0;
    for (int i = 0; i < 60; i++) {
        m.inputs.in1 ^= 0x10;
        ASSERT_TRUE(run_frames(m, 4));
        for (float s : m.audio) peak = std::max(peak, s < 0 ? -s : s);
        m.audio.clear();
    }
    EXPECT_TRUE(m.sound.amp_enabled());
    EXPECT_GT(peak, 0.01f) << "shots and fleet should be audible";
}
