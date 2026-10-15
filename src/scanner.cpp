#include "scanner.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr uint32_t kBlock = 0x1000;  // first-scan read size
static constexpr uint32_t kSpan = 0x400;    // next-scan: hits closer than this share one read
static constexpr size_t kMaxPattern = 1024;

const char *valTypeName(ValType t)
{
    switch (t)
    {
    case ValType::Int8: return "Int8";
    case ValType::Int16: return "Int16";
    case ValType::Int32: return "Int32";
    case ValType::Float: return "Float";
    case ValType::Bytes: return "Bytes (hex)";
    case ValType::Text: return "Text (ASCII)";
    }
    return "?";
}

size_t valTypeWidth(ValType t)
{
    switch (t)
    {
    case ValType::Int8: return 1;
    case ValType::Int16: return 2;
    case ValType::Int32:
    case ValType::Float: return 4;
    default: return 0;
    }
}

static std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string hexBytes(const std::vector<uint8_t> &v)
{
    static const char *d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < v.size(); i++)
    {
        if (i) s += ' ';
        s += d[v[i] >> 4];
        s += d[v[i] & 15];
    }
    return s;
}

static bool parseHexBytes(const std::string &s, std::vector<uint8_t> &out, std::string &err)
{
    int hi = -1;
    for (char c : s)
    {
        if (std::isspace((unsigned char)c)) continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else { err = "bytes must be hex digits like DE AD BE EF"; return false; }
        if (hi < 0) hi = v;
        else { out.push_back((uint8_t)((hi << 4) | v)); hi = -1; }
    }
    if (hi >= 0) { err = "hex bytes need an even number of digits"; return false; }
    if (out.empty()) { err = "enter at least one byte"; return false; }
    return true;
}

bool parseValue(ValType t, const std::string &text, std::vector<uint8_t> &out, std::string &err)
{
    out.clear();
    const std::string s = trim(text);
    switch (t)
    {
    case ValType::Int8:
    case ValType::Int16:
    case ValType::Int32:
    {
        const size_t w = valTypeWidth(t);
        if (s.empty()) { err = "enter a number"; return false; }
        char *end = nullptr;
        errno = 0;
        const long long v = std::strtoll(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != 0) { err = "'" + s + "' is not a whole number"; return false; }
        const long long lo = -(1LL << (8 * w - 1)), hi = (1LL << (8 * w)) - 1;
        if (errno == ERANGE || v < lo || v > hi)
        {
            err = std::string("out of range for ") + valTypeName(t) + " (" + std::to_string(lo) + " to " +
                  std::to_string(hi) + ")";
            return false;
        }
        for (size_t i = 0; i < w; i++) out.push_back((uint8_t)((uint64_t)v >> (8 * (w - 1 - i))));
        return true;
    }
    case ValType::Float:
    {
        if (s.empty()) { err = "enter a number"; return false; }
        char *end = nullptr;
        const float f = std::strtof(s.c_str(), &end);
        if (end == s.c_str() || *end != 0 || !std::isfinite(f)) { err = "'" + s + "' is not a finite number"; return false; }
        uint32_t u;
        std::memcpy(&u, &f, 4);
        for (int i = 3; i >= 0; i--) out.push_back((uint8_t)(u >> (8 * i)));
        return true;
    }
    case ValType::Bytes:
        if (!parseHexBytes(text, out, err)) { out.clear(); return false; }
        break;
    case ValType::Text:
        if (text.empty()) { err = "enter some text"; return false; }
        out.assign(text.begin(), text.end());
        break;
    }
    if (out.size() > kMaxPattern) { out.clear(); err = "value longer than 1024 bytes"; return false; }
    return true;
}

bool decodeNumber(ValType t, const std::vector<uint8_t> &b, double &out)
{
    const size_t w = valTypeWidth(t);
    if (w == 0 || b.size() != w) return false;
    uint32_t u = 0;
    for (uint8_t x : b) u = (u << 8) | x;
    switch (t)
    {
    case ValType::Int8: out = (int8_t)u; return true;
    case ValType::Int16: out = (int16_t)u; return true;
    case ValType::Int32: out = (int32_t)u; return true;
    case ValType::Float:
    {
        float f;
        std::memcpy(&f, &u, 4);
        out = f;
        return true;
    }
    default: return false;
    }
}

std::string formatValue(ValType t, const std::vector<uint8_t> &b)
{
    const size_t w = valTypeWidth(t);
    char buf[96];
    if (t == ValType::Text)
    {
        std::string s;
        for (uint8_t c : b) s += (c >= 32 && c < 127) ? (char)c : '.';
        return s;
    }
    if (t == ValType::Bytes || b.size() != w) return hexBytes(b);
    if (t == ValType::Float)
    {
        double d = 0;
        decodeNumber(t, b, d);
        std::snprintf(buf, sizeof(buf), "%.9g", d);
        return buf;
    }
    uint32_t u = 0;
    for (uint8_t x : b) u = (u << 8) | x;
    double sd = 0;
    decodeNumber(t, b, sd);
    const long long sv = (long long)sd;
    const int digits = (int)w * 2;
    if (sv < 0) std::snprintf(buf, sizeof(buf), "%u (signed %lld, 0x%0*X)", u, sv, digits, u);
    else std::snprintf(buf, sizeof(buf), "%u (0x%0*X)", u, digits, u);
    return buf;
}

// ------------------------------------------------------------------ scanning
static bool fatal(const ScanIO &io) { return io.alive && !io.alive(); }

bool firstScan(const ScanIO &io, uint32_t start, uint32_t end, ValType type, const std::vector<uint8_t> &pattern,
               bool aligned, size_t maxHits, ScanResults &out, const ScanProgressFn &progress,
               const std::atomic<bool> *cancel, std::string &err)
{
    out = ScanResults();
    if (!io.read) { err = "no memory reader"; return false; }
    if (pattern.empty() || pattern.size() > kMaxPattern) { err = "search value must be 1 to 1024 bytes"; return false; }
    if (end <= start) { err = "end address must be above the start address"; return false; }

    const size_t w = pattern.size();
    const size_t natural = valTypeWidth(type);
    const uint64_t align = (aligned && natural > 1) ? natural : 1;
    out.type = type;
    out.width = w;

    const uint64_t s0 = start, e0 = end;
    bool full = false;
    for (uint64_t pos = s0; pos < e0 && !full; pos += kBlock)
    {
        if (cancel && cancel->load()) { out.cancelled = true; break; }

        const uint64_t blockLen = std::min<uint64_t>(kBlock, e0 - pos);
        const uint64_t readLen = std::min<uint64_t>(blockLen + w - 1, e0 - pos);  // overlap catches straddling matches
        std::vector<uint8_t> buf;

        if (readLen >= w)
        {
            std::string e;
            bool ok = io.read((uint32_t)pos, (uint32_t)readLen, buf, e);
            if (!ok && readLen > blockLen) ok = io.read((uint32_t)pos, (uint32_t)blockLen, buf, e);  // overlap may cross into unmapped memory
            if (!ok)
            {
                if (fatal(io)) { err = e; return false; }
                out.skippedBlocks++;
                buf.clear();
            }
        }

        if (buf.size() >= w)
        {
            const uint64_t limit = std::min<uint64_t>(blockLen, buf.size() - w + 1);
            for (uint64_t i = (align - pos % align) % align; i < limit; i += align)
            {
                if (buf[i] != pattern[0] || std::memcmp(&buf[i], pattern.data(), w) != 0) continue;
                ScanHit h;
                h.addr = (uint32_t)(pos + i);
                h.last = pattern;
                h.prev = pattern;
                out.hits.push_back(std::move(h));
                if (out.hits.size() >= maxHits) { out.truncated = true; full = true; break; }
            }
        }
        if (progress) progress(pos + blockLen - s0, e0 - s0, out.hits.size());
    }
    return true;
}

bool nextScan(const ScanIO &io, ScanResults &r, ScanMode mode, const std::vector<uint8_t> &pattern,
              const ScanProgressFn &progress, const std::atomic<bool> *cancel, std::string &err)
{
    r.cancelled = false;
    if (!io.read) { err = "no memory reader"; return false; }
    const size_t w = r.width;
    if ((mode == ScanMode::Increased || mode == ScanMode::Decreased) && valTypeWidth(r.type) == 0)
    {
        err = "Increased/Decreased only work with Int8, Int16, Int32 and Float";
        return false;
    }
    if (mode == ScanMode::Exact && pattern.size() != w)
    {
        err = "the new value must be " + std::to_string(w) + " byte(s) wide, like the earlier results";
        return false;
    }

    std::vector<ScanHit> kept;
    kept.reserve(r.hits.size());
    uint32_t unreadable = 0;
    const std::vector<ScanHit> &hits = r.hits;

    size_t i = 0;
    while (i < hits.size())
    {
        if (cancel && cancel->load()) { r.cancelled = true; return true; }  // r left unchanged

        const uint32_t base = hits[i].addr;
        size_t j = i;
        while (j + 1 < hits.size() && (uint64_t)hits[j + 1].addr + w - base <= kSpan) j++;
        const uint32_t span = hits[j].addr + (uint32_t)w - base;

        std::vector<uint8_t> buf;
        std::string e;
        if (!io.read(base, span, buf, e) || buf.size() < span)
        {
            if (fatal(io)) { err = e.empty() ? "read failed" : e; return false; }
            unreadable += (uint32_t)(j - i + 1);
            i = j + 1;
            continue;
        }

        for (size_t k = i; k <= j; k++)
        {
            const size_t off = hits[k].addr - base;
            std::vector<uint8_t> cur(buf.begin() + (long)off, buf.begin() + (long)(off + w));
            const std::vector<uint8_t> &old = hits[k].last;
            bool keep = true;
            switch (mode)
            {
            case ScanMode::Exact: keep = (cur == pattern); break;
            case ScanMode::Changed: keep = (cur != old); break;
            case ScanMode::Unchanged: keep = (cur == old); break;
            case ScanMode::Increased:
            case ScanMode::Decreased:
            {
                double a = 0, b = 0;
                keep = decodeNumber(r.type, cur, a) && decodeNumber(r.type, old, b) &&
                       (mode == ScanMode::Increased ? a > b : a < b);
                break;
            }
            case ScanMode::Refresh: break;
            }
            if (!keep) continue;
            ScanHit h;
            h.addr = hits[k].addr;
            h.prev = old;
            h.last = std::move(cur);
            kept.push_back(std::move(h));
        }
        i = j + 1;
        if (progress) progress(i, hits.size(), kept.size());
    }

    r.hits = std::move(kept);
    r.unreadableHits = unreadable;
    return true;
}
