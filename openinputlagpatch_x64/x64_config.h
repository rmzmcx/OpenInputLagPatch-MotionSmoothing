#pragma once

#include <Windows.h>

namespace X64Config {
extern float ReplaySlowRate;
extern UINT ReplayFastRate;
extern BOOL ForceFlipDiscard;
extern BOOL AllowTearing;
extern int Interpolation;
extern UINT InterpolationFPS;
extern BOOL InterpolateEnemyBullets;
extern BOOL InterpolateLasers;
extern BOOL InterpolatePlayerBody;
extern BOOL InterpolatePlayerShots;
}
