// nn::hid at the API level: player 1's Npad state comes from the platform (XInput + keyboard on Windows, game
// controllers + touch on Android), players 2-4 (Npad ids No2-No4) from plat::read_extra_pad where the platform has
// more controllers (PS4: one DualShock 4 per signed-in user). Vibration is accepted and dropped.
#include <array>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

using namespace plat;

constexpr uint32_t kStyleFullKey = 1u << 0;
uint64_t g_sampling = 0;

// Scripted input for automated runs: NX_INPUT="3300:A,3900:A,4200:Down" holds the button for 8 frames from the
// given presented frame.
struct Scripted {
    uint64_t frame;
    uint64_t buttons;
};

std::vector<Scripted> input_script() {
    std::vector<Scripted> v;
    const char* e = std::getenv("NX_INPUT");
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            uint64_t bits = b == "A" ? kA : b == "B" ? kB : b == "X" ? kX : b == "Y" ? kY : b == "Plus" ? kPlus :
                            b == "Minus" ? kMinus : b == "Up" ? kUp : b == "Down" ? kDown : b == "Left" ? kLeft :
                            b == "Right" ? kRight : b == "L" ? kL : b == "R" ? kR : 0;
            v.push_back({std::strtoull(item.c_str(), nullptr, 10), bits});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

constexpr uint32_t kPlayers = 4;  // Npad ids No1..No4 (0..3)

// Player `id` (0..3): false when no controller is there for it. Player 1 is always there.
bool read(uint32_t id, PadState& p) {
    p = PadState();
    if (id == 0) {
        static const std::vector<Scripted> script = input_script();
        for (const auto& s : script)
            if (g_frame >= s.frame && g_frame < s.frame + 8) p.buttons |= s.buttons;
        read_pad(p);
    } else if (id >= kPlayers || !read_extra_pad(int(id), p)) {
        return false;
    }
    // Stick directions as buttons, like the hid sysmodule reports them.
    constexpr int32_t t = kStickMax / 2;
    if (p.lx < -t) p.buttons |= kStickLLeft;
    if (p.lx > t) p.buttons |= kStickLRight;
    if (p.ly > t) p.buttons |= kStickLUp;
    if (p.ly < -t) p.buttons |= kStickLDown;
    if (p.rx < -t) p.buttons |= kStickRLeft;
    if (p.rx > t) p.buttons |= kStickRRight;
    if (p.ry > t) p.buttons |= kStickRUp;
    if (p.ry < -t) p.buttons |= kStickRDown;
    return true;
}

// Players 2-4 come and go: when one does, the game's style set update event for that id is signalled (games wait for
// it before they look at an npad again). Events are bound by BindNpadStyleSetUpdateEvent.
std::mutex g_conn_lock;
uint64_t g_style_events[kPlayers] = {};
bool g_connected[kPlayers] = {true, false, false, false};

bool connected(uint32_t id) {
    if (id >= kPlayers) return false;
    std::lock_guard lk(g_conn_lock);
    return g_connected[id];
}

void signal_style_event(CPU& c, uint64_t ev) {
    static uint64_t signal = find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (!signal || !ev) return;
    uint64_t saved[32];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = ev;
    call(c, signal);
    std::memcpy(c.x, saved, sizeof saved);
}

// Called from the npad reads (every frame): every 30th call, whether players 2-4 are there.
void poll_players(CPU& c) {
    static std::atomic<uint32_t> calls{0};
    if (calls++ % 30) return;
    for (uint32_t id = 1; id < kPlayers; id++) {
        PadState p;
        bool on = read_extra_pad(int(id), p);
        uint64_t ev = 0;
        {
            std::lock_guard lk(g_conn_lock);
            if (on == g_connected[id]) continue;
            g_connected[id] = on;
            ev = g_style_events[id];
        }
        log("hid: player %u %s", id + 1, on ? "connected" : "disconnected");
        signal_style_event(c, ev);
    }
}

// NpadFullKeyState / NpadJoyDualState / NpadHandheldState:
// {s64 sampling; u64 buttons; {s32 x, y} left; {s32 x, y} right; u32 attributes; pad}
void GetNpadState(CPU& c) {
    poll_players(c);
    uint64_t out = c.x[0];
    uint32_t id = ld<uint32_t>(c.x[1]);
    std::memset(reinterpret_cast<void*>(out), 0, 0x28);
    st<int64_t>(out, int64_t(++g_sampling));
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s", id, hle_current_name());
    }
    PadState p;
    if (!connected(id) || !read(id, p)) return;  // no controller for this player
    static bool pressed[kPlayers] = {};
    if (p.buttons && !pressed[id]) log("hid: player %u's first buttons %llx", id + 1, (unsigned long long)p.buttons), pressed[id] = true;
    st<uint64_t>(out + 8, p.buttons);
    st<int32_t>(out + 16, p.lx);
    st<int32_t>(out + 20, p.ly);
    st<int32_t>(out + 24, p.rx);
    st<int32_t>(out + 28, p.ry);
    st<uint32_t>(out + 32, 1);  // IsConnected
}

// GetNpadStates(State* out, int count, const u32& id) -> number written: the newest first. One sample: the current
// one for player 1's full-key style, a not-connected one otherwise.
// GetNpadStates(State* out, int count, const u32& id) -> number written, newest first. Like the real hid, a history:
// Wonder ignores the return value and reads all 16 entries it asks for, telling new samples by their sampling
// numbers, so every call adds a sample and returns up to 16 of the latest (older entries stay as they were).
void GetNpadStatesFullKey(CPU& c) {
    {
        static std::atomic<uint64_t> calls{0};
        uint64_t n = ++calls;
        if (n % 3000 == 0) log("hid: %llu full-key reads (lr %s)", (unsigned long long)n, describe(c.x[30]).c_str());
    }
    uint64_t out = c.x[0], id_ref = c.x[2];
    int n = int(c.x[1]);
    if (n < 1) {
        c.x[0] = 0;
        return;
    }
    static std::mutex m;
    static std::deque<std::array<uint8_t, 0x28>> histories[kPlayers];
    std::lock_guard lk(m);
    c.x[0] = out;
    c.x[1] = id_ref;
    GetNpadState(c);  // the new sample, into out[0]
    uint32_t id = ld<uint32_t>(id_ref);
    if (id >= kPlayers || !connected(id)) {
        c.x[0] = 1;
        return;
    }
    auto& history = histories[id];
    std::array<uint8_t, 0x28> now;
    std::memcpy(now.data(), reinterpret_cast<const void*>(out), now.size());
    history.push_front(now);
    if (history.size() > 16) history.pop_back();
    int count = std::min<int>(n, int(history.size()));
    for (int k = 1; k < count; k++) std::memcpy(reinterpret_cast<void*>(out + 0x28ull * k), history[k].data(), 0x28);
    c.x[0] = uint64_t(count);
}

void GetNpadStatesOther(CPU& c) {
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        uint32_t id = ld<uint32_t>(c.x[2]);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s (not connected)", id, hle_current_name());
    }
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// MouseState {s64 sampling; s32 x, y, dx, dy, wheel dx, wheel dy; u32 buttons; u32 attributes}: no mouse.
void GetMouseStates(CPU& c) {
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// KeyboardState {s64 sampling; u64 modifiers; u8 keys[32]}: nothing pressed (the keyboard drives the pad).
void GetKeyboardState(CPU& c) {
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x30);
    st<int64_t>(c.x[0], int64_t(g_sampling));
}

void zero(CPU& c) { c.x[0] = 0; }
void one(CPU& c) { c.x[0] = 1; }

void GetNpadStyleSet(CPU& c) {
    static int logged = 0;
    if (logged++ < 4) log("hid: GetNpadStyleSet id %#x", ld<uint32_t>(c.x[0]));
    c.x[0] = connected(ld<uint32_t>(c.x[0])) ? kStyleFullKey : 0;
}

void BindNpadStyleSetUpdateEvent(CPU& c) {  // (const u32& id, SystemEventType*, EventClearMode)
    uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t ev = c.x[1], mode = c.x[2];
    uint32_t id = ld<uint32_t>(c.x[0]);
    c.x[0] = ev;
    c.x[1] = mode;
    c.x[2] = 0;
    call(c, create);
    // A controller that is there (player 1 always) "changed" (connected) once: games wait for this event before they
    // look at the npad. Players 2-4 get theirs when they connect (poll_players).
    bool on = false;
    if (id < kPlayers) {
        std::lock_guard lk(g_conn_lock);
        g_style_events[id] = ev;
        on = g_connected[id];
    }
    if (on) signal_style_event(c, ev);
    log("hid: style set event bound for id %#x%s", id, on ? ", signalled" : "");
}

void GetVibrationDeviceHandles(CPU& c) { c.x[0] = 0; }  // no vibration devices
void ShowControllerSupport(CPU& c) {                     // (ControllerSupportResultInfo*, const Arg&)
    int8_t players = 0;
    for (uint32_t id = 0; id < kPlayers; id++) players += connected(id) ? 1 : 0;
    log("hid: controller support applet: %d players", players);
    st<int8_t>(c.x[0], players);  // player count
    st<uint32_t>(c.x[0] + 4, 0);  // selected npad id: No1
    c.x[0] = 0;
}
void noop(CPU& c) { c.x[0] = 0; }

}  // namespace

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", noop);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", noop);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
NX_HLE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", ShowControllerSupport);
// SDK 16 (Super Mario Bros. Wonder)
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", GetNpadStatesFullKey);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid15InitializeMouseEv", noop);
NX_HLE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", GetMouseStates);
NX_HLE("_ZN2nn3hid18InitializeKeyboardEv", noop);
NX_HLE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", GetKeyboardState);
NX_HLE("_ZN2nn3hid14DisconnectNpadERKj", noop);
NX_HLE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", noop);
NX_HLE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", noop);
NX_HLE("_ZN2nn3hid21StartLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20StopLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", zero);  // dual
NX_HLE("_ZN2nn3hid20IsVibrationPermittedEv", one);
NX_HLE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", noop);
NX_HLE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", zero);

}  // namespace nx
