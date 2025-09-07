//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: VR Health Overlay - Renders health bar on hand-attached quad in VR
//
//=============================================================================//

#ifndef VR_HEALTH_OVERLAY_H
#define VR_HEALTH_OVERLAY_H
#ifdef _WIN32
#pragma once
#endif

#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"

class IMaterial;
class ITexture;
class C_TFPlayer;
class CTFHudPlayerHealth;

//-----------------------------------------------------------------------------
// Purpose: Manages rendering of health bar as a 3D quad attached to player's hand
//-----------------------------------------------------------------------------
class CVRHealthOverlay
{
public:
    CVRHealthOverlay();
    ~CVRHealthOverlay();

    // Initialize the overlay system
    bool Initialize();
    
    // Shutdown and cleanup
    void Shutdown();
    
    // Update each frame - checks for health changes and updates position
    void Update();
    
    // Render the health quad in 3D space
    void RenderHealthQuad();
    
    // Set which hand the overlay is attached to (0=left, 1=right)
    void SetHandAttachment(int hand);
    
    // Enable/disable the overlay
    void SetEnabled(bool enabled) { m_bEnabled = enabled; }
    bool IsEnabled() const { return m_bEnabled; }
    
    // Set position offset relative to hand
    void SetQuadOffset(const Vector& offset) { m_vQuadOffset = offset; }
    void SetQuadRotation(const QAngle& rotation) { m_angQuadRotation = rotation; }

private:
    // Create the render target texture for health display
    bool CreateHealthTexture();
    
    // Update the health panel and render to texture
    void UpdateHealthTexture(float healthPercent, int currentHealth, int maxHealth);
    
    // Calculate the quad transform matrix based on hand position
    bool CalculateQuadTransform(VMatrix& quadTransform);
    
    // Get current player health information
    bool GetPlayerHealthInfo(float& healthPercent, int& currentHealth, int& maxHealth);

private:
    // Rendering resources
    ITexture*   m_pHealthTexture;       // Render target for health display
    IMaterial*  m_pHealthMaterial;      // Material for rendering the quad
    CTFHudPlayerHealth* m_pPlayerStatusPanel; // TF2's health widget (cross + numbers)
    
    // State tracking
    bool        m_bInitialized;
    bool        m_bEnabled;
    int         m_nAttachedHand;        // 0=left, 1=right
    float       m_flLastHealthPercent;  // Cache to avoid unnecessary updates
    int         m_nLastHealth;
    int         m_nLastMaxHealth;
    float       m_flLastUpdateTime;
    
    // Positioning
    Vector      m_vQuadOffset;          // Offset from hand position
    QAngle      m_angQuadRotation;      // Rotation relative to hand
    
    // Health panel dimensions (for texture size)
    static const int HEALTH_TEXTURE_WIDTH = 64;
    static const int HEALTH_TEXTURE_HEIGHT = 64;
};

// Global instance
extern CVRHealthOverlay* g_pVRHealthOverlay;

#endif // VR_HEALTH_OVERLAY_H
