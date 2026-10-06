#ifndef LIWA_VEHICLE_SYSTEM_H
#define LIWA_VEHICLE_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x;
    float y;
    float z;
} VehicleSystemVector3;

void VehicleSystem_ResetInput(void);
void VehicleSystem_PollInput(void *pad, uint32_t *pressed, uint32_t *released);
float VehicleSystem_DistanceSquared3D(VehicleSystemVector3 a, VehicleSystemVector3 b);
void VehicleSystem_ExitCandidates(VehicleSystemVector3 vehicle_position, float yaw,
                                  float lateral_offset, VehicleSystemVector3 *left,
                                  VehicleSystemVector3 *right);
void VehicleSystem_RestoreAndBounce(VehicleSystemVector3 *position,
                                    VehicleSystemVector3 previous_safe_position,
                                    float *speed, float *velocity_x, float *velocity_z,
                                    float elasticity);
VehicleSystemVector3 VehicleSystem_LerpVector(VehicleSystemVector3 from,
                                               VehicleSystemVector3 to, float dt,
                                               float response);

#ifdef __cplusplus
}
#endif

#endif