#include <gtest/gtest.h>

#include "machine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    Hold() = default;
    Hold(Hold&& o) noexcept : dir(std::move(o.dir)), tmp(std::move(o.tmp)) { o.tmp.clear(); }
    Hold(const Hold&) = delete;
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
    return std::optional<Hold>(std::move(h));
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
    // The 51XX ignores coins until POST ends and the game enters credit mode.
    m.run_cycles(galaga::kCpuHz * 20);
    EXPECT_EQ(m.coin_counter[0], 0);
    m.inputs.in1 = 0x10;
    m.run_cycles(galaga::kCpuHz / 10);
    m.inputs.in1 = 0x04;
    m.run_cycles(galaga::kCpuHz);
    EXPECT_EQ(m.coin_counter[0], 1);
    EXPECT_EQ(m.coin_counter[1], 0);
    EXPECT_GT(m.frames, 60);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, UserRom54xxPlaysThroughItsDacs) {
    auto hold = find_dump();
    if (!hold) GTEST_SKIP() << "no local galagamw dump";
    const fs::path& dir = hold->dir;
    galaga::RomSet set;
    if (!read_file(dir / "3200a.bin", set.main.data(), 0x1000) ||
        !read_file(dir / "3300b.bin", set.main.data() + 0x1000, 0x1000) ||
        !read_file(dir / "3400c.bin", set.main.data() + 0x2000, 0x1000) ||
        !read_file(dir / "3500d.bin", set.main.data() + 0x3000, 0x1000) ||
        !read_file(dir / "3600e.bin", set.sub.data(), 0x1000) ||
        !read_file(dir / "3700g.bin", set.sound.data(), 0x1000) ||
        !read_file(dir / "54xx.bin", set.mcu54.data(), set.mcu54.size())) {
        GTEST_SKIP() << "set lacks 54xx.bin";
    }
    set.has54 = true;
    read_file(dir / "51xx.bin", set.mcu51.data(), set.mcu51.size()) && (set.has51 = true);
    galaga::Machine m;
    m.load_roms(set);
    m.reset();
    int changes = 0;
    uint8_t last[3] = {m.dac54(0), m.dac54(1), m.dac54(2)};
    for (int i = 0; i < galaga::kCpuHz * 15 / 64; i++) {
        m.run_cycles(64);
        for (int ch = 0; ch < 3; ch++) {
            if (m.dac54(ch) != last[ch]) changes++;
            last[ch] = m.dac54(ch);
        }
    }
    EXPECT_GT(changes, 50);
}

namespace {

// Finds "CREDIT" in tile codes (A = 0x0A) and returns the digit two cells on, or -1.
int credit_digit(const galaga::Machine& m) {
    static const uint8_t word[] = {0x0C, 0x1B, 0x0E, 0x0D, 0x12, 0x1D};
    for (int stride : {1, -1, 32, -32}) {
        for (int start = 0; start < 0x400; start++) {
            bool hit = true;
            for (int k = 0; k < 6 && hit; k++) {
                int a = start + k * stride;
                hit = a >= 0 && a < 0x400 && m.video.videoram[unsigned(a)] == word[k];
            }
            int d = start + 7 * stride;
            if (hit && d >= 0 && d < 0x400) return m.video.videoram[unsigned(d)];
        }
    }
    return -1;
}

}  // namespace

TEST(Machine, UserRom51xxCreditsReachTheMainCpu) {
    auto hold = find_dump();
    if (!hold) GTEST_SKIP() << "no local galagamw dump";
    const fs::path& dir = hold->dir;
    galaga::RomSet set;
    if (!read_file(dir / "3200a.bin", set.main.data(), 0x1000) ||
        !read_file(dir / "3300b.bin", set.main.data() + 0x1000, 0x1000) ||
        !read_file(dir / "3400c.bin", set.main.data() + 0x2000, 0x1000) ||
        !read_file(dir / "3500d.bin", set.main.data() + 0x3000, 0x1000) ||
        !read_file(dir / "3600e.bin", set.sub.data(), 0x1000) ||
        !read_file(dir / "3700g.bin", set.sound.data(), 0x1000) ||
        !read_file(dir / "51xx.bin", set.mcu51.data(), set.mcu51.size())) {
        GTEST_SKIP() << "set lacks 51xx.bin";
    }
    set.has51 = true;
    read_file(dir / "2600j.bin", set.tiles.data(), set.tiles.size());
    galaga::Machine m;
    m.load_roms(set);
    m.reset();
    m.run_cycles(galaga::kCpuHz * 20);
    for (int i = 0; i < 3; i++) {
        m.inputs.in1 = 0x10;
        m.run_cycles(galaga::kCpuHz / 10);
        m.inputs.in1 = 0;
        m.run_cycles(galaga::kCpuHz / 5);
    }
    m.run_cycles(galaga::kCpuHz);
    // The main CPU reads the genuine 51XX's credit count over the 06XX and prints it.
    EXPECT_EQ(credit_digit(m), 3);
}

namespace {

// Drives the 06XX the way the main CPU's NMI handler does, against either the 51xx.bin core or the fallback.
class Rig51 {
public:
    Rig51(const fs::path& dir, bool genuine) {
        galaga::RomSet set;
        read_file(dir / "51xx.bin", set.mcu51.data(), set.mcu51.size());
        set.has51 = genuine;
        m.load_roms(set);
        const uint8_t spin[] = {0x31, 0x00, 0x8C, 0x18, 0xFE};
        std::copy(std::begin(spin), std::end(spin), m.rom_main.begin());
        m.rom_main[0x66] = 0xED;
        m.rom_main[0x67] = 0x45;
        m.reset();
        m.mem_write(0x6823, 1);
        run(200000);
    }

    void run(int n) {
        while (n > 0) {
            int k = std::min(n, 20000);
            m.run_cycles(k);
            n -= k;
            m.mem_write(0x6830, 0);
        }
    }

    void until(uint64_t t) {
        while (m.main.cycles < t) run(int(std::min<uint64_t>(t - m.main.cycles, 20000)));
    }

    void settle() { run(300000); }

    // Control $A1 selects the 51XX every 2048 T-states; each byte goes out between selects.
    void send(const std::vector<int>& bytes) {
        m.mem_write(0x7100, 0xA1);
        run(1100);
        for (int b : bytes) {
            m.mem_write(0x7000, uint8_t(b));
            run(2048);
        }
        m.mem_write(0x7100, 0x10);
        run(300);
        settle();
    }

    void tap(int bit) {
        m.inputs.in1 = uint8_t(m.inputs.in1 | bit);
        run(300000);
        m.inputs.in1 = uint8_t(m.inputs.in1 & ~bit);
        run(300000);
    }

    // Control $71 selects every 512 T-states from a fixed phase. The first select raises no NMI, so the handler
    // reads after selects 2 to 4, which keeps the 51XX's four-byte ring aligned from one pass to the next.
    std::string read3() {
        run(250000);
        m.mem_write(0x7100, 0x71);
        const uint64_t first = ((m.main.cycles - 256) / 512 + 1) * 512 + 256;
        uint8_t v[3];
        for (int k = 0; k < 3; k++) {
            until(first + 512 * uint64_t(k + 1) + 60);
            v[k] = m.mem_read(0x7000);
        }
        until(first + 1536 + 120);
        m.mem_write(0x7100, 0x10);
        run(300);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02X %02X %02X", v[0], v[1], v[2]);
        return buf;
    }

    galaga::Machine m;
};

using Script = std::function<void(Rig51&, std::vector<std::string>&)>;

std::vector<std::string> run_script(const fs::path& dir, bool genuine, const std::vector<int>& setup, const Script& s) {
    Rig51 r(dir, genuine);
    r.send(setup);
    std::vector<std::string> out;
    s(r, out);
    return out;
}

}  // namespace

TEST(Machine, UserRom51xxAndFallbackAnswerTheMainCpuAlike) {
    auto hold = find_dump();
    if (!hold) GTEST_SKIP() << "no local galagamw dump";
    const fs::path& dir = hold->dir;
    uint8_t probe[galaga::kMcuRomBytes];
    if (!read_file(dir / "51xx.bin", probe, sizeof probe)) GTEST_SKIP() << "set lacks 51xx.bin";

    struct Case {
        const char* name = "";
        std::vector<int> setup;
        Script script;
    };
    const std::vector<int> credit_mode = {1, 1, 1, 1, 1, 2, 3, 0};
    std::vector<Case> cases;
    cases.push_back({"switch mode", {5, 5, 5, 5}, [](Rig51& r, auto& o) {
        o.push_back(r.read3());
        r.m.inputs.in0 = 0x5A;
        r.m.inputs.in1 = 0x35;
        o.push_back(r.read3());
        r.m.inputs.in0 = 0;
        r.m.inputs.in1 = 0x10;
        o.push_back(r.read3());
        r.m.inputs.in1 = 0;
        o.push_back(r.read3());
        r.m.inputs.in1 = 0x80;
        o.push_back(r.read3());
        r.m.inputs.in1 = 0;
    }});
    for (int z = 0; z < 4; z++) {
        std::vector<int> setup = {1, 1, 1, 1, 1, 2, 3, 0};
        setup[1 + z] = 0;
        cases.push_back({"zero coinage argument", setup, [](Rig51& r, auto& o) {
            o.push_back(r.read3());
            r.tap(0x10);
            o.push_back(r.read3());
            r.tap(0x20);
            o.push_back(r.read3());
            r.tap(0x04);
            o.push_back(r.read3());
        }});
    }
    cases.push_back({"2 coins per credit, both chutes and the service coin", {1, 2, 1, 1, 1, 2, 3, 0}, [](Rig51& r, auto& o) {
        o.push_back(r.read3());
        r.tap(0x10);
        o.push_back(r.read3());
        r.tap(0x10);
        o.push_back(r.read3());
        r.tap(0x20);
        o.push_back(r.read3());
        r.tap(0x20);
        o.push_back(r.read3());
        r.tap(0x40);
        o.push_back(r.read3());
    }});
    cases.push_back({"2 credits per coin A", {1, 1, 2, 1, 1, 2, 3, 0}, [](Rig51& r, auto& o) {
        r.tap(0x10);
        o.push_back(r.read3());
        r.tap(0x10);
        o.push_back(r.read3());
        r.tap(0x20);
        o.push_back(r.read3());
        r.tap(0x20);
        o.push_back(r.read3());
    }});
    cases.push_back({"3 credits per coin B", {1, 1, 1, 1, 3, 2, 3, 0}, [](Rig51& r, auto& o) {
        r.tap(0x20);
        o.push_back(r.read3());
        r.tap(0x10);
        o.push_back(r.read3());
    }});
    cases.push_back({"START needs credits, START 2 needs two", credit_mode, [](Rig51& r, auto& o) {
        r.tap(0x04);
        o.push_back(r.read3());
        r.tap(0x08);
        o.push_back(r.read3());
        r.tap(0x10);
        r.tap(0x08);
        o.push_back(r.read3());
        r.tap(0x04);
        o.push_back(r.read3());
    }});
    cases.push_back({"both STARTs, a game under way, credit mode again", credit_mode, [](Rig51& r, auto& o) {
        r.tap(0x10);
        r.tap(0x10);
        r.tap(0x10);
        r.m.inputs.in1 = 0x0C;
        o.push_back(r.read3());
        r.m.inputs.in1 = 0;
        o.push_back(r.read3());
        r.send({2, 3, 0});
        o.push_back(r.read3());
        r.tap(0x08);
        o.push_back(r.read3());
        r.tap(0x04);
        o.push_back(r.read3());
    }});
    cases.push_back({"credits stop at 99", credit_mode, [](Rig51& r, auto& o) {
        for (int i = 0; i < 12; i++) r.tap(0x40);
        o.push_back(r.read3());
    }});
    for (int cmd : {3, 4}) {
        cases.push_back({cmd == 3 ? "stick and buttons, command 03" : "stick and buttons, command 04", {1, 1, 1, 1, 1, 2, cmd, 0},
                         [](Rig51& r, auto& o) {
            r.tap(0x10);
            r.tap(0x04);
            o.push_back(r.read3());
            for (int v = 0; v < 16; v++) {
                r.m.inputs.in0 = uint8_t(v);
                o.push_back(r.read3());
                r.m.inputs.in0 = uint8_t(v << 4);
                o.push_back(r.read3());
            }
            r.m.inputs.in0 = 0;
            for (int bit : {0x01, 0x02, 0x03, 0x04, 0x08}) {
                r.m.inputs.in1 = uint8_t(bit);
                o.push_back(r.read3());
            }
            r.m.inputs.in1 = 0;
        }});
    }

    int reads = 0;
    for (const Case& c : cases) {
        auto genuine = run_script(dir, true, c.setup, c.script);
        auto fallback = run_script(dir, false, c.setup, c.script);
        ASSERT_EQ(genuine.size(), fallback.size()) << c.name;
        for (size_t i = 0; i < genuine.size(); i++) {
            EXPECT_EQ(fallback[i], genuine[i]) << c.name << ", read " << i;
            reads++;
        }
    }
    EXPECT_GT(reads, 100);
}
