#include "cbase.h"
#include "vr_ammo_overlay.h"

// Include VGUI headers first to ensure types are defined
#include "vgui/ISurface.h"
#include <vgui/IScheme.h>
#include <vgui/IVGui.h>
#include <vgui_controls/Controls.h>
#include <vgui_controls/EditablePanel.h>
#include <vgui_controls/Label.h>
#include <vgui_controls/ImagePanel.h>
#include "tf_controls.h"

// Now include other headers
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
#include "KeyValues.h"
#include "ienginevgui.h"
#include "hud.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include "tf_shareddefs.h"

// Include TF2 headers last, after VGUI types are defined
#include "tf/tf_hud_ammostatus.h"
#include "tf/tf_weaponbase.h"

//-----------------------------------------------------------------------------
// Purpose: VR-only ammo panel that doesn't draw to main HUD
//-----------------------------------------------------------------------------
class CVRAmmoPanel : public CTFHudWeaponAmmo
{
    DECLARE_CLASS_SIMPLE(CVRAmmoPanel, CTFHudWeaponAmmo);

public:
    CVRAmmoPanel(const char* pElementName) : CTFHudWeaponAmmo(pElementName) {}
    
    // Override ShouldDraw to prevent drawing to main HUD
    virtual bool ShouldDraw() override
    {
        // Never draw to main HUD - only render via DrawPanelIn3DSpace
        return false;
    }
    
    // Public method to check if we WOULD draw (for VR overlay logic)
    bool ShouldDrawInVR()
    {
        return CTFHudWeaponAmmo::ShouldDraw();
    }
};

// ConVars for configuration
ConVar tfvr_ammo_overlay_enabled("tfvr_ammo_overlay_enabled", "1", FCVAR_ARCHIVE, "Enable VR ammo overlay on hand");
ConVar tfvr_ammo_overlay_hand("tfvr_ammo_overlay_hand", "1", FCVAR_ARCHIVE, "Hand to attach ammo overlay to: 0=left, 1=right (should be main shooting hand)");
ConVar tfvr_ammo_overlay_use_hand_tracking("tfvr_ammo_overlay_use_hand_tracking", "1", FCVAR_ARCHIVE, "Use hand tracking instead of controller pose: 0=controller, 1=hand tracking");
ConVar tfvr_ammo_overlay_offset_x("tfvr_ammo_overlay_offset_x", "0", FCVAR_ARCHIVE, "X offset from hand position");
ConVar tfvr_ammo_overlay_offset_y("tfvr_ammo_overlay_offset_y", "8", FCVAR_ARCHIVE, "Y offset from hand position (up)");
ConVar tfvr_ammo_overlay_offset_z("tfvr_ammo_overlay_offset_z", "3", FCVAR_ARCHIVE, "Z offset from hand position (forward)");
ConVar tfvr_ammo_overlay_scale("tfvr_ammo_overlay_scale", "0.8", FCVAR_ARCHIVE, "Scale of ammo overlay");
ConVar tfvr_ammo_overlay_panel_x("tfvr_ammo_overlay_panel_x", "50", FCVAR_ARCHIVE, "Panel X position within capture area");
ConVar tfvr_ammo_overlay_panel_y("tfvr_ammo_overlay_panel_y", "50", FCVAR_ARCHIVE, "Panel Y position within capture area");
ConVar tfvr_ammo_overlay_debug_bg("tfvr_ammo_overlay_debug_bg", "0", FCVAR_ARCHIVE, "Show debug background to see quad boundaries");
ConVar tfvr_ammo_overlay_simple_transform("tfvr_ammo_overlay_simple_transform", "0", FCVAR_ARCHIVE, "Use simple identity transform for debugging");
ConVar tfvr_ammo_overlay_no_rotation("tfvr_ammo_overlay_no_rotation", "0", FCVAR_ARCHIVE, "Skip final 180-degree rotation for debugging");
ConVar tfvr_ammo_overlay_world_width("tfvr_ammo_overlay_world_width", "0", FCVAR_ARCHIVE, "Override world width (0=auto)");
ConVar tfvr_ammo_overlay_panel_width("tfvr_ammo_overlay_panel_width", "512", FCVAR_ARCHIVE, "Panel capture width in pixels");
ConVar tfvr_ammo_overlay_panel_height("tfvr_ammo_overlay_panel_height", "256", FCVAR_ARCHIVE, "Panel capture height in pixels");
ConVar tfvr_ammo_overlay_wrist_back("tfvr_ammo_overlay_wrist_back", "1.5", FCVAR_ARCHIVE, "Distance behind wrist (toward forearm)");
ConVar tfvr_ammo_overlay_wrist_up("tfvr_ammo_overlay_wrist_up", "0.5", FCVAR_ARCHIVE, "Distance above wrist bone");
ConVar tfvr_ammo_overlay_wrist_side("tfvr_ammo_overlay_wrist_side", "0.3", FCVAR_ARCHIVE, "Distance toward thumb side");
ConVar tfvr_ammo_overlay_pitch("tfvr_ammo_overlay_pitch", "-45", FCVAR_ARCHIVE, "Pitch rotation (up/down tilt) in degrees");
ConVar tfvr_ammo_overlay_yaw("tfvr_ammo_overlay_yaw", "90", FCVAR_ARCHIVE, "Yaw rotation (left/right turn) in degrees");
ConVar tfvr_ammo_overlay_roll("tfvr_ammo_overlay_roll", "0", FCVAR_ARCHIVE, "Roll rotation (twist) in degrees");

// Global instance
CVRAmmoOverlay* g_pVRAmmoOverlay = nullptr;

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CVRAmmoOverlay::CVRAmmoOverlay()
{
    m_bInitialized = false;
    m_bEnabled = false;
    m_nAttachedHand = 1; // Default to right hand (main shooting hand for most players)
    m_nLastAmmo = -1;
    m_nLastReserveAmmo = -1;
    m_nLastMaxAmmo = -1;
    m_bLastUsesClips = false;
    m_flLastUpdateTime = 0.0f;
    m_pAmmoPanel = nullptr;
    
    // Set default offsets (slightly different from health overlay)
    m_vQuadOffset.Init(
        tfvr_ammo_overlay_offset_x.GetFloat(),
        tfvr_ammo_overlay_offset_y.GetFloat(), 
        tfvr_ammo_overlay_offset_z.GetFloat()
    );
    m_angQuadRotation.Init(0, 0, 0);
}

//-----------------------------------------------------------------------------
// Purpose: Destructor
//-----------------------------------------------------------------------------
CVRAmmoOverlay::~CVRAmmoOverlay()
{
    Shutdown();
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the overlay system
//-----------------------------------------------------------------------------
bool CVRAmmoOverlay::Initialize()
{
    if (m_bInitialized)
        return true;

    // Create VR-only ammo panel that won't draw to main HUD
    m_pAmmoPanel = new CVRAmmoPanel("VRAmmoOverlay");
    if (!m_pAmmoPanel)
    {
        return false;
    }

    // Set up the panel
    m_pAmmoPanel->SetVisible(true);
    m_pAmmoPanel->SetPos(0, 0);
    m_pAmmoPanel->SetSize(400, 200);

    // Apply scheme settings for proper TF2 styling
    vgui::IScheme* pScheme = vgui::scheme()->GetIScheme(vgui::scheme()->GetDefaultScheme());
    if (pScheme)
    {
        m_pAmmoPanel->ApplySchemeSettings(pScheme);
    }

    m_bEnabled = tfvr_ammo_overlay_enabled.GetBool();
    m_nAttachedHand = tfvr_ammo_overlay_hand.GetInt();

    m_bInitialized = true;
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown and cleanup
//-----------------------------------------------------------------------------
void CVRAmmoOverlay::Shutdown()
{
    if (!m_bInitialized)
        return;
    
    if (m_pAmmoPanel)
    {
        delete m_pAmmoPanel;
        m_pAmmoPanel = nullptr;
    }
    
    m_bInitialized = false;
}

//-----------------------------------------------------------------------------
// Purpose: Update each frame - checks for ammo changes
//-----------------------------------------------------------------------------
void CVRAmmoOverlay::Update()
{
    if (!m_bInitialized)
        return;

    // Update settings from ConVars
    m_bEnabled = tfvr_ammo_overlay_enabled.GetBool();
    m_nAttachedHand = tfvr_ammo_overlay_hand.GetInt();

    // Update offsets from ConVars
    m_vQuadOffset.Init(
        tfvr_ammo_overlay_offset_x.GetFloat(),
        tfvr_ammo_overlay_offset_y.GetFloat(),
        tfvr_ammo_overlay_offset_z.GetFloat()
    );

    // No need to update ammo manually - we sample directly from the HUD render target
}

//-----------------------------------------------------------------------------
// Purpose: Render the ammo panel in 3D space
//-----------------------------------------------------------------------------
void CVRAmmoOverlay::RenderAmmoQuad()
{
    VPROF("VR_AmmoOverlay_Render");
    
    if (!m_bInitialized || !m_bEnabled || !m_pAmmoPanel)
        return;
        
    // Safety check: Don't render if there's no valid player or we're in spectator mode
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer || pPlayer->IsObserver() || !pPlayer->IsAlive())
    {
        return;
    }
    
    // Check if the ammo panel should be drawn using VR-specific logic
    if (!m_pAmmoPanel->ShouldDrawInVR())
    {
        return;
    }
        
    // Calculate panel-to-world transform based on hand position
    VMatrix panelToWorld;
    
    if (tfvr_ammo_overlay_simple_transform.GetBool())
    {
        // Simple identity transform for debugging - just put it in front of player
        panelToWorld.Identity();
        C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
        if (pPlayer)
        {
            Vector playerPos = pPlayer->EyePosition();
            playerPos += Vector(100, -50, 0); // 100 units in front, 50 units down
            panelToWorld.SetTranslation(playerPos);
        }
    }
    else if (!CalculateQuadTransform(panelToWorld))
    {
        return;
    }
    
    // Simple panel dimensions
    int panelWidth = 400;
    int panelHeight = 200;
    
    // Calculate world size based on scale ConVar
    float scale = tfvr_ammo_overlay_scale.GetFloat();
    float worldWidth = 0.15f * scale;  // 15cm base width
    float worldHeight = (worldWidth * panelHeight) / panelWidth;  // Maintain aspect ratio
    
    // Allow override of world width
    if (tfvr_ammo_overlay_world_width.GetFloat() > 0.0f)
    {
        worldWidth = tfvr_ammo_overlay_world_width.GetFloat();
        worldHeight = (worldWidth * panelHeight) / panelWidth;
    }
    
    // Use DrawPanelIn3DSpace to render the ammo panel in world space
    g_pMatSystemSurface->DrawPanelIn3DSpace(
        m_pAmmoPanel->GetVPanel(),  // The VR ammo panel to render
        panelToWorld,               // Transform matrix (panel center to world)
        panelWidth,                 // Panel pixel width
        panelHeight,                // Panel pixel height  
        worldWidth,                 // World width (meters)
        worldHeight                 // World height (meters)
    );
}

//-----------------------------------------------------------------------------
// Purpose: Force the ammo panel to update by triggering the same logic as OnThink
//-----------------------------------------------------------------------------
void CVRAmmoOverlay::ForceAmmoUpdate()
{
    // The ammo panel updates automatically via OnThink, but we can
    // force a layout update if needed
    if (m_pAmmoPanel)
    {
        m_pAmmoPanel->InvalidateLayout(true);
        m_pAmmoPanel->PerformLayout();
    }
}

//-----------------------------------------------------------------------------
// Purpose: Set which hand the overlay is attached to
//-----------------------------------------------------------------------------
void CVRAmmoOverlay::SetHandAttachment(int hand)
{
    m_nAttachedHand = clamp(hand, 0, 1);
}

//-----------------------------------------------------------------------------
// Purpose: Calculate the quad transform matrix based on hand position
//-----------------------------------------------------------------------------
bool CVRAmmoOverlay::CalculateQuadTransform(VMatrix& quadTransform)
{
    // Check if we should use hand tracking instead of controller
    if (tfvr_ammo_overlay_use_hand_tracking.GetBool())
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
    
    // Get the appropriate hand grip pose
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
    
    // Position like a tactical display behind the wrist
    // Instead of using generic offset, position specifically for "pistol grip" viewing
    Vector quadPos = handPos + 
                     right * m_vQuadOffset.x +       // Side offset (slightly toward thumb side)
                     up * m_vQuadOffset.y +          // Up/down (toward wrist)  
                     forward * m_vQuadOffset.z;      // Forward/back (behind wrist)
    
    // Use the exact controller pose matrix directly
    quadTransform = handPose;
    quadTransform.SetTranslation(quadPos);
    
    // Apply rotations for tactical display orientation (face forward/up for pistol grip viewing)
    if (m_angQuadRotation.x != 0 || m_angQuadRotation.y != 0 || m_angQuadRotation.z != 0)
    {
        VMatrix rotationMatrix;
        QAngle totalRotation = m_angQuadRotation;
        // Add tactical display orientation from ConVars
        totalRotation.x += tfvr_ammo_overlay_pitch.GetFloat();  // Pitch: Tilt up/down
        totalRotation.y += tfvr_ammo_overlay_yaw.GetFloat();    // Yaw: Turn left/right
        totalRotation.z += tfvr_ammo_overlay_roll.GetFloat();   // Roll: Twist
        
        matrix3x4_t rotMatrix;
        AngleMatrix(totalRotation, Vector(0,0,0), rotMatrix);
        rotationMatrix.CopyFrom3x4(rotMatrix);
        
        // Apply rotation on top of hand pose
        quadTransform = quadTransform * rotationMatrix;
    }
    else
    {
        // Default tactical display orientation using ConVars
        VMatrix adjustMatrix;
        matrix3x4_t adjustMatrix3x4;
        QAngle tacticalAngles(
            tfvr_ammo_overlay_pitch.GetFloat(),  // Pitch up/down
            tfvr_ammo_overlay_yaw.GetFloat(),    // Yaw left/right
            tfvr_ammo_overlay_roll.GetFloat()    // Roll twist
        );
        AngleMatrix(tacticalAngles, Vector(0,0,0), adjustMatrix3x4);
        adjustMatrix.CopyFrom3x4(adjustMatrix3x4);
        
        quadTransform = quadTransform * adjustMatrix;
    }
    
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Get current player ammo information
//-----------------------------------------------------------------------------
bool CVRAmmoOverlay::GetPlayerAmmoInfo(int& currentAmmo, int& reserveAmmo, int& maxAmmo, bool& usesClips)
{
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer)
        return false;
        
    CTFWeaponBase* pWeapon = pPlayer->GetActiveTFWeapon();
    if (!pWeapon || !pWeapon->UsesPrimaryAmmo())
        return false;
    
    // Get ammo information
    currentAmmo = pWeapon->Clip1();
    reserveAmmo = 0;
    usesClips = (currentAmmo >= 0);
    
    if (usesClips)
    {
        // Weapon uses clips - reserve ammo is total ammo
        reserveAmmo = pPlayer->GetAmmoCount(pWeapon->GetPrimaryAmmoType());
    }
    else
    {
        // No clips - current ammo is total ammo
        currentAmmo = pPlayer->GetAmmoCount(pWeapon->GetPrimaryAmmoType());
    }
    
    maxAmmo = pPlayer->GetMaxAmmo(pWeapon->GetPrimaryAmmoType());
    if (usesClips && pWeapon->GetMaxClip1() > 0)
    {
        maxAmmo += pWeapon->GetMaxClip1();
    }
    
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Calculate transform using hand tracking instead of controller
//-----------------------------------------------------------------------------
bool CVRAmmoOverlay::CalculateHandTrackingTransform(VMatrix& quadTransform)
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
    
    // For ammo overlay, position at wrist like a tactical display
    if (wristValid)
    {
        // Use actual wrist position as base instead of palm
        quadPosition = wristPosition;
        
        // Position behind the wrist for "pistol grip" style viewing
        Vector wristOffset = -handForward * tfvr_ammo_overlay_wrist_back.GetFloat() +    // Behind wrist (toward forearm)
                            handUp * tfvr_ammo_overlay_wrist_up.GetFloat() +              // Above wrist bone
                            handRight * tfvr_ammo_overlay_wrist_side.GetFloat();          // Toward thumb side
        quadPosition += wristOffset;
    }
    else
    {
        // Fallback to palm-based positioning if wrist tracking unavailable
        Vector backOfHandOffset = -handForward * 2.0f + handUp * 1.0f;
        quadPosition += backOfHandOffset;
    }
    
    // Apply ConVar offsets in hand coordinate space
    Vector userOffset(
        tfvr_ammo_overlay_offset_x.GetFloat(),
        tfvr_ammo_overlay_offset_y.GetFloat(), 
        tfvr_ammo_overlay_offset_z.GetFloat()
    );
    
    // Transform user offset to hand coordinate space
    Vector worldOffset = handRight * userOffset.x + handUp * userOffset.y + handForward * userOffset.z;
    quadPosition += worldOffset;
    
    // Create the quad transform matrix
    quadTransform.Identity();
    quadTransform.SetTranslation(quadPosition);
    
    // Create a matrix from the palm angles
    matrix3x4_t handMatrix;
    AngleMatrix(palmAngles, Vector(0,0,0), handMatrix);
    
    VMatrix handVMatrix;
    handVMatrix.CopyFrom3x4(handMatrix);
    
    // Apply the hand orientation
    quadTransform = quadTransform * handVMatrix;
    
    // Apply tactical display rotation for wrist-mounted viewing
    if (m_angQuadRotation.x != 0 || m_angQuadRotation.y != 0 || m_angQuadRotation.z != 0)
    {
        VMatrix rotationMatrix;
        QAngle totalRotation = m_angQuadRotation;
        // Add tactical display orientation from ConVars
        totalRotation.x += tfvr_ammo_overlay_pitch.GetFloat();  // Pitch up/down
        totalRotation.y += tfvr_ammo_overlay_yaw.GetFloat();    // Yaw left/right
        totalRotation.z += tfvr_ammo_overlay_roll.GetFloat();   // Roll twist
        
        matrix3x4_t rotMatrix;
        AngleMatrix(totalRotation, Vector(0,0,0), rotMatrix);
        rotationMatrix.CopyFrom3x4(rotMatrix);
        quadTransform = quadTransform * rotationMatrix;
    }
    else if (!tfvr_ammo_overlay_no_rotation.GetBool())
    {
        // Default tactical display orientation using ConVars
        VMatrix tacticalMatrix;
        matrix3x4_t tacticalMatrix3x4;
        QAngle tacticalAngles(
            tfvr_ammo_overlay_pitch.GetFloat(),  // Pitch up/down
            tfvr_ammo_overlay_yaw.GetFloat(),    // Yaw left/right
            tfvr_ammo_overlay_roll.GetFloat()    // Roll twist
        );
        AngleMatrix(tacticalAngles, Vector(0,0,0), tacticalMatrix3x4);
        tacticalMatrix.CopyFrom3x4(tacticalMatrix3x4);
        quadTransform = quadTransform * tacticalMatrix;
    }
     
    return true;
}
