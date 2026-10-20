// nn::oe calls whose sdk client aborts on the stub am/oe services: there is never a launch parameter (the applet
// storage it pops does not exist here) and never a notification message. Breath of the Wild aborted in
// TryPopLaunchParameter at boot (v00.05).
#include <nx/runtime.h>

namespace nx {

namespace {

void none(CPU& c) { c.x[0] = 0; }  // bool: false, nothing popped

}  // namespace

NX_HLE("_ZN2nn2oe21TryPopLaunchParameterEPmPvm", none);
NX_HLE("_ZN2nn2oe25TryPopNotificationMessageEPj", none);

}  // namespace nx
