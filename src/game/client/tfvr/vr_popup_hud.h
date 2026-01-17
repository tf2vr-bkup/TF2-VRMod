#ifndef VR_POPUP_HUD_H
#define VR_POPUP_HUD_H
#ifdef _WIN32
#pragma once
#endif

#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "vgui_controls/Panel.h"
#include "vgui_controls/EditablePanel.h"

// Forward declarations
class CHudElement;

//-----------------------------------------------------------------------------
// Purpose: Simple wrapper panel that paints a target panel at (0,0) offset
//          Used to bypass clipping issues when rendering HUD panels in 3D
//-----------------------------------------------------------------------------
class CVRPanelWrapper : public vgui::EditablePanel
{
    DECLARE_CLASS_SIMPLE(CVRPanelWrapper, vgui::EditablePanel);
public:
    CVRPanelWrapper(vgui::Panel* parent, const char* name);
    
    void SetTargetPanel(vgui::Panel* pPanel) { m_pTargetPanel = pPanel; }
    vgui::Panel* GetTargetPanel() const { return m_pTargetPanel; }
    
    // Content offset to nudge where content renders (like the hand HUD does)
    void SetContentOffset(int x, int y) { m_nContentOffsetX = x; m_nContentOffsetY = y; }
    
    virtual void Paint() override;
    
private:
    vgui::Panel* m_pTargetPanel;
    int m_nContentOffsetX;
    int m_nContentOffsetY;
};

//-----------------------------------------------------------------------------
// Purpose: Manages VR popup HUD elements like win/loss panels and scoreboard.
//          Uses spring-follow positioning for comfortable viewing.
//          Scoreboard overrides win/loss panel when TAB is held (vanilla behavior).
//-----------------------------------------------------------------------------
class CVRPopupHUDManager
{
public:
    CVRPopupHUDManager();
    ~CVRPopupHUDManager();
    
    bool Initialize();
    void Shutdown();
    void Update(float deltaTime);
    void Render();
    void ResetState();
    
    void SetEnabled(bool enabled) { m_bEnabled = enabled; }
    bool IsEnabled() const { return m_bEnabled; }
    
    // Check if any popup is currently being shown
    bool IsShowingPopup() const { return m_pActivePanel != nullptr; }

private:
    // Find TF2's HUD panels
    void AcquirePanels();
    
    // Determine which panel should be shown (scoreboard has priority)
    vgui::Panel* DetermineActivePanel();
    
    // Spring-follow positioning
    bool CalculateSpringTransform(VMatrix& transform);
    float GetCurrentViewYaw() const;
    void UpdateSpringYaw(float deltaTime);
    
private:
    bool m_bInitialized;
    bool m_bEnabled;
    
    // TF2 HUD panels we capture
    vgui::Panel* m_pScoreboardPanel;      // Scoreboard (TAB)
    vgui::Panel* m_pWinPanel;             // Win/loss panel
    vgui::Panel* m_pArenaWinPanel;        // Arena mode win panel
    vgui::Panel* m_pMatchSummaryPanel;    // Match summary
    
    // Wrapper panel for rendering match status without clipping
    CVRPanelWrapper* m_pMatchStatusWrapper;
    
    // Currently active panel (what we're rendering)
    vgui::Panel* m_pActivePanel;
    
    // Spring arm state
    float m_flCurrentYaw;       // Current spring arm yaw (world space)
    float m_flTargetYaw;        // Target yaw (player's view yaw)
    
    // Configuration
    float m_flDistance;         // Distance from head
    float m_flFollowSpeed;      // How fast the HUD follows rotation
    float m_flDeadzone;         // Angle deadzone where HUD stays locked
    float m_flMaxLagAngle;      // Max angle HUD can lag behind
    float m_flScale;            // World scale of the panel
    
    // Vertical offset (positive = up)
    float m_flVerticalOffset;
};

// Global instance
extern CVRPopupHUDManager* g_pVRPopupHUDManager;

#endif // VR_POPUP_HUD_H
