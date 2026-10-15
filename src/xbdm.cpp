#include "xbdm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

XbdmClient::~XbdmClient() { disconnect(); }

void XbdmClient::disconnectLocked()
{
    if (sock_ != SOCK_INVALID)
    {
        net::closeSock(sock_);
        sock_ = SOCK_INVALID;
    }
    buf_.clear();
}

void XbdmClient::disconnect()
{
    std::lock_guard<std::mutex> lk(mu_);
    disconnectLocked();
}

static int parseCode(const std::string &s)
{
    if (s.size() < 3 || !std::isdigit((unsigned char)s[0]) || !std::isdigit((unsigned char)s[1]) ||
        !std::isdigit((unsigned char)s[2]))
        return 0;
    return std::atoi(s.substr(0, 3).c_str());
}

bool XbdmClient::connect(const std::string &host, std::string &err, uint16_t port)
{
    std::lock_guard<std::mutex> lk(mu_);
    disconnectLocked();

    if (!net::init())
    {
        err = "network initialisation failed";
        return false;
    }
    sock_ = net::connectTcp(host, port, timeoutMs, err);
    if (sock_ == SOCK_INVALID) return false;

    std::string banner;
    if (!readLine(banner, err)) return false;
    trace("<< " + banner);
    if (parseCode(banner) != 201)
    {
        err = "unexpected banner from console: " + banner;
        disconnectLocked();
        return false;
    }
    return true;
}

bool XbdmClient::readLine(std::string &line, std::string &err)
{
    for (;;)
    {
        size_t p = buf_.find("\r\n");
        if (p != std::string::npos)
        {
            line = buf_.substr(0, p);
            buf_.erase(0, p + 2);
            return true;
        }
        char tmp[2048];
        int n = net::recvSome(sock_, tmp, sizeof(tmp));
        if (n <= 0)
        {
            err = (n == 0) ? "console closed the connection" : "receive failed or timed out";
            disconnectLocked();
            return false;
        }
        buf_.append(tmp, (size_t)n);
    }
}

bool XbdmClient::send(const std::string &cmd, XbdmReply &rep, std::string &err)
{
    std::lock_guard<std::mutex> lk(mu_);
    return sendLocked(cmd, rep, err);
}

bool XbdmClient::sendLocked(const std::string &cmd, XbdmReply &rep, std::string &err)
{
    rep = XbdmReply();
    if (sock_ == SOCK_INVALID)
    {
        err = "not connected";
        return false;
    }
    if (cmd.size() > kMaxCommand)
    {
        err = "command longer than 512 bytes";
        return false;
    }

    trace(">> " + cmd);
    const std::string out = cmd + "\r\n";
    if (!net::sendAll(sock_, out.data(), out.size()))
    {
        err = "send failed";
        disconnectLocked();
        return false;
    }

    std::string first;
    if (!readLine(first, err)) return false;
    trace("<< " + first);
    rep.status = first;
    rep.code = parseCode(first);

    if (rep.code == 202)  // multiline: body lines until a lone "."
    {
        for (;;)
        {
            std::string l;
            if (!readLine(l, err)) return false;
            if (l == ".") break;
            rep.lines.push_back(std::move(l));
        }
        trace("<< (" + std::to_string(rep.lines.size()) + " body lines)");
    }
    return true;
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool XbdmClient::getMemory(uint32_t addr, uint32_t len, std::vector<uint8_t> &out, std::string &err)
{
    out.clear();
    out.reserve(len);
    std::lock_guard<std::mutex> lk(mu_);

    uint32_t done = 0;
    while (done < len)
    {
        const uint32_t n = std::min<uint32_t>(kReadChunk, len - done);
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd), "getmem addr=0x%08X length=0x%X", addr + done, n);

        XbdmReply rep;
        if (!sendLocked(cmd, rep, err)) return false;
        if (rep.code == 203)
        {
            err = "console answered getmem with a binary (203) reply, which this client doesn't "
                  "parse yet. Capture one with Wireshark and extend XbdmClient::getMemory.";
            return false;
        }
        if (rep.code != 202)
        {
            err = "getmem failed: " + rep.status;
            return false;
        }

        const size_t before = out.size();
        int hi = -1;
        for (const std::string &l : rep.lines)
        {
            for (size_t i = 0; i < l.size(); i++)
            {
                const char c = l[i];
                if (std::isspace((unsigned char)c)) continue;
                if (c == '?')
                {
                    err = "unreadable memory near 0x" + std::to_string(addr + done);
                    return false;
                }
                const int v = hexVal(c);
                if (v < 0) continue;
                if (hi < 0) hi = v;
                else
                {
                    out.push_back((uint8_t)((hi << 4) | v));
                    hi = -1;
                }
            }
        }
        if (out.size() - before != n)
        {
            err = "getmem returned " + std::to_string(out.size() - before) + " bytes, expected " +
                  std::to_string(n);
            return false;
        }
        done += n;
    }
    return true;
}

bool XbdmClient::setMemory(uint32_t addr, const uint8_t *data, size_t len, std::string &err)
{
    std::lock_guard<std::mutex> lk(mu_);
    static const char *digits = "0123456789ABCDEF";

    size_t done = 0;
    while (done < len)
    {
        const size_t n = std::min<size_t>(kWriteChunk, len - done);
        std::string hex;
        hex.reserve(n * 2);
        for (size_t i = 0; i < n; i++)
        {
            hex.push_back(digits[data[done + i] >> 4]);
            hex.push_back(digits[data[done + i] & 0xF]);
        }
        char head[64];
        std::snprintf(head, sizeof(head), "setmem addr=0x%08X data=", (uint32_t)(addr + done));

        XbdmReply rep;
        if (!sendLocked(std::string(head) + hex, rep, err)) return false;
        if (!rep.ok())
        {
            err = "setmem failed: " + rep.status;
            return false;
        }
        done += n;
    }
    return true;
}
