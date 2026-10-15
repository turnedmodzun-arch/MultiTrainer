#include "mod.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ------------------------------------------------------------------ parsing helpers
std::string formatHexBytes(const std::vector<uint8_t> &bytes)
{
    static const char *digits = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < bytes.size(); i++)
    {
        if (i) s += ' ';
        s += digits[bytes[i] >> 4];
        s += digits[bytes[i] & 0xF];
    }
    return s;
}

static uint32_t parseAddr(const json &j)
{
    if (j.is_number_integer()) return (uint32_t)j.get<int64_t>();
    if (j.is_string())
    {
        const std::string s = j.get<std::string>();
        char *end = nullptr;
        const unsigned long v = std::strtoul(s.c_str(), &end, 16);  // accepts a leading 0x
        if (s.empty() || *end != 0) throw std::runtime_error("bad address '" + s + "' (use hex like \"0x8226F590\")");
        return (uint32_t)v;
    }
    throw std::runtime_error("address must be a hex string or an integer");
}

static std::vector<uint8_t> parseHexBytes(const std::string &s)
{
    std::vector<uint8_t> out;
    int hi = -1;
    for (char c : s)
    {
        if (std::isspace((unsigned char)c)) continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else throw std::runtime_error("bad hex byte string '" + s + "'");
        if (hi < 0) hi = v;
        else { out.push_back((uint8_t)((hi << 4) | v)); hi = -1; }
    }
    if (hi >= 0) throw std::runtime_error("hex byte string has an odd number of digits");
    if (out.empty()) throw std::runtime_error("hex byte string is empty");
    return out;
}

// A value that is either a JSON number/string or a string; always stored as text so
// "{value}" can appear in it. Numbers are rendered back to text.
static std::string valueText(const json &j)
{
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) return std::to_string(j.get<int64_t>());
    if (j.is_number_float())
    {
        char b[64];
        std::snprintf(b, sizeof(b), "%.9g", j.get<double>());
        return b;
    }
    throw std::runtime_error("value must be a number or a string");
}

static ModAction parseAction(const json &a)
{
    if (!a.is_object() || a.size() != 1)
        throw std::runtime_error("each action must be an object with exactly one key (cbuf, poke, poke32, pokef or call)");

    ModAction act;
    const std::string key = a.begin().key();
    const json &v = a.begin().value();

    if (key == "cbuf")
    {
        if (!v.is_string()) throw std::runtime_error("\"cbuf\" must be a string");
        act.kind = ModAction::Cbuf;
        act.text = v.get<std::string>();
    }
    else if (key == "poke")
    {
        act.kind = ModAction::Poke;
        act.addr = parseAddr(v.at("addr"));
        act.bytes = parseHexBytes(v.at("bytes").get<std::string>());
        act.bytesHex = formatHexBytes(act.bytes);
    }
    else if (key == "poke32" || key == "pokef")
    {
        act.kind = (key == "poke32") ? ModAction::Poke32 : ModAction::PokeF;
        act.addr = parseAddr(v.at("addr"));
        act.text = valueText(v.at("value"));
    }
    else if (key == "call")
    {
        act.kind = ModAction::Call;
        act.addr = parseAddr(v.at("addr"));
        if (v.contains("args"))
        {
            if (!v["args"].is_array()) throw std::runtime_error("\"args\" must be an array");
            if (v["args"].size() > 9) throw std::runtime_error("a call takes at most 9 arguments");
            for (const json &x : v["args"])
            {
                ModArg arg;
                if (x.is_number_integer()) { arg.kind = ModArg::Int; arg.i = x.get<int64_t>(); }
                else if (x.is_string()) { arg.kind = ModArg::Str; arg.s = x.get<std::string>(); }
                else if (x.is_object() && x.size() == 1 && x.contains("int"))
                {
                    arg.kind = ModArg::IntExpr;
                    arg.s = valueText(x["int"]);
                }
                else throw std::runtime_error("call args must be integers, strings, or {\"int\": \"{value}\"}");
                act.args.push_back(std::move(arg));
            }
        }
    }
    else throw std::runtime_error("unknown action '" + key + "'");
    return act;
}

static std::vector<ModAction> parseActionList(const json &j)
{
    std::vector<ModAction> out;
    if (!j.is_array()) throw std::runtime_error("actions must be an array");
    for (const json &a : j) out.push_back(parseAction(a));
    return out;
}

static ModAction cbufAction(const std::string &text)
{
    ModAction a;
    a.kind = ModAction::Cbuf;
    a.text = text;
    return a;
}

// Reads "key" (an action array) and/or the "<prefix>cbuf" shorthand into out.
static void readActions(const json &c, const char *arrayKey, const char *cbufKey, std::vector<ModAction> &out)
{
    if (c.contains(arrayKey)) out = parseActionList(c[arrayKey]);
    if (c.contains(cbufKey))
    {
        if (!c[cbufKey].is_string()) throw std::runtime_error(std::string("\"") + cbufKey + "\" must be a string");
        out.push_back(cbufAction(c[cbufKey].get<std::string>()));
    }
}

static void parseControl(const json &c, ModControl &ctl)
{
    if (!c.is_object()) throw std::runtime_error("control must be an object");
    const std::string type = c.value("type", std::string());
    ctl.label = c.value("label", std::string());

    if (type == "label")
    {
        ctl.type = ModControl::Label;
        if (ctl.label.empty()) ctl.label = c.value("text", std::string());
    }
    else if (type == "separator")
    {
        ctl.type = ModControl::Separator;
    }
    else if (type == "button")
    {
        ctl.type = ModControl::Button;
        readActions(c, "actions", "cbuf", ctl.actions);
        if (ctl.actions.empty()) throw std::runtime_error("button has no actions");
    }
    else if (type == "checkbox")
    {
        ctl.type = ModControl::Checkbox;
        readActions(c, "on", "on_cbuf", ctl.onActions);
        readActions(c, "off", "off_cbuf", ctl.offActions);
        if (ctl.onActions.empty() && ctl.offActions.empty()) throw std::runtime_error("checkbox has no on/off actions");
        ctl.defaultOn = c.value("default", false);
        ctl.on = ctl.defaultOn;
    }
    else if (type == "textbox")
    {
        ctl.type = ModControl::TextBox;
        readActions(c, "actions", "cbuf", ctl.actions);
        if (ctl.actions.empty()) throw std::runtime_error("textbox has no actions");
        ctl.hint = c.value("hint", std::string());
        ctl.buttonLabel = c.value("button", std::string("Apply"));
        ctl.defaultText = c.value("default", std::string());
        std::strncpy(ctl.text, ctl.defaultText.c_str(), sizeof(ctl.text) - 1);
    }
    else throw std::runtime_error("unknown control type '" + type + "'");
}

// ------------------------------------------------------------------ loading
bool loadModFile(const fs::path &path, ModFile &out)
{
    out = ModFile();
    out.path = path;
    out.game = path.stem().string();

    std::ifstream f(path);
    if (!f)
    {
        out.error = "could not open file";
        return false;
    }

    try
    {
        // Comments (// and /* */) are allowed so mod authors can annotate their files.
        const json doc = json::parse(f, nullptr, true, /*ignore_comments=*/true);
        if (!doc.is_object()) throw std::runtime_error("top level must be a JSON object");

        out.game = doc.value("game", out.game);
        out.description = doc.value("description", std::string());

        const int version = doc.value("version", 1);
        if (version != 1) throw std::runtime_error("unsupported mod file version " + std::to_string(version));

        if (doc.contains("settings"))
        {
            const json &s = doc["settings"];
            if (s.contains("cbuf_address")) out.settings.cbufAddr = parseAddr(s["cbuf_address"]);
            out.settings.client = s.value("client", 0);
            out.settings.appendNewline = s.value("append_newline", false);
        }

        if (!doc.contains("controls") || !doc["controls"].is_array())
            throw std::runtime_error("missing \"controls\" array");

        size_t idx = 0;
        for (const json &c : doc["controls"])
        {
            idx++;
            ModControl ctl;
            try
            {
                parseControl(c, ctl);
                out.controls.push_back(std::move(ctl));
            }
            catch (const std::exception &e)
            {
                out.warnings.push_back("control #" + std::to_string(idx) + " skipped: " + e.what());
            }
        }
        if (out.controls.empty()) throw std::runtime_error("no usable controls");
    }
    catch (const std::exception &e)
    {
        out.error = e.what();
        out.controls.clear();
        return false;
    }
    return true;
}

std::vector<ModFile> scanMods(const fs::path &dir)
{
    std::vector<ModFile> mods;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return mods;

    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (ext != ".json") continue;

        ModFile m;
        loadModFile(e.path(), m);
        mods.push_back(std::move(m));
    }

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    std::sort(mods.begin(), mods.end(), [&](const ModFile &a, const ModFile &b) {
        if (a.ok() != b.ok()) return a.ok();  // broken files last
        return lower(a.game) < lower(b.game);
    });
    return mods;
}

// ------------------------------------------------------------------ saving / validating
bool parseHexString(const std::string &s, std::vector<uint8_t> &out, std::string &err)
{
    try
    {
        out = parseHexBytes(s);
        return true;
    }
    catch (const std::exception &e)
    {
        out.clear();
        err = e.what();
        return false;
    }
}

using ojson = nlohmann::ordered_json;  // keeps keys in the order we insert them

static std::string hexAddr(uint32_t a)
{
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", a);
    return b;
}

static ojson actionToJson(const ModAction &a)
{
    ojson j = ojson::object();
    switch (a.kind)
    {
    case ModAction::Cbuf:
        j["cbuf"] = a.text;
        break;
    case ModAction::Poke:
    {
        ojson p = ojson::object();
        p["addr"] = hexAddr(a.addr);
        p["bytes"] = a.bytes.empty() ? a.bytesHex : formatHexBytes(a.bytes);
        j["poke"] = p;
        break;
    }
    case ModAction::Poke32:
    case ModAction::PokeF:
    {
        ojson p = ojson::object();
        p["addr"] = hexAddr(a.addr);
        p["value"] = a.text;
        j[a.kind == ModAction::Poke32 ? "poke32" : "pokef"] = p;
        break;
    }
    case ModAction::Call:
    {
        ojson p = ojson::object();
        p["addr"] = hexAddr(a.addr);
        ojson args = ojson::array();
        for (const ModArg &m : a.args)
        {
            if (m.kind == ModArg::Int) args.push_back(m.i);
            else if (m.kind == ModArg::Str) args.push_back(m.s);
            else
            {
                ojson e = ojson::object();
                e["int"] = m.s;
                args.push_back(e);
            }
        }
        p["args"] = args;
        j["call"] = p;
        break;
    }
    }
    return j;
}

static ojson actionsToJson(const std::vector<ModAction> &v)
{
    ojson arr = ojson::array();
    for (const ModAction &a : v) arr.push_back(actionToJson(a));
    return arr;
}

std::string modToJson(const ModFile &mod)
{
    ojson doc = ojson::object();
    doc["game"] = mod.game;
    if (!mod.description.empty()) doc["description"] = mod.description;
    doc["version"] = 1;

    ojson s = ojson::object();
    if (mod.settings.cbufAddr) s["cbuf_address"] = hexAddr(mod.settings.cbufAddr);
    s["client"] = mod.settings.client;
    s["append_newline"] = mod.settings.appendNewline;
    doc["settings"] = s;

    ojson controls = ojson::array();
    for (const ModControl &c : mod.controls)
    {
        ojson j = ojson::object();
        switch (c.type)
        {
        case ModControl::Label:
            j["type"] = "label";
            j["text"] = c.label;
            break;
        case ModControl::Separator:
            j["type"] = "separator";
            if (!c.label.empty()) j["label"] = c.label;
            break;
        case ModControl::Button:
            j["type"] = "button";
            j["label"] = c.label;
            j["actions"] = actionsToJson(c.actions);
            break;
        case ModControl::Checkbox:
            j["type"] = "checkbox";
            j["label"] = c.label;
            j["default"] = c.defaultOn;
            j["on"] = actionsToJson(c.onActions);
            j["off"] = actionsToJson(c.offActions);
            break;
        case ModControl::TextBox:
            j["type"] = "textbox";
            j["label"] = c.label;
            if (!c.hint.empty()) j["hint"] = c.hint;
            if (!c.defaultText.empty()) j["default"] = c.defaultText;
            j["button"] = c.buttonLabel;
            j["actions"] = actionsToJson(c.actions);
            break;
        }
        controls.push_back(j);
    }
    doc["controls"] = controls;
    return doc.dump(2) + "\n";
}

bool saveModFile(const ModFile &mod, const fs::path &path, std::string &err)
{
    const std::string text = modToJson(mod);
    const fs::path tmp = fs::path(path).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            err = "could not write " + tmp.string();
            return false;
        }
        f << text;
        f.flush();
        if (!f)
        {
            err = "write failed for " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        err = "could not replace " + path.string();
        return false;
    }
    return true;
}

static void validateActions(const ModFile &mod, const std::vector<ModAction> &list, const std::string &where,
                            std::vector<std::string> &out)
{
    for (size_t i = 0; i < list.size(); i++)
    {
        const ModAction &a = list[i];
        const std::string at = where + ", action " + std::to_string(i + 1);
        switch (a.kind)
        {
        case ModAction::Cbuf:
            if (a.text.empty()) out.push_back(at + ": command is empty");
            if (mod.settings.cbufAddr == 0) out.push_back(at + ": console commands need a Cbuf_AddText address in the file settings");
            break;
        case ModAction::Poke:
            if (a.addr == 0) out.push_back(at + ": address is 0");
            if (a.bytes.empty()) out.push_back(at + ": no valid bytes (use hex like DE AD BE EF)");
            break;
        case ModAction::Poke32:
        case ModAction::PokeF:
            if (a.addr == 0) out.push_back(at + ": address is 0");
            if (a.text.empty()) out.push_back(at + ": value is empty");
            break;
        case ModAction::Call:
            if (a.addr == 0) out.push_back(at + ": function address is 0");
            if (a.args.size() > 9) out.push_back(at + ": a call takes at most 9 arguments");
            break;
        }
    }
}

std::vector<std::string> validateMod(const ModFile &mod)
{
    std::vector<std::string> out;
    if (mod.game.empty()) out.push_back("Game name is empty");
    if (mod.controls.empty()) out.push_back("The trainer has no controls");

    for (size_t i = 0; i < mod.controls.size(); i++)
    {
        const ModControl &c = mod.controls[i];
        const std::string where = "Control " + std::to_string(i + 1) + (c.label.empty() ? "" : " (" + c.label + ")");
        switch (c.type)
        {
        case ModControl::Label:
        case ModControl::Separator:
            break;
        case ModControl::Button:
            if (c.label.empty()) out.push_back(where + ": button has no label");
            if (c.actions.empty()) out.push_back(where + ": button has no actions");
            validateActions(mod, c.actions, where, out);
            break;
        case ModControl::Checkbox:
            if (c.label.empty()) out.push_back(where + ": checkbox has no label");
            if (c.onActions.empty() && c.offActions.empty()) out.push_back(where + ": checkbox has no on/off actions");
            validateActions(mod, c.onActions, where + " [on]", out);
            validateActions(mod, c.offActions, where + " [off]", out);
            break;
        case ModControl::TextBox:
            if (c.buttonLabel.empty()) out.push_back(where + ": text box has no button label");
            if (c.actions.empty()) out.push_back(where + ": text box has no actions");
            validateActions(mod, c.actions, where, out);
            break;
        }
    }
    return out;
}

// ------------------------------------------------------------------ running
std::string expandValue(const std::string &tmpl, const std::string &value)
{
    static const std::string token = "{value}";
    std::string out;
    size_t pos = 0;
    for (;;)
    {
        const size_t hit = tmpl.find(token, pos);
        if (hit == std::string::npos)
        {
            out.append(tmpl, pos, std::string::npos);
            return out;
        }
        out.append(tmpl, pos, hit - pos);
        out += value;
        pos = hit + token.size();
    }
}

static bool parseInt(const std::string &s, int64_t &v)
{
    char *end = nullptr;
    v = std::strtoll(s.c_str(), &end, 0);  // base 0: decimal, 0x hex, 0 octal
    return !s.empty() && end && *end == 0;
}

static std::string hex8(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

void runActions(XbdmClient &client, XrpcClient &xrpc, const ModSettings &settings,
                const std::vector<ModAction> &actions, const std::string &value,
                const std::function<void(const std::string &)> &log)
{
    for (const ModAction &a : actions)
    {
        std::string err;
        switch (a.kind)
        {
        case ModAction::Cbuf:
        {
            if (settings.cbufAddr == 0)
            {
                log("cbuf action needs \"settings\": {\"cbuf_address\": ...} in the mod file");
                return;
            }
            const std::string text = expandValue(a.text, value);
            const std::string payload = settings.appendNewline ? text + "\n" : text;
            uint32_t res = 0;
            if (xrpc.call(settings.cbufAddr, {XrpcArg::i(settings.client), XrpcArg::s(payload)}, res, err))
                log("cbuf \"" + text + "\"");
            else
            {
                log("cbuf \"" + text + "\" failed: " + err);
                return;  // a transport failure would repeat for every later action
            }
            break;
        }
        case ModAction::Poke:
            if (client.setMemory(a.addr, a.bytes.data(), a.bytes.size(), err))
                log("poke " + hex8(a.addr) + " (" + std::to_string(a.bytes.size()) + " bytes)");
            else { log("poke " + hex8(a.addr) + " failed: " + err); return; }
            break;

        case ModAction::Poke32:
        {
            const std::string expr = expandValue(a.text, value);
            int64_t v = 0;
            if (!parseInt(expr, v)) { log("poke32: '" + expr + "' is not an integer"); return; }
            const uint32_t u = (uint32_t)v;
            const uint8_t be[4] = {(uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8), (uint8_t)u};
            if (client.setMemory(a.addr, be, 4, err)) log("poke32 " + hex8(a.addr) + " = " + std::to_string(v));
            else { log("poke32 " + hex8(a.addr) + " failed: " + err); return; }
            break;
        }
        case ModAction::PokeF:
        {
            const std::string expr = expandValue(a.text, value);
            char *end = nullptr;
            const float f = std::strtof(expr.c_str(), &end);
            if (expr.empty() || !end || *end != 0) { log("pokef: '" + expr + "' is not a number"); return; }
            uint32_t u;
            std::memcpy(&u, &f, 4);
            const uint8_t be[4] = {(uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8), (uint8_t)u};
            if (client.setMemory(a.addr, be, 4, err)) log("pokef " + hex8(a.addr) + " = " + expr);
            else { log("pokef " + hex8(a.addr) + " failed: " + err); return; }
            break;
        }
        case ModAction::Call:
        {
            std::vector<XrpcArg> args;
            bool bad = false;
            for (const ModArg &m : a.args)
            {
                if (m.kind == ModArg::Int) args.push_back(XrpcArg::i((int32_t)m.i));
                else if (m.kind == ModArg::Str) args.push_back(XrpcArg::s(expandValue(m.s, value)));
                else
                {
                    const std::string expr = expandValue(m.s, value);
                    int64_t v = 0;
                    if (!parseInt(expr, v)) { log("call: '" + expr + "' is not an integer"); bad = true; break; }
                    XrpcArg x;
                    x.value = (uint64_t)v;
                    args.push_back(x);
                }
            }
            if (bad) return;
            uint32_t res = 0;
            if (xrpc.call(a.addr, args, res, err)) log("call " + hex8(a.addr) + " -> " + hex8(res));
            else { log("call " + hex8(a.addr) + " failed: " + err); return; }
            break;
        }
        }
    }
}
