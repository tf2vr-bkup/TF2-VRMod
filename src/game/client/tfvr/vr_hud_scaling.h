#ifndef TFVR_VR_HUD_SCALING_H
#define TFVR_VR_HUD_SCALING_H

#include "openxr_manager.h"
#include "vgui/ISurface.h"
#include "vgui_controls/Controls.h"

// VR HUD pixel layouts use the original 1280x720 spectator surface as their
// logical coordinate space. Increasing mirror resolution should increase
// capture density without changing a panel's world-space footprint.
inline float TFVR_GetHUDPixelScale()
{
	if (g_pOpenXRManager)
	{
		uint32 width = 0;
		uint32 height = 0;
		g_pOpenXRManager->GetSpectatorScreenDims(width, height);
		if (width > 0)
			return (float)width / 1280.0f;
	}

	int width = 0;
	int height = 0;
	vgui::surface()->GetScreenSize(width, height);
	return width > 0 ? (float)width / 1280.0f : 1.0f;
}

inline int TFVR_ScaleHUDPixels(int pixels)
{
	int scaledPixels = (int)(pixels * TFVR_GetHUDPixelScale());
	return scaledPixels > 0 ? scaledPixels : 1;
}

#endif // TFVR_VR_HUD_SCALING_H
