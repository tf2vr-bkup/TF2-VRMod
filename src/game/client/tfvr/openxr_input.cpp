#include "cbase.h"
#include "openxr_input.h"
#include "openxr_manager.h"

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

    // Add menu press action for cursor control
    XrInputAction menuPress = CreateBooleanAction("menu_press", "Menu Press");
    if (menuPress.handle == XR_NULL_HANDLE) return false;
    m_actions["menu_press"] = menuPress;

    return true;
}

bool COpenXRInputManager::CreateInteractionProfiles()
{
    // Create Valve Index profile
    XrInteractionProfile indexProfile;
    indexProfile.name = "valve_index_controller";
    
    XrResult result = xrStringToPath(m_instance, "/interaction_profiles/valve/index_controller", &indexProfile.path);
    if (!XR_SUCCEEDED(result)) 
    {
        DevMsg("Failed to create path for Valve Index profile: %d\n", result);
        return false;
    }

    // Movement bindings (left controller)
    if (!AddBinding(indexProfile, "move_x", "/user/hand/left/input/thumbstick/x")) return false;
    if (!AddBinding(indexProfile, "move_y", "/user/hand/left/input/thumbstick/y")) return false;

    // Turning bindings (right controller)
    if (!AddBinding(indexProfile, "turn_x", "/user/hand/right/input/thumbstick/x")) return false;

    // Primary attack (right trigger)
    if (!AddBinding(indexProfile, "primary_attack", "/user/hand/right/input/trigger/value")) return false;

    // Secondary attack (left trigger)
    if (!AddBinding(indexProfile, "secondary_attack", "/user/hand/left/input/trigger/value")) return false;

    // Use (right A button)
    if (!AddBinding(indexProfile, "use", "/user/hand/right/input/a/click")) return false;

    // Duck (left A button)
    if (!AddBinding(indexProfile, "duck", "/user/hand/left/input/a/click")) return false;

    // Jump (right B button)
    if (!AddBinding(indexProfile, "jump", "/user/hand/right/input/b/click")) return false;

    // Menu (left B button)
    if (!AddBinding(indexProfile, "menu", "/user/hand/left/input/b/click")) return false;

    // Menu press bindings for cursor control (both hands)
    if (!AddBinding(indexProfile, "menu_press", "/user/hand/left/input/trigger/click")) return false;
    if (!AddBinding(indexProfile, "menu_press", "/user/hand/right/input/trigger/click")) return false;

    // Suggest bindings for the profile
    if (!SuggestBindings(indexProfile)) return false;

    m_profiles.push_back(indexProfile);
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
    action.subactionPaths = {};

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

bool COpenXRInputManager::AddBinding(XrInteractionProfile& profile, const char* actionName, const char* bindingPath)
{
    auto it = m_actions.find(actionName);
    if (it == m_actions.end())
    {
        DevMsg("Action %s not found\n", actionName);
        return false;
    }

    XrPath path;
    XrResult result = xrStringToPath(m_instance, bindingPath, &path);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to create path for binding %s: %d\n", bindingPath, result);
        return false;
    }

    XrInputBinding binding;
    binding.action = it->second.handle;
    binding.binding = path;
    profile.bindings.push_back(binding);
    return true;
}

bool COpenXRInputManager::SuggestBindings(const XrInteractionProfile& profile)
{
    // Convert our bindings to XrActionSuggestedBinding format
    std::vector<XrActionSuggestedBinding> suggestedBindings;
    suggestedBindings.reserve(profile.bindings.size());
    
    DevMsg("Suggesting bindings for profile %s with %d bindings\n", profile.name.c_str(), profile.bindings.size());
    
    for (const auto& binding : profile.bindings)
    {
        XrActionSuggestedBinding suggestedBinding;
        suggestedBinding.action = binding.action;
        suggestedBinding.binding = binding.binding;
        suggestedBindings.push_back(suggestedBinding);
        
        // Get the path string for debugging
        char pathStr[XR_MAX_PATH_LENGTH];
        uint32_t pathStrLen = 0;
        XrResult pathResult = xrPathToString(m_instance, binding.binding, XR_MAX_PATH_LENGTH, &pathStrLen, pathStr);
        if (XR_SUCCEEDED(pathResult))
        {
            DevMsg("  Binding action to path: %s\n", pathStr);
        }
    }

    XrInteractionProfileSuggestedBinding suggestedBindingsInfo{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggestedBindingsInfo.interactionProfile = profile.path;
    suggestedBindingsInfo.suggestedBindings = suggestedBindings.data();
    suggestedBindingsInfo.countSuggestedBindings = suggestedBindings.size();

    // Get the profile path string for debugging
    char profilePathStr[XR_MAX_PATH_LENGTH];
    uint32_t profilePathStrLen = 0;
    XrResult profilePathResult = xrPathToString(m_instance, profile.path, XR_MAX_PATH_LENGTH, &profilePathStrLen, profilePathStr);
    if (XR_SUCCEEDED(profilePathResult))
    {
        DevMsg("Suggesting bindings for profile path: %s\n", profilePathStr);
    }

    XrResult result = xrSuggestInteractionProfileBindings(m_instance, &suggestedBindingsInfo);
    if (!XR_SUCCEEDED(result))
    {
        DevMsg("Failed to suggest bindings for profile %s: %d\n", profile.name.c_str(), result);
        return false;
    }
    return true;
}

void COpenXRInputManager::PollInput()
{
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
                    
                    // Log state changes
                    if (state.currentState != previousState)
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
                    
                    // Log significant changes in analog values (more than 0.1 difference)
                    if (fabs(state.currentState - previousValue) > 0.1f)
                    {
                        DevMsg("Analog %s changed to %.2f\n", key.c_str(), state.currentState);
                    }
                }
            //}
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
