#include "cbase.h"
#include "openxr_input.h"
#include "openxr_manager.h"
#include "hmdWrapper.h"

COpenXRInputManager::COpenXRInputManager(COpenXRManager* manager)
    : m_manager(manager)
    , m_instance(manager->GetInstance())
    , m_session(manager->GetSession())
    , m_actionSet(XR_NULL_HANDLE)
{
}

COpenXRInputManager::~COpenXRInputManager()
{
    Shutdown();
}

bool COpenXRInputManager::Initialize()
{
    if (!CreateActionSet()) return false;
    if (!CreateActions()) return false;
    if (!CreateInteractionProfiles()) return false;
    if (!AttachActionSet()) return false;
    return true;
}

void COpenXRInputManager::Shutdown()
{
    for (auto& action : m_actions)
    {
        if (action.second.handle != XR_NULL_HANDLE)
        {
            xrDestroyAction(action.second.handle);
        }
    }
    m_actions.clear();

    // Clean up action spaces
    for (auto& actionSpace : m_actionSpaces)
    {
        if (actionSpace.second != XR_NULL_HANDLE)
        {
            xrDestroySpace(actionSpace.second);
        }
    }
    m_actionSpaces.clear();

    if (m_actionSet != XR_NULL_HANDLE)
    {
        xrDestroyActionSet(m_actionSet);
        m_actionSet = XR_NULL_HANDLE;
    }
}

bool COpenXRInputManager::CreateActionSet()
{
    XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(actionSetInfo.actionSetName, "gameplay");
    strcpy_s(actionSetInfo.localizedActionSetName, "Gameplay");
    
    XrResult result = xrCreateActionSet(m_instance, &actionSetInfo, &m_actionSet);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create action set: %d\n", result);
        return false;
    }

    result = xrStringToPath(m_instance, "/user/hand/left", &m_leftHandPath);
    if (!XR_SUCCEEDED(result)) return false;

    result = xrStringToPath(m_instance, "/user/hand/right", &m_rightHandPath);
    if (!XR_SUCCEEDED(result)) return false;

    return true;
}

bool COpenXRInputManager::CreateActions()
{
    // Create movement actions
    XrInputAction moveX = CreateFloatAction("move_x", "Move X");
    if (moveX.handle == XR_NULL_HANDLE) return false;
    m_actions["move_x"] = moveX;

    XrInputAction moveY = CreateFloatAction("move_y", "Move Y");
    if (moveY.handle == XR_NULL_HANDLE) return false;
    m_actions["move_y"] = moveY;

    // Create turning actions for smooth/snap turning
    XrInputAction turnX = CreateFloatAction("turn_x", "Turn X");
    if (turnX.handle == XR_NULL_HANDLE) return false;
    m_actions["turn_x"] = turnX;

    // Create controller pose actions for tracking
    XrInputAction leftHandPose = CreatePoseAction("left_hand_pose", "Left Hand Pose");
    if (leftHandPose.handle == XR_NULL_HANDLE) return false;
    m_actions["left_hand_pose"] = leftHandPose;

    XrInputAction rightHandPose = CreatePoseAction("right_hand_pose", "Right Hand Pose");
    if (rightHandPose.handle == XR_NULL_HANDLE) return false;
    m_actions["right_hand_pose"] = rightHandPose;

    // Create grip pose actions for tracking
    XrInputAction leftHandGripPose = CreatePoseAction("left_hand_grip_pose", "Left Hand Grip Pose");
    if (leftHandGripPose.handle == XR_NULL_HANDLE) return false;
    m_actions["left_hand_grip_pose"] = leftHandGripPose;

    XrInputAction rightHandGripPose = CreatePoseAction("right_hand_grip_pose", "Right Hand Grip Pose");
    if (rightHandGripPose.handle == XR_NULL_HANDLE) return false;
    m_actions["right_hand_grip_pose"] = rightHandGripPose;

    // Create button actions
    XrInputAction primaryAttack = CreateFloatAction("primary_attack", "Primary Attack");
    if (primaryAttack.handle == XR_NULL_HANDLE) return false;
    m_actions["primary_attack"] = primaryAttack;

    XrInputAction secondaryAttack = CreateFloatAction("secondary_attack", "Secondary Attack");
    if (secondaryAttack.handle == XR_NULL_HANDLE) return false;
    m_actions["secondary_attack"] = secondaryAttack;

    XrInputAction use = CreateBooleanAction("use", "Use");
    if (use.handle == XR_NULL_HANDLE) return false;
    m_actions["use"] = use;

    XrInputAction duck = CreateBooleanAction("duck", "Duck");
    if (duck.handle == XR_NULL_HANDLE) return false;
    m_actions["duck"] = duck;

    XrInputAction jump = CreateBooleanAction("jump", "Jump");
    if (jump.handle == XR_NULL_HANDLE) return false;
    m_actions["jump"] = jump;

    XrInputAction menu = CreateBooleanAction("menu", "Menu");
    if (menu.handle == XR_NULL_HANDLE) return false;
    m_actions["menu"] = menu;

    // Add separate UI interaction actions for left and right hands
    XrInputAction leftUIInteract = CreateBooleanAction("left_ui_interact", "Left UI Interact");
    if (leftUIInteract.handle == XR_NULL_HANDLE) return false;
    m_actions["left_ui_interact"] = leftUIInteract;

    XrInputAction rightUIInteract = CreateBooleanAction("right_ui_interact", "Right UI Interact");
    if (rightUIInteract.handle == XR_NULL_HANDLE) return false;
    m_actions["right_ui_interact"] = rightUIInteract;

    // Add class menu action for left A button
    XrInputAction leftClassMenu = CreateBooleanAction("left_class_menu", "Left Class Menu");
    if (leftClassMenu.handle == XR_NULL_HANDLE) return false;
    m_actions["left_class_menu"] = leftClassMenu;

    // Add weapon switching actions
    XrInputAction weaponSwitch = CreateFloatAction("right_weapon_switch", "Right Weapon Switch");
    if (weaponSwitch.handle == XR_NULL_HANDLE) return false;
    m_actions["weapon_switch"] = weaponSwitch;
    DevMsg("Created weapon switching action: weapon_switch (handle: %p)\n", weaponSwitch.handle);

    return true;
}

bool COpenXRInputManager::CreateInteractionProfiles()
{
    bool success = false;
    
    // Try to create Valve Index profile
    success |= CreateIndexControllerProfile();
    
    // Try to create Quest controller profile
    success |= CreateQuestControllerProfile();
    
    // Fall back to generic profile if both specific profiles fail
    if (!success)
    {
        success |= CreateGenericControllerProfile();
    }
    
    return success;
}

bool COpenXRInputManager::CreateIndexControllerProfile()
{
    XrPath indexProfilePath;
    
    XrResult result = xrStringToPath(m_instance, "/interaction_profiles/valve/index_controller", &indexProfilePath);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create path for Valve Index profile: %d\n", result);
        return false;
    }

    // Create suggested bindings for Index controller
    std::vector<XrActionSuggestedBinding> suggestedBindings;
    
    // Movement bindings (left controller)
    if (m_actions.find("move_x") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/x", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["move_x"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }
    
    if (m_actions.find("move_y") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/y", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["move_y"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Turning bindings (right controller)
    if (m_actions.find("turn_x") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/x", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["turn_x"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Primary attack (right trigger)
    if (m_actions.find("primary_attack") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trigger/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["primary_attack"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Secondary attack (left trigger)
    if (m_actions.find("secondary_attack") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/trigger/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["secondary_attack"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Use (right A button)
    if (m_actions.find("use") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/a/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["use"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Duck (left A button)
    if (m_actions.find("duck") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/a/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["duck"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Jump (right B button)
    if (m_actions.find("jump") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/b/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["jump"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Menu (left B button)
    if (m_actions.find("menu") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/b/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["menu"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Left UI interaction binding (left trigger)
    if (m_actions.find("left_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/trigger/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_ui_interact"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Left class menu binding (left A button)
    if (m_actions.find("left_class_menu") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/a/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_class_menu"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Right UI interaction binding (right trigger)
    if (m_actions.find("right_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trigger/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_ui_interact"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Weapon switching bindings (right stick tilt forward/backward)
    if (m_actions.find("weapon_switch") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/y", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["weapon_switch"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added weapon switching binding: weapon_switch -> /user/hand/right/input/thumbstick/y\n");
        }
    }

    // Pose action bindings for controller tracking
    if (m_actions.find("left_hand_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/aim/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_hand_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added left hand pose binding: /user/hand/left/input/aim/pose\n");
        }
    }
    
    if (m_actions.find("right_hand_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/aim/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_hand_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added right hand pose binding: /user/hand/right/input/aim/pose\n");
        }
    }

    // Grip pose action bindings for controller tracking
    if (m_actions.find("left_hand_grip_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/grip/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_hand_grip_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added left hand grip pose binding: /user/hand/left/input/grip/pose\n");
        }
    }
    
    if (m_actions.find("right_hand_grip_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/grip/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_hand_grip_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added right hand grip pose binding: /user/hand/right/input/grip/pose\n");
        }
    }

    // Suggest bindings for Index controller
    return SuggestBindings(indexProfilePath, suggestedBindings, "Valve Index");
}

bool COpenXRInputManager::CreateQuestControllerProfile()
{
    XrPath questProfilePath;
    
    XrResult result = xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &questProfilePath);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create path for Quest controller profile: %d\n", result);
        return false;
    }

    // Create suggested bindings for Quest controller
    std::vector<XrActionSuggestedBinding> suggestedBindings;
    
    // Movement bindings (left controller thumbstick)
    if (m_actions.find("move_x") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/x", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["move_x"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }
    
    if (m_actions.find("move_y") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/y", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["move_y"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Turning bindings (right controller thumbstick)
    if (m_actions.find("turn_x") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/x", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["turn_x"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Primary attack (right trigger)
    if (m_actions.find("primary_attack") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trigger/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["primary_attack"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Secondary attack (left trigger)
    if (m_actions.find("secondary_attack") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/trigger/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["secondary_attack"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Use (right A button)
    if (m_actions.find("use") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/a/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["use"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Duck (right A button - same as use, Quest controllers have fewer buttons)
    if (m_actions.find("duck") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/a/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["duck"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Jump (right B button)
    if (m_actions.find("jump") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/b/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["jump"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Menu (left menu button - Quest controllers have a dedicated menu button)
    if (m_actions.find("menu") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/menu/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["menu"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Left UI interaction binding (left trigger)
    if (m_actions.find("left_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/trigger/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_ui_interact"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Left class menu binding (left X button)
    if (m_actions.find("left_class_menu") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/x/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_class_menu"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Right UI interaction binding (right trigger)
    if (m_actions.find("right_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trigger/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_ui_interact"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Weapon switching bindings (right stick tilt forward/backward)
    if (m_actions.find("weapon_switch") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/y", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["weapon_switch"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added Quest weapon switching binding: weapon_switch -> /user/hand/right/input/thumbstick/y\n");
        }
    }

    // Pose action bindings for controller tracking
    if (m_actions.find("left_hand_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/aim/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_hand_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added Quest left hand pose binding: /user/hand/left/input/aim/pose\n");
        }
    }
    
    if (m_actions.find("right_hand_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/aim/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_hand_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added Quest right hand pose binding: /user/hand/right/input/aim/pose\n");
        }
    }

    // Grip pose action bindings for controller tracking
    if (m_actions.find("left_hand_grip_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/grip/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_hand_grip_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added Quest left hand grip pose binding: /user/hand/left/input/grip/pose\n");
        }
    }
    
    if (m_actions.find("right_hand_grip_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/grip/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_hand_grip_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
            DevMsg("Added Quest right hand grip pose binding: /user/hand/right/input/grip/pose\n");
        }
    }

    // Suggest bindings for Quest controller
    return SuggestBindings(questProfilePath, suggestedBindings, "Quest Touch");
}

bool COpenXRInputManager::CreateGenericControllerProfile()
{
    XrPath genericProfilePath;
    
    XrResult result = xrStringToPath(m_instance, "/interaction_profiles/khr/simple_controller", &genericProfilePath);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create path for generic profile: %d\n", result);
        return false;
    }

    // Create basic bindings for generic controller (simplified set)
    std::vector<XrActionSuggestedBinding> suggestedBindings;
    
    // Basic movement and actions for simple controller
    if (m_actions.find("primary_attack") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/select/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["primary_attack"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }
    
    if (m_actions.find("menu") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/menu/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["menu"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Pose bindings
    if (m_actions.find("left_hand_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/aim/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_hand_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }
    
    if (m_actions.find("right_hand_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/aim/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_hand_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Grip pose bindings for generic controller
    if (m_actions.find("left_hand_grip_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/grip/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_hand_grip_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }
    
    if (m_actions.find("right_hand_grip_pose") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/grip/pose", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_hand_grip_pose"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Suggest bindings for generic controller
    return SuggestBindings(genericProfilePath, suggestedBindings, "Generic");
}

bool COpenXRInputManager::SuggestBindings(XrPath profilePath, const std::vector<XrActionSuggestedBinding>& bindings, const char* profileName)
{
    if (bindings.empty())
    {
        DevMsg("No bindings to suggest for %s profile\n", profileName);
        return false;
    }

    XrInteractionProfileSuggestedBinding suggestedBindingsInfo{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggestedBindingsInfo.interactionProfile = profilePath;
    suggestedBindingsInfo.suggestedBindings = bindings.data();
    suggestedBindingsInfo.countSuggestedBindings = bindings.size();

    // Get the profile path string for debugging
    char profilePathStr[XR_MAX_PATH_LENGTH];
    uint32_t profilePathStrLen = 0;
    XrResult profilePathResult = xrPathToString(m_instance, profilePath, XR_MAX_PATH_LENGTH, &profilePathStrLen, profilePathStr);
    if (XR_SUCCEEDED(profilePathResult))
    {
        DevMsg("Suggesting bindings for %s profile path: %s\n", profileName, profilePathStr);
    }

    XrResult result = xrSuggestInteractionProfileBindings(m_instance, &suggestedBindingsInfo);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to suggest bindings for %s profile: %d\n", profileName, result);
        return false;
    }
    
    DevMsg("Successfully suggested %d bindings for %s profile\n", bindings.size(), profileName);
    return true;
}

bool COpenXRInputManager::AttachActionSet()
{
    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    
    XrResult result = xrAttachSessionActionSets(m_session, &attachInfo);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to attach action set: %d\n", result);
        return false;
    }
    return true;
}

XrInputAction COpenXRInputManager::CreateBooleanAction(const char* name, const char* localizedName)
{
    XrInputAction action;
    action.name = name;           // Now properly copies the string
    action.localizedName = localizedName;  // Now properly copies the string
    action.type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    action.subactionPaths = { m_leftHandPath, m_rightHandPath };

    XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
    strcpy_s(actionInfo.actionName, name);
    strcpy_s(actionInfo.localizedActionName, localizedName);
    actionInfo.actionType = action.type;
    actionInfo.countSubactionPaths = action.subactionPaths.size();
    actionInfo.subactionPaths = action.subactionPaths.data();

    XrResult result = xrCreateAction(m_actionSet, &actionInfo, &action.handle);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create boolean action %s: %d\n", name, result);
        action.handle = XR_NULL_HANDLE;
    }
    return action;
}

XrInputAction COpenXRInputManager::CreateFloatAction(const char* name, const char* localizedName)
{
    XrInputAction action;
    action.name = name;           // Now properly copies the string
    action.localizedName = localizedName;  // Now properly copies the string
    action.type = XR_ACTION_TYPE_FLOAT_INPUT;
    
    // Set subaction paths based on the action name
    if (strstr(name, "left") != nullptr)
    {
        action.subactionPaths = { m_leftHandPath };
    }
    else if (strstr(name, "right") != nullptr)
    {
        action.subactionPaths = { m_rightHandPath };
    }
    else
    {
        // Default to both hands for general actions
        action.subactionPaths = { m_leftHandPath, m_rightHandPath };
    }

    XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
    strcpy_s(actionInfo.actionName, name);
    strcpy_s(actionInfo.localizedActionName, localizedName);
    actionInfo.actionType = action.type;
    actionInfo.countSubactionPaths = action.subactionPaths.size();
    actionInfo.subactionPaths = action.subactionPaths.data();

    XrResult result = xrCreateAction(m_actionSet, &actionInfo, &action.handle);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create float action %s: %d\n", name, result);
        action.handle = XR_NULL_HANDLE;
    }
    return action;
}

XrInputAction COpenXRInputManager::CreatePoseAction(const char* name, const char* localizedName)
{
    XrInputAction action;
    action.name = name;
    action.localizedName = localizedName;
    action.type = XR_ACTION_TYPE_POSE_INPUT;
    action.handle = XR_NULL_HANDLE;

    XrActionCreateInfo createInfo{ XR_TYPE_ACTION_CREATE_INFO };
    strcpy_s(createInfo.actionName, name);
    strcpy_s(createInfo.localizedActionName, localizedName);
    createInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    
    // Pose actions need subaction paths for left/right hands
    if (strstr(name, "left") != nullptr)
    {
        createInfo.countSubactionPaths = 1;
        createInfo.subactionPaths = &m_leftHandPath;
    }
    else if (strstr(name, "right") != nullptr)
    {
        createInfo.countSubactionPaths = 1;
        createInfo.subactionPaths = &m_rightHandPath;
    }
    else
    {
        createInfo.countSubactionPaths = 0;
        createInfo.subactionPaths = nullptr;
    }

    XrResult result = xrCreateAction(m_actionSet, &createInfo, &action.handle);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create pose action %s: %d\n", name, result);
        action.handle = XR_NULL_HANDLE;
        return action;
    }

    // Create action space for this pose action
    XrActionSpaceCreateInfo actionSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    actionSpaceInfo.action = action.handle;
    
    // Set the appropriate subaction path for the action space
    if (strstr(name, "left") != nullptr)
    {
        actionSpaceInfo.subactionPath = m_leftHandPath;
    }
    else if (strstr(name, "right") != nullptr)
    {
        actionSpaceInfo.subactionPath = m_rightHandPath;
    }
    else
    {
        actionSpaceInfo.subactionPath = XR_NULL_PATH;
    }
    
    actionSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
    actionSpaceInfo.poseInActionSpace.orientation.x = 0.0f;
    actionSpaceInfo.poseInActionSpace.orientation.y = 0.0f;
    actionSpaceInfo.poseInActionSpace.orientation.z = 0.0f;
    actionSpaceInfo.poseInActionSpace.position.x = 0.0f;
    actionSpaceInfo.poseInActionSpace.position.y = 0.0f;
    actionSpaceInfo.poseInActionSpace.position.z = 0.0f;

    XrSpace actionSpace;
    result = xrCreateActionSpace(m_session, &actionSpaceInfo, &actionSpace);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create action space for %s: %d\n", name, result);
        xrDestroyAction(action.handle);
        action.handle = XR_NULL_HANDLE;
        return action;
    }

    // Store the action space
    m_actionSpaces[name] = actionSpace;

    return action;
}

void COpenXRInputManager::PollInput()
{
    static int frameCount = 0;
    frameCount++;
    
    if (frameCount % 300 == 0) // Log every 300 frames (about once per 5 seconds)
    {
        DevMsg("OpenXR Input: Polling input (frame %d)\n", frameCount);
    }
    
    m_previousButtonStates = m_currentButtonStates;

    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    XrActiveActionSet activeActionSet{ m_actionSet };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    
    XrResult result = xrSyncActions(m_session, &syncInfo);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to sync actions: %d\n", result);
        return;
    }

    for (const auto& action : m_actions)
    {
        if (action.second.type == XR_ACTION_TYPE_BOOLEAN_INPUT)
        {
            // Poll for both hands
            //for (size_t i = 0; i < action.second.subactionPaths.size(); ++i)
            //{
                XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
                getInfo.action = action.second.handle;

                XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
                result = xrGetActionStateBoolean(m_session, &getInfo, &state);
                if (XR_SUCCEEDED(result))
                {
                    std::string key = action.first;
                    bool previousState = m_currentButtonStates[key];
                    m_currentButtonStates[key] = state.currentState;
                    
                    // Only log state changes for UI interaction actions to reduce noise
                    if (state.currentState != previousState && 
                        (key == "left_ui_interact" || key == "right_ui_interact"))
                    {
                        DevMsg("Button %s changed to %s\n", key.c_str(), state.currentState ? "pressed" : "released");
                    }
                }
            //}
        }
        else if (action.second.type == XR_ACTION_TYPE_FLOAT_INPUT)
        {
            // Poll for both hands
            //for (size_t i = 0; i < action.second.subactionPaths.size(); ++i)
            //{
                XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
                getInfo.action = action.second.handle;

                XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
                result = xrGetActionStateFloat(m_session, &getInfo, &state);
                if (XR_SUCCEEDED(result))
                {
                    std::string key = action.first;
                    float previousValue = m_currentAnalogStates[key];
                    m_currentAnalogStates[key] = state.currentState;
                    
                    // Only log significant changes in analog values (more than 0.2 difference) to reduce noise
                    if (fabs(state.currentState - previousValue) > 0.2f)
                    {
                        DevMsg("Analog %s changed to %.2f\n", key.c_str(), state.currentState);
                    }
                }
            //}
        }
        else if (action.second.type == XR_ACTION_TYPE_POSE_INPUT)
        {
            // Poll pose state for both hands
            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = action.second.handle;

            XrActionStatePose state{ XR_TYPE_ACTION_STATE_POSE };
            result = xrGetActionStatePose(m_session, &getInfo, &state);
            if (XR_SUCCEEDED(result))
            {
                std::string key = action.first;
                m_currentPoseValidStates[key] = state.isActive;
                
                // If pose is active, get the current pose
                if (state.isActive)
                {
                    // Get pose relative to reference space using the action's space
                    XrSpaceLocation spaceLocation{ XR_TYPE_SPACE_LOCATION };
                    XrSpaceLocationFlags requiredFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                    
                    // Get pose relative to reference space
                    auto actionSpaceIt = m_actionSpaces.find(key);
                    if (actionSpaceIt != m_actionSpaces.end())
                    {
                        // Get the current frame time from the DXVK layer instead of the manager's frame state
                        XrTime currentTime = 0;
                        dxvkGetPredictedDisplayTime(currentTime);
                        
                        if (currentTime == 0)
                        {
                            DevMsg("Warning: Invalid frame time for pose location, skipping\n");
                            continue;
                        }
                        
                        result = xrLocateSpace(actionSpaceIt->second, m_manager->GetReferenceSpace(), 
                                             currentTime, &spaceLocation);
                        if (XR_SUCCEEDED(result) && (spaceLocation.locationFlags & requiredFlags) == requiredFlags)
                        {
                            m_currentPoseStates[key] = spaceLocation.pose;
                        }
                        else
                        {
                            DevMsg("Failed to locate space for %s: result=%d, flags=0x%X\n", 
                                   key.c_str(), result, spaceLocation.locationFlags);
                        }
                    }
                    else
                    {
                        DevMsg("No action space found for pose action: %s\n", key.c_str());
                    }
                }
            }
            else
            {
                DevMsg("Failed to get pose state for %s: %d\n", action.first.c_str(), result);
            }
        }
    }
}

bool COpenXRInputManager::IsButtonPressed(const char* actionName)
{
    auto it = m_currentButtonStates.find(actionName);
    return it != m_currentButtonStates.end() && it->second;
}

bool COpenXRInputManager::WasButtonPressed(const char* actionName)
{
    auto prevIt = m_previousButtonStates.find(actionName);
    auto currIt = m_currentButtonStates.find(actionName);
    return prevIt != m_previousButtonStates.end() && 
           currIt != m_currentButtonStates.end() && 
           !prevIt->second && currIt->second;
}

bool COpenXRInputManager::WasButtonReleased(const char* actionName)
{
    auto prevIt = m_previousButtonStates.find(actionName);
    auto currIt = m_currentButtonStates.find(actionName);
    return prevIt != m_previousButtonStates.end() && 
           currIt != m_currentButtonStates.end() && 
           prevIt->second && !currIt->second;
}

float COpenXRInputManager::GetAnalogValue(const char* actionName)
{
    auto it = m_currentAnalogStates.find(actionName);
    return it != m_currentAnalogStates.end() ? it->second : 0.0f;
}

bool COpenXRInputManager::GetControllerPose(const char* actionName, XrPosef& pose)
{
    auto it = m_currentPoseStates.find(actionName);
    if (it != m_currentPoseStates.end())
    {
        pose = it->second;
        return true;
    }
    return false;
}

bool COpenXRInputManager::IsControllerPoseValid(const char* actionName)
{
    auto it = m_currentPoseValidStates.find(actionName);
    return it != m_currentPoseValidStates.end() && it->second;
}
