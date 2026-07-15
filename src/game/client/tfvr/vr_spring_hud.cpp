#include "cbase.h"
#include "vr_spring_hud.h"

ConVar tfvr_killfeed_enabled("tfvr_killfeed_enabled", "1", FCVAR_ARCHIVE,
    "Enable the VR kill feed on the popup HUD");
ConVar tfvr_killfeed_scale("tfvr_killfeed_scale", ".2", FCVAR_ARCHIVE,
    "Scale of the kill feed panel");
ConVar tfvr_killfeed_offset_x("tfvr_killfeed_offset_x", "-0.6", FCVAR_ARCHIVE,
    "Horizontal offset on the popup HUD");
ConVar tfvr_killfeed_offset_y("tfvr_killfeed_offset_y", "1", FCVAR_ARCHIVE,
    "Vertical offset on the popup HUD");
ConVar tfvr_killfeed_width("tfvr_killfeed_width", "400", FCVAR_ARCHIVE,
    "Logical pixel width of the kill feed panel");
ConVar tfvr_killfeed_height("tfvr_killfeed_height", "300", FCVAR_ARCHIVE,
    "Logical pixel height of the kill feed panel");

// Preserve archived configs from the former standalone spring. These no longer
// affect rendering because the popup HUD owns distance and spring behavior.
ConVar tfvr_killfeed_distance("tfvr_killfeed_distance", "80", FCVAR_ARCHIVE | FCVAR_HIDDEN,
    "Deprecated; kill feed uses tfvr_popup_hud_distance");
ConVar tfvr_killfeed_follow_speed("tfvr_killfeed_follow_speed", "4", FCVAR_ARCHIVE | FCVAR_HIDDEN,
    "Deprecated; kill feed uses tfvr_popup_hud_follow_speed");
ConVar tfvr_killfeed_deadzone("tfvr_killfeed_deadzone", "10", FCVAR_ARCHIVE | FCVAR_HIDDEN,
    "Deprecated; kill feed uses tfvr_popup_hud_deadzone");
ConVar tfvr_killfeed_max_lag("tfvr_killfeed_max_lag", "45", FCVAR_ARCHIVE | FCVAR_HIDDEN,
    "Deprecated; kill feed uses tfvr_popup_hud_max_lag");
ConVar tfvr_killfeed_debug("tfvr_killfeed_debug", "0", FCVAR_ARCHIVE | FCVAR_HIDDEN,
    "Deprecated");

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"
