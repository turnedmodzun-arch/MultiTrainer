// Scanner tab: memory search (first scan + narrowing next scans), and a watch list for typed
// peek (read) and poke (write). Depends on Dear ImGui and scanner.h, not on the console client:
// the host supplies read/write/submit callbacks, so it can be driven by fake memory in tests.
#pragma once
#include "scanner.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct MemHost
{
    bool connected = false;
    ScanIO io;  // reads (and the "alive" check)
    std::function<bool(uint32_t addr, const std::vector<uint8_t> &data, std::string &err)> write;
    std::function<void(std::function<void()>)> submit;  // runs a job on the worker thread
    std::function<void(const std::string &)> log;
};

struct WatchEntry
{
    int id = 0;
    uint32_t addr = 0;
    ValType type = ValType::Int32;
    size_t width = 4;            // bytes read; for Bytes/Text this is the chosen length
    std::vector<uint8_t> value;  // last peeked bytes (valid only if `valid`)
    bool valid = false;
    std::string error;           // why the last peek failed
    std::string pokeText;        // UI: what the Poke button will write
};

class MemTools
{
public:
    // ---- UI inputs (UI thread only)
    std::string startHex = "82000000";
    std::string endHex = "83000000";  // exclusive; 16 MB by default because reads go over XBDM
    std::string valueText;
    int type = (int)ValType::Int32;
    int mode = 1;  // index into ScanMode: 0 Exact, 1 Changed, 2 Unchanged, 3 Increased, 4 Decreased
    bool aligned = true;
    int maxHits = 100000;
    std::string addrHex = "82000000";  // manual peek/poke address
    int manualType = (int)ValType::Int32;
    int manualLen = 4;                 // for Bytes/Text
    bool autoRefresh = false;
    double lastRefresh = 0;

    // ---- shared with the worker (guard with mu)
    std::mutex mu;
    ScanResults results;
    bool haveResults = false;
    std::string message;
    std::vector<WatchEntry> watch;
    int nextId = 1;

    // ---- progress / state flags (lock-free)
    std::atomic<bool> busy{false};       // a scan is running
    std::atomic<bool> cancel{false};
    std::atomic<bool> watchBusy{false};  // a peek/poke job is queued or running
    std::atomic<uint64_t> progDone{0};
    std::atomic<uint64_t> progTotal{0};
    std::atomic<uint64_t> progHits{0};

    // Operations. Each validates its input on the calling thread, then queues the console work.
    void firstScan(const MemHost &host);
    void nextScan(const MemHost &host, ScanMode mode);
    void resetScan();
    void addWatch(uint32_t addr, ValType type, size_t width, const std::vector<uint8_t> &initial);
    bool addManualWatch(const MemHost &host);  // from addrHex/manualType/manualLen; also peeks it
    void peekAll(const MemHost &host);
    void poke(const MemHost &host, int id);

    void setMessage(const std::string &m)
    {
        std::lock_guard<std::mutex> lk(mu);
        message = m;
    }
};

bool parseAddress(const std::string &text, uint32_t &out);

void drawMemTools(MemTools &t, const MemHost &host);
