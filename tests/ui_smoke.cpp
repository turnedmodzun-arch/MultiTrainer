// Headless smoke test for the builder UI: runs real ImGui frames with no window or GPU.
//
// Dear ImGui asserts on Begin/End mismatches, unbalanced ID stacks and similar mistakes, so
// rendering every screen with asserts enabled catches that whole class of bug. It cannot
// check layout or that a click does the right thing; look at the real app for that.
#include "../src/builder.h"
#include "imgui.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

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

    ImGui::DestroyContext();
    fs::remove_all(dir);

    std::printf("\n%s (%d checks, %d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
