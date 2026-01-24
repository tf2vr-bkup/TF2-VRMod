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
#include <game/client/iviewport.h>
#include "viewport_panel_names.h"

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

// Voice chat gesture ConVars (walkie-talkie style activation)
ConVar tfvr_voice_gesture_enabled( "tfvr_voice_gesture_enabled", "1", FCVAR_ARCHIVE, "Enable walkie-talkie style voice chat (hold offhand near ear and press trigger)" );
ConVar tfvr_voice_ear_radius( "tfvr_voice_ear_radius", "15.0", FCVAR_ARCHIVE, "Radius around left ear for voice chat activation (in game units)" );
ConVar tfvr_voice_ear_offset( "tfvr_voice_ear_offset", "8.0", FCVAR_ARCHIVE, "Lateral offset from head center to left ear (in game units)" );
ConVar tfvr_voice_gesture_debug( "tfvr_voice_gesture_debug", "0", FCVAR_ARCHIVE, "Show debug output for voice chat gesture detection" );

// Voice gesture state - used to suppress offhand attack when voice is active
static bool s_bVoiceGestureActive = false;

// Left thumbstick click: quick press = medic call, long hold = scoreboard
ConVar tfvr_medic_hold_threshold( "tfvr_medic_hold_threshold", "0.3", FCVAR_ARCHIVE, "Hold time in seconds before scoreboard shows (shorter = medic call)" );

// Primary hand ConVar (defined in vr_menu_manager.cpp)
extern ConVar tfvr_primary_hand;

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
    
    // Get left thumbstick click state (medic call on quick press, scoreboard on hold)
    static bool bLastScoreboardButtonState = false;
    static float flButtonPressTime = 0.0f;
    static bool bScoreboardShowing = false;
    
    bool bScoreboard = g_pOpenXRManager->IsButtonPressed("scoreboard");
    bool bScoreboardButtonPressed = bScoreboard && !bLastScoreboardButtonState;
    bool bScoreboardButtonReleased = !bScoreboard && bLastScoreboardButtonState;
    
    float flHoldThreshold = tfvr_medic_hold_threshold.GetFloat();
    
    // Handle left thumbstick click: quick press = medic, long hold = scoreboard
    if (bScoreboardButtonPressed)
    {
        // Record when button was pressed
        flButtonPressTime = gpGlobals->curtime;
        bScoreboardShowing = false;
    }
    else if (bScoreboard && !bScoreboardShowing)
    {
        // Button is being held - check if we've passed the threshold to show scoreboard
        float flHoldTime = gpGlobals->curtime - flButtonPressTime;
        if (flHoldTime >= flHoldThreshold)
        {
            // Show scoreboard after hold threshold
            if (gViewPortInterface)
            {
                gViewPortInterface->ShowPanel(PANEL_SCOREBOARD, true);
            }
            bScoreboardShowing = true;
        }
    }
    else if (bScoreboardButtonReleased)
    {
        float flHoldTime = gpGlobals->curtime - flButtonPressTime;
        
        if (bScoreboardShowing)
        {
            // Hide scoreboard when button is released after long hold
            if (gViewPortInterface)
            {
                gViewPortInterface->ShowPanel(PANEL_SCOREBOARD, false);
            }
        }
        else if (flHoldTime < flHoldThreshold)
        {
            // Quick press - call for medic
            engine->ClientCmd_Unrestricted("voicemenu 0 0");
        }
        
        bScoreboardShowing = false;
    }
    bLastScoreboardButtonState = bScoreboard;
    
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
    
    // =========================================================================
    // Voice chat activation
    // MUST be processed BEFORE attack inputs so we can suppress the offhand trigger
    // Uses the "voice" action which is bound to the left trigger by default.
    // 
    // When gesture mode is ENABLED (tfvr_voice_gesture_enabled 1):
    //   - Press trigger while controller is near left ear to activate voice
    //   - Voice stays active until trigger is released (even if moving out of range)
    //   - Moving into range while already holding trigger does NOT activate
    //
    // When gesture mode is DISABLED (tfvr_voice_gesture_enabled 0):
    //   - Simply pressing the voice action activates voice directly
    //   - No proximity check required
    //
    // In both modes, when voice is active, secondary fire is suppressed.
    // =========================================================================
    if (g_pOpenXRManager)
    {
        // Persistent state for voice action edge detection
        static bool bLastVoiceActionPressed = false;
        
        // Get the voice action value (bound to left trigger by default)
        float voiceActionValue = g_pOpenXRManager->GetAnalogValue("voice");
        bool bVoiceActionPressed = (voiceActionValue > 0.5f);
        bool bVoiceActionJustPressed = bVoiceActionPressed && !bLastVoiceActionPressed;
        bool bVoiceActionJustReleased = !bVoiceActionPressed && bLastVoiceActionPressed;
        
        if (tfvr_voice_gesture_enabled.GetBool())
        {
            // GESTURE MODE: Require proximity to left ear
            
            // Determine which hand is the offhand (opposite of primary hand)
            // tfvr_primary_hand: 0 = left primary (right is offhand), 1 = right primary (left is offhand)
            bool bLeftIsOffhand = (tfvr_primary_hand.GetInt() == 1);
            
            // Get HMD pose in playspace/chaperone coordinates
            Vector hmdOrigin;
            QAngle hmdAngles;
            g_pOpenXRManager->GetHMDInChaperone(hmdOrigin, hmdAngles);
            
            // Calculate left ear position (offset to the left of the head center)
            Vector hmdForward, hmdRight, hmdUp;
            AngleVectors(hmdAngles, &hmdForward, &hmdRight, &hmdUp);
            
            float earOffset = tfvr_voice_ear_offset.GetFloat();
            Vector leftEarPos = hmdOrigin - hmdRight * earOffset; // Negative right = left
            
            // Get offhand controller position in RAW playspace coordinates (same as HMD)
            Vector offhandPos;
            bool bOffhandValid = false;
            VMatrix offhandPose;
            
            if (bLeftIsOffhand)
            {
                if (g_pOpenXRManager->GetLeftControllerPoseRaw(offhandPose))
                {
                    offhandPos = offhandPose.GetTranslation();
                    bOffhandValid = true;
                }
            }
            else
            {
                if (g_pOpenXRManager->GetRightControllerPoseRaw(offhandPose))
                {
                    offhandPos = offhandPose.GetTranslation();
                    bOffhandValid = true;
                }
            }
            
            // Check proximity to left ear
            float earRadius = tfvr_voice_ear_radius.GetFloat();
            bool bNearEar = false;
            float distanceToEar = 0.0f;
            
            if (bOffhandValid)
            {
                distanceToEar = (offhandPos - leftEarPos).Length();
                bNearEar = (distanceToEar <= earRadius);
            }
            
            // Gesture activation logic:
            // - Activate ONLY when voice action is pressed (rising edge) while inside ear radius
            // - Stay active until voice action is released, regardless of position
            // - Moving into range while already holding doesn't activate
            
            if (bVoiceActionJustPressed && bNearEar && !s_bVoiceGestureActive)
            {
                s_bVoiceGestureActive = true;
                engine->ClientCmd_Unrestricted("+voicerecord\n");
                if (tfvr_voice_gesture_debug.GetBool())
                {
                    DevMsg("VR Voice: Activated (gesture mode, distance: %.1f)\n", distanceToEar);
                }
            }
            else if (bVoiceActionJustReleased && s_bVoiceGestureActive)
            {
                s_bVoiceGestureActive = false;
                engine->ClientCmd_Unrestricted("-voicerecord\n");
                if (tfvr_voice_gesture_debug.GetBool())
                {
                    DevMsg("VR Voice: Deactivated (gesture mode)\n");
                }
            }
            
            // Debug output
            if (tfvr_voice_gesture_debug.GetBool())
            {
                static float lastVoiceDebugTime = 0.0f;
                float currentTime = gpGlobals->curtime;
                
                if (currentTime - lastVoiceDebugTime > 0.5f)
                {
                    DevMsg("VR Voice: Mode=Gesture, Offhand=%s, Valid=%d, Distance=%.1f (radius=%.1f), Action=%.2f, NearEar=%d, Active=%d\n",
                           bLeftIsOffhand ? "Left" : "Right",
                           bOffhandValid ? 1 : 0,
                           distanceToEar,
                           earRadius,
                           voiceActionValue,
                           bNearEar ? 1 : 0,
                           s_bVoiceGestureActive ? 1 : 0);
                    lastVoiceDebugTime = currentTime;
                }
            }
        }
        else
        {
            // DIRECT MODE: Voice action directly activates voice (no proximity check)
            
            if (bVoiceActionJustPressed && !s_bVoiceGestureActive)
            {
                s_bVoiceGestureActive = true;
                engine->ClientCmd_Unrestricted("+voicerecord\n");
                if (tfvr_voice_gesture_debug.GetBool())
                {
                    DevMsg("VR Voice: Activated (direct mode)\n");
                }
            }
            else if (bVoiceActionJustReleased && s_bVoiceGestureActive)
            {
                s_bVoiceGestureActive = false;
                engine->ClientCmd_Unrestricted("-voicerecord\n");
                if (tfvr_voice_gesture_debug.GetBool())
                {
                    DevMsg("VR Voice: Deactivated (direct mode)\n");
                }
            }
            
            // Debug output
            if (tfvr_voice_gesture_debug.GetBool())
            {
                static float lastVoiceDebugTime = 0.0f;
                float currentTime = gpGlobals->curtime;
                
                if (currentTime - lastVoiceDebugTime > 0.5f)
                {
                    DevMsg("VR Voice: Mode=Direct, Action=%.2f, Active=%d\n",
                           voiceActionValue,
                           s_bVoiceGestureActive ? 1 : 0);
                    lastVoiceDebugTime = currentTime;
                }
            }
        }
        
        // Update action state for next frame
        bLastVoiceActionPressed = bVoiceActionPressed;
    }
    
    // Normal gameplay input processing (only when menu is not visible)
    
    // Determine which hand is offhand for voice gesture suppression
    // tfvr_primary_hand: 0 = left primary (right is offhand), 1 = right primary (left is offhand)
    bool bLeftIsOffhand = (tfvr_primary_hand.GetInt() == 1);
    
    // Primary attack - suppress if voice gesture is active AND right hand is offhand
    bool bPrimaryAttack = g_pOpenXRManager->GetAnalogValue("primary_attack") > 0.5f;
    bool bSuppressPrimary = s_bVoiceGestureActive && !bLeftIsOffhand; // Right is offhand, uses primary_attack
    if (bPrimaryAttack && !bSuppressPrimary)
        cmd->buttons |= IN_ATTACK;

    // Secondary attack - suppress if voice gesture is active AND left hand is offhand
    bool bSecondaryAttack = g_pOpenXRManager->GetAnalogValue("secondary_attack") > 0.5f;
    bool bSuppressSecondary = s_bVoiceGestureActive && bLeftIsOffhand; // Left is offhand, uses secondary_attack
    if (bSecondaryAttack && !bSuppressSecondary)
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

 