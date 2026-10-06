#include "VehicleSystem.h"

#include <math.h>

#if __has_include(<pspctrl.h>)
#include <pspctrl.h>
#else
typedef struct {
    unsigned int Buttons;
    signed char Lx;
    signed char Ly;
} SceCtrlData;
extern "C" int sceCtrlPeekBufferPositive(void *pad, int count);
#endif

namespace {
// Keep the previous button mask between polls so taps are emitted once.
uint32_t previous_buttons = 0;
}

extern "C" void VehicleSystem_ResetInput(void) {
    previous_buttons = 0;
}

extern "C" void VehicleSystem_PollInput(void *pad_data, uint32_t *pressed,
                                         uint32_t *released) {
    SceCtrlData *pad = static_cast<SceCtrlData *>(pad_data);
    sceCtrlPeekBufferPositive(pad, 1);
    *pressed = pad->Buttons & ~previous_buttons;
    *released = previous_buttons & ~pad->Buttons;
    previous_buttons = pad->Buttons;
}

extern "C" float VehicleSystem_DistanceSquared3D(VehicleSystemVector3 a,
                                                  VehicleSystemVector3 b) {
    // Compare squared distance against a squared range without a square root.
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

extern "C" void VehicleSystem_ExitCandidates(VehicleSystemVector3 vehicle_position,
                                              float yaw, float lateral_offset,
                                              VehicleSystemVector3 *left,
                                              VehicleSystemVector3 *right) {
    // At yaw zero, the driver's left side points along negative X.
    const float side_x = -cosf(yaw) * lateral_offset;
    const float side_z = sinf(yaw) * lateral_offset;
    *left = vehicle_position;
    *right = vehicle_position;
    left->x += side_x;
    left->z += side_z;
    right->x -= side_x;
    right->z -= side_z;
}

extern "C" void VehicleSystem_RestoreAndBounce(VehicleSystemVector3 *position,
                                                VehicleSystemVector3 previous_safe_position,
                                                float *speed, float *velocity_x,
                                                float *velocity_z, float elasticity) {
    // Restore the pre-step location, then reverse a fraction of impact velocity.
    *position = previous_safe_position;
    const float bounce = -fabsf(elasticity);
    *speed *= bounce;
    *velocity_x *= bounce;
    *velocity_z *= bounce;
}

extern "C" VehicleSystemVector3 VehicleSystem_LerpVector(VehicleSystemVector3 from,
                                                          VehicleSystemVector3 to,
                                                          float dt, float response) {
    // Clamp dt-scaled response so frame spikes cannot overshoot the target.
    float amount = dt * response;
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    from.x += (to.x - from.x) * amount;
    from.y += (to.y - from.y) * amount;
    from.z += (to.z - from.z) * amount;
    return from;
}