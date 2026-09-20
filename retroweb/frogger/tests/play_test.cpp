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

// MAME frogger chip names / sizes. CRC is not a whitelist.
constexpr size_t kProgChip = 0x1000;
constexpr size_t kSndChip = 0x0800;
constexpr size_t kGfxChip = 0x0800;
constexpr size_t kProm = 0x20;

// Computer Archaeology RAMUse: work RAM at $8000.
constexpr uint16_t kFrogX = 0x8044;
constexpr uint16_t kFrogY = 0x8047;
constexpr uint16_t kLives = 0x83B7;
constexpr uint16_t kP1Lives = 0x83B8;
constexpr uint16_t kCredits = 0x83E1;  // packed BCD
constexpr uint16_t kP1Score = 0x83ED;  // 16-bit
constexpr uint16_t kPlayFlag = 0x83FE; // 0 = attract
constexpr uint16_t kFurthest = 0x8269;

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
    // python3 is already a test dependency (gen_hwtest.py).
    const std::string cmd =
        "python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' " +
        sh_quote(zip.string()) + " " + sh_quote(dest.string());
    return std::system(cmd.c_str()) == 0;
}

std::optional<frogger::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    auto get = [&](const char* name) -> const std::vector<uint8_t>* {
        auto it = files.find(name);
        return it == files.end() ? nullptr : &it->second;
    };

    frogger::RomSet s;
    const auto* p26 = get("frogger.26");
    const auto* p27 = get("frogger.27");
    const auto* p7 = get("frsm3.7");
    const auto* s608 = get("frogger.608");
    const auto* s609 = get("frogger.609");
    const auto* s610 = get("frogger.610");
    const auto* g606 = get("frogger.606");
    const auto* g607 = get("frogger.607");
    const auto* prom = get("pr-91.6l");
    if (!p26 || !p27 || !p7 || !s608 || !s609 || !s610 || !g606 || !g607 || !prom)
        return std::nullopt;
    // MAME frogger: 4K+4K+4K. Some sets ship an 8K last chip; pad a 4K one.
    if (p26->size() < kProgChip || p27->size() < kProgChip || p7->size() < kProgChip)
        return std::nullopt;
    if (s608->size() < kSndChip || s609->size() < kSndChip || s610->size() < kSndChip)
        return std::nullopt;
    if (g606->size() < kGfxChip || g607->size() < kGfxChip || prom->size() < kProm)
        return std::nullopt;

    std::copy(p26->begin(), p26->begin() + kProgChip, s.program.begin());
    std::copy(p27->begin(), p27->begin() + kProgChip, s.program.begin() + kProgChip);
    const size_t last = std::min(p7->size(), size_t(0x2000));
    std::copy(p7->begin(), p7->begin() + last, s.program.begin() + 0x2000);
    std::copy(s608->begin(), s608->begin() + kSndChip, s.sound.begin());
    std::copy(s609->begin(), s609->begin() + kSndChip, s.sound.begin() + kSndChip);
    std::copy(s610->begin(), s610->begin() + kSndChip, s.sound.begin() + 2 * kSndChip);
    std::copy(g607->begin(), g607->begin() + kGfxChip, s.gfx.begin());
    std::copy(g606->begin(), g606->begin() + kGfxChip, s.gfx.begin() + kGfxChip);
    std::copy(prom->begin(), prom->begin() + kProm, s.color_prom.begin());
    return s;
}

std::optional<frogger::RomSet> load_zip(const fs::path& zip) {
    TmpDir tmp;
    tmp.p = fs::temp_directory_path() /
            ("frogger-rom-" + std::to_string(std::rand()) + "-" + zip.filename().string());
    if (!extract_zip(zip, tmp.p)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    collect_dir(files, tmp.p);
    return identify(files);
}

std::optional<frogger::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    if (fs::is_regular_file(p) && is_zip_name(p.filename().string())) return load_zip(p);
    if (!fs::is_directory(p)) return std::nullopt;
    std::map<std::string, std::vector<uint8_t>> files;
    collect_dir(files, p);
    if (auto set = identify(files)) return set;
    for (auto it = fs::directory_iterator(p, ec); it != fs::directory_iterator(); ++it) {
        if (ec || !it->is_regular_file()) continue;
        if (is_zip_name(it->path().filename().string())) {
            if (auto set = load_zip(it->path())) return set;
        }
    }
    return std::nullopt;
}

fs::path default_user_dir() {
    if (CORE_USER_ROMS[0]) return fs::path(CORE_USER_ROMS);
    return fs::path(__FILE__).parent_path().parent_path() / "roms" / "user";
}

std::optional<frogger::RomSet> load_user_set() {
    if (const char* env = std::getenv("FROGGER_ROM"); env && *env) {
        return load_path(env);
    }
    if (auto set = load_path(default_user_dir())) return set;
    if (const char* home = std::getenv("HOME"); home && *home) {
        if (auto set = load_path(fs::path(home) / "Downloads" / "frogger.zip")) return set;
    }
    return std::nullopt;
}

bool run_frames(frogger::Machine& m, int frames) {
    m.run_cycles(frogger::kCpuPerFrame * frames);
    return !m.watchdog_reset;
}

uint16_t score_p1(frogger::Machine& m) {
    return uint16_t(m.mem_read(kP1Score) | (m.mem_read(kP1Score + 1) << 8));
}

bool is_hwtest(const frogger::Machine& m) {
    return m.ram[0] == 'T' && m.ram[1] == 'S' && m.ram[2] == 'T' && m.ram[3] == '1';
}

}  // namespace

TEST(Machine, UserRomInsertsCoinAndStarts) {
    auto set = load_user_set();
    if (!set) {
        GTEST_SKIP() << "no local frogger dump in roms/user/, FROGGER_ROM, or ~/Downloads/frogger.zip";
    }
    frogger::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(frogger::kCpuHz * 4);
    // Pulse coin 1 (IN0 bit 7) then 1P start (IN1 bit 7).
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    m.run_cycles(frogger::kCpuHz / 10);
    m.inputs.in0 = 0xFF;
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    m.run_cycles(frogger::kCpuHz / 5);
    m.inputs.in1 = 0xFC;
    m.run_cycles(frogger::kCpuHz);
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> rgb{};
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
        GTEST_SKIP() << "no local frogger dump in roms/user/, FROGGER_ROM, or ~/Downloads/frogger.zip";
    }
    frogger::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(frogger::kCpuHz * 4);
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    m.run_cycles(frogger::kCpuHz / 10);
    m.inputs.in0 = 0xFF;
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    m.run_cycles(frogger::kCpuHz / 5);
    m.inputs.in1 = 0xFC;
    m.audio.clear();
    m.run_cycles(frogger::kCpuHz);
    ASSERT_FALSE(m.audio.empty());
    bool any = false;
    for (float s : m.audio) {
        if (s != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any) << "Konami ROM should program the AY after coin+start";
    EXPECT_FALSE(m.ay.mute);
}

TEST(Machine, UserRomPlayfieldUsesKonamiPalette) {
    auto set = load_user_set();
    if (!set) {
        GTEST_SKIP() << "no local frogger dump in roms/user/, FROGGER_ROM, or ~/Downloads/frogger.zip";
    }
    frogger::Machine m;
    m.load_roms(*set);
    m.reset();
    m.run_cycles(frogger::kCpuHz * 4);
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    m.run_cycles(frogger::kCpuHz / 10);
    m.inputs.in0 = 0xFF;
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    m.run_cycles(frogger::kCpuHz / 5);
    m.inputs.in1 = 0xFC;
    m.run_cycles(frogger::kCpuHz);
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> rgb{};
    m.render(rgb.data());
    auto count = [&](int y0, int y1, auto pred) {
        int n = 0;
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < frogger::kUprightW; x++) {
                uint32_t p = rgb[unsigned(y * frogger::kUprightW + x)] & 0xFFFFFF;
                int r = int(p >> 16), g = int(p >> 8) & 0xFF, b = int(p & 0xFF);
                if (pred(r, g, b)) n++;
            }
        return n;
    };
    // Goal strip is green grass; log lanes are brown, not white.
    int grass = count(32, 48, [](int r, int g, int b) { return g > 140 && r < 80 && b < 40; });
    int brown = count(48, 112, [](int r, int g, int b) { return r > 140 && g > 60 && g < 160 && b < 120; });
    int white = count(48, 112, [](int r, int g, int b) { return r > 180 && g > 180 && b > 180; });
    EXPECT_GT(grass, 40) << "home row should be green grass";
    EXPECT_GT(brown, 80) << "logs should be brown";
    EXPECT_LT(white, brown) << "log lanes should not be dominated by white";
}

// Optional: CI never has a Konami dump, so this skips. Locally, drop a MAME
// frogger zip (or the loose chips) in roms/user/, set FROGGER_ROM, or leave
// frogger.zip in ~/Downloads. See FROGGER_REVIEW.md §8.
TEST(Machine, UserRomInsertsCoinStartsAndHops) {
    std::optional<frogger::RomSet> set;
    if (const char* env = std::getenv("FROGGER_ROM"); env && *env) {
        set = load_path(env);
        ASSERT_TRUE(set) << "FROGGER_ROM=" << env
                         << " is not a complete frogger set "
                            "(frogger.26/27 + frsm3.7 + 608/609/610 + 606/607 + pr-91.6l)";
    } else {
        set = load_user_set();
        if (!set) {
            GTEST_SKIP() << "no user ROM set — unzip a MAME frogger set into "
                            "roms/user/, set FROGGER_ROM, or leave frogger.zip in "
                            "~/Downloads (CI skips this on purpose; see FROGGER_REVIEW.md §8)";
        }
    }

    frogger::Machine m;
    m.load_roms(*set);
    m.reset();
    ASSERT_TRUE(run_frames(m, 240)) << "watchdog tripped during POST/attract";
    if (is_hwtest(m)) {
        GTEST_SKIP() << "that dump is the generated hardware self-test ROM, not Frogger";
    }

    const uint8_t cred0 = m.mem_read(kCredits);
    // Coin credits on the release edge (scanCoinInputAndCredit).
    m.inputs.in0 = uint8_t(0xFF & ~0x80);
    ASSERT_TRUE(run_frames(m, 30));
    m.inputs.in0 = 0xFF;
    ASSERT_TRUE(run_frames(m, 60));
    const uint8_t cred1 = m.mem_read(kCredits);
    ASSERT_GE(cred1, 1) << "coin did not increment credits at $83E1"
                        << " (before=" << int(cred0) << " after=" << int(cred1)
                        << " play=" << int(m.mem_read(kPlayFlag))
                        << " pc=" << m.main.pc << ")";
    ASSERT_LE(cred1, 0x99) << "credits at $83E1 look like garbage: " << int(cred1);

    // Hold 1P start (IN1 bit 7) until play flag / lives appear.
    m.inputs.in1 = uint8_t(0xFC & ~0x80);
    bool started = false;
    for (int i = 0; i < 600; i++) {
        ASSERT_TRUE(run_frames(m, 1));
        const uint8_t lives = m.mem_read(kLives);
        const uint8_t p1 = m.mem_read(kP1Lives);
        if (m.mem_read(kPlayFlag) != 0 ||
            (lives >= 1 && lives <= 5) ||
            (p1 >= 1 && p1 <= 5) ||
            m.mem_read(kCredits) < cred1) {
            started = true;
            break;
        }
    }
    m.inputs.in1 = 0xFC;
    ASSERT_TRUE(started) << "1P start did not take (lives=" << int(m.mem_read(kLives))
                         << " credits=" << int(m.mem_read(kCredits))
                         << " play=" << int(m.mem_read(kPlayFlag)) << ")";

    for (int i = 0; i < 300 && m.mem_read(kPlayFlag) == 0; i++) {
        ASSERT_TRUE(run_frames(m, 1));
    }
    ASSERT_NE(m.mem_read(kPlayFlag), 0) << "play flag at $83FE stayed attract";

    // Spawn sidewalk. Hold UP (IN2 bit 4) through the start jingle so the
    // first hop fires as soon as input is live.
    const uint8_t x0 = m.mem_read(kFrogX);
    const uint8_t y0 = m.mem_read(kFrogY);
    const uint16_t s0 = score_p1(m);
    const uint8_t row0 = m.mem_read(kFurthest);
    m.inputs.in2 = uint8_t(0xF1 & ~0x10);
    bool hopped = false;
    bool scored = false;
    for (int i = 0; i < 900; i++) {
        ASSERT_TRUE(run_frames(m, 1)) << "watchdog tripped during play";
        if (m.mem_read(kFrogX) != x0 || m.mem_read(kFrogY) != y0) hopped = true;
        if (score_p1(m) != s0 || m.mem_read(kFurthest) != row0) scored = true;
        if (hopped && scored) break;
    }
    EXPECT_TRUE(hopped) << "frog at $8044/$8047 did not hop"
                        << " (x=" << int(m.mem_read(kFrogX)) << " y=" << int(m.mem_read(kFrogY))
                        << " start x=" << int(x0) << " y=" << int(y0) << ")";
    EXPECT_TRUE(scored) << "P1 score $83ED / furthest-row $8269 stayed 0 after an UP hop"
                        << " (score=" << score_p1(m) << " row=" << int(m.mem_read(kFurthest)) << ")";
}
