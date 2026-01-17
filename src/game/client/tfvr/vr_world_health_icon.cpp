// Purpose: VR World Health Icon - Renders floating health icons in 3D world
//          space above players' heads, always facing the player's view.

#include "cbase.h"
#include "vr_world_health_icon.h"
#include "c_tf_player.h"
#include "tf_hud_target_id.h"
#include "hudelement.h"
#include "hud.h"
#include "view.h"
#include "vgui/ISurface.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include "iclientmode.h"
#include "tier0/vprof.h"
#include "sourcevr/isourcevirtualreality.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//=============================================================================
// CVRHealthIconWrapper Implementation
//=============================================================================

DECLARE_BUILD_FACTORY(CVRHealthIconWrapper);

CVRHealthIconWrapper::CVRHealthIconWrapper(vgui::Panel* parent, const char* name)
    : BaseClass(parent, name)
{
    m_pTargetPanel = nullptr;
    SetPaintBackgroundEnabled(false);
}

void CVRHealthIconWrapper::Paint()
{
    if (!m_pTargetPanel)
        return;
    
    // Get our size and the target panel size
    int wrapperWidth, wrapperHeight;
    GetSize(wrapperWidth, wrapperHeight);
    
    int targetWidth, targetHeight;
    m_pTargetPanel->GetSize(targetWidth, targetHeight);
    
    // Calculate offset to center the target panel content within the wrapper
    int offsetX = (wrapperWidth - targetWidth) / 2;
    int offsetY = (wrapperHeight - targetHeight) / 2;
    
    // Temporarily make the target panel visible
    bool bWasVisible = m_pTargetPanel->IsVisible();
    m_pTargetPanel->SetVisible(true);
    
    // Disable clipping
    g_pMatSystemSurface->DisableClipping(true);
    
    // Apply offset to center the content, then paint the target panel
    vgui::surface()->ForceScreenPosOffset(true, offsetX, offsetY);
    vgui::surface()->PaintTraverse(m_pTargetPanel->GetVPanel());
    vgui::surface()->ForceScreenPosOffset(false, 0, 0);
    
    // Restore clipping
    g_pMatSystemSurface->DisableClipping(false);
    
    // Restore visibility
    m_pTargetPanel->SetVisible(bWasVisible);
}

// Global instance
CVRWorldHealthIconManager* g_pVRWorldHealthIconManager = nullptr;

// Static member for 3D rendering flag
bool CVRWorldHealthIconManager::s_bIsRendering3D = false;

//=============================================================================
// ConVars
//=============================================================================

ConVar tfvr_world_health_enabled("tfvr_world_health_enabled", "1", FCVAR_ARCHIVE, 
    "Enable VR world-space health icons above players");
ConVar tfvr_world_health_scale("tfvr_world_health_scale", "0.15", FCVAR_ARCHIVE, 
    "Scale of the world health icon (world height = scale * 100 units)");
ConVar tfvr_world_health_height_offset("tfvr_world_health_height_offset", "15", FCVAR_ARCHIVE, 
    "Additional height offset above player head");
ConVar tfvr_world_health_offset_x("tfvr_world_health_offset_x", "0", FCVAR_ARCHIVE, 
    "Horizontal offset in world units (positive = right)");
ConVar tfvr_world_health_offset_y("tfvr_world_health_offset_y", "0", FCVAR_ARCHIVE, 
    "Forward/back offset in world units (positive = toward player)");
ConVar tfvr_world_health_render_scale("tfvr_world_health_render_scale", "4", FCVAR_ARCHIVE, 
    "Resolution multiplier for sharper text (1-8, higher = sharper but more expensive)");

//=============================================================================
// CVRWorldHealthIconManager Implementation
//=============================================================================

//-----------------------------------------------------------------------------
CVRWorldHealthIconManager::CVRWorldHealthIconManager()
{
    m_bInitialized = false;
    m_bEnabled = true;
    m_pWrapper = nullptr;
    
    m_flScale = 0.15f;
    m_flHeightOffset = 15.0f;
    m_flOffsetX = 0.0f;
    m_flOffsetY = 0.0f;
    m_nRenderScale = 4;
}

//-----------------------------------------------------------------------------
CVRWorldHealthIconManager::~CVRWorldHealthIconManager()
{
    Shutdown();
}

//-----------------------------------------------------------------------------
bool CVRWorldHealthIconManager::Initialize()
{
    if (m_bInitialized)
        return true;
    
    // Create the wrapper panel - parent to the viewport
    vgui::Panel* pViewport = g_pClientMode ? g_pClientMode->GetViewport() : nullptr;
    m_pWrapper = new CVRHealthIconWrapper(pViewport, "VRHealthIconWrapper");
    m_pWrapper->SetBounds(0, 0, 512, 512);  // Large enough for high-res rendering
    m_pWrapper->SetVisible(false);  // Hidden normally, only visible during 3D render
    
    m_bInitialized = true;
    DevMsg("VR World Health Icon Manager: Initialized\n");
    
    return true;
}

//-----------------------------------------------------------------------------
void CVRWorldHealthIconManager::Shutdown()
{
    if (m_pWrapper)
    {
        m_pWrapper->MarkForDeletion();
        m_pWrapper = nullptr;
    }
    m_bInitialized = false;
}

//-----------------------------------------------------------------------------
void CVRWorldHealthIconManager::ResetState()
{
    // Nothing to reset currently
}

//-----------------------------------------------------------------------------
void CVRWorldHealthIconManager::Update()
{
    if (!m_bInitialized)
        return;
    
    // Update configuration from ConVars
    m_bEnabled = tfvr_world_health_enabled.GetBool();
    m_flScale = tfvr_world_health_scale.GetFloat();
    m_flHeightOffset = tfvr_world_health_height_offset.GetFloat();
    m_flOffsetX = tfvr_world_health_offset_x.GetFloat();
    m_flOffsetY = tfvr_world_health_offset_y.GetFloat();
    m_nRenderScale = clamp(tfvr_world_health_render_scale.GetInt(), 1, 8);
}

//-----------------------------------------------------------------------------
bool CVRWorldHealthIconManager::ShouldSuppressVanillaRendering()
{
    // Don't suppress if we're currently doing 3D rendering (let Paint() execute)
    if (s_bIsRendering3D)
        return false;
    
    // Suppress vanilla 2D rendering when VR world health is active
    return g_pVRWorldHealthIconManager && 
           g_pVRWorldHealthIconManager->m_bInitialized && 
           g_pVRWorldHealthIconManager->m_bEnabled &&
           UseVR();
}

//-----------------------------------------------------------------------------
Vector CVRWorldHealthIconManager::GetEntityHeadPosition(C_BaseEntity* pEntity)
{
    if (!pEntity)
        return vec3_origin;
    
    Vector vecTarget = pEntity->GetAbsOrigin();
    
    // Get the entity's bounding box height and add offset
    C_BaseAnimating* pAnimating = pEntity->GetBaseAnimating();
    if (pAnimating)
    {
        vecTarget.z += VEC_HULL_MAX_SCALED(pAnimating).z;
    }
    
    // Add the configurable height offset
    vecTarget.z += m_flHeightOffset;
    
    // Add any entity-specific health bar offset
    vecTarget.z += pEntity->GetHealthBarHeightOffset();
    
    return vecTarget;
}

//-----------------------------------------------------------------------------
bool CVRWorldHealthIconManager::CalculateBillboardTransform(C_BaseEntity* pEntity, VMatrix& transform)
{
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer || !pEntity)
        return false;
    
    // Get the world position above the entity's head
    Vector panelPos = GetEntityHeadPosition(pEntity);
    
    // Get the player's eye position for billboarding
    Vector vecEyePos = MainViewOrigin();
    
    // Calculate direction from panel to player (for billboarding)
    Vector forward = vecEyePos - panelPos;
    forward.z = 0; // Keep the icon upright (only rotate around Z axis)
    
    // Handle edge case where player is directly above/below
    if (forward.LengthSqr() < 0.001f)
    {
        forward = Vector(1, 0, 0);
    }
    VectorNormalize(forward);
    
    // Build orientation vectors
    Vector up(0, 0, 1);  // Keep upright
    Vector right;
    CrossProduct(up, forward, right);
    VectorNormalize(right);
    
    // Recalculate up to ensure orthogonality
    CrossProduct(forward, right, up);
    VectorNormalize(up);
    
    // Panel faces the player (negative forward direction)
    // Same pattern as vr_spring_hud.cpp
    Vector panelForward = -forward;  // Face the player
    Vector panelRight = right;
    Vector panelUp = up;
    
    // Apply manual offsets (in world units relative to billboard orientation)
    // X offset: positive = right
    // Y offset: positive = toward player (forward)
    panelPos += panelRight * m_flOffsetX;
    panelPos += forward * m_flOffsetY;  // Use forward (toward player), not panelForward
    
    // Build rotation matrix (column-major for VMatrix) - same as vr_spring_hud.cpp
    transform.Identity();
    transform[0][0] = panelRight.x;  transform[0][1] = panelUp.x;  transform[0][2] = panelForward.x;
    transform[1][0] = panelRight.y;  transform[1][1] = panelUp.y;  transform[1][2] = panelForward.y;
    transform[2][0] = panelRight.z;  transform[2][1] = panelUp.z;  transform[2][2] = panelForward.z;
    transform.SetTranslation(panelPos);
    
    return true;
}

//-----------------------------------------------------------------------------
void CVRWorldHealthIconManager::Render()
{
    VPROF("VRWorldHealthIconManager_Render");
    
    if (!m_bInitialized || !m_bEnabled || !m_pWrapper)
        return;
    
    if (!UseVR())
        return;
    
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer || pPlayer->IsObserver())
        return;
    
    // Check if player is actually aiming at a valid target
    int iIDTarget = pPlayer->GetIDTarget();
    if (iIDTarget <= 0)
        return;
    
    // Get the main target ID HUD element to access the floating health icon
    CMainTargetID* pTargetID = (CMainTargetID*)GET_HUDELEMENT(CMainTargetID);
    if (!pTargetID)
        return;
    
    // Get the floating health icon directly from the target ID
    CFloatingHealthIcon* pFloatingIcon = pTargetID->GetFloatingHealthIcon();
    if (!pFloatingIcon)
        return;
    
    // Get the entity this icon is tracking
    C_BaseEntity* pEntity = pFloatingIcon->GetEntity();
    if (!pEntity || pEntity->IsDormant())
        return;
    
    // Get the actual panel size and scale up for higher resolution text
    int basePanelWidth, basePanelHeight;
    pFloatingIcon->GetSize(basePanelWidth, basePanelHeight);
    
    // Use actual panel size, or fallback if panel reports 0
    if (basePanelWidth <= 0 || basePanelHeight <= 0)
    {
        basePanelWidth = 48;
        basePanelHeight = 48;
    }
    
    // Scale up for sharper text
    int renderWidth = basePanelWidth * m_nRenderScale;
    int renderHeight = basePanelHeight * m_nRenderScale;
    
    // Calculate billboard transform (this gives us the center position)
    VMatrix panelToWorld;
    if (!CalculateBillboardTransform(pEntity, panelToWorld))
        return;
    
    // Calculate distance from player to target for consistent sizing
    Vector panelPos = GetEntityHeadPosition(pEntity);
    Vector eyePos = MainViewOrigin();
    float distance = (panelPos - eyePos).Length();
    
    // Scale world size based on distance to keep apparent size consistent
    // At reference distance (100 units), use the base scale
    // At other distances, scale proportionally
    const float referenceDistance = 100.0f;
    float distanceScale = distance / referenceDistance;
    
    // Calculate world dimensions (based on base size, not render size)
    float aspectRatio = (float)basePanelWidth / (float)basePanelHeight;
    float worldHeight = m_flScale * 100.0f * distanceScale;
    float worldWidth = worldHeight * aspectRatio;
    
    // DrawPanelIn3DSpace draws from top-left corner, but we want the panel centered
    // Extract the orientation vectors from the transform to offset properly
    Vector panelRight(panelToWorld[0][0], panelToWorld[1][0], panelToWorld[2][0]);
    Vector panelUp(panelToWorld[0][1], panelToWorld[1][1], panelToWorld[2][1]);
    Vector currentPos = panelToWorld.GetTranslation();
    
    // Offset from center to top-left: move left by half width, up by half height
    Vector topLeftPos = currentPos - panelRight * (worldWidth * 0.5f) + panelUp * (worldHeight * 0.5f);
    panelToWorld.SetTranslation(topLeftPos);
    
    // Make sure the wrapper is sized correctly for high-res rendering
    m_pWrapper->SetSize(renderWidth, renderHeight);
    
    // Set the target panel for the wrapper to render
    m_pWrapper->SetTargetPanel(pFloatingIcon);
    
    // Set flag to allow Paint() to execute during 3D rendering
    s_bIsRendering3D = true;
    
    // Make wrapper visible for rendering
    m_pWrapper->SetVisible(true);
    
    // Render the wrapper panel in 3D world space
    // renderWidth/Height = high-res pixels for sharp text
    // worldWidth/Height = same world size, so it appears the same size but sharper
    g_pMatSystemSurface->DrawPanelIn3DSpace(
        m_pWrapper->GetVPanel(),
        panelToWorld,
        renderWidth,
        renderHeight,
        worldWidth,
        worldHeight
    );
    
    // Hide wrapper
    m_pWrapper->SetVisible(false);
    
    // Clear the flag
    s_bIsRendering3D = false;
}
