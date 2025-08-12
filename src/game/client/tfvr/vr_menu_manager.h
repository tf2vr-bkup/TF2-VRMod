#ifndef VR_MENU_MANAGER_H
#define VR_MENU_MANAGER_H

#include "cbase.h"
#include "mathlib/vector.h"
#include "vgui/IInput.h"
#include "vgui/IInputInternal.h"
#include "client_virtualreality.h"

// Forward declarations
class C_TFPlayer;
class COpenXRManager;

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

private:
    // Helper functions
    void UpdateCursorPosition();
    void HandleMenuButtonInput();
    
    // Cursor positioning
    void ComputeCursorPosition(const Vector& pointerPosition, const QAngle& pointerRotation, int& px, int& py);
    bool ComputeIntersectionBarycentricCoordinates(const Vector& rayStart, const Vector& rayEnd, 
                                                   const Vector& ul, const Vector& ur, const Vector& ll, const Vector& lr,
                                                   float& u, float& v);

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
    
    // ConVars
    ConVar* m_pConVarPrimaryHand;
};

// Global instance
extern CVRMenuManager* g_pVRMenuManager;

#endif // VR_MENU_MANAGER_H
