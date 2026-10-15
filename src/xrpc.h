// Port of XRPC.Call from your C# XRPCLib, running over XbdmClient get/setmem
// instead of XDevkit.dll. The mechanism is unchanged: a plugin on the console
// owns a shared buffer; we write the arguments and then the function address,
// the plugin runs the call, and the result appears at buffer + 4092.
//
// Buffer layout (all values big-endian, taken from your XRPC.cs):
//   +0     function address (written last; this is what triggers the call)
//   +4     argument count (int32)
//   +8     up to 9 arguments, 8 bytes each
//   +1500  area for string arguments
//   +4092  result (uint32)
#pragma once
#include "xbdm.h"

#include <cstdint>
#include <string>
#include <vector>

struct XrpcArg
{
    uint64_t value = 0;
    std::string str;
    bool isString = false;

    static XrpcArg i(int32_t v) { XrpcArg a; a.value = (uint64_t)(int64_t)v; return a; }  // sign-extended like Convert.ToInt64
    static XrpcArg u(uint32_t v) { XrpcArg a; a.value = v; return a; }
    static XrpcArg s(std::string v) { XrpcArg a; a.str = std::move(v); a.isString = true; return a; }
};

class XrpcClient
{
public:
    // The unaligned constant taken verbatim from XRPC.cs (2445314222 == 0x91C088AE).
    static constexpr uint32_t kBufferPtrAddr = 2445314222u;

    explicit XrpcClient(XbdmClient &c) : c_(c) {}

    // Forget the cached buffer address (call after reconnecting).
    void reset() { buf_ = 0; }

    bool call(uint32_t funcAddr, const std::vector<XrpcArg> &args, uint32_t &result, std::string &err);

    // How long to wait for the console-side plugin to run the call. XRPC.cs uses 50 ms.
    int settleMs = 50;

private:
    bool ensureBuffer(std::string &err);
    bool writeU32(uint32_t addr, uint32_t v, std::string &err);
    bool readU32(uint32_t addr, uint32_t &v, std::string &err);

    XbdmClient &c_;
    uint32_t buf_ = 0;
};
