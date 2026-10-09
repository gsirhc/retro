// SingleStepTests/z80 runner: per-instruction state, RAM, ports, and bus timing.
//   build:  make sst
//   run:    ./sst/sst_host sst/v1/*.json

#include "../cpu_z80.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json& operator[](const char* key) const {
        static const Json none;
        for (const auto& kv : obj)
            if (kv.first == key) return kv.second;
        return none;
    }
    int i() const { return int(num); }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    Json parse() {
        ws();
        Json v;
        switch (s_[p_]) {
            case '{': {
                v.kind = Json::Obj;
                ++p_;
                ws();
                if (s_[p_] == '}') { ++p_; return v; }
                for (;;) {
                    ws();
                    std::string k = str();
                    ws();
                    ++p_;  // ':'
                    v.obj.emplace_back(std::move(k), parse());
                    ws();
                    if (s_[p_++] == '}') return v;
                }
            }
            case '[': {
                v.kind = Json::Arr;
                ++p_;
                ws();
                if (s_[p_] == ']') { ++p_; return v; }
                for (;;) {
                    v.arr.push_back(parse());
                    ws();
                    if (s_[p_++] == ']') return v;
                }
            }
            case '"':
                v.kind = Json::Str;
                v.str = str();
                return v;
            case 'n':
                p_ += 4;
                return v;
            default: {
                v.kind = Json::Num;
                char* end = nullptr;
                v.num = std::strtod(s_.c_str() + p_, &end);
                p_ = size_t(end - s_.c_str());
                return v;
            }
        }
    }

private:
    const std::string& s_;
    size_t p_ = 0;
    void ws() { while (std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_; }
    std::string str() {
        std::string out;
        ++p_;
        while (s_[p_] != '"') out.push_back(s_[p_++]);
        ++p_;
        return out;
    }
};

struct Access {
    int t;
    char kind;  // r w i o
    uint16_t addr;
    uint8_t data;
};

struct Rig {
    std::map<uint16_t, uint8_t> mem;
    std::vector<Access> log;
    std::vector<std::pair<int, uint16_t>> refresh;
    std::vector<std::pair<uint16_t, uint8_t>> port_in;  // queued IN data
    size_t port_in_i = 0;
    uint64_t start = 0;
    std::unique_ptr<z80::Cpu> cpu;

    Rig() {
        z80::Bus bus;
        bus.read = [this](uint16_t a) {
            uint8_t v = mem.count(a) ? mem[a] : 0;
            log.push_back({t(), 'r', a, v});
            return v;
        };
        bus.write = [this](uint16_t a, uint8_t v) {
            mem[a] = v;
            log.push_back({t(), 'w', a, v});
        };
        bus.in = [this](uint16_t port) {
            uint8_t v = port_in_i < port_in.size() ? port_in[port_in_i++].second : 0xFF;
            log.push_back({t(), 'i', port, v});
            return v;
        };
        bus.out = [this](uint16_t port, uint8_t v) { log.push_back({t(), 'o', port, v}); };
        bus.refresh = [this](uint16_t a) { refresh.push_back({t(), a}); };
        cpu = std::make_unique<z80::Cpu>(bus);
    }
    int t() const { return int(cpu->cycles - start); }
};

void load_state(z80::Cpu& c, const Json& s) {
    c.pc = uint16_t(s["pc"].i());
    c.sp = uint16_t(s["sp"].i());
    c.a = uint8_t(s["a"].i());
    c.f = uint8_t(s["f"].i());
    c.b = uint8_t(s["b"].i());
    c.c = uint8_t(s["c"].i());
    c.d = uint8_t(s["d"].i());
    c.e = uint8_t(s["e"].i());
    c.h = uint8_t(s["h"].i());
    c.l = uint8_t(s["l"].i());
    c.i = uint8_t(s["i"].i());
    c.r = uint8_t(s["r"].i());
    c.ix = uint16_t(s["ix"].i());
    c.iy = uint16_t(s["iy"].i());
    c.wz = uint16_t(s["wz"].i());
    c.im = uint8_t(s["im"].i());
    c.q = uint8_t(s["q"].i());
    c.after_ei = s["ei"].i() != 0;
    c.after_ld_air = s["p"].i() != 0;
    c.iff1 = s["iff1"].i() != 0;
    c.iff2 = s["iff2"].i() != 0;
    int af_ = s["af_"].i(), bc_ = s["bc_"].i(), de_ = s["de_"].i(), hl_ = s["hl_"].i();
    c.a_ = uint8_t(af_ >> 8); c.f_ = uint8_t(af_);
    c.b_ = uint8_t(bc_ >> 8); c.c_ = uint8_t(bc_);
    c.d_ = uint8_t(de_ >> 8); c.e_ = uint8_t(de_);
    c.h_ = uint8_t(hl_ >> 8); c.l_ = uint8_t(hl_);
    c.halted = false;
}

struct Checker {
    std::string errors;
    void eq(const char* what, long got, long want) {
        if (got == want) return;
        char buf[96];
        std::snprintf(buf, sizeof buf, " %s=%04lX(want %04lX)", what, got, want);
        errors += buf;
    }
};

// Index of each strobe in the SST "cycles" list matches our T offset at the callback.
std::vector<Access> expected_accesses(const Json& test) {
    std::vector<Access> out;
    const Json& cyc = test["cycles"];
    for (size_t n = 0; n < cyc.arr.size(); ++n) {
        const Json& c = cyc.arr[n];
        const std::string& pins = c.arr[2].str;
        bool rd = pins[0] == 'r', wr = pins[1] == 'w', mreq = pins[2] == 'm', io = pins[3] == 'i';
        if (!rd && !wr) continue;
        uint16_t addr = uint16_t(c.arr[0].i());
        char kind = mreq ? (rd ? 'r' : 'w') : (io ? (rd ? 'i' : 'o') : '?');
        // Read data lands on the next sample.
        int data = rd ? (n + 1 < cyc.arr.size() ? cyc.arr[n + 1].arr[1].i() : 0) : c.arr[1].i();
        out.push_back({int(n), kind, addr, uint8_t(data)});
    }
    return out;
}

bool run_test(const Json& test, std::string& why) {
    Rig rig;
    z80::Cpu& c = *rig.cpu;
    const Json& ini = test["initial"];
    const Json& fin = test["final"];
    for (const Json& m : ini["ram"].arr) rig.mem[uint16_t(m.arr[0].i())] = uint8_t(m.arr[1].i());
    for (const Json& pt : test["ports"].arr)
        if (pt.arr[2].str == "r") rig.port_in.push_back({uint16_t(pt.arr[0].i()), uint8_t(pt.arr[1].i())});
    load_state(c, ini);
    rig.start = c.cycles;

    const int want_t = int(test["cycles"].arr.size());
    c.step();

    Checker k;
    k.eq("pc", c.pc, fin["pc"].i());
    k.eq("sp", c.sp, fin["sp"].i());
    k.eq("a", c.a, fin["a"].i());
    k.eq("f", c.f, fin["f"].i());
    k.eq("b", c.b, fin["b"].i());
    k.eq("c", c.c, fin["c"].i());
    k.eq("d", c.d, fin["d"].i());
    k.eq("e", c.e, fin["e"].i());
    k.eq("h", c.h, fin["h"].i());
    k.eq("l", c.l, fin["l"].i());
    k.eq("i", c.i, fin["i"].i());
    k.eq("r", c.r, fin["r"].i());
    k.eq("ix", c.ix, fin["ix"].i());
    k.eq("iy", c.iy, fin["iy"].i());
    k.eq("wz", c.wz, fin["wz"].i());
    k.eq("q", c.q, fin["q"].i());
    k.eq("im", c.im, fin["im"].i());
    k.eq("ei", c.after_ei, fin["ei"].i());
    k.eq("p", c.after_ld_air, fin["p"].i());
    k.eq("iff1", c.iff1, fin["iff1"].i());
    k.eq("iff2", c.iff2, fin["iff2"].i());
    k.eq("af_", (c.a_ << 8) | c.f_, fin["af_"].i());
    k.eq("bc_", (c.b_ << 8) | c.c_, fin["bc_"].i());
    k.eq("de_", (c.d_ << 8) | c.e_, fin["de_"].i());
    k.eq("hl_", (c.h_ << 8) | c.l_, fin["hl_"].i());
    k.eq("T", rig.t(), want_t);
    for (const Json& m : fin["ram"].arr) {
        uint16_t a = uint16_t(m.arr[0].i());
        k.eq("ram", rig.mem.count(a) ? rig.mem[a] : 0, m.arr[1].i());
    }

    std::vector<Access> want = expected_accesses(test);
    k.eq("accesses", long(rig.log.size()), long(want.size()));
    for (size_t n = 0; n < want.size() && n < rig.log.size(); ++n) {
        const Access& g = rig.log[n];
        const Access& w = want[n];
        if (g.t != w.t || g.kind != w.kind || g.addr != w.addr || g.data != w.data) {
            char buf[128];
            std::snprintf(buf, sizeof buf, " bus[%zu]=%c@%d %04X:%02X(want %c@%d %04X:%02X)", n,
                          g.kind, g.t, g.addr, g.data, w.kind, w.t, w.addr, w.data);
            k.errors += buf;
            break;
        }
    }
    const Json& cyc = test["cycles"];
    for (const auto& rf : rig.refresh) {
        if (rf.first < 0 || size_t(rf.first) >= cyc.arr.size()) continue;
        k.eq("rfsh", rf.second, cyc.arr[size_t(rf.first)].arr[0].i());
    }

    why = k.errors;
    return why.empty();
}

}  // namespace

int main(int argc, char** argv) {
    // SST_LIMIT caps tests per file (nightly runs all 1000).
    int limit = std::getenv("SST_LIMIT") ? std::atoi(std::getenv("SST_LIMIT")) : 1000;
    long pass = 0, fail = 0;
    int bad_files = 0;
    for (int n = 1; n < argc; ++n) {
        std::ifstream in(argv[n], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "sst_host: cannot open %s\n", argv[n]);
            return 1;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Json tests = Parser(text).parse();
        int file_fail = 0;
        std::string first;
        int count = 0;
        for (const Json& t : tests.arr) {
            if (count++ >= limit) break;
            std::string why;
            if (run_test(t, why)) {
                ++pass;
            } else {
                ++fail;
                if (file_fail++ == 0) first = t["name"].str + ":" + why;
            }
        }
        if (file_fail) {
            ++bad_files;
            std::printf("FAIL %-22s %4d  %s\n", argv[n], file_fail, first.c_str());
        }
    }
    std::printf("sst: %ld passed, %ld failed, %d files with failures\n", pass, fail, bad_files);
    return fail ? 3 : 0;
}
