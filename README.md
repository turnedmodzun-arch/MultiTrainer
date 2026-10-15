# Xbox 360 Toolkit (C++ / Dear ImGui)

Cross-platform replacement for the XDevkit.dll-based XRPC/JRPC transport, with a per-game trainer
system driven by mod files. It talks to the console's debug service (XBDM, TCP port 730) directly
over a socket, so it builds on Windows, macOS and Linux.

```
src/net.h      socket helpers (Winsock / BSD)
src/xbdm.*     XbdmClient: connect, send a command line, getmem, setmem
src/xrpc.*     XrpcClient: port of XRPC.Call (buffer at the pointer stored at 0x91C088AE)
src/mod.*      mod file loader + action runner
src/main.cpp   ImGui + GLFW app (worker thread, so the UI never blocks)
mods/          sample mod files (copied next to the executable on build)
tests/         core_test: clients, loader and runner against an in-process mock console
```

## Build

Needs CMake 3.16+ and a C++17 compiler. ImGui 1.91.9, GLFW 3.4 and nlohmann/json 3.11.3 are
fetched automatically.

```
cmake -S . -B build
cmake --build build --config Release
./build/XboxToolkit          # Windows: build\Release\XboxToolkit.exe
```

Linux also needs the usual GLFW dependencies (e.g. `libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev libgl-dev`). Use `-DBUILD_GUI=OFF` to build only the library and tests.

## Using the app

1. Enter the console's IP and press Connect (XBDM and your XRPC plugin must be running).
2. **Games** tab: lists every `.json` file in the mods folder. Click one to open its trainer; use
   "< All games" to go back. "Rescan folder" re-reads the folder after you edit or add files.
   Controls are greyed out until you're connected.
3. **Console** tab: free-form `Cbuf_AddText` box. Opening a mod copies its address into this tab.
4. **Memory** tab: hex dump reads and hex writes. **Raw XBDM** tab: send any command line.
5. "Show raw protocol trace" prints every line sent and received.

The mods folder is `./mods` if it exists, otherwise `mods` next to the executable. You can edit the
path in the box on the Games tab.

## Mod file format

A mod file is JSON (`//` and `/* */` comments are allowed). See `mods/mw2_alpha.json` for a working
file and `mods/example_all_controls.json` as a template.

```json
{
  "game": "Modern Warfare 2 (Xbox 360 alpha)",
  "description": "Shown under the name in the game list.",
  "version": 1,
  "settings": { "cbuf_address": "0x8226F590", "client": 0, "append_newline": false },
  "controls": [ ... ]
}
```

`settings` is optional unless you use `cbuf` actions, which need `cbuf_address`. Addresses are hex
strings (`"0x82001234"`) or plain integers.

### Controls

| `type`      | Fields | Behaviour |
|-------------|--------|-----------|
| `label`     | `text` | Plain text. |
| `separator` | `label` (optional) | A divider, with a heading if `label` is set. |
| `button`    | `label`, `actions` or `cbuf` | Runs the actions when clicked. |
| `checkbox`  | `label`, `on` / `off` (action arrays) or `on_cbuf` / `off_cbuf`, `default` | Runs `on` when ticked and `off` when cleared. `default` only sets the starting state; nothing is sent at load. |
| `textbox`   | `label`, `hint`, `default`, `button` (button text), `actions` or `cbuf` | Runs the actions when the button is pressed or Enter is hit. The text replaces `{value}` in the actions. |

`cbuf` is shorthand for a single `{"cbuf": "..."}` action.

### Actions

Each action is an object with exactly one key. Actions in a list run in order, and a failure stops
the rest of that list.

| Action | Example | Effect |
|--------|---------|--------|
| `cbuf`   | `{"cbuf": "com_maxfps {value}"}` | `Cbuf_AddText(client, text)` through XRPC. |
| `poke`   | `{"poke": {"addr": "0x82001234", "bytes": "DE AD BE EF"}}` | Writes raw bytes. |
| `poke32` | `{"poke32": {"addr": "0x82001234", "value": "{value}"}}` | Writes a big-endian 32-bit integer (decimal or `0x` hex). |
| `pokef`  | `{"pokef": {"addr": "0x82001234", "value": 1.5}}` | Writes a big-endian float. |
| `call`   | `{"call": {"addr": "0x82ABCDEF", "args": [0, "text {value}", {"int": "{value}"}]}}` | Calls a function through XRPC. Integers are passed as integers, strings as pointers to NUL-terminated text, and `{"int": "..."}` as an integer taken from an expression (up to 9 arguments). |

A control that is invalid (unknown type, bad action, bad address) is skipped and listed as a
warning on the trainer screen; a file that is not valid JSON shows up in red in the game list.

## Windows and macOS builds at once

`.github/workflows/build.yml` builds both in parallel on GitHub's runners (Windows x64, and a macOS
universal binary that runs on Apple Silicon and Intel). Push the project to a GitHub repo, open the
**Actions** tab, run **build** (or just push), then download `XboxToolkit-windows-x64` and
`XboxToolkit-macos-universal` from the run's Artifacts. Each contains the app plus the `mods` folder.

Building by hand works the same on both systems:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release
    cmake --install build --config Release --prefix dist

The Windows build links the C++ runtime statically, so it needs no extra installs. The macOS binary is
unsigned: after downloading, run `xattr -dr com.apple.quarantine XboxToolkit` once (or right-click >
Open) to get past Gatekeeper.

## Trainer builder

The **Builder** tab creates and edits trainer files without hand-writing JSON.

- **New / Open existing / Save**: the file bar starts a fresh starter trainer, opens any trainer from
  the mods folder, and saves to `<mods folder>/<file name>.json`. A `*` marks unsaved changes.
  Overwriting an existing file and discarding unsaved changes both ask for confirmation.
- **Left list**: file settings (game name, `cbuf_address`) plus the controls. Add, Up, Down, Copy
  and Delete work on the selected control.
- **Right pane, Edit tab**: edit the selected control (label, hint, default, button text) and its
  actions (`cbuf`, `poke`, `poke32`, `pokef`, `call` with int/string args). Each action has a **Run**
  button that executes it on the console now, using the "test value" for `{value}`.
  A **Checks** list shows validation problems (missing address, bad hex, and so on).
- **Preview tab**: shows the trainer exactly as the Games screen will, and works live when connected.
- The Games screen has an **Edit in builder** button for the open trainer.

Saving writes canonical JSON, so comments and shorthand keys (`cbuf`, `on_cbuf`) in a hand-written file
are not preserved. `poke32`/`pokef` values are saved as strings so `{value}` templating keeps working.

## Verify the protocol

The XBDM framing in `xbdm.cpp` is written from memory, and the tests only prove the client matches
the mock. If anything misbehaves on a real console:

1. Run Wireshark filtered on `tcp.port == 730` while your existing C# tool does a `Cbuf_AddText` call.
2. Compare it with the app's raw trace (banner, `getmem` reply format, `setmem` reply, any
   debugger-connect step).
3. Fix `XbdmClient::getMemory` or add the connect step to match.

## Notes

- XRPC port: strings are written with a trailing NUL (XRPC.cs didn't), so a short string can't pick up
  leftover bytes from a longer earlier one. Float arguments are not ported yet.
- Checkbox state lives only in the app; it isn't read back from the console, so a checkbox can drift
  if you change the same setting another way.
- XBDM has no authentication. Keep the console on your home network.
