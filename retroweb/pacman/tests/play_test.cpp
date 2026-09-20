#include <gtest/gtest.h>

#include "machine.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Same board-socket names and sizes as web/app.js identifySet. CRC is not
// a whitelist — a complete original-board dump is enough. 82s126.3m is the
// unused timing PROM and is ignored.
const std::regex kChipRe{R"((?:^|[._-])(6e|6f|6h|6j|5e|5f|7f|4a|1m)(?:[^a-z0-9]|$))",
                         std::regex::icase};
const std::regex kIgnoreRe{R"((?:^|[._-])3m(?:[^a-z0-9]|$))", std::regex::icase};

constexpr size_t kProgram = 0x4000;
constexpr size_t kBank = 0x1000;
constexpr size_t kTiles = 0x1000;
constexpr size_t kSprites = 0x1000;
constexpr size_t kColor = 0x20;
constexpr size_t kLookup = 0x100;
constexpr size_t kWave = 0x100;

// Work-RAM cells the original Pac-Man program keeps (Data Crystal
// "Pac-Man (Arcade)/RAM map"; same labels in the Midway disassembly
// comments at cubeman.org/arcade-source/mspac.asm). $4D08/$4D09 is the
// player sprite Y/X word — collision uses `ld ix,$4D08`.
constexpr uint16_t kPacY = 0x4D08;
constexpr uint16_t kPacX = 0x4D09;
constexpr uint16_t kLives = 0x4E14;
constexpr uint16_t kCredits = 0x4E6E;
constexpr uint16_t kP1Score0 = 0x4E80;

std::string basename_lower(const std::string& path) {
    auto n = fs::path(path).filename().string();
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

std::string role_of(const std::string& path) {
    const std::string n = basename_lower(path);
    if (n == "program.bin" || n == "prg.bin") return "program";
    if (std::regex_search(n, kIgnoreRe)) return "ignore";
    std::smatch m;
    if (std::regex_search(n, m, kChipRe)) {
        std::string r = m[1].str();
        for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
    return {};
}

const uint8_t* clip(const std::vector<uint8_t>& b, size_t n) {
    if (b.size() < n) return nullptr;
    return b.data();
}

void copy_n(uint8_t* dst, const uint8_t* src, size_t n) {
    std::copy(src, src + n, dst);
}

std::optional<pacman::RomSet> identify(const std::map<std::string, std::vector<uint8_t>>& files) {
    std::map<std::string, const std::vector<uint8_t>*> found;
    for (const auto& [name, data] : files) {
        const std::string role = role_of(name);
        if (role.empty() || role == "ignore") continue;
        if (found.count(role) && found[role]->size() >= data.size()) continue;
        found[role] = &data;
    }
    pacman::RomSet set;
    if (auto it = found.find("program"); it != found.end()) {
        const uint8_t* p = clip(*it->second, kProgram);
        if (!p) return std::nullopt;
        copy_n(set.program.data(), p, kProgram);
    } else {
        const char* banks[] = {"6e", "6f", "6h", "6j"};
        for (int i = 0; i < 4; i++) {
            auto b = found.find(banks[i]);
            if (b == found.end()) return std::nullopt;
            const uint8_t* p = clip(*b->second, kBank);
            if (!p) return std::nullopt;
            copy_n(set.program.data() + i * kBank, p, kBank);
        }
    }
    auto take = [&](const char* role, uint8_t* dst, size_t n) -> bool {
        auto it = found.find(role);
        if (it == found.end()) return false;
        const uint8_t* p = clip(*it->second, n);
        if (!p) return false;
        copy_n(dst, p, n);
        return true;
    };
    if (!take("5e", set.tiles.data(), kTiles)) return std::nullopt;
    if (!take("5f", set.sprites.data(), kSprites)) return std::nullopt;
    if (!take("7f", set.color_prom.data(), kColor)) return std::nullopt;
    if (!take("4a", set.lookup_prom.data(), kLookup)) return std::nullopt;
    if (!take("1m", set.wave_prom.data(), kWave)) return std::nullopt;
    return set;
}

std::optional<std::vector<uint8_t>> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto n = static_cast<size_t>(in.tellg());
    if (n == 0 || n > 2 * 1024 * 1024) return std::nullopt;
    in.seekg(0);
    std::vector<uint8_t> b(n);
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    if (!in) return std::nullopt;
    return b;
}

bool is_zip_name(const std::string& n) {
    if (n.size() < 4) return false;
    auto ext = n.substr(n.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".zip";
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

std::map<std::string, std::vector<uint8_t>> collect_chips(const fs::path& dir) {
    std::map<std::string, std::vector<uint8_t>> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec || !it->is_regular_file()) continue;
        auto data = read_file(it->path());
        if (!data) continue;
        files[it->path().string()] = std::move(*data);
    }
    return files;
}

std::optional<pacman::RomSet> load_zip(const fs::path& zip) {
    TmpDir tmp;
    tmp.p = fs::temp_directory_path() /
            ("pacman-rom-" + std::to_string(std::rand()) + "-" + zip.filename().string());
    std::error_code ec;
    fs::create_directories(tmp.p, ec);
    if (ec) return std::nullopt;
    // python3 is already a test dependency (gen_hwtest.py). MAME zips are
    // deflate; this avoids a zlib CMake dependency just for one optional test.
    const std::string cmd = "python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' " +
                            sh_quote(zip.string()) + " " + sh_quote(tmp.p.string());
    if (std::system(cmd.c_str()) != 0) return std::nullopt;
    return identify(collect_chips(tmp.p));
}

std::optional<pacman::RomSet> load_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    if (fs::is_regular_file(p) && is_zip_name(p.filename().string())) return load_zip(p);
    if (!fs::is_directory(p)) return std::nullopt;
    if (auto set = identify(collect_chips(p))) return set;
    for (auto it = fs::directory_iterator(p, ec); it != fs::directory_iterator(); ++it) {
        if (ec || !it->is_regular_file()) continue;
        if (is_zip_name(it->path().filename().string())) {
            if (auto set = load_zip(it->path())) return set;
        }
    }
    return std::nullopt;
}

fs::path default_user_dir() {
    return fs::path(__FILE__).parent_path().parent_path() / "roms" / "user";
}

bool run_cycles(pacman::Machine& m, int n) {
    m.run_cycles(n);
    return !m.watchdog_reset;
}

bool run_frames(pacman::Machine& m, int frames) {
    return run_cycles(m, pacman::kCpuPerFrame * frames);
}

bool score_nonzero(pacman::Machine& m) {
    // BCD, low byte first ($4E80 CC / $4E81 BB / $4E82 AA).
    return m.mem_read(kP1Score0) || m.mem_read(kP1Score0 + 1) || m.mem_read(kP1Score0 + 2);
}

}  // namespace

// Optional: CI never has a Namco dump, so this skips. Locally, drop a MAME
// pacman/puckman zip (or the loose chips) in roms/user/ or set PACMAN_ROM.
// See PACMAN_REVIEW.md §8.
TEST(Machine, UserRomInsertsCoinStartsAndEatsAPellet) {
    std::optional<pacman::RomSet> set;
    if (const char* env = std::getenv("PACMAN_ROM"); env && *env) {
        set = load_path(env);
        ASSERT_TRUE(set) << "PACMAN_ROM=" << env
                         << " is not a complete original-board set "
                            "(16K program or 6e/6f/6h/6j + 5e/5f + 7f/4a/1m)";
    } else {
        set = load_path(default_user_dir());
        if (!set) {
            GTEST_SKIP() << "no user ROM set — unzip a MAME pacman/puckman set into "
                            "roms/user/ or set PACMAN_ROM to a zip/directory "
                            "(CI skips this on purpose; see PACMAN_REVIEW.md §8)";
        }
    }

    pacman::Machine m;
    m.load_roms(*set);
    m.watchdog_reset = false;
    m.reset();
    ASSERT_EQ(m.program[0], 0xF3) << "loaded program does not start with Pac-Man's DI";

    // POST checksum plus attract; 8 s of emulated time is plenty.
    ASSERT_TRUE(run_frames(m, 480)) << "watchdog tripped during POST/attract";
    if (m.ram[0x4C00 - 0x4800] == 'T' && m.ram[0x4C01 - 0x4800] == 'S' &&
        m.ram[0x4C02 - 0x4800] == 'T' && m.ram[0x4C03 - 0x4800] == '1') {
        GTEST_SKIP() << "that dump is the generated hardware self-test ROM, not Pac-Man";
    }

    const uint8_t cred0 = m.mem_read(kCredits);
    // Debounce wants a released→pressed edge held 2 frames (history $0C);
    // hold well past that, then settle so the pending-coin path can DAA
    // into $4E6E.
    m.inputs.in0 = static_cast<uint8_t>(0xFF & ~0x20);
    ASSERT_TRUE(run_frames(m, 30));
    m.inputs.in0 = 0xFF;
    ASSERT_TRUE(run_frames(m, 60));
    const uint8_t cred1 = m.mem_read(kCredits);
    ASSERT_GE(cred1, 1) << "coin did not increment credits at $4E6E"
                        << " (before=" << int(cred0) << " after=" << int(cred1)
                        << " mode=" << int(m.mem_read(0x4E00))
                        << " pc=" << m.cpu.pc << ")";
    ASSERT_LE(cred1, 9) << "credits at $4E6E look like garbage, not a coin count: "
                        << int(cred1);

    // Hold 1P start until lives appear or the credit is spent. A short pulse
    // is easy to miss during attract-demo; a real cabinet button stays down.
    m.inputs.in1 = static_cast<uint8_t>(0xFF & ~0x20);
    bool started = false;
    for (int i = 0; i < 600; i++) {
        ASSERT_TRUE(run_frames(m, 1));
        const uint8_t lives = m.mem_read(kLives);
        const uint8_t disp = m.mem_read(0x4E15);
        if ((lives >= 1 && lives <= 5) || (disp >= 1 && disp <= 5) ||
            m.mem_read(kCredits) < cred1) {
            started = true;
            break;
        }
    }
    m.inputs.in1 = 0xFF;
    ASSERT_TRUE(started) << "1P start did not take (lives=" << int(m.mem_read(kLives))
                         << " credits=" << int(m.mem_read(kCredits))
                         << " mode=" << int(m.mem_read(0x4E00)) << ")";
    EXPECT_LT(m.mem_read(kCredits), cred1) << "start should spend the credit";

    // Pac-Man faces left at spawn. Hold LEFT through the jingle + READY so
    // the first pellets are eaten as soon as the maze is live.
    m.inputs.in0 = static_cast<uint8_t>(0xFF & ~0x02);
    const uint8_t x0 = m.mem_read(kPacX);
    const uint8_t y0 = m.mem_read(kPacY);
    bool ate = false;
    bool moved = false;
    for (int i = 0; i < 900; i++) {  // ~15 s
        ASSERT_TRUE(run_frames(m, 1)) << "watchdog tripped during play";
        if (score_nonzero(m)) ate = true;
        if (m.mem_read(kPacX) != x0 || m.mem_read(kPacY) != y0) moved = true;
        if (ate && moved) break;
    }
    EXPECT_TRUE(moved) << "Pac-Man sprite at $4D08/$4D09 did not move";
    EXPECT_TRUE(score_nonzero(m)) << "P1 score at $4E80 stayed 0 — no pellet eaten";
}
