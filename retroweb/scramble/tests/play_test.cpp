#include <gtest/gtest.h>

#include "machine.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef CORE_USER_ROMS
#define CORE_USER_ROMS ""
#endif

namespace {

namespace fs = std::filesystem;

constexpr size_t kProgChip = 0x0800;
constexpr size_t kSndChip = 0x0800;
constexpr size_t kGfxChip = 0x0800;
constexpr size_t kProm = 0x20;

std::string lower(std::string n) {
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

std::optional<std::vector<uint8_t>> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool is_zip_name(const std::string& n) {
    if (n.size() < 4) return false;
    auto ext = n.substr(n.size() - 4);
    return lower(ext) == ".zip";
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

struct TmpDir {
    fs::path p;
    ~TmpDir() {
        std::error_code ec;
        if (!p.empty()) fs::remove_all(p, ec);
    }
};

void consider(std::map<std::string, std::vector<uint8_t>>& files, const fs::path& p) {
    if (!fs::is_regular_file(p)) return;
    auto b = read_file(p);
    if (b) files[lower(p.filename().string())] = std::move(*b);
}

void collect_dir(std::map<std::string, std::vector<uint8_t>>& files, const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec || !it->is_regular_file()) continue;
        consider(files, it->path());
    }
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

std::optional<scramble::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    auto get = [&](const char* name) -> const std::vector<uint8_t>* {
        auto it = files.find(name);
        return it == files.end() ? nullptr : &it->second;
    };

    scramble::RomSet s;
    const char* prog[8] = {"s1.2d", "s2.2e", "s3.2f", "s4.2h", "s5.2j", "s6.2l", "s7.2m", "s8.2p"};
    const char* snd[3] = {"ot1.5c", "ot2.5d", "ot3.5e"};
    const auto* g0 = get("c2.5f");
    const auto* g1 = get("c1.5h");
    const auto* prom = get("c01s.6e");
    if (!g0 || !g1 || !prom) return std::nullopt;
    for (const char* n : prog)
        if (!get(n) || get(n)->size() < kProgChip) return std::nullopt;
    for (const char* n : snd)
        if (!get(n) || get(n)->size() < kSndChip) return std::nullopt;
    if (g0->size() < kGfxChip || g1->size() < kGfxChip || prom->size() < kProm)
        return std::nullopt;
    for (int i = 0; i < 8; i++)
        std::copy_n(get(prog[i])->begin(), kProgChip, s.program.begin() + i * kProgChip);
    for (int i = 0; i < 3; i++)
        std::copy_n(get(snd[i])->begin(), kSndChip, s.sound.begin() + i * kSndChip);
    std::copy_n(g0->begin(), kGfxChip, s.gfx.begin());
    std::copy_n(g1->begin(), kGfxChip, s.gfx.begin() + kGfxChip);
    std::copy_n(prom->begin(), kProm, s.color_prom.begin());
    return s;
}

std::optional<scramble::RomSet> load_zip(const fs::path& zip) {
    TmpDir tmp;
    tmp.p = fs::temp_directory_path() / ("scramble-rom-" + std::to_string(std::rand()));
    if (!extract_zip(zip, tmp.p)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    collect_dir(files, tmp.p);
    return identify(files);
}

std::optional<scramble::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    if (fs::is_regular_file(p) && is_zip_name(p.filename().string())) return load_zip(p);
    std::map<std::string, std::vector<uint8_t>> files;
    if (fs::is_directory(p)) collect_dir(files, p);
    else consider(files, p);
    return identify(files);
}

fs::path default_user_dir() { return CORE_USER_ROMS; }

std::optional<scramble::RomSet> load_user_set() {
    if (const char* env = std::getenv("SCRAMBLE_ROM"); env && *env) {
        return load_path(env);
    }
    if (auto set = load_path(default_user_dir())) return set;
    if (const char* home = std::getenv("HOME"); home && *home) {
        if (auto set = load_path(fs::path(home) / "Downloads" / "scramble.zip")) return set;
    }
    return std::nullopt;
}

bool run_frames(scramble::Machine& m, int frames) {
    m.run_cycles(scramble::kCpuPerFrame * frames);
    return !m.watchdog_reset;
}

bool is_hwtest(const scramble::Machine& m) {
    return m.ram[0] == 'T' && m.ram[1] == 'S' && m.ram[2] == 'T' && m.ram[3] == '1';
}

}  // namespace

TEST(Machine, UserRomInsertsCoinAndStarts) {
    auto set = load_user_set();
    if (!set) {
        GTEST_SKIP() << "no local scramble dump in roms/user/, SCRAMBLE_ROM, or ~/Downloads/scramble.zip";
    }
    scramble::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(scramble::kCpuHz * 4);
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    m.run_cycles(scramble::kCpuHz / 10);
    m.inputs.in0 = 0xFF;
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    m.run_cycles(scramble::kCpuHz / 5);
    m.inputs.in1 = 0xFC;
    m.run_cycles(scramble::kCpuHz);
    std::array<uint32_t, scramble::kUprightW * scramble::kUprightH> rgb{};
    m.render(rgb.data());
    int lit = 0;
    for (uint32_t p : rgb) {
        if (p & 0x00FFFFFF) lit++;
    }
    EXPECT_GT(lit, 200) << "user ROM should have painted the playfield";
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, UserRomProducesAudio) {
    auto set = load_user_set();
    if (!set) {
        GTEST_SKIP() << "no local scramble dump in roms/user/, SCRAMBLE_ROM, or ~/Downloads/scramble.zip";
    }
    scramble::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(scramble::kCpuHz * 4);
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    m.run_cycles(scramble::kCpuHz / 10);
    m.inputs.in0 = 0xFF;
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    m.run_cycles(scramble::kCpuHz / 5);
    m.inputs.in1 = 0xFC;
    m.audio.clear();
    m.run_cycles(scramble::kCpuHz);
    ASSERT_FALSE(m.audio.empty());
    bool any = false;
    for (float s : m.audio) {
        if (s != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any) << "Konami ROM should program the AYs after coin+start";
}

TEST(Machine, UserRomInsertsCoinStartsAndFires) {
    std::optional<scramble::RomSet> set;
    if (const char* env = std::getenv("SCRAMBLE_ROM"); env && *env) {
        set = load_path(env);
        ASSERT_TRUE(set) << "SCRAMBLE_ROM=" << env << " is not a complete scramble set";
    } else {
        set = load_user_set();
        if (!set) {
            GTEST_SKIP() << "no user ROM set — unzip a MAME scramble set into "
                            "roms/user/, set SCRAMBLE_ROM, or leave scramble.zip in "
                            "~/Downloads (CI skips this on purpose)";
        }
    }

    scramble::Machine m;
    m.load_roms(*set);
    m.reset();
    ASSERT_TRUE(run_frames(m, 240)) << "watchdog tripped during POST/attract";
    if (is_hwtest(m)) {
        GTEST_SKIP() << "that dump is the generated hardware self-test ROM, not Scramble";
    }

    auto snapshot = m.ram;
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    ASSERT_TRUE(run_frames(m, 30));
    m.inputs.in0 = 0xFF;
    ASSERT_TRUE(run_frames(m, 60));
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    ASSERT_TRUE(run_frames(m, 90));
    m.inputs.in1 = 0xFC;
    // Fire + right: IN0 bits 3 and 4.
    m.inputs.in0 = uint8_t(0xFF & ~0x08 & ~0x10);
    bool moved = false;
    for (int i = 0; i < 600; i++) {
        ASSERT_TRUE(run_frames(m, 1));
        for (size_t a = 0; a < m.ram.size(); a++) {
            if (m.ram[a] != snapshot[a]) { moved = true; break; }
        }
        if (moved) break;
    }
    EXPECT_TRUE(moved) << "work RAM did not change after coin/start/fire";
    EXPECT_FALSE(m.watchdog_reset);
}
