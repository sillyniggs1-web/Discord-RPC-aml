// Function table shared by the stub (libDiscord_RPC.so) and the core (libhxrpc_core.so).
#pragma once
#define HX_API_VERSION 1
struct HxApi {
    int  version;
    int  (*start)(unsigned long long appId, const char* dir);
    void (*stop)();
    void (*setState)(const char* nick, int id, const char* server, const char* ip, int port, int players, long long start);
    void (*clear)();
    void (*login)();
    void (*logout)();
    int  (*status)(char* out, int cap);
    int  (*flags)();   // bit0 ready, bit1 authed, bit2 saved login, bit3 running
};
