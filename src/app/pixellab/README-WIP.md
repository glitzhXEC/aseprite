# PixelLab native integration — experimental Windows / Linux build

**Not production-ready.** The feature is built into Aseprite, not a Lua
extension or a helper process. The development menu is under File. It can
list/search synthetic or PixelLab characters, open rotations and animations,
and has an experimental save command. The experimental panel shows rotation and
animation previews, with animation lists loaded automatically.
Do not use the save command on irreplaceable assets. The implementation has
been validated on Linux with real PixelLab reads and one rotation write;
it has not been tested on Windows.

The default build leaves the client disabled. Windows development build (x64
Native Tools Command Prompt for VS 2022; see the repository INSTALL.md for
the supported Windows SDK and Skia dependencies):

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DENABLE_PIXELLAB_NATIVE=ON -DLAF_BACKEND=skia ^
  -DSKIA_DIR=C:\deps\skia ^
  -DSKIA_LIBRARY_DIR=C:\deps\skia\out\Release-x64 ^
  -DSKIA_LIBRARY=C:\deps\skia\out\Release-x64\skia.lib
ninja -C build aseprite
```

Linux development build:

```sh
cmake -S . -B build -G Ninja -DENABLE_PIXELLAB_NATIVE=ON \
  -DUSE_SHARED_CURL=ON -DLAF_BACKEND=skia \
  -DSKIA_DIR=/path/to/skia \
  -DSKIA_LIBRARY_DIR=/path/to/skia/out/Release-x64 \
  -DSKIA_LIBRARY=/path/to/skia/out/Release-x64/libskia.a
ninja -C build aseprite
```

Follow Aseprite's `INSTALL.md` for the compiler and dependencies. This code is
based on fork main `3669f4774d181d77deb3e6db95fe6df8ca347410`, not the
v1.3.18.6 tag. Building the library requires libcurl on Linux and Windows uses WinHTTP.
The Windows application built successfully in GitHub Actions; the UI and live
PixelLab connection have not yet been tested on Windows.
There is no Python runtime dependency. Initial CMake configuration requires network
access to download pinned dependencies; each download is verified against SHA-256.

## Safety and limitations

* Connection uses `PIXELLAB_API_KEY` from the process environment; this is a
  development-only mechanism. Environment variables can be exposed to other
  processes and descendants. Do not use this on a shared machine. No key is
  intentionally written to files or logs. A secure masked UI/credential store
  is still required.
* Save targets are bound to character ID, animation group ID, and original cel
  direction markers. Only directions with pixel or duration changes are sent.
  Missing markers or changed frame count block saving. Originals are held in
  memory only; close/reopen the document or finish a save attempt, then re-open
  from PixelLab for a new snapshot. No animation undo is available.
* Save preflight refreshes the cloud pixels and refuses the whole batch if
  they differ from the opened snapshot. This is not an atomic conditional
  write: another writer can still modify cloud data between checking and
  writing. Animation timing and other remote metadata are not compared. An
  error/cancel can have an uncertain outcome; check PixelLab before retrying.
* GIF transport supports at most 255 opaque colors, binary transparency, and
  one uniform frame duration per direction. Partial transparency and unequal
  or non-10ms durations are refused. Imported animation timing currently
  defaults to 100 ms per frame and may not match PixelLab. Frame insertion/deletion or replacing
  imported cels is currently refused; the safe behavior is to reopen.
* ZIP export fallback uses a name/direction/frame-count matching heuristic;
  unique does not guarantee correct identity. Inspect a duplicate asset on
  PixelLab before editing. Saving an animation opened through the heuristic
  is blocked until a reliable mapping is implemented.
* Skeleton references from PixelLab are hidden by default and must remain
  hidden during save. Paid estimation is optional and asks for confirmation
  before opening; it may cost 0.1 generation per missing frame.
* The panel is modal and closes when an asset opens. Reopen it to use its save
  button, or use the File menu save command. Disconnect, in-app key entry,
  secure credentials, persistent snapshots and safe UI undo are not implemented.
  The animation preview uses a fixed 100ms per frame; All directions previews
  the first available direction. Preview and UI require further testing.

## Synthetic-only test build

Set `-DPIXELLAB_FAKE_NETWORK_FOR_TESTS=ON` as well. The panel is marked
SIMULATION ONLY and takes a hardcoded synthetic credential; it never sends a
request to PixelLab. Generate a fixture using `tests/make_fixture.py` and set
`PIXELLAB_FIXTURE` and `PIXELLAB_CAPTURE` to disposable paths before running.
Development tests require Python/Pillow; the built application does not.

The client also has standalone synthetic tests in `tests/README.txt`. HTTPS is
restricted to TLS with certificate validation and no redirects, and the
Authorization header is restricted to api.pixellab.ai:443. See included license files for nlohmann/json, miniz, and stb.

## Verified locally (development)

* Linux build compiled and started under a virtual X11 display.
* In-app tests with synthetic network data: rotations, two animation
  directions, GIF/PNG destinations, no-change and changed-cloud preflight,
  unverified ZIP fallback blocked, and empty search result UI.
* Linux native client authenticated a real account, listed 35 characters, read
  rotations for 5 characters, and opened a 3-direction animation. One pixel in
  one south rotation was written and the complete image was verified by reload.
  This was a client test, not an in-app GUI test; no real animation write was tested.
* The Windows WinHTTP client can be cross-compiled on Linux; no Windows runtime
  or full Aseprite/MSVC application test has been completed.
