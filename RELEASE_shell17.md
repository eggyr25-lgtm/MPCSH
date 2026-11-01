# MPCSH 1.7 / MPCECC 1.7 — Release `shell17`

## Linux x86_64 binaries (built 2026-11-01)

| File | Size | SHA256 |
|------|------|--------|
| `mpcsh` / `mpcsh-1.7-linux-x86_64` | 141560 | `5ba74c01f57812f5d05a592a85286ad040b9afbfce74977d1295bcfc61d1f54e` |
| `mpcecc` / `mpcecc-1.7-linux-x86_64` | 96504 | `237f16251576af9eb0a027995f0f20b6ed32671961eda427c68e89ae5e864bdd` |
| `MPCSH-1.7-linux-x86_64.tar.gz` | ~101 KB | (contains both) |

### Runtime deps (Ubuntu 24.04 / similar)

- `libgtk-4-1`, `libvte-2.91-gtk4-0`, `libcurl4`, `libx11-6`, `libxtst6`
- PixelWindow helpers on PATH: `Xvfb`, `xdpyinfo`, `import` (ImageMagick), `xdotool`

### Publish on GitHub (same flow as shell16)

```bash
gh release create shell17 \
  --title "MPCSH 1.7 AND MPCECC 1.7" \
  --notes-file CHANGELOG-1.7.md \
  mpcsh-1.7-linux-x86_64 \
  mpcecc-1.7-linux-x86_64 \
  MPCSH-1.7-linux-x86_64.tar.gz
```

Or web UI: **Releases → Draft a new release → tag `shell17`** → attach `mpcsh` + `mpcecc` (plain names like shell16).

### What is in 1.7

- Multi-window PixelWindows (wlid)
- Ctrl+Shift+Tab cycle (WM cannot steal)
- Default framebuffer 1440×900, frame cap 1920×1200 (Krita not cut off)
- CONTAIN aspect fit
