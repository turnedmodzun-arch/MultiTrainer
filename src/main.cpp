// Cross-platform Xbox 360 tool: Dear ImGui + GLFW front end over XbdmClient/XrpcClient.
// All console I/O runs on a worker thread so the UI never blocks.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "builder.h"
#include "mod.h"
#include "xbdm.h"
#include "xrpc.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

// ------------------------------------------------------------------ worker thread
class Worker
{
public:
    Worker() : th_([this] { run(); }) {}
    ~Worker()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    void post(std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(f));
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        for (;;)
        {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                f = std::move(q_.front());
                q_.pop_front();
            }
            f();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    std::thread th_;  // declared last so the other members exist before it starts
};

// ------------------------------------------------------------------ app state
struct App
{
    XbdmClient client;
    XrpcClient xrpc{client};

    std::atomic<bool> connected{false};
    std::atomic<int> pending{0};
    std::atomic<bool> traceRaw{false};

    std::mutex logMu;
    std::deque<std::string> logLines;
    std::string memDump;  // guarded by logMu

    // UI-thread-only state
    char ip[64] = "192.168.1.50";
    char modsFolder[512] = "mods";
    std::vector<ModFile> mods;
    int activeMod = -1;  // index into mods, or -1 for the game list

    // Settings used by the generic Console tab (seeded from a mod when one is opened).
    char cbufAddr[16] = "8226F590";
    int clientNum = 0;
    bool appendNewline = false;

    Builder builder;            // state of the Builder tab
    bool focusBuilder = false;  // switch to the Builder tab on the next frame

    // Declared last so it is destroyed first: its destructor finishes queued
    // tasks, which still use everything above.
    Worker worker;

    void log(const std::string &s)
    {
        std::lock_guard<std::mutex> lk(logMu);
        logLines.push_back(s);
        if (logLines.size() > 2000) logLines.pop_front();
    }

    // Runs f on the worker, tracking "busy" for the UI.
    void run(std::function<void()> f)
    {
        pending++;
        worker.post([this, f] {
            f();
            connected = client.isConnected();
            pending--;
        });
    }

    void rescan()
    {
        mods = scanMods(modsFolder);
        activeMod = -1;
        log("Found " + std::to_string(mods.size()) + " mod file(s) in " + modsFolder);
    }

    App()
    {
        client.onTrace = [this](const std::string &s) {
            if (traceRaw) log("[xbdm] " + s);
        };
    }
};

static uint32_t parseHex(const char *s)
{
    return (uint32_t)std::strtoul(s, nullptr, 16);  // strtoul accepts a leading 0x
}

static std::string hexDump(uint32_t base, const std::vector<uint8_t> &d)
{
    std::string out;
    char line[128];
    for (size_t i = 0; i < d.size(); i += 16)
    {
        int n = std::snprintf(line, sizeof(line), "%08X  ", (uint32_t)(base + i));
        for (size_t j = 0; j < 16; j++)
        {
            if (i + j < d.size()) n += std::snprintf(line + n, sizeof(line) - n, "%02X ", d[i + j]);
            else n += std::snprintf(line + n, sizeof(line) - n, "   ");
        }
        n += std::snprintf(line + n, sizeof(line) - n, " ");
        for (size_t j = 0; j < 16 && i + j < d.size(); j++)
        {
            const uint8_t c = d[i + j];
            line[n++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[n] = 0;
        out += line;
        out += '\n';
    }
    return out;
}

// Queues a mod control's actions on the worker thread.
static void runMod(App &app, const ModSettings &settings, std::vector<ModAction> actions, std::string value)
{
    app.run([&app, settings, actions = std::move(actions), value = std::move(value)] {
        runActions(app.client, app.xrpc, settings, actions, value, [&app](const std::string &m) { app.log(m); });
    });
}

// ------------------------------------------------------------------ Games tab
static void drawGameList(App &app)
{
    ImGui::SetNextItemWidth(-150);
    ImGui::InputText("##folder", app.modsFolder, sizeof(app.modsFolder));
    ImGui::SameLine();
    if (ImGui::Button("Rescan folder")) app.rescan();
    ImGui::Spacing();

    if (app.mods.empty())
    {
        ImGui::TextWrapped("No mod files found in \"%s\". Put .json mod files in that folder and press Rescan folder.",
                           app.modsFolder);
        return;
    }

    ImGui::BeginChild("##gamelist", ImVec2(0, 0), false);
    for (size_t i = 0; i < app.mods.size(); i++)
    {
        ModFile &m = app.mods[i];
        ImGui::PushID((int)i);
        const std::string fname = m.path.filename().string();

        if (m.ok())
        {
            if (ImGui::Button(m.game.c_str(), ImVec2(-1, 34)))
            {
                app.activeMod = (int)i;
                // Seed the Console tab from this mod so both agree on the Cbuf address.
                if (m.settings.cbufAddr)
                    std::snprintf(app.cbufAddr, sizeof(app.cbufAddr), "%08X", m.settings.cbufAddr);
                app.clientNum = m.settings.client;
                app.appendNewline = m.settings.appendNewline;
                app.log("Loaded mod: " + m.game + " (" + fname + ")");
            }
            std::string sub = fname + "  -  " + std::to_string(m.controls.size()) + " controls";
            if (!m.warnings.empty()) sub += "  -  " + std::to_string(m.warnings.size()) + " warning(s)";
            if (!m.description.empty()) sub += "  -  " + m.description;
            ImGui::TextDisabled("%s", sub.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s  (cannot load)", fname.c_str());
            ImGui::TextWrapped("    %s", m.error.c_str());
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void drawTrainer(App &app, bool isConnected)
{
    ModFile &mod = app.mods[(size_t)app.activeMod];

    if (ImGui::Button("< All games"))
    {
        app.activeMod = -1;
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Edit in builder"))
    {
        builderOpen(app.builder, mod);
        app.focusBuilder = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(mod.game.c_str());
    if (!mod.description.empty()) ImGui::TextDisabled("%s", mod.description.c_str());
    for (const std::string &w : mod.warnings)
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "warning: %s", w.c_str());
    if (!isConnected) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Connect to the console to use these controls.");
    ImGui::Separator();

    ImGui::BeginChild("##controls", ImVec2(0, 0), false);
    drawModControls(mod, isConnected, [&app](const ModSettings &s, std::vector<ModAction> a, std::string v) {
        runMod(app, s, std::move(a), std::move(v));
    });
    ImGui::EndChild();
}

// ------------------------------------------------------------------ UI
static void drawUI(App &app)
{
    static int memLen = 64;
    static char cmdBuf[256] = "";
    static char memAddr[16] = "82000000";
    static char writeHex[512] = "";
    static char rawCmd[400] = "";

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- connection bar
    const bool isConnected = app.connected;
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("Console IP", app.ip, sizeof(app.ip));
    ImGui::SameLine();
    if (!isConnected)
    {
        if (ImGui::Button("Connect"))
        {
            const std::string host = app.ip;
            app.run([&app, host] {
                std::string err;
                app.log("Connecting to " + host + ":730 ...");
                if (app.client.connect(host, err))
                {
                    app.xrpc.reset();
                    app.log("Connected.");
                }
                else app.log("Connect failed: " + err);
            });
        }
    }
    else if (ImGui::Button("Disconnect"))
    {
        app.run([&app] {
            app.client.disconnect();
            app.log("Disconnected.");
        });
    }
    ImGui::SameLine();
    ImGui::TextColored(isConnected ? ImVec4(0.3f, 1, 0.3f, 1) : ImVec4(1, 0.4f, 0.4f, 1), "%s",
                       isConnected ? "connected" : "not connected");
    if (app.pending > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(working...)");
    }
    ImGui::Separator();

    const float logHeight = 190.0f;
    ImGui::BeginChild("##tabs", ImVec2(0, ImGui::GetContentRegionAvail().y - logHeight - 8), false);
    if (ImGui::BeginTabBar("##tabbar"))
    {
        // ---- games / trainers
        if (ImGui::BeginTabItem("Games"))
        {
            if (app.activeMod < 0 || app.activeMod >= (int)app.mods.size()) drawGameList(app);
            else drawTrainer(app, isConnected);
            ImGui::EndTabItem();
        }

        // ---- trainer builder
        const ImGuiTabItemFlags builderFlags = app.focusBuilder ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        app.focusBuilder = false;
        if (ImGui::BeginTabItem("Builder", nullptr, builderFlags))
        {
            BuilderHost host;
            host.connected = isConnected;
            host.folder = app.modsFolder;
            host.mods = &app.mods;
            host.run = [&app](const ModSettings &s, std::vector<ModAction> a, std::string v) {
                runMod(app, s, std::move(a), std::move(v));
            };
            drawBuilder(app.builder, host);
            ImGui::EndTabItem();
        }

        // ---- generic console
        if (ImGui::BeginTabItem("Console"))
        {
            ImGui::SetNextItemWidth(110);
            ImGui::InputText("Cbuf_AddText addr", app.cbufAddr, sizeof(app.cbufAddr),
                             ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::InputInt("client", &app.clientNum, 0, 0);
            ImGui::SameLine();
            ImGui::Checkbox("append \\n", &app.appendNewline);

            ImGui::Spacing();
            ImGui::SetNextItemWidth(-90);
            const bool enter = ImGui::InputText("##cmd", cmdBuf, sizeof(cmdBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Send") || enter) && isConnected && cmdBuf[0])
            {
                ModSettings s;
                s.cbufAddr = parseHex(app.cbufAddr);
                s.client = app.clientNum;
                s.appendNewline = app.appendNewline;
                ModAction a;
                a.kind = ModAction::Cbuf;
                a.text = cmdBuf;
                runMod(app, s, {a}, "");
                cmdBuf[0] = 0;
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::EndTabItem();
        }

        // ---- memory
        if (ImGui::BeginTabItem("Memory"))
        {
            ImGui::SetNextItemWidth(110);
            ImGui::InputText("Address (hex)", memAddr, sizeof(memAddr), ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Length", &memLen, 0, 0);
            if (memLen < 1) memLen = 1;
            if (memLen > 0x4000) memLen = 0x4000;
            ImGui::SameLine();
            if (ImGui::Button("Read") && isConnected)
            {
                const uint32_t a = parseHex(memAddr);
                const uint32_t n = (uint32_t)memLen;
                app.run([&app, a, n] {
                    std::vector<uint8_t> d;
                    std::string err;
                    if (app.client.getMemory(a, n, d, err))
                    {
                        std::lock_guard<std::mutex> lk(app.logMu);
                        app.memDump = hexDump(a, d);
                    }
                    else app.log("read failed: " + err);
                });
            }

            ImGui::SetNextItemWidth(-90);
            ImGui::InputText("##wr", writeHex, sizeof(writeHex));
            ImGui::SameLine();
            if (ImGui::Button("Write hex") && isConnected)
            {
                std::vector<uint8_t> bytes;
                std::istringstream ss(writeHex);
                std::string tok;
                bool bad = false;
                while (ss >> tok)
                {
                    if (tok.size() % 2 != 0) { bad = true; break; }  // each token must be whole bytes
                    for (size_t i = 0; i < tok.size(); i += 2)
                    {
                        char *end = nullptr;
                        const std::string pair = tok.substr(i, 2);
                        const unsigned long v = std::strtoul(pair.c_str(), &end, 16);
                        if (end != pair.c_str() + 2) { bad = true; break; }
                        bytes.push_back((uint8_t)v);
                    }
                    if (bad) break;
                }
                if (bad || bytes.empty()) app.log("write: enter hex bytes like \"DE AD BE EF\"");
                else
                {
                    const uint32_t a = parseHex(memAddr);
                    app.run([&app, a, bytes] {
                        std::string err;
                        if (app.client.setMemory(a, bytes.data(), bytes.size(), err))
                            app.log("wrote " + std::to_string(bytes.size()) + " bytes");
                        else app.log("write failed: " + err);
                    });
                }
            }

            ImGui::BeginChild("##dump", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            std::string copy;
            {
                std::lock_guard<std::mutex> lk(app.logMu);
                copy = app.memDump;
            }
            ImGui::TextUnformatted(copy.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ---- raw
        if (ImGui::BeginTabItem("Raw XBDM"))
        {
            ImGui::TextWrapped("Send any XBDM command line and see the reply in the log. Useful for checking "
                               "the protocol, e.g.  getmem addr=0x82000000 length=0x10");
            ImGui::SetNextItemWidth(-90);
            const bool enter = ImGui::InputText("##raw", rawCmd, sizeof(rawCmd), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Send##raw") || enter) && isConnected && rawCmd[0])
            {
                const std::string c = rawCmd;
                app.run([&app, c] {
                    XbdmReply rep;
                    std::string err;
                    if (app.client.send(c, rep, err))
                    {
                        app.log("<< " + rep.status);
                        for (size_t i = 0; i < rep.lines.size() && i < 40; i++) app.log("   " + rep.lines[i]);
                        if (rep.lines.size() > 40) app.log("   ... (" + std::to_string(rep.lines.size()) + " lines)");
                    }
                    else app.log("raw send failed: " + err);
                });
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // ---- log
    bool trace = app.traceRaw;
    if (ImGui::Checkbox("Show raw protocol trace", &trace)) app.traceRaw = trace;
    ImGui::SameLine();
    if (ImGui::Button("Clear log"))
    {
        std::lock_guard<std::mutex> lk(app.logMu);
        app.logLines.clear();
    }
    ImGui::BeginChild("##log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(app.logMu);
        for (const std::string &l : app.logLines) ImGui::TextUnformatted(l.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// ------------------------------------------------------------------ main
static void glfwError(int code, const char *desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

// Prefers ./mods, then a "mods" folder next to the executable.
static std::string defaultModsFolder(const char *argv0)
{
    std::error_code ec;
    if (fs::is_directory("mods", ec)) return fs::absolute("mods", ec).string();
    if (argv0 && *argv0)
    {
        const fs::path beside = fs::absolute(argv0, ec).parent_path() / "mods";
        if (fs::is_directory(beside, ec)) return beside.string();
    }
    return fs::absolute("mods", ec).string();
}

int main(int, char **argv)
{
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char *glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char *glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow *window = glfwCreateWindow(980, 680, "Xbox 360 Toolkit", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    {
        App app;
        std::snprintf(app.modsFolder, sizeof(app.modsFolder), "%s", defaultModsFolder(argv ? argv[0] : nullptr).c_str());
        app.log("Ready. Enter your console's IP and press Connect, then pick a game on the Games tab.");
        app.rescan();

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            drawUI(app);
            if (app.builder.needsRescan)
            {
                app.builder.needsRescan = false;
                app.rescan();
            }

            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
        // App's destructor stops the worker (finishing any queued calls) before UI teardown.
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
