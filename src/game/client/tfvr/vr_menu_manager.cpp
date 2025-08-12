#include "cbase.h"
#include "vr_menu_manager.h"
#include "c_tf_player.h"
#include "openxr_manager.h"
#include "ienginevgui.h"
#include "vgui/IInput.h"
#include "vgui/IInputInternal.h"
#include "vgui/ISurface.h"
#include "vgui_controls/Controls.h"
#include "mathlib/mathlib.h"
#include "convar.h"
#include <algorithm>

// Global instances
CVRMenuManager* g_pVRMenuManager = nullptr;
extern vgui::IInputInternal *g_InputInternal;

// ConVars for VR menu control
ConVar tfvr_primary_hand("tfvr_primary_hand", "1", FCVAR_ARCHIVE, "Primary hand for VR input: 0=left, 1=right");
ConVar tfvr_menu_distance("tfvr_menu_distance", "100", FCVAR_ARCHIVE, "Distance from player to VR menu plane");

CVRMenuManager::CVRMenuManager()
    : m_bMenuButtonPressed(false)
    , m_bMenuButtonReleased(false)
    , m_nMenuHand(1) // Default to right hand
    , m_nOldCursorX(-1)
    , m_nOldCursorY(-1)
    , m_pVRManager(nullptr)
    , m_pLocalPlayer(nullptr)
    , m_savedPlayerViewOrigin(0, 0, 0)
    , m_fixedMenuPosition(0, 0, 0)
    , m_fixedMenuRotation(0, 0, 0)
    , m_bMenuPositionFixed(false)
    , m_pConVarPrimaryHand(nullptr)
{
}

CVRMenuManager::~CVRMenuManager()
{
    Shutdown();
}

void CVRMenuManager::Initialize()
{
    m_pConVarPrimaryHand = cvar->FindVar("tfvr_primary_hand");
    if (m_pConVarPrimaryHand)
    {
        m_nMenuHand = m_pConVarPrimaryHand->GetInt();
    }
    
    DevMsg("VR Menu Manager initialized\n");
}

void CVRMenuManager::Shutdown()
{
    m_pVRManager = nullptr;
    m_pLocalPlayer = nullptr;
    DevMsg("VR Menu Manager shutdown\n");
}

void CVRMenuManager::Update()
{
    if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
        return;

    m_pVRManager = g_pOpenXRManager;
    m_pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
    
    if (m_pLocalPlayer)
    {
        // Save the current view origin for menu input
        m_savedPlayerViewOrigin = m_pLocalPlayer->EyePosition();
    }
    
    HandleMenuInput();
}

void CVRMenuManager::HandleMenuInput()
{
    bool menuVisible = enginevgui && enginevgui->IsGameUIVisible();
    
    // If menu just became visible, capture the fixed position
    if (menuVisible && !m_bMenuPositionFixed)
    {
        if (m_pLocalPlayer)
        {
            m_fixedMenuPosition = m_pLocalPlayer->EyePosition();
            m_fixedMenuRotation = m_pLocalPlayer->EyeAngles();
            m_fixedMenuRotation.x = 0;
            m_fixedMenuRotation.z = 0;

            m_bMenuPositionFixed = true;
            
            // Set custom HUD bounds to override the VR system's head-based positioning
            float menuDistance = tfvr_menu_distance.GetFloat();
            Vector forward, right, up;
            AngleVectors(m_fixedMenuRotation, &forward, &right, &up);
            Vector menuPlaneCenter;
            VectorMA(m_fixedMenuPosition, menuDistance, forward, menuPlaneCenter);
            
            // Create a quad representing the menu plane at the fixed position
            // Use the actual screen aspect ratio for proper proportions
            int screenWidth, screenHeight;
            vgui::surface()->GetScreenSize(screenWidth, screenHeight);
            float aspectRatio = (float)screenWidth / (float)screenHeight;
            
            // Base the menu size on the distance and maintain aspect ratio
            float menuHeight = menuDistance * 0.6f; // Menu height
            float menuWidth = menuHeight * aspectRatio; // Menu width (maintain aspect ratio)
            
            Vector ul = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (menuHeight * 0.5f);
            Vector ur = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (menuHeight * 0.5f);
            Vector ll = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
            Vector lr = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
            
            // Set the custom HUD bounds in the VR system
            g_ClientVirtualReality.SetCustomHUDBounds(m_fixedMenuPosition, ul, ur, ll, lr);
            
            DevMsg("VR Menu: Fixed position captured, menu locked in place\n");
        }
    }
    // If menu just became hidden, reset the fixed position
    else if (!menuVisible && m_bMenuPositionFixed)
    {
        m_bMenuPositionFixed = false;
        
        // Clear the custom HUD bounds to restore normal VR system behavior
        g_ClientVirtualReality.ClearCustomHUDBounds();
        
        DevMsg("VR Menu: Fixed position reset, menu unlocked\n");
    }
    
    if (!menuVisible || m_savedPlayerViewOrigin == Vector(0, 0, 0))
    {
        return;
    }

    if (!m_pLocalPlayer || !m_pVRManager)
        return;

    // Handle menu button input
    HandleMenuButtonInput();
    
    // Update cursor position
    UpdateCursorPosition();
}

bool CVRMenuManager::IsMenuVisible()
{
    return enginevgui && enginevgui->IsGameUIVisible();
}

void CVRMenuManager::HandleMenuButtonInput()
{
    // Check for menu press on both hands
    bool leftMenuPress = m_pVRManager->IsButtonPressed("menu_press");
    bool rightMenuPress = m_pVRManager->IsButtonPressed("menu_press");
    
    // Determine which hand is being used
    if (leftMenuPress && !m_bMenuButtonPressed)
    {
        m_nMenuHand = 0; // Left hand
        m_bMenuButtonPressed = true;
        
        if (g_InputInternal)
        {
            g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_PRESSED);
            g_InputInternal->InternalMousePressed(MOUSE_LEFT);
        }
    }
    else if (rightMenuPress && !m_bMenuButtonPressed)
    {
        m_nMenuHand = 1; // Right hand
        m_bMenuButtonPressed = true;
        
        if (g_InputInternal)
        {
            g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_PRESSED);
            g_InputInternal->InternalMousePressed(MOUSE_LEFT);
        }
    }
    
    // Handle button release
    if (!leftMenuPress && !rightMenuPress && m_bMenuButtonPressed)
    {
        m_bMenuButtonPressed = false;
        
        if (g_InputInternal)
        {
            g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_RELEASED);
            g_InputInternal->InternalMouseReleased(MOUSE_LEFT);
        }
    }
}

void CVRMenuManager::UpdateCursorPosition()
{
    if (!m_pLocalPlayer || !m_bMenuPositionFixed)
        return;

    // Use the fixed menu position and rotation for cursor calculations
    // This keeps the menu in a stable position even when the player looks around
    Vector pointerPosition = m_fixedMenuPosition;
    QAngle pointerRotation = m_fixedMenuRotation;
    
    // Compute cursor position using the fixed menu position
    int px, py;
    ComputeCursorPosition(pointerPosition, pointerRotation, px, py);
    
    // Update cursor if position changed
    if ((px != m_nOldCursorX) || (py != m_nOldCursorY))
    {
        if (g_InputInternal)
        {
            g_InputInternal->InternalCursorMoved(px, py);
        }
        
        if (vgui::input())
        {
            vgui::input()->SetCursorPos(px, py);
        }
        
        m_nOldCursorX = px;
        m_nOldCursorY = py;

    }
}

void CVRMenuManager::ComputeCursorPosition(const Vector& pointerPosition, const QAngle& pointerRotation, int& px, int& py)
{
    // Get the current eye angles (actual VR head rotation)
    QAngle currentViewAngles = m_pLocalPlayer->EyeAngles();
    
    // Create ray from the player's current head position in the direction they're looking
    // This provides natural, intuitive cursor control based on where the player is looking from
    Vector rayDir;
    AngleVectors(currentViewAngles, &rayDir);
    VectorNormalize(rayDir);
    
    Vector currentEyePos = m_pLocalPlayer->EyePosition();
    Vector rayStart = currentEyePos; // Start from current head position
    Vector rayEnd;
    VectorMA(currentEyePos, 1000.0f, rayDir, rayEnd);
    
    // Recreate the fixed menu plane using the same calculations as when we set it
    float menuDistance = tfvr_menu_distance.GetFloat();
    Vector forward, right, up;
    AngleVectors(m_fixedMenuRotation, &forward, &right, &up);
    Vector menuPlaneCenter;
    VectorMA(m_fixedMenuPosition, menuDistance, forward, menuPlaneCenter);
    
    // Use the same aspect ratio calculation as when we set the HUD bounds
    int screenWidth, screenHeight;
    vgui::surface()->GetScreenSize(screenWidth, screenHeight);
    float aspectRatio = (float)screenWidth / (float)screenHeight;
    
    float menuHeight = menuDistance * 0.6f;
    float menuWidth = menuHeight * aspectRatio;
    
    // Calculate menu plane corners
    Vector ul = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (menuHeight * 0.5f);
    Vector ur = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (menuHeight * 0.5f);
    Vector ll = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
    Vector lr = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
    
    // Compute intersection with the fixed menu plane
    float u, v;
    if (ComputeIntersectionBarycentricCoordinates(rayStart, rayEnd, ul, ur, ll, lr, u, v))
    {
        // Clamp UV coordinates to valid range
        u = max(0.0f, min(1.0f, u));
        v = max(0.0f, min(1.0f, v));
        
        // Convert (u,v) to screen coordinates
        px = (int)(u * screenWidth + 0.5f);
        py = (int)(v * screenHeight + 0.5f);
        
        // Clamp screen coordinates to valid range
        px = max(0, min(screenWidth - 1, px));
        py = max(0, min(screenHeight - 1, py));
        return;
    }
    
    // If no intersection, use center of screen
    px = screenWidth / 2;
    py = screenHeight / 2;
}

bool CVRMenuManager::ComputeIntersectionBarycentricCoordinates(const Vector& rayStart, const Vector& rayEnd, 
                                                              const Vector& ul, const Vector& ur, const Vector& ll, const Vector& lr,
                                                              float& u, float& v)
{
    // Compute ray direction
    Vector rayDir = rayEnd - rayStart;
    VectorNormalize(rayDir);
    
    // Compute the plane normal from the quad
    Vector edge1 = ur - ul;
    Vector edge2 = ll - ul;
    Vector planeNormal = CrossProduct(edge1, edge2);
    VectorNormalize(planeNormal);
    
    // Check if ray is parallel to plane
    float denominator = DotProduct(rayDir, planeNormal);
    if (fabs(denominator) < 0.0001f)
        return false;
    
    // Find intersection point with plane
    Vector planePoint = ul;
    float t = DotProduct(planePoint - rayStart, planeNormal) / denominator;
    
    if (t < 0)
        return false; // Behind ray start
    
    Vector intersectionPoint = rayStart + rayDir * t;
    
    // Project intersection point onto quad's local coordinate system
    Vector localX = ur - ul; // Right vector
    Vector localY = ll - ul; // Down vector
    
    VectorNormalize(localX);
    VectorNormalize(localY);
    
    Vector toIntersection = intersectionPoint - ul;
    
    // Calculate u and v coordinates
    float quadWidth = VectorLength(ur - ul);
    float quadHeight = VectorLength(ll - ul);
    
    u = DotProduct(toIntersection, localX) / quadWidth;
    v = DotProduct(toIntersection, localY) / quadHeight;
    
    return true; // Return true and let the caller check bounds
}
