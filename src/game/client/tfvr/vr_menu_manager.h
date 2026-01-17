#ifndef VR_MENU_MANAGER_H
#define VR_MENU_MANAGER_H

#include "cbase.h"
#include "mathlib/vector.h"
#include "vgui/IInput.h"
#include "vgui/IInputInternal.h"
#include "client_virtualreality.h"
#include "hmdWrapper.h"

// Forward declarations
class C_TFPlayer;
class COpenXRManager;
class CVRStatusHUDManager;
class CVRWeaponHUDManager;
class CVRSpringHUDManager;
class CVRDamageIndicatorManager;
class CVRWeaponSelectManager;
class CVRPopupHUDManager;

// Global input pointer declarations
extern vgui::IInputInternal *g_InputInternal;

class CVRMenuManager
{
public:
    CVRMenuManager();
    ~CVRMenuManager();

    void Initialize();
    void Shutdown();
    void Update();

    // Main menu input handling function
    void HandleMenuInput();

    // Public method to check if menu is visible (for external access)
    bool IsMenuVisible();
    
    // Public method to get the world position where the controller ray intersects the menu plane
    Vector GetMenuPlaneIntersection(const Vector& controllerPos, const Vector& controllerForward);
    
    // Public method to get which hand is currently active for menu interaction
    int GetActiveMenuHand() const { return m_nMenuHand; }

    // Compositor integration methods
    void SubmitMenuFrameToCompositor();
    void SubmitLoadingFrameToCompositor();
    
    // VGUI rendering method (public so it can be called from render loop)
    void RenderVGUIToTexture();
    
    // State determination and mode handling
    SourceEngineState DetermineSourceState();
    
    // Public method to get playspace origin for external access (needed for HUD positioning)
    Vector GetPlayspaceOriginWorldPos();
    
    // Public method to update cursor position (called from OverridePlayerMotion for fresh positioning)
    void UpdateCursorPosition();

private:
    // Helper functions
    void HandleMenuButtonInput();
    void ComputeCursorPositionCompositor(int& px, int& py); // Playspace-relative cursor for compositor mode
    void HandleCompositorMode(SourceEngineState state);
    void HandleTraditionalVRMode(SourceEngineState state);
    
    // VR rendering methods
    void RenderMenuOnlyMode();
    void RenderLoadingScreenMode();
    void CopyVGUIDirectlyToVR();
    void SetupMinimal3DWorld();
    void RenderMenuQuadIn3D();
    void RenderTestPattern();
    void RenderTestPatternDirectly();
    
    // Cursor positioning
    void ComputeCursorPosition(const Vector& pointerPosition, const QAngle& pointerRotation, int& px, int& py);
    bool ComputeIntersectionBarycentricCoordinates(const Vector& rayStart, const Vector& rayEnd, 
                                                   const Vector& ul, const Vector& ur, const Vector& ll, const Vector& lr,
                                                   float& u, float& v);
    
    // Playspace anchoring methods
    void CalculatePlayspaceAnchor();
    void UpdatePlayspaceAnchoredPosition();
    Vector CalculateCurrentPlayspaceOriginWorldPos();

    // VR input state
    bool m_bMenuButtonPressed;
    bool m_bMenuButtonReleased;
    int m_nMenuHand; // 0 = left, 1 = right
    
    // Cursor state
    int m_nOldCursorX;
    int m_nOldCursorY;
    
    // VR manager reference
    COpenXRManager* m_pVRManager;
    
    // Player reference
    C_TFPlayer* m_pLocalPlayer;
    
    // Saved view origin for menu input
    Vector m_savedPlayerViewOrigin;
    
    // Fixed menu position and orientation (captured when menu opens)
    Vector m_fixedMenuPosition;
    QAngle m_fixedMenuRotation;
    bool m_bMenuPositionFixed;
    
    // Playspace-anchored menu positioning
    VMatrix m_menuPlayspaceAnchor;  // Menu's absolute position/rotation in playspace coordinates
    bool m_bUsePlayspaceAnchoring;
    Vector m_menuPositionInPlayspace;  // Menu's fixed position relative to playspace origin
    
    // ConVars
    ConVar* m_pConVarPrimaryHand;
    
    // Map change detection
    char m_szLastMapName[64];
    float m_flLastClassMenuTime; // Time when changeclass was last executed
    
    // VR frame management
    bool m_bVRFrameStarted;
    
    // VR tracking update optimization
    int m_nLastVRTrackingUpdateFrame;
    
    // VR Status HUD (left hand: health, objectives)
    CVRStatusHUDManager* m_pVRStatusHUDManager;
    
    // VR Weapon HUD (right hand: ammo, meters, charges)
    CVRWeaponHUDManager* m_pVRWeaponHUDManager;
    
    // VR Spring HUD (head-relative: kill feed with spring-arm behavior)
    CVRSpringHUDManager* m_pVRSpringHUDManager;
    
    // VR Damage Indicator (head-relative: damage direction with spring-arm behavior)
    CVRDamageIndicatorManager* m_pVRDamageIndicatorManager;
    
    // VR Weapon Select (radial weapon selection menu)
    CVRWeaponSelectManager* m_pVRWeaponSelectManager;
    
    // VR Popup HUD (head-relative: win/loss panels, scoreboard)
    CVRPopupHUDManager* m_pVRPopupHUDManager;
};

// Global instance
extern CVRMenuManager* g_pVRMenuManager;

#endif // VR_MENU_MANAGER_H
