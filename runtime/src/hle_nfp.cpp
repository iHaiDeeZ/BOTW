// nn::nfp (amiibo) at the API level: there is no NFC reader. Initialize succeeds, no device is ever listed (the
// availability event never fires), and every per-device call reports DeviceNotFound. Without this the sdk's nfp
// client talked to the stub nfp:user service and crashed in nn::nfp::Initialize (Tears of the Kingdom, at boot).
// As MacNativeGames' applet_host.c answers it.
#include <memory>

#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

namespace {

constexpr uint32_t kDeviceNotFound = 0x8073;  // nfp module 115, description 64

int g_state = 0;  // nn::nfp::State: 0 none, 1 initialized

void Initialize(CPU& c) {
    g_state = 1;
    c.x[0] = 0;
}

void Finalize(CPU& c) {
    g_state = 0;
    c.x[0] = 0;
}

void GetState(CPU& c) { c.x[0] = uint64_t(g_state); }

void ListDevices(CPU& c) {  // (DeviceHandle* out, int* count, int max)
    if (c.x[1]) st<int32_t>(c.x[1], 0);
    c.x[0] = kDeviceNotFound;
}

// (os::SystemEventType*): a real event of ours that is never signaled (no reader comes or goes).
void AttachAvailabilityChangeEvent(CPU& c) {
    uint64_t sysev = c.x[0];
    auto state = std::make_shared<EventState>();
    auto r = std::make_shared<KEvent>();
    auto w = std::make_shared<KEvent>();
    r->state = w->state = state;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static const uint64_t attach =
        find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    if (!attach) fatal("nn::os::AttachSystemEvent not found");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = sysev, c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 0;  // auto clear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = 0;
}

void GetDeviceState(CPU& c) { c.x[0] = 0; }  // DeviceState_Init: nothing there

void NoDevice(CPU& c) { c.x[0] = kDeviceNotFound; }

}  // namespace

NX_HLE("_ZN2nn3nfp10InitializeEv", Initialize);
NX_HLE("_ZN2nn3nfp8FinalizeEv", Finalize);
NX_HLE("_ZN2nn3nfp8GetStateEv", GetState);
NX_HLE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ListDevices);
NX_HLE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", AttachAvailabilityChangeEvent);
NX_HLE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", GetDeviceState);
NX_HLE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", NoDevice);
NX_HLE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", NoDevice);
NX_HLE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", NoDevice);
NX_HLE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", NoDevice);
NX_HLE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", NoDevice);
NX_HLE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", NoDevice);
NX_HLE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", NoDevice);
NX_HLE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", NoDevice);

}  // namespace nx
