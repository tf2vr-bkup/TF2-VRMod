#ifndef VR_INTEGRATION_H
#define VR_INTEGRATION_H

#include "view_shared.h"

class ITexture;
class CMatRenderContextPtr;
struct CViewSetup;

struct VRViewData_t
{
    Vector origin;
    QAngle angles;
    float fov;
    CViewSetup viewSetup;
};

namespace VRIntegration
{
    bool IsVRActive();
    bool BeginFrame();
    bool EndFrame();
    bool GetEyeViewData(VRViewData_t eyeData[2]);
    ITexture* GetEyeRenderTarget(int eye);
    ITexture* GetSharedRenderTarget();
}

#endif // VR_INTEGRATION_H