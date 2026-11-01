# MPCSH / MPCECC 1.7 — tag `shell17`

## Highlights

### Multi-window PixelWindows
- Registry of `MpcEccWlid` entries (`wlid`).
- Commands: `windowlaunch`, `windowterminate`, `wlidlist`, focus via control socket.
- Multiple GUI apps (Krita, Firefox, …) can run as overlays in the **same** notebook tab.
- F3 star-map picker groups same-command windows.

### Window cycling without WM theft
- **Ctrl+Shift+Tab** (forward) / **Ctrl+Shift+`** or ISO_Left_Tab (reverse).
- Requires both Control and Shift so compositors and DEs that grab Alt+Tab or plain Ctrl+Tab never intercept the cycle.

### Scaling so Krita is not cut off
- Default Xvfb framebuffer: **1440×900** (was 1280×720 / help text 960×600).
- In-tab frame size caps raised to **1920×1200** (was hard-capped ~1280×820).
- `GTK_CONTENT_FIT_CONTAIN` kept — letterbox instead of stretching toolbars.
- `windowlaunch --size WxH` still overrides per launch.

### Carried / restored from 1.6
- Isolated Firefox profile `MPCECCWLPROFILE`.
- LXQt-style title bar (min / max / restore / close) + resize grip.
- Capture-phase clipboard (Ctrl+Shift+C/V) and tab shortcuts.
- Shell crash dialog with Restart Shell.
- EINTR-safe Ctrl+C in mpcsh (does not kill the shell).

## Files in this release
- `mpcsh.c` — shell 1.7.0
- `mpcecc.c` — console 1.7
- `README.md`, `CHANGELOG-1.7.md`
