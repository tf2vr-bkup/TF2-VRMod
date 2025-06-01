#ifndef OPENXR_INPUT_H
#define OPENXR_INPUT_H

#include "../public/openxr/openxr.h"
#include <string>
#include <map>
#include <vector>

class COpenXRManager;

struct XrInputAction {
    std::string name;
    std::string localizedName;
    XrActionType type;
    XrAction handle;
    std::vector<XrPath> subactionPaths;
};

struct XrInputBinding {
    XrAction action;
    XrPath binding;
};

struct XrInteractionProfile {
    std::string name;
    XrPath path;
    std::vector<XrInputBinding> bindings;
};

class COpenXRInputManager {
public:
    COpenXRInputManager(COpenXRManager* manager);
    ~COpenXRInputManager();

    bool Initialize();
    void Shutdown();
    void PollInput();

    bool IsButtonPressed(const char* actionName);
    bool WasButtonPressed(const char* actionName);
    bool WasButtonReleased(const char* actionName);
    float GetAnalogValue(const char* actionName);

private:
    bool CreateActionSet();
    bool CreateActions();
    bool CreateInteractionProfiles();
    bool AttachActionSet();

    XrInputAction CreateBooleanAction(const char* name, const char* localizedName);
    XrInputAction CreateFloatAction(const char* name, const char* localizedName);
    XrInputAction CreateVector2Action(const char* name, const char* localizedName);
    XrInputAction CreatePoseAction(const char* name, const char* localizedName);

    bool AddBinding(XrInteractionProfile& profile, const char* actionName, const char* bindingPath);
    bool SuggestBindings(const XrInteractionProfile& profile);

    COpenXRManager* m_manager;
    XrInstance m_instance;
    XrSession m_session;
    XrActionSet m_actionSet;

    // Input state
    std::map<std::string, XrInputAction> m_actions;
    std::vector<XrInteractionProfile> m_profiles;
    std::map<std::string, bool> m_previousButtonStates;
    std::map<std::string, bool> m_currentButtonStates;
    std::map<std::string, float> m_currentAnalogStates;

    // Common paths
    XrPath m_leftHandPath;
    XrPath m_rightHandPath;
};

#endif // OPENXR_INPUT_H 