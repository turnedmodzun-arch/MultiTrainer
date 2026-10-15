// Exercises XbdmClient + XrpcClient against an in-process mock console.
//
// IMPORTANT: this only proves the client agrees with the mock, which implements
// the protocol as *I* remember it. It cannot tell you whether a real console
// speaks it. Use a Wireshark capture / the app's raw-trace view for that.
//
// POSIX only (Linux/macOS); the test server uses BSD sockets directly.
#include "../src/mod.h"
#include "../src/xbdm.h"
#include "../src/xrpc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

static int g_failures = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL: %s\n", msg); g_failures++; }           \
        else         { std::printf("ok:   %s\n", msg); }                         \
    } while (0)

// ---------------------------------------------------------------- mock console
struct MockConsole
{
    std::map<uint32_t, uint8_t> mem;
    std::mutex memMu;
    std::vector<std::pair<int, std::string>> cbufCalls;  // (clientNum, text)
    std::atomic<bool> stop{false};
    int listenFd = -1;
    uint16_t port = 0;
    uint32_t bufAddr = 0x90000000;

    uint32_t rd32(uint32_t a)
    {
        std::lock_guard<std::mutex> lk(memMu);
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v = (v << 8) | (mem.count(a + i) ? mem[a + i] : 0);
        return v;
    }
    void wr32(uint32_t a, uint32_t v)
    {
        std::lock_guard<std::mutex> lk(memMu);
        for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i));
    }
    uint8_t rd8(uint32_t a)
    {
        std::lock_guard<std::mutex> lk(memMu);
        return mem.count(a) ? mem[a] : 0;
    }

    void start(bool installPlugin)
    {
        if (installPlugin) wr32(XrpcClient::kBufferPtrAddr, bufAddr);

        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(listenFd, (sockaddr *)&a, sizeof(a));
        listen(listenFd, 1);
        socklen_t l = sizeof(a);
        getsockname(listenFd, (sockaddr *)&a, &l);
        port = ntohs(a.sin_port);

        std::thread([this] { serve(); }).detach();
        if (installPlugin) std::thread([this] { plugin(); }).detach();
    }

    // Plays the role of the console-side XRPC plugin.
    void plugin()
    {
        while (!stop)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            uint32_t fn = rd32(bufAddr);
            if (!fn) continue;
            uint32_t argPtr = rd32(bufAddr + 8 + 8 + 4);  // arg[1] low word (big-endian 64-bit)
            int client = (int)rd32(bufAddr + 8 + 4);       // arg[0] low word
            std::string s;
            for (uint32_t p = argPtr; rd8(p) && s.size() < 512; p++) s += (char)rd8(p);
            cbufCalls.push_back({client, s});
            wr32(bufAddr + 4092, 0x1234);
            wr32(bufAddr, 0);
        }
    }

    static bool readLine(int fd, std::string &out)
    {
        out.clear();
        char c;
        while (recv(fd, &c, 1, 0) == 1)
        {
            if (c == '\n') { if (!out.empty() && out.back() == '\r') out.pop_back(); return true; }
            out += c;
        }
        return false;
    }
    static void sendStr(int fd, const std::string &s) { send(fd, s.data(), s.size(), 0); }

    void serve()
    {
        while (!stop)
        {
            int fd = accept(listenFd, nullptr, nullptr);
            if (fd < 0) return;
            sendStr(fd, "201- connected\r\n");
            std::string line;
            while (readLine(fd, line))
            {
                unsigned addr = 0, len = 0;
                char data[1024];
                if (std::sscanf(line.c_str(), "getmem addr=0x%x length=0x%x", &addr, &len) == 2)
                {
                    sendStr(fd, "202- multiline response follows\r\n");
                    static const char *d = "0123456789ABCDEF";
                    for (unsigned i = 0; i < len; i += 16)
                    {
                        std::string row;
                        for (unsigned j = i; j < i + 16 && j < len; j++)
                        {
                            uint8_t b = rd8(addr + j);
                            row += d[b >> 4];
                            row += d[b & 15];
                        }
                        sendStr(fd, row + "\r\n");
                    }
                    sendStr(fd, ".\r\n");
                }
                else if (std::sscanf(line.c_str(), "setmem addr=0x%x data=%1023s", &addr, data) == 2)
                {
                    std::string h = data;
                    {
                        std::lock_guard<std::mutex> lk(memMu);
                        for (size_t i = 0; i + 1 < h.size(); i += 2)
                            mem[addr + i / 2] = (uint8_t)std::stoi(h.substr(i, 2), nullptr, 16);
                    }
                    sendStr(fd, "200- OK\r\n");
                }
                else
                {
                    sendStr(fd, "400- unknown command\r\n");
                }
            }
            close(fd);
        }
    }
};

// ------------------------------------------------------------------------ tests
int main()
{
    // 1) Memory round trip, including chunking in both directions.
    {
        MockConsole &mc = *new MockConsole;  // intentionally leaked: detached server threads outlive this scope
        mc.start(true);
        XbdmClient c;
        std::string err;
        CHECK(c.connect("127.0.0.1", err, mc.port), "connect and read 201 banner");

        std::vector<uint8_t> big(3000);
        for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7 + 3);
        CHECK(c.setMemory(0x82000000, big.data(), big.size(), err), "setMemory 3000 bytes (chunked)");

        std::vector<uint8_t> back;
        CHECK(c.getMemory(0x82000000, (uint32_t)big.size(), back, err) && back == big,
              "getMemory 3000 bytes (chunked) matches");

        XbdmReply rep;
        CHECK(c.send("nonsense", rep, err) && rep.code == 400, "console-side error is reported, not thrown");
        mc.stop = true;
    }

    // 2) XRPC port: argument layout, string handling, result read.
    {
        MockConsole &mc = *new MockConsole;  // intentionally leaked: detached server threads outlive this scope
        mc.start(true);
        XbdmClient c;
        std::string err;
        c.connect("127.0.0.1", err, mc.port);
        XrpcClient x(c);

        uint32_t res = 0;
        bool ok = x.call(0x8226F590, {XrpcArg::i(0), XrpcArg::s("give all")}, res, err);
        CHECK(ok && res == 0x1234, "call returns the value at buffer+4092");
        ok = x.call(0x8226F590, {XrpcArg::i(0), XrpcArg::s("god")}, res, err);
        CHECK(ok, "second call succeeds");

        CHECK(mc.cbufCalls.size() == 2, "plugin saw two calls");
        if (mc.cbufCalls.size() == 2)
        {
            CHECK(mc.cbufCalls[0] == std::make_pair(0, std::string("give all")), "first call text/client intact");
            CHECK(mc.cbufCalls[1] == std::make_pair(0, std::string("god")),
                  "short string after long one has no leftover bytes");
        }
        mc.stop = true;
    }

    // 3) Plugin missing: pointer reads back as 0 and we say so.
    {
        MockConsole &mc = *new MockConsole;
        mc.start(false);
        XbdmClient c;
        std::string err;
        c.connect("127.0.0.1", err, mc.port);
        XrpcClient x(c);
        uint32_t res;
        CHECK(!x.call(0x8226F590, {XrpcArg::i(0), XrpcArg::s("god")}, res, err) &&
                  err.find("plugin") != std::string::npos,
              "missing plugin gives a clear error");
        mc.stop = true;
    }

    // 4) Mod files: parsing, validation, folder scan.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "xbox_toolkit_mod_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto writeFile = [&](const char *name, const std::string &text) {
        std::ofstream(dir / name) << text;
        return dir / name;
    };

    const fs::path goodPath = writeFile("b_game.json", R"({
        // comments are allowed
        "game": "B Game",
        "settings": { "cbuf_address": "0x8226F590", "client": 2, "append_newline": true },
        "controls": [
          { "type": "label", "text": "hi" },
          { "type": "separator", "label": "S" },
          { "type": "button", "label": "B", "cbuf": "god" },
          { "type": "checkbox", "label": "C", "default": true, "on_cbuf": "x 1", "off_cbuf": "x 0" },
          { "type": "textbox", "label": "T", "default": "250", "cbuf": "maxfps {value}" },
          { "type": "textbox", "label": "P32", "actions": [ { "poke32": { "addr": "0x82001000", "value": "{value}" } } ] },
          { "type": "textbox", "label": "PF",  "actions": [ { "pokef":  { "addr": "0x82001010", "value": "{value}" } } ] },
          { "type": "button", "label": "PB", "actions": [ { "poke": { "addr": "0x82001020", "bytes": "DE AD BE EF" } } ] },
          { "type": "button", "label": "CALL", "actions": [ { "call": { "addr": "0x82123456", "args": [7, "hi {value}"] } } ] },
          { "type": "slider", "label": "unknown type" },
          { "type": "button", "label": "bad action", "actions": [ { "nope": 1 } ] }
        ]
    })");
    writeFile("a_game.json", R"({ "game": "A Game", "controls": [ { "type": "label", "text": "x" } ] })");
    writeFile("broken.json", "{ this is not json");
    writeFile("notes.txt", "ignored: not a .json file");

    ModFile mod;
    CHECK(loadModFile(goodPath, mod), "valid mod file loads");
    CHECK(mod.game == "B Game" && mod.settings.cbufAddr == 0x8226F590 && mod.settings.client == 2 &&
              mod.settings.appendNewline,
          "settings parsed");
    CHECK(mod.controls.size() == 9, "9 usable controls parsed");
    CHECK(mod.warnings.size() == 2, "unknown control type and bad action are warnings, not fatal");
    if (mod.controls.size() == 9)
    {
        CHECK(mod.controls[3].type == ModControl::Checkbox && mod.controls[3].on, "checkbox default on");
        CHECK(std::string(mod.controls[4].text) == "250", "textbox default text");
    }

    ModFile bad;
    CHECK(!loadModFile(dir / "broken.json", bad) && !bad.error.empty() && bad.game == "broken",
          "invalid JSON is reported as an error");
    CHECK(expandValue("a {value} b {value}", "X") == "a X b X", "{value} expands everywhere");

    std::vector<ModFile> found = scanMods(dir);
    CHECK(found.size() == 3, "scan finds the three .json files and ignores notes.txt");
    if (found.size() == 3)
        CHECK(found[0].game == "A Game" && found[1].game == "B Game" && !found[2].ok(),
              "scan sorts by name with broken files last");

    // 5) Running actions against the mock console.
    if (mod.controls.size() == 9)
    {
        MockConsole &mc = *new MockConsole;
        mc.start(true);
        XbdmClient c;
        std::string err;
        c.connect("127.0.0.1", err, mc.port);
        XrpcClient x(c);
        std::vector<std::string> logs;
        auto log = [&](const std::string &s) { logs.push_back(s); };
        auto lastCbuf = [&] { return mc.cbufCalls.empty() ? std::make_pair(-1, std::string()) : mc.cbufCalls.back(); };

        runActions(c, x, mod.settings, mod.controls[2].actions, "", log);
        CHECK(lastCbuf() == std::make_pair(2, std::string("god\n")), "button: cbuf with client and appended newline");
        runActions(c, x, mod.settings, mod.controls[3].onActions, "", log);
        CHECK(lastCbuf() == std::make_pair(2, std::string("x 1\n")), "checkbox on action");
        runActions(c, x, mod.settings, mod.controls[3].offActions, "", log);
        CHECK(lastCbuf() == std::make_pair(2, std::string("x 0\n")), "checkbox off action");
        runActions(c, x, mod.settings, mod.controls[4].actions, "300", log);
        CHECK(lastCbuf() == std::make_pair(2, std::string("maxfps 300\n")), "textbox value substituted into cbuf");

        runActions(c, x, mod.settings, mod.controls[5].actions, "100", log);
        CHECK(mc.rd32(0x82001000) == 100, "poke32 writes a big-endian int");
        runActions(c, x, mod.settings, mod.controls[5].actions, "0x10", log);
        CHECK(mc.rd32(0x82001000) == 16, "poke32 accepts hex values");
        logs.clear();
        runActions(c, x, mod.settings, mod.controls[5].actions, "abc", log);
        CHECK(mc.rd32(0x82001000) == 16 && !logs.empty() && logs[0].find("not an integer") != std::string::npos,
              "poke32 rejects a non-number without writing");
        runActions(c, x, mod.settings, mod.controls[6].actions, "1.5", log);
        CHECK(mc.rd32(0x82001010) == 0x3FC00000, "pokef writes a big-endian float");
        runActions(c, x, mod.settings, mod.controls[7].actions, "", log);
        CHECK(mc.rd32(0x82001020) == 0xDEADBEEF, "poke writes raw bytes");
        runActions(c, x, mod.settings, mod.controls[8].actions, "X", log);
        CHECK(lastCbuf() == std::make_pair(7, std::string("hi X")), "call passes integer and string args");

        logs.clear();
        ModSettings none;
        runActions(c, x, none, mod.controls[2].actions, "", log);
        CHECK(!logs.empty() && logs[0].find("cbuf_address") != std::string::npos,
              "cbuf without cbuf_address gives a clear message");
        mc.stop = true;
    }
    // 6) Saving: serialise -> load -> compare, and idempotence of the writer.
    {
        const std::string json1 = modToJson(mod);
        const fs::path out = dir / "roundtrip.json";
        std::string err;
        CHECK(saveModFile(mod, out, err), "saveModFile writes the file");
        CHECK(!fs::exists(out.string() + ".tmp"), "no temp file left behind");

        ModFile re;
        CHECK(loadModFile(out, re) && re.warnings.empty(), "saved file loads back with no warnings");
        CHECK(re.game == mod.game && re.settings.cbufAddr == mod.settings.cbufAddr &&
                  re.settings.client == mod.settings.client && re.settings.appendNewline == mod.settings.appendNewline,
              "settings survive the round trip");
        CHECK(re.controls.size() == mod.controls.size(), "control count survives the round trip");
        CHECK(modToJson(re) == json1, "writer is idempotent (save, load, save gives identical text)");
        if (re.controls.size() == 9)
        {
            CHECK(re.controls[3].defaultOn && re.controls[3].onActions.size() == 1 && re.controls[3].offActions.size() == 1,
                  "checkbox default and on/off lists survive");
            CHECK(re.controls[7].actions[0].bytes == std::vector<uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}) &&
                      re.controls[7].actions[0].bytesHex == "DE AD BE EF",
                  "poke bytes survive");
            CHECK(re.controls[8].actions[0].args.size() == 2 && re.controls[8].actions[0].args[0].i == 7 &&
                      re.controls[8].actions[0].args[1].s == "hi {value}",
                  "call args survive");
        }

        CHECK(!saveModFile(mod, dir / "no_such_folder" / "x.json", err) && !err.empty(),
              "saving into a missing folder reports an error");
    }

    // 7) Validation and hex helpers (used by the builder).
    {
        ModFile bad;
        bad.settings.cbufAddr = 0;  // cbuf actions below need this
        ModControl b;
        b.type = ModControl::Button;
        ModAction a;
        a.kind = ModAction::Cbuf;
        a.text = "god";
        b.actions.push_back(a);
        ModAction p;
        p.kind = ModAction::Poke;  // address 0 and no bytes
        b.actions.push_back(p);
        bad.controls.push_back(b);

        std::vector<std::string> problems = validateMod(bad);
        auto has = [&](const char *needle) {
            for (const std::string &s : problems) if (s.find(needle) != std::string::npos) return true;
            return false;
        };
        CHECK(has("Game name is empty"), "validate: empty game name");
        CHECK(has("button has no label"), "validate: unlabeled button");
        CHECK(has("Cbuf_AddText address"), "validate: cbuf action without an address");
        CHECK(has("address is 0") && has("no valid bytes"), "validate: poke with no address or bytes");

        bad.game = "G";
        bad.settings.cbufAddr = 0x8226F590;
        bad.controls[0].label = "L";
        bad.controls[0].actions[1].addr = 0x82001000;
        bad.controls[0].actions[1].bytes = {1, 2};
        problems = validateMod(bad);
        CHECK(problems.empty(), "validate: a fixed mod has no problems");

        std::vector<uint8_t> v;
        std::string herr;
        CHECK(parseHexString("de AD 01", v, herr) && v == std::vector<uint8_t>({0xDE, 0xAD, 0x01}), "parseHexString accepts mixed case with spaces");
        CHECK(!parseHexString("DE A", v, herr) && !parseHexString("zz", v, herr) && !parseHexString("", v, herr),
              "parseHexString rejects odd length, junk and empty input");
        CHECK(formatHexBytes({0x0A, 0xFF}) == "0A FF", "formatHexBytes");
    }
    fs::remove_all(dir);

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
