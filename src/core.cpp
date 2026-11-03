// Hexed Discord RPC - core library (libhxrpc_core.so)
// Discord Social SDK + rich presence. Loaded through the SDK's DexClassLoader by HxBridge, so it
// shares one copy of libdiscord_partner_sdk.so (and its JNI state) with the SDK's Java classes.
// Ported from the SampDiscordRpc APK (rpc.cpp): same token handling, no UDP socket - the Lua script
// pushes the game state straight in through the stub (libDiscord_RPC.so).

#define DISCORDPP_IMPLEMENTATION // exactly one file
#include "discordpp.h"

#include <jni.h>
#include <android/log.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "api.h"

#define TAG "HexedRPC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_run{false};
std::thread g_thread;
std::mutex g_mu;              // guards g_status and g_state
std::string g_status = "Stopped";

std::atomic<bool> g_authed{false};       // a token was accepted and Connect() was called
std::atomic<bool> g_ready{false};        // SDK status is Ready
std::atomic<bool> g_loginReq{false};
std::atomic<bool> g_repush{false};
std::atomic<bool> g_needRefresh{false};
std::atomic<bool> g_refreshing{false};   // refresh tokens rotate: never run two requests at once
std::atomic<bool> g_authorizeReq{false}; // saved login is invalid: start the login flow
std::atomic<bool> g_logoutReq{false};
std::string g_tokenPath;

struct State {
    bool active = false;
    std::string nick, server, ip;
    int port = 7777;
    int players = 0;
    long long start = 0;
};
State g_state;
std::atomic<long long> g_lastSetMs{0};   // steady-clock ms of the last state pushed by Lua

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_status = s;
    LOGI("%s", s.c_str());
}

std::string clip(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string readRefresh() {
    std::string out;
    if (FILE* f = std::fopen(g_tokenPath.c_str(), "r")) {
        char b[1024];
        while (std::fgets(b, sizeof(b), f)) out += b;
        std::fclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

void writeRefresh(const std::string& token) {
    // temp file + rename: a kill in the middle must never lose the (rotated) token
    const std::string tmp = g_tokenPath + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        const bool ok = std::fputs(token.c_str(), f) >= 0;
        std::fclose(f);
        chmod(tmp.c_str(), 0600);
        if (ok) std::rename(tmp.c_str(), g_tokenPath.c_str());
    }
}

void eraseRefresh() {
    std::remove(g_tokenPath.c_str());
    std::remove((g_tokenPath + ".tmp").c_str());
}

void connectWith(const std::shared_ptr<discordpp::Client>& client,
                 discordpp::AuthorizationTokenType type, const std::string& access) {
    client->UpdateToken(type, access, [client](discordpp::ClientResult r) {
        if (!r.Successful()) {
            setStatus("Token update failed, retrying: " + std::string(r.Error()));
            g_needRefresh = true;
            return;
        }
        g_authed = true;
        if (client->GetStatus() == discordpp::Client::Status::Disconnected) {
            setStatus("Connecting to Discord...");
            client->Connect();
        }
    });
}

void tryRefresh(const std::shared_ptr<discordpp::Client>& client, uint64_t appId, bool loginIfInvalid) {
    const std::string rt = readRefresh();
    if (rt.empty()) return;
    if (g_refreshing.exchange(true)) return;
    setStatus("Restoring saved Discord login...");
    client->RefreshToken(appId, rt, [client, rt, loginIfInvalid](discordpp::ClientResult r, std::string access,
                                             std::string refresh, discordpp::AuthorizationTokenType type,
                                             int32_t, std::string) {
        g_refreshing = false;
        if (!r.Successful()) {
            const int http = static_cast<int>(r.Status());
            if (http == 400 || http == 401 || http == 403) {
                if (readRefresh() == rt) eraseRefresh();
                g_authed = false;
                setStatus("Saved login expired - log in again");
                if (loginIfInvalid) g_authorizeReq = true;
            } else {
                setStatus("Discord unreachable, retrying: " + std::string(r.Error()));
                g_needRefresh = true;
            }
            return;
        }
        if (!refresh.empty()) writeRefresh(refresh);
        connectWith(client, type, access);
    });
}

// Device-code login: no redirect URI and no manifest entry needed, so it works from inside another
// app's process. The application must be a Public Client in the developer portal.
void startLogin(const std::shared_ptr<discordpp::Client>& client, uint64_t appId) {
    discordpp::DeviceAuthorizationArgs args;
    args.SetClientId(appId);
    args.SetScopes(discordpp::Client::GetDefaultPresenceScopes());
    setStatus("Starting Discord device login...");
    client->GetTokenFromDevice(std::move(args), [client](discordpp::ClientResult r, std::string access,
                                                        std::string refresh,
                                                        discordpp::AuthorizationTokenType type,
                                                        int32_t, std::string) {
        if (!r.Successful()) {
            setStatus("Device login failed: " + std::string(r.Error()));
            return;
        }
        if (!refresh.empty()) writeRefresh(refresh);
        connectWith(client, type, access);
    });
}

void push(const std::shared_ptr<discordpp::Client>& client, const State& s) {
    if (!s.active) {
        client->ClearRichPresence();
        setStatus("Presence cleared (not in a server)");
        return;
    }
    std::string details = s.server.empty() ? std::string("SA-MP server") : s.server;
    std::string who = s.nick.empty() ? std::string("player") : s.nick;
    std::string state = who + " \xE2\x80\xA2 " + s.ip + ":" + std::to_string(s.port);

    discordpp::Activity a;
    a.SetType(discordpp::ActivityTypes::Playing);
    a.SetName("San Andreas Multiplayer");
    a.SetDetails(clip(details, 120));
    a.SetState(clip(state, 120));
    if (s.start > 0) {
        discordpp::ActivityTimestamps ts;
        ts.SetStart(static_cast<uint64_t>(s.start));
        a.SetTimestamps(ts);
    }
    client->UpdateRichPresence(a, [](discordpp::ClientResult r) {
        if (r.Successful()) setStatus("Presence active");
        else setStatus("Presence failed: " + std::string(r.Error()));
    });
}

void worker(uint64_t appId) {
    using clock = std::chrono::steady_clock;
    using std::chrono::seconds;

    auto client = std::make_shared<discordpp::Client>();
    client->AddLogCallback([](auto message, auto) { LOGI("[sdk] %s", std::string(message).c_str()); },
                           discordpp::LoggingSeverity::Info);

    discordpp::Client* raw = client.get();
    client->SetStatusChangedCallback([raw](discordpp::Client::Status status, discordpp::Client::Error error, int32_t) {
        if (status == discordpp::Client::Status::Ready) {
            g_ready = true;
            g_repush = true;
            std::string name = "Discord";
            if (auto u = raw->GetCurrentUserV2()) name = u->DisplayName();
            setStatus("Logged in as " + name + " (login saved)");
        } else {
            g_ready = false;
            if (error != discordpp::Client::Error::None)
                setStatus("Discord connection error: " + discordpp::Client::ErrorToString(error));
            else
                setStatus("Discord: " + discordpp::Client::StatusToString(status));
            if (status == discordpp::Client::Status::Disconnected && g_authed && g_run) g_needRefresh = true;
        }
    });
    client->SetTokenExpirationCallback([]() { g_needRefresh = true; });

    client->SetApplicationId(appId);
    tryRefresh(client, appId, false);
    if (readRefresh().empty()) setStatus("Ready. Not logged in (local Discord app presence)");

    std::string lastKey;
    auto lastPush = clock::time_point{};
    auto nextRefresh = clock::time_point{};

    while (g_run) {
        if (g_logoutReq.exchange(false)) {
            g_authed = false; g_ready = false; g_needRefresh = false;
            eraseRefresh();
            client->Disconnect();
            setStatus("Logged out. Log in again to use your account");
        }
        if (g_authorizeReq.exchange(false)) startLogin(client, appId);
        if (g_loginReq.exchange(false)) {
            if (g_ready) setStatus("Already logged in (saved). Use Log out to switch account");
            else if (!readRefresh().empty()) tryRefresh(client, appId, true);
            else startLogin(client, appId);
        }
        if (g_needRefresh && clock::now() >= nextRefresh) {
            g_needRefresh = false;
            nextRefresh = clock::now() + seconds(15);
            tryRefresh(client, appId, false);
        }

        State cur;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            // the Lua side refreshes the state about every second; 10 s of silence means "left the server"
            if (g_state.active && nowMs() - g_lastSetMs.load() > 10000) g_state = State{};
            cur = g_state;
        }
        const auto now = clock::now();

        std::string key = cur.active
            ? cur.server + "|" + cur.ip + ":" + std::to_string(cur.port) + "|" + cur.nick + "|" + std::to_string(cur.start)
            : std::string();
        if (g_repush.exchange(false)) lastKey = "\x01force";
        const bool canPush = !g_authed || g_ready;
        if (canPush && (key != lastKey || (cur.active && now - lastPush > seconds(30)))) {
            push(client, cur);
            lastKey = key;
            lastPush = now;
        }
        discordpp::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client->ClearRichPresence();
    for (int i = 0; i < 5; ++i) {
        discordpp::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

static int c_start(unsigned long long appId, const char* dir) {
    if (g_run.exchange(true)) return 1;
    g_tokenPath = std::string(dir ? dir : ".") + "/discord_login.txt";
    g_authed = false; g_ready = false; g_needRefresh = false; g_refreshing = false; g_logoutReq = false;
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread(worker, static_cast<uint64_t>(appId));
    return 1;
}

static void c_stop() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    setStatus("Stopped");
}

static void c_set_state(const char* nick, int, const char* server, const char* ip, int port, int players, long long start) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_state.active = true;
    g_state.nick = nick ? nick : "";
    g_state.server = server ? server : "";
    g_state.ip = ip ? ip : "";
    g_state.port = port;
    g_state.players = players;
    g_state.start = start;
    g_lastSetMs = nowMs();
}

static void c_clear() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_state = State{};
}

static void c_login()  { g_loginReq = true; }
static void c_logout() { if (g_run) g_logoutReq = true; else { eraseRefresh(); } }

static int c_status(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_mu);
    std::snprintf(out, static_cast<size_t>(cap), "%s", g_status.c_str());
    return static_cast<int>(std::strlen(out));
}

static int c_flags() {
    int f = 0;
    if (g_ready) f |= 1;
    if (g_authed) f |= 2;
    if (!g_tokenPath.empty() && !readRefresh().empty()) f |= 4;
    if (g_run) f |= 8;
    return f;
}

static HxApi g_api = { HX_API_VERSION, c_start, c_stop, c_set_state, c_clear, c_login, c_logout, c_status, c_flags };

// Reached through HxBridge.apiTable() once the Java side has System.load()ed this library.
extern "C" JNIEXPORT jlong JNICALL Java_com_hexed_rpc_HxBridge_apiTable(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(&g_api));
}
