#ifndef VR_INPUT_H
#define VR_INPUT_H

#include "cbase.h"
#include "input.h"
#include "usercmd.h"
#include "openxr_manager.h"
#include "iinput.h"

// Forward declarations
class CInput;
class IInput;

// Global input pointer declaration
extern IInput* input;

class CVRInput : public CInput
{
public:
    CVRInput();
    virtual ~CVRInput();

    // Override input processing functions
    void CreateMove(int sequence_number, float input_sample_frametime, bool active) override;
    void ExtraMouseSample(float frametime, bool active) override;

    static void CopyVRPosesToUserCmd(CUserCmd* cmd);

protected:
    // Helper functions for VR input processing
    void ProcessVRControllerInput(CUserCmd* cmd);
    void ProcessVRViewAngles(CUserCmd* cmd);
    void ProcessVRMovement(CUserCmd* cmd);
    
};

// Global instances - declared after class definition
extern CVRInput g_VRInput;
extern IInput* g_OriginalNonVRInputPtr;

#endif // VR_INPUT_H 