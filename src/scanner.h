// Memory searching: typed values, first scan, and "narrowing" next scans (Cheat Engine style).
// Independent of the console client: it reads memory through ScanIO, so it can be tested
// against fake memory. Values are big-endian, as on the Xbox 360.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class ValType { Int8, Int16, Int32, Float, Bytes, Text };
constexpr int kValTypeCount = 6;

const char *valTypeName(ValType t);
size_t valTypeWidth(ValType t);  // 1, 2, 4, 4; 0 for the variable-length Bytes/Text

// Text -> big-endian bytes. Ints accept decimal or 0x hex (negative allowed, range-checked per width),
// Float a finite number, Bytes hex like "DE AD BE EF", Text any non-empty string (no NUL added).
bool parseValue(ValType t, const std::string &text, std::vector<uint8_t> &out, std::string &err);

// Bytes -> display text, e.g. "100 (0x00000064)", "1.5", "DE AD", or printable ASCII.
std::string formatValue(ValType t, const std::vector<uint8_t> &bytes);

// Numeric interpretation used for Increased/Decreased: ints are signed, floats as double.
bool decodeNumber(ValType t, const std::vector<uint8_t> &bytes, double &out);

struct ScanIO
{
    // Reads len bytes at addr. Returns false on failure (with err set).
    std::function<bool(uint32_t addr, uint32_t len, std::vector<uint8_t> &out, std::string &err)> read;
    // Optional. If a read fails and this returns false, the scan aborts instead of skipping the block.
    std::function<bool()> alive;
};

struct ScanHit
{
    uint32_t addr = 0;
    std::vector<uint8_t> last;  // value at the most recent scan
    std::vector<uint8_t> prev;  // value at the scan before that
};

struct ScanResults
{
    ValType type = ValType::Int32;
    size_t width = 4;
    std::vector<ScanHit> hits;  // ascending by address
    bool truncated = false;     // first scan stopped at maxHits
    bool cancelled = false;     // first scan: partial results kept. next scan: hits left unchanged.
    uint32_t skippedBlocks = 0; // 4 KB blocks the console refused to read (first scan)
    uint32_t unreadableHits = 0;// hits dropped by a next scan because they could no longer be read
};

enum class ScanMode { Exact, Changed, Unchanged, Increased, Decreased, Refresh };

// done/total are bytes (first scan) or hits (next scan); hits is the match count so far.
using ScanProgressFn = std::function<void(uint64_t done, uint64_t total, size_t hits)>;

// Searches [start, end) for pattern. aligned = only addresses that are a multiple of the value width
// (ints/floats). Unreadable 4 KB blocks are skipped and counted. Returns false only on a fatal error.
bool firstScan(const ScanIO &io, uint32_t start, uint32_t end, ValType type, const std::vector<uint8_t> &pattern,
               bool aligned, size_t maxHits, ScanResults &out, const ScanProgressFn &progress,
               const std::atomic<bool> *cancel, std::string &err);

// Re-reads every hit and keeps those matching mode. Exact compares with pattern (same width as the hits);
// Increased/Decreased need a numeric type; Refresh keeps everything and just updates the values.
bool nextScan(const ScanIO &io, ScanResults &r, ScanMode mode, const std::vector<uint8_t> &pattern,
              const ScanProgressFn &progress, const std::atomic<bool> *cancel, std::string &err);
