// Hexed Discord RPC - AML mod (libDiscord_RPC.so)
// This is the file AML loads. It stays small on purpose and has no link dependency on the Discord SDK:
//   1. copies the support files (core, SDK, dex) from <mods>/Discord_RPC/ into the app's private dir
//   2. builds a DexClassLoader with the SDK's Java classes + HxBridge
//   3. HxBridge gives the SDK its Activity and System.load()s the core through that loader
//   4. fetches the core's function table and exposes it to Lua over plain C (FFI)

#include "mod/amlmod.h"
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include "api.h"

#ifndef HX_DEFAULT_APP_ID
#define HX_DEFAULT_APP_ID 0ULL
#endif

#define TAG "HexedRPC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

MYMOD(net.hexedstudio.discordrpc, Hexed Discord RPC, 1.0.0, HEXED Studio)

namespace {

std::mutex g_mu;
bool g_tried = false;
HxApi* g_api = nullptr;
char g_err[256] = "not started";
std::string g_work;   // <filesDir>/hxrpc, also where the saved Discord login lives

void setErr(const std::string& s) {
    std::snprintf(g_err, sizeof(g_err), "%s", s.c_str());
    LOGE("%s", g_err);
}

std::string modDir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&modDir), &info) || !info.dli_fname) return ".";
    std::string p = info.dli_fname;
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool copyFile(const std::string& from, const std::string& to, mode_t mode) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    std::fseek(in, 0, SEEK_END);
    long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    struct stat st{};
    if (stat(to.c_str(), &st) == 0 && st.st_size == size) { std::fclose(in); return true; } // already current
    unlink(to.c_str());   // may be read-only
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    char buf[1 << 15];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) std::fwrite(buf, 1, n, out);
    std::fclose(in);
    std::fclose(out);
    chmod(to.c_str(), mode);
    return true;
}

// description of a pending Java exception, then clear it
std::string takeException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return "";
    jthrowable t = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string out = "Java exception";
    jclass tc = env->FindClass("java/lang/Throwable");
    jmethodID ts = env->GetMethodID(tc, "toString", "()Ljava/lang/String;");
    if (ts && t) {
        jstring js = (jstring)env->CallObjectMethod(t, ts);
        if (js) {
            const char* c = env->GetStringUTFChars(js, nullptr);
            if (c) { out = c; env->ReleaseStringUTFChars(js, c); }
        }
    }
    env->ExceptionClear();
    return out;
}

std::string javaString(JNIEnv* env, jobject obj, const char* method) {
    jclass c = env->GetObjectClass(obj);
    jmethodID m = env->GetMethodID(c, method, "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); return ""; }
    jstring s = (jstring)env->CallObjectMethod(obj, m);
    if (!s) return "";
    const char* c8 = env->GetStringUTFChars(s, nullptr);
    std::string r = c8 ? c8 : "";
    if (c8) env->ReleaseStringUTFChars(s, c8);
    return r;
}

bool bootstrap() {
    JavaVM* vm = aml->GetJavaVM();
    if (!vm) { setErr("AML gave no JavaVM"); return false; }
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { setErr("AttachCurrentThread failed"); return false; }
    }
    jobject ctx = aml->GetCurrentContext();
    if (!ctx) { setErr("no app context from AML"); return false; }

    // --- private working dir: <filesDir>/hxrpc
    jclass ctxCls = env->GetObjectClass(ctx);
    jmethodID getFiles = env->GetMethodID(ctxCls, "getFilesDir", "()Ljava/io/File;");
    jobject filesDir = env->CallObjectMethod(ctx, getFiles);
    if (!filesDir) { setErr("getFilesDir failed"); return false; }
    const std::string work = javaString(env, filesDir, "getAbsolutePath") + "/hxrpc";
    g_work = work;
    mkdir(work.c_str(), 0700);
    mkdir((work + "/odex").c_str(), 0700);

    // --- copy support files out of the mods folder
    const std::string src = modDir() + "/Discord_RPC/";
    const char* libs[] = { "libdiscord_partner_sdk.so", "libhxrpc_core.so" };
    for (const char* f : libs)
        if (!copyFile(src + f, work + "/" + f, 0755)) { setErr(std::string("missing ") + src + f); return false; }
    if (!copyFile(src + "hx_dex.jar", work + "/hx_dex.jar", 0444)) { setErr("missing " + src + "hx_dex.jar"); return false; }

    // --- DexClassLoader(dexPath, odexDir, libDir, parent = the game's loader)
    jmethodID getLoader = env->GetMethodID(ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallObjectMethod(ctx, getLoader);
    jclass dcl = env->FindClass("dalvik/system/DexClassLoader");
    jmethodID dclInit = env->GetMethodID(dcl, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    jstring jDex = env->NewStringUTF((work + "/hx_dex.jar").c_str());
    jstring jOpt = env->NewStringUTF((work + "/odex").c_str());
    jstring jLib = env->NewStringUTF(work.c_str());
    jobject loader = env->NewObject(dcl, dclInit, jDex, jOpt, jLib, parent);
    if (!loader) { setErr("DexClassLoader: " + takeException(env)); return false; }

    jmethodID loadClass = env->GetMethodID(dcl, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring jName = env->NewStringUTF("com.hexed.rpc.HxBridge");
    jclass bridge = (jclass)env->CallObjectMethod(loader, loadClass, jName);
    if (!bridge) { setErr("loadClass HxBridge: " + takeException(env)); return false; }
    bridge = (jclass)env->NewGlobalRef(bridge);

    // --- HxBridge.init(ctx, corePath)
    jmethodID init = env->GetStaticMethodID(bridge, "init", "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;");
    jstring jCore = env->NewStringUTF((work + "/libhxrpc_core.so").c_str());
    jstring res = (jstring)env->CallStaticObjectMethod(bridge, init, ctx, jCore);
    if (env->ExceptionCheck()) { setErr("HxBridge.init: " + takeException(env)); return false; }
    if (res) {
        const char* c = env->GetStringUTFChars(res, nullptr);
        setErr(std::string("HxBridge.init: ") + (c ? c : "?"));
        if (c) env->ReleaseStringUTFChars(res, c);
        return false;
    }

    // --- HxBridge.apiTable() -> function table inside the core
    jmethodID tbl = env->GetStaticMethodID(bridge, "apiTable", "()J");
    jlong addr = tbl ? env->CallStaticLongMethod(bridge, tbl) : 0;
    if (env->ExceptionCheck()) { setErr("apiTable: " + takeException(env)); return false; }
    g_api = reinterpret_cast<HxApi*>(static_cast<intptr_t>(addr));
    if (!g_api || g_api->version != HX_API_VERSION) { g_api = nullptr; setErr("core API mismatch"); return false; }

    std::snprintf(g_err, sizeof(g_err), "ok");
    LOGI("bootstrap ok, work dir %s", work.c_str());
    return true;
}

HxApi* api() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_api;
}

} // namespace

extern "C" {

// ---- exported for Lua (ffi.load) -------------------------------------------------------------
// returns 1 on success; on failure hx_rpc_error() says why. appId: decimal string, "" = built-in default.
__attribute__((visibility("default"))) int hx_rpc_start(const char* appId) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_tried) {
            g_tried = true;
            if (!bootstrap()) return 0;
        }
        if (!g_api) return 0;
    }
    unsigned long long id = (appId && *appId) ? std::strtoull(appId, nullptr, 10) : HX_DEFAULT_APP_ID;
    if (id == 0) { setErr("no Discord application id"); return 0; }
    return g_api->start(id, g_work.c_str());
}
__attribute__((visibility("default"))) const char* hx_rpc_error() { return g_err; }
__attribute__((visibility("default"))) void hx_rpc_stop()  { if (auto* a = api()) a->stop(); }
__attribute__((visibility("default"))) void hx_rpc_clear() { if (auto* a = api()) a->clear(); }
__attribute__((visibility("default"))) void hx_rpc_login()  { if (auto* a = api()) a->login(); }
__attribute__((visibility("default"))) void hx_rpc_logout() { if (auto* a = api()) a->logout(); }
__attribute__((visibility("default"))) void hx_rpc_set_state(const char* nick, int id, const char* server,
                                                             const char* ip, int port, int players, long long start) {
    if (auto* a = api()) a->setState(nick, id, server, ip, port, players, start);
}
__attribute__((visibility("default"))) int hx_rpc_status(char* out, int cap) {
    if (auto* a = api()) return a->status(out, cap);
    if (out && cap > 0) std::snprintf(out, static_cast<size_t>(cap), "%s", g_err);
    return 0;
}
__attribute__((visibility("default"))) int hx_rpc_flags() {
    if (auto* a = api()) return a->flags();
    return 0;
}

} // extern "C"

ON_MOD_UNLOAD() {
    if (auto* a = api()) a->stop();
}
