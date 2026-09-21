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

constexpr size_t kProgChip = 0x800;
constexpr size_t kGfxChip = 0x800;
constexpr size_t kProm = 32;

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

void consider(std::map<std::string, std::vector<uint8_t>>& files, const fs::path& p) {
    std::string name = p.filename().string();
    for (char& c : name) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    files[name] = std::move(buf);
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

std::optional<galaxian::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    auto get = [&](const char* name) -> const std::vector<uint8_t>* {
        auto it = files.find(name);
        return it == files.end() ? nullptr : &it->second;
    };

    galaxian::RomSet s;
    const auto* g0 = get("1h.bin");
    const auto* g1 = get("1k.bin");
    const auto* prom = get("6l.bpr");
    if (!prom) prom = get("6l.bin");
    if (!g0 || !g1 || !prom) return std::nullopt;
    if (g0->size() < kGfxChip || g1->size() < kGfxChip || prom->size() < kProm)
        return std::nullopt;

    const char* chips2k[5] = {"galmidw.u", "galmidw.v", "galmidw.w", "galmidw.y", "7l"};
    bool have_2k = true;
    for (const char* n : chips2k)
        if (!get(n) || get(n)->size() < kProgChip) have_2k = false;
    if (have_2k) {
        for (int i = 0; i < 5; i++)
            std::copy_n(get(chips2k[i])->begin(), kProgChip, s.program.begin() + i * kProgChip);
    } else {
        const auto* a = get("7f.bin");
        const auto* b = get("7j.bin");
        const auto* c = get("7l.bin");
        if (!c) c = get("7l");
        if (!a || !b || !c || a->size() < 0x1000 || b->size() < 0x1000 || c->size() < kProgChip)
            return std::nullopt;
        std::copy_n(a->begin(), 0x1000, s.program.begin());
        std::copy_n(b->begin(), 0x1000, s.program.begin() + 0x1000);
        std::copy_n(c->begin(), kProgChip, s.program.begin() + 0x2000);
    }
    std::copy_n(g0->begin(), kGfxChip, s.gfx.begin());
    std::copy_n(g1->begin(), kGfxChip, s.gfx.begin() + kGfxChip);
    std::copy_n(prom->begin(), kProm, s.color_prom.begin());
    return s;
}

std::optional<galaxian::RomSet> load_zip(const fs::path& zip) {
    TmpDir tmp;
    tmp.p = fs::temp_directory_path() / ("galaxian-rom-" + std::to_string(std::rand()));
    if (!extract_zip(zip, tmp.p)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    collect_dir(files, tmp.p);
    return identify(files);
}

std::optional<galaxian::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    if (fs::is_regular_file(p) && is_zip_name(p.filename().string())) return load_zip(p);
    std::map<std::string, std::vector<uint8_t>> files;
    if (fs::is_directory(p)) collect_dir(files, p);
    else consider(files, p);
    return identify(files);
}

fs::path default_user_dir() { return CORE_USER_ROMS; }

std::optional<galaxian::RomSet> load_user_set() {
    if (const char* env = std::getenv("GALAXIAN_ROM"); env && *env) {
        return load_path(env);
    }
    if (auto set = load_path(default_user_dir())) return set;
    if (const char* home = std::getenv("HOME"); home && *home) {
        if (auto set = load_path(fs::path(home) / "images" / "arcade" / "galaxian.zip")) return set;
        if (auto set = load_path(fs::path(home) / "Downloads" / "galaxian.zip")) return set;
    }
    return std::nullopt;
}

bool run_frames(galaxian::Machine& m, int frames) {
    m.run_cycles(galaxian::kCpuPerFrame * frames);
    return !m.watchdog_reset;
}

bool is_hwtest(const galaxian::Machine& m) {
    return m.ram[0] == 'T' && m.ram[1] == 'S' && m.ram[2] == 'T' && m.ram[3] == '1';
}

}  // namespace

TEST(Machine, UserRomInsertsCoinAndStarts) {
    auto set = load_user_set();
    if (!set) {
        GTEST_SKIP() << "no local galaxian dump in roms/user/, GALAXIAN_ROM, "
                        "~/images/arcade/galaxian.zip, or ~/Downloads/galaxian.zip";
    }
    galaxian::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(galaxian::kCpuHz * 4);
    m.inputs.in0 = 0x01;
    m.run_cycles(galaxian::kCpuHz / 10);
    m.inputs.in0 = 0x00;
    m.inputs.in1 = 0x01;
    m.run_cycles(galaxian::kCpuHz / 5);
    m.inputs.in1 = 0x00;
    m.run_cycles(galaxian::kCpuHz);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> rgb{};
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
        GTEST_SKIP() << "no local galaxian dump in roms/user/, GALAXIAN_ROM, "
                        "~/images/arcade/galaxian.zip, or ~/Downloads/galaxian.zip";
    }
    galaxian::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(galaxian::kCpuHz * 4);
    m.inputs.in0 = 0x01;
    m.run_cycles(galaxian::kCpuHz / 10);
    m.inputs.in0 = 0x00;
    m.inputs.in1 = 0x01;
    m.run_cycles(galaxian::kCpuHz / 5);
    m.inputs.in1 = 0x00;
    m.audio.clear();
    bool any = false;
    bool dive_pitch = false;
    bool dive_without_fire = false;
    for (int i = 0; i < 150; i++) {
        m.run_cycles(galaxian::kCpuHz / 10);
        for (float s : m.audio) {
            if (s != 0.0f) any = true;
        }
        m.audio.clear();
        if (m.sound.pitch != 0xFF) {
            dive_pitch = true;
            if (!m.sound.fire) dive_without_fire = true;
        }
        if (any && dive_without_fire) break;
    }
    EXPECT_TRUE(any) << "Namco ROM should program discrete sound after coin+start";
    EXPECT_TRUE(dive_pitch) << "a dive writes $7800 pitch below 0xFF";
    EXPECT_TRUE(dive_without_fire)
        << "pitch must keep sounding after the FIRE latch drops";
}

TEST(Machine, UserRomInsertsCoinStartsAndFires) {
    std::optional<galaxian::RomSet> set;
    if (const char* env = std::getenv("GALAXIAN_ROM"); env && *env) {
        set = load_path(env);
        ASSERT_TRUE(set) << "GALAXIAN_ROM=" << env << " is not a complete galaxian set";
    } else {
        set = load_user_set();
        if (!set) {
            GTEST_SKIP() << "no user ROM set — unzip a MAME galaxian set into "
                            "roms/user/, set GALAXIAN_ROM, or leave galaxian.zip in "
                            "~/images/arcade or ~/Downloads (CI skips this on purpose)";
        }
    }

    galaxian::Machine m;
    m.load_roms(*set);
    m.reset();
    ASSERT_TRUE(run_frames(m, 240)) << "watchdog tripped during POST/attract";
    if (is_hwtest(m)) {
        GTEST_SKIP() << "that dump is the generated hardware self-test ROM, not Galaxian";
    }

    auto snapshot = m.ram;
    m.inputs.in0 = 0x01;
    ASSERT_TRUE(run_frames(m, 30));
    m.inputs.in0 = 0x00;
    ASSERT_TRUE(run_frames(m, 60));
    m.inputs.in1 = 0x01;
    ASSERT_TRUE(run_frames(m, 90));
    m.inputs.in1 = 0x00;
    m.inputs.in0 = 0x18;  // fire + right, active high
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
