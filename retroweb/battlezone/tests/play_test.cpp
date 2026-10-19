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

// MAME bzone / bzonea: six program chips, two vector chips, Math Box PROMs.
// The 036408 AVG state PROM is not needed; avg.cpp carries its sequence.
std::optional<battlezone::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    battlezone::RomSet s;
    auto find = [&](const std::string& part, size_t n) -> const std::vector<uint8_t>* {
        for (const auto& [name, data] : files) {
            std::string base = name.substr(name.find_last_of('/') + 1);
            if (base.rfind(part, 0) == 0 && data.size() >= n) return &data;
        }
        return nullptr;
    };
    const char* program[] = {"036414", "036413", "036412", "036411", "036410", "036409"};
    for (int i = 0; i < 6; i++) {
        const auto* chip = find(program[i], kChip);
        if (!chip) return std::nullopt;
        std::copy_n(chip->begin(), kChip, s.program.begin() + i * kChip);
    }
    const char* vector[] = {"036422", "036421"};
    for (int i = 0; i < 2; i++) {
        const auto* chip = find(vector[i], kChip);
        if (!chip) return std::nullopt;
        std::copy_n(chip->begin(), kChip, s.vector.begin() + i * kChip);
    }
    const auto* map = find("036174", 32);
    if (!map) return std::nullopt;
    std::copy_n(map->begin(), 32, s.mb_map.begin());
    const char* ucode[] = {"036175", "036176", "036177", "036178", "036179", "036180"};
    for (int i = 0; i < 6; i++) {
        const auto* chip = find(ucode[i], 256);
        if (!chip) return std::nullopt;
        std::copy_n(chip->begin(), 256, s.mb_ucode[size_t(i)].begin());
    }
    return s;
}

std::optional<battlezone::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    if (fs::is_regular_file(p) && ends_with(lower(p.filename().string()), ".zip")) {
        TmpDir tmp;
        tmp.p = fs::temp_directory_path() / ("bzone-rom-" + std::to_string(std::rand()));
        if (!extract_zip(p, tmp.p)) return std::nullopt;
        collect_dir(files, tmp.p);
        return identify(files);
    }
    if (fs::is_directory(p)) {
        for (const auto& entry : fs::directory_iterator(p, ec)) {
            if (ends_with(lower(entry.path().filename().string()), ".zip"))
                if (auto set = load_path(entry.path())) return set;
        }
        collect_dir(files, p);
    } else {
        consider(files, p);
    }
    return identify(files);
}

std::optional<battlezone::RomSet> load_user_set() {
    if (const char* env = std::getenv("BZONE_ROM"); env && *env) return load_path(env);
    if (auto set = load_path(CORE_USER_ROMS)) return set;
    if (const char* home = std::getenv("HOME"); home && *home) {
        for (const char* zip : {"bzone.zip", "bzonea.zip"}) {
            if (auto set = load_path(fs::path(home) / "images" / "arcade" / zip)) return set;
            if (auto set = load_path(fs::path(home) / "Downloads" / zip)) return set;
        }
    }
    return std::nullopt;
}

bool is_hwtest(const battlezone::Machine& m) {
    return m.ram[0] == 'B' && m.ram[1] == 'Z' && m.ram[2] == 'N' && m.ram[3] == '1';
}

void run_seconds(battlezone::Machine& m, double s) {
    m.run_cycles(int(battlezone::kCpuHz * s));
}

#define NEED_USER_SET(set)                                                              \
    auto set = load_user_set();                                                         \
    if (!set) GTEST_SKIP() << "no local bzone dump in roms/user/, BZONE_ROM, "          \
                              "~/images/arcade/bzone.zip, or ~/Downloads/bzone.zip"

}  // namespace

// Factory table from PWRON: each score's first byte is 5 (BZSTST.MAC).
TEST(Play, UserRomSeedsTheHighScoreTableAndDraws) {
    NEED_USER_SET(set);
    battlezone::Machine m;
    m.load_roms(*set);
    run_seconds(m, 4);
    if (is_hwtest(m)) GTEST_SKIP() << "that dump is the generated self-test ROM";
    for (int i = 0; i < 10; i++) EXPECT_EQ(m.ram[size_t(0x300 + 3 * i)], 5) << i;
    EXPECT_NE(m.ram[0x31E], 0) << "factory initials";
    EXPECT_GT(m.avg.frames, 100);
    EXPECT_GT(m.avg.frame.size(), 100u);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Play, UserRomCoinStartDriveAndShoot) {
    NEED_USER_SET(set);
    battlezone::Machine m;
    m.load_roms(*set);
    m.inputs.dsw1 = 0x02;  // 1 coin / 1 credit
    run_seconds(m, 2);
    if (is_hwtest(m)) GTEST_SKIP() << "that dump is the generated self-test ROM";
    m.inputs.in0 &= uint8_t(~0x01);
    run_seconds(m, 0.1);
    m.inputs.in0 |= 0x01;
    run_seconds(m, 0.5);
    m.inputs.in3 = 0x20;
    run_seconds(m, 0.2);
    m.inputs.in3 = 0x0A;  // both treads forward
    run_seconds(m, 3);
    EXPECT_EQ(m.sound.latch & 0xA0, 0xA0) << "motor and sound enabled in a game";
    uint64_t steps = m.mathbox.steps;
    m.audio.clear();
    float peak = 0;
    for (int i = 0; i < 40; i++) {
        m.inputs.in3 = uint8_t(0x0A | ((i & 4) ? 0x10 : 0));
        run_seconds(m, 0.05);
        for (float s : m.audio) peak = std::max(peak, s < 0 ? -s : s);
        m.audio.clear();
    }
    EXPECT_GT(m.mathbox.steps, steps + 10000) << "3D view keeps the Math Box busy";
    EXPECT_GT(peak, 0.02f);
    EXPECT_FALSE(m.watchdog_reset);
}
