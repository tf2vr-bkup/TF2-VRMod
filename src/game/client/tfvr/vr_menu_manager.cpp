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
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "iloadingdisc.h" 
#include "tfvr/hmdWrapper.h"
#include "econ/econ_ui.h"
#include "tf/vgui/class_loadout_panel.h"
#include "tf/vgui/character_info_panel.h"
#include "vr_health_overlay.h"

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
ConVar tfvr_cursor_threshold("tfvr_cursor_threshold", "0.05", FCVAR_ARCHIVE, "Minimum VR controller movement required to override mouse (in world units)");
ConVar tfvr_cursor_head_threshold("tfvr_cursor_head_threshold", "1.0", FCVAR_ARCHIVE, "Minimum VR head movement required to override mouse (in world units)");
ConVar tfvr_cursor_debug("tfvr_cursor_debug", "0", FCVAR_ARCHIVE, "Show debug info for VR cursor threshold");
ConVar tfvr_menu_debug("tfvr_menu_debug", "0", FCVAR_ARCHIVE, "Show debug info for VR menu rendering");
ConVar tfvr_playspace_anchoring("tfvr_playspace_anchoring", "1", FCVAR_ARCHIVE, "Anchor menu to playspace origin instead of player");

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
    , m_bUsePlayspaceAnchoring(true)
    , m_pConVarPrimaryHand(nullptr)
    , m_szLastMapName("")
    , m_flLastClassMenuTime(0.0f)
    , m_bVRFrameStarted(false)
    , m_pVRHealthOverlay(nullptr)
{
    m_menuPlayspaceAnchor.Identity();
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
    
    // Initialize VR Health Overlay
    if (!m_pVRHealthOverlay)
    {
        m_pVRHealthOverlay = new CVRHealthOverlay();
        if (m_pVRHealthOverlay->Initialize())
        {
            g_pVRHealthOverlay = m_pVRHealthOverlay;
            DevMsg("VR Menu Manager: Health overlay initialized\n");
        }
        else
        {
            delete m_pVRHealthOverlay;
            m_pVRHealthOverlay = nullptr;
            Warning("VR Menu Manager: Failed to initialize health overlay\n");
        }
    }
    
    // VR Menu Manager initialized
}

void CVRMenuManager::Shutdown()
{
    // Shutdown VR Health Overlay
    if (m_pVRHealthOverlay)
    {
        m_pVRHealthOverlay->Shutdown();
        delete m_pVRHealthOverlay;
        m_pVRHealthOverlay = nullptr;
        g_pVRHealthOverlay = nullptr;
    }
    
    m_pVRManager = nullptr;
    m_pLocalPlayer = nullptr;
    // VR Menu Manager shutdown
}

void CVRMenuManager::Update()
{
    if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
        return;

    m_pVRManager = g_pOpenXRManager;
    m_pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
    
    // Debug output during state checks
    static float s_flLastDebugTime = 0.0f;
    static SourceEngineState s_lastState = SOURCE_STATE_GAMEPLAY;
    SourceEngineState currentState = DetermineSourceState();
    
    if (gpGlobals->curtime > s_flLastDebugTime + 1.0f || currentState != s_lastState)  // Every second or state change
    {
        s_flLastDebugTime = gpGlobals->curtime;
        
        // Only call dxvkSetSourceState when state actually changes (not every frame!)
        if (currentState != s_lastState) {
            dxvkSetSourceState(currentState);
        }
        
        s_lastState = currentState;
    }
    
    // Handle VR rendering based on compositor state
    
    if (dxvkIsCompositorActive())
    {
        // Compositor is handling VR frames - we just submit content
        HandleCompositorMode(currentState);
    }
    else
    {
        // Traditional Source VR pipeline (gameplay)
        HandleTraditionalVRMode(currentState);
    }
    
    if (m_pLocalPlayer)
    {
        // Save the current view origin for menu input
        m_savedPlayerViewOrigin = m_pLocalPlayer->EyePosition();
    }
    
    // Update VR Health Overlay
    if (m_pVRHealthOverlay)
    {
        m_pVRHealthOverlay->Update();
    }
    
    HandleMenuInput();
}

// Global loading state - set by VGui_PreRender() every frame with authoritative loading state
static bool g_bIsGloballyLoading = false;

void TF2VR_SetLoadingState(bool isLoading)
{
    static bool s_lastLoadingState = false;
    
    g_bIsGloballyLoading = isLoading;
    
    // Only update when state actually changes
    if (isLoading != s_lastLoadingState) {
        s_lastLoadingState = isLoading;
        
        // Notify DXVK when loading state changes
        if (g_pVRMenuManager) {
            SourceEngineState newState = g_pVRMenuManager->DetermineSourceState();
            dxvkSetSourceState(newState);
        }
    }
}

// TF2VR: Simple loading detection
void TF2VR_CheckEarlyLoadingState()
{
    static bool s_lastLoadingState = false;
    static int s_checkCount = 0;
    s_checkCount++;
    
    // Combine multiple detection methods for the most reliable loading state
    bool bEngineDrawingLoading = engine && engine->IsDrawingLoadingImage();

    // Primary detection: engine->IsDrawingLoadingImage() is the most authoritative
    bool bIsLoading = bEngineDrawingLoading;
    
    // Update global loading state if it changed
    if (bIsLoading != s_lastLoadingState) {
        TF2VR_SetLoadingState(bIsLoading);
        s_lastLoadingState = bIsLoading;
    }
}

SourceEngineState CVRMenuManager::DetermineSourceState()
{
    // Use the authoritative global loading state
    bool bInMenu = (!engine->IsInGame() && !engine->IsConnected() && !m_pLocalPlayer);
    
    if (g_bIsGloballyLoading)
    {
        return SOURCE_STATE_LOADING;
    }
    else if (bInMenu)
    {
        return SOURCE_STATE_MENU;
    }
    else
    {
        return SOURCE_STATE_GAMEPLAY;
    }
}

void CVRMenuManager::HandleCompositorMode(SourceEngineState state)
{
    // Compositor is handling VR frames - we just submit content when needed
    switch (state)
    {
        case SOURCE_STATE_MENU:
            SubmitMenuFrameToCompositor();
            break;
            
        case SOURCE_STATE_LOADING:
            SubmitLoadingFrameToCompositor();
            break;
            
        default:
            // Compositor shouldn't be active in other states
            break;
    }
}

void CVRMenuManager::HandleTraditionalVRMode(SourceEngineState state)
{
    // Traditional Source VR pipeline - handle BeginFrame/EndFrame ourselves
    switch (state)
    {
        case SOURCE_STATE_MENU:
            RenderMenuOnlyMode();
            break;
            
        case SOURCE_STATE_LOADING:
            RenderLoadingScreenMode();
            break;
            
        case SOURCE_STATE_GAMEPLAY:
            // Normal gameplay - VR handled by main pipeline
            break;
            
        default:
            break;
    }
}

void CVRMenuManager::SubmitMenuFrameToCompositor()
{
    // Render VGUI to our local texture
    RenderVGUIToTexture();
    
    // Submit to DXVK compositor (non-blocking)
    ITexture *pVGUITexture = materials->FindTexture("_rt_vgui", NULL, false);
    if (pVGUITexture)
    {
        // Get texture handle and submit to compositor
        // Note: This would need proper texture handle extraction
        dxvkSubmitMenuFrame(pVGUITexture, pVGUITexture->GetActualWidth(), pVGUITexture->GetActualHeight());
    }
}

void CVRMenuManager::SubmitLoadingFrameToCompositor()
{
    // Similar to menu but for loading screens
    RenderVGUIToTexture();  // This includes loading disc
    
    ITexture *pVGUITexture = materials->FindTexture("_rt_vgui", NULL, false);
    if (pVGUITexture)
    {
        dxvkSubmitMenuFrame(pVGUITexture, pVGUITexture->GetActualWidth(), pVGUITexture->GetActualHeight());
    }
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
             
                         // Playspace anchoring provides stability, so immediately set menu position
            // Use the player's current position and view angles for consistent placement
            m_fixedMenuPosition = currentPlayerPos;
            m_fixedMenuRotation = currentAngles;
            m_fixedMenuRotation.x = 0; // Keep level
            m_fixedMenuRotation.z = 0; // No roll
            
            m_bMenuPositionFixed = true;
            
            // Check if we should use playspace anchoring
            m_bUsePlayspaceAnchoring = tfvr_playspace_anchoring.GetBool();
            
            if (m_bUsePlayspaceAnchoring && m_pVRManager)
            {
                // Calculate and store the menu's absolute position in playspace coordinates
                CalculatePlayspaceAnchor();
            }
            
            // For ViewPort menus, we need to make the cursor visible
            if (vgui::surface())
            {
                vgui::surface()->SetCursorAlwaysVisible(true);
            }
            
            // Set custom HUD bounds ONCE when menu opens (only if not using playspace anchoring)
            if (!m_bUsePlayspaceAnchoring)
            {
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
                // g_ClientVirtualReality.SetCustomHUDBounds(m_fixedMenuPosition, ul, ur, ll, lr);
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
    // Check for menu press on both hands using trigger threshold
    bool leftMenuPress = m_pVRManager->IsUIInteractionPressed("left_ui_interact");
    bool rightMenuPress = m_pVRManager->IsUIInteractionPressed("right_ui_interact");
    
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
    
    // Update playspace anchored position each frame if enabled
    if (m_bUsePlayspaceAnchoring)
    {
        UpdatePlayspaceAnchoredPosition();
    }

    // Get current VR controller position for threshold checking
    Vector currentControllerPos;
    bool controllerValid = false;
    
    if (m_nMenuHand == 0) // Left hand
    {
        if (g_pOpenXRManager && g_pOpenXRManager->IsLeftControllerPoseValid())
        {
            VMatrix controllerMatrix;
            if (g_pOpenXRManager->GetLeftControllerPose(controllerMatrix))
            {
                currentControllerPos = controllerMatrix.GetTranslation();
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
                currentControllerPos = controllerMatrix.GetTranslation();
                controllerValid = true;
            }
        }
    }
    
    // Simple threshold check: only update cursor if VR movement is significant
    if (controllerValid)
    {
        static Vector lastControllerPos = currentControllerPos;
        float movementDistance = (currentControllerPos - lastControllerPos).Length();
        
        // If movement is too small, don't update cursor (preserve mouse input)
        if (movementDistance < tfvr_cursor_threshold.GetFloat())
        {
            if (tfvr_cursor_debug.GetBool())
            {
                static float lastDebugTime = 0;
                if (gpGlobals->curtime - lastDebugTime > 1.0f)
                {
                    DevMsg("VR Cursor: Controller movement too small (%.2f < %.2f), preserving mouse input\n", 
                           movementDistance, tfvr_cursor_threshold.GetFloat());
                    lastDebugTime = gpGlobals->curtime;
                }
            }
            return;
        }
        
        lastControllerPos = currentControllerPos;
    }
    else
    {
        // No controller available, check head movement instead
        if (m_pLocalPlayer)
        {
            static Vector lastHeadPos = m_pLocalPlayer->EyePosition();
            Vector currentHeadPos = m_pLocalPlayer->EyePosition();
            float headMovementDistance = (currentHeadPos - lastHeadPos).Length();
            
            // If head movement is too small, don't update cursor (preserve mouse input)
            if (headMovementDistance < tfvr_cursor_head_threshold.GetFloat())
            {
                if (tfvr_cursor_debug.GetBool())
                {
                    static float lastDebugTime = 0;
                    if (gpGlobals->curtime - lastDebugTime > 1.0f)
                    {
                        DevMsg("VR Cursor: Head movement too small (%.2f < %.2f), preserving mouse input\n", 
                               headMovementDistance, tfvr_cursor_head_threshold.GetFloat());
                        lastDebugTime = gpGlobals->curtime;
                    }
                }
                return;
            }
            
            lastHeadPos = currentHeadPos;
        }
    }

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
    
    // For playspace anchoring, m_fixedMenuPosition is the actual menu world position
    // For cursor calculation, we need to use that directly (no distance offset)
    Vector menuPlaneCenter;
    Vector forward, right, up;
    AngleVectors(m_fixedMenuRotation, &forward, &right, &up);
    
    if (m_bUsePlayspaceAnchoring)
    {
        // m_fixedMenuPosition is already the menu world position
        menuPlaneCenter = m_fixedMenuPosition;
    }
    else
    {
        // Legacy mode: m_fixedMenuPosition is viewer position, add distance
        float menuDistance = tfvr_menu_distance.GetFloat();
        menuPlaneCenter = m_fixedMenuPosition + forward * menuDistance;
    }
    
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

Vector CVRMenuManager::GetMenuPlaneIntersection(const Vector& controllerPos, const Vector& controllerForward)
{
    // Only return valid intersection if menu position is fixed
    if (!m_bMenuPositionFixed)
        return vec3_origin;
    
    // Use the EXACT same fixed menu plane logic as ComputeCursorPosition
    Vector menuPlaneCenter;
    Vector forward, right, up;
    AngleVectors(m_fixedMenuRotation, &forward, &right, &up);
    
    if (m_bUsePlayspaceAnchoring)
    {
        // m_fixedMenuPosition is already the menu world position
        menuPlaneCenter = m_fixedMenuPosition;
    }
    else
    {
        // Legacy mode: m_fixedMenuPosition is viewer position, add distance
        float menuDistance = tfvr_menu_distance.GetFloat();
        menuPlaneCenter = m_fixedMenuPosition + forward * menuDistance;
    }
    
    // Use the exact same size calculations as in HandleMenuInput
    float menuHeight = 80.0f; // Fixed height in world units
    float menuWidth = menuHeight * 1.6f; // 16:10 aspect ratio
    
    // Calculate menu plane corners - EXACTLY the same as in ComputeCursorPosition
    Vector ul = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (menuHeight * 0.5f);
    Vector ur = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (menuHeight * 0.5f);
    Vector ll = menuPlaneCenter + right * (-menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
    Vector lr = menuPlaneCenter + right * (menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
    
    // Set up ray from controller
    Vector rayStart = controllerPos;
    Vector rayEnd = controllerPos + controllerForward * 1000.0f;
    
    // Use the same intersection calculation
    float u, v;
    if (ComputeIntersectionBarycentricCoordinates(rayStart, rayEnd, ul, ur, ll, lr, u, v))
    {
        // Compute the actual world intersection point
        Vector rayDir = rayEnd - rayStart;
        VectorNormalize(rayDir);
        
        // Compute the plane normal from the quad
        Vector edge1 = ur - ul;
        Vector edge2 = ll - ul;
        Vector planeNormal = CrossProduct(edge1, edge2);
        VectorNormalize(planeNormal);
        
        // Find intersection point with plane
        Vector planePoint = ul;
        float denominator = DotProduct(rayDir, planeNormal);
        if (fabs(denominator) >= 0.0001f)
        {
            float t = DotProduct(planePoint - rayStart, planeNormal) / denominator;
            if (t >= 0)
            {
                return rayStart + rayDir * t;
            }
        }
    }
    
    return vec3_origin; // No valid intersection
}

void CVRMenuManager::CalculatePlayspaceAnchor()
{
    if (!m_pVRManager || !m_pLocalPlayer)
        return;
    
    // Simple approach: Calculate current playspace origin and store menu position relative to it
    
    // STEP 1: Calculate current playspace origin in world coordinates
    Vector playspaceOriginWorldPos = CalculateCurrentPlayspaceOriginWorldPos();
    
    // STEP 2: Calculate menu position in playspace coordinates
    float menuDistance = tfvr_menu_distance.GetFloat();
    QAngle leveledAngles = m_fixedMenuRotation; // Already leveled
    
    Vector forward, right, up;
    AngleVectors(leveledAngles, &forward, &right, &up);
    Vector menuWorldPos = m_fixedMenuPosition + forward * menuDistance;
    
    // Store menu position relative to playspace origin
    m_menuPositionInPlayspace = menuWorldPos - playspaceOriginWorldPos;
    
    // Store full matrix with rotation
    m_menuPlayspaceAnchor.Identity();
    matrix3x4_t menuMatrix3x4;
    AngleMatrix(leveledAngles, m_menuPositionInPlayspace, menuMatrix3x4);
    m_menuPlayspaceAnchor.CopyFrom3x4(menuMatrix3x4);
    
    DevMsg("VR Menu ANCHOR CALCULATION:\n");
    DevMsg("  Current playspace origin: (%.1f, %.1f, %.1f)\n", 
           playspaceOriginWorldPos.x, playspaceOriginWorldPos.y, playspaceOriginWorldPos.z);
    DevMsg("  Menu world pos: (%.1f, %.1f, %.1f)\n", 
           menuWorldPos.x, menuWorldPos.y, menuWorldPos.z);
    DevMsg("  Menu playspace offset: (%.3f, %.3f, %.3f)\n", 
           m_menuPositionInPlayspace.x, m_menuPositionInPlayspace.y, m_menuPositionInPlayspace.z);
}

Vector CVRMenuManager::CalculateCurrentPlayspaceOriginWorldPos()
{
    if (!m_pVRManager || !m_pLocalPlayer)
        return Vector(0, 0, 0);
    
    // GetMideyePose() returns head position relative to playspace origin (in Source coordinates)
    VMatrix headRelativeToPlayspace = m_pVRManager->GetMideyePose();
    
    // Calculate playspace origin relative to head
    VMatrix headToPlayspaceTransform = headRelativeToPlayspace.InverseTR();
    
    // Get current head world position
    Vector currentHeadWorldPos = m_pLocalPlayer->EyePosition();
    QAngle currentHeadWorldAngles = m_pLocalPlayer->EyeAngles();
    
    VMatrix currentHeadWorldMatrix;
    currentHeadWorldMatrix.Identity();
    matrix3x4_t headMatrix3x4;
    AngleMatrix(currentHeadWorldAngles, currentHeadWorldPos, headMatrix3x4);
    currentHeadWorldMatrix.CopyFrom3x4(headMatrix3x4);
    
    // Transform playspace origin to world coordinates
    VMatrix playspaceWorldMatrix = currentHeadWorldMatrix * headToPlayspaceTransform;
    return playspaceWorldMatrix.GetTranslation();
}

void CVRMenuManager::UpdatePlayspaceAnchoredPosition()
{
    if (!m_bUsePlayspaceAnchoring || !m_pVRManager || !m_pLocalPlayer)
        return;
    
    // STEP 1: Calculate current playspace origin in world coordinates
    Vector currentPlayspaceOriginWorldPos = CalculateCurrentPlayspaceOriginWorldPos();
    
    static float lastDebugTime = 0.0f;
    if (gpGlobals && gpGlobals->realtime - lastDebugTime > 0.5f) // Debug every 0.5 seconds
    {
        Vector currentHeadWorldPos = m_pLocalPlayer->EyePosition();
        VMatrix headRelativeToPlayspace = m_pVRManager->GetMideyePose();
        Vector headPlayspacePos = headRelativeToPlayspace.GetTranslation();
    }
    
    // STEP 3: Transform the stored playspace menu position to current world coordinates  
    Vector newMenuWorldPos = currentPlayspaceOriginWorldPos + m_menuPositionInPlayspace;
    
    // DEBUG: Check for frame lag
    static Vector lastMenuWorldPos = newMenuWorldPos;
    float menuMovement = (newMenuWorldPos - lastMenuWorldPos).Length();
    if (menuMovement > 0.1f)
    {
        DevMsg("VR Menu: Menu moved %.2f units (potential frame lag)\n", menuMovement);
    }
    lastMenuWorldPos = newMenuWorldPos;
    
    // Get menu rotation from stored anchor
    QAngle newMenuWorldAngles;
    MatrixToAngles(m_menuPlayspaceAnchor, newMenuWorldAngles);
    
    // Update the HUD bounds using the new position
    float menuHeight = 80.0f;
    float menuWidth = menuHeight * 1.6f;
    
    Vector forward, right, up;
    AngleVectors(newMenuWorldAngles, &forward, &right, &up);
    
    Vector ul = newMenuWorldPos + right * (-menuWidth * 0.5f) + up * (menuHeight * 0.5f);
    Vector ur = newMenuWorldPos + right * (menuWidth * 0.5f) + up * (menuHeight * 0.5f);
    Vector ll = newMenuWorldPos + right * (-menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
    Vector lr = newMenuWorldPos + right * (menuWidth * 0.5f) + up * (-menuHeight * 0.5f);
    
    // Get current head position for viewer reference
    Vector currentHeadWorldPos = m_pLocalPlayer->EyePosition();
    
    // Update HUD bounds and cached position for cursor calculations
    g_ClientVirtualReality.SetCustomHUDBounds(currentHeadWorldPos, ul, ur, ll, lr);
    
    // Update cached values for cursor calculations
    // For playspace anchoring, we need to store the actual menu world position for cursor calculations
    m_fixedMenuPosition = newMenuWorldPos; // Use actual menu position, not head position
    m_fixedMenuRotation = newMenuWorldAngles;
}

//-----------------------------------------------------------------------------
// Purpose: Render 3D menu in world space when in menu-only mode
//-----------------------------------------------------------------------------
void CVRMenuManager::RenderMenuOnlyMode()
{
    if (!m_pVRManager)
    {
        DevMsg("VR Menu: m_pVRManager is null!\n");
        return;
    }

    // SIMPLIFIED: Just render the menu - let normal VR frame cycle handle BeginFrame/EndFrame
    CopyVGUIDirectlyToVR();
    
    // Debug output (only once per second to avoid spam)
    static float lastDebugTime = 0;
    float currentTime = gpGlobals->realtime;
    if (currentTime - lastDebugTime > 1.0f)
    {
        DevMsg("VR Menu Manager: Rendering menu in VR (simplified approach)\n");
        lastDebugTime = currentTime;
    }
}

//-----------------------------------------------------------------------------
// Purpose: Render VR during loading screens
//-----------------------------------------------------------------------------
void CVRMenuManager::RenderLoadingScreenMode()
{
    if (!m_pVRManager)
    {
        DevMsg("VR Menu: m_pVRManager is null during loading!\n");
        return;
    }

    // Check if normal render cycle is happening by checking frame number
    static int s_lastFrameCount = -1;
    static bool s_normalCycleRunning = false;
    
    if (gpGlobals->framecount != s_lastFrameCount)
    {
        s_lastFrameCount = gpGlobals->framecount;
        // Reset normal cycle detection each frame
        s_normalCycleRunning = false;
    }
    
    // During loading screens, check if we need to manage VR frames
    // Only start our own frame if normal cycle hasn't started one
    bool bShouldManageFrame = !s_normalCycleRunning;
    
    if (bShouldManageFrame && !m_bVRFrameStarted)
    {
        if (m_pVRManager->BeginFrame())
        {
            m_bVRFrameStarted = true;
        }
        else
        {
            // BeginFrame failed, might mean normal cycle already started frame
            s_normalCycleRunning = true;
            bShouldManageFrame = false;
        }
    }

    // For loading screens, render the VGUI (which includes the loading disc)
    CopyVGUIDirectlyToVR();
    
    // Only end frame if we started it
    if (bShouldManageFrame && m_bVRFrameStarted)
    {
        m_pVRManager->EndFrame();
        m_bVRFrameStarted = false;
    }
    
    // Debug output (only once per second to avoid spam)
    static float lastDebugTime = 0;
    float currentTime = gpGlobals->realtime;
    if (currentTime - lastDebugTime > 1.0f)
    {
        DevMsg("VR Menu Manager: Rendering loading screen in VR (managing frames: %d)\n", bShouldManageFrame);
        lastDebugTime = currentTime;
    }
}

//-----------------------------------------------------------------------------
// Purpose: Render VGUI menu panels to texture (based on DrawMainMenu)
//-----------------------------------------------------------------------------
void CVRMenuManager::RenderVGUIToTexture()
{
    // Find the VGUI render texture
    ITexture *pTexture = materials->FindTexture("_rt_vgui", NULL, false);
    if (!pTexture)
        return;

    CMatRenderContextPtr pRenderContext(materials);
    int viewActualWidth = pTexture->GetActualWidth();
    int viewActualHeight = pTexture->GetActualHeight();

    int viewWidth, viewHeight;
    vgui::surface()->GetScreenSize(viewWidth, viewHeight);

    // Clear depth before we push the render target
    pRenderContext->ClearBuffers(false, true, true);

    // Set up render target for VGUI
    pRenderContext->PushRenderTargetAndViewport(pTexture, NULL, 0, 0, viewActualWidth, viewActualHeight);

    // When in compositor mode, always use opaque rendering
    bool bUseTranslucent = false;
    
    // Check if we're in main pause menu vs overlay menus
    bool bIsMainMenu = enginevgui && enginevgui->IsGameUIVisible();
    bool bIsEconUIVisible = false;
    
    // Check if any EconUI panels are visible (loadout, backpack, crafting, etc.)
    if ( EconUI() )
    {
        bIsEconUIVisible = EconUI()->IsUIPanelVisible( ECONUI_BACKPACK ) ||
                           EconUI()->IsUIPanelVisible( ECONUI_LOADOUT ) ||
                           EconUI()->IsUIPanelVisible( ECONUI_CRAFTING ) ||
                           EconUI()->IsUIPanelVisible( ECONUI_ARMORY ) ||
                           EconUI()->IsUIPanelVisible( ECONUI_TRADING );
    }
    
    // In compositor mode, always use opaque for better visibility
    // (Translucent rendering can be handled by the compositor itself if needed)
    bUseTranslucent = false;
    
    // Additional detection for other menu types
    bool bIsLoadoutScreen = false;
    bool bIsCursorVisible = vgui::surface() && vgui::surface()->IsCursorVisible();
    bool bIsConnectedToServer = engine && engine->IsConnected();
    
    // Check for class menu state via console variables (loadout selection)
    if (engine && engine->IsInGame())
    {
        ConVar* pClassMenuOpen = g_pCVar->FindVar("_cl_classmenuopen");
        if (pClassMenuOpen && pClassMenuOpen->GetBool())
        {
            bIsLoadoutScreen = true;
        }
    }
    
    // Check for class loadout panel specifically
    if (g_pClassLoadoutPanel && g_pClassLoadoutPanel->IsVisible())
    {
        bIsLoadoutScreen = true;
    }
    
    // Check for character info panel (class selection screen) 
    CCharacterInfoPanel* pCharInfoPanel = GetCharInfoPanel(false);
    if (pCharInfoPanel && pCharInfoPanel->IsVisible())
    {
        bIsLoadoutScreen = true;
    }
    
    // Only log material changes, not every frame
    static bool s_bLastUseTranslucent = true;
    if ( s_bLastUseTranslucent != bUseTranslucent )
    {
        s_bLastUseTranslucent = bUseTranslucent;
        DevMsg("VR VGUI: Switching to %s rendering\n", bUseTranslucent ? "TRANSLUCENT" : "OPAQUE");
    }

    // Configure alpha writing and clear based on menu type
    if ( bUseTranslucent )
    {
        // Translucent: Allow alpha writing and clear with transparent background
        pRenderContext->OverrideAlphaWriteEnable(true, true);
        pRenderContext->ClearColor4ub(0, 0, 0, 0);  // Transparent background
    }
    else
    {
        // Opaque: Disable alpha writing and clear with solid background
        pRenderContext->OverrideAlphaWriteEnable(true, false);
        pRenderContext->ClearColor4ub(0, 0, 0, 255);  // Solid black background
    }
    pRenderContext->ClearBuffers(true, false);

    // Set up VGUI panels for rendering
    vgui::VPANEL root = enginevgui->GetPanel(PANEL_CLIENTDLL);
    if (root != 0)
    {
        vgui::ipanel()->SetSize(root, viewWidth, viewHeight);
    }
    root = enginevgui->GetPanel(PANEL_CLIENTDLL_TOOLS);
    if (root != 0)
    {
        vgui::ipanel()->SetSize(root, viewWidth, viewHeight);
    }

    // Paint the main menu and cursor (this is the key part!)
    render->VGui_Paint((PaintMode_t)(PAINT_UIPANELS | PAINT_CURSOR));
    
    // TF2VR: Also paint in-game panels during loading to capture loading screen
    bool bIsLoading = engine && engine->IsDrawingLoadingImage();
    bool bIsConnected = engine && engine->IsConnected();
    bool bIsInGame = engine && engine->IsInGame();
    bool bIsLoadingAlternative = bIsConnected && !bIsInGame;

    // Restore render context
    pRenderContext->OverrideAlphaWriteEnable(false, true);
    pRenderContext->PopRenderTargetAndViewport();
    pRenderContext->Flush();
}

//-----------------------------------------------------------------------------
// Purpose: Copy VGUI texture directly to VR eye buffer (bypass 3D)
//-----------------------------------------------------------------------------
void CVRMenuManager::CopyVGUIDirectlyToVR()
{
    // Render every frame for responsive loading screen
    // Removed frame skipping to ensure loading screen appears immediately
        
    // Step 1: Render the actual TF2 menu to the VGUI texture
    RenderVGUIToTexture();
    
    // Step 2: Get both textures
    ITexture *pVGUITexture = materials->FindTexture("_rt_vgui", NULL, false);
    ITexture *pVRTexture = m_pVRManager->GetRenderTarget();
    
    if (!pVGUITexture || !pVRTexture)
        return;
    
    CMatRenderContextPtr pRenderContext(materials);
    
    // Clear VR buffer first to prevent artifacts
    pRenderContext->PushRenderTargetAndViewport(pVRTexture, NULL, 
        0, 0, pVRTexture->GetActualWidth(), pVRTexture->GetActualHeight());
    
    // Clear with solid color first
    pRenderContext->ClearColor4ub(64, 0, 128, 255);  // Dark purple
    pRenderContext->ClearBuffers(true, true);
    
    // Use a simple copy material to copy VGUI texture to VR buffer
    IMaterial *pCopyMaterial = materials->FindMaterial("vgui/white", TEXTURE_GROUP_VGUI);
    if (pCopyMaterial && !pCopyMaterial->IsErrorMaterial())
    {
        // Bind the VGUI texture to the copy material
        IMaterialVar* pBaseTextureVar = pCopyMaterial->FindVar("$basetexture", NULL);
        if (pBaseTextureVar)
        {
            pBaseTextureVar->SetTextureValue(pVGUITexture);
        }
        
        // Draw a smaller rectangle in the center to test
        int vrWidth = pVRTexture->GetActualWidth();
        int vrHeight = pVRTexture->GetActualHeight();
        int quadWidth = vrWidth / 2;   // Half size
        int quadHeight = vrHeight / 2; // Half size
        int offsetX = vrWidth / 4;     // Center it
        int offsetY = vrHeight / 4;    // Center it
        
        pRenderContext->DrawScreenSpaceRectangle(
            pCopyMaterial,
            offsetX, offsetY,           // x, y (centered)
            quadWidth, quadHeight,      // width, height (half size)
            0, 0,                       // src x, y  
            pVGUITexture->GetActualWidth(), pVGUITexture->GetActualHeight(), // src width, height
            pVGUITexture->GetActualWidth(), pVGUITexture->GetActualHeight()  // texture size
        );
    }
    
    // Restore render target
    pRenderContext->PopRenderTargetAndViewport();
    pRenderContext->Flush();
}

//-----------------------------------------------------------------------------
// Purpose: Render a test pattern to the VGUI texture for debugging
//-----------------------------------------------------------------------------
void CVRMenuManager::RenderTestPattern()
{
    // Find the VGUI render texture
    ITexture *pTexture = materials->FindTexture("_rt_vgui", NULL, false);
    if (!pTexture)
        return;

    CMatRenderContextPtr pRenderContext(materials);
    int viewActualWidth = pTexture->GetActualWidth();
    int viewActualHeight = pTexture->GetActualHeight();

    // Set up render target for test pattern
    pRenderContext->PushRenderTargetAndViewport(pTexture, NULL, 0, 0, viewActualWidth, viewActualHeight);
    pRenderContext->OverrideAlphaWriteEnable(true, true);

    // Clear with a bright color first
    pRenderContext->ClearColor4ub(255, 0, 255, 255);  // Magenta background
    pRenderContext->ClearBuffers(true, false);
    
    // Draw some simple test rectangles using screen space rectangles
    IMaterial *pWhiteMaterial = materials->FindMaterial("vgui/white", TEXTURE_GROUP_VGUI);
    if (pWhiteMaterial && !pWhiteMaterial->IsErrorMaterial())
    {
        // Draw a green rectangle in the center
        pRenderContext->Bind(pWhiteMaterial);
        pRenderContext->DrawScreenSpaceRectangle(
            pWhiteMaterial,
            viewActualWidth/4, viewActualHeight/4,     // x, y
            viewActualWidth/2, viewActualHeight/2,     // width, height
            0, 0, 1, 1,                                // texture coords
            viewActualWidth, viewActualHeight          // texture size
        );
    }

    // Restore render context
    pRenderContext->OverrideAlphaWriteEnable(false, true);
    pRenderContext->PopRenderTargetAndViewport();
    pRenderContext->Flush();
}

//-----------------------------------------------------------------------------
// Purpose: Render test pattern directly to VR render target (debug)
//-----------------------------------------------------------------------------
void CVRMenuManager::RenderTestPatternDirectly()
{
    // Get the VR render target
    ITexture* pVRTexture = m_pVRManager->GetRenderTarget();
    if (!pVRTexture)
    {
        DevMsg("VR Menu: GetRenderTarget() returned null!\n");
        return;
    }

    DevMsg("VR Menu: Rendering test pattern to VR target %dx%d\n", 
           pVRTexture->GetActualWidth(), pVRTexture->GetActualHeight());

    CMatRenderContextPtr pRenderContext(materials);
    
    // Render directly to the VR target
    pRenderContext->PushRenderTargetAndViewport(pVRTexture, NULL, 
        0, 0, pVRTexture->GetActualWidth(), pVRTexture->GetActualHeight());
    
    // Clear with bright red to test
    pRenderContext->ClearColor4ub(255, 0, 0, 255);  // Bright red
    pRenderContext->ClearBuffers(true, true);
    
    // Draw some test rectangles directly in screen space
    IMaterial *pWhiteMaterial = materials->FindMaterial("vgui/white", TEXTURE_GROUP_VGUI);
    if (pWhiteMaterial && !pWhiteMaterial->IsErrorMaterial())
    {
        int width = pVRTexture->GetActualWidth();
        int height = pVRTexture->GetActualHeight();
        
        // Draw a blue rectangle in the center
        pRenderContext->DrawScreenSpaceRectangle(
            pWhiteMaterial,
            width/4, height/4,      // x, y
            width/2, height/2,      // width, height  
            0, 0, 1, 1,             // texture coords
            width, height           // texture size
        );
    }
    
    // Pop render target
    pRenderContext->PopRenderTargetAndViewport();
    pRenderContext->Flush();
}

//-----------------------------------------------------------------------------
// Purpose: Set up minimal 3D world context for rendering
//-----------------------------------------------------------------------------
void CVRMenuManager::SetupMinimal3DWorld()
{
    // Get the VR render target
    ITexture* pVRTexture = m_pVRManager->GetRenderTarget();
    if (!pVRTexture)
        return;

    // Create a basic view setup for 3D rendering
    CViewSetup viewSetup;
    memset(&viewSetup, 0, sizeof(viewSetup));
    
    // Set up basic 3D view parameters
    viewSetup.x = 0;
    viewSetup.y = 0;
    viewSetup.width = pVRTexture->GetActualWidth();
    viewSetup.height = pVRTexture->GetActualHeight();
    viewSetup.fov = 90.0f;
    viewSetup.origin = Vector(0, 0, 64);  // Player head position
    viewSetup.angles = QAngle(0, 0, 0);   // Looking forward
    viewSetup.zNear = 1.0f;
    viewSetup.zFar = 30000.0f;
    viewSetup.m_bOrtho = false;

    // Set up render target
    CMatRenderContextPtr pRenderContext(materials);
    pRenderContext->PushRenderTargetAndViewport(pVRTexture, NULL, 
        0, 0, viewSetup.width, viewSetup.height);

    // Clear with a dark background (not pure black to see the menu better)
    pRenderContext->ClearColor4ub(32, 32, 32, 255);
    pRenderContext->ClearBuffers(true, true);

    // Set up 3D view for rendering
    render->Push3DView(viewSetup, 0, pVRTexture, NULL);
}

//-----------------------------------------------------------------------------
// Purpose: Render the menu texture as a 3D quad in world space
//-----------------------------------------------------------------------------
void CVRMenuManager::RenderMenuQuadIn3D()
{
    // Render menu quad in 3D space
    
    // Find the VGUI texture that we rendered to
    ITexture *pVGUITexture = materials->FindTexture("_rt_vgui", NULL, false);
    if (!pVGUITexture)
    {
        DevMsg("VR Menu: _rt_vgui texture not found!\n");
        return;
    }
    
    // DevMsg("VR Menu: Rendering 3D quad with VGUI texture %dx%d\n", 
    //        pVGUITexture->GetActualWidth(), pVGUITexture->GetActualHeight());

    // Use static material caching to avoid repeated lookups and precaching
    static IMaterial *pCachedMaterial = nullptr;
    static bool bMaterialInitialized = false;
    
    // Determine material selection based on menu type (similar to RenderHUDQuad logic)
    bool bUseTranslucent = false;
    
    // Check if we're in main pause menu vs overlay menus
    bool bIsMainMenu = enginevgui && enginevgui->IsGameUIVisible();
    bool bIsEconUIVisible = false;
    
    	// Check if any EconUI panels are visible (loadout, backpack, crafting, etc.)
	if ( EconUI() )
	{
		bIsEconUIVisible = EconUI()->IsUIPanelVisible( ECONUI_BACKPACK ) ||
						   EconUI()->IsUIPanelVisible( ECONUI_LOADOUT ) ||
						   EconUI()->IsUIPanelVisible( ECONUI_CRAFTING ) ||
						   EconUI()->IsUIPanelVisible( ECONUI_ARMORY ) ||
						   EconUI()->IsUIPanelVisible( ECONUI_TRADING );
	}
	
	// Additional checks for loadout/armory screens that EconUI might miss
	bool bIsLoadoutOrArmoryScreen = false;
	if (engine && engine->IsConnected())
	{
		// Check for class loadout panel specifically
		if (g_pClassLoadoutPanel && g_pClassLoadoutPanel->IsVisible())
		{
			bIsLoadoutOrArmoryScreen = true;
		}
		
		// Check for character info panel (class selection screen) 
		CCharacterInfoPanel* pCharInfoPanel = GetCharInfoPanel(false);
		if (pCharInfoPanel && pCharInfoPanel->IsVisible())
		{
			bIsLoadoutOrArmoryScreen = true;
		}
	}
	
	// Use translucent for main pause menu, opaque for overlay menus
	if ( bIsMainMenu && !bIsEconUIVisible && !bIsLoadoutOrArmoryScreen )
    {
        // Main pause menu - use translucent
        bUseTranslucent = true;
    }
    else
    {
        // Overlay menus (loadout, items, etc.) - use opaque
        bUseTranslucent = false;
    }
    
    // Debug output
    static bool s_bLastUseTranslucent = true;
    static int s_debugFrameCount = 0;
    s_debugFrameCount++;
    
    if ( s_debugFrameCount % 60 == 0 ) // Every second
    {
        DevMsg("VR Menu Manager: MainMenu=%d, EconUI=%d, UseTranslucent=%d\n", 
            bIsMainMenu ? 1 : 0, bIsEconUIVisible ? 1 : 0, bUseTranslucent ? 1 : 0);
    }
    
    if ( s_bLastUseTranslucent != bUseTranslucent )
    {
        s_bLastUseTranslucent = bUseTranslucent;
        DevMsg("VR Menu Manager: Switching to %s material\n", bUseTranslucent ? "TRANSLUCENT" : "OPAQUE");
    }

    // Select the appropriate material based on menu type
    if ( bUseTranslucent )
    {
        pCachedMaterial = materials->FindMaterial("vgui/inworldui", TEXTURE_GROUP_VGUI);
    }
    else
    {
        pCachedMaterial = materials->FindMaterial("vgui/inworldui_opaque", TEXTURE_GROUP_VGUI);
    }
    
    // If material loading fails, fall back to basic material
    if (!pCachedMaterial || pCachedMaterial->IsErrorMaterial())
    {
        pCachedMaterial = materials->FindMaterial("vgui/white", TEXTURE_GROUP_VGUI);
    }
    
    // Ensure the material is properly precached and referenced
    if (pCachedMaterial && !pCachedMaterial->IsErrorMaterial())
    {
        if (!pCachedMaterial->IsPrecached()) 
        {
            PrecacheMaterial(pCachedMaterial->GetName());
            pCachedMaterial->IncrementReferenceCount();
        }
    }
    
    if (!pCachedMaterial || pCachedMaterial->IsErrorMaterial())
        return;

    // Force render state for opaque materials to ensure no transparency
    if ( !bUseTranslucent )
    {
        // For opaque materials, force full opacity and disable blending
        float color[3] = { 1.0f, 1.0f, 1.0f };
        render->SetColorModulation( color );
        render->SetBlend( 1.0f );
    }

    // Position the menu quad MUCH closer to the player
    Vector vCenter = Vector(0, 50, 64);  // 50 units in front, at head height  
    float menuWidth = 80.0f;   // Smaller size
    float menuHeight = 60.0f;  // Smaller size
    
    // Create quad corners
    Vector vUL = vCenter + Vector(-menuWidth/2, 0, menuHeight/2);   // Upper Left
    Vector vUR = vCenter + Vector(menuWidth/2, 0, menuHeight/2);    // Upper Right
    Vector vLL = vCenter + Vector(-menuWidth/2, 0, -menuHeight/2);  // Lower Left
    Vector vLR = vCenter + Vector(menuWidth/2, 0, -menuHeight/2);   // Lower Right

    // Bind the VGUI texture to the material 
    CMatRenderContextPtr pRenderContext(materials);
    pRenderContext->Bind(pCachedMaterial, NULL);
    
    // Set the VGUI texture as the base texture for the material
    IMaterialVar* pBaseTextureVar = pCachedMaterial->FindVar("$basetexture", NULL);
    if (pBaseTextureVar)
    {
        pBaseTextureVar->SetTextureValue(pVGUITexture);
    }
    
    // Render the quad using dynamic mesh (same pattern as RenderHUDQuad)
    IMesh *pMesh = pRenderContext->GetDynamicMesh(true, NULL, NULL, pCachedMaterial);

    CMeshBuilder meshBuilder;
    meshBuilder.Begin(pMesh, MATERIAL_TRIANGLE_STRIP, 2);

    // Lower Right
    meshBuilder.Position3fv(vLR.Base());
    meshBuilder.TexCoord2f(0, 1, 1);
    meshBuilder.AdvanceVertexF<VTX_HAVEPOS, 1>();

    // Lower Left
    meshBuilder.Position3fv(vLL.Base());
    meshBuilder.TexCoord2f(0, 0, 1);
    meshBuilder.AdvanceVertexF<VTX_HAVEPOS, 1>();

    // Upper Right
    meshBuilder.Position3fv(vUR.Base());
    meshBuilder.TexCoord2f(0, 1, 0);
    meshBuilder.AdvanceVertexF<VTX_HAVEPOS, 1>();

    // Upper Left
    meshBuilder.Position3fv(vUL.Base());
    meshBuilder.TexCoord2f(0, 0, 0);
    meshBuilder.AdvanceVertexF<VTX_HAVEPOS, 1>();

    meshBuilder.End();
    pMesh->Draw();
    
    // Render VR Health Overlay if enabled
    if (m_pVRHealthOverlay)
    {
        m_pVRHealthOverlay->RenderHealthQuad();
    }

    // Clean up 3D view
    render->PopView(NULL);
    
    // Pop render target
    CMatRenderContextPtr pRenderContext2(materials);
    pRenderContext2->PopRenderTargetAndViewport();
    pRenderContext2->Flush();
}