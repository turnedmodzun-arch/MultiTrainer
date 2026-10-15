// Tests for the memory scanner against fake memory (no sockets, runs on every platform).
#include "../src/scanner.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL: %s\n", msg); g_failures++; }           \
        else         { std::printf("ok:   %s\n", msg); }                         \
    } while (0)

static const uint32_t kBase = 0x82000000;

struct FakeMem
{
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x6000, 0);
    uint32_t badLo = 0, badHi = 0;  // unreadable range [badLo, badHi)
    bool dead = false;              // simulates a dropped connection
    int reads = 0;

    void put32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a - kBase + i] = (uint8_t)(v >> (24 - 8 * i)); }
    void putf(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(a, u); }
    void putBytes(uint32_t a, const std::vector<uint8_t> &b) { std::memcpy(&mem[a - kBase], b.data(), b.size()); }

    ScanIO io()
    {
        ScanIO s;
        s.read = [this](uint32_t a, uint32_t n, std::vector<uint8_t> &out, std::string &err) {
            reads++;
            if (dead) { err = "connection lost"; return false; }
            if (a < kBase || (uint64_t)a + n > kBase + mem.size()) { err = "out of range"; return false; }
            if (badHi && a < badHi && (uint64_t)a + n > badLo) { err = "402- not mapped"; return false; }
            out.assign(mem.begin() + (a - kBase), mem.begin() + (a - kBase) + n);
            return true;
        };
        s.alive = [this] { return !dead; };
        return s;
    }
};

static std::vector<uint8_t> pat(ValType t, const char *s)
{
    std::vector<uint8_t> v;
    std::string e;
    if (!parseValue(t, s, v, e)) std::printf("(parse error in test: %s)\n", e.c_str());
    return v;
}

int main()
{
    std::string err;

    // ---- parse / format
    {
        std::vector<uint8_t> v;
        CHECK(parseValue(ValType::Int32, "100", v, err) && v == std::vector<uint8_t>({0, 0, 0, 100}), "int32 decimal");
        CHECK(parseValue(ValType::Int32, "0xDEADBEEF", v, err) && v == std::vector<uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}), "int32 hex above INT_MAX");
        CHECK(parseValue(ValType::Int32, "-1", v, err) && v == std::vector<uint8_t>({0xFF, 0xFF, 0xFF, 0xFF}), "int32 negative");
        CHECK(!parseValue(ValType::Int32, "4294967296", v, err) && !err.empty(), "int32 out of range rejected");
        CHECK(!parseValue(ValType::Int8, "256", v, err) && parseValue(ValType::Int8, "255", v, err), "int8 range");
        CHECK(!parseValue(ValType::Int16, "12abc", v, err) && !parseValue(ValType::Int16, "", v, err), "junk and empty rejected");
        CHECK(parseValue(ValType::Float, "1.5", v, err) && v == std::vector<uint8_t>({0x3F, 0xC0, 0, 0}), "float big-endian");
        CHECK(!parseValue(ValType::Float, "nan", v, err), "non-finite float rejected");
        CHECK(parseValue(ValType::Bytes, "de AD 01", v, err) && v == std::vector<uint8_t>({0xDE, 0xAD, 1}), "bytes");
        CHECK(!parseValue(ValType::Bytes, "DE A", v, err), "odd hex rejected");
        CHECK(parseValue(ValType::Text, "hi there", v, err) && v.size() == 8, "text keeps spaces");
        CHECK(formatValue(ValType::Int32, {0, 0, 0, 100}) == "100 (0x00000064)", "format int32");
        CHECK(formatValue(ValType::Int32, {0xFF, 0xFF, 0xFF, 0xFF}) == "4294967295 (signed -1, 0xFFFFFFFF)", "format negative int32");
        CHECK(formatValue(ValType::Float, {0x3F, 0xC0, 0, 0}) == "1.5", "format float");
        CHECK(formatValue(ValType::Text, {'a', 0, 'b'}) == "a.b", "format text");
    }

    // ---- first scan
    {
        FakeMem m;
        m.put32(kBase + 0x100, 0x12345678);
        m.put32(kBase + 0x2004, 0x12345678);
        m.putBytes(kBase + 0x1FFE, {0x12, 0x34, 0x56, 0x78});  // unaligned: bytes 1234 5678 at ..FFE
        ScanResults r;
        auto p = pat(ValType::Int32, "0x12345678");
        CHECK(firstScan(m.io(), kBase, kBase + 0x6000, ValType::Int32, p, true, 1000, r, nullptr, nullptr, err) &&
                  r.hits.size() == 2 && r.hits[0].addr == kBase + 0x100 && r.hits[1].addr == kBase + 0x2004,
              "aligned int32 scan finds only aligned matches, ascending");
        CHECK(firstScan(m.io(), kBase, kBase + 0x6000, ValType::Int32, p, false, 1000, r, nullptr, nullptr, err) &&
                  r.hits.size() == 3 && r.hits[1].addr == kBase + 0x1FFE,
              "unaligned scan also finds the match straddling a 4 KB block boundary");

        // straddling match must not be reported twice
        auto b5 = std::vector<uint8_t>{1, 2, 3, 4, 5};
        m.putBytes(kBase + 0x2FFD, b5);
        CHECK(firstScan(m.io(), kBase, kBase + 0x6000, ValType::Bytes, b5, true, 1000, r, nullptr, nullptr, err) &&
                  r.hits.size() == 1 && r.hits[0].addr == kBase + 0x2FFD,
              "bytes match across a block boundary is reported once");

        // range ends mid-value: match must lie wholly inside [start, end)
        CHECK(firstScan(m.io(), kBase, kBase + 0x2001, ValType::Int32, p, false, 1000, r, nullptr, nullptr, err) &&
                  r.hits.size() == 1 && r.hits[0].addr == kBase + 0x100,
              "matches that would run past the end of the range are excluded");

        auto t = pat(ValType::Text, "Hello");
        m.putBytes(kBase + 0x4010, {'H', 'e', 'l', 'l', 'o'});
        CHECK(firstScan(m.io(), kBase, kBase + 0x6000, ValType::Text, t, true, 10, r, nullptr, nullptr, err) &&
                  r.hits.size() == 1 && r.hits[0].addr == kBase + 0x4010,
              "text search");

        CHECK(firstScan(m.io(), kBase, kBase + 0x6000, ValType::Int32, pat(ValType::Int32, "0"), true, 50, r, nullptr, nullptr, err) &&
                  r.hits.size() == 50 && r.truncated,
              "maxHits truncates and says so");
        CHECK(!firstScan(m.io(), kBase + 10, kBase + 10, ValType::Int32, p, true, 10, r, nullptr, nullptr, err), "empty range rejected");
    }

    // ---- unreadable memory and dropped connections
    {
        FakeMem m;
        m.put32(kBase + 0x0FF0, 0xCAFEBABE);  // just before the bad page
        m.put32(kBase + 0x2800, 0xCAFEBABE);  // after it
        m.badLo = kBase + 0x1000;
        m.badHi = kBase + 0x2000;
        ScanResults r;
        auto p = pat(ValType::Int32, "0xCAFEBABE");
        CHECK(firstScan(m.io(), kBase, kBase + 0x4000, ValType::Int32, p, true, 100, r, nullptr, nullptr, err) &&
                  r.hits.size() == 2 && r.skippedBlocks == 1,
              "unreadable block is skipped and counted; neighbours (incl. the overlap retry) still scanned");
        m.dead = true;
        CHECK(!firstScan(m.io(), kBase, kBase + 0x4000, ValType::Int32, p, true, 100, r, nullptr, nullptr, err) &&
                  err.find("lost") != std::string::npos,
              "a dead connection aborts the scan with the error");
    }

    // ---- cancel + progress
    {
        FakeMem m;
        m.put32(kBase + 0x10, 7);
        m.put32(kBase + 0x5010, 7);
        std::atomic<bool> cancel{false};
        uint64_t lastDone = 0;
        ScanResults r;
        auto prog = [&](uint64_t d, uint64_t, size_t) { lastDone = d; if (d >= 0x1000) cancel = true; };
        CHECK(firstScan(m.io(), kBase, kBase + 0x6000, ValType::Int32, pat(ValType::Int32, "7"), true, 100, r, prog, &cancel, err) &&
                  r.cancelled && r.hits.size() == 1 && lastDone == 0x1000,
              "cancel stops the first scan and keeps partial results");
    }

    // ---- next scans
    {
        FakeMem m;
        const uint32_t a = kBase + 0x100, b = kBase + 0x104, c = kBase + 0x3000;
        m.put32(a, 50); m.put32(b, 50); m.put32(c, 50);
        ScanResults r;
        firstScan(m.io(), kBase, kBase + 0x6000, ValType::Int32, pat(ValType::Int32, "50"), true, 100, r, nullptr, nullptr, err);
        CHECK(r.hits.size() == 3, "setup: three 50s");

        m.put32(a, 60); m.put32(b, 40);  // c unchanged
        ScanResults inc = r, dec = r, chg = r, same = r, ref = r, ex = r;
        CHECK(nextScan(m.io(), inc, ScanMode::Increased, {}, nullptr, nullptr, err) && inc.hits.size() == 1 && inc.hits[0].addr == a, "Increased");
        CHECK(nextScan(m.io(), dec, ScanMode::Decreased, {}, nullptr, nullptr, err) && dec.hits.size() == 1 && dec.hits[0].addr == b, "Decreased");
        CHECK(nextScan(m.io(), chg, ScanMode::Changed, {}, nullptr, nullptr, err) && chg.hits.size() == 2, "Changed");
        CHECK(nextScan(m.io(), same, ScanMode::Unchanged, {}, nullptr, nullptr, err) && same.hits.size() == 1 && same.hits[0].addr == c, "Unchanged");
        CHECK(nextScan(m.io(), ref, ScanMode::Refresh, {}, nullptr, nullptr, err) && ref.hits.size() == 3 &&
                  ref.hits[0].last == pat(ValType::Int32, "60") && ref.hits[0].prev == pat(ValType::Int32, "50"),
              "Refresh keeps all hits, updates last and prev");
        CHECK(nextScan(m.io(), ex, ScanMode::Exact, pat(ValType::Int32, "40"), nullptr, nullptr, err) && ex.hits.size() == 1 && ex.hits[0].addr == b, "Exact");
        CHECK(!nextScan(m.io(), ex, ScanMode::Exact, pat(ValType::Int8, "1"), nullptr, nullptr, err), "Exact with the wrong width rejected");

        // chained narrowing: Changed then Exact on the survivors
        CHECK(nextScan(m.io(), chg, ScanMode::Exact, pat(ValType::Int32, "60"), nullptr, nullptr, err) && chg.hits.size() == 1, "narrowing can be chained");

        // coalesced reads: a, b are adjacent, c is far away -> 2 reads, not 3
        m.reads = 0;
        ScanResults two = r;
        nextScan(m.io(), two, ScanMode::Refresh, {}, nullptr, nullptr, err);
        CHECK(m.reads == 2, "hits within 1 KB share a single read");

        // cancel leaves results untouched
        std::atomic<bool> cancel{true};
        ScanResults cx = r;
        CHECK(nextScan(m.io(), cx, ScanMode::Changed, {}, nullptr, &cancel, err) && cx.cancelled && cx.hits.size() == 3,
              "cancelled next scan leaves hits unchanged");

        // unreadable hits are dropped and counted
        m.badLo = c; m.badHi = c + 4;
        ScanResults un = r;
        CHECK(nextScan(m.io(), un, ScanMode::Refresh, {}, nullptr, nullptr, err) && un.hits.size() == 2 && un.unreadableHits == 1,
              "hits that become unreadable are dropped and counted");
        m.dead = true;
        CHECK(!nextScan(m.io(), r, ScanMode::Refresh, {}, nullptr, nullptr, err), "dead connection fails the next scan");
    }

    // ---- float + text type rules
    {
        FakeMem m;
        m.putf(kBase + 0x20, 1.5f);
        ScanResults r;
        firstScan(m.io(), kBase, kBase + 0x1000, ValType::Float, pat(ValType::Float, "1.5"), true, 10, r, nullptr, nullptr, err);
        CHECK(r.hits.size() == 1, "float exact search");
        m.putf(kBase + 0x20, 2.5f);
        CHECK(nextScan(m.io(), r, ScanMode::Increased, {}, nullptr, nullptr, err) && r.hits.size() == 1, "float Increased");

        ScanResults t;
        t.type = ValType::Text;
        t.width = 3;
        CHECK(!nextScan(m.io(), t, ScanMode::Increased, {}, nullptr, nullptr, err), "Increased rejected for text");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
