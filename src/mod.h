// Per-game "mod files": JSON documents that describe a trainer's controls and what
// each one does on the console. See README.md for the full format.
#pragma once
#include "xbdm.h"
#include "xrpc.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// One argument of a "call" action.
struct ModArg
{
    enum Kind { Int, Str, IntExpr } kind = Int;
    int64_t i = 0;   // Int
    std::string s;   // Str (the text) or IntExpr (an expression that expands to a number)
};

struct ModAction
{
    enum Kind { Cbuf, Poke, Poke32, PokeF, Call } kind = Cbuf;
    std::string text;            // Cbuf: command text.  Poke32/PokeF: value expression.
    uint32_t addr = 0;           // Poke*, Call
    std::vector<uint8_t> bytes;  // Poke (what actually gets written)
    std::string bytesHex;        // Poke: the same bytes as editable text, e.g. "DE AD BE EF"
    std::vector<ModArg> args;    // Call
};

struct ModSettings
{
    uint32_t cbufAddr = 0;  // Cbuf_AddText address; 0 = not provided
    int client = 0;
    bool appendNewline = false;
};

struct ModControl
{
    enum Type { Label, Separator, Button, Checkbox, TextBox } type = Label;
    std::string label;
    std::string hint;                 // TextBox placeholder
    std::string buttonLabel = "Apply";  // TextBox
    std::string defaultText;
    bool defaultOn = false;
    std::vector<ModAction> actions;     // Button, TextBox
    std::vector<ModAction> onActions;   // Checkbox
    std::vector<ModAction> offActions;  // Checkbox

    // Runtime UI state (not part of the file).
    bool on = false;
    char text[256] = {0};
};

struct ModFile
{
    std::filesystem::path path;
    std::string game;
    std::string description;
    ModSettings settings;
    std::vector<ModControl> controls;
    std::vector<std::string> warnings;  // non-fatal problems (skipped controls etc.)
    std::string error;                  // fatal problem; the mod can't be loaded
    bool ok() const { return error.empty(); }
};

// Parses one mod file. On failure out.error is set (out.game falls back to the file name).
bool loadModFile(const std::filesystem::path &path, ModFile &out);

// Loads every *.json in dir (non-recursive). Valid mods first, sorted by name.
std::vector<ModFile> scanMods(const std::filesystem::path &dir);

// Serialises a mod to its JSON text (stable key order, 2-space indent).
std::string modToJson(const ModFile &mod);

// Writes the mod as JSON (via a temp file, then rename). Fills err on failure.
bool saveModFile(const ModFile &mod, const std::filesystem::path &path, std::string &err);

// Human-readable problems that would make controls misbehave at run time.
// Empty means the mod looks fine. Used by the builder; loading doesn't require it.
std::vector<std::string> validateMod(const ModFile &mod);

// "DE AD be ef" <-> bytes. parseHexString fails on odd digit counts, junk, or empty input.
bool parseHexString(const std::string &s, std::vector<uint8_t> &out, std::string &err);
std::string formatHexBytes(const std::vector<uint8_t> &bytes);

// Replaces every "{value}" in tmpl with value.
std::string expandValue(const std::string &tmpl, const std::string &value);

// Executes actions in order, reporting each outcome through log. `value` feeds {value}.
// Blocking: call it from a worker thread.
void runActions(XbdmClient &client, XrpcClient &xrpc, const ModSettings &settings,
                const std::vector<ModAction> &actions, const std::string &value,
                const std::function<void(const std::string &)> &log);
