#include "cbase.h"
#include "vr_input.h"
#include "in_buttons.h"
#include "prediction.h"
#include "mathlib/mathlib.h"
#include "iinput.h"
#include "iclientmode.h"
#include "input.h"
#include "convar.h"
#include "vr_menu_manager.h"
#include "engine/ivdebugoverlay.h"
#include "tf/c_tf_player.h"
#include "c_tfvr_hand.h"

// Engine interface for executing client commands
extern IVEngineClient *engine;

// ConVars for input sensitivity and deadzone
ConVar tfvr_move_sensitivity("tfvr_move_sensitivity", "1.0", FCVAR_ARCHIVE, "Sensitivity multiplier for VR movement");
ConVar tfvr_thumbstick_deadzone("tfvr_thumbstick_deadzone", "0.1", FCVAR_ARCHIVE, "Deadzone for thumbstick movement");
ConVar tfvr_use_hmd_angles("tfvr_use_hmd_angles", "0", FCVAR_ARCHIVE, "Use HMD angles for view");

// Controller tracking ConVars
ConVar tfvr_enable_controller_tracking("tfvr_enable_controller_tracking", "1", FCVAR_ARCHIVE, "Enable VR controller position and orientation tracking");
ConVar tfvr_controller_tracking_debug("tfvr_controller_tracking_debug", "0", FCVAR_ARCHIVE, "Show debug output for controller tracking");

// VR Turning ConVars
ConVar tfvr_turning_mode( "tfvr_turning_mode", "1", FCVAR_ARCHIVE, "VR turning mode: 0=disabled, 1=smooth, 2=snap" );
ConVar tfvr_smooth_turn_rate( "tfvr_smooth_turn_rate", "120", FCVAR_ARCHIVE, "Smooth turning rate in degrees per second" );
ConVar tfvr_snap_turn_angle( "tfvr_snap_turn_angle", "45", FCVAR_ARCHIVE, "Snap turning angle in degrees" );
ConVar tfvr_turn_deadzone( "tfvr_turn_deadzone", "0.3", FCVAR_ARCHIVE, "Deadzone for turning input (0.0-1.0)" );
ConVar tfvr_snap_turn_delay( "tfvr_snap_turn_delay", "0.25", FCVAR_ARCHIVE, "Delay between snap turns in seconds" );

// Weapon switching ConVars
ConVar tfvr_weapon_switch_stick_enabled( "tfvr_weapon_switch_stick_enabled", "0", FCVAR_ARCHIVE, "Enable right stick up/down weapon switching (0=disabled, 1=enabled). Disabled by default since radial weapon select menu is available." );
ConVar tfvr_weapon_switch_tilt_threshold( "tfvr_weapon_switch_tilt_threshold", "0.7", FCVAR_ARCHIVE, "Tilt threshold for weapon switching (0.0-1.0)" );
ConVar tfvr_weapon_switch_debug( "tfvr_weapon_switch_debug", "0", FCVAR_ARCHIVE, "Show debug output for weapon switching actions" );

// Movement speed ConVars
extern ConVar cl_forwardspeed;
extern ConVar cl_sidespeed;

// Define global instances
CVRInput g_VRInput;
IInput* g_OriginalNonVRInputPtr = nullptr;

// Global variable for snap turning (shared with in_main.cpp)
float s_flLastSnapTurnTime = 0.0f;

CVRInput::CVRInput()
{
    // Constructor - no initialization needed
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
        
        // Process VR controller tracking
        ProcessVRControllerTracking(cmd);
        
        // Process VR view angles to use controller angles for aiming
        ProcessVRViewAngles(cmd);
        
        // Process VR movement
        ProcessVRMovement(cmd, input_sample_frametime);
        
        // VR turning is now handled in the main input system
    }

    // CopyVRPosesToUserCmd(cmd);

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
    CUserCmd dummy;
    CUserCmd *cmd = &dummy;

    CInput::ExtraMouseSample(frametime, active);

   cmd->Reset();

   QAngle viewangles;
   engine->GetViewAngles(viewangles);
   QAngle originalViewangles = viewangles;

   if (active)
   {
       // Process VR-specific input
       ProcessVRControllerInput(cmd);
        
       // Process VR controller tracking
       ProcessVRControllerTracking(cmd);
        
       // Only process VR view angles if explicitly enabled
       if (tfvr_use_hmd_angles.GetBool())
       {
           // ProcessVRViewAngles(cmd);
       }
       
       // Process VR movement
       ProcessVRMovement(cmd, frametime);
       
       // VR turning is now handled in the main input system
   }

   // Retreive view angles from engine ( could have been set in IN_AdjustAngles above )
   engine->GetViewAngles(viewangles);

   cmd->buttons = GetButtonBits(0);

   VectorCopy(m_angPreviousViewAngles, cmd->viewangles);

   //CopyVRPosesToUserCmd(cmd);

   g_bExtraMouseSample = true;

   // Let the move manager override anything it wants to.
   if (g_pClientMode->CreateMove(frametime, cmd))
   {
       // Get current view angles after the client mode tweaks with it
       //engine->SetViewAngles( cmd->viewangles );
       prediction->SetLocalViewAngles(cmd->viewangles);
   }

   g_bExtraMouseSample = false;
}

void CVRInput::ProcessVRControllerInput(CUserCmd* cmd)
{
    // Store original button state
    int oldButtons = cmd->buttons;
    
    // Get current menu button state first
    bool bMenu = g_pOpenXRManager->IsButtonPressed("menu");
    
    // Track button press events
    static bool bLastMenuButtonState = false;
    bool bMenuButtonPressed = bMenu && !bLastMenuButtonState;
    
    // Check if menu is visible
    bool bMenuVisible = g_pVRMenuManager && g_pVRMenuManager->IsMenuVisible();
    
    // Track menu state changes
    static bool lastMenuState = false;
    if (bMenuVisible != lastMenuState)
    {
        lastMenuState = bMenuVisible;
    }
    
    // Handle menu button press to toggle menu state
    if (bMenuButtonPressed)
    {
        if (bMenuVisible)
        {
            engine->ClientCmd_Unrestricted("gameui_hide\n");
        }
        else
        {
            engine->ClientCmd_Unrestricted("gameui_activate\n");
        }
    }
    
    // If menu is visible, block all gameplay actions except menu controls
    if (bMenuVisible)
    {
        // Menu interaction (cursor control) is handled separately by the menu manager
        bLastMenuButtonState = bMenu;
        return;
    }
    
    // Normal gameplay input processing (only when menu is not visible)
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

    // Weapon switching
    float weaponSwitchValue = g_pOpenXRManager->GetAnalogValue("weapon_switch");
    
    // Validate that we got valid values
    if (weaponSwitchValue == 0.0f)
    {
        // This could indicate the actions aren't properly bound or working
        static float lastWarningTime = 0.0f;
        float currentTime = gpGlobals->curtime;
        
        if (currentTime - lastWarningTime > 5.0f) // Only warn every 5 seconds
        {
            if (tfvr_weapon_switch_debug.GetBool())
            {
                DevMsg("VR: Warning - Weapon switching action returning 0.0 values. Check OpenXR bindings.\n");
            }
            lastWarningTime = currentTime;
        }
    }
    
    // Track weapon switching button press events to avoid continuous execution
    static bool bLastNextWeaponState = false;
    static bool bLastPrevWeaponState = false;
    
    // Threshold for detecting tilt (0.7 = 70% tilt)
    const float TILT_THRESHOLD = tfvr_weapon_switch_tilt_threshold.GetFloat();
    
    // Detect forward tilt (positive Y) for next weapon
    bool bNextWeaponActive = weaponSwitchValue > TILT_THRESHOLD;
    // Detect backward tilt (negative Y) for previous weapon
    bool bPrevWeaponActive = weaponSwitchValue < -TILT_THRESHOLD;
    
    bool bNextWeaponPressed = bNextWeaponActive && !bLastNextWeaponState;
    bool bPrevWeaponPressed = bPrevWeaponActive && !bLastPrevWeaponState;
    
    // Check if stick weapon switching is enabled (disabled by default, use radial menu instead)
    if (tfvr_weapon_switch_stick_enabled.GetBool())
    {
        // Check if weapon switching is allowed before executing commands
        C_TFPlayer* pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
        bool bCanSwitchWeapons = pLocalPlayer && pLocalPlayer->IsAllowedToSwitchWeapons();
        
        if (bNextWeaponPressed && bCanSwitchWeapons)
        {
            engine->ExecuteClientCmd("invnext");
            if (tfvr_weapon_switch_debug.GetBool())
            {
                DevMsg("VR: Next weapon triggered (tilt: %.2f, threshold: %.2f)\n", weaponSwitchValue, TILT_THRESHOLD);
            }
        }
        else if (bNextWeaponPressed && !bCanSwitchWeapons)
        {
            if (tfvr_weapon_switch_debug.GetBool())
            {
                DevMsg("VR: Next weapon blocked - weapon switching not allowed\n");
            }
        }
        
        if (bPrevWeaponPressed && bCanSwitchWeapons)
        {
            engine->ExecuteClientCmd("invprev");
            if (tfvr_weapon_switch_debug.GetBool())
            {
                DevMsg("VR: Previous weapon triggered (tilt: %.2f, threshold: %.2f)\n", weaponSwitchValue, TILT_THRESHOLD);
            }
        }
        else if (bPrevWeaponPressed && !bCanSwitchWeapons)
        {
            if (tfvr_weapon_switch_debug.GetBool())
            {
                DevMsg("VR: Previous weapon blocked - weapon switching not allowed\n");
            }
        }
    }
    
    // Update weapon switching button state tracking
    bLastNextWeaponState = bNextWeaponActive;
    bLastPrevWeaponState = bPrevWeaponActive;
    
    // Debug output for weapon switching values
    if (tfvr_weapon_switch_debug.GetBool())
    {
        static float lastDebugTime = 0.0f;
        float currentTime = gpGlobals->curtime;
        
        // Only output debug info every 0.5 seconds to avoid spam
        if (currentTime - lastDebugTime > 0.5f)
        {
            const char* direction = "neutral";
            if (weaponSwitchValue > TILT_THRESHOLD)
                direction = "forward (next)";
            else if (weaponSwitchValue < -TILT_THRESHOLD)
                direction = "backward (prev)";
            
            DevMsg("VR: Weapon switch - Y-axis: %.2f, Direction: %s, Threshold: %.2f\n", 
                   weaponSwitchValue, direction, TILT_THRESHOLD);
            lastDebugTime = currentTime;
        }
    }

    // Update button state tracking
    bLastMenuButtonState = bMenu;
}

void CVRInput::ProcessVRViewAngles(CUserCmd* cmd)
{
    // Check if controller tracking is enabled
    if (!tfvr_enable_controller_tracking.GetBool())
        return;

    // Check if menu is visible - if so, disable view angle changes
    bool bMenuVisible = g_pVRMenuManager && g_pVRMenuManager->IsMenuVisible();
    if (bMenuVisible)
    {
        // Don't process view angle changes when menu is open
        return;
    }

    // Get right controller pose for aiming (typically the shooting hand)
    VMatrix rightControllerPose;
    if (g_pOpenXRManager->GetRightControllerPose(rightControllerPose))
    {
        // Extract angles from the controller pose matrix
        QAngle controllerAngles;
        MatrixAngles(rightControllerPose.As3x4(), controllerAngles);
        
        // We DON'T change cmd->viewangles here because:
        // - cmd->viewangles affects locomotion (should use headset)
        // - Weapon_ShootAngles() handles shooting direction (uses controller)
        // This keeps locomotion and shooting completely separate

        // Debug output for controller angles
        if (tfvr_controller_tracking_debug.GetBool())
        {
            DevMsg("VR Aim: Using controller angles - pitch=%.1f yaw=%.1f roll=%.1f\n", 
                   controllerAngles.x, controllerAngles.y, controllerAngles.z);
        }
    }
}

void CVRInput::ProcessVRMovement(CUserCmd* cmd, float frametime)
{
    // Check if menu is visible - if so, disable movement
    bool bMenuVisible = g_pVRMenuManager && g_pVRMenuManager->IsMenuVisible();
    if (bMenuVisible)
    {
        // Don't process any movement when menu is open
        return;
    }
    
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

void CVRInput::ProcessVRControllerTracking(CUserCmd* cmd)
{
    // Check if controller tracking is enabled
    if (!tfvr_enable_controller_tracking.GetBool())
        return;
    
    // Check if menu is visible - if so, disable tracking
    bool bMenuVisible = g_pVRMenuManager && g_pVRMenuManager->IsMenuVisible();
    if (bMenuVisible)
    {
        // Don't process controller tracking when menu is open
        return;
    }
    
    // Get controller poses
    VMatrix leftControllerPose, rightControllerPose;
    bool leftValid = g_pOpenXRManager->GetLeftControllerPose(leftControllerPose);
    bool rightValid = g_pOpenXRManager->GetRightControllerPose(rightControllerPose);
    
    
    
    // Store controller poses in the command for use by other systems
    if (leftValid)
    {
        // Extract position and orientation from the pose matrix
        Vector leftPos = leftControllerPose.GetTranslation();
        QAngle leftAngles;
        MatrixAngles(leftControllerPose.As3x4(), leftAngles);
        
        // Store in command for weapon shooting synchronization
        cmd->leftControllerOrigin = leftPos;
        cmd->leftControllerAngles = leftAngles;
        
        // Debug output
        if (tfvr_controller_tracking_debug.GetBool())
        {
            DevMsg("Left Controller: pos(%.2f, %.2f, %.2f) angles(%.1f, %.1f, %.1f)\n", 
                   leftPos.x, leftPos.y, leftPos.z, leftAngles.x, leftAngles.y, leftAngles.z);
        }
        

    }
    
    if (rightValid)
    {
        // Extract position and orientation from the pose matrix
        Vector rightPos = rightControllerPose.GetTranslation();
        QAngle rightAngles;
        MatrixAngles(rightControllerPose.As3x4(), rightAngles);
        
        // Check if right hand is holding a weapon - if so, send muzzle position/angles
        C_TFVRHand* pRightHand = GetLocalPlayerRightHand();
        if (pRightHand && pRightHand->GetHeldWeapon())
        {
            Vector muzzlePos;
            QAngle muzzleAngles;
            if (pRightHand->GetWeaponMuzzlePositionAndAngles(muzzlePos, muzzleAngles))
            {
                // Send weapon muzzle position AND angles for server-side hit detection
                cmd->rightControllerOrigin = muzzlePos;
                cmd->rightControllerAngles = muzzleAngles;
                
                if (tfvr_controller_tracking_debug.GetBool())
                {
                    DevMsg("Right Hand: Weapon muzzle pos(%.2f, %.2f, %.2f), angles(%.1f, %.1f, %.1f)\n", 
                           muzzlePos.x, muzzlePos.y, muzzlePos.z,
                           muzzleAngles.x, muzzleAngles.y, muzzleAngles.z);
                }
                return; // Early return, we've set the values
            }
        }
        
        // Fallback: No weapon held, use controller position/angles
        cmd->rightControllerOrigin = rightPos;
        cmd->rightControllerAngles = rightAngles;
        
        if (tfvr_controller_tracking_debug.GetBool())
        {
            DevMsg("Right Controller: pos(%.2f, %.2f, %.1f) angles(%.1f, %.1f, %.1f)\n", 
                   rightPos.x, rightPos.y, rightPos.z, rightAngles.x, rightAngles.y, rightAngles.z);
        }
    }
    
    // Laser pointer functionality is now handled by CVRLaserPointer class
}

 