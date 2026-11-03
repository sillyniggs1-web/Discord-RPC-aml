# Hexed Discord RPC - native AML mod (v2.0)

Native replacement for the `SampDiscordRpc` APK + `Discord_RPC_Bridge.lua`.
Everything now runs inside the Nezuko process (MonetLoader + AML): no second app, no UDP.

```
SA-MP client -> Discord_RPC.lua (menu, game info) -> FFI -> libDiscord_RPC.so (AML mod)
                                                         -> core + Discord Social SDK -> Discord
```

## Build (GitHub)
1. Push this folder to a repo, open **Actions -> Build Discord_RPC AML mod -> Run workflow** (it also runs on push).
2. Download the `Discord_RPC_AML` artifact. It has one folder per ABI (Nezuko is `armeabi-v7a`).
3. Optional: `-DHX_APP_ID=<id>` in the workflow changes the built-in application id (default is the one from the APK).

## Install (armeabi-v7a folder)
```
<your AML mods folder>/libDiscord_RPC.so            <- the AML mod
<your AML mods folder>/Discord_RPC/                 <- support folder, keep next to the .so
        libhxrpc_core.so
        libdiscord_partner_sdk.so
        hx_dex.jar
<MonetLoader scripts folder>/Discord_RPC.lua
```
Delete the old `Discord_RPC_Bridge.lua`, otherwise both scripts will fight over the presence.

## Use
- `/drpc` opens the menu: **Log in with Discord**, **Log out**, enable toggle, live status.
- `/rpcdebug` prints the status in chat, `/rpctoggle` flips presence on/off.
- Logging in uses the SDK's **device-code login** (an APK-only redirect can't exist inside the game).
  In the Discord developer portal the application must be a **Public Client**.
  The login is saved (refresh token, atomic write) in the app's private `files/hxrpc/` folder and renewed automatically.
- Without a login it falls back to the local Discord app, same as the APK's "Start service" mode.

## How the pieces fit
| File | Role |
|---|---|
| `src/stub.cpp` -> `libDiscord_RPC.so` | The AML mod. Copies support files to private storage, builds a `DexClassLoader`, exposes plain C functions to Lua. |
| `java/.../HxBridge.java` -> `hx_dex.jar` | Gives the SDK the game's Activity, then `System.load()`s the core through the same class loader so the SDK's Java and native halves share one copy of the SDK. |
| `src/core.cpp` -> `libhxrpc_core.so` | `rpc.cpp` from the APK minus the UDP socket: SDK client, token storage/refresh, presence push. |
| `lua/Discord_RPC.lua` | Menu + SA-MP data, replaces the bridge script. |

## Not tested on a device yet
This was written without access to an Android toolchain or a device, so the first build/run is the real test. `core.cpp` was
syntax-checked against the real `discordpp.h`; the rest was not compiled. If something fails, `/rpcdebug` shows the stage, and
`adb logcat -s HexedRPC` has the full detail. The most likely trouble spots, in order:
1. `HxBridge.init` fails (no Activity found / class loader problem) - the error text names it.
2. The SDK expects its Java classes from a native thread (`FindClass`) and can't see the dex loader's classes.
3. `GetTokenFromDevice` shows its code/link in a way that isn't visible in-game - if so, say what appears and the UI can be adapted.
