#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#if !__has_include(<hyprland/src/desktop/view/Window.hpp>)
#define HYPRPARK_NEW_API 1
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#else
#define HYPRPARK_NEW_API 0
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#endif
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <lua.hpp>
#include <stdexcept>
#include <string>
#include <vector>

static HANDLE handle;

// Weak references let closed windows disappear without extending their lifetime.
// Only windows hidden by this instance may be restored by it.
struct ParkedWindow {
    PHLWINDOWREF window;
    bool detached;
    Fullscreen::SFullscreenMode fullscreen;
};

static std::vector<ParkedWindow> parked;
#if HYPRPARK_NEW_API
static SP<IPC::Socket1::SCommand> statusCommand;
#else
static SP<SHyprCtlCommand> statusCommand;
#endif

static bool isMapped(const PHLWINDOW& w) {
#if HYPRPARK_NEW_API
    return w->mapped();
#else
    return w->m_isMapped;
#endif
}

static bool isGrouped(const PHLWINDOW& w) {
#if HYPRPARK_NEW_API
    return static_cast<bool>(w->grouping().group());
#else
    return static_cast<bool>(w->m_group);
#endif
}

static void postChange() {
#if HYPRPARK_NEW_API
    IPC::Socket2::sock()->postEvent({"hyprpark", ""});
#else
    g_pEventManager->postEvent({"hyprpark", ""});
#endif
}

// Resolve IPC addresses against mapped windows rather than dereferencing pointers.
static PHLWINDOW findWindow(const std::string& address) {
    for (const auto& w : Desktop::windowState()->windows()) {
        if (isMapped(w) && std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get())) == address)
            return w;
    }
    return nullptr;
}

// Closure events are handled by the shell; prune stale entries before queries/actions.
static void prune() {
    std::erase_if(parked, [](const auto& entry) {
        const auto w = entry.window.lock();
        return !w || !isMapped(w);
    });
}

static void redraw(PHLWINDOW w) {
    if (const auto monitor = w->m_monitor.lock())
        g_pHyprRenderer->damageMonitor(monitor);
}

static int parkWindow(lua_State* L) {
    const auto w = findWindow(luaL_checkstring(L, 1));
    if (!w || w->isHidden() || isGrouped(w))
        return luaL_error(L, "hyprpark: window unavailable, already hidden, or grouped");
    prune();

    const auto target = w->layoutTarget();
    const bool detached = target && target->space() && !target->floating();
    const auto fullscreen = Fullscreen::controller()->getFullscreenModes(w);
    parked.push_back({w, detached, fullscreen});

    // Suspend fullscreen coverage before removing its tile. Save both modes so
    // restoring can recover maximize/fullscreen without keeping neighbors covered.
    if (fullscreen.internal != Fullscreen::FSMODE_NONE || fullscreen.client != Fullscreen::FSMODE_NONE)
        Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
    if (Desktop::focusState()->isWindowActive(w))
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    w->setHidden(true);

    // Ghost membership preserves the workspace but frees the tile. Reinsertion
    // follows the current layout, rather than preserving the old split tree.
    if (detached)
        target->setSpaceGhost(target->space());
    redraw(w);
    g_pInputManager->refocus();
    postChange();
    return 0;
}

// Reinsert a detached tile using the current layout and reapply saved modes.
static void reveal(const ParkedWindow& entry) {
    const auto w = entry.window.lock();
    if (!w || !isMapped(w))
        return;
    w->setHidden(false);

    if (entry.detached) {
        const auto target = w->layoutTarget();
        if (target && target->space())
            target->assignToSpace(target->space());
    }
    if (entry.fullscreen.internal != Fullscreen::FSMODE_NONE || entry.fullscreen.client != Fullscreen::FSMODE_NONE)
        Fullscreen::controller()->setFullscreenMode(w, entry.fullscreen.internal, entry.fullscreen.client);
    redraw(w);
}

// Callers decide whether to focus the restored window or switch workspaces.
static int restoreWindow(lua_State* L) {
    const auto w = findWindow(luaL_checkstring(L, 1));
    prune();
    const auto it = std::find_if(
        parked.begin(), parked.end(), [&](const auto& entry) { return entry.window.lock() == w; });
    if (!w || it == parked.end())
        return 0;
    reveal(*it);
    parked.erase(it);
    postChange();
    redraw(w);
    return 0;
}

#if !HYPRPARK_NEW_API
// Dispatchers share the Lua implementation so both interfaces enforce ownership.
static SDispatchResult dispatch(const std::string& address, bool restore) {
    auto* L = luaL_newstate();
    if (!L)
        return {.success = false, .error = "hyprpark: unable to allocate Lua state"};
    lua_pushcfunction(L, restore ? restoreWindow : parkWindow);
    std::string target = address;
    if (target.starts_with("address:"))
        target.erase(0, 8);
    if (target.empty() && !restore) {
        const auto focused = Desktop::focusState()->window();
        if (focused)
            target = std::format("0x{:x}", reinterpret_cast<uintptr_t>(focused.get()));
    }
    lua_pushlstring(L, target.data(), target.size());

    // A protected call converts Lua API errors into dispatcher failure messages.
    const bool success = lua_pcall(L, 1, 0, 0) == LUA_OK;
    const std::string error = success ? "" : lua_tostring(L, -1);
    lua_close(L);
    return {.success = success, .error = error};
}

#endif

static void restoreAll() {
    for (const auto& entry : parked)
        reveal(entry);
    parked.clear();
    postChange();
}

static int restoreAllLua(lua_State*) {
    restoreAll();
    return 0;
}

static std::string listWindows() {
    prune();
    std::string json = "[";
    for (const auto& entry : parked) {
        if (json.size() > 1)
            json += ",";
        json += std::format("\"0x{:x}\"", reinterpret_cast<uintptr_t>(entry.window.lock().get()));
    }
    return json + "]";
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE pluginHandle) {
    handle = pluginHandle;
    // Internal window APIs require headers from the exact running compositor.
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("hyprpark: Hyprland/header version mismatch; rebuild with matching headers");

    if (!HyprlandAPI::addLuaFunction(handle, "hyprpark", "park", parkWindow) ||
        !HyprlandAPI::addLuaFunction(handle, "hyprpark", "restore", restoreWindow) ||
        !HyprlandAPI::addLuaFunction(handle, "hyprpark", "restore_all", restoreAllLua))
        throw std::runtime_error("hyprpark: unable to register Lua functions");

#if !HYPRPARK_NEW_API
    // The new API retains addDispatcherV2 only as a stub returning false.
    if (!HyprlandAPI::addDispatcherV2(handle, "hyprpark:park",
                                     [](std::string address) { return dispatch(address, false); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprpark:restore",
                                     [](std::string address) { return dispatch(address, true); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprpark:restoreall", [](std::string) {
            restoreAll();
            return SDispatchResult{};
        }))
        throw std::runtime_error("hyprpark: unable to register dispatchers");
#endif

    // Always return JSON so shells can rebuild their view after a reload.
#if HYPRPARK_NEW_API
    statusCommand = HyprlandAPI::registerHyprCtlCommand(handle, {
        .name = "hyprparkwindows",
        .match = IPC::Socket1::COMMAND_MATCH_EXACT,
        .handler = [](const IPC::Socket1::SRequest&) { return IPC::Socket1::SResponse{listWindows()}; }
    });
#else
    statusCommand = HyprlandAPI::registerHyprCtlCommand(handle, {
        .name = "hyprparkwindows",
        .exact = true,
        .fn = [](eHyprCtlOutputFormat, std::string) {
            return listWindows();
        }
    });
#endif
    if (!statusCommand)
        throw std::runtime_error("hyprpark: unable to register status command");
    return {"hyprpark", "Park windows in place and free their tiled space", "AI-generated", "0.1.0"};
}

// Unloading must never strand windows hidden by this plugin.
APICALL EXPORT void PLUGIN_EXIT() {
    restoreAll();
    statusCommand.reset();
}
