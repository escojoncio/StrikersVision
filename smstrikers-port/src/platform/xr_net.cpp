// See include/port/xr_net.h (the game's side) and xr_net.hpp (xr_quest.cpp's side). Built with
// xr_quest.cpp in strikers_xr (C++20, no port prelude).
//
// The room (r39) is a room code on the game's own relay (server/ in this repository): each headset holds
// one outbound WebSocket to it (nothing to forward on anyone's router) and sends small binary
// messages, which the relay hands to everyone in the room, so each headset also hears its own and
// skips them. A network thread owns the connection; the game thread queues what to send and takes
// what arrived once a headset frame (poll), so the lockstep never waits on the network in a frame.
#include "../../include/port/xr_net.h"
#include "xr_net.hpp"

extern "C" void PortNetSetFixedStep(int on);  // src/platform/vi.c

// Which transport carries the room: Android (Quest) reaches NetWs.java over JNI; visionOS reaches the
// sv_net_* functions of visionos/include/sv_bridge.h (WebSocket relay or SharePlay, in Swift). The
// lockstep, the message format and the protocol number are shared, so a Vision Pro and a Quest on
// the same release play each other through the same relay.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) && defined(STRIKERS_XR)
#define XRNET_ANDROID 1
#elif defined(__APPLE__) && (defined(STRIKERS_VISIONOS) || (defined(TARGET_OS_VISION) && TARGET_OS_VISION))
#define XRNET_APPLE 1
#endif

#if defined(XRNET_ANDROID) || defined(XRNET_APPLE)

#if defined(XRNET_ANDROID)
#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <jni.h>
#else
#include "../../../visionos/include/sv_bridge.h"
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace xrnet {
namespace {

// r39: also into the game log (stderr -> /sdcard/Strikers/strikers-log.txt). The headset's system
// log turns over within minutes (thermal audit spam), and the first online night's lines were gone
// before anyone could read them.
static void net_file_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void net_file_log(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[xr] %s\n", line);
}
#if defined(XRNET_ANDROID)
#define NLOG(...)                                                       \
  do {                                                                  \
    __android_log_print(ANDROID_LOG_INFO, "strikers_xr", __VA_ARGS__); \
    xrnet::net_file_log(__VA_ARGS__);                                   \
  } while (0)
#else
// visionOS: stderr is the game log (STRIKERS_LOG), which the app's log viewer shows.
#define NLOG(...) xrnet::net_file_log(__VA_ARGS__)
#endif

// r39: the game's own room server (server/ in this repository, a Cloudflare Worker). Each headset holds
// one WebSocket to wss://<host>/room/<CODE>; every binary message goes to everyone in that room,
// the sender included (the game skips its own, and its echoes prove the link is alive). The code is
// the one the player picked in the launcher panel (LobbyActivity -> STRIKERS_ROOM, or
// StrikersActivity.nativeSetRoom). No code: no network at all, the game is offline.
// r33-r38 used one fixed room on free public MQTT brokers; two headsets could end up on different
// brokers, and the brokers dropped messages (the first online night froze).
constexpr const char* kServerHost = "strikers-rooms.strikers-rooms.workers.dev";
constexpr int kServerPort = 443;
constexpr uint8_t kProtocol = 2;  // r39: room server + room codes + resend fix (r38 headsets: 1)

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// A room code: 3-8 letters or digits, upper case; anything else means no room.
std::string sanitize_room(const char* s) {
  std::string r;
  if (s == nullptr) return r;
  for (; *s != '\0' && r.size() < 8; ++s) {
    const char c = *s;
    if (c >= 'a' && c <= 'z') r.push_back(static_cast<char>(c - 'a' + 'A'));
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) r.push_back(c);
  }
  return r.size() >= 3 ? r : std::string();
}

// ---------------------------------------------------------------------------------------------
// The queues between the game thread and the network thread
// ---------------------------------------------------------------------------------------------

struct Shared {
  std::mutex m;
  std::deque<std::vector<uint8_t>> out;  // to send
  std::deque<std::vector<uint8_t>> in;   // received (every message in the room, ours included)
  std::string room;                      // the room picked, "" = none (under m)
  // strikers-quest r39b: the room the game has moved to (poll, never during a match), which the
  // connection follows (under m). `room` is only the pick, so a pick mid-match no longer cuts it.
  std::string activeRoom;
  bool roomSet = false;                  // room came from nativeSetRoom (under m)
  std::atomic<bool> connected{false};
  std::atomic<bool> running{false};
};
Shared sh;

#if defined(XRNET_ANDROID)
// ---------------------------------------------------------------------------------------------
// The connection: NetWs.java (TLS + WebSocket, Android's own TLS and certificate checks) over JNI.
// The class is looked up on the game thread, a Java thread whose class loader knows the app's
// classes; the network thread attaches itself through SDL and uses the cached global references.
// ---------------------------------------------------------------------------------------------

struct Jni {
  jclass cls = nullptr;
  jmethodID open = nullptr, send = nullptr, recv = nullptr, close = nullptr;
};
Jni g_jni;

bool jni_ok(JNIEnv* env, const char* what) {
  if (!env->ExceptionCheck()) return true;
  env->ExceptionDescribe();
  env->ExceptionClear();
  NLOG("net: the Java side of %s threw", what);
  return false;
}

bool jni_init() {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) return false;
  jclass local = env->FindClass("io/github/newcoke/strikers/NetWs");
  if (local == nullptr) {
    env->ExceptionClear();
    return false;
  }
  g_jni.cls = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  // strikers-quest r39b: each lookup checked on its own. A missing method throws, and the next JNI
  // call made with that exception still pending aborts the app.
  auto method = [&](const char* name, const char* sig) -> jmethodID {
    const jmethodID id = env->GetStaticMethodID(g_jni.cls, name, sig);
    return jni_ok(env, name) ? id : nullptr;
  };
  g_jni.open = method("open", "(Ljava/lang/String;ILjava/lang/String;I)I");
  g_jni.send = method("send", "(I[B)Z");
  g_jni.recv = method("recv", "(I[BI)I");
  g_jni.close = method("close", "(I)V");
  return g_jni.open != nullptr && g_jni.send != nullptr && g_jni.recv != nullptr && g_jni.close != nullptr;
}

// ---------------------------------------------------------------------------------------------
// The network thread
// ---------------------------------------------------------------------------------------------

bool net_bridge_init() { return jni_init(); }

void net_thread(uint64_t myId) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    NLOG("net: the network thread has no Java environment, staying offline");
    return;
  }
  auto rbuf = static_cast<jbyteArray>(env->NewGlobalRef(env->NewByteArray(65536)));
  auto jhost = static_cast<jstring>(env->NewGlobalRef(env->NewStringUTF(kServerHost)));
  std::vector<uint8_t> rx(65536);
  int handle = -1;
  std::string room;
  uint64_t retryAt = 0;
  // strikers-quest r39b: when the oldest send still waiting for its echo went out, 0 = none waiting.
  uint64_t unechoedSince = 0;
  unsigned failures = 0;
  auto drop = [&](const char* why) {
    if (handle >= 0) {
      env->CallStaticVoidMethod(g_jni.cls, g_jni.close, handle);
      jni_ok(env, "close");
      if (why != nullptr) NLOG("net: left room %s (%s)", room.c_str(), why);
    }
    handle = -1;
    sh.connected = false;
  };
  while (sh.running.load()) {
    std::string want;
    {
      std::lock_guard<std::mutex> lock(sh.m);
      want = sh.activeRoom;
    }
    if (handle >= 0 && want != room) drop("the room changed");
    if (handle < 0) {
      if (want.empty() || now_ms() < retryAt) {
        usleep(50000);
        continue;
      }
      const std::string path = "/room/" + want;
      jstring jpath = env->NewStringUTF(path.c_str());
      // r39b: no call with an exception (out of memory) still pending from NewStringUTF.
      handle = jpath != nullptr ? env->CallStaticIntMethod(g_jni.cls, g_jni.open, jhost, kServerPort, jpath, 5000)
                                : -1;
      if (jpath != nullptr) env->DeleteLocalRef(jpath);
      if (!jni_ok(env, "open")) handle = -1;
      if (handle < 0) {
        ++failures;
        retryAt = now_ms() + std::min<uint64_t>(10000, 1000ull * failures);
        if (failures <= 3 || failures % 10 == 0) NLOG("net: the room server did not answer (try %u)", failures);
        continue;
      }
      failures = 0;
      room = want;
      unechoedSince = 0;
      sh.connected = true;
      NLOG("net: in room %s", room.c_str());
    }

    // Out, in order. What could not be sent goes back to the front of the queue for the next link.
    std::deque<std::vector<uint8_t>> out;
    {
      std::lock_guard<std::mutex> lock(sh.m);
      // r39b: the game has moved to another room: nothing of the new room's down this link (the
      // top of the loop leaves it first).
      if (sh.activeRoom != room) continue;
      out.swap(sh.out);
    }
    while (!out.empty()) {
      const std::vector<uint8_t>& msg = out.front();
      jbyteArray arr = env->NewByteArray(static_cast<jsize>(msg.size()));
      if (arr == nullptr) {  // r39b: out of memory; cleared before any other JNI call
        jni_ok(env, "send");
        break;
      }
      env->SetByteArrayRegion(arr, 0, static_cast<jsize>(msg.size()), reinterpret_cast<const jbyte*>(msg.data()));
      // strikers-quest r39b: the exception check first, whatever send returned. After `send() &&`
      // it was skipped on a false return, and the next JNI call (close) aborted the app.
      const jboolean ok = env->CallStaticBooleanMethod(g_jni.cls, g_jni.send, handle, arr);
      const bool sent = jni_ok(env, "send") && ok;
      env->DeleteLocalRef(arr);
      if (!sent) break;
      out.pop_front();
      if (unechoedSince == 0) unechoedSince = now_ms();
    }
    if (!out.empty()) {
      {
        std::lock_guard<std::mutex> lock(sh.m);
        while (sh.activeRoom == room && !out.empty()) {  // r39b: only while it is still this room's
          sh.out.push_front(std::move(out.back()));
          out.pop_back();
        }
      }
      drop("a send failed");
      retryAt = now_ms() + 200;
      continue;
    }

    // In: whole messages, until none arrives for 4 ms.
    for (;;) {
      const jint n = env->CallStaticIntMethod(g_jni.cls, g_jni.recv, handle, rbuf, 4);
      if (!jni_ok(env, "recv") || n == -2) break;
      if (n < 0) {
        drop("closed by the server");
        retryAt = now_ms() + 200;
        break;
      }
      env->GetByteArrayRegion(rbuf, 0, n, reinterpret_cast<jbyte*>(rx.data()));
      // strikers-quest r39b: one of this headset's own messages back (the sender id at byte 2, the
      // header handle() reads): the link carries ours both ways. The friend's messages don't count;
      // they say nothing about whether ours get out.
      if (n >= 10) {
        uint64_t sender = 0;
        for (int i = 0; i < 8; ++i) sender |= static_cast<uint64_t>(rx[2 + i]) << (8 * i);
        if (sender == myId) unechoedSince = 0;
      }
      std::lock_guard<std::mutex> lock(sh.m);
      if (sh.activeRoom != room) continue;  // r39b: the old room's, after the game moved on
      sh.in.emplace_back(rx.begin(), rx.begin() + n);
      if (sh.in.size() > 8192) sh.in.pop_front();
    }

    // The server echoes everything we send: none of ours back for 5 s is a dead link.
    // strikers-quest r39b: timed from the oldest send still unanswered, not from the last message
    // in: after any quiet spell (headset off, both loading) the first send found that one 5 s old
    // and dropped a healthy link before its echo could come back.
    if (handle >= 0 && unechoedSince != 0 && now_ms() - unechoedSince > 5000) {
      drop("no echo for 5 s");
      retryAt = now_ms() + 200;
    }
  }
  drop(nullptr);
}

#endif  // XRNET_ANDROID

// ---------------------------------------------------------------------------------------------
// The connection on visionOS: the sv_net_* transports (visionos/net). The room key is the code, or
// "~" + the code for the SharePlay group (sv_net_set_room), so switching between the relay and
// SharePlay reads as a room change everywhere below and the link follows it.
// ---------------------------------------------------------------------------------------------

#if defined(XRNET_APPLE)
constexpr char kSharePlayMark = '~';

bool net_bridge_init() { return true; }

void net_thread(uint64_t myId) {
  std::vector<uint8_t> rx(65536);
  int handle = -1;
  std::string room;
  uint64_t retryAt = 0;
  uint64_t unechoedSince = 0;  // as on Android: the oldest send still waiting for its echo
  unsigned failures = 0;
  auto drop = [&](const char* why) {
    if (handle >= 0) {
      sv_net_close(handle);
      if (why != nullptr) NLOG("net: left room %s (%s)", room.c_str(), why);
    }
    handle = -1;
    sh.connected = false;
  };
  while (sh.running.load()) {
    std::string want;
    {
      std::lock_guard<std::mutex> lock(sh.m);
      want = sh.activeRoom;
    }
    if (handle >= 0 && want != room) drop("the room changed");
    if (handle < 0) {
      if (want.empty() || now_ms() < retryAt) {
        usleep(50000);
        continue;
      }
      const bool viaSharePlay = want[0] == kSharePlayMark;
      const std::string code = viaSharePlay ? want.substr(1) : want;
      // sv_net_open never blocks; the link counts as up once the transport says so (NetWs.open
      // returned only after the WebSocket upgrade, with the same 5 s budget).
      handle = sv_net_open(viaSharePlay ? SV_NET_SHAREPLAY : kServerHost, kServerPort, code.c_str(), 0);
      const uint64_t deadline = now_ms() + 5000;
      while (handle >= 0 && sv_net_connected(handle) == 0 && now_ms() < deadline && sh.running.load()) {
        usleep(10000);
      }
      if (handle >= 0 && sv_net_connected(handle) == 0) {
        sv_net_close(handle);
        handle = -1;
      }
      if (handle < 0) {
        ++failures;
        retryAt = now_ms() + std::min<uint64_t>(10000, 1000ull * failures);
        if (failures <= 3 || failures % 10 == 0) {
          NLOG("net: %s did not answer (try %u)", viaSharePlay ? "the SharePlay group" : "the room server",
               failures);
        }
        continue;
      }
      failures = 0;
      room = want;
      unechoedSince = 0;
      sh.connected = true;
      NLOG("net: in room %s%s", code.c_str(), viaSharePlay ? " (SharePlay)" : "");
    }

    // Out, in order. What could not be sent goes back to the front of the queue for the next link.
    std::deque<std::vector<uint8_t>> out;
    {
      std::lock_guard<std::mutex> lock(sh.m);
      if (sh.activeRoom != room) continue;
      out.swap(sh.out);
    }
    while (!out.empty()) {
      const std::vector<uint8_t>& msg = out.front();
      if (sv_net_send(handle, msg.data(), static_cast<int>(msg.size())) != 1) break;
      out.pop_front();
      if (unechoedSince == 0) unechoedSince = now_ms();
    }
    if (!out.empty()) {
      {
        std::lock_guard<std::mutex> lock(sh.m);
        while (sh.activeRoom == room && !out.empty()) {
          sh.out.push_front(std::move(out.back()));
          out.pop_back();
        }
      }
      drop("a send failed");
      retryAt = now_ms() + 200;
      continue;
    }

    // In: every whole message already here; sv_net_recv never waits, so a quiet link sleeps 4 ms
    // (the wait NetWs.recv did on Android).
    bool any = false;
    for (;;) {
      const int n = sv_net_recv(handle, rx.data(), static_cast<int>(rx.size()));
      if (n == 0) break;
      if (n == -2) {  // bigger than the game takes: it would read as a different message
        drop("a message bigger than the game takes");
        retryAt = now_ms() + 200;
        break;
      }
      if (n < 0) {
        drop("closed by the server");
        retryAt = now_ms() + 200;
        break;
      }
      any = true;
      if (n >= 10) {
        uint64_t sender = 0;
        for (int i = 0; i < 8; ++i) sender |= static_cast<uint64_t>(rx[2 + i]) << (8 * i);
        if (sender == myId) unechoedSince = 0;
      }
      std::lock_guard<std::mutex> lock(sh.m);
      if (sh.activeRoom != room) continue;
      sh.in.emplace_back(rx.begin(), rx.begin() + n);
      if (sh.in.size() > 8192) sh.in.pop_front();
    }
    if (handle < 0) continue;
    if (!any) usleep(4000);

    // Both transports hand a sender its own messages back (the relay does; the SharePlay transport
    // loops them back once delivered), so the same 5 s rule finds a dead link.
    if (unechoedSince != 0 && now_ms() - unechoedSince > 5000) {
      drop("no echo for 5 s");
      retryAt = now_ms() + 200;
    }
  }
  drop(nullptr);
}
#endif  // XRNET_APPLE

// ---------------------------------------------------------------------------------------------
// Messages: [type][protocol][sender id: 8 bytes] then the body, little-endian.
// ---------------------------------------------------------------------------------------------

enum Type : uint8_t { kHello = 1, kStart = 2, kInput = 3, kHash = 4, kPing = 5, kPong = 6 };

struct Writer {
  std::vector<uint8_t> v;
  void u8(uint8_t x) { v.push_back(x); }
  void u32(uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
  }
  void u64(uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
  }
  void bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
  }
};

struct Reader {
  const std::vector<uint8_t>& v;
  size_t at = 0;
  bool ok = true;
  bool need(size_t n) {
    if (at + n > v.size()) ok = false;
    return ok;
  }
  uint8_t u8() { return need(1) ? v[at++] : 0; }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t x = 0;
    for (int i = 0; i < 4; ++i) x |= static_cast<uint32_t>(v[at++]) << (8 * i);
    return x;
  }
  uint64_t u64() {
    if (!need(8)) return 0;
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<uint64_t>(v[at++]) << (8 * i);
    return x;
  }
  void bytes(void* p, size_t n) {
    if (need(n)) {
      std::memcpy(p, v.data() + at, n);
      at += n;
    }
  }
};

// ---------------------------------------------------------------------------------------------
// The room and the match (game thread)
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kRing = 512;  // frames of input kept either way (more than any delay)
constexpr uint32_t kRttSamples = 9;  // r39b: round trips kept for the median

struct Choice {
  int team = 0, sidekick = 0, stadium = 0;
  uint8_t options[64] = {};
  int optionsSize = 0;
};

struct State {
  bool threadStarted = false;
  bool netBroken = false;  // no Java bridge: offline for good
  std::string room;        // the room the state below belongs to
  std::string pickedRoom;  // r39b: the last pick seen (a pick that has to wait is logged once)
  uint64_t myId = 0;
  bool wasConnected = false;
  // The friend.
  uint64_t peerId = 0;
  uint64_t peerSeenMs = 0;
  uint64_t peerHelloMs = 0;  // r39b: their last hello, so their ready bit is a current one
  bool peerReady = false;
  // r39b: their first "not ready" hello after a ready one while this headset waited for them, 0 = none.
  uint64_t peerLeftHoldMs = 0;
  Choice peerChoice;
  uint32_t rttMs = 200;
  uint32_t rttSamples[kRttSamples] = {};  // r39b: the friend's last round trips; rttMs is their median
  uint32_t rttCount = 0, rttNext = 0;
  uint64_t lastHelloMs = 0, lastPingMs = 0;
  // r39b: a headset in the room on another protocol (the match start says so when it goes offline).
  uint64_t otherBuildMs = 0;
  uint8_t otherBuildProtocol = 0;
  bool otherBuildLogged = false;
  // This headset at the match start (the loading screen).
  bool localReady = false;
  uint64_t readySinceMs = 0, lastHoldMs = 0;
  bool gaveUp = false;
  Choice localChoice;
  // The online match.
  bool startPending = false, online = false, isHost = false;
  std::vector<uint8_t> startMsg;
  uint64_t lastStartSendMs = 0, onlineSinceMs = 0;
  uint32_t seed = 1;
  int delay = 8;
  PortNetMatch match{};
  bool startReported = false;
  uint32_t frame = 0;      // the next game frame to run
  bool stepped = false;    // r39b: step() handed out `frame`; PortNetCommitFrame uses it up
  uint32_t localNext = 0;  // the next frame this headset's input goes to
  NetPad localBuf[kRing];
  NetPad remoteBuf[kRing];
  uint32_t remoteTag[kRing] = {};  // frame + 1 of what remoteBuf holds, 0 = nothing
  uint64_t lastRemoteMs = 0, lastSendMs = 0, stallSinceMs = 0;
  uint64_t lastProgressMs = 0;  // r39: last time a game frame ran online (the watchdog); r39b: or ended
  uint32_t myHash[kRing] = {}, myHashTag[kRing] = {};
  uint32_t peerHash[kRing] = {}, peerHashTag[kRing] = {};
  uint32_t desyncs = 0, checks = 0;
  int frontEndFrames = 0;
  uint32_t stalls = 0;
};
State st;

void send(std::vector<uint8_t> msg) {
  std::lock_guard<std::mutex> lock(sh.m);
  sh.out.emplace_back(std::move(msg));
  if (sh.out.size() > 2048) sh.out.pop_front();
}

Writer header(Type t) {
  Writer w;
  w.u8(t);
  w.u8(kProtocol);
  w.u64(st.myId);
  return w;
}

void write_choice(Writer& w, const Choice& c) {
  w.u32(static_cast<uint32_t>(c.team));
  w.u32(static_cast<uint32_t>(c.sidekick));
  w.u32(static_cast<uint32_t>(c.stadium));
  w.u8(static_cast<uint8_t>(c.optionsSize));
  w.bytes(c.options, static_cast<size_t>(c.optionsSize));
}

Choice read_choice(Reader& r) {
  Choice c;
  c.team = static_cast<int>(r.u32());
  c.sidekick = static_cast<int>(r.u32());
  c.stadium = static_cast<int>(r.u32());
  c.optionsSize = std::min<int>(r.u8(), 64);
  r.bytes(c.options, static_cast<size_t>(c.optionsSize));
  return c;
}

void send_hello() {
  Writer w = header(kHello);
  const bool ready = st.localReady && !st.online && !st.startPending;
  w.u8(ready ? 1 : 0);
  write_choice(w, st.localChoice);
  send(std::move(w.v));
  st.lastHelloMs = now_ms();
}

void send_inputs() {
  if (st.localNext <= static_cast<uint32_t>(st.delay)) return;
  // Everything the friend may still need: they are at most `delay` + 1 frames behind this headset
  // (each side fills its own input up to its frame + delay before it waits for the other's).
  // r39: was `delay`, one frame short: one lost message then stalled both headsets for good.
  const uint32_t d = static_cast<uint32_t>(st.delay);
  uint32_t first = std::max<uint32_t>(d, st.frame > d + 1 ? st.frame - d - 1 : 0);
  if (st.localNext - first > 255) first = st.localNext - 255;
  Writer w = header(kInput);
  w.u32(first);
  w.u8(static_cast<uint8_t>(st.localNext - first));
  for (uint32_t f = first; f < st.localNext; ++f) w.bytes(&st.localBuf[f % kRing], sizeof(NetPad));
  send(std::move(w.v));
  st.lastSendMs = now_ms();
}

// r39b: the friend was heard from within the last `ms`.
bool peer_heard(uint64_t ms) { return st.peerId != 0 && now_ms() - st.peerSeenMs < ms; }
bool peer_present() { return peer_heard(5000); }

// strikers-quest r39b: how long a friend may go quiet while this headset already waits for them at
// the loading screen (their headset off a moment, a long load, their link reconnecting). It was the
// 5 s above: this match then started offline, and the friend, back a moment later, held its own
// start for 3 minutes for a headset that was no longer waiting.
constexpr uint64_t kHoldQuietMs = 20000;

uint32_t frame_seed(uint32_t seed, uint32_t frame) {
  uint32_t x = seed ^ (frame * 0x9E3779B9u);
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x != 0 ? x : 1;
}

void end_online(const char* why) {
  if (!st.online && !st.startPending) return;
  NLOG("net: online match over (%s) after %u frames, %u/%u state checks differed, %u stalls", why, st.frame,
       st.desyncs, st.checks, st.stalls);
  st.online = false;
  st.startPending = false;
  st.stepped = false;
  st.localReady = false;
  st.startReported = false;
  st.frontEndFrames = 0;
  std::memset(st.remoteTag, 0, sizeof(st.remoteTag));
  std::memset(st.myHashTag, 0, sizeof(st.myHashTag));
  std::memset(st.peerHashTag, 0, sizeof(st.peerHashTag));
  PortNetSetFixedStep(0);
}

void compare_hash(uint32_t f) {
  const uint32_t i = f % kRing;
  if (st.myHashTag[i] != f + 1 || st.peerHashTag[i] != f + 1) return;
  ++st.checks;
  if (st.myHash[i] != st.peerHash[i]) {
    ++st.desyncs;
    if (st.desyncs <= 5 || st.desyncs % 20 == 0) {
      NLOG("net: the matches differ at frame %u (%08x here, %08x there), %u times so far", f, st.myHash[i],
           st.peerHash[i], st.desyncs);
    }
  }
}

// The host has both choices: home = the host's pick, away = the guest's, the host's stadium and
// settings, a fresh seed, and an input delay that covers the trip through the broker.
void host_start() {
  st.isHost = true;
  std::random_device rd;
  st.seed = (rd() ^ static_cast<uint32_t>(now_ms())) | 1u;
  // One way is about half the round trip; a frame is 16.7 ms; three frames of slack for jitter and
  // the inputs going out in pairs.
  st.delay = std::clamp(static_cast<int>((st.rttMs / 2 + 16) / 17) + 3, 5, 20);
  Writer w = header(kStart);
  w.u64(st.myId);
  w.u64(st.peerId);
  w.u32(st.seed);
  w.u8(static_cast<uint8_t>(st.delay));
  write_choice(w, st.localChoice);
  write_choice(w, st.peerChoice);
  st.startMsg = w.v;
  st.match = PortNetMatch{};
  st.match.teams[0] = st.localChoice.team;
  st.match.teams[1] = st.peerChoice.team;
  st.match.sidekicks[0] = st.localChoice.sidekick;
  st.match.sidekicks[1] = st.peerChoice.sidekick;
  st.match.stadium = st.localChoice.stadium;
  st.match.optionsSize = st.localChoice.optionsSize;
  std::memcpy(st.match.options, st.localChoice.options, sizeof(st.match.options));
  st.frame = 0;  // the friend's first inputs may arrive before this headset's frame 0
  std::memset(st.remoteTag, 0, sizeof(st.remoteTag));
  st.lastRemoteMs = 0;
  st.startPending = true;
  send(st.startMsg);
  st.lastStartSendMs = now_ms();
  NLOG("net: starting the online match as home (round trip %u ms, input delay %d frames)", st.rttMs, st.delay);
}

void handle(const std::vector<uint8_t>& msg) {
  Reader r{msg};
  const uint8_t type = r.u8();
  const uint8_t proto = r.u8();
  const uint64_t sender = r.u64();
  if (!r.ok || sender == st.myId) return;
  const uint64_t now = now_ms();
  if (proto != kProtocol) {
    static uint64_t warned = 0;
    // r39b: both numbers, and remembered for the match start, which says why it goes offline.
    if (warned != sender) {
      NLOG("net: someone in the room runs another version of the game (online protocol %u there, %u here); "
           "the two cannot play each other", static_cast<unsigned>(proto), static_cast<unsigned>(kProtocol));
    }
    warned = sender;
    st.otherBuildMs = now;
    st.otherBuildProtocol = proto;
    return;
  }
  // The friend: whoever else is here; a newcomer replaces one gone quiet.
  if (st.peerId != sender) {
    if (peer_present()) return;  // a third headset: ignored
    st.peerId = sender;
    st.peerReady = false;
    st.peerLeftHoldMs = 0;
    st.rttCount = st.rttNext = 0;  // r39b: this friend's round trips only
    st.rttMs = 200;
    NLOG("net: friend in the room");
  }
  st.peerSeenMs = now;
  switch (type) {
  case kHello: {
    const bool ready = r.u8() != 0;
    const Choice c = read_choice(r);
    if (!r.ok) return;
    if (ready && !st.peerReady) NLOG("net: friend started a Friendly, waiting at the loading screen");
    // r39b: the friend stopped waiting while this headset waits for them (see PortNetHoldMatchStart).
    if (ready) {
      st.peerLeftHoldMs = 0;
    } else if (st.peerReady && st.localReady && !st.online && !st.startPending) {
      st.peerLeftHoldMs = now;
    }
    st.peerReady = ready;
    st.peerHelloMs = now;
    if (ready) st.peerChoice = c;
    break;
  }
  case kPing: {
    const uint32_t t = r.u32();
    Writer w = header(kPong);
    w.u64(sender);
    w.u32(t);
    send(std::move(w.v));
    break;
  }
  case kPong: {
    const uint64_t to = r.u64();
    const uint32_t t = r.u32();
    if (!r.ok || to != st.myId) return;
    const uint32_t rtt = static_cast<uint32_t>(now) - t;
    if (rtt >= 5000) break;
    // strikers-quest r39b: the median of the last few round trips, not a running average. A ping is
    // sent and answered between game frames, so one that waited out a load or a pause on either
    // headset measured that too, and that one sample could set the whole match's input delay to 20
    // frames. Only the host's delay goes out (in the start), so either build still takes the other's.
    st.rttSamples[st.rttNext] = rtt;
    st.rttNext = (st.rttNext + 1) % kRttSamples;
    st.rttCount = std::min(st.rttCount + 1, kRttSamples);
    uint32_t sorted[kRttSamples];
    std::copy(st.rttSamples, st.rttSamples + st.rttCount, sorted);
    std::sort(sorted, sorted + st.rttCount);
    st.rttMs = sorted[(st.rttCount - 1) / 2];
    break;
  }
  case kStart: {
    const uint64_t host = r.u64();
    const uint64_t guest = r.u64();
    const uint32_t seed = r.u32();
    const int delay = r.u8();
    const Choice home = read_choice(r);
    const Choice away = read_choice(r);
    if (!r.ok || guest != st.myId || host != sender) return;
    if (st.online || st.startPending || !st.localReady) return;  // already going, or not at the start
    st.isHost = false;
    st.seed = seed;
    st.delay = std::clamp(delay, 1, 60);
    st.match = PortNetMatch{};
    st.match.teams[0] = home.team;
    st.match.teams[1] = away.team;
    st.match.sidekicks[0] = home.sidekick;
    st.match.sidekicks[1] = away.sidekick;
    st.match.stadium = home.stadium;
    st.match.optionsSize = home.optionsSize;
    std::memcpy(st.match.options, home.options, sizeof(st.match.options));
    st.frame = 0;  // the host's first inputs may arrive before this headset's frame 0
    std::memset(st.remoteTag, 0, sizeof(st.remoteTag));
    st.lastRemoteMs = 0;
    st.startPending = true;
    NLOG("net: starting the online match as away (input delay %d frames)", st.delay);
    break;
  }
  case kInput: {
    const uint32_t first = r.u32();
    const uint32_t count = r.u8();
    if (!r.ok || !(st.online || st.startPending)) return;
    for (uint32_t k = 0; k < count; ++k) {
      NetPad p;
      r.bytes(&p, sizeof(p));
      if (!r.ok) return;
      const uint32_t f = first + k;
      if (f < st.frame || f >= st.frame + kRing) continue;
      st.remoteBuf[f % kRing] = p;
      st.remoteTag[f % kRing] = f + 1;
    }
    st.lastRemoteMs = now;
    break;
  }
  case kHash: {
    const uint32_t f = r.u32();
    const uint32_t h = r.u32();
    if (!r.ok || !st.online) return;
    st.peerHash[f % kRing] = h;
    st.peerHashTag[f % kRing] = f + 1;
    compare_hash(f);
    break;
  }
  default:
    break;
  }
}

void begin_online() {
  st.startPending = false;
  st.online = true;
  st.onlineSinceMs = now_ms();
  st.lastProgressMs = st.onlineSinceMs;
  st.frame = 0;
  st.stepped = false;
  st.localNext = static_cast<uint32_t>(st.delay);  // the first `delay` frames are neutral on both
  st.stalls = 0;
  st.desyncs = st.checks = 0;
  st.startReported = false;
  st.frontEndFrames = 0;
  PortNetSetFixedStep(1);
  NLOG("net: online, frame 0");
}

}  // namespace

void poll() {
  // The room: the launcher's pick (STRIKERS_ROOM), or a later one from nativeSetRoom.
  std::string room;
  {
    std::lock_guard<std::mutex> lock(sh.m);
    if (!sh.roomSet) {
      sh.room = sanitize_room(std::getenv("STRIKERS_ROOM"));
      sh.roomSet = true;
    }
    room = sh.room;
  }
  // strikers-quest r39b: a new pick waits while an online match runs or starts. Before, only the
  // game waited; the connection switched at once and the match froze on both headsets until its
  // watchdog. Not while this headset only waits at the loading screen: a pick there is the way out
  // of the wait (the switch below ends it, and that Friendly starts offline).
  const bool busy = st.online || st.startPending;
  if (room != st.pickedRoom) {
    st.pickedRoom = room;
    if (busy && room != st.room) {
      NLOG("net: room %s picked, it takes effect after this match", room.empty() ? "(none)" : room.c_str());
    }
  }
  if (room != st.room && !busy) {
    // A different room (or none): forget the old room's friend and any pending start.
    if (!room.empty()) NLOG("net: room %s", room.c_str());
    st.room = room;
    st.peerId = 0;
    st.peerReady = false;
    st.peerLeftHoldMs = 0;
    st.localReady = false;
    st.gaveUp = false;
    // r39b: and anything still queued from or for the old room; the connection follows from here
    // (net_thread drops what the old link still brings in after this).
    std::lock_guard<std::mutex> lock(sh.m);
    sh.activeRoom = room;
    sh.in.clear();
    sh.out.clear();
  }
  if (!st.threadStarted) {
    if (st.room.empty() || st.netBroken) return;  // offline: nothing on the network at all
    if (!net_bridge_init()) {
      st.netBroken = true;
      NLOG("net: no Java bridge for the room connection, staying offline");
      return;
    }
    st.threadStarted = true;
    std::random_device rd;
    st.myId = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ now_ms();
    sh.running = true;
    std::thread(net_thread, st.myId).detach();
  }
  const bool connected = sh.connected.load() && !st.room.empty();
  if (connected != st.wasConnected) {
    st.wasConnected = connected;
    if (!connected) NLOG("net: lost the room, reconnecting");
  }
  std::deque<std::vector<uint8_t>> in;
  {
    std::lock_guard<std::mutex> lock(sh.m);
    in.swap(sh.in);
  }
  for (const auto& msg : in) handle(msg);

  const uint64_t now = now_ms();
  // r39 watchdogs, with or without a link: the friend's input stopped coming (they left), or it
  // keeps coming but neither side can run a frame (a deadlock: the r38 timeout only looked at the
  // first, and resends kept it quiet while both headsets stood still).
  if (st.online) {
    // strikers-quest r39b: 30 s, not 10, in the frames around the match load. Both headsets load the
    // whole match in one blocking frame (online frame 1); while the friend's load runs longer this
    // headset hears nothing and runs nothing, just as if the friend had gone. (This headset's own
    // long frame no longer counts at all: PortNetFrameEnd marks the progress when a frame ends.)
    const bool matchLoad = st.frame < static_cast<uint32_t>(st.delay) + 10;
    const uint64_t quietMs = matchLoad ? 30000 : 10000;
    if (st.lastRemoteMs == 0) {
      if (now - st.onlineSinceMs > 30000) end_online("the friend never sent input");
    } else if (now - st.lastRemoteMs > quietMs) {
      end_online("friend stopped answering");
    } else if (now - st.lastProgressMs > quietMs) {
      end_online(matchLoad ? "no game frame for 30 s at the match start" : "no game frame for 10 s");
    }
  }
  if (!connected) return;
  if (!st.online && now - st.lastHelloMs > 500) send_hello();
  if (!st.online && now - st.lastPingMs > 1000) {
    Writer w = header(kPing);
    w.u32(static_cast<uint32_t>(now));
    send(std::move(w.v));
    st.lastPingMs = now;
  }
  // Both at the loading screen: the lower id hosts (home side). r39b: on a recent hello (they come
  // every 0.5 s); the hold now waits out a quiet friend, whose last ready bit may be an old one.
  if (st.localReady && st.peerReady && peer_present() && now - st.peerHelloMs < 2000 && !st.online &&
      !st.startPending && st.myId < st.peerId) {
    host_start();
  }
  // The host repeats the start until the friend's input shows up.
  if (st.isHost && (st.online || st.startPending) && st.lastRemoteMs == 0 && now - st.lastStartSendMs > 300) {
    send(st.startMsg);
    st.lastStartSendMs = now;
  }
  if (st.online) {
    // Every 5 s: how the match keeps up (frames run, time spent waiting for the friend's input).
    static uint64_t lastStatusMs = 0, waitedMs = 0, lastPollMs = 0;
    static uint32_t lastStatusFrame = 0, lastStatusStalls = 0;
    if (st.stallSinceMs != 0 && lastPollMs != 0) waitedMs += now - lastPollMs;
    lastPollMs = now;
    if (lastStatusMs == 0 || st.frame < lastStatusFrame) {
      lastStatusMs = now;
      lastStatusFrame = st.frame;
      lastStatusStalls = st.stalls;
      waitedMs = 0;
    } else if (now - lastStatusMs >= 5000) {
      NLOG("net: %u frames in %.1f s (waited %llu ms for the friend, %u stalls), their input %llu ms old, "
           "delay %d", st.frame - lastStatusFrame, static_cast<double>(now - lastStatusMs) / 1000.0,
           static_cast<unsigned long long>(waitedMs), st.stalls - lastStatusStalls,
           static_cast<unsigned long long>(st.lastRemoteMs != 0 ? now - st.lastRemoteMs : 0), st.delay);
      lastStatusMs = now;
      lastStatusFrame = st.frame;
      lastStatusStalls = st.stalls;
      waitedMs = 0;
    }
    // Keep the latest input flowing while waiting (a lost message would otherwise stall both).
    if (now - st.lastSendMs > 100) send_inputs();
  }
}

bool lockstep() { return st.online || st.startPending; }

bool step(const NetPad& local, NetPad* pad0, NetPad* pad1) {
  if (st.startPending) begin_online();
  st.stepped = false;
  if (!st.online) return false;
  bool fresh = false;
  while (st.localNext <= st.frame + static_cast<uint32_t>(st.delay)) {
    st.localBuf[st.localNext % kRing] = local;
    ++st.localNext;
    fresh = true;
  }
  // Two frames a message (30 a second): gentler on the free broker; the input delay has the slack.
  if (fresh && ((st.localNext & 1) == 0 || now_ms() - st.lastSendMs > 30)) send_inputs();
  NetPad remote{};
  if (st.frame >= static_cast<uint32_t>(st.delay)) {
    const uint32_t i = st.frame % kRing;
    if (st.remoteTag[i] != st.frame + 1) {
      if (st.stallSinceMs == 0) {
        st.stallSinceMs = now_ms();
        ++st.stalls;
      }
      return false;
    }
    remote = st.remoteBuf[i];
  }
  st.stallSinceMs = 0;
  const NetPad mine = st.frame >= static_cast<uint32_t>(st.delay) ? st.localBuf[st.frame % kRing] : NetPad{};
  *pad0 = st.isHost ? mine : remote;
  *pad1 = st.isHost ? remote : mine;
  // strikers-quest r39b: handed out, not used up. PortNetCommitFrame (main.cpp) moves on to the next
  // frame once the game frame really runs. Aurora can still refuse it (aurora_begin_frame: surface
  // not ready, a staging buffer that would not map); then the next step() hands out this same frame,
  // where before its inputs and seed were gone and this headset ran one frame off for the rest of
  // the match.
  st.stepped = true;
  return true;
}

int local_pad() { return (st.online || st.startPending) && !st.isHost ? 1 : 0; }

}  // namespace xrnet

// ---------------------------------------------------------------------------------------------
// The game's side (include/port/xr_net.h)
// ---------------------------------------------------------------------------------------------

using xrnet::st;

extern "C" int PortNetHoldMatchStart(const PortNetMatch* localChoice, int localSide) {
  const uint64_t now = xrnet::now_ms();
  if (now - st.lastHoldMs > 2000) {  // a new match start
    st.gaveUp = false;
    st.otherBuildLogged = false;
  }
  st.lastHoldMs = now;
  if (st.online) return 0;
  if (st.startPending) return 1;
  if (st.gaveUp || !st.threadStarted) return 0;
  // r39b: once waiting, a friend gone quiet is still waited for up to kHoldQuietMs (was 5 s).
  if (!(st.localReady ? xrnet::peer_heard(xrnet::kHoldQuietMs) : xrnet::peer_present())) {
    if (st.localReady) {
      st.localReady = false;
      NLOG("net: nothing from the friend for 20 s, this match is offline");
    } else if (st.otherBuildMs != 0 && now - st.otherBuildMs < 5000 && !st.otherBuildLogged) {
      // strikers-quest r39b: the reason, where a player would otherwise only see a CPU opponent.
      st.otherBuildLogged = true;
      NLOG("net: this match is offline: the other headset in the room runs another version of the game "
           "(online protocol %u there, %u here), both need the same one",
           static_cast<unsigned>(st.otherBuildProtocol), static_cast<unsigned>(xrnet::kProtocol));
    }
    return 0;
  }
  if (!st.localReady) {
    const int side = localSide == 1 ? 1 : 0;
    st.localChoice = xrnet::Choice{};
    st.localChoice.team = localChoice->teams[side];
    st.localChoice.sidekick = localChoice->sidekicks[side];
    st.localChoice.stadium = localChoice->stadium;
    st.localChoice.optionsSize = std::clamp(localChoice->optionsSize, 0, 64);
    std::memcpy(st.localChoice.options, localChoice->options, sizeof(st.localChoice.options));
    st.localReady = true;
    st.readySinceMs = now;
    st.peerLeftHoldMs = 0;
    xrnet::send_hello();
    NLOG("net: Friendly started with the friend in the room: waiting for them to start one too");
  }
  // strikers-quest r39b: the friend was waiting too and stopped (it gave up, or its game went ahead
  // offline: an r39a headset does that after 5 s without hearing this one). Its hellos have said "not
  // ready" for 2 s; the 20 s wait above would otherwise keep this headset here for 3 minutes. A friend
  // starting the match as host says "not ready" for a frame at most, and its start arrives first.
  if (st.peerLeftHoldMs != 0 && !st.peerReady && st.peerHelloMs - st.peerLeftHoldMs >= 2000) {
    st.gaveUp = true;
    st.localReady = false;
    st.peerLeftHoldMs = 0;
    NLOG("net: the friend stopped waiting for this match, it is offline");
    return 0;
  }
  if (now - st.readySinceMs > 180000) {
    st.gaveUp = true;
    st.localReady = false;
    xrnet::send_hello();
    NLOG("net: the friend did not start a match within 3 minutes, this one is offline");
    return 0;
  }
  return 1;
}

extern "C" int PortNetFrameBegin(unsigned int* seed, PortNetMatch* startMatch, int* isStartFrame) {
  *isStartFrame = 0;
  if (!st.online || st.frame == 0) return 0;
  *seed = xrnet::frame_seed(st.seed, st.frame - 1);
  if (!st.startReported) {
    st.startReported = true;
    *startMatch = st.match;
    *isStartFrame = 1;
  }
  return 1;
}

// strikers-quest r39b: main.cpp, once the game frame really runs (aurora_begin_frame took it): the
// frame step() handed out is used up only now, so a refused frame comes back with the same inputs.
extern "C" void PortNetCommitFrame(void) {
  if (!st.online || !st.stepped) return;
  st.stepped = false;
  ++st.frame;
  st.lastProgressMs = xrnet::now_ms();
}

extern "C" void PortNetFrameEnd(unsigned int stateHash, int inFrontEnd) {
  if (!st.online || st.frame == 0) return;
  // r39b: progress also counts from when a frame ends, so this headset's own long frame (the whole
  // match loads in online frame 1) is not taken for a stall by the watchdog in poll().
  st.lastProgressMs = xrnet::now_ms();
  const uint32_t f = st.frame - 1;
  if (f % 300 == 0) NLOG("net: frame %u state %08x", f, stateHash);  // two runs compare line by line
  if (f % 30 == 0) {
    st.myHash[f % xrnet::kRing] = stateHash;
    st.myHashTag[f % xrnet::kRing] = f + 1;
    xrnet::Writer w = xrnet::header(xrnet::kHash);
    w.u32(f);
    w.u32(stateHash);
    xrnet::send(std::move(w.v));
    xrnet::compare_hash(f);
  }
  // Back in the menus after the match (both headsets reach it on the same frame).
  if (inFrontEnd && f > 120) {
    if (++st.frontEndFrames > 30) xrnet::end_online("back in the menus");
  } else {
    st.frontEndFrames = 0;
  }
}

extern "C" int PortNetLocalPad(void) { return xrnet::local_pad(); }

extern "C" int PortNetOnline(void) { return st.online ? 1 : 0; }

// r39: the room code from the launcher panel when the game was already running (StrikersActivity
// .onNewIntent). Takes effect between matches; "" is offline. r39b: only the pick is set here; poll()
// moves the game and the connection to it (sh.activeRoom) once no match needs the old room.
#if defined(XRNET_ANDROID)
extern "C" JNIEXPORT void JNICALL Java_io_github_newcoke_strikers_StrikersActivity_nativeSetRoom(JNIEnv* env, jclass,
                                                                                               jstring jroom) {
  const char* s = jroom != nullptr ? env->GetStringUTFChars(jroom, nullptr) : nullptr;
  const std::string room = xrnet::sanitize_room(s);
  if (s != nullptr) env->ReleaseStringUTFChars(jroom, s);
  std::lock_guard<std::mutex> lock(xrnet::sh.m);
  xrnet::sh.room = room;
  xrnet::sh.roomSet = true;
}
#else
// visionOS: the launcher's pick (sv_bridge.h), before the game starts or between matches, the same
// role as nativeSetRoom. viaSharePlay routes the room through the SharePlay group instead of the
// relay; the code then only names the match inside that group.
extern "C" void sv_net_set_room(const char* room, int viaSharePlay) {
  std::string r = xrnet::sanitize_room(room);
  if (!r.empty() && viaSharePlay != 0) r.insert(r.begin(), xrnet::kSharePlayMark);
  std::lock_guard<std::mutex> lock(xrnet::sh.m);
  xrnet::sh.room = r;
  xrnet::sh.roomSet = true;
}
#endif

#else  // no online transport on this platform

namespace xrnet {
void poll() {}
bool lockstep() { return false; }
bool step(const NetPad&, NetPad*, NetPad*) { return false; }
int local_pad() { return 0; }
}  // namespace xrnet

extern "C" int PortNetHoldMatchStart(const PortNetMatch*, int) { return 0; }
extern "C" int PortNetFrameBegin(unsigned int*, PortNetMatch*, int* isStartFrame) {
  *isStartFrame = 0;
  return 0;
}
extern "C" void PortNetCommitFrame(void) {}
extern "C" void PortNetFrameEnd(unsigned int, int) {}
extern "C" int PortNetLocalPad(void) { return 0; }
extern "C" int PortNetOnline(void) { return 0; }

#endif
