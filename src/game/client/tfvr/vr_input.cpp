#include "cbase.h"
#include "vr_input.h"
#include "in_buttons.h"
#include "mathlib/mathlib.h"
#include "iinput.h"
#include "iclientmode.h"
#include "input.h"
#include "convar.h"

// ConVars for input sensitivity and deadzone
ConVar tfvr_move_sensitivity("tfvr_move_sensitivity", "1.0", FCVAR_ARCHIVE, "Sensitivity multiplier for VR movement");
ConVar tfvr_thumbstick_deadzone("tfvr_thumbstick_deadzone", "0.1", FCVAR_ARCHIVE, "Deadzone for thumbstick movement");
ConVar tfvr_use_hmd_angles("tfvr_use_hmd_angles", "0", FCVAR_ARCHIVE, "Use HMD angles for view");

// Movement speed ConVars
extern ConVar cl_forwardspeed;
extern ConVar cl_sidespeed;

// Define global instances
CVRInput g_VRInput;
IInput* g_OriginalNonVRInputPtr = nullptr;

CVRInput::CVRInput()
{
}

CVRInput::~CVRInput()
{
}

void CVRInput::CopyVRPosesToUserCmd(CUserCmd *cmd)
{
    g_pOpenXRManager->GetHMDInChaperone(cmd->playerToHmdOrigin, cmd->playerToHmdAngles);
}

void CVRInput::CreateMove(int sequence_number, float input_sample_frametime, bool active)
{
    // Get the command for this sequence
    CUserCmd* cmd = &m_pCommands[sequence_number % MULTIPLAYER_BACKUP];
    CVerifiedUserCmd* pVerified = &m_pVerifiedCommands[sequence_number % MULTIPLAYER_BACKUP];
    
    // Let base input system handle everything first
    CInput::CreateMove(sequence_number, input_sample_frametime, active);
    
    // Only process VR input if active
    if (active && g_pOpenXRManager && g_pOpenXRManager->IsActive())
    {
        // Process VR-specific input
        ProcessVRControllerInput(cmd);
        
        // Only process VR view angles if explicitly enabled
        if (tfvr_use_hmd_angles.GetBool())
        {
            // ProcessVRViewAngles(cmd);
        }
        
        // Process VR movement
        ProcessVRMovement(cmd);
    }

    CopyVRPosesToUserCmd(cmd);

    // Let the game mode process the command
    //if (g_pClientMode)
    //{
    //    g_pClientMode->CreateMove(input_sample_frametime, cmd);
    //}

    // Store the command for verification
    pVerified->m_cmd = *cmd;
    pVerified->m_crc = cmd->GetChecksum();
}

bool g_bExtraMouseSample = false;

void CVRInput::ExtraMouseSample(float frametime, bool active)
{
    // Only disable mouse sampling if VR is active AND we're using HMD angles
    if (g_pOpenXRManager && g_pOpenXRManager->IsActive() && tfvr_use_hmd_angles.GetBool())
    {
        return;
    }

    // Otherwise, allow normal mouse input
    CInput::ExtraMouseSample(frametime, active);
}

void CVRInput::ProcessVRControllerInput(CUserCmd* cmd)
{
    // Store original button state
    int oldButtons = cmd->buttons;
    
    // Primary attack
    bool bPrimaryAttack = g_pOpenXRManager->GetAnalogValue("primary_attack") > 0.5f;
    if (bPrimaryAttack)
        cmd->buttons |= IN_ATTACK;

    // Secondary attack
    bool bSecondaryAttack = g_pOpenXRManager->GetAnalogValue("secondary_attack") > 0.5f;
    if (bSecondaryAttack)
        cmd->buttons |= IN_ATTACK2;

    // Use
    bool bUse = g_pOpenXRManager->IsButtonPressed("use");
    if (bUse)
        cmd->buttons |= IN_USE;

    // Duck
    bool bDuck = g_pOpenXRManager->IsButtonPressed("duck");
    if (bDuck)
        cmd->buttons |= IN_DUCK;

    // Jump
    bool bJump = g_pOpenXRManager->IsButtonPressed("jump");
    if (bJump)
        cmd->buttons |= IN_JUMP;

    // Menu
    bool bMenu = g_pOpenXRManager->IsButtonPressed("menu");
    if (bMenu)
        cmd->buttons |= IN_USE;

    // Debug output for button states
    if (cmd->buttons != oldButtons)
    {
        DevMsg("VR Input: buttons changed %d->%d\n", oldButtons, cmd->buttons);
    }
}

void CVRInput::ProcessVRViewAngles(CUserCmd* cmd)
{
    // Only process view angles if explicitly enabled
    if (!tfvr_use_hmd_angles.GetBool())
        return;

    // Get HMD orientation
    VMatrix hmdMatrix = g_pOpenXRManager->GetMideyePose();
    
    // Extract angles from the matrix
    QAngle angles;
    MatrixAngles(hmdMatrix.As3x4(), angles);
    
    // Set the view angles
    cmd->viewangles = angles;
    
    // Update engine view angles
    engine->SetViewAngles(angles);

    // Debug output for view angles
    DevMsg("VR View: pitch=%.1f yaw=%.1f roll=%.1f\n", angles.x, angles.y, angles.z);
}

void CVRInput::ProcessVRMovement(CUserCmd* cmd)
{
    // Get movement values
    float moveX = g_pOpenXRManager->GetAnalogValue("move_x");
    float moveY = g_pOpenXRManager->GetAnalogValue("move_y");

    // DevMsg("VR Movement: Raw values - x=%.2f y=%.2f\n", moveX, moveY);

    // Apply deadzone
    float deadzone = tfvr_thumbstick_deadzone.GetFloat();
    if (fabs(moveX) < deadzone) moveX = 0;
    if (fabs(moveY) < deadzone) moveY = 0;

    //DevMsg("VR Movement: After deadzone - x=%.2f y=%.2f\n", moveX, moveY);

    // Apply sensitivity
    float sensitivity = tfvr_move_sensitivity.GetFloat();
    moveX *= sensitivity;
    moveY *= sensitivity;

    //DevMsg("VR Movement: After sensitivity - x=%.2f y=%.2f\n", moveX, moveY);

    // Get current movement values before modification
    float oldForward = cmd->forwardmove;
    float oldSide = cmd->sidemove;

    // Map to movement values - use cl_forwardspeed and cl_sidespeed for proper scaling
    float forwardSpeed = cl_forwardspeed.GetFloat();
    float sideSpeed = cl_sidespeed.GetFloat();
    
    // DevMsg("VR Movement: Speed values - forward=%.2f side=%.2f\n", forwardSpeed, sideSpeed);

    // Add VR movement to existing values
    cmd->forwardmove += moveY * forwardSpeed;
    cmd->sidemove += moveX * sideSpeed;

    // Clamp movement values to prevent excessive speed
    float maxSpeed = forwardSpeed;
    cmd->forwardmove = clamp(cmd->forwardmove, -maxSpeed, maxSpeed);
    cmd->sidemove = clamp(cmd->sidemove, -maxSpeed, maxSpeed);

    // Debug output for movement values
    //DevMsg("VR Movement: Final values - forward=%.2f->%.2f side=%.2f->%.2f\n", 
    //       oldForward, cmd->forwardmove, oldSide, cmd->sidemove);
} 