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

    // Add separate UI interaction actions for left and right hands (using float for trigger values)
    XrInputAction leftUIInteract = CreateFloatAction("left_ui_interact", "Left UI Interact");
    if (leftUIInteract.handle == XR_NULL_HANDLE) return false;
    m_actions["left_ui_interact"] = leftUIInteract;

    XrInputAction rightUIInteract = CreateFloatAction("right_ui_interact", "Right UI Interact");
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

    // Add left grip button action for two-handed weapon gripping
    XrInputAction leftGrip = CreateFloatAction("left_grip", "Left Grip");
    if (leftGrip.handle == XR_NULL_HANDLE) return false;
    m_actions["left_grip"] = leftGrip;

    // Add weapon select hold action (for radial weapon selection menu)
    XrInputAction weaponSelectHold = CreateBooleanAction("weapon_select_hold", "Weapon Select Hold");
    if (weaponSelectHold.handle == XR_NULL_HANDLE) return false;
    m_actions["weapon_select_hold"] = weaponSelectHold;

    return true;
}

bool COpenXRInputManager::CreateInteractionProfiles()
{
    bool success = false;
    
    // Try to create Valve Index profile
    success |= CreateIndexControllerProfile();
    
    // Try to create Quest controller profile
    success |= CreateQuestControllerProfile();
    
    // Try minimal Quest profile for debugging if full profile fails
    if (!success)
    {

        success |= CreateMinimalQuestControllerProfile();
    }
    
    // Fall back to generic profile if all specific profiles fail
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

    // Menu (left Y button)
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

    // Left UI interaction binding (left trigger value for Index)
    if (m_actions.find("left_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/trigger/value", &bindingPath)))
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

    // Right UI interaction binding (right trigger value for Index)
    if (m_actions.find("right_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trigger/value", &bindingPath)))
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

        }
    }

    // Left grip button binding (for two-handed weapon gripping) - Index uses squeeze
    if (m_actions.find("left_grip") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_grip"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Weapon select hold binding (right trackpad click for Index)
    if (m_actions.find("weapon_select_hold") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trackpad/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["weapon_select_hold"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
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
    // Using only well-supported paths according to OpenXR spec for Oculus Touch
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

    // Use action not bound for now - will be added later

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

    // Menu (left Y button)
    if (m_actions.find("menu") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/y/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["menu"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Duck/Crouch (right A button)
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

    // Left UI interaction binding (left trigger value - will use threshold)
    if (m_actions.find("left_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/trigger/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_ui_interact"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);

        }
    }

    // Right UI interaction binding (right trigger value - will use threshold)
    if (m_actions.find("right_ui_interact") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/trigger/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["right_ui_interact"].handle;
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

        }
    }

    // Left grip button binding (for two-handed weapon gripping) - Quest uses squeeze
    if (m_actions.find("left_grip") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/value", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["left_grip"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
        }
    }

    // Weapon select hold binding (right thumbstick click for Quest)
    if (m_actions.find("weapon_select_hold") != m_actions.end())
    {
        XrPath bindingPath;
        if (XR_SUCCEEDED(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/click", &bindingPath)))
        {
            XrActionSuggestedBinding binding;
            binding.action = m_actions["weapon_select_hold"].handle;
            binding.binding = bindingPath;
            suggestedBindings.push_back(binding);
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

    // Suggest bindings for Quest controller
    return SuggestBindings(questProfilePath, suggestedBindings, "Quest Touch");
}

bool COpenXRInputManager::CreateMinimalQuestControllerProfile()
{
    XrPath questProfilePath;
    
    XrResult result = xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &questProfilePath);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create path for minimal Quest controller profile: %d\n", result);
        return false;
    }

    // Create minimal bindings for Quest controller - only essential controls and poses
    std::vector<XrActionSuggestedBinding> suggestedBindings;
    
    // Just basic movement and one trigger for testing
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

    // One trigger for primary attack
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

    // Pose action bindings for controller tracking (most important for debugging)
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

    // Suggest minimal bindings for Quest controller
    return SuggestBindings(questProfilePath, suggestedBindings, "Quest Touch (Minimal)");
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

    XrResult result = xrSuggestInteractionProfileBindings(m_instance, &suggestedBindingsInfo);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to suggest bindings for %s profile: %d (XR_ERROR_PATH_UNSUPPORTED = -22)\n", profileName, result);
        
        // Try fallback approach for unsupported paths
        if (result == -22) // XR_ERROR_PATH_UNSUPPORTED
        {
            DevMsg("Some binding paths unsupported for %s profile, trying fallback approach\n", profileName);
            
            // Try to suggest bindings one by one to identify valid ones
            std::vector<XrActionSuggestedBinding> validBindings;
            for (size_t i = 0; i < bindings.size(); ++i)
            {
                XrInteractionProfileSuggestedBinding singleBindingInfo{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
                singleBindingInfo.interactionProfile = profilePath;
                singleBindingInfo.suggestedBindings = &bindings[i];
                singleBindingInfo.countSuggestedBindings = 1;
                
                XrResult singleResult = xrSuggestInteractionProfileBindings(m_instance, &singleBindingInfo);
                if (XR_SUCCEEDED(singleResult))
                {
                    validBindings.push_back(bindings[i]);
                }
            }
            
            // Try to suggest just the valid bindings
            if (!validBindings.empty())
            {
                XrInteractionProfileSuggestedBinding validBindingsInfo{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
                validBindingsInfo.interactionProfile = profilePath;
                validBindingsInfo.suggestedBindings = validBindings.data();
                validBindingsInfo.countSuggestedBindings = validBindings.size();
                
                XrResult validResult = xrSuggestInteractionProfileBindings(m_instance, &validBindingsInfo);
                if (XR_SUCCEEDED(validResult))
                {
                    DevMsg("Successfully suggested %zu supported bindings for %s profile\n", validBindings.size(), profileName);
                    return true;
                }
            }
        }
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
    m_previousButtonStates = m_currentButtonStates;
    m_previousAnalogStates = m_currentAnalogStates;

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
                    
                    // Store button states (no logging to reduce noise)
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
                    
                    // Store analog values (no logging to reduce noise)
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
                            continue;
                        }
                        
                        result = xrLocateSpace(actionSpaceIt->second, m_manager->GetReferenceSpace(), 
                                             currentTime, &spaceLocation);
                        if (XR_SUCCEEDED(result) && (spaceLocation.locationFlags & requiredFlags) == requiredFlags)
                        {
                            m_currentPoseStates[key] = spaceLocation.pose;
                        }
                        // Pose location failed - silent handling
                    }
                }
            }
            // Failed to get pose state - silent handling
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

bool COpenXRInputManager::IsUIInteractionPressed(const char* actionName, float threshold)
{
    auto it = m_currentAnalogStates.find(actionName);
    return it != m_currentAnalogStates.end() && it->second >= threshold;
}

bool COpenXRInputManager::WasUIInteractionPressed(const char* actionName, float threshold)
{
    auto prevIt = m_previousAnalogStates.find(actionName);
    auto currIt = m_currentAnalogStates.find(actionName);
    return prevIt != m_previousAnalogStates.end() && 
           currIt != m_currentAnalogStates.end() && 
           prevIt->second < threshold && currIt->second >= threshold;
}

bool COpenXRInputManager::WasUIInteractionReleased(const char* actionName, float threshold)
{
    auto prevIt = m_previousAnalogStates.find(actionName);
    auto currIt = m_currentAnalogStates.find(actionName);
    return prevIt != m_previousAnalogStates.end() && 
           currIt != m_currentAnalogStates.end() && 
           prevIt->second >= threshold && currIt->second < threshold;
}
