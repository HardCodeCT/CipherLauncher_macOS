/*
 ══════════════════════════════════════════════════════════════════════════════
  cipher_launcher.cpp  –  Cipher Engine Launcher for macOS
  Version 1.1  (macOS POSIX port — parallel to the Windows build)
 ══════════════════════════════════════════════════════════════════════════════

  WHAT IT DOES
  ─────────────
  First run
    1. Detect CPU architecture (x86_64 / arm64) at runtime
    2. Check for Homebrew; prompt user to install if absent
    3. brew install fairy-stockfish  (if not already present)
    4. Check for Python 3; prompt user to install via Homebrew if absent
    5. pip3 install websockets
    6. Write engine_server.py to ~/Library/Application Support/Cipher/
    7. Download the NNUE weights file
    8. Register as a LaunchAgent (~/Library/LaunchAgents/) for auto-start
    9. Start engine_server.py

  Subsequent runs (TTY present — interactive)
    • All files present + engine already on :8765  →  just stream its output
    • All files present + engine NOT running        →  start engine
    • Any file missing despite marker existing      →  repair (re-run setup)

  Auto-start (no TTY — running from launchd)
    • exec() into python3 directly so launchd manages the Python process
      and will restart it automatically if it crashes

  BUILD (macOS — Command Line Tools / Xcode required)
  ──────────────────────────────────────────────────
    # Install Homebrew curl (recommended for up-to-date TLS):
    brew install curl

    # Intel / Rosetta2:
    clang++ -std=c++17 -O2 -Wall -Wextra \
        -I$(brew --prefix curl)/include \
        cipher_launcher.cpp \
        -L$(brew --prefix curl)/lib -lcurl \
        -framework CoreFoundation \
        -o CipherLauncher

    # Apple Silicon (arm64):
    # Same command — arch detected at runtime via uname()
 ══════════════════════════════════════════════════════════════════════════════
*/

// ── POSIX / BSD / macOS headers ───────────────────────────────────────────────
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/file.h>   // flock()
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mach-o/dyld.h>  // _NSGetExecutablePath

// ── C++ standard library ─────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <functional>

// ── libcurl (brew install curl) ───────────────────────────────────────────────
#include <curl/curl.h>


// ══════════════════════════════════════════════════════════════════════════════
//  Embedded engine_server.py
//  Written verbatim to ~/Library/Application Support/Cipher/engine_server.py
//  on first run.
// ══════════════════════════════════════════════════════════════════════════════
static const char ENGINE_SERVER_PY[] = R"RAWPY(
"""
Cipher - engine_server.py
Managed by CipherLauncher -- do not move or rename this file.
Runs Fairy-Stockfish and exposes it over a local WebSocket (ws://localhost:8765).

Install deps:  pip3 install websockets   (done automatically by the launcher)
"""
import asyncio
import concurrent.futures
import websockets
import subprocess
import json
import os
import sys
import shutil

# ── Locate fairy-stockfish ────────────────────────────────────────────────────
# Search order:
#   1. Sibling file in the same directory as this script (bundled/downloaded)
#   2. Homebrew prefix locations (arm64 and Intel)
#   3. PATH  (covers any other system-wide install)

BASE_DIR  = os.path.dirname(os.path.abspath(__file__))
_SF_NAME  = 'fairy-stockfish.exe' if sys.platform == 'win32' else 'fairy-stockfish'

def _find_stockfish() -> str:
    # 1. Bundled sibling
    sibling = os.path.join(BASE_DIR, _SF_NAME)
    if os.path.isfile(sibling) and os.access(sibling, os.X_OK):
        return sibling

    # 2. Homebrew — Apple Silicon prefix
    arm_path = '/opt/homebrew/bin/fairy-stockfish'
    if os.path.isfile(arm_path) and os.access(arm_path, os.X_OK):
        return arm_path

    # 3. Homebrew — Intel prefix
    intel_path = '/usr/local/bin/fairy-stockfish'
    if os.path.isfile(intel_path) and os.access(intel_path, os.X_OK):
        return intel_path

    # 4. Anything on PATH
    on_path = shutil.which('fairy-stockfish')
    if on_path:
        return on_path

    return ''

STOCKFISH_PATH   = _find_stockfish()
HOST             = 'localhost'
PORT             = 8765
DEFAULT_MOVETIME = 100   # ms — raise for stronger play

# ── Startup checks ────────────────────────────────────────────────────────────
print(f'[Engine] Stockfish path : {STOCKFISH_PATH if STOCKFISH_PATH else "<not found>"}')
if not STOCKFISH_PATH or not os.path.isfile(STOCKFISH_PATH):
    print('[Engine] ERROR – Fairy-Stockfish not found. Install with: brew install fairy-stockfish')
    sys.exit(1)
print(f'[Engine] Stockfish OK   ({os.path.getsize(STOCKFISH_PATH):,} bytes)')

NNUE_FILE = 'nn-46832cfbead3.nnue'
nnue_path = os.path.join(BASE_DIR, NNUE_FILE)
if os.path.isfile(nnue_path):
    print(f'[Engine] NNUE OK')
else:
    print(f'[Engine] WARNING – NNUE not found at {nnue_path}; engine uses default eval')


# ── Engine wrapper ────────────────────────────────────────────────────────────
class Engine:
    def __init__(self):
        self.variant = 'standard'
        self.nnue    = NNUE_FILE
        self.proc    = None
        self._start()

    def _start(self):
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        self.proc = subprocess.Popen(
            [STOCKFISH_PATH],
            stdin  = subprocess.PIPE,
            stdout = subprocess.PIPE,
            stderr = subprocess.PIPE,
            universal_newlines = True,
            creationflags      = flags,
            cwd                = BASE_DIR,
        )
        self._configure()

    def is_alive(self):
        return self.proc is not None and self.proc.poll() is None

    def restart(self):
        print('[Engine] Restarting ...')
        try:
            if self.proc:
                self.proc.kill()
                self.proc.wait(timeout=3)
        except Exception:
            pass
        self._start()

    def _send(self, cmd: str):
        if not self.is_alive():
            raise RuntimeError('Engine process is dead')
        self.proc.stdin.write(cmd + '\n')
        self.proc.stdin.flush()

    def _read(self) -> str:
        if not self.is_alive():
            raise RuntimeError('Engine process is dead')
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError('Engine stdout closed unexpectedly')
        return line.strip()

    def _await_token(self, token: str):
        while True:
            if self._read() == token:
                return

    def _configure(self):
        self._send('uci')
        self._await_token('uciok')
        self._apply_options()
        self._send('isready')
        self._await_token('readyok')
        print(f'[Engine] ready — variant={self.variant}')

    def _apply_options(self):
        self._send('setoption name Use NNUE value true')
        self._send(f'setoption name EvalFile value {self.nnue}')
        self._send(f'setoption name UCI_Variant value {self.variant}')
        self._send('setoption name Threads value 2')

    def reconfigure(self, variant: str, nnue: str):
        if variant == self.variant and nnue == self.nnue:
            return
        self.variant = variant
        self.nnue    = nnue
        self._apply_options()
        self._send('isready')
        self._await_token('readyok')
        print(f'[Engine] reconfigured -> {variant}')

    def best_move(self, moves: list, movetime: int = DEFAULT_MOVETIME):
        if not self.is_alive():
            self.restart()
        moves_str = ' '.join(moves) if moves else ''
        self._send(f'position startpos moves {moves_str}')
        self._send(f'go movetime {movetime}')
        while True:
            line = self._read()
            if line.startswith('bestmove'):
                parts = line.split()
                move  = parts[1] if len(parts) > 1 else '0000'
                return move[:2], move[2:4]

    def analyze_sync(self, variant: str, nnue: str, moves: list, movetime: int):
        self.reconfigure(variant, nnue)
        return self.best_move(moves, movetime)

    def close(self):
        try:
            if self.is_alive():
                self._send('quit')
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            pass


engine = Engine()

_engine_executor = concurrent.futures.ThreadPoolExecutor(
    max_workers=1, thread_name_prefix='engine'
)
_engine_lock = asyncio.Lock()


# ── WebSocket server ──────────────────────────────────────────────────────────
async def handler(websocket):
    addr = websocket.remote_address
    print(f'[Server] connected    {addr}')

    _pending = asyncio.Queue(maxsize=1)

    async def _analyzer():
        loop = asyncio.get_running_loop()
        while True:
            msg      = await _pending.get()
            variant  = msg.get('variant', engine.variant)
            nnue     = msg.get('nnue',    engine.nnue)
            moves    = msg.get('moves',   [])
            movetime = msg.get('movetime', DEFAULT_MOVETIME)
            async with _engine_lock:
                try:
                    frm, to = await loop.run_in_executor(
                        _engine_executor,
                        engine.analyze_sync, variant, nnue, moves, movetime,
                    )
                    await websocket.send(json.dumps({
                        'type': 'bestmove', 'from': frm, 'to': to, 'move': frm + to,
                    }))
                except RuntimeError as exc:
                    print(f'[Engine] error: {exc}')
                    try:
                        await websocket.send(json.dumps({'type': 'error', 'message': str(exc)}))
                    except Exception:
                        pass
                except (websockets.ConnectionClosedOK, websockets.ConnectionClosedError):
                    return
                except asyncio.CancelledError:
                    raise

    analyzer = asyncio.create_task(_analyzer())

    try:
        async for raw in websocket:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            kind = msg.get('type')

            if kind == 'ping':
                try:
                    await websocket.send(json.dumps({'type': 'pong'}))
                except Exception:
                    break

            elif kind == 'analyze':
                if _pending.full():
                    try:
                        _pending.get_nowait()
                    except asyncio.QueueEmpty:
                        pass
                await _pending.put(msg)

            elif kind == 'configure':
                loop = asyncio.get_running_loop()
                async with _engine_lock:
                    await loop.run_in_executor(
                        _engine_executor,
                        engine.reconfigure,
                        msg.get('variant', engine.variant),
                        msg.get('nnue',    engine.nnue),
                    )

    except (websockets.ConnectionClosedOK, websockets.ConnectionClosedError):
        pass
    finally:
        analyzer.cancel()
        try:
            await analyzer
        except (asyncio.CancelledError, Exception):
            pass
        print(f'[Server] disconnected  {addr}')

async def _main():
    print(f'[Server] listening on ws://{HOST}:{PORT}')
    async with websockets.serve(handler, HOST, PORT):
        await asyncio.Future()   # run forever


if __name__ == '__main__':
    try:
        asyncio.run(_main())
    except KeyboardInterrupt:
        engine.close()
        print('[Server] stopped')
)RAWPY";


// ══════════════════════════════════════════════════════════════════════════════
//  Configuration
// ══════════════════════════════════════════════════════════════════════════════
namespace Cfg {

// NNUE weights (still downloaded directly — not a Homebrew asset)
const char* NNUE_URL =
    "https://tests.stockfishchess.org/api/nn/nn-46832cfbead3.nnue";

// Homebrew formula name
const char* BREW_SF_FORMULA = "fairy-stockfish";

// fairy-stockfish binary name on PATH / in Homebrew prefix
const char* SF_EXE_NAME     = "fairy-stockfish";

// Local names
const char* APP_DIR_NAME    = "Cipher";                    // inside ~/Library/Application Support/
const char* NNUE_FILE       = "nn-46832cfbead3.nnue";
const char* PY_SCRIPT       = "engine_server.py";
const char* LAUNCHER_EXE    = "CipherLauncher";
const char* MARKER_FILE     = "installed.marker";
const char* PID_FILE        = "engine.pid";
const char* LOG_FILE        = "engine.log";
const char* LAUNCHAGENT_ID  = "com.cipher.engine";        // plist label
const int   ENGINE_PORT     = 8765;
const char* LOCK_PATH       = "/tmp/cipher_engine_launcher.lock";

} // namespace Cfg


// ══════════════════════════════════════════════════════════════════════════════
//  Console logging  (ANSI — native on macOS Terminal / iTerm2)
// ══════════════════════════════════════════════════════════════════════════════
static void Log    (const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void LogOK  (const char* msg);
static void LogFail(const char* msg);
static void LogStep(const char* msg);

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[Cipher] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static void LogOK  (const char* msg) { printf("  \x1b[32m[  OK  ]\x1b[0m  %s\n", msg); fflush(stdout); }
static void LogFail(const char* msg) { printf("  \x1b[31m[ FAIL ]\x1b[0m  %s\n", msg); fflush(stdout); }
static void LogStep(const char* msg) { printf("  \x1b[36m[ .... ]\x1b[0m  %s\n", msg); fflush(stdout); }


// ══════════════════════════════════════════════════════════════════════════════
//  Path helpers
// ══════════════════════════════════════════════════════════════════════════════
static std::string GetHomeDir() {
    const char* home = getenv("HOME");
    if (home && *home) return home;
    struct passwd* pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/tmp";
}

// ~/Library/Application Support/Cipher
static std::string GetAppDir() {
    std::string dir = GetHomeDir() + "/Library/Application Support/" + Cfg::APP_DIR_NAME;
    // mkdir -p equivalent
    std::string tmp;
    for (char c : dir) {
        tmp += c;
        if (c == '/') mkdir(tmp.c_str(), 0755);
    }
    mkdir(dir.c_str(), 0755);
    return dir;
}

static std::string Join(const std::string& dir, const char* name) {
    return dir + "/" + name;
}

static bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Returns true if file exists AND has at least minBytes
static bool FileReady(const std::string& path, size_t minBytes = 1024) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return (size_t)st.st_size >= minBytes;
}

// Current executable path (macOS-specific API)
static std::string GetExePath() {
    char buf[4096] = {};
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) return buf;
    // Buffer too small — allocate
    char* dynBuf = (char*)malloc(sz);
    if (_NSGetExecutablePath(dynBuf, &sz) == 0) {
        std::string result(dynBuf);
        free(dynBuf);
        return result;
    }
    return "";
}

// Detect CPU architecture at runtime (handles Rosetta2 correctly)
static std::string GetArch() {
    struct utsname u;
    if (uname(&u) != 0) return "x86_64";
    std::string machine = u.machine;
    if (machine == "arm64") return "arm64";
    return "x86_64";
}


// ══════════════════════════════════════════════════════════════════════════════
//  libcurl download helper with live progress bar
// ══════════════════════════════════════════════════════════════════════════════
struct DownloadState {
    const char* label;
    bool        firstProgress;
};

static int CurlProgressCB(void* clientp,
                           curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* state = static_cast<DownloadState*>(clientp);
    if (dltotal > 0) {
        int pct = (int)(100.0 * dlnow / dltotal);
        int filled = pct / 5;
        char bar[22] = {};
        for (int i = 0; i < 20; i++) bar[i] = (i < filled) ? '#' : '-';
        bar[20] = '\0';
        printf("\r  \x1b[36m[ .... ]\x1b[0m  %-28s  [%s] %3d%%  (%lld/%lld KB)  ",
               state->label, bar, pct,
               (long long)(dlnow / 1024), (long long)(dltotal / 1024));
    } else if (dlnow > 0) {
        printf("\r  \x1b[36m[ .... ]\x1b[0m  %-28s  %lld KB downloaded       ",
               state->label, (long long)(dlnow / 1024));
    }
    fflush(stdout);
    return 0;  // non-zero aborts the transfer
}

static size_t CurlWriteCB(void* ptr, size_t size, size_t nmemb, void* stream) {
    return fwrite(ptr, size, nmemb, (FILE*)stream);
}

static bool DownloadFile(const char* url,
                         const std::string& destPath,
                         const char* label) {
    printf("\n");
    LogStep(label);

    // Remove partial/stale file
    unlink(destPath.c_str());

    FILE* fp = fopen(destPath.c_str(), "wb");
    if (!fp) {
        LogFail((std::string(label) + " — cannot create destination file").c_str());
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        LogFail((std::string(label) + " — curl_easy_init failed").c_str());
        return false;
    }

    DownloadState state{ label, true };

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  CurlWriteCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      10L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,     0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCB);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,   &state);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "CipherLauncher/1.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  60L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    printf("\n");  // end progress line

    if (res != CURLE_OK) {
        unlink(destPath.c_str());
        LogFail((std::string(label) + " — " + curl_easy_strerror(res)).c_str());
        return false;
    }
    if (!FileExists(destPath)) {
        LogFail((std::string(label) + " — file not written").c_str());
        return false;
    }
    LogOK(label);
    return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Process helpers
// ══════════════════════════════════════════════════════════════════════════════

static void ExecArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
}

static int RunAndWait(const std::vector<std::string>& args, bool verbose = false) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        if (!verbose) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        ExecArgv(args);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Run a command, capture its stdout into `out`, return exit code.
static int RunCapture(const std::vector<std::string>& args, std::string& out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid == -1) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) { dup2(null_fd, STDERR_FILENO); close(null_fd); }
        close(pipefd[1]);
        ExecArgv(args);
    }

    close(pipefd[1]);
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        out += buf;
    }
    close(pipefd[0]);

    // trim trailing whitespace / newline
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Homebrew detection
// ══════════════════════════════════════════════════════════════════════════════

// Returns the path to the `brew` executable, or empty string if not found.
static std::string FindBrew() {
    const char* candidates[] = {
        "/opt/homebrew/bin/brew",   // Apple Silicon
        "/usr/local/bin/brew",      // Intel
        "/home/linuxbrew/.linuxbrew/bin/brew",  // Linux (future-proofing)
    };
    for (const char* p : candidates) {
        if (FileExists(p)) return p;
    }
    // Fall back to PATH
    FILE* fp = popen("which brew 2>/dev/null", "r");
    if (fp) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            buf[strcspn(buf, "\n")] = '\0';
            if (buf[0] != '\0') { pclose(fp); return buf; }
        }
        pclose(fp);
    }
    return {};
}

// Returns the Homebrew prefix (e.g. /opt/homebrew or /usr/local), or "".
static std::string GetBrewPrefix(const std::string& brewExe) {
    std::string out;
    if (RunCapture({ brewExe, "--prefix" }, out) == 0 && !out.empty())
        return out;
    return {};
}

// Returns true if `formula` is already installed via Homebrew.
static bool BrewIsInstalled(const std::string& brewExe, const char* formula) {
    return RunAndWait({ brewExe, "list", "--formula", formula }) == 0;
}

// Runs `brew install <formula>` with output forwarded to the terminal.
static bool BrewInstall(const std::string& brewExe, const char* formula) {
    LogStep((std::string("brew install ") + formula).c_str());
    int rc = RunAndWait({ brewExe, "install", formula }, /*verbose=*/true);
    if (rc == 0) {
        LogOK((std::string("brew install ") + formula).c_str());
        return true;
    }
    LogFail((std::string("brew install ") + formula + " failed").c_str());
    return false;
}


// ══════════════════════════════════════════════════════════════════════════════
//  fairy-stockfish finder
//  Search order:
//    1. <brewPrefix>/bin/fairy-stockfish   (Homebrew install — most reliable)
//    2. /opt/homebrew/bin/fairy-stockfish  (hard-coded Apple Silicon fallback)
//    3. /usr/local/bin/fairy-stockfish     (hard-coded Intel fallback)
//    4. PATH  (any other system-wide install)
// ══════════════════════════════════════════════════════════════════════════════
static std::string FindStockfish(const std::string& brewPrefix) {
    auto isExec = [](const std::string& p) -> bool {
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
               (st.st_mode & S_IXUSR);
    };

    // 1. Homebrew prefix
    if (!brewPrefix.empty()) {
        std::string p = brewPrefix + "/bin/" + Cfg::SF_EXE_NAME;
        if (isExec(p)) return p;
    }

    // 2 & 3. Hard-coded prefixes
    const char* hardcoded[] = {
        "/opt/homebrew/bin/fairy-stockfish",
        "/usr/local/bin/fairy-stockfish",
    };
    for (const char* p : hardcoded) {
        if (isExec(p)) return p;
    }

    // 4. PATH via `which`
    FILE* fp = popen("which fairy-stockfish 2>/dev/null", "r");
    if (fp) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            buf[strcspn(buf, "\n")] = '\0';
            std::string p(buf);
            if (!p.empty() && isExec(p)) { pclose(fp); return p; }
        }
        pclose(fp);
    }

    return {};
}


// ══════════════════════════════════════════════════════════════════════════════
//  Python discovery
// ══════════════════════════════════════════════════════════════════════════════

static bool IsPython3(const std::string& exe) {
    if (!FileExists(exe)) return false;
    return RunAndWait({ exe, "-c", "import sys; sys.exit(0 if sys.version_info.major==3 else 1)" }) == 0;
}

static std::string FindPython() {
    const char* candidates[] = {
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "/opt/homebrew/bin/python3.13",
        "/opt/homebrew/bin/python3.12",
        "/opt/homebrew/bin/python3.11",
        "/usr/local/bin/python3.13",
        "/usr/local/bin/python3.12",
        "/usr/local/bin/python3.11",
        "/usr/local/bin/python3.10",
        "/usr/bin/python3",
    };
    for (const char* p : candidates) {
        if (IsPython3(p)) return p;
    }
    FILE* fp = popen("which python3 2>/dev/null", "r");
    if (fp) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            buf[strcspn(buf, "\n")] = '\0';
            std::string p(buf);
            if (!p.empty() && IsPython3(p)) { pclose(fp); return p; }
        }
        pclose(fp);
    }
    return {};
}


// ══════════════════════════════════════════════════════════════════════════════
//  pip
// ══════════════════════════════════════════════════════════════════════════════
static bool IsPackageInstalled(const std::string& pyExe, const char* pkg) {
    std::string import_stmt = std::string("import ") + pkg;
    return RunAndWait({ pyExe, "-c", import_stmt }) == 0;
}

static bool PipInstall(const std::string& pyExe, const char* package) {
    if (IsPackageInstalled(pyExe, package)) {
        LogOK((std::string("pip: ") + package + " already installed — skipping").c_str());
        return true;
    }
    LogStep((std::string("pip install ") + package).c_str());
    int rc = RunAndWait({ pyExe, "-m", "pip", "install", "--quiet",
                          "--break-system-packages", package });
    if (rc == 0) { LogOK((std::string("pip: ") + package).c_str()); return true; }
    rc = RunAndWait({ pyExe, "-m", "pip", "install", "--quiet", package });
    if (rc == 0) { LogOK((std::string("pip: ") + package).c_str()); return true; }
    LogFail((std::string("pip failed for ") + package).c_str());
    return false;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Deploy engine_server.py
// ══════════════════════════════════════════════════════════════════════════════
static bool DeployScript(const std::string& appDir) {
    std::string dest = Join(appDir, Cfg::PY_SCRIPT);
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) {
        LogFail("Cannot write engine_server.py to AppDir");
        return false;
    }
    const char* src = ENGINE_SERVER_PY;
    if (*src == '\n') ++src;
    f.write(src, (std::streamsize)strlen(src));
    LogOK("engine_server.py deployed");
    return true;
}


// ══════════════════════════════════════════════════════════════════════════════
//  LaunchAgent
// ══════════════════════════════════════════════════════════════════════════════
static void AddToStartup(const std::string& launcherPath) {
    std::string launchAgentsDir = GetHomeDir() + "/Library/LaunchAgents";
    mkdir(launchAgentsDir.c_str(), 0755);

    std::string plistPath = launchAgentsDir + "/" + Cfg::LAUNCHAGENT_ID + ".plist";

    std::ofstream f(plistPath, std::ios::trunc);
    if (!f) { LogFail("Could not write LaunchAgent plist"); return; }

    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n"
      << "<dict>\n"
      << "    <key>Label</key>\n"
      << "    <string>" << Cfg::LAUNCHAGENT_ID << "</string>\n"
      << "    <key>ProgramArguments</key>\n"
      << "    <array>\n"
      << "        <string>" << launcherPath << "</string>\n"
      << "    </array>\n"
      << "    <key>RunAtLoad</key>\n"
      << "    <true/>\n"
      << "    <key>KeepAlive</key>\n"
      << "    <true/>\n"
      << "    <key>StandardOutPath</key>\n"
      << "    <string>" << GetHomeDir() << "/Library/Application Support/"
         << Cfg::APP_DIR_NAME << "/" << Cfg::LOG_FILE << "</string>\n"
      << "    <key>StandardErrorPath</key>\n"
      << "    <string>" << GetHomeDir() << "/Library/Application Support/"
         << Cfg::APP_DIR_NAME << "/" << Cfg::LOG_FILE << "</string>\n"
      << "</dict>\n"
      << "</plist>\n";
    f.close();

    RunAndWait({ "launchctl", "unload", plistPath }, false);
    int rc = RunAndWait({ "launchctl", "load", plistPath }, false);
    if (rc == 0)
        LogOK("LaunchAgent registered (com.cipher.engine)");
    else
        LogFail("launchctl load failed — auto-start may not work");
}


// ══════════════════════════════════════════════════════════════════════════════
//  Installation marker
// ══════════════════════════════════════════════════════════════════════════════
static bool IsInstalled(const std::string& appDir) {
    return FileExists(Join(appDir, Cfg::MARKER_FILE));
}
static void WriteMarker(const std::string& appDir) {
    std::ofstream f(Join(appDir, Cfg::MARKER_FILE), std::ios::trunc);
    if (f) f << "installed";
}
static void RemoveMarker(const std::string& appDir) {
    unlink(Join(appDir, Cfg::MARKER_FILE).c_str());
}


// ══════════════════════════════════════════════════════════════════════════════
//  PID file
// ══════════════════════════════════════════════════════════════════════════════
static void WritePidFile(const std::string& appDir, pid_t pid) {
    std::ofstream f(Join(appDir, Cfg::PID_FILE), std::ios::trunc);
    if (f) f << (long)pid;
}
static pid_t ReadPidFile(const std::string& appDir) {
    std::ifstream f(Join(appDir, Cfg::PID_FILE));
    long pid = 0;
    if (f) f >> pid;
    return (pid_t)pid;
}
static bool IsPidAlive(pid_t pid) {
    if (pid <= 0) return false;
    return (kill(pid, 0) == 0 || errno == EPERM);
}


// ══════════════════════════════════════════════════════════════════════════════
//  Check whether engine WebSocket is listening on localhost:ENGINE_PORT
// ══════════════════════════════════════════════════════════════════════════════
static bool IsPortListening(int port) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    struct timeval tv{ 0, 400000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(s);
    return ok;
}


// ══════════════════════════════════════════════════════════════════════════════
//  File integrity check
// ══════════════════════════════════════════════════════════════════════════════
static bool FilesIntact(const std::string& appDir,
                        const std::string& pyExe,
                        const std::string& sfPath) {
    bool ok = true;
    auto chk = [&](const std::string& p, size_t minSz, const char* label) {
        if (!FileReady(p, minSz)) {
            Log("Missing / too small: %s", label);
            ok = false;
        }
    };
    chk(pyExe,                        1024,       "python3");
    chk(sfPath,                       512*1024,   "fairy-stockfish");
    chk(Join(appDir, Cfg::NNUE_FILE), 1024*1024,  "NNUE weights");
    chk(Join(appDir, Cfg::PY_SCRIPT), 1024,       "engine_server.py");
    return ok;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Kill the engine stored in the PID file
// ══════════════════════════════════════════════════════════════════════════════
static void KillEngine(const std::string& appDir) {
    pid_t pid = ReadPidFile(appDir);
    if (pid > 0 && IsPidAlive(pid)) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 30; i++) {
            usleep(100000);
            if (!IsPidAlive(pid)) break;
        }
        if (IsPidAlive(pid)) kill(pid, SIGKILL);
        LogOK("Engine server process terminated.");
    } else {
        Log("No running engine process found in PID file.");
    }
    unlink(Join(appDir, Cfg::PID_FILE).c_str());
}


// ══════════════════════════════════════════════════════════════════════════════
//  Start engine_server.py
// ══════════════════════════════════════════════════════════════════════════════
static bool StartEngine(const std::string& pyExe,
                        const std::string& appDir) {
    std::string scriptPath = Join(appDir, Cfg::PY_SCRIPT);

    // ── No-TTY / launchd mode ─────────────────────────────────────────────────
    if (!isatty(STDOUT_FILENO)) {
        std::string logPath = Join(appDir, Cfg::LOG_FILE);
        int logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }
        execl(pyExe.c_str(), pyExe.c_str(), "-u", scriptPath.c_str(), nullptr);
        _exit(1);
    }

    // ── TTY / interactive mode ────────────────────────────────────────────────
    int pipefd[2];
    if (pipe(pipefd) == -1) { LogFail("pipe() failed"); return false; }

    pid_t pid = fork();
    if (pid == -1) {
        LogFail("fork() failed");
        close(pipefd[0]); close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        chdir(appDir.c_str());
        execl(pyExe.c_str(), pyExe.c_str(), "-u", scriptPath.c_str(), nullptr);
        _exit(1);
    }

    close(pipefd[1]);

    WritePidFile(appDir, pid);
    Log("Engine process launched (PID %d)", (int)pid);
    printf("\n  \x1b[36m[engine output] ──────────────────────────────────────────\x1b[0m\n");

    std::atomic<bool> portLive(false);
    std::atomic<bool> stopProbe(false);
    bool              shownLive = false;

    std::thread portProbeThread([&]() {
        while (!stopProbe.load(std::memory_order_relaxed)) {
            if (IsPortListening(Cfg::ENGINE_PORT)) {
                portLive.store(true, std::memory_order_relaxed);
                return;
            }
            usleep(250000);
        }
    });

    char        buf[4096] = {};
    std::string leftover;
    ssize_t     bytesRead = 0;

    while ((bytesRead = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[bytesRead] = '\0';
        leftover += buf;

        size_t pos;
        while ((pos = leftover.find('\n')) != std::string::npos) {
            std::string line = leftover.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            printf("  %s\n", line.c_str());
            leftover.erase(0, pos + 1);
        }
        fflush(stdout);

        if (!shownLive && portLive.load(std::memory_order_relaxed)) {
            shownLive = true;
            printf("\n  \x1b[36m──────────────────────────────────────────────────────────\x1b[0m\n");
            LogOK("WebSocket server is live on ws://localhost:8765");
        }
    }

    if (!leftover.empty()) { printf("  %s\n", leftover.c_str()); fflush(stdout); }

    stopProbe.store(true, std::memory_order_relaxed);
    if (portProbeThread.joinable()) portProbeThread.join();

    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    printf("  \x1b[36m──────────────────────────────────────────────────────────\x1b[0m\n\n");

    bool finalPortLive = portLive.load(std::memory_order_relaxed);
    if (!finalPortLive) {
        LogFail("engine_server.py exited without starting the WebSocket server.");
        printf("  Python exit code: %d\n\n", exitCode);
    }
    return finalPortLive;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Single-instance lock
// ══════════════════════════════════════════════════════════════════════════════
static int AcquireLock() {
    int fd = open(Cfg::LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;
}


// ══════════════════════════════════════════════════════════════════════════════
//  Entry point
// ══════════════════════════════════════════════════════════════════════════════
int main(int /*argc*/, char* /*argv*/[]) {

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int lockFd = -1;
    if (isatty(STDOUT_FILENO)) {
        lockFd = AcquireLock();
        if (lockFd < 0) {
            printf("\n  [Cipher] Another instance is already running.\n");
            printf("  Press Enter to close ...\n");
            getchar();
            return 0;
        }
    }

    const std::string appDir   = GetAppDir();
    const std::string nnuePath = Join(appDir, Cfg::NNUE_FILE);
    const std::string arch     = GetArch();

    // ── Interactive banner ────────────────────────────────────────────────────
    if (isatty(STDOUT_FILENO)) {
        printf("\n");
        printf("  \x1b[1m\x1b[34m"
               " ╔══════════════════════════════════════╗\n"
               " ║       Cipher Engine Launcher          ║\n"
               " ╚══════════════════════════════════════╝"
               "\x1b[0m\n\n");
        Log("Install directory : %s", appDir.c_str());
        Log("Architecture      : %s", arch.c_str());
    }

    // ── Discover Homebrew, fairy-stockfish, and Python ────────────────────────
    std::string brewExe    = FindBrew();
    std::string brewPrefix = brewExe.empty() ? "" : GetBrewPrefix(brewExe);
    std::string sfPath     = FindStockfish(brewPrefix);
    std::string pyExe      = FindPython();

    // ══════════════════════════════════════════════════════════════════════════
    //  FAST PATH — already installed, ensure engine is running
    // ══════════════════════════════════════════════════════════════════════════
    if (IsInstalled(appDir) && !pyExe.empty() && !sfPath.empty()) {
        if (!isatty(STDOUT_FILENO)) {
            StartEngine(pyExe, appDir);
            return 1;
        }

        printf("\n");
        Log("Installation detected — checking engine status ...");

        if (FilesIntact(appDir, pyExe, sfPath)) {
            pid_t pid = ReadPidFile(appDir);
            if (IsPidAlive(pid)) {
                Log("Stale server detected (PID %d) — killing ...", (int)pid);
                KillEngine(appDir);
                usleep(400000);
            }
            if (IsPortListening(Cfg::ENGINE_PORT)) {
                Log("Port 8765 still occupied — waiting for release ...");
                usleep(600000);
            }
        } else {
            Log("One or more files are missing — starting repair ...");
            RemoveMarker(appDir);
            goto install_path;
        }

        printf("\n");
        LogStep("Starting engine_server.py ...");
        {
            bool ok = StartEngine(pyExe, appDir);
            if (ok) {
                printf("\n  Server is running. Press Enter to kill the server ...\n");
                getchar();
                KillEngine(appDir);
            } else {
                LogFail("Engine failed to start. See output above for details.");
                printf("\n  Press Enter to close ...\n");
                getchar();
            }
            if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
            curl_global_cleanup();
            return ok ? 0 : 1;
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  INSTALL PATH — first run or repair
    // ══════════════════════════════════════════════════════════════════════════
    install_path:
    printf("\n");
    printf("  \x1b[33m──────────────── First-time Setup ────────────────\x1b[0m\n\n");

    // Step 1: Homebrew
    printf("  \x1b[33m[1/6]\x1b[0m Homebrew\n");
    if (brewExe.empty()) {
        printf("\n  Homebrew was not found on this system.\n");
        printf("  Install it with:\n\n");
        printf("    /bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"\n\n");
        printf("  Then run CipherLauncher again.\n");
        printf("\n  Press Enter to close ...\n");
        getchar();
        if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
        curl_global_cleanup();
        return 1;
    } else {
        LogOK(("Homebrew found: " + brewExe + "  (prefix: " + brewPrefix + ")").c_str());
    }

    // Step 2: fairy-stockfish
    printf("\n  \x1b[33m[2/6]\x1b[0m Fairy-Stockfish\n");
    if (sfPath.empty()) {
        if (!BrewIsInstalled(brewExe, Cfg::BREW_SF_FORMULA)) {
            if (!BrewInstall(brewExe, Cfg::BREW_SF_FORMULA)) {
                printf("\n  You can also install it manually:\n");
                printf("    brew install fairy-stockfish\n");
                printf("\n  Press Enter to close ...\n");
                getchar();
                if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
                curl_global_cleanup();
                return 1;
            }
        }
        // Re-discover after install
        brewPrefix = GetBrewPrefix(brewExe);
        sfPath     = FindStockfish(brewPrefix);
        if (sfPath.empty()) {
            LogFail("fairy-stockfish installed but binary not found — PATH issue?");
            printf("\n  Press Enter to close ...\n");
            getchar();
            if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
            curl_global_cleanup();
            return 1;
        }
    } else {
        LogOK(("fairy-stockfish found: " + sfPath).c_str());
    }

    // Step 3: Python
    printf("\n  \x1b[33m[3/6]\x1b[0m Python\n");
    if (pyExe.empty()) {
        printf("\n  Python 3 was not found on this system.\n");
        printf("  Install it with Homebrew:\n\n");
        printf("    brew install python3\n\n");
        printf("  Or download from https://www.python.org/downloads/\n\n");
        printf("  After installing Python, run CipherLauncher again.\n");
        printf("\n  Press Enter to close ...\n");
        getchar();
        if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
        curl_global_cleanup();
        return 1;
    } else {
        LogOK(("Python found: " + pyExe).c_str());
    }

    // Step 4: websockets
    printf("\n  \x1b[33m[4/6]\x1b[0m Python packages\n");
    if (!PipInstall(pyExe, "websockets")) {
        LogFail("Failed to install websockets. Check your internet connection.");
        printf("\n  Press Enter to close ...\n");
        getchar();
        if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
        curl_global_cleanup();
        return 1;
    }

    // Step 5: engine_server.py
    printf("\n  \x1b[33m[5/6]\x1b[0m Engine script\n");
    if (!DeployScript(appDir)) {
        printf("\n  Press Enter to close ...\n");
        getchar();
        if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
        curl_global_cleanup();
        return 1;
    }

    // Step 5b: NNUE weights
    printf("\n  \x1b[33m[5b]\x1b[0m NNUE weights\n");
    if (!FileReady(nnuePath, 1024*1024)) {
        if (!DownloadFile(Cfg::NNUE_URL, nnuePath, "nn-46832cfbead3.nnue")) {
            Log("NNUE download failed — engine will use classical evaluation (weaker)");
        }
    } else {
        LogOK("NNUE weights already present");
    }

    // Step 6: LaunchAgent
    printf("\n  \x1b[33m[6/6]\x1b[0m macOS LaunchAgent\n");
    {
        std::string selfPath = GetExePath();
        std::string destExe  = Join(appDir, Cfg::LAUNCHER_EXE);
        if (selfPath != destExe) {
            RunAndWait({ "cp", "-f", selfPath, destExe }, false);
            chmod(destExe.c_str(), 0755);
            RunAndWait({ "xattr", "-d", "com.apple.quarantine", destExe }, false);
        }
        AddToStartup(destExe);
    }

    WriteMarker(appDir);

    // Start engine
    printf("\n");
    LogStep("Starting engine_server.py ...");
    bool started = StartEngine(pyExe, appDir);

    if (started) {
        printf("\n");
        printf("  \x1b[1m\x1b[32m"
               " ╔══════════════════════════════════════════════════╗\n"
               " ║  Setup complete!                                   ║\n"
               " ║  Server is running at ws://localhost:8765          ║\n"
               " ║  CipherLauncher will start automatically on login. ║\n"
               " ╚══════════════════════════════════════════════════╝"
               "\x1b[0m\n\n");
        printf("  Install the Cipher Chrome extension and enjoy!\n");
        printf("\n  Press Enter to kill the server ...\n");
        getchar();
        KillEngine(appDir);
    } else {
        LogFail("Engine failed to start. See output above for details.");
        printf("\n  Press Enter to close ...\n");
        getchar();
    }

    if (lockFd >= 0) { flock(lockFd, LOCK_UN); close(lockFd); }
    curl_global_cleanup();
    return started ? 0 : 1;
}
