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
    // Create trigger action
    XrInputAction triggerAction = CreateBooleanAction("trigger", "Trigger");
    if (triggerAction.handle == XR_NULL_HANDLE) return false;
    m_actions["trigger"] = triggerAction;

    // Create thumbstick actions
    XrInputAction thumbstickX = CreateFloatAction("thumbstick_x", "Thumbstick X");
    if (thumbstickX.handle == XR_NULL_HANDLE) return false;
    m_actions["thumbstick_x"] = thumbstickX;

    XrInputAction thumbstickY = CreateFloatAction("thumbstick_y", "Thumbstick Y");
    if (thumbstickY.handle == XR_NULL_HANDLE) return false;
    m_actions["thumbstick_y"] = thumbstickY;

    return true;
}

bool COpenXRInputManager::CreateInteractionProfiles()
{
    // Create Valve Index profile
    XrInteractionProfile indexProfile;
    indexProfile.name = "valve_index_controller";
    
    XrResult result = xrStringToPath(m_instance, "/interaction_profiles/valve/index_controller", &indexProfile.path);
    if (!XR_SUCCEEDED(result)) return false;

    // Add bindings for Index controller
    if (!AddBinding(indexProfile, "trigger", "/user/hand/left/input/trigger/click")) return false;
    if (!AddBinding(indexProfile, "trigger", "/user/hand/right/input/trigger/click")) return false;
    if (!AddBinding(indexProfile, "thumbstick_x", "/user/hand/left/input/thumbstick/x")) return false;
    if (!AddBinding(indexProfile, "thumbstick_x", "/user/hand/right/input/thumbstick/x")) return false;
    if (!AddBinding(indexProfile, "thumbstick_y", "/user/hand/left/input/thumbstick/y")) return false;
    if (!AddBinding(indexProfile, "thumbstick_y", "/user/hand/right/input/thumbstick/y")) return false;

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
    action.name = name;
    action.localizedName = localizedName;
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
    action.name = name;
    action.localizedName = localizedName;
    action.type = XR_ACTION_TYPE_FLOAT_INPUT;
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
    
    for (const auto& binding : profile.bindings)
    {
        XrActionSuggestedBinding suggestedBinding;
        suggestedBinding.action = binding.action;
        suggestedBinding.binding = binding.binding;
        suggestedBindings.push_back(suggestedBinding);
    }

    XrInteractionProfileSuggestedBinding suggestedBindingsInfo{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggestedBindingsInfo.interactionProfile = profile.path;
    suggestedBindingsInfo.suggestedBindings = suggestedBindings.data();
    suggestedBindingsInfo.countSuggestedBindings = suggestedBindings.size();

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
            for (size_t i = 0; i < action.second.subactionPaths.size(); ++i)
            {
                XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
                getInfo.action = action.second.handle;
                getInfo.subactionPath = action.second.subactionPaths[i];

                XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
                result = xrGetActionStateBoolean(m_session, &getInfo, &state);
                if (XR_SUCCEEDED(result))
                {
                    std::string key = action.first + (i == 0 ? "_left" : "_right");
                    bool previousState = m_currentButtonStates[key];
                    m_currentButtonStates[key] = state.currentState;
                    
                    // Log state changes
                    if (state.currentState != previousState)
                    {
                        DevMsg("Button %s changed to %s\n", key.c_str(), state.currentState ? "pressed" : "released");
                    }
                }
            }
        }
        else if (action.second.type == XR_ACTION_TYPE_FLOAT_INPUT)
        {
            // Poll for both hands
            for (size_t i = 0; i < action.second.subactionPaths.size(); ++i)
            {
                XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
                getInfo.action = action.second.handle;
                getInfo.subactionPath = action.second.subactionPaths[i];

                XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
                result = xrGetActionStateFloat(m_session, &getInfo, &state);
                if (XR_SUCCEEDED(result))
                {
                    std::string key = action.first + (i == 0 ? "_left" : "_right");
                    float previousValue = m_currentAnalogStates[key];
                    m_currentAnalogStates[key] = state.currentState;
                    
                    // Log significant changes in analog values (more than 0.1 difference)
                    if (fabs(state.currentState - previousValue) > 0.1f)
                    {
                        DevMsg("Analog %s changed to %.2f\n", key.c_str(), state.currentState);
                    }
                }
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
