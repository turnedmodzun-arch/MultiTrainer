// In-app trainer builder: create and edit mod files without writing JSON by hand.
// Depends on Dear ImGui (with misc/cpp/imgui_stdlib) and the mod core, not on the console.
#pragma once
#include "mod.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Queues actions for execution (the host runs them on its worker thread).
using RunFn = std::function<void(const ModSettings &, std::vector<ModAction>, std::string)>;

// Draws a mod's controls (shared by the trainer screen and the builder preview).
void drawModControls(ModFile &mod, bool connected, const RunFn &run);

struct Builder
{
    Builder();  // starts with a small starter trainer

    ModFile mod;
    std::string fileName;  // target file name without ".json"
    bool dirty = false;
    int selected = -1;     // control index, or -1 for the file settings
    std::string testValue = "test";  // fills {value} when running a single action
    std::string status;    // result of the last open/save
    bool needsRescan = false;  // set after a save; the host should refresh its game list

    // Pending confirmations (modal popups).
    enum class Pending { None, New, Open } pending = Pending::None;
    ModFile pendingOpen;
    bool askOverwrite = false;
};

struct BuilderHost
{
    bool connected = false;
    std::filesystem::path folder;           // where trainers are saved
    const std::vector<ModFile> *mods = nullptr;  // for the "Open existing" list
    RunFn run;
};

void builderNew(Builder &b);
void builderOpen(Builder &b, const ModFile &m);
void drawBuilder(Builder &b, const BuilderHost &host);
