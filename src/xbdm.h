// Minimal client for the Xbox 360 debug service (XBDM, TCP port 730).
// Replaces XDevkit.dll's COM wrapper with a plain socket so it runs anywhere.
//
// PROTOCOL NOTE: the framing here (text lines ending in CRLF, "201-" banner,
// "202-" multiline replies terminated by a lone ".", getmem/setmem command
// syntax) is written from recollection of how XBDM behaves. Verify it against
// a Wireshark capture of your existing tool (filter: tcp.port == 730) before
// trusting it. Turn on "raw protocol trace" in the app to see exactly what
// goes over the wire.
#pragma once
#include "net.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct XbdmReply
{
    int code = 0;                    // numeric status, e.g. 200, 202, 4xx
    std::string status;              // the first reply line, verbatim
    std::vector<std::string> lines;  // body lines of a multiline (202) reply
    bool ok() const { return code >= 200 && code < 300; }
};

class XbdmClient
{
public:
    ~XbdmClient();

    bool connect(const std::string &host, std::string &err, uint16_t port = 730);
    void disconnect();
    bool isConnected() const { return sock_ != SOCK_INVALID; }

    // Sends one command line. Returns false only on transport errors; check
    // rep.ok() for console-side errors.
    bool send(const std::string &cmd, XbdmReply &rep, std::string &err);

    bool getMemory(uint32_t addr, uint32_t len, std::vector<uint8_t> &out, std::string &err);
    bool setMemory(uint32_t addr, const uint8_t *data, size_t len, std::string &err);

    // Optional hook that receives every line sent (">> ") and received ("<< ").
    std::function<void(const std::string &)> onTrace;
    int timeoutMs = 5000;

private:
    static constexpr size_t kMaxCommand = 512;  // XBDM rejects longer command lines
    static constexpr uint32_t kReadChunk = 0x400;
    static constexpr size_t kWriteChunk = 128;  // 256 hex chars + header stays under 512

    bool sendLocked(const std::string &cmd, XbdmReply &rep, std::string &err);
    bool readLine(std::string &line, std::string &err);
    void disconnectLocked();
    void trace(const std::string &s) { if (onTrace) onTrace(s); }

    sock_t sock_ = SOCK_INVALID;
    std::string buf_;
    std::mutex mu_;
};
