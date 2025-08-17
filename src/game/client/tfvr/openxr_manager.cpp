#include "cbase.h"
#include "openxr_manager.h"
#include "openxr_input.h"
#include "hmdWrapper.h"
#include "vr_rendertargets.h"
#include "cdll_client_int.h"
#include "ienginevgui.h"
#include "iclientmode.h"
#include "vr_input.h"
#include "iinput.h"
#include "vr_laser_pointer.h"
#include "vr_menu_manager.h"
#include "engine/ivdebugoverlay.h"
#include "c_tf_player.h"
#include "tf_playerclass_shared.h"
#include "const.h"
#include "c_tf_playerclass.h"

#include "mathlib/mathlib.h"

// Forward declaration for global menu manager pointer
extern class CVRMenuManager* g_pVRMenuManager;

ConVar tfvr_worldscale("tfvr_worldscale", "48", FCVAR_ARCHIVE | FCVAR_REPLICATED, "This scales everything.");
#define METERS_TO_GAME_UNITS tfvr_worldscale.GetFloat()

ConVar tfvr_controller_debug_draw("tfvr_controller_debug_draw", "1", FCVAR_ARCHIVE, "Draw debug visualization for controller positions and orientations");
ConVar tfvr_msaa("tfvr_msaa", "4", FCVAR_ARCHIVE, "Controls multi-sampling anti-aliasing levels in TFVR. Set to the number of samples to use.");
ConVar tfvr_dynamic_worldscale("tfvr_dynamic_worldscale", "1", FCVAR_ARCHIVE, "Enable dynamic world scaling based on merc height and crouch state");
ConVar tfvr_forcemaxlod("tfvr_forcemaxlod", "1", FCVAR_ARCHIVE);
ConVar tfvr_hud_forward("tfvr_hud_forward", "15", FCVAR_ARCHIVE, "Apparent distance of the HUD in inches");
ConVar tfvr_hud_scale("tfvr_hud_scale", "0.5", FCVAR_ARCHIVE);
ConVar tfvr_hud_axis_lock_to_world("tfvr_hud_axis_lock_to_world", "5", FCVAR_ARCHIVE, "Bitfield - locks HUD axes to the world - 1=pitch, 2=yaw, 4=roll");
ConVar tfvr_hud_height_adjust("tfvr_hud_height_adjust", "0", FCVAR_ARCHIVE);

// Height calibration and seated mode ConVars
ConVar tfvr_player_height("tfvr_player_height", "67", FCVAR_ARCHIVE, "Player's real height in inches for VR calibration");
ConVar tfvr_height_calibration("tfvr_height_calibration", "1", FCVAR_ARCHIVE, "Enable height calibration for world scaling");
ConVar tfvr_seated_mode("tfvr_seated_mode", "0", FCVAR_ARCHIVE, "Enable seated VR mode");
ConVar tfvr_seated_height_offset("tfvr_seated_height_offset", "24", FCVAR_ARCHIVE, "Height offset for seated mode in inches");
ConVar tfvr_calibration_debug("tfvr_calibration_debug", "0", FCVAR_ARCHIVE, "Show debug output for height calibration and seated mode");


ConVar tfvr_menu_scale("tfvr_menu_scale", "0.7", FCVAR_ARCHIVE);

ConVar tfvr_r_show_both_eyes("tfvr_r_show_both_eyes", "0", FCVAR_ARCHIVE, "Show both eyes on the game window.");

// Common conversions
namespace
{
	// Calculate dynamic world scale based on merc height, crouch state, and height calibration
	float CalculateDynamicWorldScale()
	{
		// Get the base world scale ConVar
		static ConVar* tfvr_worldscale = cvar->FindVar("tfvr_worldscale");
		float baseWorldScale = tfvr_worldscale ? tfvr_worldscale->GetFloat() : 48.0f;
		
		// Check if dynamic scaling is enabled
		static ConVar* tfvr_dynamic_worldscale = cvar->FindVar("tfvr_dynamic_worldscale");
		if (!tfvr_dynamic_worldscale || !tfvr_dynamic_worldscale->GetBool())
			return baseWorldScale;
		
		// Get local player to check class and crouch state
		C_TFPlayer* pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
		if (!pLocalPlayer)
			return baseWorldScale;
		
		// Get the player's class eye height
		float classEyeHeight = 72.0f; // Default TF2 eye height
		
		// Get player class for height calculation and debug output
		const C_TFPlayerClass* pPlayerClass = pLocalPlayer->GetPlayerClass();
		
		// Only adjust world scale based on class height, not crouch state
		// Crouching will use the normal TF2 animation/view changes
		if (pPlayerClass)
		{
			// Get class-specific eye height
			int classIndex = pPlayerClass->GetClassIndex();
			switch (classIndex)
			{
				case TF_CLASS_SCOUT:
				case TF_CLASS_CIVILIAN:
					classEyeHeight = 65.0f;
					break;
				case TF_CLASS_SNIPER:
				case TF_CLASS_MEDIC:
				case TF_CLASS_HEAVYWEAPONS:
				case TF_CLASS_SPY:
					classEyeHeight = 75.0f;
					break;
				case TF_CLASS_SOLDIER:
				case TF_CLASS_DEMOMAN:
				case TF_CLASS_PYRO:
				case TF_CLASS_ENGINEER:
					classEyeHeight = 68.0f;
					break;
				default:
					classEyeHeight = 72.0f; // Default
					break;
			}
		}
		
		// Calculate base class scale: base scale * (class height / default height)
		float classScale = baseWorldScale * (classEyeHeight / 72.0f);
		
		// Apply height calibration if enabled
		static ConVar* tfvr_height_calibration = cvar->FindVar("tfvr_height_calibration");
		static ConVar* tfvr_player_height = cvar->FindVar("tfvr_player_height");
		static ConVar* tfvr_seated_mode = cvar->FindVar("tfvr_seated_mode");
		
		if (tfvr_height_calibration && tfvr_height_calibration->GetBool() && 
		    tfvr_player_height && tfvr_seated_mode)
		{
			// Don't apply height calibration in seated mode - it uses base class scale only
			if (!tfvr_seated_mode->GetBool())
			{
				// Standard height calibration for standing VR
				// Shorter players need BIGGER world scale, taller players need SMALLER world scale
				// This makes the world feel the right size relative to their height
				float playerHeight = tfvr_player_height->GetFloat();
				float averageHeight = 67.0f; // Average adult height in inches
				float heightScale = averageHeight / playerHeight; // INVERTED: shorter = bigger scale
				
				classScale *= heightScale;
			}
		}
		
		float dynamicScale = classScale;
		
		// Debug output with calibration info
		static float lastDebugTime = 0.0f;
		static ConVar* tfvr_calibration_debug = cvar->FindVar("tfvr_calibration_debug");
		if (gpGlobals && gpGlobals->realtime - lastDebugTime > 2.0f && 
		    tfvr_calibration_debug && tfvr_calibration_debug->GetBool()) // Only print every 2 seconds
		{
			bool heightCalibActive = tfvr_height_calibration && tfvr_height_calibration->GetBool();
			bool seatedMode = tfvr_seated_mode && tfvr_seated_mode->GetBool();
			float playerHeight = tfvr_player_height ? tfvr_player_height->GetFloat() : 67.0f;
			
			DevMsg("VR World Scale: Class=%s, EyeHeight=%.1f, Scale=%.1f, HeightCalib=%s, Seated=%s, PlayerHeight=%.1f\n", 
				pPlayerClass ? pPlayerClass->GetName() : "Unknown",
				classEyeHeight,
				dynamicScale,
				heightCalibActive ? "ON" : "OFF",
				seatedMode ? "ON" : "OFF",
				playerHeight);
			lastDebugTime = gpGlobals->realtime;
		}
		
		return dynamicScale;
	}

	VMatrix ConvertFromOpenXRQuatVector(const Quaternion &rot, const Vector &pos)
	{
		VMatrix result;
		result.Identity();

		// OpenXR to Source coordinate conversion:
		// OpenXR: +X=right, +Y=up, +Z=forward
		// Source: +X=forward, +Y=left, +Z=up
		// So: OpenXR.x -> -Source.y (right -> left)
		//     OpenXR.y -> Source.z  (up -> up)
		//     OpenXR.z -> Source.x  (forward -> forward)
		Quaternion convertedRot(-rot.z, -rot.x, rot.y, rot.w);
		Vector convertedPos(-pos.z, -pos.x, pos.y);
		convertedPos *= CalculateDynamicWorldScale();

        matrix3x4_t matrix;
		QuaternionMatrix(convertedRot, convertedPos, matrix);
        result.CopyFrom3x4(matrix);
		return result;
	}

	VMatrix ToSourceCoordinateSystem(const XrPosef& pose)
	{
		Vector oXrPos(pose.position.x, pose.position.y, pose.position.z);
        Quaternion oXrRot(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);

		return ConvertFromOpenXRQuatVector(oXrRot, oXrPos);
	}

	// New function for proper floor-aligned coordinate conversion
	VMatrix ToSourceCoordinateSystemFloorAligned(const XrPosef& pose)
	{
		// OpenXR to Source coordinate conversion with automatic floor alignment:
		// OpenXR: +X=right, +Y=up, +Z=forward
		// Source: +X=forward, +Y=left, +Z=up
		// 
		// The key insight is that OpenXR's STAGE reference space should already
		// be aligned with the physical floor. We just need to convert coordinates
		// properly without manual offsets.
		
		Quaternion convertedRot(-pose.orientation.z, -pose.orientation.x, pose.orientation.y, pose.orientation.w);
		
		// Convert position with automatic floor alignment
		Vector convertedPos;
		convertedPos.x = -pose.position.z * CalculateDynamicWorldScale();  // OpenXR Z -> Source X (forward)
		convertedPos.y = -pose.position.x * CalculateDynamicWorldScale();  // OpenXR X -> Source Y (left)
		
		// Convert position with automatic floor alignment
		// The world scaling system should handle class height differences naturally
		float rawZ = pose.position.y * CalculateDynamicWorldScale();
		
		// Ensure Z is never negative - the floor should always be at Z=0 or higher
		convertedPos.z = max(0.0f, rawZ);  // OpenXR Y -> Source Z (up)
		
		
		// Debug output to see what we're getting
		static float lastDebugTime = 0.0f;
		if (gpGlobals && gpGlobals->realtime - lastDebugTime > 1.0f) // Only print every second
		{
			DevMsg("OpenXR Coord Debug - Raw: (%.3f, %.3f, %.3f) -> Converted: (%.1f, %.1f, %.1f)\n", 
				pose.position.x, pose.position.y, pose.position.z,
				convertedPos.x, convertedPos.y, convertedPos.z);
			lastDebugTime = gpGlobals->realtime;
		}

        matrix3x4_t matrix;
		QuaternionMatrix(convertedRot, convertedPos, matrix);
		
		VMatrix result;
        result.CopyFrom3x4(matrix);
		return result;
	}
}

void ShutdownOpenXRManager() 
{
    if (g_pOpenXRManager) 
    {
        g_pOpenXRManager->Shutdown();
        delete g_pOpenXRManager;
        g_pOpenXRManager = nullptr;
        DevMsg("OpenXR manager shut down.\n");
    }
}

void InitializeOpenXRManager() 
{
    if (!g_pOpenXRManager) 
    {
        DevMsg("Allocating OpenXR manager...\n");
        g_pOpenXRManager = new COpenXRManager();


        if (!g_pOpenXRManager->Initialize()) 
        {
            DevMsg("OpenXR manager initialization failed!\n");
            ShutdownOpenXRManager();
        }
        else 
        {
            DevMsg("OpenXR manager initialized successfully.\n");
        }
    }
    else 
    {
        DevMsg("OpenXR manager already initialized.\n");
    }
}

COpenXRManager::COpenXRManager() 
{
    m_vrActive = false;
    m_inputManager = nullptr;
    m_menuManager = nullptr;
    m_laserPointer = nullptr;
}

COpenXRManager::~COpenXRManager() 
{
    Shutdown();
}

bool COpenXRManager::Initialize() 
{
    if (m_vrActive) return true;

    // Initialize OpenXR
    if (!CreateOpenXRInstance()) return false;
    if (!GetSystem()) return false;

    XrViewConfigurationView viewConfigs[2] = { {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW} };
    XrResult result = xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &m_viewCount, m_viewConfigs);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to enumerate view configurations: %d\n", result);
        return false;
    }

    if (!CreateSession()) return false;
    if (!CreateReferenceSpace()) return false;
    if (!CreateHeadSpace()) return false;

    // Initialize input system
    m_inputManager = new COpenXRInputManager(this);
    if (!m_inputManager->Initialize())
    {
        DevMsg("Failed to initialize input system\n");
        delete m_inputManager;
        m_inputManager = nullptr;
        return false;
    }

    if (!dxvkInitOpenXR(m_instance, m_systemId, m_session, m_referenceSpace, m_headSpace))
    {
        DevMsg("Failed to send OpenXR info to DXVK");
        return false;
    }

    // Initialize VR Menu Manager
    m_menuManager = new CVRMenuManager();
    m_menuManager->Initialize();

    // Initialize VR Laser Pointer
    m_laserPointer = new CVRLaserPointer();
    m_laserPointer->Initialize();
    DevMsg("VR Laser Pointer initialized\n");

    // Set the global pointer for external access
    g_pVRMenuManager = m_menuManager;

    m_vrActive = true;
    DevMsg("OpenXR VR mode initialized successfully!\n");
    return true;
}

void COpenXRManager::Shutdown() 
{
    if (!m_vrActive) return;

    // Clean up VR Menu Manager
    if (m_menuManager)
    {
        m_menuManager->Shutdown();
        delete m_menuManager;
        m_menuManager = nullptr;
    }

    // Clean up VR Laser Pointer
    if (m_laserPointer)
    {
        m_laserPointer->Shutdown();
        delete m_laserPointer;
        m_laserPointer = nullptr;
    }

    // Clean up input manager
    if (m_inputManager)
    {
        m_inputManager->Shutdown();
        delete m_inputManager;
        m_inputManager = nullptr;
    }

    ReleaseResources();

    m_vrActive = false;
    DevMsg("OpenXR resources cleaned up.\n");
}

bool COpenXRManager::CreateOpenXRInstance() 
{
    const char* extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = extensions;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    strcpy_s(createInfo.applicationInfo.applicationName, "TF2VR");
    strcpy_s(createInfo.applicationInfo.engineName, "Source 2013");
    
    XrResult result = xrCreateInstance(&createInfo, &m_instance);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create OpenXR instance: %d\n", result);
        return false;
    }
    
    // Load extension function pointers right after instance creation
    result = xrGetInstanceProcAddr
    (
        m_instance,
        "xrGetVulkanGraphicsRequirements2KHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnGetVulkanGraphicsRequirements2KHR)
    );
    
    if (result != XR_SUCCESS || m_pfnGetVulkanGraphicsRequirements2KHR == nullptr) 
    {
        DevMsg("Failed to get xrGetVulkanGraphicsRequirements2KHR function: %d\n", result);
        return false;
    }
    
    // Other Vulkan-related function pointers
    result = xrGetInstanceProcAddr
    (
        m_instance,
        "xrCreateVulkanInstanceKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnCreateVulkanInstanceKHR)
    );
    
    if (result != XR_SUCCESS || m_pfnCreateVulkanInstanceKHR == nullptr) 
    {
        DevMsg("Failed to get xrCreateVulkanInstanceKHR function: %d\n", result);
        return false;
    }
    
    result = xrGetInstanceProcAddr
    (
        m_instance,
        "xrCreateVulkanDeviceKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnCreateVulkanDeviceKHR)
    );
    
    if (result != XR_SUCCESS || m_pfnCreateVulkanDeviceKHR == nullptr) 
    {
        DevMsg("Failed to get xrCreateVulkanDeviceKHR function: %d\n", result);
        return false;
    }

    result = xrGetInstanceProcAddr
    (
        m_instance,
        "xrGetVulkanGraphicsDevice2KHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnGetVulkanGraphicsDevice2KHR)
    );

    if (result != XR_SUCCESS || m_pfnGetVulkanGraphicsDevice2KHR == nullptr) 
    {
        DevMsg("Failed to get xrGetVulkanGraphicsDevice2KHR function: %d\n", result);
        return false;
    }
    
    DevMsg("OpenXR instance created with Vulkan 2 support!\n");
    return true;
}

bool COpenXRManager::GetSystem() 
{
    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult result = xrGetSystem(m_instance, &systemInfo, &m_systemId);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to get VR system: %d\n", result);
        return false;
    }
    DevMsg("VR system retrieved with ID: %llu\n", m_systemId);
    return true;
}

bool COpenXRManager::CreateSession() 
{
    if (!m_pfnGetVulkanGraphicsRequirements2KHR) 
    {
        DevMsg("Vulkan graphics requirements function not available\n");
        return false;
    }
    
    // Get OpenXR's Vulkan requirements BEFORE creating the Vulkan instance
    XrGraphicsRequirementsVulkan2KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    XrResult result = m_pfnGetVulkanGraphicsRequirements2KHR(m_instance, m_systemId, &graphicsRequirements);
    if (result != XR_SUCCESS) 
    {
        DevMsg("xrGetVulkanGraphicsRequirements2KHR failed: %d\n", result);
        return false;
    }
    
    // Now that we have the requirements, create the Vulkan context using OpenXR helper functions
    if (!CreateVulkanContext(&graphicsRequirements)) 
    {
        DevMsg("Vulkan context creation failed!\n");
        return false;
    }
   
    // Set up Vulkan graphics binding - use the Vulkan2KHR version 
    XrGraphicsBindingVulkan2KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    graphicsBinding.instance = m_vkInstance;
    graphicsBinding.physicalDevice = m_vkPhysicalDevice;
    graphicsBinding.device = m_vkDevice;
    graphicsBinding.queueFamilyIndex = m_vkQueueFamilyIndex;
    graphicsBinding.queueIndex = 0;

    // Create OpenXR session
    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = m_systemId;
    sessionInfo.createFlags = 0;

    result = xrCreateSession(m_instance, &sessionInfo, &m_session);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create OpenXR session: %d\n", result);
        return false;
    }
    DevMsg("OpenXR session created!\n");

    XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    result = xrBeginSession(m_session, &beginInfo);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to begin OpenXR session: %d\n", result);
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
        return false;
    }
    DevMsg("OpenXR session started successfully!\n");
	m_sessionRunning = true;
    return true;
}

bool COpenXRManager::CreateReferenceSpace() 
{
    // Try to create a STAGE reference space first (floor-aligned)
    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace = { {0, 0, 0, 1}, {0, 0, 0} };
    XrResult result = xrCreateReferenceSpace(m_session, &spaceInfo, &m_referenceSpace);
    
    if (!XR_SUCCEEDED(result))
    {
        // Fall back to LOCAL if STAGE is not supported
        DevMsg("STAGE reference space not supported, falling back to LOCAL: %d\n", result);
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = xrCreateReferenceSpace(m_session, &spaceInfo, &m_referenceSpace);
        
        if (!XR_SUCCEEDED(result))
        {
            DevMsg("Failed to create reference space: %d\n", result);
            return false;
        }
        DevMsg("LOCAL reference space created successfully\n");
    }
    else
    {
        DevMsg("STAGE reference space created successfully (floor-aligned)\n");
    }
    
    return true;
}

bool COpenXRManager::CreateHeadSpace()
{
    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace = { {0, 0, 0, 1}, {0, 0, 0} };
    XrResult result = xrCreateReferenceSpace(m_session, &spaceInfo, &m_headSpace);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create head space: %d\n", result);
        return false;
    }
    DevMsg("Head space created successfully!\n");
    return true;
}

void COpenXRManager::ReleaseResources() 
{
    // Clean up shared render target
    if (m_pSharedRenderTarget) 
    {
        m_pSharedRenderTarget->DecrementReferenceCount();
        m_pSharedRenderTarget = nullptr;
    }

    for (auto& swapchain : m_swapchains) 
    {
        xrDestroySwapchain(swapchain);
    }

    if (m_viewSpace) xrDestroySpace(m_viewSpace);
    if (m_referenceSpace) xrDestroySpace(m_referenceSpace);
    if (m_headSpace) xrDestroySpace(m_headSpace);
    if (m_session) xrDestroySession(m_session);
    if (m_instance) xrDestroyInstance(m_instance);
}

bool COpenXRManager::BeginFrame()
{
    return dxvkBeginFrame();
}

bool COpenXRManager::EndFrame()
{
    return dxvkEndFrame();
}

bool COpenXRManager::InitializeSharedRenderTarget()
{
    // Clean up existing shared render target
    if (m_pSharedRenderTarget) 
    {
        m_pSharedRenderTarget->DecrementReferenceCount();
        m_pSharedRenderTarget = nullptr;
    }

    // Get width and height from the view configs
    uint32_t width = m_viewConfigs[0].recommendedImageRectWidth;
    uint32_t height = m_viewConfigs[0].recommendedImageRectHeight;

    // Create a shared render target with double the width for both eyes
    char rtName[64];
    V_sprintf_safe(rtName, "_rt_VRSharedEyes");

    materials->BeginRenderTargetAllocation();
    
    m_pSharedRenderTarget = materials->CreateNamedRenderTargetTextureEx
    (
        rtName,
        width * 2, height,  // Double width to fit both eyes
        RT_SIZE_NO_CHANGE,
        IMAGE_FORMAT_BGRA8888,
        MATERIAL_RT_DEPTH_SHARED,
        TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_SRGB
    );

    materials->EndRenderTargetAllocation();
   
    
    if (!m_pSharedRenderTarget)
    {
        Warning("Failed to create shared render target for VR eyes\n");
        return false;
    }
    
    return true;
}

ITexture* COpenXRManager::GetSharedRenderTarget()
{
    // If the shared render target doesn't exist yet, create it
    if (!m_pSharedRenderTarget) 
    {
        if (!InitializeSharedRenderTarget()) 
        {
            return nullptr;
        }
    }
    
    return m_pSharedRenderTarget;
}

IMaterial* renderTargetMaterials[VR_NUM_BUFFERS];
IMaterial* COpenXRManager::GetRenderTargetMat()
{
	if (!renderTargetMaterials[m_currentRenderBufferIndex])
	{
		char name[256];
		sprintf(name, "vr_target_material_%d", m_currentRenderBufferIndex);

		KeyValues *keys = new KeyValues("UnlitGeneric");
		keys->SetInt("$ignorez", 1);
		keys->SetInt("$nocull", 1);
		keys->SetString("$basetexture", backBufferNamePerIndex(m_currentRenderBufferIndex));

		renderTargetMaterials[m_currentRenderBufferIndex] = materials->CreateMaterial(name, keys);
		Assert(!IsErrorMaterial(renderTargetMaterials[m_currentRenderBufferIndex]));
	}

	return renderTargetMaterials[m_currentRenderBufferIndex];
}

ITexture* COpenXRManager::GetRenderTarget(int index)
{
	if (index < 0)
		index = m_currentRenderBufferIndex;
	index = index % VR_NUM_BUFFERS;
	return vrRenderTargets->GetVRRenderTarget(index);
}

void COpenXRManager::GetSpectatorScreenDims(uint32_t &width, uint32_t &height)
{
	width = m_spectatorScreenWidth;
	height = m_spectatorScreenHeight;
}

void COpenXRManager::UpdateOpenXRViewData()
{
    if (!m_vrActive || !m_session)
    {
        return;
    }

    // Poll input state here since the frame state is valid after BeginFrame
    if (m_inputManager)
    {
        m_inputManager->PollInput();
    }

    uint32_t viewCount;
    dxvkGetViews(m_views, m_headLocation, viewCount);
}

bool COpenXRManager::GetEyeViewLocations(VMatrix& leftEyePose, VMatrix& rightEyePose)
{
    if (!m_vrActive || !m_session)
    {
        return false;
    }

    leftEyePose = ToSourceCoordinateSystemFloorAligned(m_views[0].pose);
    rightEyePose = ToSourceCoordinateSystemFloorAligned(m_views[1].pose);
    return true;
}


VMatrix COpenXRManager::CreateVRProjectionMatrix(XrFovf fov, float zNear, float zFar)
{
    VMatrix mat;
    mat.Identity();

    // OpenXR FOV angles are already in radians
    float tanLeft = zNear * tanf(fov.angleLeft);
    float tanRight = zNear * tanf(fov.angleRight);
    float tanDown = zNear * tanf(fov.angleDown);
    float tanUp = zNear * tanf(fov.angleUp);

    float tanWidth = tanRight - tanLeft;
    float tanHeight = tanUp - tanDown;

    // Compute scale and offset terms with near plane factor
    float xScale = (2.0f * zNear) / tanWidth;      // X scale
    float yScale = (2.0f * zNear) / tanHeight;     // Y scale
    float xOffset = (tanLeft + tanRight) / tanWidth;  // X offset
    float yOffset = (tanDown + tanUp) / tanHeight;    // Y offset

    // Compute depth terms
    float zScale = -(zFar + zNear) / (zFar - zNear);
    float zOffset = (-2.0f * zFar * zNear) / (zFar - zNear);

    // Populate the matrix (assuming VMatrix is row-major: mat[row][col])
    mat[0][0] = xScale;    // X scale
    mat[1][1] = yScale;    // Y scale
    mat[0][2] = xOffset;   // X offset
    mat[1][2] = yOffset;   // Y offset
    mat[2][2] = zScale;    // Z scale
    mat[2][3] = zOffset;   // Z offset
    mat[3][2] = -1.0f;     // Perspective term
    mat[3][3] = 0.0f;      // Homogeneous coordinate

    /*
    // Log input values
    DevMsg("VR Projection Matrix Input:\n");
    DevMsg("  FOV (rad): L=%.3f, R=%.3f, D=%.3f, U=%.3f\n", fov.angleLeft, fov.angleRight, fov.angleDown, fov.angleUp);
    DevMsg("  FOV (deg): L=%.1f, R=%.1f, D=%.1f, U=%.1f\n", RAD2DEG(fov.angleLeft), RAD2DEG(fov.angleRight), RAD2DEG(fov.angleDown), RAD2DEG(fov.angleUp));
    DevMsg("  zNear=%.3f, zFar=%.3f\n", zNear, zFar);

    // Log computed values
    DevMsg("Computed values:\n");
    DevMsg("  tanLeft=%.3f, tanRight=%.3f, tanDown=%.3f, tanUp=%.3f\n", tanLeft, tanRight, tanDown, tanUp);
    DevMsg("  xScale=%.3f, yScale=%.3f\n", xScale, yScale);
    DevMsg("  xOffset=%.3f, yOffset=%.3f\n", xOffset, yOffset);
    DevMsg("  zScale=%.3f, zOffset=%.3f\n", zScale, zOffset);

    // Log resulting matrix
    DevMsg("Projection Matrix:\n");
    DevMsg("  [%.3f %.3f %.3f %.3f]\n", mat[0][0], mat[0][1], mat[0][2], mat[0][3]);
    DevMsg("  [%.3f %.3f %.3f %.3f]\n", mat[1][0], mat[1][1], mat[1][2], mat[1][3]);
    DevMsg("  [%.3f %.3f %.3f %.3f]\n", mat[2][0], mat[2][1], mat[2][2], mat[2][3]);
    DevMsg("  [%.3f %.3f %.3f %.3f]\n", mat[3][0], mat[3][1], mat[3][2], mat[3][3]);
    */

    return mat;
}

void COpenXRManager::GetEyeProjectionMatrix(VMatrix& pResult, ISourceVirtualReality::VREye eye, float zNear, float zFar)
{
    XrFovf fov = m_views[eye].fov;
    // Use a smaller zNear value for better VR near plane
    pResult = CreateVRProjectionMatrix(fov, zNear, zFar);
}

bool COpenXRManager::CreateVulkanContext(XrGraphicsRequirementsVulkan2KHR* pRequirements) {
    if (!m_pfnCreateVulkanInstanceKHR || !m_pfnCreateVulkanDeviceKHR) 
    {
        DevMsg("Vulkan creation functions not available\n");
        return false;
    }
    
    // Let OpenXR create the Vulkan instance for us
    VkInstanceCreateInfo instanceCreateInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = pRequirements->minApiVersionSupported;
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pApplicationName = "TF2VR";
    appInfo.pEngineName = "Source Engine";
    instanceCreateInfo.pApplicationInfo = &appInfo;
    
    // Let OpenXR create the Vulkan instance with proper extensions
    XrVulkanInstanceCreateInfoKHR createInfo{ XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
    createInfo.systemId = m_systemId;
    createInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    createInfo.vulkanCreateInfo = &instanceCreateInfo;
    createInfo.vulkanAllocator = nullptr;
    
    VkResult vkResult;
    XrResult result = m_pfnCreateVulkanInstanceKHR(m_instance, &createInfo, &m_vkInstance, &vkResult);
    if (result != XR_SUCCESS || vkResult != VK_SUCCESS) 
    {
        DevMsg("Failed to create Vulkan instance. XrResult: %d, VkResult: %d\n", result, vkResult);
        return false;
    }
    
    // Select the physical device that the OpenXR runtime wants us to use
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_vkInstance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_vkInstance, &deviceCount, devices.data());
    
    // Let OpenXR select the physical device
    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
    deviceGetInfo.systemId = m_systemId;
    deviceGetInfo.vulkanInstance = m_vkInstance;
    
    result = m_pfnGetVulkanGraphicsDevice2KHR(m_instance, &deviceGetInfo, &m_vkPhysicalDevice);

    if (result != XR_SUCCESS) 
    {
        DevMsg("Failed to get Vulkan physical device from OpenXR: %d\n", result);
        return false;
    }
    
    // Find queue family that supports graphics
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_vkPhysicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_vkPhysicalDevice, &queueFamilyCount, queueFamilies.data());
    
    m_vkQueueFamilyIndex = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) 
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) 
        {
            m_vkQueueFamilyIndex = i;
            break;
        }
    }
    
    if (m_vkQueueFamilyIndex == UINT32_MAX) 
    {
        DevMsg("Failed to find graphics queue family!\n");
        return false;
    }
    
    // Let OpenXR create the Vulkan device for us
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = m_vkQueueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;
    
    VkPhysicalDeviceFeatures deviceFeatures{};
    
    VkDeviceCreateInfo deviceCreateInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueInfo;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
    
    XrVulkanDeviceCreateInfoKHR deviceCreateInfoXr{ XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
    deviceCreateInfoXr.systemId = m_systemId;
    deviceCreateInfoXr.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    deviceCreateInfoXr.vulkanPhysicalDevice = m_vkPhysicalDevice;
    deviceCreateInfoXr.vulkanCreateInfo = &deviceCreateInfo;
    deviceCreateInfoXr.vulkanAllocator = nullptr;
    
    result = m_pfnCreateVulkanDeviceKHR(m_instance, &deviceCreateInfoXr, &m_vkDevice, &vkResult);
    if (result != XR_SUCCESS || vkResult != VK_SUCCESS) 
    {
        DevMsg("Failed to create Vulkan device. XrResult: %d, VkResult: %d\n", result, vkResult);
        return false;
    }
    
    // Get device queue
    vkGetDeviceQueue(m_vkDevice, m_vkQueueFamilyIndex, 0, &m_vkQueue);
    
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(m_vkPhysicalDevice, &deviceProps);
    DevMsg("Vulkan context created successfully with device: %s (API version %u.%u.%u)\n",
           deviceProps.deviceName,
           VK_VERSION_MAJOR(deviceProps.apiVersion),
           VK_VERSION_MINOR(deviceProps.apiVersion),
           VK_VERSION_PATCH(deviceProps.apiVersion));
    
    return true;
}

void COpenXRManager::RecenterView()
{
    if (!m_vrActive || !m_session)
    {
        return;
    }
    
    // Get player view angles
    C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
    QAngle playerAngles = pPlayer ? pPlayer->EyeAngles() : QAngle(0, 0, 0);
    Vector playerPos = pPlayer ? pPlayer->EyePosition() : Vector(0, 0, 0);
    
    // Get current HMD orientation as a quaternion
    XrPosef eyePose = m_views[0].pose;  // Use center eye or first eye
    Quaternion currentHmdQuat
    (
        eyePose.orientation.z, 
        eyePose.orientation.x,
        eyePose.orientation.y,
        eyePose.orientation.w
    );
    
    // Convert player angles to quaternion - this is the target orientation
    Quaternion playerQuat;
    AngleQuaternion(playerAngles, playerQuat);
    
    // When recentering, we want to calculate the quaternion that would make the HMD's
    // current forward direction match the game camera's forward direction
    
    // Get the inverse of the current HMD orientation
    Quaternion inverseHmdQuat;
    QuaternionConjugate(currentHmdQuat, inverseHmdQuat);
    
    // Calculate the recenter quaternion
    QuaternionMult(playerQuat, inverseHmdQuat, m_recenterQuaternion);
    
    // Also store position offset - this should be the delta to make the current HMD position
    // appear centered relative to the game camera
    static ConVar* vr_position_scale = cvar->FindVar("vr_position_scale");
    float scale = vr_position_scale ? vr_position_scale->GetFloat() : 1.0f;
    
    // Use the same floor-aligned coordinate conversion as in GetMideyePose
    Vector currentPos
    (
        -eyePose.position.z * CalculateDynamicWorldScale(),
        -eyePose.position.x * CalculateDynamicWorldScale(),
        eyePose.position.y * CalculateDynamicWorldScale()
    );
    
    // The recenter position is designed to cancel out the current HMD position offset
    // Making it centered at the player's position when recentering
    m_recenterPosition = Vector(0, 0, 0) - currentPos;
    
    m_hasRecenterData = true;
    
    DevMsg("VR view recentered. Forward direction aligned with game camera.\n");
}

// Command to trigger recentering
CON_COMMAND(vr_recenter, "Recenter the VR headset orientation")
{
    if (g_pOpenXRManager)
    {
        g_pOpenXRManager->RecenterView();
    }
}

// Command to calibrate view height (simplified - just recenters)
CON_COMMAND(vr_calibrate_height, "Recenter VR view to match game camera")
{
    if (g_pOpenXRManager)
    {
        g_pOpenXRManager->RecenterView();
        DevMsg("VR view recentered. Use this command to align your view with the game camera.\n");
    }
}

// Command to calibrate player height
CON_COMMAND(vr_calibrate_player_height, "Calibrate your real height for VR scaling")
{
    if (args.ArgC() < 2)
    {
        ConVar* tfvr_player_height = cvar->FindVar("tfvr_player_height");
        float currentHeight = tfvr_player_height ? tfvr_player_height->GetFloat() : 67.0f;
        DevMsg("Current player height: %.1f inches\n", currentHeight);
        DevMsg("Usage: vr_calibrate_player_height <height_in_inches>\n");
        DevMsg("Example: vr_calibrate_player_height 72 (for 6 feet tall)\n");
        DevMsg("Or use: vr_calibrate_player_height_auto\n");
        return;
    }
    
    float height = atof(args.Arg(1));
    if (height < 20.0f || height > 100.0f) // Reasonable range: 4' to 7'
    {
        DevMsg("Height must be between 20 and 100 inches\n");
        return;
    }
    
    ConVar* tfvr_player_height = cvar->FindVar("tfvr_player_height");
    if (tfvr_player_height)
    {
        tfvr_player_height->SetValue(height);
        DevMsg("Player height set to %.1f inches\n", height);
        
        // Enable height calibration if not already enabled
        ConVar* tfvr_height_calibration = cvar->FindVar("tfvr_height_calibration");
        if (tfvr_height_calibration && !tfvr_height_calibration->GetBool())
        {
            tfvr_height_calibration->SetValue(1);
            DevMsg("Height calibration enabled\n");
        }
    }
}

// Command to auto-calibrate player height from current HMD position
CON_COMMAND(vr_calibrate_player_height_auto, "Auto-calibrate your height from current HMD position (stand tall!)")
{
    if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
    {
        DevMsg("VR not active - cannot calibrate height\n");
        return;
    }
    
    // Get raw unscaled HMD position in play space (meters)
    Vector rawHmdPos = g_pOpenXRManager->GetRawHMDPosition();
    
    // Convert HMD height from meters to inches
    // rawHmdPos.y is height above play space floor in meters
    float hmdHeightMeters = rawHmdPos.y;
    float hmdHeightInches = hmdHeightMeters * 39.3701f; // meters to inches
    
    // Add estimate for head-to-top-of-head (about 4 inches)
    float estimatedPlayerHeight = hmdHeightInches + 4.0f;
    
    if (estimatedPlayerHeight < 24.0f || estimatedPlayerHeight > 100.0f)
    {
        DevMsg("Detected height %.1f inches seems out of range (48-84). Check your play space setup.\n", estimatedPlayerHeight);
        return;
    }
    
    ConVar* tfvr_player_height = cvar->FindVar("tfvr_player_height");
    if (tfvr_player_height)
    {
        tfvr_player_height->SetValue(estimatedPlayerHeight);
        DevMsg("Auto-calibrated player height to %.1f inches (%.1f' %.0f\")\n", 
               estimatedPlayerHeight, 
               floor(estimatedPlayerHeight / 12.0f), 
               fmod(estimatedPlayerHeight, 12.0f));
        
        // Enable height calibration
        ConVar* tfvr_height_calibration = cvar->FindVar("tfvr_height_calibration");
        if (tfvr_height_calibration)
        {
            tfvr_height_calibration->SetValue(1);
            DevMsg("Height calibration enabled\n");
        }
    }
}

// Command to toggle seated mode
CON_COMMAND(vr_seated_mode, "Toggle VR seated mode")
{
    ConVar* tfvr_seated_mode = cvar->FindVar("tfvr_seated_mode");
    if (!tfvr_seated_mode)
        return;
        
    bool currentValue = tfvr_seated_mode->GetBool();
    tfvr_seated_mode->SetValue(!currentValue);
    
    DevMsg("VR Seated Mode: %s\n", !currentValue ? "ENABLED" : "DISABLED");
    if (!currentValue)
    {
        ConVar* tfvr_seated_height_offset = cvar->FindVar("tfvr_seated_height_offset");
        float offset = tfvr_seated_height_offset ? tfvr_seated_height_offset->GetFloat() : 24.0f;
        DevMsg("  Using height offset: %.1f inches\n", offset);
    }
}

// Command to set seated mode height offset
CON_COMMAND(vr_seated_height_offset, "Set the height offset for seated mode")
{
    if (args.ArgC() < 2)
    {
        ConVar* tfvr_seated_height_offset = cvar->FindVar("tfvr_seated_height_offset");
        float currentOffset = tfvr_seated_height_offset ? tfvr_seated_height_offset->GetFloat() : 24.0f;
        DevMsg("Current seated height offset: %.1f inches\n", currentOffset);
        DevMsg("Usage: vr_seated_height_offset <offset_in_inches>\n");
        DevMsg("Example: vr_seated_height_offset 24 (for 2 feet offset)\n");
        DevMsg("Or use: vr_calibrate_seated_height_auto\n");
        return;
    }
    
    float offset = atof(args.Arg(1));
    if (offset < 1.0f || offset > 60.0f) // Reasonable range: 1' to 4'
    {
        DevMsg("Height offset must be between 1 and 60 inches\n");
        return;
    }
    
    ConVar* tfvr_seated_height_offset = cvar->FindVar("tfvr_seated_height_offset");
    if (tfvr_seated_height_offset)
    {
        tfvr_seated_height_offset->SetValue(offset);
        DevMsg("Seated height offset set to %.1f inches\n", offset);
    }
}

// Command to auto-calibrate seated height offset from current HMD position
CON_COMMAND(vr_calibrate_seated_height_auto, "Auto-calibrate seated height offset from current HMD position (sit normally!)")
{
    if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
    {
        DevMsg("VR not active - cannot calibrate seated height\n");
        return;
    }
    
    // Get raw unscaled HMD position in play space (meters)
    Vector rawHmdPos = g_pOpenXRManager->GetRawHMDPosition();
    
    // Convert HMD height from meters to inches
    float hmdHeightMeters = rawHmdPos.y;
    float hmdHeightInches = hmdHeightMeters * 39.3701f; // meters to inches
    
    // Calculate offset needed to reach a comfortable standing eye height
    // Assume comfortable standing eye height is about 60-65 inches
    // (this is rough average standing eye height for most people)
    float targetStandingEyeHeight = 64.0f; // inches
    float neededOffset = targetStandingEyeHeight - hmdHeightInches;
    
    if (neededOffset < 1.0f || neededOffset > 80.0f)
    {
        DevMsg("Calculated offset %.1f inches is out of range (1-80). Current HMD height: %.1f inches\n", 
               neededOffset, hmdHeightInches);
        DevMsg("You may need to adjust your seating position or use manual calibration.\n");
        return;
    }
    
    ConVar* tfvr_seated_height_offset = cvar->FindVar("tfvr_seated_height_offset");
    if (tfvr_seated_height_offset)
    {
        tfvr_seated_height_offset->SetValue(neededOffset);
        DevMsg("Auto-calibrated seated height offset to %.1f inches\n", neededOffset);
        DevMsg("Current seated HMD height: %.1f inches\n", hmdHeightInches);
        DevMsg("Will offset to standing height: %.1f inches\n", hmdHeightInches + neededOffset);
        
        // Enable seated mode if not already enabled
        ConVar* tfvr_seated_mode = cvar->FindVar("tfvr_seated_mode");
        if (tfvr_seated_mode && !tfvr_seated_mode->GetBool())
        {
            tfvr_seated_mode->SetValue(1);
            DevMsg("Seated mode enabled\n");
        }
    }
}

// Command to debug HMD position (raw vs scaled)
CON_COMMAND(vr_debug_hmd_position, "Show raw vs scaled HMD position for debugging")
{
    if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
    {
        DevMsg("VR not active\n");
        return;
    }
    
    Vector rawPos = g_pOpenXRManager->GetRawHMDPosition();
    VMatrix scaledPose = g_pOpenXRManager->GetMideyePose();
    Vector scaledPos = scaledPose.GetTranslation();
    
    DevMsg("HMD Position Debug:\n");
    DevMsg("  Raw OpenXR position (meters): x=%.3f, y=%.3f, z=%.3f\n", rawPos.x, rawPos.y, rawPos.z);
    DevMsg("  Raw height in inches: %.1f\n", rawPos.y * 39.3701f);
    DevMsg("  Scaled position (game units): x=%.1f, y=%.1f, z=%.1f\n", scaledPos.x, scaledPos.y, scaledPos.z);
    DevMsg("  Current world scale: %.1f\n", CalculateDynamicWorldScale());
}

// Command to test dynamic world scaling
CON_COMMAND(vr_test_scaling, "Test dynamic world scaling based on merc height")
{
    DevMsg("VR Dynamic Scaling Test:\n");
    DevMsg("  tfvr_dynamic_worldscale: %s\n", 
           cvar->FindVar("tfvr_dynamic_worldscale")->GetBool() ? "Enabled" : "Disabled");
    DevMsg("  tfvr_worldscale: %.1f\n", 
           cvar->FindVar("tfvr_worldscale")->GetFloat());
    
    ConVar* tfvr_height_calibration = cvar->FindVar("tfvr_height_calibration");
    ConVar* tfvr_player_height = cvar->FindVar("tfvr_player_height");
    ConVar* tfvr_seated_mode = cvar->FindVar("tfvr_seated_mode");
    
    DevMsg("  Height Calibration: %s\n", 
           tfvr_height_calibration && tfvr_height_calibration->GetBool() ? "Enabled" : "Disabled");
    DevMsg("  Player Height: %.1f inches\n", 
           tfvr_player_height ? tfvr_player_height->GetFloat() : 67.0f);
    DevMsg("  Seated Mode: %s\n", 
           tfvr_seated_mode && tfvr_seated_mode->GetBool() ? "Enabled" : "Disabled");
    
    C_TFPlayer* pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (pLocalPlayer)
    {
        DevMsg("  Player Class: %s\n", 
               pLocalPlayer->GetPlayerClass() ? pLocalPlayer->GetPlayerClass()->GetName() : "Unknown");
        DevMsg("  Is Crouching: %s\n", (pLocalPlayer->GetFlags() & FL_DUCKING) ? "Yes" : "No");
        DevMsg("  Current Eye Height: %.1f\n", pLocalPlayer->EyePosition().z - pLocalPlayer->GetAbsOrigin().z);
    }
    else
    {
        DevMsg("  No local player found\n");
    }
}

VMatrix COpenXRManager::GetEyeViewFromMidEyeView(ISourceVirtualReality::VREye eye)
{
    // Calculate full 3D IPD between eyes
    XrVector3f leftPos = m_views[0].pose.position;
    XrVector3f rightPos = m_views[1].pose.position;
    float dx = rightPos.x - leftPos.x;
    float dy = rightPos.y - leftPos.y;
    float dz = rightPos.z - leftPos.z;
    float ipdMeters = sqrt(dx * dx + dy * dy + dz * dz); // Full distance, e.g., 0.064m
    float ipdUnits = ipdMeters * CalculateDynamicWorldScale();   // e.g., 2.51968 units
    float halfIpd = ipdUnits / 2.0f;                     // e.g., 1.25984 units

    // Construct result matrix with offset only in Source Y
    VMatrix result;
    result.Identity();
    result.m[1][3] = (eye == 0) ? halfIpd : -halfIpd; // Left: +Y, Right: -Y

    return result;
}

VMatrix COpenXRManager::GetMideyePose() const
{
	if (!IsActive())
	    return VMatrix();

    return ToSourceCoordinateSystemFloorAligned(m_headLocation.pose);
}

Vector COpenXRManager::GetRawHMDPosition() const
{
	if (!IsActive())
	    return Vector(0, 0, 0);

    // Return the raw OpenXR head position without any world scaling applied
    // This is needed for calibration purposes
    return Vector(m_headLocation.pose.position.x, m_headLocation.pose.position.y, m_headLocation.pose.position.z);
}

void COpenXRManager::GetHMDInChaperone(class Vector& origin, QAngle& angles) const
{
    origin = GetMideyePose().GetTranslation();
    MatrixToAngles(GetMideyePose(), angles);
}

void COpenXRManager::GetViewportBounds(ISourceVirtualReality::VREye eye, int* pnX, int* pnY, int* pnWidth, int* pnHeight)
{
    if (pnX != NULL)
    {
		*pnX = m_viewConfigs[0].recommendedImageRectWidth * eye;
        *pnY = 0;
    }
    *pnWidth = m_viewConfigs[0].recommendedImageRectWidth;
    *pnHeight = m_viewConfigs[0].recommendedImageRectHeight;
}

Vector2D COpenXRManager::GetBufferSize()
{
    return Vector2D(m_viewConfigs[0].recommendedImageRectWidth, m_viewConfigs[0].recommendedImageRectHeight);
}

bool g_firstUpdateVR = true;
bool g_firstUpdateNonVR = false;
void COpenXRManager::Update(float frametime)
{
	VPROF("VRManager::Update", VPROF_BUDGETGROUP_WORLD_RENDERING);

	if (!g_pOpenXRManager->IsActive())
	{
		if (g_firstUpdateNonVR)
		{
			g_firstUpdateNonVR = false;
			g_firstUpdateVR = true;
		}
		return;
	}

	if (g_firstUpdateVR)
	{
		g_firstUpdateVR = false;
		g_firstUpdateNonVR = true;

		// Swap input system
		if (::input != nullptr)
		{
			g_OriginalNonVRInputPtr = ::input;
		}
		::input = (IInput*)&g_VRInput;
		::input->Init_All();

		for (m_currentRenderBufferIndex = 0; m_currentRenderBufferIndex < VR_NUM_BUFFERS; ++m_currentRenderBufferIndex)
		{
			// pre-create render target materials
			GetRenderTargetMat();
		}

		m_currentRenderBufferIndex = 0;
	}

	m_currentRenderBufferIndex = (++m_currentRenderBufferIndex) % VR_NUM_BUFFERS;

	// Update VR Menu Manager
	if (m_menuManager)
	{
		m_menuManager->Update();
	}

	// Poll OpenXR events
	XrEventDataBuffer eventBuffer = {XR_TYPE_EVENT_DATA_BUFFER};
	while (m_sessionRunning && xrPollEvent(m_instance, &eventBuffer) == XR_SUCCESS)
	{
		switch (eventBuffer.type)
		{
		// Handle session state changes
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			XrEventDataSessionStateChanged *sessionStateEvent = reinterpret_cast<XrEventDataSessionStateChanged *>(&eventBuffer);

			switch (sessionStateEvent->state)
			{
			// Session is stopping (e.g., user quit via XR runtime)
			case XR_SESSION_STATE_STOPPING:
				Log("Shutdown requested by OpenXR runtime\n");
				xrEndSession(m_session); // Gracefully end the session
				m_sessionRunning = false;
				if (engine)
				{
					// Assuming engine has a similar quit command
					engine->ClientCmd_Unrestricted("quit");
				}
				break;

			// Session is losing focus (e.g., dashboard activated)
			case XR_SESSION_STATE_LOSS_PENDING:
				Log("OpenXR dashboard likely activated\n");
				if (engine && !enginevgui->IsGameUIVisible()) // Assume IsGameUIVisible exists
				{
					engine->ClientCmd_Unrestricted("gameui_toggle\n");
				}
				break;

			default:
				break;
			}
			break;
		}

		// Handle instance loss (e.g., runtime shutting down)
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
		{
			XrEventDataInstanceLossPending *lossEvent = reinterpret_cast<XrEventDataInstanceLossPending *>(&eventBuffer);
			Log("OpenXR instance loss pending, shutting down\n");
			m_sessionRunning = false;
			if (engine)
			{
				engine->ClientCmd_Unrestricted("quit");
			}
			break;
		}

		default:
			break;
		}

		// Clear the event buffer for the next iteration
		eventBuffer = {XR_TYPE_EVENT_DATA_BUFFER};
	}

	UpdateOpenXRViewData();

	vrRenderTargets->UpdateVRRenderTargets();

	// unfortunately, r_lod is not archived, so to make it an option, we have to map from our own convar here
	static ConVarRef r_lod("r_lod");
	if (tfvr_forcemaxlod.GetBool() && r_lod.GetInt() != 0)
	{
		r_lod.SetValue(0);
	}
	else if (!tfvr_forcemaxlod.GetBool() && r_lod.GetInt() == 0)
	{
		r_lod.SetValue(-1);
	}

    if (WasButtonPressed("left_trigger")) 
    {
		DevMsg("Left Trigger pressed!\n");
    }

    if (WasButtonPressed("right_trigger"))
	{
		DevMsg("Right Trigger pressed!\n");
	}
}

char* backBufferNamePerIndex(int i)
{
	i = i % VR_NUM_BUFFERS;
	static char name[sizeof(VR_BACK_BUFFER_X) + 2] = {0}; // The most we could possibly need is 100, right?
	if (name[0] == 0)
	{
		sprintf(name, VR_BACK_BUFFER_X);
	}
	sprintf(&name[sizeof(VR_BACK_BUFFER_X) - 1], "%i", i);
	return name;
}

void COpenXRManager::PollInput()
{
    if (m_inputManager)
    {
        m_inputManager->PollInput();
    }
}

bool COpenXRManager::IsButtonPressed(const char* actionName)
{
    return m_inputManager ? m_inputManager->IsButtonPressed(actionName) : false;
}

bool COpenXRManager::WasButtonPressed(const char* actionName)
{
    return m_inputManager ? m_inputManager->WasButtonPressed(actionName) : false;
}

bool COpenXRManager::WasButtonReleased(const char* actionName)
{
    return m_inputManager ? m_inputManager->WasButtonReleased(actionName) : false;
}

float COpenXRManager::GetAnalogValue(const char* actionName)
{
    return m_inputManager ? m_inputManager->GetAnalogValue(actionName) : 0.0f;
}

bool COpenXRManager::GetLeftControllerPose(VMatrix& pose)
{
    if (!m_inputManager) return false;
    
    XrPosef xrPose;
    if (m_inputManager->GetControllerPose("left_hand_pose", xrPose))
    {
        // Get the head pose from OpenXR (center eye)
        XrPosef headPose = m_views[0].pose;
        
        // Create head transform matrix and get its inverse
        VMatrix headMatrix = ToSourceCoordinateSystem(headPose);
        VMatrix headInverse = headMatrix.InverseTR();
        
        // Convert controller pose to Source coordinate system
        VMatrix controllerMatrix = ToSourceCoordinateSystem(xrPose);
        
        // Transform controller to head-relative space, then through player's world transform
        VMatrix headRelativeController = headInverse * controllerMatrix;
        
        // Get player's world transform
        C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
        if (pPlayer)
        {
            // Create player transform matrix (rotation + position)
            VMatrix playerMatrix;
            playerMatrix.Identity();
            
            matrix3x4_t playerMatrix3x4;
            AngleMatrix(pPlayer->EyeAngles(), playerMatrix3x4);
            playerMatrix.CopyFrom3x4(playerMatrix3x4);
            playerMatrix.SetTranslation(pPlayer->EyePosition());
            
            // Transform controller through player's world transform
            pose = playerMatrix * headRelativeController;
            
            // Debug visualization
            if (debugoverlay && tfvr_controller_debug_draw.GetBool())
            {
                Vector worldPos = pose.GetTranslation();
                
                // Draw controller position box
                Vector boxSize(2.0f, 2.0f, 2.0f);
                debugoverlay->AddBoxOverlay(worldPos, -boxSize, boxSize, QAngle(0, 0, 0), 255, 0, 0, 255, 0.016f);
                
                // Draw orientation axes
                Vector forward, right, up;
                pose.GetBasisVectors(forward, right, up);
                debugoverlay->AddLineOverlayAlpha(worldPos, worldPos + forward * 20.0f, 255, 0, 0, 255, false, 0.016f);
                debugoverlay->AddLineOverlayAlpha(worldPos, worldPos + right * 20.0f, 0, 255, 0, 255, false, 0.016f);
                debugoverlay->AddLineOverlayAlpha(worldPos, worldPos + up * 20.0f, 0, 0, 255, 255, false, 0.016f);
            }
        }
        
        return true;
    }
    return false;
}

bool COpenXRManager::GetRightControllerPose(VMatrix& pose)
{
    if (!m_inputManager) return false;
    
    XrPosef xrPose;
    if (m_inputManager->GetControllerPose("right_hand_pose", xrPose))
    {
        // Get the head pose from OpenXR (center eye)
        XrPosef headPose = m_views[0].pose;
        
        // Create head transform matrix and get its inverse
        VMatrix headMatrix = ToSourceCoordinateSystem(headPose);
        VMatrix headInverse = headMatrix.InverseTR();
        
        // Convert controller pose to Source coordinate system
        VMatrix controllerMatrix = ToSourceCoordinateSystem(xrPose);
        
        // Transform controller to head-relative space, then through player's world transform
        VMatrix headRelativeController = headInverse * controllerMatrix;
        
        // Get player's world transform
        C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
        if (pPlayer)
        {
            // Create player transform matrix (rotation + position)
            VMatrix playerMatrix;
            playerMatrix.Identity();
            
            matrix3x4_t playerMatrix3x4;
            AngleMatrix(pPlayer->EyeAngles(), playerMatrix3x4);
            playerMatrix.CopyFrom3x4(playerMatrix3x4);
            playerMatrix.SetTranslation(pPlayer->EyePosition());
            
            // Transform controller through player's world transform
            pose = playerMatrix * headRelativeController;
            
            // Debug visualization
            if (debugoverlay && tfvr_controller_debug_draw.GetBool())
            {
                Vector worldPos = pose.GetTranslation();
                
                // Draw controller position box
                Vector boxSize(2.0f, 2.0f, 2.0f);
                debugoverlay->AddBoxOverlay(worldPos, -boxSize, boxSize, QAngle(0, 0, 0), 0, 255, 0, 255, 0.016f);
                
                // Draw orientation axes
                Vector forward, right, up;
                pose.GetBasisVectors(forward, right, up);
                debugoverlay->AddLineOverlayAlpha(worldPos, worldPos + forward * 20.0f, 255, 0, 0, 255, false, 0.016f);
                debugoverlay->AddLineOverlayAlpha(worldPos, worldPos + right * 20.0f, 0, 255, 0, 255, false, 0.016f);
                debugoverlay->AddLineOverlayAlpha(worldPos, worldPos + up * 20.0f, 0, 0, 255, 255, false, 0.016f);
            }
        }
        
        return true;
    }
    return false;
}

bool COpenXRManager::IsLeftControllerPoseValid()
{
    return m_inputManager ? m_inputManager->IsControllerPoseValid("left_hand_pose") : false;
}

bool COpenXRManager::IsRightControllerPoseValid()
{
    return m_inputManager ? m_inputManager->IsControllerPoseValid("right_hand_pose") : false;
}

COpenXRManager g_TFVR;
COpenXRManager* g_pOpenXRManager = &g_TFVR;