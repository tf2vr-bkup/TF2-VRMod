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

#include "mathlib/mathlib.h"

// Forward declaration for global menu manager pointer
extern class CVRMenuManager* g_pVRMenuManager;

ConVar tfvr_worldscale("tfvr_worldscale", "40", FCVAR_ARCHIVE | FCVAR_REPLICATED, "This scales everything.");
#define METERS_TO_GAME_UNITS tfvr_worldscale.GetFloat()

ConVar tfvr_msaa("tfvr_msaa", "4", FCVAR_ARCHIVE, "Controls multi-sampling anti-aliasing levels in TFVR. Set to the number of samples to use.");
ConVar tfvr_forcemaxlod("tfvr_forcemaxlod", "1", FCVAR_ARCHIVE);
ConVar tfvr_hud_forward("tfvr_hud_forward", "15", FCVAR_ARCHIVE, "Apparent distance of the HUD in inches");
ConVar tfvr_hud_scale("tfvr_hud_scale", "0.5", FCVAR_ARCHIVE);
ConVar tfvr_hud_axis_lock_to_world("tfvr_hud_axis_lock_to_world", "5", FCVAR_ARCHIVE, "Bitfield - locks HUD axes to the world - 1=pitch, 2=yaw, 4=roll");
ConVar tfvr_hud_height_adjust("tfvr_hud_height_adjust", "-4", FCVAR_ARCHIVE);

ConVar tfvr_menu_forward("tfvr_menu_forward", "150", FCVAR_ARCHIVE);
ConVar tfvr_menu_scale("tfvr_menu_scale", "0.7", FCVAR_ARCHIVE);

ConVar tfvr_r_show_both_eyes("tfvr_r_show_both_eyes", "0", FCVAR_ARCHIVE, "Show both eyes on the game window.");

// Common conversions
namespace
{
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
		convertedPos *= METERS_TO_GAME_UNITS;

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
    
    // Set the global pointer for external access
    g_pVRMenuManager = m_menuManager;

    m_vrActive = true;
    DevMsg("OpenXR VR mode initialized successfully!\n");
    return true;
}

void COpenXRManager::Shutdown() 
{
    if (!m_vrActive) return;

    // Clean up input system
    if (m_inputManager)
    {
        m_inputManager->Shutdown();
        delete m_inputManager;
        m_inputManager = nullptr;
    }

    // Clean up menu manager
    if (m_menuManager)
    {
        m_menuManager->Shutdown();
        delete m_menuManager;
        m_menuManager = nullptr;
        g_pVRMenuManager = nullptr; // Clear global pointer
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
    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace = { {0, 0, 0, 1}, {0, 0, 0} };
    XrResult result = xrCreateReferenceSpace(m_session, &spaceInfo, &m_referenceSpace);

    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create reference space: %d\n", result);
        return false;
    }
    DevMsg("Reference space created successfully!\n");
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

    uint32_t viewCount;
    dxvkGetViews(m_views, m_headLocation, viewCount);
}

bool COpenXRManager::GetEyeViewLocations(VMatrix& leftEyePose, VMatrix& rightEyePose)
{
    if (!m_vrActive || !m_session)
    {
        return false;
    }

    leftEyePose = ToSourceCoordinateSystem(m_views[0].pose);
    rightEyePose = ToSourceCoordinateSystem(m_views[1].pose);
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

    static ConVar* vr_floor_offset = cvar->FindVar("vr_floor_offset");
    
    // Make sure we're using the same coordinate conversion as in GetEyeViewData
    Vector currentPos
    (
        -eyePose.position.z,
        -eyePose.position.x,
        (eyePose.position.y)
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

// Command to calibrate view height
CON_COMMAND(vr_calibrate_height, "Calibrate VR view height to match eye level")
{
    if (g_pOpenXRManager)
    {
        // First recenter the view
        g_pOpenXRManager->RecenterView();
        
        // Set a reasonable floor offset to match player eye height
        ConVar* vr_floor_offset = cvar->FindVar("vr_floor_offset");
        if (vr_floor_offset)
        {
            float currentValue = vr_floor_offset->GetFloat();
            // Adjust by a small increment - players can call this multiple times as needed
            vr_floor_offset->SetValue(currentValue + 0.05f);
            DevMsg("Adjusted floor offset to %f. If still not at eye level, run command again.\n", vr_floor_offset->GetFloat());
        }
    }
}

// Command for quick view adjustment when looking up causes floor issues
CON_COMMAND(vr_fix_floor_view, "Adjust VR eye height to prevent floor clipping when looking up")
{
    ConVar* vr_eye_height_adjust = cvar->FindVar("vr_eye_height_adjust");
    if (vr_eye_height_adjust)
    {
        float currentValue = vr_eye_height_adjust->GetFloat();
        // Increment by 0.05 each time
        vr_eye_height_adjust->SetValue(currentValue + 0.05f);
        DevMsg("Set eye height adjustment to %f. If looking up still causes floor clipping, run command again.\n", 
            vr_eye_height_adjust->GetFloat());
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
    float ipdUnits = ipdMeters * METERS_TO_GAME_UNITS;   // e.g., 2.51968 units
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

    return ToSourceCoordinateSystem(m_headLocation.pose);
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

	// Poll input state
	if (m_inputManager)
	{
		m_inputManager->PollInput();
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

COpenXRManager g_TFVR;
COpenXRManager* g_pOpenXRManager = &g_TFVR;