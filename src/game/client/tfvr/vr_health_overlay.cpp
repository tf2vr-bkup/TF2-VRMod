//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: VR Health Overlay - Renders health panel in 3D space attached to hand
//
//=============================================================================//

#include "cbase.h"
#include "vr_health_overlay.h"
#include "c_tf_player.h"
#include "openxr_manager.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "materialsystem/imaterialsystem.h"
#include "bitmap/imageformat.h"
#include "view_shared.h"
#include "convar.h"
#include "tier0/dbg.h"
#include "hudelement.h"
#include "tf_hud_playerstatus.h"
#include "vgui/ISurface.h"
#include "KeyValues.h"
#include "ienginevgui.h"
#include "hud.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include <vgui/IScheme.h>

// ConVars for configuration
ConVar tfvr_health_overlay_enabled("tfvr_health_overlay_enabled", "1", FCVAR_ARCHIVE, "Enable VR health overlay on hand");
ConVar tfvr_health_overlay_hand("tfvr_health_overlay_hand", "0", FCVAR_ARCHIVE, "Hand to attach health overlay to: 0=left, 1=right");
ConVar tfvr_health_overlay_use_hand_tracking("tfvr_health_overlay_use_hand_tracking", "1", FCVAR_ARCHIVE, "Use hand tracking instead of controller pose: 0=controller, 1=hand tracking");
ConVar tfvr_health_overlay_offset_x("tfvr_health_overlay_offset_x", "0", FCVAR_ARCHIVE, "X offset from hand position");
ConVar tfvr_health_overlay_offset_y("tfvr_health_overlay_offset_y", "10", FCVAR_ARCHIVE, "Y offset from hand position (up)");
ConVar tfvr_health_overlay_offset_z("tfvr_health_overlay_offset_z", "5", FCVAR_ARCHIVE, "Z offset from hand position (forward)");
ConVar tfvr_health_overlay_scale("tfvr_health_overlay_scale", "1.0", FCVAR_ARCHIVE, "Scale of health overlay");
ConVar tfvr_health_overlay_panel_x("tfvr_health_overlay_panel_x", "100", FCVAR_ARCHIVE, "Panel X position within capture area");
ConVar tfvr_health_overlay_panel_y("tfvr_health_overlay_panel_y", "100", FCVAR_ARCHIVE, "Panel Y position within capture area");
ConVar tfvr_health_overlay_debug_bg("tfvr_health_overlay_debug_bg", "0", FCVAR_ARCHIVE, "Show debug background to see quad boundaries");
ConVar tfvr_health_overlay_simple_transform("tfvr_health_overlay_simple_transform", "0", FCVAR_ARCHIVE, "Use simple identity transform for debugging");
ConVar tfvr_health_overlay_no_rotation("tfvr_health_overlay_no_rotation", "0", FCVAR_ARCHIVE, "Skip final 180-degree rotation for debugging");
ConVar tfvr_health_overlay_world_width("tfvr_health_overlay_world_width", "0", FCVAR_ARCHIVE, "Override world width (0=auto)");

// Global instance
CVRHealthOverlay* g_pVRHealthOverlay = nullptr;

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CVRHealthOverlay::CVRHealthOverlay()
{
    m_bInitialized = false;
    m_bEnabled = false;
    m_nAttachedHand = 0; // Default to left hand
    m_flLastHealthPercent = -1.0f;
    m_nLastHealth = -1;
    m_nLastMaxHealth = -1;
    m_flLastUpdateTime = 0.0f;
    m_pPlayerStatusPanel = nullptr;
    
    // Set default offsets
    m_vQuadOffset.Init(
        tfvr_health_overlay_offset_x.GetFloat(),
        tfvr_health_overlay_offset_y.GetFloat(), 
        tfvr_health_overlay_offset_z.GetFloat()
    );
    m_angQuadRotation.Init(0, 0, 0);
}

//-----------------------------------------------------------------------------
// Purpose: Destructor
//-----------------------------------------------------------------------------
CVRHealthOverlay::~CVRHealthOverlay()
{
    Shutdown();
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the overlay system
//-----------------------------------------------------------------------------
bool CVRHealthOverlay::Initialize()
{
    if (m_bInitialized)
        return true;
        
    
    // Create the full CTFHudPlayerStatus widget (includes health + class icon)
    m_pPlayerStatusPanel = new CTFHudPlayerStatus("VRPlayerStatus");
    if (!m_pPlayerStatusPanel)
    {
        return false;
    }

    // Set up the panel with safe initialization
    try 
    {
        m_pPlayerStatusPanel->SetVisible(true);
        m_pPlayerStatusPanel->SetPos(0, 0);
        m_pPlayerStatusPanel->SetSize(200, 200);
        
        // Apply scheme settings to load the UI layout
        vgui::IScheme* pScheme = vgui::scheme()->GetIScheme(vgui::scheme()->GetDefaultScheme());
        if (pScheme)
        {
            m_pPlayerStatusPanel->ApplySchemeSettings(pScheme);
        }
        
        // Force a layout update to position child elements properly
        m_pPlayerStatusPanel->PerformLayout();
        m_pPlayerStatusPanel->InvalidateLayout(true);
    }
    catch (...)
    {
        delete m_pPlayerStatusPanel;
        m_pPlayerStatusPanel = nullptr;
        return false;
    }
    
    m_bEnabled = tfvr_health_overlay_enabled.GetBool();
    m_nAttachedHand = tfvr_health_overlay_hand.GetInt();
    
    m_bInitialized = true;
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown and cleanup
//-----------------------------------------------------------------------------
void CVRHealthOverlay::Shutdown()
{
    if (!m_bInitialized)
        return;
    
    if (m_pPlayerStatusPanel)
    {
        delete m_pPlayerStatusPanel;
        m_pPlayerStatusPanel = nullptr;
    }
    
    m_bInitialized = false;
}

//-----------------------------------------------------------------------------
// Purpose: Update each frame - checks for health changes
//-----------------------------------------------------------------------------
void CVRHealthOverlay::Update()
{
    if (!m_bInitialized)
        return;
        
    // Update settings from ConVars
    m_bEnabled = tfvr_health_overlay_enabled.GetBool();
    m_nAttachedHand = tfvr_health_overlay_hand.GetInt();
    
    // Update offsets from ConVars
    m_vQuadOffset.Init(
        tfvr_health_overlay_offset_x.GetFloat(),
        tfvr_health_overlay_offset_y.GetFloat(), 
        tfvr_health_overlay_offset_z.GetFloat()
    );
    
    // Health updates will be handled by the panel itself
}

//-----------------------------------------------------------------------------
// Purpose: Render the health panel in 3D space
//-----------------------------------------------------------------------------
void CVRHealthOverlay::RenderHealthQuad()
{
    VPROF("VR_HealthOverlay_Render");
    
    if (!m_bInitialized || !m_bEnabled || !m_pPlayerStatusPanel)
        return;
        
    // Safety check: Don't render if there's no valid player or we're in spectator mode
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer || pPlayer->IsObserver() || !pPlayer->IsAlive())
    {
        return;
    }
    
    // Additional safety: Check for reasonable health values
    int health = pPlayer->GetHealth();
    int maxHealth = pPlayer->GetMaxHealth();
    if (health <= 0 || maxHealth <= 0)
    {
        return;
    }
        
    // Calculate panel-to-world transform based on hand position
    VMatrix panelToWorld;
    
    if (tfvr_health_overlay_simple_transform.GetBool())
    {
        // Simple identity transform for debugging - just put it in front of player
        panelToWorld.Identity();
        C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
        if (pPlayer)
        {
            Vector playerPos = pPlayer->EyePosition();
            playerPos += Vector(100, 0, 0); // 100 units in front
            panelToWorld.SetTranslation(playerPos);
        }
    }
    else if (!CalculateQuadTransform(panelToWorld))
    {
        return;
    }
    
    // Square aspect ratio capture area for testing
    int panelWidth = 1024;   // Square format
    int panelHeight = 1024;  // Square format
    
    // Square world size to match square capture area
    float scale = tfvr_health_overlay_scale.GetFloat();
    float worldWidth = 0.6f * scale;   // 60cm square
    float worldHeight = 0.6f * scale;  // 60cm square
    
    
    // Allow manual override of world width for debugging
    if (tfvr_health_overlay_world_width.GetFloat() > 0)
    {
        worldWidth = tfvr_health_overlay_world_width.GetFloat();
    }
    
    // Make sure the panel is visible and positioned
    m_pPlayerStatusPanel->SetVisible(true);
    
    // Position the widget within the larger capture area using ConVars
    int posX = tfvr_health_overlay_panel_x.GetInt();
    int posY = tfvr_health_overlay_panel_y.GetInt();
    m_pPlayerStatusPanel->SetPos(posX, posY);
    
    // Give it a reasonable size - the panel needs SOME size to render into
    m_pPlayerStatusPanel->SetSize(panelWidth, panelHeight);
    
    // Force the panel to recalculate its layout
    m_pPlayerStatusPanel->InvalidateLayout(true);
    
    // Healing numbers should be handled by the main HUD system
    
    // Debug: Print panel info
    int w, h;
    m_pPlayerStatusPanel->GetSize(w, h);
    
    // Update the health panel with current values (we already validated health values above)
    if (m_pPlayerStatusPanel)
    {
        // The CTFHudPlayerStatus widget automatically updates via the HUD system
        // No manual health setting needed
        
        // Debug: Check if the widget is valid and has content
        if (m_pPlayerStatusPanel->IsVisible())
        {
        }
        else
        {
            m_pPlayerStatusPanel->SetVisible(true);
        }
        
        // Make sure the panel is ready for 3D rendering
        m_pPlayerStatusPanel->SetVisible(true);
        m_pPlayerStatusPanel->InvalidateLayout(true);
    }
    
    // Optional debug background to see quad boundaries
    if (tfvr_health_overlay_debug_bg.GetBool())
    {
        // Set the panel background to a solid color so we can see the quad boundaries
        m_pPlayerStatusPanel->SetBgColor(Color(0, 255, 0, 128)); // Semi-transparent green
        m_pPlayerStatusPanel->SetPaintBackgroundEnabled(true);
    }
    else
    {
        // Normal transparent background
        m_pPlayerStatusPanel->SetPaintBackgroundEnabled(false);
    }
    
    // Use DrawPanelIn3DSpace to render the panel directly in world space
    g_pMatSystemSurface->DrawPanelIn3DSpace(
        m_pPlayerStatusPanel->GetVPanel(),  // The VGUI panel to render
        panelToWorld,                 // Transform matrix (panel center to world)
        panelWidth,                   // Panel pixel width
        panelHeight,                  // Panel pixel height  
        worldWidth,                   // World width (meters)
        worldHeight                   // World height (meters)
    );
}

//-----------------------------------------------------------------------------
// Purpose: Set which hand the overlay is attached to
//-----------------------------------------------------------------------------
void CVRHealthOverlay::SetHandAttachment(int hand)
{
    m_nAttachedHand = clamp(hand, 0, 1);
}

//-----------------------------------------------------------------------------
// Purpose: Calculate the quad transform matrix based on hand position
//-----------------------------------------------------------------------------
bool CVRHealthOverlay::CalculateQuadTransform(VMatrix& quadTransform)
{
    // Check if we should use hand tracking instead of controller
    if (tfvr_health_overlay_use_hand_tracking.GetBool())
    {
        return CalculateHandTrackingTransform(quadTransform);
    }
    
    // Use controller grip pose (legacy mode)
    if (!g_pOpenXRManager)
    {
        return false;
    }
        
    VMatrix handPose;
    bool handValid = false;
    
    // Get the appropriate hand grip pose (more natural for overlays)
    if (m_nAttachedHand == 0) // Left hand
    {
        if (g_pOpenXRManager->IsLeftControllerPoseValid())
        {
            handValid = g_pOpenXRManager->GetLeftControllerGripPose(handPose);
        }
    }
    else // Right hand
    {
        if (g_pOpenXRManager->IsRightControllerPoseValid())
        {
            handValid = g_pOpenXRManager->GetRightControllerGripPose(handPose);
        }
    }
    
    if (!handValid)
    {
        return false;
    }
    
    // Get hand position and orientation
    Vector handPos = handPose.GetTranslation();
    Vector forward, right, up;
    handPose.GetBasisVectors(forward, right, up);
    
    // Apply offset to position the quad relative to the hand
    Vector quadPos = handPos + 
                     right * m_vQuadOffset.x + 
                     up * m_vQuadOffset.y + 
                     forward * m_vQuadOffset.z;
    
    // Use the exact controller pose matrix directly - no auto-rotation
    quadTransform = handPose;
    
    // Just update the position to the offset position we calculated
    quadTransform.SetTranslation(quadPos);
    
    // Apply any additional rotation from ConVars if needed
    if (m_angQuadRotation.x != 0 || m_angQuadRotation.y != 0 || m_angQuadRotation.z != 0)
    {
        VMatrix rotationMatrix;
        QAngle totalRotation = m_angQuadRotation;
        totalRotation.x += -90;  // Pitch to stand up from flat
        totalRotation.y += -90;  // Yaw to face backward instead of forward
        totalRotation.z += 90;   // Roll to align widget top with controller top (blue axis)
        
        matrix3x4_t rotMatrix;
        AngleMatrix(totalRotation, Vector(0,0,0), rotMatrix);
        rotationMatrix.CopyFrom3x4(rotMatrix);
        
        // Apply rotation on top of hand pose
        quadTransform = quadTransform * rotationMatrix;
    }
    else
    {
        // Apply rotations for proper orientation
        VMatrix adjustMatrix;
        matrix3x4_t adjustMatrix3x4;
        AngleMatrix(QAngle(-90, -90, 90), Vector(0,0,0), adjustMatrix3x4);  // Pitch -90 to stand up, Yaw -90 to face backward, Roll 90 to align top
        adjustMatrix.CopyFrom3x4(adjustMatrix3x4);
        
        quadTransform = quadTransform * adjustMatrix;
    }
    
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Get current player health information
//-----------------------------------------------------------------------------
bool CVRHealthOverlay::GetPlayerHealthInfo(float& healthPercent, int& currentHealth, int& maxHealth)
{
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer)
        return false;
        
    currentHealth = pPlayer->GetHealth();
    maxHealth = pPlayer->GetMaxHealth();
    
    if (maxHealth <= 0)
        return false;
        
    healthPercent = (float)currentHealth / (float)maxHealth;
    healthPercent = clamp(healthPercent, 0.0f, 2.0f); // Allow overheal up to 200%
    
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Calculate transform using hand tracking instead of controller
//-----------------------------------------------------------------------------
bool CVRHealthOverlay::CalculateHandTrackingTransform(VMatrix& quadTransform)
{
    if (!g_pOpenXRManager)
    {
        return false;
    }
    
    COpenXRHandTracker* handTracker = g_pOpenXRManager->GetHandTracker();
    if (!handTracker)
    {
        return false;
    }
    
    // Get hand joint data - use palm as the base position
    Vector palmPosition, wristPosition;
    QAngle palmAngles, wristAngles;
    bool leftHand = (m_nAttachedHand == 0);
    
    // Check if the hand is being tracked
    bool handTracked = leftHand ? handTracker->IsLeftHandTracked() : handTracker->IsRightHandTracked();
    if (!handTracked)
    {
        return false;
    }
    
    // Get palm joint (primary position reference)
    bool palmValid = handTracker->GetHandJoint(leftHand, XR_HAND_JOINT_PALM_EXT, palmPosition, palmAngles);
    if (!palmValid)
    {
        return false;
    }
    
    // Get wrist joint for orientation reference
    bool wristValid = handTracker->GetHandJoint(leftHand, XR_HAND_JOINT_WRIST_EXT, wristPosition, wristAngles);
    
    // Calculate position above the back of the hand
    Vector quadPosition = palmPosition;
    
    // Use wrist-to-palm vector to determine hand orientation if available
    Vector handForward, handRight, handUp;
    if (wristValid)
    {
        // Vector from wrist to palm gives us the hand forward direction (towards fingers)
        handForward = (palmPosition - wristPosition).Normalized();
        
        // Use palm angles to get the proper hand coordinate frame
        AngleVectors(palmAngles, nullptr, &handRight, &handUp);
        
        // Ensure right-handed coordinate system
        if (!leftHand)
        {
            handRight = -handRight; // Right hand uses inverted right vector
        }
    }
    else
    {
        // Fallback to just palm orientation if wrist not available
        AngleVectors(palmAngles, &handForward, &handRight, &handUp);
        if (!leftHand)
        {
            handRight = -handRight; // Right hand uses inverted right vector
        }
    }
    
    // Position the overlay above the back of the hand
    // The "back" of the hand is opposite to the palm direction
    Vector backOfHandOffset = -handForward * 2.0f + handUp * 1.0f;  // 2 units back, 1 unit up
    quadPosition += backOfHandOffset;
    
    // Apply ConVar offsets in hand coordinate space
    Vector userOffset(
        tfvr_health_overlay_offset_x.GetFloat(),
        tfvr_health_overlay_offset_y.GetFloat(), 
        tfvr_health_overlay_offset_z.GetFloat()
    );
    
    // Transform user offset to hand coordinate space
    Vector worldOffset = handRight * userOffset.x + handUp * userOffset.y + handForward * userOffset.z;
    quadPosition += worldOffset;
    
    // Create the quad transform matrix
    quadTransform.Identity();
    quadTransform.SetTranslation(quadPosition);
    
    // Set orientation to face upward from the back of the hand
    // Keep it simple - use the palm orientation directly
    
    // Create a matrix from the palm angles (this was working before)
    matrix3x4_t handMatrix;
    AngleMatrix(palmAngles, Vector(0,0,0), handMatrix);
    
    VMatrix handVMatrix;
    handVMatrix.CopyFrom3x4(handMatrix);
    
    // Apply the hand orientation
    quadTransform = quadTransform * handVMatrix;
    
    // Apply additional rotation to make the widget face upward from the back of the hand
    if (m_angQuadRotation.x != 0 || m_angQuadRotation.y != 0 || m_angQuadRotation.z != 0)
    {
        VMatrix rotationMatrix;
        QAngle totalRotation = m_angQuadRotation;
        
        matrix3x4_t rotMatrix;
        AngleMatrix(totalRotation, Vector(0,0,0), rotMatrix);
        rotationMatrix.CopyFrom3x4(rotMatrix);
        quadTransform = quadTransform * rotationMatrix;
    }
    else if (!tfvr_health_overlay_no_rotation.GetBool())
    {
        // Default: flip the widget 180° so it faces the correct direction
        VMatrix flipMatrix;
        matrix3x4_t flipMatrix3x4;
        AngleMatrix(QAngle(0, 0, 180), Vector(0,0,0), flipMatrix3x4);  // 180° roll to flip
        flipMatrix.CopyFrom3x4(flipMatrix3x4);
        quadTransform = quadTransform * flipMatrix;
    }
     
    return true;
}
