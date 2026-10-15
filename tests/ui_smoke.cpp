// Headless smoke test for the builder UI: runs real ImGui frames with no window or GPU.
//
// Dear ImGui asserts on Begin/End mismatches, unbalanced ID stacks and similar mistakes, so
// rendering every screen with asserts enabled catches that whole class of bug. It cannot
// check layout or that a click does the right thing; look at the real app for that.
#include "../src/builder.h"
#include "../src/memtools.h"
#include "imgui.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond, msg)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) { std::printf("FAIL: %s\n", msg); g_failures++; } \
        else         { std::printf("ok:   %s\n", msg); }              \
    } while (0)

// Renders `frames` frames; `body` draws inside a full-size window.
template <class F> static void frames(int n, F body)
{
    for (int i = 0; i < n; i++)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }
}

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1100, 750);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);  // builds the font atlas (normally done by a backend)

    const fs::path dir = fs::temp_directory_path() / "xbox_toolkit_ui_smoke";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // A mod that uses every control type and every action type, including all call argument kinds.
    {
        std::ofstream(dir / "everything.json") << R"({
          "game": "Everything",
          "settings": { "cbuf_address": "0x8226F590" },
          "controls": [
            { "type": "label", "text": "hello" },
            { "type": "separator", "label": "S" },
            { "type": "button", "label": "B", "actions": [
                { "cbuf": "god" },
                { "poke":   { "addr": "0x82001000", "bytes": "DE AD" } },
                { "poke32": { "addr": "0x82001004", "value": "{value}" } },
                { "pokef":  { "addr": "0x82001008", "value": 1.5 } },
                { "call":   { "addr": "0x82123456", "args": [7, "txt {value}", { "int": "{value}" }] } } ] },
            { "type": "checkbox", "label": "C", "default": true, "on_cbuf": "a", "off_cbuf": "b" },
            { "type": "textbox", "label": "T", "default": "x", "hint": "h", "button": "Go", "cbuf": "t {value}" }
          ] })";
    }
    ModFile everything;
    CHECK(loadModFile(dir / "everything.json", everything) && everything.controls.size() == 5,
          "test mod with every control/action type loads");

    int runCalls = 0;
    BuilderHost host;
    host.run = [&](const ModSettings &, std::vector<ModAction>, std::string) { runCalls++; };
    std::vector<ModFile> list;
    list.push_back(everything);
    host.mods = &list;
    host.folder = dir;

    // 1) The starter trainer, connected and not.
    {
        Builder b;
        for (bool connected : {false, true})
        {
            host.connected = connected;
            frames(3, [&] { drawBuilder(b, host); });
        }
        CHECK(true, "starter trainer renders (file settings, connected and not)");
    }

    // 2) Every control of the big mod, in the editor.
    {
        Builder b;
        builderOpen(b, everything);
        host.connected = true;
        for (int sel = -1; sel < (int)b.mod.controls.size(); sel++)
        {
            b.selected = sel;
            frames(3, [&] { drawBuilder(b, host); });
        }
        CHECK(true, "editor renders for file settings and each of the 5 controls");
        CHECK(runCalls == 0, "nothing ran without a click");
    }

    // 3) The shared control renderer used by trainer screen and preview.
    {
        ModFile m = everything;
        for (bool connected : {false, true})
            frames(2, [&] { drawModControls(m, connected, host.run); });
        CHECK(true, "drawModControls renders connected and disconnected");
    }

    // 4) An empty trainer and one with problems (exercises the "Checks" list).
    {
        Builder b;
        b.mod.controls.clear();
        b.selected = -1;
        frames(3, [&] { drawBuilder(b, host); });
        b.mod.game.clear();
        b.mod.settings.cbufAddr = 0;
        b.mod.controls.push_back(everything.controls[2]);
        b.selected = -1;
        frames(3, [&] { drawBuilder(b, host); });
        b.selected = 0;
        frames(3, [&] { drawBuilder(b, host); });
        CHECK(true, "empty trainer and one with validation problems render");
    }

    // 5) New / open helpers leave the builder in a consistent state.
    {
        Builder b;
        builderOpen(b, everything);
        CHECK(b.fileName == "everything" && !b.dirty && b.mod.controls[3].on, "builderOpen sets name, clean state, runtime state");
        builderNew(b);
        CHECK(b.fileName.empty() && !b.dirty && !b.mod.controls.empty(), "builderNew resets to the starter trainer");
    }

    // 6) Scanner tab: search, narrow, watch, peek, poke against fake memory (jobs run inline here).
    {
        const uint32_t base = 0x82000000;
        std::vector<uint8_t> mem(0x3000, 0);
        auto put32 = [&](uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a - base + i] = (uint8_t)(v >> (24 - 8 * i)); };
        auto get32 = [&](uint32_t a) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | mem[a - base + i]; return v; };
        put32(base + 0x100, 100);
        put32(base + 0x204, 100);
        put32(base + 0x1FF8, 100);
        put32(base + 0x300, 7);

        MemHost mh;
        mh.connected = true;
        mh.io.read = [&](uint32_t a, uint32_t n, std::vector<uint8_t> &out, std::string &err) {
            if (a < base || (uint64_t)a + n > base + mem.size()) { err = "unreadable"; return false; }
            out.assign(mem.begin() + (a - base), mem.begin() + (a - base) + n);
            return true;
        };
        mh.write = [&](uint32_t a, const std::vector<uint8_t> &d, std::string &err) {
            if (a < base || (uint64_t)a + d.size() > base + mem.size()) { err = "unwritable"; return false; }
            std::copy(d.begin(), d.end(), mem.begin() + (a - base));
            return true;
        };
        mh.submit = [](std::function<void()> f) { f(); };

        MemTools t;
        t.startHex = "82000000";
        t.endHex = "82003000";

        t.valueText = "abc";
        t.firstScan(mh);
        CHECK(!t.haveResults && t.message.find("Value") != std::string::npos, "bad value is rejected with a message");
        t.valueText = "100";
        t.endHex = "81000000";
        t.firstScan(mh);
        CHECK(!t.haveResults && t.message.find("above") != std::string::npos, "end <= start is rejected");
        t.endHex = "82003000";
        t.firstScan(mh);
        CHECK(t.haveResults && t.results.hits.size() == 3 && !t.busy, "first scan finds all three 100s and clears busy");

        put32(base + 0x100, 90);   // decreased
        put32(base + 0x204, 120);  // increased
        t.nextScan(mh, ScanMode::Decreased);
        CHECK(t.results.hits.size() == 1 && t.results.hits[0].addr == base + 0x100, "next scan Decreased keeps the one that dropped");
        t.valueText = "90";
        t.nextScan(mh, ScanMode::Exact);
        CHECK(t.results.hits.size() == 1, "next scan Exact on the new value keeps it");
        t.resetScan();
        CHECK(!t.haveResults && t.results.hits.empty(), "reset clears results");

        // Peek / poke through the watch list.
        t.addrHex = "82000300";
        t.manualType = (int)ValType::Int32;
        CHECK(t.addManualWatch(mh), "manual watch accepted");
        CHECK(t.watch.size() == 1 && t.watch[0].valid && t.watch[0].value == std::vector<uint8_t>({0, 0, 0, 7}), "peek reads the value");
        t.addrHex = "82000300";
        t.addManualWatch(mh);
        CHECK(t.watch.size() == 1, "duplicate watch is not added twice");
        t.watch[0].pokeText = "1234";
        t.poke(mh, t.watch[0].id);
        CHECK(get32(base + 0x300) == 1234, "poke writes a big-endian int32");
        CHECK(t.watch[0].value == std::vector<uint8_t>({0, 0, 4, 0xD2}), "watch shows the read-back value");
        t.watch[0].pokeText = "nope";
        t.poke(mh, t.watch[0].id);
        CHECK(get32(base + 0x300) == 1234, "invalid poke text writes nothing");

        t.addrHex = "82000400";
        t.manualType = (int)ValType::Bytes;
        t.manualLen = 3;
        t.addManualWatch(mh);
        t.watch[1].pokeText = "DE AD BE EF";
        t.poke(mh, t.watch[1].id);
        CHECK(mem[0x400] == 0xDE && mem[0x403] == 0xEF && t.watch[1].width == 4, "bytes poke writes and adopts the new length");

        t.addrHex = "83000000";
        t.manualType = (int)ValType::Int32;
        t.addManualWatch(mh);
        CHECK(!t.watch.back().valid && !t.watch.back().error.empty(), "peek of unreadable memory shows an error, not garbage");

        uint32_t a = 0;
        CHECK(parseAddress("0x82001234", a) && a == 0x82001234 && parseAddress(" 82001234 ", a) && !parseAddress("xyz", a) &&
                  !parseAddress("", a) && !parseAddress("1FFFFFFFF", a),
              "parseAddress accepts hex with or without 0x and rejects junk/overflow");

        // Render every state of the tab (no results, results, watch rows, disconnected, busy).
        MemTools r;
        for (bool connected : {false, true})
        {
            mh.connected = connected;
            frames(3, [&] { drawMemTools(r, mh); });
        }
        r.valueText = "100";
        r.endHex = "82003000";
        mh.connected = true;
        put32(base + 0x100, 100);
        r.firstScan(mh);
        r.addrHex = "82000100";
        r.addManualWatch(mh);
        r.autoRefresh = true;
        frames(4, [&] { drawMemTools(r, mh); });
        r.busy = true;
        frames(2, [&] { drawMemTools(r, mh); });
        r.busy = false;
        CHECK(true, "Scanner tab renders in all states");
    }

    ImGui::DestroyContext();
    fs::remove_all(dir);

    std::printf("\n%s (%d checks, %d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
