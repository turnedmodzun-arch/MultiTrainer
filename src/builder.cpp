#include "builder.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace fs = std::filesystem;

static const char *const kTypeNames[] = {"Label", "Separator", "Button", "Check box", "Text box"};
static const char *const kActionNames[] = {"Console command (cbuf)", "Write bytes (poke)", "Write int32 (poke32)",
                                           "Write float (pokef)", "Call function (call)"};
static const char *const kArgNames[] = {"Integer", "Text", "Integer from expression"};

// ------------------------------------------------------------------ shared control drawing
void drawModControls(ModFile &mod, bool connected, const RunFn &run)
{
    for (size_t i = 0; i < mod.controls.size(); i++)
    {
        ModControl &c = mod.controls[i];
        ImGui::PushID((int)i);
        switch (c.type)
        {
        case ModControl::Label:
            ImGui::TextWrapped("%s", c.label.c_str());
            break;

        case ModControl::Separator:
            ImGui::Spacing();
            if (c.label.empty()) ImGui::Separator();
            else ImGui::SeparatorText(c.label.c_str());
            break;

        case ModControl::Button:
            ImGui::BeginDisabled(!connected);
            if (ImGui::Button(c.label.c_str())) run(mod.settings, c.actions, "");
            ImGui::EndDisabled();
            break;

        case ModControl::Checkbox:
            ImGui::BeginDisabled(!connected);
            if (ImGui::Checkbox(c.label.c_str(), &c.on)) run(mod.settings, c.on ? c.onActions : c.offActions, "");
            ImGui::EndDisabled();
            break;

        case ModControl::TextBox:
        {
            if (!c.label.empty()) ImGui::TextUnformatted(c.label.c_str());
            ImGui::BeginDisabled(!connected);
            ImGui::SetNextItemWidth(300);
            const bool enter = ImGui::InputTextWithHint("##tb", c.hint.c_str(), c.text, sizeof(c.text),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button(c.buttonLabel.c_str()) || enter) run(mod.settings, c.actions, c.text);
            ImGui::EndDisabled();
            break;
        }
        }
        ImGui::PopID();
    }
}

// ------------------------------------------------------------------ helpers
static void resetRuntime(ModFile &m)
{
    for (ModControl &c : m.controls)
    {
        c.on = c.defaultOn;
        std::memset(c.text, 0, sizeof(c.text));
        std::strncpy(c.text, c.defaultText.c_str(), sizeof(c.text) - 1);
    }
}

static ModAction newAction(ModAction::Kind k)
{
    ModAction a;
    a.kind = k;
    if (k == ModAction::Cbuf) a.text = "echo hello";
    if (k == ModAction::Poke32 || k == ModAction::PokeF) a.text = "{value}";
    return a;
}

static ModControl newControl(ModControl::Type t)
{
    ModControl c;
    c.type = t;
    switch (t)
    {
    case ModControl::Label:
        c.label = "Some text";
        break;
    case ModControl::Separator:
        c.label = "Section";
        break;
    case ModControl::Button:
        c.label = "New button";
        c.actions.push_back(newAction(ModAction::Cbuf));
        break;
    case ModControl::Checkbox:
        c.label = "New toggle";
        c.onActions.push_back(newAction(ModAction::Cbuf));
        c.onActions.back().text = "echo on";
        c.offActions.push_back(newAction(ModAction::Cbuf));
        c.offActions.back().text = "echo off";
        break;
    case ModControl::TextBox:
        c.label = "New text box";
        c.buttonLabel = "Apply";
        c.actions.push_back(newAction(ModAction::Cbuf));
        c.actions.back().text = "echo {value}";
        break;
    }
    return c;
}

static std::string controlTitle(const ModControl &c)
{
    std::string name = c.label.empty() ? "(no label)" : c.label;
    return std::string("[") + kTypeNames[(int)c.type] + "] " + name;
}

// Marks the builder dirty when a widget reports a change.
static bool ed(Builder &b, bool changed)
{
    if (changed) b.dirty = true;
    return changed;
}

template <class T> static bool moveItem(std::vector<T> &v, int i, int dir)
{
    const int j = i + dir;
    if (i < 0 || j < 0 || i >= (int)v.size() || j >= (int)v.size()) return false;
    std::swap(v[(size_t)i], v[(size_t)j]);
    return true;
}

static std::string sanitizeFileName(const Builder &b)
{
    std::string src = b.fileName.empty() ? b.mod.game : b.fileName;
    if (src.size() > 5 && src.compare(src.size() - 5, 5, ".json") == 0) src.erase(src.size() - 5);
    std::string out;
    for (char c : src)
    {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_') out += c;
        else if (c == ' ' || c == '.') out += '_';
    }
    return out.empty() ? "trainer" : out;
}

void builderNew(Builder &b)
{
    b.mod = ModFile();
    b.mod.game = "New trainer";
    b.mod.controls.push_back(newControl(ModControl::Separator));
    b.mod.controls.push_back(newControl(ModControl::Button));
    resetRuntime(b.mod);
    b.fileName.clear();
    b.selected = -1;
    b.dirty = false;
    b.status.clear();
}

void builderOpen(Builder &b, const ModFile &m)
{
    b.mod = m;
    b.mod.warnings.clear();
    resetRuntime(b.mod);
    b.fileName = m.path.stem().string();
    b.selected = -1;
    b.dirty = false;
    b.status = "Opened " + m.path.filename().string();
}

Builder::Builder() { builderNew(*this); }

// ------------------------------------------------------------------ action editors
enum class ActCmd { None, Up, Down, Delete };

static void drawAddress(Builder &b, const char *label, uint32_t &addr)
{
    ImGui::SetNextItemWidth(130);
    ed(b, ImGui::InputScalar(label, ImGuiDataType_U32, &addr, nullptr, nullptr, "%08X",
                             ImGuiInputTextFlags_CharsHexadecimal));
}

static ActCmd drawAction(Builder &b, const BuilderHost &h, ModAction &a, int index, int count)
{
    ActCmd cmd = ActCmd::None;
    ImGui::PushID(index);
    ImGui::BeginChild("##action", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    int k = (int)a.kind;
    ImGui::SetNextItemWidth(220);
    if (ed(b, ImGui::Combo("##kind", &k, kActionNames, 5))) a.kind = (ModAction::Kind)k;

    ImGui::SameLine();
    ImGui::BeginDisabled(!h.connected || !h.run);
    if (ImGui::Button("Run") && h.run) h.run(b.mod.settings, {a}, b.testValue);
    ImGui::EndDisabled();
    if (!h.connected && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Connect to the console to test this action. {value} is filled from \"Test value\".");

    ImGui::SameLine();
    ImGui::BeginDisabled(index == 0);
    if (ImGui::Button("Up")) cmd = ActCmd::Up;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(index + 1 >= count);
    if (ImGui::Button("Down")) cmd = ActCmd::Down;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) cmd = ActCmd::Delete;

    switch (a.kind)
    {
    case ModAction::Cbuf:
        ImGui::SetNextItemWidth(-1);
        ed(b, ImGui::InputTextWithHint("##cmd", "console command, e.g. god", &a.text));
        break;

    case ModAction::Poke:
        drawAddress(b, "Address", a.addr);
        ImGui::SetNextItemWidth(-1);
        if (ed(b, ImGui::InputTextWithHint("##bytes", "bytes in hex, e.g. DE AD BE EF", &a.bytesHex)))
        {
            std::string e;
            if (!parseHexString(a.bytesHex, a.bytes, e)) a.bytes.clear();
        }
        if (a.bytes.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not valid hex yet (needs whole bytes).");
        break;

    case ModAction::Poke32:
    case ModAction::PokeF:
        drawAddress(b, "Address", a.addr);
        ImGui::SetNextItemWidth(-1);
        ed(b, ImGui::InputTextWithHint("##val", a.kind == ModAction::Poke32 ? "integer (or 0x hex), or {value}" : "number, or {value}",
                                       &a.text));
        break;

    case ModAction::Call:
    {
        drawAddress(b, "Function address", a.addr);
        int remove = -1;
        for (size_t i = 0; i < a.args.size(); i++)
        {
            ModArg &m = a.args[i];
            ImGui::PushID((int)i);
            ImGui::Text("arg %d", (int)i + 1);
            ImGui::SameLine();
            int t = (int)m.kind;
            ImGui::SetNextItemWidth(190);
            if (ed(b, ImGui::Combo("##type", &t, kArgNames, 3))) m.kind = (ModArg::Kind)t;
            ImGui::SameLine();
            if (m.kind == ModArg::Int)
            {
                ImGui::SetNextItemWidth(160);
                ed(b, ImGui::InputScalar("##int", ImGuiDataType_S64, &m.i));
            }
            else
            {
                ImGui::SetNextItemWidth(260);
                ed(b, ImGui::InputText("##text", &m.s));
            }
            ImGui::SameLine();
            if (ImGui::Button("x")) remove = (int)i;
            ImGui::PopID();
        }
        if (remove >= 0)
        {
            a.args.erase(a.args.begin() + remove);
            b.dirty = true;
        }
        ImGui::BeginDisabled(a.args.size() >= 9);
        if (ImGui::Button("+ Add argument"))
        {
            a.args.push_back(ModArg());
            b.dirty = true;
        }
        ImGui::EndDisabled();
        break;
    }
    }

    ImGui::EndChild();
    ImGui::PopID();
    return cmd;
}

static void drawActionList(Builder &b, const BuilderHost &h, std::vector<ModAction> &list, const char *title)
{
    ImGui::PushID(title);
    ImGui::SeparatorText(title);

    int del = -1, up = -1, down = -1;
    for (size_t i = 0; i < list.size(); i++)
    {
        const ActCmd c = drawAction(b, h, list[i], (int)i, (int)list.size());
        if (c == ActCmd::Delete) del = (int)i;
        else if (c == ActCmd::Up) up = (int)i;
        else if (c == ActCmd::Down) down = (int)i;
    }
    if (del >= 0) { list.erase(list.begin() + del); b.dirty = true; }
    else if (up >= 0) { moveItem(list, up, -1); b.dirty = true; }
    else if (down >= 0) { moveItem(list, down, +1); b.dirty = true; }

    if (ImGui::Button("+ Add action")) ImGui::OpenPopup("add_action");
    if (ImGui::BeginPopup("add_action"))
    {
        for (int k = 0; k < 5; k++)
        {
            if (ImGui::Selectable(kActionNames[k]))
            {
                list.push_back(newAction((ModAction::Kind)k));
                b.dirty = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// ------------------------------------------------------------------ control + file editors
static void drawControlEditor(Builder &b, const BuilderHost &h, ModControl &c)
{
    int t = (int)c.type;
    ImGui::SetNextItemWidth(160);
    if (ed(b, ImGui::Combo("Type", &t, kTypeNames, 5)))
    {
        const ModControl::Type old = c.type;
        c.type = (ModControl::Type)t;
        // Carry the actions across when switching between a button/text box and a check box.
        if (c.type == ModControl::Checkbox && old != ModControl::Checkbox && c.onActions.empty())
            c.onActions = c.actions;
        if (old == ModControl::Checkbox && c.type != ModControl::Checkbox && c.actions.empty())
            c.actions = c.onActions;
        if (c.type == ModControl::TextBox && c.buttonLabel.empty()) c.buttonLabel = "Apply";
    }

    const char *labelName = c.type == ModControl::Label       ? "Text"
                            : c.type == ModControl::Separator ? "Heading (optional)"
                                                              : "Label";
    ImGui::SetNextItemWidth(320);
    ed(b, ImGui::InputText(labelName, &c.label));

    switch (c.type)
    {
    case ModControl::Label:
    case ModControl::Separator:
        break;

    case ModControl::Button:
        drawActionList(b, h, c.actions, "When clicked");
        break;

    case ModControl::Checkbox:
        if (ed(b, ImGui::Checkbox("Starts ticked (nothing is sent at load)", &c.defaultOn))) c.on = c.defaultOn;
        drawActionList(b, h, c.onActions, "When ticked");
        drawActionList(b, h, c.offActions, "When cleared");
        break;

    case ModControl::TextBox:
        ImGui::SetNextItemWidth(320);
        ed(b, ImGui::InputText("Button text", &c.buttonLabel));
        ImGui::SetNextItemWidth(320);
        ed(b, ImGui::InputText("Placeholder hint", &c.hint));
        ImGui::SetNextItemWidth(320);
        if (ed(b, ImGui::InputText("Starting text", &c.defaultText)))
        {
            std::memset(c.text, 0, sizeof(c.text));
            std::strncpy(c.text, c.defaultText.c_str(), sizeof(c.text) - 1);
        }
        ImGui::TextDisabled("The text typed in the box replaces {value} in the actions below.");
        drawActionList(b, h, c.actions, "When the button is pressed");
        break;
    }
}

static void drawFileSettings(Builder &b)
{
    ImGui::SeparatorText("Trainer");
    ImGui::SetNextItemWidth(360);
    ed(b, ImGui::InputText("Game name", &b.mod.game));
    ImGui::SetNextItemWidth(360);
    ed(b, ImGui::InputTextWithHint("Description", "shown under the name in the game list", &b.mod.description));

    ImGui::SeparatorText("Console commands");
    drawAddress(b, "Cbuf_AddText address", b.mod.settings.cbufAddr);
    ImGui::SetNextItemWidth(90);
    ed(b, ImGui::InputInt("Client number", &b.mod.settings.client, 0, 0));
    ed(b, ImGui::Checkbox("Append a newline to each command", &b.mod.settings.appendNewline));
    ImGui::TextDisabled("Needed by every \"console command\" action. Leave blank if the trainer only writes memory.");

    ImGui::SeparatorText("Testing");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Test value", &b.testValue);
    ImGui::TextDisabled("Fills {value} when you press Run on a single action.");

    const std::vector<std::string> problems = validateMod(b.mod);
    ImGui::SeparatorText("Checks");
    if (problems.empty()) ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "No problems found.");
    for (const std::string &p : problems) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "- %s", p.c_str());
}

// ------------------------------------------------------------------ left pane
static void drawControlList(Builder &b)
{
    if (ImGui::Selectable("File settings", b.selected == -1)) b.selected = -1;
    ImGui::Separator();

    ImGui::BeginChild("##controllist", ImVec2(0, -64), ImGuiChildFlags_None);
    for (size_t i = 0; i < b.mod.controls.size(); i++)
    {
        ImGui::PushID((int)i);
        const std::string title = controlTitle(b.mod.controls[i]);
        if (ImGui::Selectable(title.c_str(), b.selected == (int)i)) b.selected = (int)i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (ImGui::Button("+ Add control")) ImGui::OpenPopup("add_control");
    if (ImGui::BeginPopup("add_control"))
    {
        for (int t = 0; t < 5; t++)
        {
            if (ImGui::Selectable(kTypeNames[t]))
            {
                // Insert after the selection, or at the end.
                const size_t at = (b.selected >= 0 && b.selected < (int)b.mod.controls.size()) ? (size_t)b.selected + 1
                                                                                              : b.mod.controls.size();
                b.mod.controls.insert(b.mod.controls.begin() + (long)at, newControl((ModControl::Type)t));
                resetRuntime(b.mod);
                b.selected = (int)at;
                b.dirty = true;
            }
        }
        ImGui::EndPopup();
    }

    const bool has = b.selected >= 0 && b.selected < (int)b.mod.controls.size();
    ImGui::BeginDisabled(!has);
    if (ImGui::Button("Up") && moveItem(b.mod.controls, b.selected, -1)) { b.selected--; b.dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Down") && moveItem(b.mod.controls, b.selected, +1)) { b.selected++; b.dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Copy") && has)
    {
        b.mod.controls.insert(b.mod.controls.begin() + b.selected + 1, b.mod.controls[(size_t)b.selected]);
        b.selected++;
        b.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && has)
    {
        b.mod.controls.erase(b.mod.controls.begin() + b.selected);
        if (b.selected >= (int)b.mod.controls.size()) b.selected = (int)b.mod.controls.size() - 1;
        b.dirty = true;
    }
    ImGui::EndDisabled();
}

// ------------------------------------------------------------------ file bar + popups
static void doSave(Builder &b, const BuilderHost &h, const fs::path &path)
{
    std::error_code ec;
    fs::create_directories(h.folder, ec);
    std::string err;
    if (saveModFile(b.mod, path, err))
    {
        b.mod.path = path;
        b.fileName = path.stem().string();
        b.dirty = false;
        b.needsRescan = true;
        b.status = "Saved " + path.string();
    }
    else b.status = "Save failed: " + err;
}

static void drawFileBar(Builder &b, const BuilderHost &h)
{
    if (ImGui::Button("New"))
    {
        if (b.dirty)
        {
            b.pending = Builder::Pending::New;
            ImGui::OpenPopup("Discard changes?");
        }
        else builderNew(b);
    }
    ImGui::SameLine();

    // Note: OpenPopup must run at this ID level, not inside the combo's own popup window,
    // or the modal below (begun at this level) would never match it.
    bool askDiscard = false;
    ImGui::SetNextItemWidth(230);
    if (ImGui::BeginCombo("##open", "Open existing..."))
    {
        if (h.mods)
        {
            for (size_t i = 0; i < h.mods->size(); i++)
            {
                const ModFile &m = (*h.mods)[i];
                if (!m.ok()) continue;
                ImGui::PushID((int)i);
                const std::string title = m.game + "  (" + m.path.filename().string() + ")";
                if (ImGui::Selectable(title.c_str()))
                {
                    if (b.dirty)
                    {
                        b.pending = Builder::Pending::Open;
                        b.pendingOpen = m;
                        askDiscard = true;
                    }
                    else builderOpen(b, m);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    if (askDiscard) ImGui::OpenPopup("Discard changes?");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##fname", sanitizeFileName(b).c_str(), &b.fileName);
    ImGui::SameLine();
    ImGui::TextUnformatted(".json");
    ImGui::SameLine();
    if (ImGui::Button(b.dirty ? "Save *" : "Save"))
    {
        const fs::path path = h.folder / (sanitizeFileName(b) + ".json");
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        const bool same = exists && !b.mod.path.empty() && fs::equivalent(path, b.mod.path, ec);
        if (exists && !same)
        {
            b.askOverwrite = true;
            ImGui::OpenPopup("Overwrite file?");
        }
        else doSave(b, h, path);
    }
    if (!b.status.empty()) ImGui::TextDisabled("%s", b.status.c_str());

    // Popups are opened and begun at this same ID level, outside any child window.
    if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("This trainer has unsaved changes.");
        if (ImGui::Button("Discard them"))
        {
            if (b.pending == Builder::Pending::New) builderNew(b);
            else if (b.pending == Builder::Pending::Open) builderOpen(b, b.pendingOpen);
            b.pending = Builder::Pending::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing"))
        {
            b.pending = Builder::Pending::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Overwrite file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s.json already exists in the mods folder.", sanitizeFileName(b).c_str());
        if (ImGui::Button("Overwrite"))
        {
            doSave(b, h, h.folder / (sanitizeFileName(b) + ".json"));
            b.askOverwrite = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            b.askOverwrite = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------ builder screen
void drawBuilder(Builder &b, const BuilderHost &h)
{
    drawFileBar(b, h);
    ImGui::Separator();

    ImGui::BeginChild("##left", ImVec2(270, 0), ImGuiChildFlags_Borders);
    drawControlList(b);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, 0), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("##builder_tabs"))
    {
        if (ImGui::BeginTabItem("Edit"))
        {
            if (b.selected >= 0 && b.selected < (int)b.mod.controls.size())
                drawControlEditor(b, h, b.mod.controls[(size_t)b.selected]);
            else
            {
                b.selected = -1;
                drawFileSettings(b);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Preview"))
        {
            ImGui::TextWrapped("This is how the trainer will look. Controls are live: with the console connected, "
                               "they run their actions.");
            ImGui::Separator();
            if (b.mod.controls.empty()) ImGui::TextDisabled("(no controls yet)");
            drawModControls(b.mod, h.connected, h.run ? h.run : RunFn([](const ModSettings &, std::vector<ModAction>, std::string) {}));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}
