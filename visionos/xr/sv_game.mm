// StrikersVision: the "App -> game" half of visionos/include/sv_bridge.h, the game thread, the shared
// log, and the hand-over queues between the app's threads and the game thread (sv_xr.h).
//
// Threads: every sv_* function here may be called from the app's main thread (or any thread); the
// game thread only reads through the sv_game_* accessors in sv_xr.h, which take the same lock. Nothing
// here ever blocks the main thread on the game.

#include "../include/sv_bridge.h"
#include "sv_xr.h"
#include "../app/Native/sv_boot_trace.h"

#import <Foundation/Foundation.h>

#include <CoreFoundation/CoreFoundation.h>

#include <fcntl.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

// SDL3 (SDL_main.h). Declared here rather than included: on Apple platforms SDL_main.h also provides
// a main() of its own unless told otherwise, and the app's main belongs to SwiftUI.
extern "C" void SDL_SetMainReady(void);

namespace {

std::mutex g_lock;
SVAppCallbacks g_callbacks{};
bool g_haveCallbacks = false;

std::atomic<int> g_started{0};
std::atomic<int> g_running{0};
std::atomic<int> g_paused{0};
std::atomic<int> g_quit{0};

// The renderer: retained here (CFBridgingRetain) while it is the current one, so it cannot go away
// under the game thread between the app's hand-over and the game's next frame.
void* g_renderer = nullptr;
uint32_t g_rendererGeneration = 0;

std::deque<std::pair<std::string, std::string>> g_settings;
// Live settings already taken by the game thread: they override the environment (which is never
// written after the game starts, so getenv on any thread stays safe). "" = back to the default.
std::map<std::string, std::string> g_overrides;
std::atomic<bool> g_readyReported{false};
int g_pendingViewMode = -1;

int g_participants = 0;
int g_spatial = 0;
SVTablePoseCallback g_tablePoseCallback = nullptr;
bool g_remotePosePending = false;
float g_remotePose[16] = {};
float g_remoteScale = 1.0f;

std::string g_home;

// The log viewer feed (see sv_xr.h): set once the tail thread sees fd 2 is the log file it follows,
// from then on lines reach the viewer through the file only (no duplicates).
std::atomic<bool> g_tailOwnsStderr{false};
char g_logPath[1024] = {};  // set once, before any reader starts

void deliver_line(const char* line) {
  void (*logLine)(const char*) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_haveCallbacks) logLine = g_callbacks.log_line;
  }
  if (logLine != nullptr) logLine(line);
}

bool stderr_is(const struct stat& file) {
  struct stat err {};
  return fstat(STDERR_FILENO, &err) == 0 && err.st_dev == file.st_dev && err.st_ino == file.st_ino;
}

// True once stderr is the followed log file (the port's dup2). Checked by every writer too, so a
// line written right after the redirect reaches the viewer once (through the file), not twice.
bool tail_owns_stderr() {
  if (g_tailOwnsStderr.load()) return true;
  if (g_logPath[0] == '\0') return false;
  struct stat st {};
  if (stat(g_logPath, &st) == 0 && stderr_is(st)) {
    g_tailOwnsStderr.store(true);
    return true;
  }
  return false;
}

// Follows the game log ten times a second and hands each complete new line to the viewer. Reading
// the file (rather than putting a pipe behind stderr) keeps the file the first and only writer's
// target: a crash loses nothing on disk, and the viewer shows it on the next launch's tail anyway.
void* log_tail_thread(void*) {
  pthread_setname_np("strikers-log-tail");
  off_t offset = 0;
  ino_t inode = 0;
  std::string pending;
  char buf[16384];
  for (;;) {
    usleep(100000);
    struct stat st {};
    if (stat(g_logPath, &st) != 0) continue;
    if (!tail_owns_stderr()) continue;
    if (st.st_ino != inode || st.st_size < offset) {
      // A new or truncated file (the port opens its log with "w"): from the top.
      inode = st.st_ino;
      offset = 0;
      pending.clear();
    }
    if (st.st_size <= offset) continue;
    const int fd = open(g_logPath, O_RDONLY);
    if (fd < 0) continue;
    while (offset < st.st_size) {
      const ssize_t n = pread(fd, buf, sizeof(buf), offset);
      if (n <= 0) break;
      offset += n;
      pending.append(buf, static_cast<size_t>(n));
      size_t start = 0;
      for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::string line = pending.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // The app's own lines, mirrored into this file by SVLog, are in the viewer already.
        if (line.rfind("[app] ", 0) == 0) continue;
        deliver_line(line.c_str());
      }
      pending.erase(0, start);
      if (pending.size() > 65536) {  // a runaway line without a newline: hand it over as it is
        deliver_line(pending.c_str());
        pending.clear();
      }
    }
    close(fd);
  }
  return nullptr;
}

// The game log, opened here (before the game thread exists) and put behind fd 2, so the first
// lines and an early death are in strikers-log.txt too. A fresh file each session ("w", as the
// port's own open). STRIKERS_LOG is then set to "0" so config.c does not open it a second time
// with "w", which would truncate what is already there; fd 2 stays this file.
void open_log_early() {
  const char* path = std::getenv("STRIKERS_LOG");
  if (path == nullptr || path[0] != '/') return;
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    SVWARN("log: could not open %s (errno %d); the game opens it later", path, errno);
    return;
  }
  std::fflush(stderr);
  sv_boot_trace("7 stderr moves to the game log (Strikers/strikers-log.txt)");
  if (dup2(fd, STDERR_FILENO) < 0) {
    SVWARN("log: dup2 onto stderr failed (errno %d); the game opens it later", errno);
    close(fd);
    return;
  }
  close(fd);
  // Unbuffered, as config.c does: the lines before a crash are the ones that matter.
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::snprintf(g_logPath, sizeof(g_logPath), "%s", path);
  setenv("STRIKERS_LOG", "0", 1);
  std::fprintf(stderr, "[xr] log: %s (opened by the app before the game thread)\n", path);
}

// An Objective-C exception nobody caught (from a system framework, usually): its reason and stack
// go to fd 2 (the log) before the process aborts.
void on_uncaught_exception(NSException* exception) {
  NSString* text = [NSString stringWithFormat:@"\n*** strikers: uncaught exception %@: %@\n%@\n", exception.name,
                                              exception.reason, [exception.callStackSymbols componentsJoinedByString:@"\n"]];
  const char* c = text.UTF8String;
  if (c != nullptr) {
    ssize_t left = static_cast<ssize_t>(std::strlen(c));
    while (left > 0) {
      const ssize_t n = write(STDERR_FILENO, c, static_cast<size_t>(left));
      if (n <= 0) break;
      c += n;
      left -= n;
    }
  }
}

void start_log_tail() {
  if (g_logPath[0] == '\0') {
    const char* path = std::getenv("STRIKERS_LOG");
    if (path == nullptr || path[0] != '/') return;  // only a file path can be followed
    std::snprintf(g_logPath, sizeof(g_logPath), "%s", path);
  }
  pthread_t thread;
  if (pthread_create(&thread, nullptr, log_tail_thread, nullptr) == 0) {
    pthread_detach(thread);
  } else {
    SVWARN("log: could not start the viewer feed; the log file is still written");
  }
}

void release_renderer_locked() {
  if (g_renderer != nullptr) {
    CFRelease(g_renderer);
    g_renderer = nullptr;
  }
}

void* game_thread(void*) {
  pthread_setname_np("strikers-game");
  // The game's own argv: no arguments, everything comes from the environment.
  static char arg0[] = "strikers";
  static char* argv[] = {arg0, nullptr};
  SVLOG("game thread: starting strikers_main (home %s)", g_home.c_str());
  // SwiftUI owns the app's main(); SDL refuses SDL_Init until told the entry point was handled.
  SDL_SetMainReady();
  const int rc = strikers_main(1, argv);
  std::fflush(stderr);
  char reason[96];
  if (g_quit.load()) {
    std::snprintf(reason, sizeof(reason), "quit (exit code %d)", rc);
  } else {
    std::snprintf(reason, sizeof(reason), "the game loop ended (exit code %d)", rc);
  }
  SVLOG("game thread: %s", reason);
  g_running.store(0);
  SVAppCallbacks cb{};
  {
    std::lock_guard<std::mutex> lock(g_lock);
    cb = g_callbacks;
  }
  if (cb.game_ended != nullptr) cb.game_ended(reason);
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------------------------

extern "C" void sv_log_line(const char* line) {
  if (line == nullptr) return;
  // stderr is the game log once PortConfigLoad has redirected it (STRIKERS_LOG); before that it is
  // the process's own stderr (the device console).
  // stderr is unbuffered there (config.c), so the line is on disk before this returns.
  std::fprintf(stderr, "%s\n", line);
  // Until the log file is stderr (and followed), the viewer gets the line directly.
  if (!tail_owns_stderr()) deliver_line(line);
}

extern "C" void sv_log(const char* level, const char* fmt, ...) {
  char body[900];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  char line[960];
  std::snprintf(line, sizeof(line), "[xr%s] %s", level != nullptr ? level : "", body);
  sv_log_line(line);
}

extern "C" int sv_setting_get(const char* name, char* out, int outSize) {
  if (name == nullptr || out == nullptr || outSize <= 0) return 0;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    const auto it = g_overrides.find(name);
    if (it != g_overrides.end()) {
      std::snprintf(out, static_cast<size_t>(outSize), "%s", it->second.c_str());
      return it->second.empty() ? 0 : 1;
    }
  }
  const char* e = std::getenv(name);
  if (e == nullptr || *e == '\0') {
    out[0] = '\0';
    return 0;
  }
  std::snprintf(out, static_cast<size_t>(outSize), "%s", e);
  return 1;
}

extern "C" void sv_setting_override(const char* name, const char* value) {
  if (name == nullptr || *name == '\0') return;
  std::lock_guard<std::mutex> lock(g_lock);
  g_overrides[name] = value != nullptr ? value : "";
}

extern "C" float sv_env_float(const char* name, float fallback, float lo, float hi) {
  char buf[256];
  const char* e = sv_setting_get(name, buf, sizeof(buf)) ? buf : nullptr;
  float v = fallback;
  if (e != nullptr && *e != '\0') {
    char* end = nullptr;
    const float parsed = std::strtof(e, &end);
    if (end != e) v = parsed;
  }
  if (!(v >= lo)) v = lo;  // also catches NaN
  if (v > hi) v = hi;
  return v;
}

extern "C" int sv_env_int(const char* name, int fallback, int lo, int hi) {
  char buf[256];
  const char* e = sv_setting_get(name, buf, sizeof(buf)) ? buf : nullptr;
  long v = fallback;
  if (e != nullptr && *e != '\0') {
    char* end = nullptr;
    const long parsed = std::strtol(e, &end, 10);
    if (end != e) v = parsed;
  }
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return static_cast<int>(v);
}

// ---------------------------------------------------------------------------------------------
// App -> game (sv_bridge.h)
// ---------------------------------------------------------------------------------------------

extern "C" int sv_game_start(const char* home, const char* discFolder) {
  int expected = 0;
  if (!g_started.compare_exchange_strong(expected, 1)) return 1;  // once per process
  if (home == nullptr || *home == '\0') {
    SVERR("sv_game_start: no home folder");
    g_started.store(0);
    return 0;
  }
  g_home = home;
  setenv("STRIKERS_HOME", home, 1);
  if (chdir(home) != 0) SVWARN("sv_game_start: chdir(%s) failed: errno %d", home, errno);
  // The picked folder is where the disc is looked for when the app did not name the image itself.
  const char* data = std::getenv("STRIKERS_DATA");
  if ((data == nullptr || *data == '\0') && discFolder != nullptr && *discFolder != '\0') {
    setenv("STRIKERS_DATA", discFolder, 1);
  }
  // The headset owns the picture: Aurora headless (PortXrWanted), and SDL with no UIKit window of its
  // own (its UIKit video driver would create UI objects on the game thread). The controllers are read
  // here through GameController (sv_xr.mm) and handed to PAD, so SDL leaves them alone: otherwise
  // each Sense controller would also appear to SDL as a generic gamepad and press the wrong buttons.
  setenv("SDL_VIDEO_DRIVER", "dummy", 0);
  setenv("SDL_JOYSTICK_MFI", "0", 0);
  setenv("SDL_JOYSTICK_HIDAPI", "0", 0);
  // Quest's defaults, unless the app set them (main.cpp and audio_out.cpp read them).
  setenv("STRIKERS_AUDIO_BUFFERS", "10", 0);

  open_log_early();
  start_log_tail();
  NSSetUncaughtExceptionHandler(&on_uncaught_exception);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
#if defined(__APPLE__)
  // The game thread paces the display: it must not be scheduled like background work.
  pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  // The decomp keeps large structures on the stack (the console had a big main thread stack too).
  const size_t kStack = 256u * 1024u * 1024u;
  pthread_attr_setstacksize(&attr, kStack);
  pthread_t thread;
  g_running.store(1);
  const int rc = pthread_create(&thread, &attr, game_thread, nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    SVERR("sv_game_start: pthread_create failed (%d)", rc);
    g_running.store(0);
    g_started.store(0);
    return 0;
  }
  pthread_detach(thread);
  SVLOG("sv_game_start: game thread started (stack %zu MB)", kStack >> 20);
  return 1;
}

extern "C" int sv_game_running(void) { return g_running.load(); }

extern "C" void sv_set_layer_renderer(void* cpLayerRenderer) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (cpLayerRenderer == g_renderer) return;
  release_renderer_locked();
  if (cpLayerRenderer != nullptr) {
    CFRetain(cpLayerRenderer);
    g_renderer = cpLayerRenderer;
  }
  ++g_rendererGeneration;
}

extern "C" void sv_layer_renderer_invalidated(void) {
  std::lock_guard<std::mutex> lock(g_lock);
  release_renderer_locked();
  ++g_rendererGeneration;
}

extern "C" void sv_set_paused(int paused) { g_paused.store(paused != 0 ? 1 : 0); }

extern "C" void sv_game_request_quit(void) { g_quit.store(1); }

extern "C" void sv_setting_changed(const char* key, const char* value) {
  if (key == nullptr || *key == '\0') return;
  std::lock_guard<std::mutex> lock(g_lock);
  g_settings.emplace_back(key, value != nullptr ? value : "");
}

extern "C" void sv_register_app_callbacks(const SVAppCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (callbacks != nullptr) {
    g_callbacks = *callbacks;
    g_haveCallbacks = true;
  } else {
    g_callbacks = SVAppCallbacks{};
    g_haveCallbacks = false;
  }
}

extern "C" void sv_set_view_mode(int viewMode) {
  if (viewMode < SV_VIEW_JUMBOTRON_ROOM || viewMode > SV_VIEW_TABLETOP_STADIUM) return;
  std::lock_guard<std::mutex> lock(g_lock);
  g_pendingViewMode = viewMode;
}

extern "C" void sv_shareplay_state(int participants, int spatial) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_participants = participants;
  g_spatial = spatial;
}

extern "C" void sv_register_table_pose_callback(SVTablePoseCallback callback) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_tablePoseCallback = callback;
}

extern "C" void sv_apply_remote_table_pose(const float m[16], float scale) {
  if (m == nullptr) return;
  std::lock_guard<std::mutex> lock(g_lock);
  std::memcpy(g_remotePose, m, sizeof(g_remotePose));
  g_remoteScale = scale;
  g_remotePosePending = true;
}

// ---------------------------------------------------------------------------------------------
// Game-thread side (sv_xr.h)
// ---------------------------------------------------------------------------------------------

extern "C" void* sv_game_copy_renderer(uint32_t* generation) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (generation != nullptr) *generation = g_rendererGeneration;
  // Retained for the caller: the app may hand over another renderer (releasing this one here) at any
  // moment, and the game thread keeps using the one it took until its next frame.
  if (g_renderer != nullptr) CFRetain(g_renderer);
  return g_renderer;
}

extern "C" int sv_game_paused(void) { return g_paused.load(); }

extern "C" int sv_game_quit_requested(void) { return g_quit.load(); }

extern "C" int sv_game_take_setting(char* key, int keySize, char* value, int valueSize) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_settings.empty()) return 0;
  const auto kv = std::move(g_settings.front());
  g_settings.pop_front();
  std::snprintf(key, static_cast<size_t>(keySize), "%s", kv.first.c_str());
  std::snprintf(value, static_cast<size_t>(valueSize), "%s", kv.second.c_str());
  return 1;
}

extern "C" int sv_game_take_view_mode(void) {
  std::lock_guard<std::mutex> lock(g_lock);
  const int m = g_pendingViewMode;
  g_pendingViewMode = -1;
  return m;
}

extern "C" void sv_game_shareplay(int* participants, int* spatial) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (participants != nullptr) *participants = g_participants;
  if (spatial != nullptr) *spatial = g_spatial;
}

extern "C" void sv_game_send_table_pose(const float m[16], float scale) {
  SVTablePoseCallback cb = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    cb = g_tablePoseCallback;
  }
  if (cb != nullptr) cb(m, scale);
}

extern "C" int sv_game_take_remote_table_pose(float m[16], float* scale) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!g_remotePosePending) return 0;
  g_remotePosePending = false;
  std::memcpy(m, g_remotePose, sizeof(g_remotePose));
  *scale = g_remoteScale;
  return 1;
}

extern "C" void sv_game_set_upper_limbs_visible(int visible) {
  void (*fn)(int) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    fn = g_callbacks.set_upper_limbs_visible;
  }
  if (fn != nullptr) fn(visible);
}

extern "C" void sv_game_ready(void) {
  if (g_readyReported.exchange(true)) return;
  void (*fn)(void) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    fn = g_callbacks.game_ready;
  }
  SVLOG("game ready: the headset loop is running");
  if (fn != nullptr) fn();
}

extern "C" void sv_game_view_mode_changed(int viewMode) {
  void (*fn)(int) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    fn = g_callbacks.view_mode_changed;
  }
  if (fn != nullptr) fn(viewMode);
}
