#include "xrpc.h"

#include <chrono>
#include <cstring>
#include <thread>

static void putBE32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void putBE64(uint8_t *p, uint64_t v)
{
    putBE32(p, (uint32_t)(v >> 32));
    putBE32(p + 4, (uint32_t)v);
}

bool XrpcClient::writeU32(uint32_t addr, uint32_t v, std::string &err)
{
    uint8_t b[4];
    putBE32(b, v);
    return c_.setMemory(addr, b, 4, err);
}

bool XrpcClient::readU32(uint32_t addr, uint32_t &v, std::string &err)
{
    std::vector<uint8_t> b;
    if (!c_.getMemory(addr, 4, b, err)) return false;
    v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
}

bool XrpcClient::ensureBuffer(std::string &err)
{
    if (buf_ != 0) return true;

    uint32_t p = 0;
    if (!readU32(kBufferPtrAddr, p, err)) return false;
    if (p == 0)
    {
        err = "XRPC buffer pointer is 0 - is the XRPC plugin loaded on the console?";
        return false;
    }
    buf_ = p;

    // XRPC.cs clears the head of the buffer and the string area on first use.
    const uint8_t zeros[100] = {0};
    if (!c_.setMemory(buf_, zeros, sizeof(zeros), err)) { buf_ = 0; return false; }
    if (!c_.setMemory(buf_ + 1500, zeros, sizeof(zeros), err)) { buf_ = 0; return false; }
    return true;
}

bool XrpcClient::call(uint32_t funcAddr, const std::vector<XrpcArg> &args, uint32_t &result, std::string &err)
{
    result = 0;
    if (!c_.isConnected())
    {
        err = "not connected";
        return false;
    }
    if (args.size() > 9)
    {
        err = "XRPC supports at most 9 arguments";
        return false;
    }
    if (!ensureBuffer(err)) return false;

    uint32_t strPtr = buf_ + 1500;
    const uint32_t strLimit = buf_ + 2700;  // where XRPC.cs places its float area
    uint64_t values[9] = {0};

    for (size_t i = 0; i < args.size(); i++)
    {
        const XrpcArg &a = args[i];
        if (!a.isString)
        {
            values[i] = a.value;
            continue;
        }

        const size_t need = a.str.size() + 1;
        if (strPtr + need > strLimit)
        {
            err = "string arguments too long for the XRPC string area";
            return false;
        }
        // Unlike XRPC.cs, write the terminating NUL too, so a short string can't
        // inherit leftover bytes from a longer one used in an earlier call.
        std::vector<uint8_t> bytes(a.str.begin(), a.str.end());
        bytes.push_back(0);
        if (!c_.setMemory(strPtr, bytes.data(), bytes.size(), err)) return false;
        values[i] = strPtr;
        strPtr += (uint32_t)need;
    }

    uint8_t block[72];
    for (int i = 0; i < 9; i++) putBE64(block + i * 8, values[i]);
    if (!c_.setMemory(buf_ + 8, block, sizeof(block), err)) return false;
    if (!writeU32(buf_ + 4, (uint32_t)args.size(), err)) return false;
    if (!writeU32(buf_, funcAddr, err)) return false;  // triggers the call

    std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
    return readU32(buf_ + 4092, result, err);
}
