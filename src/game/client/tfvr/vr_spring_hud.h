#ifndef VR_SPRING_HUD_H
#define VR_SPRING_HUD_H
#ifdef _WIN32
#pragma once
#endif

#include "convar.h"

// Killfeed layout controls. Rendering and spring behavior are owned by the
// popup HUD manager so every popup element follows the same transform.
extern ConVar tfvr_killfeed_enabled;
extern ConVar tfvr_killfeed_scale;
extern ConVar tfvr_killfeed_offset_x;
extern ConVar tfvr_killfeed_offset_y;
extern ConVar tfvr_killfeed_width;
extern ConVar tfvr_killfeed_height;

#endif // VR_SPRING_HUD_H

