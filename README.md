# aoahid-player

[![ci](https://github.com/paasann210/aoahid_player/actions/workflows/ci.yml/badge.svg)](https://github.com/paasann210/aoahid_player/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Plays scripted touch, mouse, keyboard, gamepad, and pen input to Android
devices over USB with AOA 2.0 HID, and records real touches and key presses
into the same script format with `adb`. Linux and Windows, C++20.

There are three programs, all built on one shared engine:

- `aoahid_player_gui` — a desktop app for connecting, playing, seeking, and
  recording.
- `aoa_touch` — the command-line player.
- `aoa_record` — the command-line recorder.

All HID, AOA, and USB work is done by
[libaoahid](https://github.com/paasann210/Libaoa_hid); see that repository for
the protocol, descriptor, and transport details.

## Download

Each [release](https://github.com/paasann210/aoahid_player/releases) has one
archive per platform with everything in a single folder:

```
aoahid-player-<version>-<platform>-x86_64/
  aoahid_player_gui[.exe]   aoa_touch[.exe]   aoa_record[.exe]
  libaoahid + libusb runtime (.so / .dll)
  MSVC runtime DLLs (Windows only)
  csv/            scripts; recordings are saved here
  third-party/    licences of everything bundled
  README.md  LICENSE
```

Keep the folder together: the programs find the libraries and `csv/` next to
themselves. On Linux, install the udev rule first (see
[Linux permissions](#linux-permissions-udev)). Recording needs `adb`
(Android SDK Platform-Tools) on PATH; playback does not.

## The GUI

Start `aoahid_player_gui`. The profile settings and the adb option are saved
to `aoahid_player_gui.ini` next to the program whenever you finish changing
them, and restored at the next start; delete the file to go back to the
defaults. If the folder is read-only, changes last until the app closes.

**Left side — connection**

1. *Devices* lists AOA-capable phones; the refresh button searches again. Tick
   one or more (a single phone is ticked for you).
2. *Profiles* chooses which HID devices to present: touchscreen, mouse,
   keyboard, gamepad, pen. Each one's settings open when it is switched on.
   *Detect* reads the touchscreen resolution with `adb shell wm size` (an
   override size wins, because that is what touches map to).
3. *Connect*. By default the adb server is stopped first, because it can hold
   the phone's USB interface and block the accessory handshake; recording
   keeps it running.

While connected, the profile choices are locked; disconnect to change them.
Each connected phone shows a status dot and its report count, and a phone
that stops responding is dropped without stopping playback for the others.

**Player tab**

- The *Script* box shows the loaded script. Click it (or press **Ctrl+F**) to
  open the script list: type to search (words match anywhere in the name,
  case-insensitively), move with **Up/Down**, open with **Enter** or a click,
  close with **Esc**. Each row shows the file size and when it was last
  changed. The trash button deletes a file after an inline confirmation.
  Files outside `csv/` open by path from the bottom of the list, or by
  dropping them on the window. The list rescans whenever the window gets
  focus.
- Play/pause (also **Space**), stop, and back-to-start buttons. The *Intro*
  bar is the uppercase rows that run once; the *Loop* bar is the lowercase
  rows that repeat. Click or drag either bar to seek, while playing, paused,
  or stopped.
- *Speed* is typed in directly (0.01×–1000×, with 0.5× / 1× / 2× / 4×
  buttons) and *Loops* 0 repeats until stopped; both apply immediately, even
  in the middle of a wait. *Offset* shifts every later event by ±1 or ±10 ms;
  it starts at zero on every run and can only be changed while playing.
- Pause and stop release everything the script was holding. A seek rebuilds
  exactly what would be held at the new position (fingers down, keys,
  buttons, axes), so playback continues as if it had never jumped.

**Live tab**

A black preview with the phone's shape, for using the phone from the computer
and watching what the scripts do. Turn on *Live control*, then choose what to
forward: *Touch* (drag inside the preview), *Mouse* (motion, buttons, and the
wheel), *Keyboard*, and *Gamepad* (any controller GLFW recognises). Only
profiles the connection actually has can be turned on, and touch and mouse
share the pointer, so turning one on turns the other off. The panel beside the
preview lists what is held and what was sent; contacts appear on the preview
with their coordinates, held keys along its bottom. *Rotate* turns the
preview a quarter turn at a time for landscape use, and the two numbers
beside it set its shape as width:height; both default to the connected
touchscreen, and *Reset* returns to it. Neither changes where a touch
lands: the pointer's position inside the preview is taken as a fraction,
turned back into the phone's own orientation, and only then scaled to the
connected resolution. The expand button fills the window with just the
preview and the input switches; *Exit full screen* or **Esc** (when keys
are not being forwarded) returns. The preview shows why Live control is
unavailable right now (no device connected, or a script loaded and playing)
under itself, in both the normal and the full-screen view, instead of just a
disabled toggle. Live control stays off while a script is playing — playing
a script and forwarding Live input are two uses of the same connection that
cannot run together — and comes back on its own once the player stops.

Mouse mode sends relative motion, so clicking inside the preview captures
the pointer: the cursor disappears and stops being bounded by the screen
edge, the same way a first-person game grabs the mouse, which keeps motion
flowing however far and fast it moves. It also captures the wheel, so
scrolling never leaks into the surrounding window while the pointer is
grabbed. While captured, the pointer belongs to the preview alone: it never
drifts onto — and click — some other button, tab, or setting in this window,
however far or long it moves; the window's own controls simply see no mouse
until the capture is released. **Esc** releases the capture by default
without turning off Live control or the Mouse toggle, and is still forwarded
to the phone like any other key; clicking the preview again re-captures it.
*Mouse release key*, below the preview, lets a different key take over
instead — useful when Esc itself needs to reach the phone (a game that quits
on Esc, for instance). Once changed, only that exact key releases the
capture; every other key, Esc included, goes straight to the phone like
normal.

Keyboard forwarding recognises the extra keys a JIS (Japanese) keyboard has
that a US layout does not — Henkan, Muhenkan, Kana, Zenkaku/Hankaku, and Ro —
on Linux, by their physical scancode rather than by name: GLFW has no
`GLFW_KEY_*` constant for any of them on any platform, so without this they
would never reach the phone at all. This was a gap in this app talking to
GLFW, not a limitation of the phone or Android — once a usage reaches the
connection it is forwarded like any other key. The connection's *Usages*
range (in the Keyboard profile settings) has to include 87-94 for these to
actually reach the phone; raise its upper end from the default 65 if you
need them. ISO keyboards (the extra key beside left Shift, common outside
the US) are already covered by the default range on every platform. Windows
uses a different scancode numbering than Linux, so this recognition is
Linux-only for now; on Windows these five keys still do not reach the phone.

*Reference image* loads a picture (a screenshot works well) over the
preview, by path or by dropping the file on the preview while this tab is
open. While unlocked, drag inside it to move, its corner handle to resize,
and its top handle to rotate; *Lock image* freezes it in place and lets
touches and clicks pass straight through to the phone. It is not saved
between runs.

**Playlist tab**

Plays several scripts in order, each for its own number of loops, with an
optional limit in minutes on the whole run. Playlists are saved as
`playlists/<name>.playlist` next to the program and reopened from *Open*.

**Recorder tab**

Choose the phone (automatic works when adb sees only one) and optionally limit
it to one `/dev/input/eventN`. Type the *File name* before you start (`.csv` is
added; blank gives `record-<date>-<time>`). Names with characters Windows or
Linux cannot store are refused, and a name that already exists turns the
button into *Replace and record*. *Stop and save* writes the file into `csv/`,
and *Open in player* loads it.

The window only redraws when something changes, so an idle window uses no
CPU. It follows the monitor's scale factor on Windows and X11 and the
compositor's scale on Wayland.

## aoa_touch — usage

```
aoa_touch [options] [script.csv]

Device selection:
  --devices <list>        Comma-separated device numbers from the discovery
                           list (e.g. "1,3"), or "all". If omitted, an
                           interactive numbered list is shown.

Profile setup (all explicit; there are no presets):
  --touch-res WxH          Touch surface resolution, e.g. 1080x1920
  --touch-max-contacts N   Max simultaneous touch contacts (1-16)
  --gamepad-buttons N      Number of gamepad buttons
  --gamepad-axes LIST      Comma-separated axis roles, e.g. x,y,rx,ry
                           (x y z rx ry rz slider dial wheel rudder
                            throttle accelerator brake steering;
                            x and y are required)
  --gamepad-axis-bits N    Bit width per axis, 2-32
  --gamepad-dpad MODE      none|hat|buttons
  --key-usage-range LO,HI  Keyboard HID usage range (default 0x04,0x65)
  --mouse-buttons N        Number of mouse buttons
  --pen-mode MODE          direct|indirect

Playback:
  -A                       Auto-detect touch resolution via `adb shell wm size`
  --speed FACTOR           Playback speed multiplier (default 1.0)
  --loop N                 Stop after N loop iterations (default: infinite)
  --no-prompt              Do not start the live "-> " offset prompt
  -h, --help               Show this text
```

With no script path, the `csv/` folder next to the executable is listed for
interactive selection. A script given on the command line is checked before
any USB work, so a mistake is reported with its file and line straight away.

While playing, type a number of milliseconds at the `->` prompt to shift every
later event (negative is earlier). Ctrl+C stops playback and releases
everything that was held before the devices are closed.

## aoa_record — usage

```
aoa_record [options]

  -o, --output NAME   Recording name or path. A bare name is saved in the
                       csv/ folder next to this program (".csv" is added
                       when missing). Default: record-<date>-<time>
  -s, --serial ID     adb device serial, for `adb -s ID`
      --input PATH    Only record /dev/input/eventN; default is every
                       device getevent reports
      --echo          Print each captured row while recording
  -h, --help          Show this text
```

Runs `adb shell getevent -lt`, turns touch (`ABS_MT_*`) and key (`EV_KEY`)
frames into lowercase `t` and `k` rows with measured `wait_ms`, and streams
them to the file. The touch panel's own coordinate range is read with
`getevent -lp` and written as a `# screen WxH` comment, so playback can scale
the recording to whatever touchscreen resolution is connected. Ctrl+C stops; the file is flushed and closed first. A
recording that captured nothing leaves no file behind.

## CSV script format

Plain text, no header row, one event per line; `#` starts a comment.

| prefix | profile        | columns                                     |
|--------|----------------|---------------------------------------------|
| `t`    | touch          | `finger_id,state,x,y,wait_ms`               |
| `m`    | mouse move     | `dx,dy,wait_ms`                             |
| `b`    | mouse button   | `button_no,pressed,wait_ms`                 |
| `k`    | keyboard key   | `usage,down,wait_ms` (usage accepts `0x..`) |
| `g`    | gamepad button | `button_no,pressed,wait_ms`                 |
| `a`    | gamepad axis   | `axis_index,value,wait_ms`                  |
| `h`    | gamepad dpad   | `up,down,right,left,wait_ms`                |
| `p`    | pen            | `in_range,tip,x,y,pressure,wait_ms`         |

**Case controls when a row runs:**
- Uppercase prefix (`T,M,B,K,G,A,H,P`) — the intro, run once before the
  first loop iteration.
- Lowercase prefix (`t,m,b,k,g,a,h,p`) — the loop, repeated.

`state`, `down`, `pressed`, `in_range`, `tip`, and the four dpad directions
are `0` or `1`. `axis_index` is the position of the axis in the gamepad axis
list (0-based). A `# screen WxH` comment before the rows names the coordinate
space the touch and pen rows were written in; playback scales them from it
onto the connected surfaces. `wait_ms` is the delay after the row; rows with `0` are sent
in the same report as the next row. Every row runs at an absolute time
measured from the start of its segment, so a slow USB transfer never makes the
script drift. Keyboard modifiers (usages `0xE0`–`0xE7`) are always available,
whatever the usage range. See `csv/example.csv`.

## Linux permissions (udev)

On Linux, raw USB devices are root-only by default, so the programs cannot
open the phone even when `adb` can already see it. Install the rule shipped in
this repository, then unplug and replug the phone:

```sh
sudo cp udev/51-aoahid.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

See the comments in [`udev/51-aoahid.rules`](udev/51-aoahid.rules) for the
group your user needs to be in and for adding your phone's pre-switch vendor
ID (the rule as shipped covers the post-AOA-switch Google accessory vendor
ID, `18d1`).

## Building from source

### Requirements

- CMake 3.21+ and a C++20 compiler (GCC or Clang on Linux, MSVC on Windows)
- [libaoahid](https://github.com/paasann210/Libaoa_hid) (see below)
- For the GUI on Linux, the X11 and Wayland development headers GLFW builds
  against, for example on Debian/Ubuntu:
  `libwayland-dev libxkbcommon-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev pkg-config`
  (Arch/CachyOS: `wayland libxkbcommon libx11 libxrandr libxinerama libxcursor libxi libxext`).
  At run time GLFW loads X11 or Wayland and OpenGL itself, so the GUI runs
  on either.
- Network access at configure time for the GUI: Dear ImGui 1.92.9 and GLFW
  3.5.1 are downloaded at pinned, SHA-256-checked releases and linked
  statically. To build offline, pass
  `-DFETCHCONTENT_SOURCE_DIR_IMGUI=<dir> -DFETCHCONTENT_SOURCE_DIR_GLFW=<dir>`
  pointing at local copies of those releases, or `-DAOAHID_PLAYER_BUILD_GUI=OFF`.

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/libaoahid
cmake --build build --config Release
```

Every program lands in `build/out/<config>/`, next to a copy of `csv/` (and,
on Windows, the libaoahid DLLs), so it runs straight from there. To lay out
the same folder the release archive contains:

```sh
cmake --install build --config Release --component runtime --prefix aoahid-player
```

Options: `AOAHID_PLAYER_BUILD_GUI` (default `ON`) and
`AOAHID_PLAYER_BUNDLE_RUNTIME` (default `ON`: install libaoahid, libusb and,
with MSVC, the C++ runtime next to the programs).

### Getting libaoahid

`find_package(aoahid CONFIG REQUIRED)` fails immediately if CMake cannot
find libaoahid. It is not packaged by distributions or vcpkg/conan, so get it
one of these two ways.

**Option A — prebuilt release (fastest, no libusb build needed):** download
the shared-library archive for your platform from
[libaoahid's releases](https://github.com/paasann210/Libaoa_hid/releases)
(the same asset the workflows here use), extract it, and point
`CMAKE_PREFIX_PATH` at the extracted directory:

```sh
curl -LO https://github.com/paasann210/Libaoa_hid/releases/latest/download/libaoahid-<version>-linux-x86_64-ubuntu22.04-shared.tar.gz
tar -xzf libaoahid-<version>-linux-x86_64-ubuntu22.04-shared.tar.gz
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/libaoahid-<version>-linux-x86_64-ubuntu22.04-shared"
```

**Option B — build libaoahid from source:**

```sh
git clone https://github.com/paasann210/Libaoa_hid.git
cmake -S Libaoa_hid -B Libaoa_hid/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/libaoahid-staging"
cmake --build Libaoa_hid/build --config Release
cmake --install Libaoa_hid/build
```

libaoahid requires libusb 1.0.30 or newer (`LIBUSB_API_VERSION >=
0x0100010C`). Many distributions ship older versions (Ubuntu 24.04 has 1.0.27),
so building from source there also means building libusb 1.0.30 first and
pointing `PKG_CONFIG_PATH` at it. The prebuilt archives already bundle a
matching libusb runtime.

### Testing

```sh
ctest --test-dir build --output-on-failure -C Release
```

Unit tests cover CSV parsing, the script timeline and coordinate scaling,
`adb shell getevent` parsing, HID descriptor bit widths, and the input-state
transitions used for stop, pause, and seek. They use [doctest](https://github.com/doctest/doctest),
vendored under `tests/third_party/`. They are built only when this is the
top-level project; pass `-DBUILD_TESTING=OFF` to skip them.

## Using `aoahid_player_core` as a library

The engine behind all three programs is a static library with no console
I/O: status and errors reach the caller through an `EventSink`. The
`development` install component exports it as a CMake package:

```sh
cmake --install build --config Release --component development --prefix /path/to/staging
```

```cmake
find_package(aoahid_player_core CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE aoahid_player::aoahid_player_core)
```

The main headers in `include/aoahid_player/`:

- `session.hpp` — device discovery, connect and disconnect with a
  `ProfileSetup`.
- `player.hpp` — playback with pause, seek, speed, loop limit, live offset,
  and a wall-clock stop time, all callable from any thread; `status()` is
  lock-free, and a `PlaybackObserver` sees every row sent.
- `recorder.hpp` — `adb getevent` recording into a CSV file.
- `event_script.hpp` — CSV loading and the script timeline.
- `adb.hpp`, `paths.hpp`, `events.hpp` — adb helpers, UTF-8 paths and the
  `csv/` folder, and the `EventSink` interface.

`CMAKE_PREFIX_PATH` needs both this package's prefix and libaoahid's.

## Known limitations

- Recording captures touches and keyboard keys only, not mouse, gamepad, or
  pen input.
- Recording tracks the multi-touch Type B protocol (`ABS_MT_SLOT` plus
  `ABS_MT_TRACKING_ID`); the older Type A `SYN_MT_REPORT` form is not parsed.
- Only keys on the HID Keyboard/Keypad page are recorded. Consumer-page keys
  such as `KEY_VOLUMEUP` are counted and reported, not written.
- libaoahid's toggle, battery, and raw profiles are not exposed.

## License

MIT; see [LICENSE](LICENSE). The release archives also contain, each under
its own licence in `third-party/`:

- [libaoahid](https://github.com/paasann210/Libaoa_hid) (MIT) and libusb-1.0
  (LGPL-2.1-or-later, with its corresponding source), as shipped by libaoahid
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) and
  [GLFW](https://www.glfw.org/) (zlib), linked into the GUI
- the Roboto font (Apache-2.0), embedded in the GUI
- [stb_image](https://github.com/nothings/stb) (MIT/public domain), vendored
  for the Live tab's reference image
- on Windows, the Microsoft Visual C++ runtime DLLs, redistributed under
  Microsoft's redistribution terms

The test suite vendors doctest (MIT, `tests/third_party/LICENSE-doctest.txt`);
it is not linked into any program.
