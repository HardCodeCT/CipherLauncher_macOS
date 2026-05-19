# Cipher Launcher — macOS Build Guide

Native macOS port of the Cipher Engine Launcher.
Uses POSIX APIs throughout — no Wine, no MinGW, no cross-compilation needed.

## Project structure

```
CipherLauncher/
    cipher_launcher.cpp     ← All C++ source (single file, POSIX port)
.github/
    workflows/
        build.yml           ← GitHub Actions CI (arm64 + x86_64 + universal)
Makefile                    ← Local build shortcut
README.md                   ← This file
```

---

## Prerequisites

| Tool | How to get it |
|---|---|
| **Xcode Command Line Tools** | `xcode-select --install` |
| **Homebrew** | https://brew.sh |
| **Homebrew curl** | `brew install curl` |

> **Note:** fairy-stockfish is installed automatically by the launcher via
> `brew install fairy-stockfish` on first run. You do **not** need to install
> it manually before building or distributing.

---

## Local build

```bash
# Install dependencies (once)
xcode-select --install
brew install curl

# Native binary for your machine (auto-detects x86_64 or arm64)
make

# Universal binary (fat binary covering both Intel and Apple Silicon)
make universal

# Clean
make clean
```

Output lands in `build/CipherLauncher` (or `build/CipherLauncher_universal`).

### One-liner without Make

```bash
clang++ -std=c++17 -O2 -Wall \
    -I$(brew --prefix curl)/include \
    CipherLauncher/cipher_launcher.cpp \
    -L$(brew --prefix curl)/lib -lcurl \
    -framework CoreFoundation \
    -rpath $(brew --prefix curl)/lib \
    -o CipherLauncher
```

---

## Build via GitHub Actions

Push to `main` (or trigger **workflow_dispatch** from the Actions tab).
Three artifacts are produced:

| Artifact | Runner | Notes |
|---|---|---|
| `CipherLauncher_x86_64` | `macos-13` | Intel Macs; also runs on Apple Silicon via Rosetta 2 |
| `CipherLauncher_arm64`  | `macos-14` | Apple Silicon (M1/M2/M3/M4) native |
| `CipherLauncher_universal` | `macos-14` | Fat binary containing both slices — **recommended for distribution** |

---

## What the binary does at runtime

### First run (or after files are missing)

1. Detects CPU architecture (`uname -m` at runtime: `x86_64` or `arm64`)
2. Locates Homebrew (`/opt/homebrew/bin/brew` → `/usr/local/bin/brew` → PATH)
3. If Homebrew not found → prints install instructions and exits
4. Runs `brew install fairy-stockfish` if not already installed
5. Searches for Python 3 (Homebrew arm64, Homebrew Intel, system)
6. If not found → prints install instructions (`brew install python3`)
7. `pip3 install websockets` (skips if already installed; handles PEP 668)
8. Writes the embedded `engine_server.py` to `~/Library/Application Support/Cipher/`
9. Downloads the **NNUE** weights file (`nn-46832cfbead3.nnue`)
10. Copies itself to `~/Library/Application Support/Cipher/CipherLauncher`
11. Writes & loads a **LaunchAgent** plist at `~/Library/LaunchAgents/com.cipher.engine.plist`
12. Starts `engine_server.py`, streaming its output live to the terminal

### Subsequent interactive runs (`./CipherLauncher` in terminal)

| Situation | Action |
|---|---|
| All files intact, engine NOT running | Kill any stale PID, start fresh |
| Any file missing despite marker | Run full repair (re-install/re-download what's needed) |

### Auto-start (LaunchAgent, no TTY)

When launchd launches the binary on login, `isatty(stdout)` is false.
The launcher immediately `exec()`s into Python — replacing itself — so launchd
tracks Python's PID directly and restarts it automatically if it crashes
(`KeepAlive true` in the plist).

Server output and errors are written to:
```
~/Library/Application Support/Cipher/engine.log
```

### fairy-stockfish discovery (engine_server.py)

The embedded Python script searches for the engine in this order:

1. Sibling file in `~/Library/Application Support/Cipher/` (bundled fallback)
2. `/opt/homebrew/bin/fairy-stockfish` (Homebrew Apple Silicon)
3. `/usr/local/bin/fairy-stockfish` (Homebrew Intel)
4. `shutil.which('fairy-stockfish')` (any PATH install)

---

## File locations on the end-user machine

```
~/Library/Application Support/Cipher/
    CipherLauncher          ← copy of the launcher (stable startup path)
    engine_server.py        ← embedded Python WebSocket server
    nn-46832cfbead3.nnue    ← neural network weights
    installed.marker        ← presence = install complete
    engine.pid              ← PID of last engine process
    engine.log              ← stdout/stderr when running headless

~/Library/LaunchAgents/
    com.cipher.engine.plist ← auto-start on login

/opt/homebrew/bin/fairy-stockfish   ← managed by Homebrew (Apple Silicon)
/usr/local/bin/fairy-stockfish      ← managed by Homebrew (Intel)
```

---

## Removing the launcher

```bash
# Unload the LaunchAgent
launchctl unload ~/Library/LaunchAgents/com.cipher.engine.plist

# Remove Cipher's app directory
rm -rf ~/Library/Application\ Support/Cipher
rm ~/Library/LaunchAgents/com.cipher.engine.plist

# Optionally uninstall fairy-stockfish
brew uninstall fairy-stockfish
```

---

## Key differences from the Windows version

| Windows | macOS |
|---|---|
| `%APPDATA%\Cipher\` | `~/Library/Application Support/Cipher/` |
| HKCU Run registry key | `~/Library/LaunchAgents/com.cipher.engine.plist` |
| `URLDownloadToFile` (urlmon) | libcurl with CURLOPT_FOLLOWLOCATION |
| Direct GitHub binary download | `brew install fairy-stockfish` |
| `CreateProcess` / `ReadFile` pipe | `fork()` / `exec()` / POSIX `read()` pipe |
| `IsWow64Process` arch detect | `uname()` → `machine` field |
| `Global\CipherEngineLauncherMutex` | `flock()` on `/tmp/cipher_engine_launcher.lock` |
| `GetExtendedTcpTable` port kill | `SIGTERM` on PID-file process |
| `wchar_t` Win32 strings | `std::string` UTF-8 throughout |
| `/SUBSYSTEM:WINDOWS` (no console) | `isatty()` check for interactive vs headless |

---

## Distribution checklist

- [ ] Build `CipherLauncher_universal` (covers all modern Macs)
- [ ] Test on a clean macOS VM (both Intel and Apple Silicon if possible)
- [ ] Test without Homebrew pre-installed to verify install instructions
- [ ] Test without Python pre-installed to verify install instructions
- [ ] Bundle with the Cipher Chrome extension ZIP
- [ ] Ship `CipherLauncher_universal` as the default; single-arch builds on request
