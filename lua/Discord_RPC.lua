script_name("Hexed Discord RPC")
script_author("HEXED Studio")
script_version("2.0")

-- Lua side of the Discord_RPC AML mod: reads the SA-MP info and hands it to libDiscord_RPC.so over FFI.
-- (replaces Discord_RPC_Bridge.lua + the SampDiscordRpc companion app: no UDP, no second APK)

if script_properties then pcall(script_properties, "work-in-pause") end

require "lib.moonloader"
local ffi      = require("ffi")
local imgui    = require("mimgui")
local inicfg   = require("inicfg")
local encoding = require("encoding")
encoding.default = "CP1251"
local u8 = encoding.UTF8

local INI = "Hexed DiscordRpc.ini"
local cfg = inicfg.load({ main = { enabled = true, appid = "", interval = 1000, grace = 10 } }, INI)
inicfg.save(cfg, INI)

pcall(ffi.cdef, [[
    int         hx_rpc_start(const char* appId);
    const char* hx_rpc_error(void);
    void        hx_rpc_stop(void);
    void        hx_rpc_clear(void);
    void        hx_rpc_login(void);
    void        hx_rpc_logout(void);
    void        hx_rpc_set_state(const char* nick, int id, const char* server, const char* ip,
                                 int port, int players, long long start);
    int         hx_rpc_status(char* out, int cap);
    int         hx_rpc_flags(void);
]])

-- opening links: same approach as CameraSettings.lua, the game's own AND_OpenLink (no Java needed)
pcall(ffi.cdef, "void _Z12AND_OpenLinkPKc(const char* link);")
local okGta, gta = pcall(ffi.load, "GTASA")
local DISCORD_URL = "https://discord.gg/MmqnsXg2QC"

local function openLink(link)
    if okGta and gta then pcall(function() gta._Z12AND_OpenLinkPKc(link) end) end
end

local S = {
    lib = nil, started = false, startAt = 0, key = "", myId = nil,
    lastData = nil, lostAt = 0, win = imgui.new.bool(false),
    buf = ffi.new("char[256]"), err = "",
}

local function loadLib()
    for _, name in ipairs({ "libDiscord_RPC.so", "Discord_RPC" }) do
        local ok, l = pcall(ffi.load, name)
        if ok and l then return l end
    end
    return nil
end

local function say(msg) sampAddChatMessage(msg, -1) end
local function toU8(s) local ok, r = pcall(u8, s); return (ok and r) or s end
local function clean(s) return (tostring(s or ""):gsub("[\t\r\n]", " ")) end

local function status()
    if not S.lib then return "libDiscord_RPC.so not loaded (is the AML mod installed?)" end
    S.lib.hx_rpc_status(S.buf, 256)
    return ffi.string(S.buf)
end

local function flags() return S.lib and S.lib.hx_rpc_flags() or 0 end

local function start()
    if S.started then return true end
    if not S.lib then S.lib = loadLib() end
    if not S.lib then S.err = "libDiscord_RPC.so not found"; return false end
    if S.lib.hx_rpc_start(tostring(cfg.main.appid or "")) == 1 then
        S.started = true
        return true
    end
    S.err = ffi.string(S.lib.hx_rpc_error())
    return false
end

local function getMyId()
    local ok, found, id = pcall(sampGetPlayerIdByCharHandle, PLAYER_PED)
    if ok and found and type(id) == "number" then return id end
    return S.myId
end

local function snapshot()
    local okA, ip, port = pcall(sampGetCurrentServerAddress)
    if not okA or not ip or ip == "" then return nil end
    local id, nick = getMyId(), ""
    if id then
        local okN, n = pcall(sampGetPlayerNickname, id)
        if okN and n then nick = n end
    end
    local okS, server = pcall(sampGetCurrentServerName)
    local okP, players = pcall(sampGetPlayerCount, false)
    return { nick = nick, id = id or -1, server = (okS and server) or "", ip = ip,
             port = port or 7777, players = (okP and players) or 0 }
end

local function tick()
    if not cfg.main.enabled then return end
    if not start() then return end
    local d = snapshot()
    if d then
        S.lastData, S.lostAt = d, 0
    elseif S.lastData then
        if S.lostAt == 0 then S.lostAt = os.clock() end
        if os.clock() - S.lostAt > (tonumber(cfg.main.grace) or 10) then
            S.lastData, S.lostAt, S.key = nil, 0, ""
            S.lib.hx_rpc_clear()
            return
        end
        d = S.lastData
    else
        return
    end
    local key = d.ip .. ":" .. d.port
    if key ~= S.key then S.key, S.startAt = key, os.time() end
    S.lib.hx_rpc_set_state(clean(toU8(d.nick)), d.id, clean(toU8(d.server)), clean(d.ip),
                           d.port, d.players, S.startAt)
end

require("samp.events").onInitGame = function(playerId) S.myId = playerId end

function onScriptTerminate(scr)
    if scr == thisScript() and S.lib and S.started then pcall(S.lib.hx_rpc_stop) end
end

-- ---------------------------------------------------------------- menu
imgui.OnFrame(function() return S.win[0] end, function()
    imgui.SetNextWindowSize(imgui.ImVec2(420, 0), imgui.Cond.FirstUseEver)
    imgui.Begin("Hexed Discord RPC", S.win)
    local f = flags()
    imgui.TextWrapped(status())
    imgui.Separator()
    imgui.Text("Account: " .. (bit.band(f, 1) ~= 0 and "connected" or (bit.band(f, 4) ~= 0 and "saved login" or "not logged in")))
    if imgui.Button("Log in with Discord", imgui.ImVec2(-1, 0)) then
        if start() then S.lib.hx_rpc_login() end
    end
    if imgui.Button("Log out (forget account)", imgui.ImVec2(-1, 0)) then
        if S.lib then S.lib.hx_rpc_logout() end
    end
    local en = imgui.new.bool(cfg.main.enabled)
    if imgui.Checkbox("Rich presence enabled", en) then
        cfg.main.enabled = en[0]
        inicfg.save(cfg, INI)
        if not en[0] and S.lib and S.started then S.lib.hx_rpc_clear(); S.lastData, S.key = nil, "" end
    end
    imgui.Separator()
    if imgui.Button("HEXED STUDIO Discord", imgui.ImVec2(-1, 0)) then openLink(DISCORD_URL) end
    if S.err ~= "" then imgui.TextWrapped("Error: " .. S.err) end
    imgui.End()
end)

function main()
    while not isSampLoaded() or not isSampfuncsLoaded() do wait(500) end
    while not isSampAvailable() do wait(100) end

    S.lib = loadLib()
    if start() then
        say("{ff0000}[INFO] {ffffff}Hexed Discord RPC " .. thisScript().version .. " - /drpc")
    else
        say("{ff0000}[RPC] {ffffff}" .. S.err)
    end

    sampRegisterChatCommand("drpc", function() S.win[0] = not S.win[0] end)
    sampRegisterChatCommand("rpcdebug", function()
        say("[RPC] " .. status() .. " | flags=" .. flags() .. (S.err ~= "" and (" | err=" .. S.err) or ""))
    end)
    sampRegisterChatCommand("rpctoggle", function()
        cfg.main.enabled = not cfg.main.enabled
        inicfg.save(cfg, INI)
        if not cfg.main.enabled and S.lib and S.started then S.lib.hx_rpc_clear(); S.lastData, S.key = nil, "" end
        say("[RPC] enabled=" .. tostring(cfg.main.enabled))
    end)

    while true do
        wait(math.min(3000, math.max(250, tonumber(cfg.main.interval) or 1000)))
        pcall(tick)
    end
end
