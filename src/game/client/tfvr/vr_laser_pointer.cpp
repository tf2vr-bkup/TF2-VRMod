#include "cbase.h"
#include "vr_laser_pointer.h"
#include "openxr_manager.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialsystem.h"
#include "engine/ivdebugoverlay.h"
#include "vgui_controls/Controls.h"
#include "convar.h"

// ConVars for laser pointer control
ConVar tfvr_laser_enabled("tfvr_laser_enabled", "1", FCVAR_ARCHIVE, "Enable VR laser pointer");
ConVar tfvr_laser_length("tfvr_laser_length", "100.0", FCVAR_ARCHIVE, "Length of the laser pointer in game units");
ConVar tfvr_laser_width("tfvr_laser_width", "2.0", FCVAR_ARCHIVE, "Width of the laser pointer line");
ConVar tfvr_laser_color_r("tfvr_laser_color_r", "255", FCVAR_ARCHIVE, "Red component of laser color (0-255)");
ConVar tfvr_laser_color_g("tfvr_laser_color_g", "0", FCVAR_ARCHIVE, "Green component of laser color (0-255)");
ConVar tfvr_laser_color_b("tfvr_laser_color_b", "0", FCVAR_ARCHIVE, "Blue component of laser color (0-255)");
ConVar tfvr_laser_debug("tfvr_laser_debug", "0", FCVAR_ARCHIVE, "Show debug info for laser pointer");

// Global instance
CVRLaserPointer* g_pVRLaserPointer = nullptr;

CVRLaserPointer::CVRLaserPointer()
    : m_bLaserActive(false)
    , m_laserStart(vec3_origin)
    , m_laserEnd(vec3_origin)
    , m_laserHitPoint(vec3_origin)
    , m_laserHitNormal(vec3_origin)
    , m_pLaserHitEntity(nullptr)
    , m_laserLength(100.0f)
    , m_laserWidth(2.0f)
    , m_laserColor(255, 0, 0, 255) // Red laser
    , m_bLaserHit(false)
    , m_pLaserMaterial(nullptr)
    , m_bLaserMaterialCreated(false)
{
}

CVRLaserPointer::~CVRLaserPointer()
{
    Shutdown();
}

void CVRLaserPointer::Initialize()
{
    // Temporarily disable material creation to avoid crashes
    // TODO: Fix material creation once basic system is working
    /*
    // Create laser material with a valid shader
    KeyValues* pVMTKeyValues = new KeyValues("VertexLitGeneric");
    pVMTKeyValues->SetString("$basetexture", "white");
    pVMTKeyValues->SetString("$color", "255 0 0");
    pVMTKeyValues->SetString("$additive", "1");
    pVMTKeyValues->SetString("$translucent", "1");
    pVMTKeyValues->SetString("$selfillum", "1");
    
    m_pLaserMaterial = materials->CreateMaterial("vr_laser_material", pVMTKeyValues);
    if (m_pLaserMaterial)
    {
        m_bLaserMaterialCreated = true;
        DevMsg("VR Laser Pointer: Material created successfully\n");
    }
    else
    {
        DevMsg("VR Laser Pointer: Failed to create material\n");
    }
    
    pVMTKeyValues->deleteThis();
    */
    
    DevMsg("VR Laser Pointer: Initialized (material creation disabled)\n");
}

void CVRLaserPointer::Shutdown()
{
    // Materials are reference-counted and automatically destroyed
    m_pLaserMaterial = nullptr;
    m_bLaserMaterialCreated = false;
}

void CVRLaserPointer::Update(float frametime)
{
    if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
        return;
    
    // Check if laser is enabled
    if (!tfvr_laser_enabled.GetBool())
        return;
    
    UpdateLaserPointer();
    PerformLaserRaycast();
    RenderLaserPointer();
}

void CVRLaserPointer::UpdateLaserPointer()
{
    // Get right controller pose for laser pointer
    VMatrix rightControllerPose;
    if (g_pOpenXRManager->GetRightControllerPose(rightControllerPose))
    {
        // Extract position and orientation
        Vector controllerPos = rightControllerPose.GetTranslation();
        QAngle controllerAngles;
        MatrixAngles(rightControllerPose.As3x4(), controllerAngles);
        
        // Calculate laser direction
        Vector forward;
        AngleVectors(controllerAngles, &forward);
        
        // Set laser start and end points using ConVar
        m_laserStart = controllerPos;
        m_laserLength = tfvr_laser_length.GetFloat();
        m_laserEnd = controllerPos + forward * m_laserLength;
        m_bLaserActive = true;
        
        // Update laser color from ConVars
        m_laserColor.SetColor(
            tfvr_laser_color_r.GetInt(),
            tfvr_laser_color_g.GetInt(),
            tfvr_laser_color_b.GetInt(),
            255
        );
        
        if (tfvr_laser_debug.GetBool())
        {
            DevMsg("Laser: pos(%.2f, %.2f, %.2f) dir(%.2f, %.2f, %.2f) length=%.1f\n",
                   controllerPos.x, controllerPos.y, controllerPos.z,
                   forward.x, forward.y, forward.z, m_laserLength);
        }
    }
    else
    {
        m_bLaserActive = false;
    }
}

void CVRLaserPointer::PerformLaserRaycast()
{
    if (!m_bLaserActive)
        return;
    
    // Perform raycast from laser start to end
    m_bLaserHit = false;
    m_pLaserHitEntity = nullptr;
    
    // Use engine's trace line function
    trace_t tr;
    UTIL_TraceLine(m_laserStart, m_laserEnd, MASK_SOLID, nullptr, COLLISION_GROUP_NONE, &tr);
    
    if (tr.fraction < 1.0f)
    {
        m_bLaserHit = true;
        m_laserHitPoint = tr.endpos;
        m_laserHitNormal = tr.plane.normal;
        m_pLaserHitEntity = tr.m_pEnt;
        
        // Update laser end to hit point
        m_laserEnd = m_laserHitPoint;
    }
}

void CVRLaserPointer::RenderLaserPointer()
{
    if (!m_bLaserActive)
        return;
    
    // Draw laser line using debug overlay (no material required)
    if (debugoverlay)
    {
        // Draw the laser line
        debugoverlay->AddLineOverlayAlpha(m_laserStart, m_laserEnd,
                                         m_laserColor.r(), m_laserColor.g(), m_laserColor.b(), m_laserColor.a(),
                                         false, 0.016f); // 16ms frame time
        
        // Draw hit point if we hit something
        if (m_bLaserHit)
        {
            debugoverlay->AddBoxOverlay(m_laserHitPoint, Vector(-2, -2, -2), Vector(2, 2, 2), 
                                       QAngle(0, 0, 0), 255, 255, 0, 255, 0.016f);
        }
    }
}

bool CVRLaserPointer::GetLaserHitPoint(Vector& hitPoint, Vector& hitNormal, C_BaseEntity*& hitEntity)
{
    if (!m_bLaserHit)
        return false;
    
    hitPoint = m_laserHitPoint;
    hitNormal = m_laserHitNormal;
    hitEntity = m_pLaserHitEntity;
    return true;
}
