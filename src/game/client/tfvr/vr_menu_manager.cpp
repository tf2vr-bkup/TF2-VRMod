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
#include "iclientmode.h"
#include "hud.h"
#include "hudelement.h"
#include "menu.h"
#include "tf_viewport.h"
#include "tf_shareddefs.h"
#include "tf_playerclass_shared.h"
#include <algorithm>

// Global instances
CVRMenuManager* g_pVRMenuManager = nullptr;
extern vgui::IInputInternal *g_InputInternal;
extern COpenXRManager* g_pOpenXRManager;

// TF2-specific externs
extern IViewPort* gViewPortInterface;
extern IClientMode* g_pClientMode;

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
    , m_szLastMapName("")
    , m_flLastClassMenuTime(0.0f)
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
    bool menuVisible = IsMenuVisible();
    
    // Check for class menu button press (left A button)
    static bool bLastClassMenuButtonState = false;
    bool bCurrentClassMenuButtonState = m_pVRManager && m_pVRManager->IsButtonPressed("left_class_menu");
    
    // Only execute on button press (not hold) and with cooldown
    if (bCurrentClassMenuButtonState && !bLastClassMenuButtonState)
    {
        float currentTime = gpGlobals->curtime;
        // Prevent rapid-fire: require at least 0.5 seconds between executions
        if (currentTime - m_flLastClassMenuTime >= 0.5f)
        {
            // Toggle the class menu (open if closed, close if open)
            if (engine && engine->IsInGame())
            {
                // Check if class menu is already open
                bool bClassMenuOpen = false;
                ConVar* pClassMenuOpen = g_pCVar->FindVar("_cl_classmenuopen");
                if (pClassMenuOpen && pClassMenuOpen->GetBool())
                {
                    bClassMenuOpen = true;
                }
                
                if (bClassMenuOpen)
                {
                    // Close the class menu by pressing escape
                    engine->ClientCmd("escape");
                    DevMsg("VR Menu: Class menu closed via left A button (escape command)\n");
                }
                else
                {
                    // Open the class menu
                    engine->ClientCmd("changeclass");
                    DevMsg("VR Menu: Class menu opened via left A button (changeclass command)\n");
                }
                
                m_flLastClassMenuTime = currentTime;
            }
        }
        else
        {
            DevMsg("VR Menu: Class menu command blocked - too soon since last execution (%.1f seconds)\n", 
                   currentTime - m_flLastClassMenuTime);
        }
    }
    
    bLastClassMenuButtonState = bCurrentClassMenuButtonState;
    
    // Check if player has moved significantly (e.g., changed maps)
    if (m_pLocalPlayer && m_bMenuPositionFixed)
    {
        Vector currentPos = m_pLocalPlayer->EyePosition();
        float distanceMoved = (currentPos - m_fixedMenuPosition).Length();
        
        // If player moved more than 1000 units, they probably changed maps
        if (distanceMoved > 1000.0f)
        {
            DevMsg("VR Menu: Player moved %.1f units, resetting menu position\n", distanceMoved);
            m_bMenuPositionFixed = false;
            // Clear the old HUD bounds
            g_ClientVirtualReality.ClearCustomHUDBounds();
        }
    }
    
    // Check for map changes
    if (engine && engine->IsInGame())
    {
        const char* currentMapName = engine->GetLevelName();
        if (currentMapName && strcmp(currentMapName, m_szLastMapName) != 0)
        {
            DevMsg("VR Menu: Map changed from '%s' to '%s', resetting menu position\n", 
                   m_szLastMapName, currentMapName);
            m_bMenuPositionFixed = false;
            Q_strncpy(m_szLastMapName, currentMapName, sizeof(m_szLastMapName));
            // Clear the old HUD bounds
            g_ClientVirtualReality.ClearCustomHUDBounds();
        }
    }
    
    // If menu just became visible, capture the fixed position
    if (menuVisible && !m_bMenuPositionFixed)
    {
        if (m_pLocalPlayer)
        {
                         // Wait for player to have a valid rotation (not zero angles)
             QAngle currentAngles = m_pLocalPlayer->EyeAngles();
             if (currentAngles.LengthSqr() < 0.1f)
             {
                 // Player rotation not ready yet, wait for next frame
                 return;
             }
            
                         // Check if this is a natural menu opening (not button-triggered)
             // Only run HMD stabilization check for natural openings like startup/MOTD
             static bool bFirstPositioning = true;
             static Vector lastValidPosition = Vector(0, 0, 0);
             static QAngle lastValidRotation = QAngle(0, 0, 0);
             static float flRotationStableTime = 0.0f;
             Vector currentPlayerPos = m_pLocalPlayer->EyePosition();
             
             // If this is the first time or player moved significantly from last valid position
             if (bFirstPositioning || (lastValidPosition != Vector(0, 0, 0) && 
                 (currentPlayerPos - lastValidPosition).Length() > 100.0f))
             {
                 bFirstPositioning = false;
                 lastValidPosition = currentPlayerPos;
                 lastValidRotation = currentAngles;
                 flRotationStableTime = gpGlobals->curtime;
                 
                 // Don't set menu position yet - wait for rotation to stabilize
                 return;
             }
             
             // Wait for HMD rotation to stabilize (no significant changes for 0.2 seconds)
             if (lastValidRotation != QAngle(0, 0, 0))
             {
                 float rotationDelta = (currentAngles - lastValidRotation).Length();
                 float timeSinceLastChange = gpGlobals->curtime - flRotationStableTime;
                 
                 // If rotation changed significantly, reset the timer
                 if (rotationDelta > 2.0f)
                 {
                     lastValidRotation = currentAngles;
                     flRotationStableTime = gpGlobals->curtime;
                     return;
                 }
                 
                 // If rotation has been stable for 0.05 seconds, capture the menu position
                 if (timeSinceLastChange >= 0.05f)
                 {
                     // Use the player's current position and view angles for consistent placement
                     m_fixedMenuPosition = currentPlayerPos;
                     m_fixedMenuRotation = currentAngles;
                     m_fixedMenuRotation.x = 0; // Keep level
                     m_fixedMenuRotation.z = 0; // No roll
                     
                     m_bMenuPositionFixed = true;
                     
                     // For ViewPort menus, we need to make the cursor visible
                     if (vgui::surface())
                     {
                         vgui::surface()->SetCursorAlwaysVisible(true);
                     }
                     
                     // Set custom HUD bounds ONCE when menu opens
                     float menuDistance = tfvr_menu_distance.GetFloat();
                     Vector forward, right, up;
                     AngleVectors(m_fixedMenuRotation, &forward, &right, &up);
                     
                     // Place the menu directly in front of the player at the specified distance
                     Vector menuPlaneCenter = m_fixedMenuPosition + forward * menuDistance;
                     
                     // Create a quad representing the menu plane
                     // Use a fixed size that's reasonable for VR
                     float menuHeight = 80.0f; // Fixed height in world units
                     float menuWidth = menuHeight * 1.6f; // 16:10 aspect ratio
                     
                     Vector ul = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (menuHeight * 0.5f);
                     Vector ur = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (menuHeight * 0.5f);
                     Vector ll = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
                     Vector lr = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
                     
                     // Set the custom HUD bounds in the VR system
                     g_ClientVirtualReality.SetCustomHUDBounds(m_fixedMenuPosition, ul, ur, ll, lr);
                 }
                 else
                 {
                     // Still waiting for rotation to stabilize
                     return;
                 }
             }
         }
     }
     
     // If menu just became hidden, reset the fixed position
    else if (!menuVisible && m_bMenuPositionFixed)
    {
        m_bMenuPositionFixed = false;
        
                 // Hide the cursor when menu closes
         if (vgui::surface())
         {
             vgui::surface()->SetCursorAlwaysVisible(false);
         }
         
         // Clear the custom HUD bounds to restore normal VR system behavior
         g_ClientVirtualReality.ClearCustomHUDBounds();
        
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
    // Check multiple conditions to be more robust
    bool bGameUIVisible = enginevgui && enginevgui->IsGameUIVisible();
    bool bCursorVisible = vgui::surface() && vgui::surface()->IsCursorVisible();
    bool bNotInGame = engine && (!engine->IsInGame() || !engine->IsConnected());
    
             // Check for ViewPort panels (class select, team select, intro, MOTD, etc.)
         bool bViewPortVisible = false;
         if (gViewPortInterface)
         {
             // Check common TF2 ViewPort panels by finding them and checking visibility
             IViewPortPanel* pPanel = nullptr;
             
             // Check class selection panels
             pPanel = gViewPortInterface->FindPanelByName(PANEL_CLASS_RED);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
             
             pPanel = gViewPortInterface->FindPanelByName(PANEL_CLASS_BLUE);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
             
             // Check team selection panel
             pPanel = gViewPortInterface->FindPanelByName(PANEL_TEAM);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
             
             // Check intro panel
             pPanel = gViewPortInterface->FindPanelByName(PANEL_INTRO);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
             
             // Check info panel (MOTD, etc.)
             pPanel = gViewPortInterface->FindPanelByName(PANEL_INFO);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
             
             // Check map info panel
             pPanel = gViewPortInterface->FindPanelByName(PANEL_MAPINFO);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
             
             // Check arena team panel
             pPanel = gViewPortInterface->FindPanelByName(PANEL_ARENA_TEAM);
             if (pPanel && pPanel->IsVisible()) 
             {
                 bViewPortVisible = true;
             }
         }
    
         // Check for HUD menus (voice commands, etc.)
     bool bHudMenuVisible = false;
     if (g_pClientMode)
     {
         CHudMenu* pHudMenu = GET_HUDELEMENT(CHudMenu);
         if (pHudMenu && pHudMenu->IsMenuOpen())
         {
             bHudMenuVisible = true;
         }
     }
    
         // Check for class menu state via console variables
     bool bClassMenuOpen = false;
     if (engine && engine->IsInGame())
     {
         ConVar* pClassMenuOpen = g_pCVar->FindVar("_cl_classmenuopen");
         if (pClassMenuOpen && pClassMenuOpen->GetBool())
         {
             bClassMenuOpen = true;
         }
     }
    
    // Check for TF2-specific menu states via ConVars
    bool bTF2MenuState = false;
    if (engine && engine->IsInGame())
    {
                 // Check for team UI setup state
         ConVar* pTeamUI = g_pCVar->FindVar("team_ui_setup");
         if (pTeamUI && pTeamUI->GetBool())
         {
             bTF2MenuState = true;
         }
         
         // Check player state for menu indicators
         C_TFPlayer* pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
         if (pLocalPlayer)
         {
             // If dead and not spectating, likely showing class selection
             if (pLocalPlayer->IsPlayerDead() && pLocalPlayer->GetTeamNumber() != TEAM_SPECTATOR)
             {
                 bTF2MenuState = true;
             }
             
             // Check for team assignment state
             if (pLocalPlayer->GetTeamNumber() == TEAM_UNASSIGNED)
             {
                 bTF2MenuState = true;
             }
             
             // Check for undefined class state (shows class selection)
             if (pLocalPlayer->GetPlayerClass() && 
                 pLocalPlayer->GetPlayerClass()->GetClassIndex() == TF_CLASS_UNDEFINED)
             {
                 bTF2MenuState = true;
             }
         }
    }
    
         // Menu is considered visible if any of these conditions are true
     return bGameUIVisible || bViewPortVisible || bClassMenuOpen || bTF2MenuState;
    
    
}

void CVRMenuManager::HandleMenuButtonInput()
{
    // Check for menu press on both hands
    bool leftMenuPress = m_pVRManager->IsButtonPressed("left_ui_interact");
    bool rightMenuPress = m_pVRManager->IsButtonPressed("right_ui_interact");
    
    // Determine which hand is being used
    if (leftMenuPress && !m_bMenuButtonPressed)
    {
        m_nMenuHand = 0; // Left hand
        m_bMenuButtonPressed = true;
        
        // For ViewPort menus, we need to simulate a mouse click at the current cursor position
        if (vgui::surface() && vgui::surface()->IsCursorVisible())
        {
            // Get current cursor position
            int cursorX, cursorY;
            vgui::surface()->SurfaceGetCursorPos(cursorX, cursorY);
            
            // Simulate mouse press at cursor position
            if (g_InputInternal)
            {
                g_InputInternal->InternalCursorMoved(cursorX, cursorY);
                g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_PRESSED);
                g_InputInternal->InternalMousePressed(MOUSE_LEFT);
            }
        }
        else
        {
            // Fallback to VGUI input system
            if (g_InputInternal)
            {
                g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_PRESSED);
                g_InputInternal->InternalMousePressed(MOUSE_LEFT);
            }
        }
    }
    else if (rightMenuPress && !m_bMenuButtonPressed)
    {
        m_nMenuHand = 1; // Right hand
        m_bMenuButtonPressed = true;
        
        // For ViewPort menus, we need to simulate a mouse click at the current cursor position
        if (vgui::surface() && vgui::surface()->IsCursorVisible())
        {
            // Get current cursor position
            int cursorX, cursorY;
            vgui::surface()->SurfaceGetCursorPos(cursorX, cursorY);
            
            // Simulate mouse press at cursor position
            if (g_InputInternal)
            {
                g_InputInternal->InternalCursorMoved(cursorX, cursorY);
                g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_PRESSED);
                g_InputInternal->InternalMousePressed(MOUSE_LEFT);
            }
        }
        else
        {
            // Fallback to VGUI input system
            if (g_InputInternal)
            {
                g_InputInternal->SetMouseCodeState(MOUSE_LEFT, vgui::BUTTON_PRESSED);
                g_InputInternal->InternalMousePressed(MOUSE_LEFT);
            }
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
        // For ViewPort menus, we need to use surface cursor functions
        if (vgui::surface())
        {
            // Set the cursor position on the surface
            vgui::surface()->SurfaceSetCursorPos(px, py);
        }
        
        // Also update VGUI input system for compatibility
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
    // Get the active controller pose for cursor control
    Vector controllerPos;
    QAngle controllerAngles;
    bool controllerValid = false;
    
    // Try to get the pose from the active hand
    if (m_nMenuHand == 0) // Left hand
    {
        if (g_pOpenXRManager && g_pOpenXRManager->IsLeftControllerPoseValid())
        {
            VMatrix controllerMatrix;
            if (g_pOpenXRManager->GetLeftControllerPose(controllerMatrix))
            {
                controllerPos = controllerMatrix.GetTranslation();
                // Extract forward direction directly from matrix
                Vector forward = controllerMatrix.GetForward();
                VectorAngles(forward, controllerAngles);
                controllerValid = true;
            }
        }
    }
    else // Right hand
    {
        if (g_pOpenXRManager && g_pOpenXRManager->IsRightControllerPoseValid())
        {
            VMatrix controllerMatrix;
            if (g_pOpenXRManager->GetRightControllerPose(controllerMatrix))
            {
                controllerPos = controllerMatrix.GetTranslation();
                // Extract forward direction directly from matrix
                Vector forward = controllerMatrix.GetForward();
                VectorAngles(forward, controllerAngles);
                controllerValid = true;
            }
        }
    }
    
    Vector rayStart, rayEnd;
    
    if (controllerValid)
    {
        // Use controller position and orientation for ray
        rayStart = controllerPos;
        
        // Get forward direction from controller angles
        Vector rayDir;
        AngleVectors(controllerAngles, &rayDir);
        VectorMA(controllerPos, 1000.0f, rayDir, rayEnd);
        
        
    }
    else
    {
        // Fallback to head-based cursor if controller not available
        QAngle currentViewAngles = m_pLocalPlayer->EyeAngles();
        Vector rayDir;
        AngleVectors(currentViewAngles, &rayDir);
        VectorNormalize(rayDir);
        
        Vector currentEyePos = m_pLocalPlayer->EyePosition();
        rayStart = currentEyePos;
        VectorMA(currentEyePos, 1000.0f, rayDir, rayEnd);
        
        
    }
    
    // Use the SAME fixed menu plane that was set when the menu opened
    float menuDistance = tfvr_menu_distance.GetFloat();
    Vector forward, right, up;
    AngleVectors(m_fixedMenuRotation, &forward, &right, &up);
    
    // Use the exact same placement logic as in HandleMenuInput
    Vector menuPlaneCenter = m_fixedMenuPosition + forward * menuDistance;
    
    // Use the exact same size calculations as in HandleMenuInput
    float menuHeight = 80.0f; // Fixed height in world units
    float menuWidth = menuHeight * 1.6f; // 16:10 aspect ratio
    
    // Calculate menu plane corners - EXACTLY the same as in HandleMenuInput
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
        int screenWidth, screenHeight;
        vgui::surface()->GetScreenSize(screenWidth, screenHeight);
        px = (int)(u * screenWidth + 0.5f);
        py = (int)(v * screenHeight + 0.5f);
        
        // Clamp screen coordinates to valid range
        px = max(0, min(screenWidth - 1, px));
        py = max(0, min(screenHeight - 1, py));
        
        
        return;
    }
    
    
    // If no intersection, use center of screen
    int screenWidth, screenHeight;
    vgui::surface()->GetScreenSize(screenWidth, screenHeight);
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
