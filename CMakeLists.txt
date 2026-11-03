cmake_minimum_required(VERSION 3.22)
project(hexed_discord_rpc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Discord application id used when Lua passes "" (same one the APK used). Override: -DHX_APP_ID=...
set(HX_APP_ID "1566671569479147600" CACHE STRING "Discord application id")

# ---- core: Discord Social SDK + presence. Loaded by HxBridge, NOT by AML ----------------------
add_library(hxrpc_core SHARED src/core.cpp)
target_include_directories(hxrpc_core PRIVATE third_party/include src)
target_link_libraries(hxrpc_core PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/lib/${ANDROID_ABI}/libdiscord_partner_sdk.so
    log)
target_link_options(hxrpc_core PRIVATE -Wl,--gc-sections)

# ---- stub: the AML mod (libDiscord_RPC.so). No dependency on the SDK ---------------------------
add_library(Discord_RPC SHARED src/stub.cpp)
target_include_directories(Discord_RPC PRIVATE third_party/aml src)
target_compile_definitions(Discord_RPC PRIVATE HX_DEFAULT_APP_ID=${HX_APP_ID}ULL)
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_definitions(Discord_RPC PRIVATE AML32)
endif()
target_link_libraries(Discord_RPC PRIVATE log dl)
target_link_options(Discord_RPC PRIVATE -Wl,--gc-sections)
