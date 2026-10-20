// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is the platform's window
// (Win32 window / Android surface); its handle is the "native window" the game hands to vkCreateViSurfaceNN, which
// the Vulkan bridge turns into the platform's surface.
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx {

namespace {

constexpr uint64_t kFakeDisplay = kKernelBase - 0x1000;  // opaque pointers, never dereferenced by the game
constexpr uint64_t kFakeLayer = kKernelBase - 0x2000;

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], kFakeDisplay);
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    void* w = plat::create_window();
    log("vi: layer created -> native window %p", w);
    plat::release_window(w);
    st<uint64_t>(c.x[0], kFakeLayer);
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(plat::create_window()));
    c.x[0] = 0;
}

void nv_noop(CPU&) {}

// The display's vsync: a kernel event a host thread signals every 1/60 s. The game gets it as an nn::os
// SystemEvent (AttachSystemEvent with our read/write handles, auto clear: each wait consumes one vsync).
std::shared_ptr<EventState> g_vsync = std::make_shared<EventState>();

// A vsync that fires while the last one is still unconsumed is owed (at most one): it is signalled again as soon as
// the game takes the first. At 30 fps the game waits for two vsyncs a frame, and a frame a little over 33 ms (the PS4
// outside the cave) found the first one latched and the second lost, then waited up to 16.7 ms for the next: frames
// snapped from 34 ms to 50 (20.0 fps where 29 was possible). The game times its frames itself (it picks its present
// interval from them), so a late frame is not made later. NX_VSYNC_CATCHUP=0: missed vsyncs are lost, as on the Switch.
void vsync_thread() {
    plat::set_thread_name("vsync");
    static const bool catchup = [] {
        const char* e = std::getenv("NX_VSYNC_CATCHUP");
        return !(e && e[0] == '0');
    }();
    uint64_t next = plat::monotonic_ns();
    int owed = 0;
    for (;;) {
        next += 16'666'667;
        for (;;) {
            uint64_t now = plat::monotonic_ns();
            if (now >= next) break;
            if (owed && !g_vsync->signaled) {
                owed--;
                g_vsync->signaled = true;
                notify_all();
            }
            plat::sleep_ns(owed ? std::min<uint64_t>(next - now, 1'000'000) : next - now);
        }
        if (catchup && g_vsync->signaled) owed = 1;
        g_vsync->signaled = true;
        notify_all();
    }
}

void GetDisplayVsyncEvent(CPU& c) {  // (os::SystemEventType*, Display*)
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = g_vsync;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEventType; 1: AutoClear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    static std::once_flag once;
    std::call_once(once, [] { std::thread(vsync_thread).detach(); });
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", GetDisplayVsyncEvent);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_noop);

}  // namespace nx
