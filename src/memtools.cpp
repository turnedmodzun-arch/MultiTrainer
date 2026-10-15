#include "memtools.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

static const char *kModeNames[] = {"Exact value", "Changed", "Unchanged", "Increased", "Decreased"};
static constexpr int kModeCount = 5;

bool parseAddress(const std::string &text, uint32_t &out)
{
    size_t a = 0, b = text.size();
    while (a < b && std::isspace((unsigned char)text[a])) a++;
    while (b > a && std::isspace((unsigned char)text[b - 1])) b--;
    const std::string s = text.substr(a, b - a);
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 16);  // accepts a leading 0x
    if (end == s.c_str() || *end != 0 || errno == ERANGE || v > 0xFFFFFFFFull) return false;
    out = (uint32_t)v;
    return true;
}

static std::string hex8(uint32_t a)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", a);
    return b;
}

static bool needsLength(ValType t) { return valTypeWidth(t) == 0; }

// ------------------------------------------------------------------ scanning
void MemTools::firstScan(const MemHost &host)
{
    if (busy) return;
    std::string err;
    const ValType ty = (ValType)type;
    std::vector<uint8_t> pat;
    if (!parseValue(ty, valueText, pat, err)) { setMessage("Value: " + err); return; }
    uint32_t s = 0, e = 0;
    if (!parseAddress(startHex, s)) { setMessage("Start must be a hex address"); return; }
    if (!parseAddress(endHex, e)) { setMessage("End must be a hex address"); return; }
    if (e <= s) { setMessage("End must be above Start"); return; }

    const size_t cap = (size_t)std::clamp(maxHits, 1, 1000000);
    const bool al = aligned;
    busy = true;
    cancel = false;
    progDone = 0;
    progTotal = (uint64_t)e - s;
    progHits = 0;
    setMessage("Scanning...");

    host.submit([this, io = host.io, log = host.log, s, e, ty, pat, al, cap] {
        ScanResults r;
        std::string err;
        const bool ok = ::firstScan(io, s, e, ty, pat, al, cap, r,
                                    [this](uint64_t d, uint64_t tot, size_t h) {
                                        progDone = d;
                                        progTotal = tot;
                                        progHits = h;
                                    },
                                    &cancel, err);
        std::string msg;
        if (ok)
        {
            msg = "Found " + std::to_string(r.hits.size()) + " result(s)";
            if (r.truncated) msg += " (stopped at the result limit; narrow the range or raise the limit)";
            if (r.cancelled) msg += " (cancelled, partial)";
            if (r.skippedBlocks) msg += "; " + std::to_string(r.skippedBlocks) + " unreadable 4 KB block(s) skipped";
            std::lock_guard<std::mutex> lk(mu);
            results = std::move(r);
            haveResults = true;
            message = msg;
        }
        else
        {
            msg = "Scan failed: " + err;
            std::lock_guard<std::mutex> lk(mu);
            message = msg;
        }
        if (log) log(msg);
        busy = false;
    });
}

void MemTools::nextScan(const MemHost &host, ScanMode m)
{
    if (busy) return;
    ScanResults copy;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!haveResults) { message = "Run a first scan before narrowing"; return; }
        copy = results;
    }
    std::vector<uint8_t> pat;
    std::string err;
    if (m == ScanMode::Exact && !parseValue(copy.type, valueText, pat, err)) { setMessage("Value: " + err); return; }

    busy = true;
    cancel = false;
    progDone = 0;
    progTotal = copy.hits.size();
    progHits = 0;
    setMessage("Scanning...");

    host.submit([this, io = host.io, log = host.log, copy = std::move(copy), m, pat]() mutable {
        std::string err;
        const bool ok = ::nextScan(io, copy, m, pat,
                                   [this](uint64_t d, uint64_t tot, size_t h) {
                                       progDone = d;
                                       progTotal = tot;
                                       progHits = h;
                                   },
                                   &cancel, err);
        std::string msg;
        if (!ok) msg = "Scan failed: " + err;
        else if (copy.cancelled) msg = "Cancelled; results unchanged";
        else
        {
            msg = std::to_string(copy.hits.size()) + " result(s) left";
            if (copy.unreadableHits) msg += "; " + std::to_string(copy.unreadableHits) + " could no longer be read and were dropped";
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            if (ok && !copy.cancelled) results = std::move(copy);
            message = msg;
        }
        if (log) log(msg);
        busy = false;
    });
}

void MemTools::resetScan()
{
    if (busy) return;
    std::lock_guard<std::mutex> lk(mu);
    results = ScanResults();
    haveResults = false;
    message = "Results cleared";
}

// ------------------------------------------------------------------ peek / poke
void MemTools::addWatch(uint32_t addr, ValType ty, size_t width, const std::vector<uint8_t> &initial)
{
    std::lock_guard<std::mutex> lk(mu);
    for (const WatchEntry &w : watch)
        if (w.addr == addr && w.type == ty && w.width == width) return;  // already watching this
    WatchEntry w;
    w.id = nextId++;
    w.addr = addr;
    w.type = ty;
    w.width = width;
    w.value = initial;
    w.valid = initial.size() == width;
    w.pokeText = w.valid ? formatValue(ty, initial) : "";
    // Seed the poke box with something the parser accepts (formatValue adds extras for ints).
    if (w.valid && valTypeWidth(ty) != 0 && ty != ValType::Float)
    {
        double d = 0;
        if (decodeNumber(ty, initial, d)) w.pokeText = std::to_string((long long)d);
    }
    watch.push_back(std::move(w));
}

bool MemTools::addManualWatch(const MemHost &host)
{
    uint32_t a = 0;
    if (!parseAddress(addrHex, a)) { setMessage("Address must be hex, like 82001234"); return false; }
    const ValType ty = (ValType)manualType;
    size_t w = valTypeWidth(ty);
    if (needsLength(ty))
    {
        if (manualLen < 1 || manualLen > 256) { setMessage("Length must be 1 to 256"); return false; }
        w = (size_t)manualLen;
    }
    if ((uint64_t)a + w > 0x100000000ull) { setMessage("Address + length runs past the end of memory"); return false; }
    addWatch(a, ty, w, {});
    if (host.connected) peekAll(host);
    return true;
}

void MemTools::peekAll(const MemHost &host)
{
    if (watchBusy.exchange(true)) return;
    host.submit([this, io = host.io] {
        struct Job { int id; uint32_t addr; size_t width; };
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const WatchEntry &w : watch) jobs.push_back({w.id, w.addr, w.width});
        }
        for (const Job &j : jobs)
        {
            std::vector<uint8_t> buf;
            std::string err;
            const bool ok = io.read && io.read(j.addr, (uint32_t)j.width, buf, err) && buf.size() == j.width;
            std::lock_guard<std::mutex> lk(mu);
            for (WatchEntry &w : watch)
            {
                if (w.id != j.id) continue;
                w.valid = ok;
                if (ok) { w.value = buf; w.error.clear(); }
                else w.error = err.empty() ? "read failed" : err;
            }
        }
        watchBusy = false;
    });
}

void MemTools::poke(const MemHost &host, int id)
{
    ValType ty = ValType::Int32;
    uint32_t addr = 0;
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mu);
        const auto it = std::find_if(watch.begin(), watch.end(), [id](const WatchEntry &w) { return w.id == id; });
        if (it == watch.end()) return;
        ty = it->type;
        addr = it->addr;
        text = it->pokeText;
    }
    std::vector<uint8_t> bytes;
    std::string err;
    if (!parseValue(ty, text, bytes, err)) { setMessage("Poke " + hex8(addr) + ": " + err); return; }
    if ((uint64_t)addr + bytes.size() > 0x100000000ull) { setMessage("Poke runs past the end of memory"); return; }
    if (!host.write) return;

    watchBusy = true;  // keeps auto-refresh from interleaving; cleared below
    host.submit([this, io = host.io, write = host.write, log = host.log, id, addr, ty, bytes] {
        std::string err;
        std::string msg;
        if (!write(addr, bytes, err)) msg = "Poke " + hex8(addr) + " failed: " + err;
        else
        {
            // Read back so the list shows what is really there (a game may overwrite it at once).
            std::vector<uint8_t> back;
            std::string rerr;
            const bool ok = io.read && io.read(addr, (uint32_t)bytes.size(), back, rerr) && back.size() == bytes.size();
            msg = "Poked " + std::to_string(bytes.size()) + " byte(s) at " + hex8(addr);
            if (ok && back != bytes) msg += " (read back " + formatValue(ty, back) + ": the game changed it again)";
            std::lock_guard<std::mutex> lk(mu);
            for (WatchEntry &w : watch)
            {
                if (w.id != id) continue;
                if (valTypeWidth(ty) == 0) w.width = bytes.size();  // Bytes/Text take the length written
                w.value = ok ? back : bytes;
                w.valid = true;
                w.error.clear();
            }
            message = msg;
        }
        if (!err.empty()) { std::lock_guard<std::mutex> lk(mu); message = msg; }
        if (log) log(msg);
        watchBusy = false;
    });
}

// ------------------------------------------------------------------ drawing
static void drawSearch(MemTools &t, const MemHost &host)
{
    bool have;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        have = t.haveResults;
    }
    const bool busy = t.busy;
    const bool canScan = host.connected && !busy;

    ImGui::SeparatorText("Search");
    ImGui::BeginDisabled(have || busy);
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("Start", &t.startHex, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("End", &t.endHex, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("Type", valTypeName((ValType)t.type)))
    {
        for (int i = 0; i < kValTypeCount; i++)
            if (ImGui::Selectable(valTypeName((ValType)i), t.type == i)) t.type = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(220);
    ImGui::InputText("Value", &t.valueText);
    ImGui::SameLine();
    ImGui::BeginDisabled(have || busy);
    ImGui::Checkbox("Aligned", &t.aligned);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("Max results", &t.maxHits, 0, 0);
    t.maxHits = std::clamp(t.maxHits, 1, 1000000);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!canScan || have);
    if (ImGui::Button("First scan")) t.firstScan(host);
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!canScan || !have);
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##mode", kModeNames[t.mode]))
    {
        for (int i = 0; i < kModeCount; i++)
            if (ImGui::Selectable(kModeNames[i], t.mode == i)) t.mode = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Next scan")) t.nextScan(host, (ScanMode)t.mode);
    ImGui::SameLine();
    if (ImGui::Button("Refresh values")) t.nextScan(host, ScanMode::Refresh);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !have);
    if (ImGui::Button("Reset")) t.resetScan();
    ImGui::EndDisabled();
    if (busy)
    {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) t.cancel = true;
        const uint64_t tot = t.progTotal, done = t.progDone;
        ImGui::ProgressBar(tot ? (float)((double)done / (double)tot) : 0.0f, ImVec2(-1, 0));
        ImGui::TextDisabled("%llu match(es) so far", (unsigned long long)t.progHits.load());
    }

    if (!host.connected) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Connect to the console to scan.");
    else if (!have) ImGui::TextDisabled("First scan looks for the value; then change it in-game and use Next scan to narrow.");

    std::string msg;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        msg = t.message;
    }
    if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
}

static void drawResults(MemTools &t)
{
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> toWatch;
    ValType ty = ValType::Int32;
    size_t w = 0;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        if (!t.haveResults) return;
        ty = t.results.type;
        w = t.results.width;
        const size_t n = t.results.hits.size();
        ImGui::SeparatorText("Results");
        ImGui::TextDisabled("%zu result(s), %s", n, valTypeName(ty));

        if (ImGui::BeginTable("##results", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, 200)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Previous");
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin((int)n);
            while (clip.Step())
            {
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++)
                {
                    const ScanHit &h = t.results.hits[(size_t)i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(hex8(h.addr).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(formatValue(ty, h.last).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(formatValue(ty, h.prev).c_str());
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Watch")) toWatch.emplace_back(h.addr, h.last);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    // addWatch takes the lock itself, so it runs after the block above has released it.
    for (auto &p : toWatch) t.addWatch(p.first, ty, w, p.second);
}

static void drawWatch(MemTools &t, const MemHost &host)
{
    ImGui::SeparatorText("Peek / Poke");

    ImGui::SetNextItemWidth(100);
    ImGui::InputText("Address", &t.addrHex, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("##mtype", valTypeName((ValType)t.manualType)))
    {
        for (int i = 0; i < kValTypeCount; i++)
            if (ImGui::Selectable(valTypeName((ValType)i), t.manualType == i)) t.manualType = i;
        ImGui::EndCombo();
    }
    if (needsLength((ValType)t.manualType))
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("bytes", &t.manualLen, 0, 0);
        t.manualLen = std::clamp(t.manualLen, 1, 256);
    }
    ImGui::SameLine();
    if (ImGui::Button("Peek")) t.addManualWatch(host);

    ImGui::BeginDisabled(!host.connected || t.watchBusy);
    if (ImGui::Button("Refresh all")) t.peekAll(host);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh (0.5 s)", &t.autoRefresh);
    if (t.autoRefresh && host.connected && !t.busy && !t.watchBusy && ImGui::GetTime() - t.lastRefresh > 0.5)
    {
        t.lastRefresh = ImGui::GetTime();
        bool any;
        {
            std::lock_guard<std::mutex> lk(t.mu);
            any = !t.watch.empty();
        }
        if (any) t.peekAll(host);
    }

    int removeId = -1, pokeId = -1;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        if (t.watch.empty())
        {
            ImGui::TextDisabled("Nothing watched. Peek an address above, or press Watch on a scan result.");
            return;
        }
        if (ImGui::BeginTable("##watch", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Poke");
            ImGui::TableHeadersRow();
            for (WatchEntry &w : t.watch)
            {
                ImGui::PushID(w.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hex8(w.addr).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(valTypeName(w.type));
                ImGui::TableNextColumn();
                if (w.valid) ImGui::TextUnformatted(formatValue(w.type, w.value).c_str());
                else if (!w.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", w.error.c_str());
                else ImGui::TextDisabled("(not read yet)");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(130);
                const bool enter = ImGui::InputText("##p", &w.pokeText, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                ImGui::BeginDisabled(!host.connected);
                if (ImGui::Button("Poke") || (enter && host.connected)) pokeId = w.id;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("X")) removeId = w.id;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (pokeId >= 0) t.poke(host, pokeId);
    if (removeId >= 0)
    {
        std::lock_guard<std::mutex> lk(t.mu);
        t.watch.erase(std::remove_if(t.watch.begin(), t.watch.end(), [removeId](const WatchEntry &w) { return w.id == removeId; }),
                      t.watch.end());
    }
}

void drawMemTools(MemTools &t, const MemHost &host)
{
    drawSearch(t, host);
    drawResults(t);
    drawWatch(t, host);
}
