#ifndef VR_AMMO_OVERLAY_H
#define VR_AMMO_OVERLAY_H
#ifdef _WIN32
#pragma once
#endif

#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "openxr_hand_tracking.h"

// Forward declarations
class CTFHudWeaponAmmo;

class IMaterial;
class ITexture;
class C_TFPlayer;

// Forward declaration of VR ammo panel (implementation in .cpp)
class CVRAmmoPanel;

//-----------------------------------------------------------------------------
// Purpose: Manages rendering of ammo display as a 3D quad attached to player's main shooting hand
//-----------------------------------------------------------------------------
class CVRAmmoOverlay
{
public:
    CVRAmmoOverlay();
    ~CVRAmmoOverlay();

    // Initialize the overlay system
    bool Initialize();
    
    // Shutdown and cleanup
    void Shutdown();
    
    // Update each frame - checks for ammo changes and updates position
    void Update();
    
    // Render the ammo quad in 3D space
    void RenderAmmoQuad();
    
    // Set which hand the overlay is attached to (0=left, 1=right) - should be main shooting hand
    void SetHandAttachment(int hand);
    
    // Enable/disable the overlay
    void SetEnabled(bool enabled) { m_bEnabled = enabled; }
    bool IsEnabled() const { return m_bEnabled; }
    
    // Set position offset relative to hand
    void SetQuadOffset(const Vector& offset) { m_vQuadOffset = offset; }
    void SetQuadRotation(const QAngle& rotation) { m_angQuadRotation = rotation; }

private:
    // Calculate the quad transform matrix based on hand position
    bool CalculateQuadTransform(VMatrix& quadTransform);
    
    // Calculate transform using hand tracking instead of controller
    bool CalculateHandTrackingTransform(VMatrix& quadTransform);
    
    // Get current player ammo information
    bool GetPlayerAmmoInfo(int& currentAmmo, int& reserveAmmo, int& maxAmmo, bool& usesClips);
    
    // Force the ammo panel to update (since we can't call protected OnThink directly)
    void ForceAmmoUpdate();
    // Rendering resources
    CVRAmmoPanel* m_pAmmoPanel;          // VR-only ammo panel that doesn't draw to main HUD
    
    // State tracking
    bool        m_bInitialized;
    bool        m_bEnabled;
    int         m_nAttachedHand;          // 0=left, 1=right (should be main shooting hand)
    int         m_nLastAmmo;              // Cache to avoid unnecessary updates
    int         m_nLastReserveAmmo;
    int         m_nLastMaxAmmo;
    bool        m_bLastUsesClips;
    float       m_flLastUpdateTime;
    
    // Positioning
    Vector      m_vQuadOffset;            // Offset from hand position
    QAngle      m_angQuadRotation;        // Rotation relative to hand
    
    // Ammo region sampling from HUD (pixel coordinates in _rt_vgui)
    static const int AMMO_REGION_X = 1500;      // X position of ammo in HUD (right side)
    static const int AMMO_REGION_Y = 850;       // Y position of ammo in HUD (bottom)
    static const int AMMO_REGION_WIDTH = 400;   // Width of ammo region
    static const int AMMO_REGION_HEIGHT = 200;  // Height of ammo region
};

// Global instance
extern CVRAmmoOverlay* g_pVRAmmoOverlay;

#endif // VR_AMMO_OVERLAY_H
