#include "cbase.h"
#include "vr_integration.h"
#include "openxr_manager.h"
#include "materialsystem/itexture.h"
#include "hmdWrapper.h"

namespace VRIntegration
{
    bool IsVRActive()
    {
        return g_pOpenXRManager && g_pOpenXRManager->IsActive();
    }

    bool BeginFrame()
    {
        if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
            return false;

        return g_pOpenXRManager->BeginFrame();
    }

    bool EndFrame()
    {
        if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
            return false;

        return g_pOpenXRManager->EndFrame();
    }

    bool GetEyeViewData(VRViewData_t eyeData[2])
    {
        if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
            return false;

        // Get view transforms from OpenXR
        return g_pOpenXRManager->GetEyeViewData(eyeData);
    }
    
    ITexture* GetSharedRenderTarget()
    {
        if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
            return nullptr;

        return g_pOpenXRManager->GetSharedRenderTarget();
    }
}