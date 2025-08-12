#ifndef VR_LASER_POINTER_H
#define VR_LASER_POINTER_H

#include "cbase.h"
#include "openxr_manager.h"

class CVRInput;

class CVRLaserPointer : public CAutoGameSystemPerFrame
{
public:
    CVRLaserPointer();
    ~CVRLaserPointer();

    void Initialize();
    void Shutdown();
    void Update(float frametime) override;

    // Laser pointer state
    bool IsLaserActive() const { return m_bLaserActive; }
    Vector GetLaserStart() const { return m_laserStart; }
    Vector GetLaserEnd() const { return m_laserEnd; }
    bool GetLaserHitPoint(Vector& hitPoint, Vector& hitNormal, C_BaseEntity*& hitEntity);

private:
    void UpdateLaserPointer();
    void RenderLaserPointer();
    void PerformLaserRaycast();

    // Laser state
    bool m_bLaserActive;
    Vector m_laserStart;
    Vector m_laserEnd;
    Vector m_laserHitPoint;
    Vector m_laserHitNormal;
    C_BaseEntity* m_pLaserHitEntity;
    
    // Laser properties
    float m_laserLength;
    float m_laserWidth;
    Color m_laserColor;
    
    // Raycast results
    trace_t m_laserTrace;
    bool m_bLaserHit;
    
    // Rendering
    IMaterial* m_pLaserMaterial;
    bool m_bLaserMaterialCreated;
};

extern CVRLaserPointer* g_pVRLaserPointer;

#endif // VR_LASER_POINTER_H
