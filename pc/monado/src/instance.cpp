// The service's xrt_instance: Monado's IPC server asks it for the system once a game connects.
// Where Monado's own instance probes for hardware, this one builds RinCast's devices and gives
// the main compositor RinCast's target (rincast_monado.h).
#include <cstdlib>

#include "rincast_monado.h"

#include "main/comp_main_interface.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_system.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_space.h"

namespace {

xrt_result_t IsSystemAvailable(xrt_instance*, bool* out) {
    *out = true;
    return XRT_SUCCESS;
}

xrt_result_t CreateSystem(xrt_instance*, xrt_system** outSys, xrt_system_devices** outDevices,
                          xrt_space_overseer** outSpaces, xrt_system_compositor** outCompositor) {
    u_system* usys = u_system_create();
    xrt_system_devices* xsysd = nullptr;
    xrt_space_overseer* xso = nullptr;
    xrt_result_t r = rcm::CreateSystemDevices(&usys->broadcast, &xsysd, &xso);
    if (r != XRT_SUCCESS) {
        u_system_destroy(&usys);
        return r;
    }
    xrt_device* head = xsysd->static_roles.head;
    u_system_fill_properties(usys, head->str);
    xrt_system_compositor* xsysc = nullptr;
    if (outCompositor) {
        r = comp_main_create_system_compositor(head, rcm::TargetFactory(), nullptr, &xsysc);
        if (r != XRT_SUCCESS) {
            U_LOG_E("RinCast: the compositor didn't start (%d)", int(r));
            xrt_space_overseer_destroy(&xso);
            xrt_system_devices_destroy(&xsysd);
            u_system_destroy(&usys);
            return r;
        }
        u_system_set_system_compositor(usys, xsysc);
        *outCompositor = xsysc;
    }
    *outSys = &usys->base;
    *outDevices = xsysd;
    *outSpaces = xso;
    return XRT_SUCCESS;
}

xrt_result_t GetProber(xrt_instance*, xrt_prober**) { return XRT_ERROR_PROBER_NOT_SUPPORTED; }

void Destroy(xrt_instance* xinst) { free(xinst); }

}  // namespace

extern "C" xrt_result_t xrt_instance_create(xrt_instance_info*, xrt_instance** out) {
    auto* inst = U_TYPED_CALLOC(xrt_instance);
    inst->is_system_available = IsSystemAvailable;
    inst->create_system = CreateSystem;
    inst->get_prober = GetProber;
    inst->destroy = Destroy;
    inst->startup_timestamp = os_monotonic_get_ns();
    *out = inst;
    return XRT_SUCCESS;
}
