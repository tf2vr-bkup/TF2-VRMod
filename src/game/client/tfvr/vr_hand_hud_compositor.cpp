#include "cbase.h"
#include "vr_hand_hud_compositor.h"
#include "vr_world_ui_queue.h"
#include "c_tf_player.h"
#include "c_tfvr_hand.h"
#include "openxr_manager.h"
#include "hudelement.h"
#include "hud.h"
#include "tf_hud_playerstatus.h"
#include "tf/tf_hud_objectivestatus.h"
#include "tf_hud_ammostatus.h"
#include "tf_hud_itemeffectmeter.h"
#include "tf_hud_match_status.h"
#include "tfvr/tfvr_weapon_base.h"
#include "KeyValues.h"
#include "filesystem.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "view.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include "ienginevgui.h"
#include "iclientmode.h"
#include "tier0/vprof.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Global instances
CVRStatusHUDManager* g_pVRStatusHUDManager = nullptr;
CVRWeaponHUDManager* g_pVRWeaponHUDManager = nullptr;

extern ConVar tfvr_lefthand_mirror_axis;
extern ConVar tfvr_lefthand_mirror_spin;

static float GetVRScreenScaleFactor()
{
	int w, h;
	vgui::surface()->GetScreenSize(w, h);
	return (float)w / 1280.0f;
}

//=============================================================================
// ConVars - Status HUD (left hand: health/objective)
//=============================================================================

ConVar tfvr_status_hud_enabled("tfvr_status_hud_enabled", "1", FCVAR_ARCHIVE,
    "Enable the VR hand HUD (health/objective on left hand)");
ConVar tfvr_status_hud_hand("tfvr_status_hud_hand", "0", FCVAR_ARCHIVE,
    "Manual status HUD hand when auto hand is disabled: 0=left, 1=right");
ConVar tfvr_status_hud_auto_hand("tfvr_status_hud_auto_hand", "1", FCVAR_ARCHIVE,
    "Automatically attach status HUD to the off-hand based on tfvr_primary_hand");
ConVar tfvr_status_hud_use_hand_tracking("tfvr_status_hud_use_hand_tracking", "1", FCVAR_ARCHIVE,
    "Use hand tracking instead of controller pose");
ConVar tfvr_status_hud_width("tfvr_status_hud_width", "500", FCVAR_ARCHIVE,
    "Width of the VR hand HUD canvas in pixels");
ConVar tfvr_status_hud_height("tfvr_status_hud_height", "500", FCVAR_ARCHIVE,
    "Height of the VR hand HUD canvas in pixels");
ConVar tfvr_status_hud_scale("tfvr_status_hud_scale", "9", FCVAR_ARCHIVE,
    "Scale of the hand HUD in world units");
ConVar tfvr_status_hud_center_on_palm("tfvr_status_hud_center_on_palm", "1", FCVAR_ARCHIVE,
    "Auto-center the HUD on the palm bone (1=centered, 0=use offsets from palm)");
ConVar tfvr_status_hud_offset_x("tfvr_status_hud_offset_x", "-1.2", FCVAR_ARCHIVE,
    "X offset from palm center (right/left in palm space)");
ConVar tfvr_status_hud_offset_y("tfvr_status_hud_offset_y", "0", FCVAR_ARCHIVE,
    "Y offset from palm center (up/down in palm space)");
ConVar tfvr_status_hud_offset_z("tfvr_status_hud_offset_z", "0", FCVAR_ARCHIVE,
    "Z offset from palm center (forward/back in palm space)");
ConVar tfvr_status_hud_base_pitch("tfvr_status_hud_base_pitch", "0", FCVAR_ARCHIVE,
    "Base pitch rotation to orient panel as watch face (-90 = facing up from palm)");
ConVar tfvr_status_hud_base_yaw("tfvr_status_hud_base_yaw", "0", FCVAR_ARCHIVE,
    "Base yaw rotation for panel orientation");
ConVar tfvr_status_hud_base_roll("tfvr_status_hud_base_roll", "-90", FCVAR_ARCHIVE,
    "Base roll rotation for panel orientation");
ConVar tfvr_status_hud_mirror_surface_roll("tfvr_status_hud_mirror_surface_roll", "90", FCVAR_ARCHIVE,
    "Extra in-plane roll for the mirrored/off-hand status HUD");
ConVar tfvr_status_hud_pitch("tfvr_status_hud_pitch", "0", FCVAR_ARCHIVE,
    "Additional pitch rotation adjustment");
ConVar tfvr_status_hud_yaw("tfvr_status_hud_yaw", "0", FCVAR_ARCHIVE,
    "Additional yaw rotation adjustment");
ConVar tfvr_status_hud_roll("tfvr_status_hud_roll", "0", FCVAR_ARCHIVE,
    "Additional roll rotation adjustment");

// Health panel layout
ConVar tfvr_status_hud_health_enabled("tfvr_status_hud_health_enabled", "1", FCVAR_ARCHIVE,
    "Show health panel in hand HUD");
ConVar tfvr_status_hud_health_x("tfvr_status_hud_health_x", "0", FCVAR_ARCHIVE,
    "Health panel X offset (0=centered)");
ConVar tfvr_status_hud_health_y("tfvr_status_hud_health_y", "0", FCVAR_ARCHIVE,
    "Health panel Y offset from top");
ConVar tfvr_status_hud_health_center("tfvr_status_hud_health_center", "1", FCVAR_ARCHIVE,
    "Auto-center health panel horizontally");
ConVar tfvr_status_hud_health_center_v("tfvr_status_hud_health_center_v", "1", FCVAR_ARCHIVE,
    "Auto-center health panel vertically");
ConVar tfvr_status_hud_health_content_offset_x("tfvr_status_hud_health_content_offset_x", "555", FCVAR_ARCHIVE,
    "Content X offset to visually center the health cross (default -50 compensates for internal layout)");
ConVar tfvr_status_hud_health_content_offset_y("tfvr_status_hud_health_content_offset_y", "-330", FCVAR_ARCHIVE,
    "Content Y offset for health panel");
ConVar tfvr_status_hud_health_scale("tfvr_status_hud_health_scale", "1.0", FCVAR_ARCHIVE,
    "Scale factor for health panel (1.0 = native size, 0.5 = half size, 2.0 = double)");

// Objective panel layout
ConVar tfvr_status_hud_objective_enabled("tfvr_status_hud_objective_enabled", "1", FCVAR_ARCHIVE,
    "Show objective panel in hand HUD");
ConVar tfvr_status_hud_objective_x("tfvr_status_hud_objective_x", "0", FCVAR_ARCHIVE,
    "Objective panel X offset (0=centered)");
ConVar tfvr_status_hud_objective_y("tfvr_status_hud_objective_y", "140", FCVAR_ARCHIVE,
    "Objective panel Y offset (below health)");
ConVar tfvr_status_hud_objective_center("tfvr_status_hud_objective_center", "1", FCVAR_ARCHIVE,
    "Auto-center objective panel horizontally");
ConVar tfvr_status_hud_objective_center_v("tfvr_status_hud_objective_center_v", "1", FCVAR_ARCHIVE,
    "Auto-center objective panel vertically");
ConVar tfvr_status_hud_objective_content_offset_x("tfvr_status_hud_objective_content_offset_x", "0", FCVAR_ARCHIVE,
    "Content X offset for objective panel");
ConVar tfvr_status_hud_objective_content_offset_y("tfvr_status_hud_objective_content_offset_y", "-400", FCVAR_ARCHIVE,
    "Content Y offset for objective panel");
ConVar tfvr_status_hud_objective_scale("tfvr_status_hud_objective_scale", "1.0", FCVAR_ARCHIVE,
    "Scale factor for objective panel (1.0 = native size, 0.5 = half size, 2.0 = double)");

// Match status panel layout (team compositions + timer)
ConVar tfvr_status_hud_matchstatus_enabled("tfvr_status_hud_matchstatus_enabled", "0", FCVAR_ARCHIVE,
    "Show match status panel (team compositions + timer) in hand HUD (disabled by default, use VR popup HUD instead)");
ConVar tfvr_status_hud_matchstatus_x("tfvr_status_hud_matchstatus_x", "0", FCVAR_ARCHIVE,
    "Match status panel X offset");
ConVar tfvr_status_hud_matchstatus_y("tfvr_status_hud_matchstatus_y", "-200", FCVAR_ARCHIVE,
    "Match status panel Y offset (negative = above health)");
ConVar tfvr_status_hud_matchstatus_center("tfvr_status_hud_matchstatus_center", "1", FCVAR_ARCHIVE,
    "Auto-center match status panel horizontally");
ConVar tfvr_status_hud_matchstatus_content_offset_x("tfvr_status_hud_matchstatus_content_offset_x", "0", FCVAR_ARCHIVE,
    "Content X offset for match status panel");
ConVar tfvr_status_hud_matchstatus_content_offset_y("tfvr_status_hud_matchstatus_content_offset_y", "285", FCVAR_ARCHIVE,
    "Content Y offset for match status panel");
ConVar tfvr_status_hud_matchstatus_scale("tfvr_status_hud_matchstatus_scale", "1", FCVAR_ARCHIVE,
    "Scale factor for match status panel");

ConVar tfvr_status_hud_debug_bg("tfvr_status_hud_debug_bg", "0", FCVAR_ARCHIVE,
    "Show debug background for compositor bounds");
ConVar tfvr_status_hud_allow_player_model("tfvr_status_hud_allow_player_model", "0", FCVAR_ARCHIVE,
    "Allow the embedded 3D class model panel in the VR status HUD. 0 uses the safer image/label-only capture path.");

//=============================================================================
// ConVars - Weapon HUD (right hand: ammo/meters)
//=============================================================================

ConVar tfvr_weapon_hud_enabled("tfvr_weapon_hud_enabled", "1", FCVAR_ARCHIVE,
    "Enable the VR weapon HUD (ammo/meters on right hand)");
ConVar tfvr_weapon_hud_width("tfvr_weapon_hud_width", "300", FCVAR_ARCHIVE,
    "Width of the weapon HUD canvas in pixels");
ConVar tfvr_weapon_hud_height("tfvr_weapon_hud_height", "200", FCVAR_ARCHIVE,
    "Height of the weapon HUD canvas in pixels");
ConVar tfvr_weapon_hud_scale("tfvr_weapon_hud_scale", "8", FCVAR_ARCHIVE,
    "Scale of the weapon HUD in world units");
ConVar tfvr_weapon_hud_center("tfvr_weapon_hud_center", "1", FCVAR_ARCHIVE,
    "Auto-center the weapon HUD on the weapon bone");
ConVar tfvr_weapon_hud_backface_cull("tfvr_weapon_hud_backface_cull", "0", FCVAR_ARCHIVE,
    "Cull weapon HUD when its panel normal faces away from the camera");
ConVar tfvr_weapon_hud_offset_x("tfvr_weapon_hud_offset_x", "-2", FCVAR_ARCHIVE,
    "X offset from weapon bone");
ConVar tfvr_weapon_hud_offset_y("tfvr_weapon_hud_offset_y", "15", FCVAR_ARCHIVE,
    "Y offset from weapon bone");
ConVar tfvr_weapon_hud_offset_z("tfvr_weapon_hud_offset_z", "4", FCVAR_ARCHIVE,
    "Z offset from weapon bone");
// Pistol-specific placement (small weapon; the generic spot sits poorly)
ConVar tfvr_weapon_hud_pistol_offset_x("tfvr_weapon_hud_pistol_offset_x", "-2", FCVAR_ARCHIVE,
    "Pistol weapon HUD: side offset in weapon space");
ConVar tfvr_weapon_hud_pistol_offset_y("tfvr_weapon_hud_pistol_offset_y", "8", FCVAR_ARCHIVE,
    "Pistol weapon HUD: up offset in weapon space");
ConVar tfvr_weapon_hud_pistol_offset_z("tfvr_weapon_hud_pistol_offset_z", "0", FCVAR_ARCHIVE,
    "Pistol weapon HUD: forward offset in weapon space");
ConVar tfvr_weapon_hud_pistol_pitch("tfvr_weapon_hud_pistol_pitch", "180", FCVAR_ARCHIVE,
    "Pistol weapon HUD: pitch rotation in degrees");
ConVar tfvr_weapon_hud_pistol_yaw("tfvr_weapon_hud_pistol_yaw", "0", FCVAR_ARCHIVE,
    "Pistol weapon HUD: yaw rotation in degrees");
ConVar tfvr_weapon_hud_pistol_roll("tfvr_weapon_hud_pistol_roll", "0", FCVAR_ARCHIVE,
    "Pistol weapon HUD: roll rotation in degrees");
ConVar tfvr_weapon_hud_pistol_scale("tfvr_weapon_hud_pistol_scale", "6", FCVAR_ARCHIVE,
    "Pistol weapon HUD: world height of the HUD panel");

ConVar tfvr_weapon_hud_pitch("tfvr_weapon_hud_pitch", "180", FCVAR_ARCHIVE,
    "Pitch rotation adjustment");
ConVar tfvr_weapon_hud_yaw("tfvr_weapon_hud_yaw", "0", FCVAR_ARCHIVE,
    "Yaw rotation adjustment");
ConVar tfvr_weapon_hud_roll("tfvr_weapon_hud_roll", "0", FCVAR_ARCHIVE,
    "Roll rotation adjustment");
ConVar tfvr_weapon_hud_mirrored_bone_roll("tfvr_weapon_hud_mirrored_bone_roll", "180", FCVAR_ARCHIVE,
    "Extra roll applied when the weapon HUD is attached to a mirrored weapon bone");

// Ammo panel layout
ConVar tfvr_weapon_hud_ammo_enabled("tfvr_weapon_hud_ammo_enabled", "1", FCVAR_ARCHIVE,
    "Show ammo panel in weapon HUD");
ConVar tfvr_weapon_hud_ammo_x("tfvr_weapon_hud_ammo_x", "0", FCVAR_ARCHIVE,
    "Ammo panel X offset");
ConVar tfvr_weapon_hud_ammo_y("tfvr_weapon_hud_ammo_y", "0", FCVAR_ARCHIVE,
    "Ammo panel Y offset");
ConVar tfvr_weapon_hud_ammo_center("tfvr_weapon_hud_ammo_center", "1", FCVAR_ARCHIVE,
    "Auto-center ammo panel horizontally");
ConVar tfvr_weapon_hud_ammo_content_offset_x("tfvr_weapon_hud_ammo_content_offset_x", "0", FCVAR_ARCHIVE,
    "Content X offset for ammo panel");
ConVar tfvr_weapon_hud_ammo_content_offset_y("tfvr_weapon_hud_ammo_content_offset_y", "40", FCVAR_ARCHIVE,
    "Content Y offset for ammo panel");
ConVar tfvr_weapon_hud_ammo_scale("tfvr_weapon_hud_ammo_scale", "1.0", FCVAR_ARCHIVE,
    "Scale factor for ammo panel");

// Charge bar layout (bow charge, sticky charge - elements that layer on ammo)
ConVar tfvr_weapon_hud_charge_offset_x("tfvr_weapon_hud_charge_offset_x", "20", FCVAR_ARCHIVE,
    "X offset for charge bars relative to ammo position");
ConVar tfvr_weapon_hud_charge_offset_y("tfvr_weapon_hud_charge_offset_y", "50", FCVAR_ARCHIVE,
    "Y offset for charge bars relative to ammo position (positive = below ammo)");

// Metal/account panel layout
ConVar tfvr_weapon_hud_account_offset_x("tfvr_weapon_hud_account_offset_x", "5", FCVAR_ARCHIVE,
    "X offset for metal count panel (added after meter layout, positive = right)");
ConVar tfvr_weapon_hud_account_offset_y("tfvr_weapon_hud_account_offset_y", "-150", FCVAR_ARCHIVE,
    "Y offset for metal count panel (added after meter layout, positive = down)");

// Meter layout (sticky count, effect meters, etc.)
ConVar tfvr_weapon_hud_meters_y("tfvr_weapon_hud_meters_y", "100", FCVAR_ARCHIVE,
    "Y offset for meter panels (below ammo)");
ConVar tfvr_weapon_hud_meters_spacing("tfvr_weapon_hud_meters_spacing", "2", FCVAR_ARCHIVE,
    "Horizontal spacing between meter panels");
ConVar tfvr_weapon_hud_meters_width_override("tfvr_weapon_hud_meters_width_override", "60", FCVAR_ARCHIVE,
    "Override width for meter panels (0=use actual panel width, >0=use this width for spacing calculation)");
ConVar tfvr_weapon_hud_meters_content_offset_x("tfvr_weapon_hud_meters_content_offset_x", "-58", FCVAR_ARCHIVE,
    "X content offset for meter panels (negative to shift left)");

ConVar tfvr_weapon_hud_debug_bg("tfvr_weapon_hud_debug_bg", "0", FCVAR_ARCHIVE,
    "Show debug background for weapon HUD");

// Live tuning cvars are intentionally not archived; profile data is the
// persistent store, while these are temporary in-game adjustment deltas.
ConVar tfvr_weapon_hud_tune_enabled("tfvr_weapon_hud_tune_enabled", "0", 0,
    "Enable temporary active-weapon HUD tuning overrides");
ConVar tfvr_weapon_hud_tune_target("tfvr_weapon_hud_tune_target", "0", 0,
    "Active weapon HUD tuning target: 0=both, 1=placement, 2=layout");
ConVar tfvr_weapon_hud_tune_offset_x("tfvr_weapon_hud_tune_offset_x", "0", 0,
    "Active weapon HUD tuning: placement X offset delta");
ConVar tfvr_weapon_hud_tune_offset_y("tfvr_weapon_hud_tune_offset_y", "0", 0,
    "Active weapon HUD tuning: placement Y offset delta");
ConVar tfvr_weapon_hud_tune_offset_z("tfvr_weapon_hud_tune_offset_z", "0", 0,
    "Active weapon HUD tuning: placement Z offset delta");
ConVar tfvr_weapon_hud_tune_pitch("tfvr_weapon_hud_tune_pitch", "0", 0,
    "Active weapon HUD tuning: pitch delta");
ConVar tfvr_weapon_hud_tune_yaw("tfvr_weapon_hud_tune_yaw", "0", 0,
    "Active weapon HUD tuning: yaw delta");
ConVar tfvr_weapon_hud_tune_roll("tfvr_weapon_hud_tune_roll", "0", 0,
    "Active weapon HUD tuning: roll delta");
ConVar tfvr_weapon_hud_tune_scale("tfvr_weapon_hud_tune_scale", "0", 0,
    "Active weapon HUD tuning: world scale delta");
ConVar tfvr_weapon_hud_tune_ammo_x("tfvr_weapon_hud_tune_ammo_x", "0", 0,
    "Active weapon HUD tuning: ammo X offset delta");
ConVar tfvr_weapon_hud_tune_ammo_y("tfvr_weapon_hud_tune_ammo_y", "0", 0,
    "Active weapon HUD tuning: ammo Y offset delta");
ConVar tfvr_weapon_hud_tune_ammo_content_offset_x("tfvr_weapon_hud_tune_ammo_content_offset_x", "0", 0,
    "Active weapon HUD tuning: ammo content X offset delta");
ConVar tfvr_weapon_hud_tune_ammo_content_offset_y("tfvr_weapon_hud_tune_ammo_content_offset_y", "0", 0,
    "Active weapon HUD tuning: ammo content Y offset delta");
ConVar tfvr_weapon_hud_tune_ammo_scale("tfvr_weapon_hud_tune_ammo_scale", "0", 0,
    "Active weapon HUD tuning: ammo scale delta");
ConVar tfvr_weapon_hud_tune_meters_y("tfvr_weapon_hud_tune_meters_y", "0", 0,
    "Active weapon HUD tuning: meter Y offset delta");

//=============================================================================
// Helper Functions
//=============================================================================

//-----------------------------------------------------------------------------
// Check if a panel is facing away from the camera (backface culling)
//-----------------------------------------------------------------------------
static bool IsPanelBackfacing(const VMatrix& panelToWorld)
{
    // Panel normal is the Z-axis of the transform
    Vector panelNormal(panelToWorld[0][2], panelToWorld[1][2], panelToWorld[2][2]);
    Vector panelPos = panelToWorld.GetTranslation();
    Vector toCamera = MainViewOrigin() - panelPos;
    return DotProduct(panelNormal, toCamera) < 0;
}

//-----------------------------------------------------------------------------
// Rotate the rendered panel within its own surface. This spins the panel X/Y
// basis around the panel normal without changing where the surface faces.
//-----------------------------------------------------------------------------
static void RotatePanelInPlane(VMatrix& panelToWorld, float flDegrees)
{
    if (flDegrees == 0.0f)
        return;

    float flRadians = DEG2RAD(flDegrees);
    float flCos = cosf(flRadians);
    float flSin = sinf(flRadians);

    Vector panelX(panelToWorld[0][0], panelToWorld[1][0], panelToWorld[2][0]);
    Vector panelY(panelToWorld[0][1], panelToWorld[1][1], panelToWorld[2][1]);

    Vector rotatedX = panelX * flCos + panelY * flSin;
    Vector rotatedY = panelY * flCos - panelX * flSin;

    panelToWorld.SetForward(rotatedX);
    panelToWorld.SetLeft(rotatedY);
}

//-----------------------------------------------------------------------------
static void GetWeaponHudMirrorSigns(float sign[3])
{
    sign[0] = 1.0f;
    sign[1] = 1.0f;
    sign[2] = 1.0f;

    const int reflectAxis = clamp(tfvr_lefthand_mirror_axis.GetInt(), 0, 2);
    sign[reflectAxis] = -sign[reflectAxis];

    const int spin = tfvr_lefthand_mirror_spin.GetInt();
    if (spin >= 1 && spin <= 3)
    {
        const int spinAxis = spin - 1;
        for (int axis = 0; axis < 3; axis++)
        {
            if (axis != spinAxis)
                sign[axis] = -sign[axis];
        }
    }
}

//-----------------------------------------------------------------------------
static Vector MirrorWeaponHudLocalOffset(const Vector& offset)
{
    float sign[3];
    GetWeaponHudMirrorSigns(sign);

    return Vector(offset.x * sign[0], offset.y * sign[1], offset.z * sign[2]);
}

//-----------------------------------------------------------------------------
struct VRWeaponHudPlacementProfile_t
{
    int nWidth;
    int nHeight;
    Vector vOffset;
    QAngle angRotation;
    float flScale;
};

struct VRWeaponHudLayoutProfile_t
{
    bool bAmmoEnabled;
    int nAmmoX;
    int nAmmoY;
    bool bAmmoCenter;
    int nAmmoContentOffsetX;
    int nAmmoContentOffsetY;
    float flAmmoScale;
    int nChargeOffsetX;
    int nChargeOffsetY;
    int nAccountOffsetX;
    int nAccountOffsetY;
    int nMetersY;
    int nMetersSpacing;
    int nMetersWidthOverride;
    int nMetersContentOffsetX;
};

struct VRWeaponHudProfile_t
{
    VRWeaponHudPlacementProfile_t placement;
    VRWeaponHudLayoutProfile_t layout;
};

static const char* TFVR_WEAPON_HUD_PROFILE_FILE = "scripts/tfvr_weapon_hud_profiles.txt";
static KeyValues* g_pWeaponHudProfileData = nullptr;
static bool g_bWeaponHudProfileDataLoaded = false;

static bool IsPistolWeaponID(int iWeaponID)
{
    return iWeaponID == TF_WEAPON_PISTOL ||
           iWeaponID == TF_WEAPON_PISTOL_SCOUT ||
           iWeaponID == TF_WEAPON_HANDGUN_SCOUT_PRIMARY ||
           iWeaponID == TF_WEAPON_HANDGUN_SCOUT_SECONDARY;
}

static const char* GetWeaponHudFamilyKey(const C_TFWeaponBase* pWeapon)
{
    if (!pWeapon)
        return nullptr;

    int iWeaponID = pWeapon->GetWeaponID();
    if (IsPistolWeaponID(iWeaponID))
        return "pistol";

    switch (iWeaponID)
    {
    case TF_WEAPON_SHOTGUN_PRIMARY:
    case TF_WEAPON_SHOTGUN_SOLDIER:
    case TF_WEAPON_SHOTGUN_HWG:
    case TF_WEAPON_SHOTGUN_PYRO:
    case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
    case TF_WEAPON_SCATTERGUN:
        return "shotgun";
    case TF_WEAPON_ROCKETLAUNCHER:
    case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
    case TF_WEAPON_GRENADELAUNCHER:
    case TF_WEAPON_PIPEBOMBLAUNCHER:
        return "launcher";
    case TF_WEAPON_FLAMETHROWER:
    case TF_WEAPON_FLAREGUN:
    case TF_WEAPON_FLAREGUN_REVENGE:
        return "pyro";
    case TF_WEAPON_MINIGUN:
        return "minigun";
    case TF_WEAPON_SNIPERRIFLE:
    case TF_WEAPON_SNIPERRIFLE_DECAP:
    case TF_WEAPON_SNIPERRIFLE_CLASSIC:
    case TF_WEAPON_COMPOUND_BOW:
    case TF_WEAPON_SMG:
        return "sniper";
    case TF_WEAPON_MEDIGUN:
        return "medigun";
    case TF_WEAPON_BAT:
    case TF_WEAPON_BAT_WOOD:
    case TF_WEAPON_BAT_FISH:
    case TF_WEAPON_BAT_GIFTWRAP:
    case TF_WEAPON_BOTTLE:
    case TF_WEAPON_FIREAXE:
    case TF_WEAPON_CLUB:
    case TF_WEAPON_KNIFE:
    case TF_WEAPON_FISTS:
    case TF_WEAPON_SHOVEL:
    case TF_WEAPON_WRENCH:
    case TF_WEAPON_SWORD:
        return "melee";
    case TF_WEAPON_PDA:
    case TF_WEAPON_PDA_ENGINEER_BUILD:
    case TF_WEAPON_PDA_ENGINEER_DESTROY:
    case TF_WEAPON_PDA_SPY:
    case TF_WEAPON_PDA_SPY_BUILD:
    case TF_WEAPON_BUILDER:
        return "pda";
    case TF_WEAPON_LUNCHBOX:
    case TF_WEAPON_CLEAVER:
        return "throwable";
    default:
        return nullptr;
    }
}

static C_TFWeaponBase* GetActiveHudWeapon()
{
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    return pPlayer ? pPlayer->GetActiveTFWeapon() : nullptr;
}

static bool WeaponHudProfileHasKey(KeyValues* pProfileData, const char* pszKey)
{
    return pProfileData && pProfileData->FindKey(pszKey, false) != nullptr;
}

static void WeaponHudProfileReload()
{
    if (g_pWeaponHudProfileData)
    {
        g_pWeaponHudProfileData->deleteThis();
        g_pWeaponHudProfileData = nullptr;
    }

    g_bWeaponHudProfileDataLoaded = false;
}

static KeyValues* GetWeaponHudProfileData()
{
    if (g_bWeaponHudProfileDataLoaded)
        return g_pWeaponHudProfileData;

    g_bWeaponHudProfileDataLoaded = true;

    KeyValues* pProfileData = new KeyValues("TFVRWeaponHudProfiles");
    if (!pProfileData->LoadFromFile(filesystem, TFVR_WEAPON_HUD_PROFILE_FILE, "GAME"))
    {
        pProfileData->deleteThis();
        return nullptr;
    }

    g_pWeaponHudProfileData = pProfileData;
    return g_pWeaponHudProfileData;
}

static void ParseWeaponHudProfileVector2(KeyValues* pProfileData, const char* pszKey, int& nX, int& nY)
{
    if (!WeaponHudProfileHasKey(pProfileData, pszKey))
        return;

    Vector vecValue(0, 0, 0);
    UTIL_StringToVector(vecValue.Base(), pProfileData->GetString(pszKey, "0 0 0"));
    nX = (int)vecValue.x;
    nY = (int)vecValue.y;
}

static bool TFVRWeaponHudTuningAffectsPlacement()
{
    if (!tfvr_weapon_hud_tune_enabled.GetBool())
        return false;

    int nTarget = tfvr_weapon_hud_tune_target.GetInt();
    return nTarget == 0 || nTarget == 1;
}

static bool TFVRWeaponHudTuningAffectsLayout()
{
    if (!tfvr_weapon_hud_tune_enabled.GetBool())
        return false;

    int nTarget = tfvr_weapon_hud_tune_target.GetInt();
    return nTarget == 0 || nTarget == 2;
}

static VRWeaponHudProfile_t BuildWeaponHudGlobalDefaults()
{
    VRWeaponHudProfile_t profile;

    profile.placement.nWidth = tfvr_weapon_hud_width.GetInt();
    profile.placement.nHeight = tfvr_weapon_hud_height.GetInt();
    profile.placement.vOffset.Init(
        tfvr_weapon_hud_offset_x.GetFloat(),
        tfvr_weapon_hud_offset_y.GetFloat(),
        tfvr_weapon_hud_offset_z.GetFloat()
    );
    profile.placement.angRotation.Init(
        tfvr_weapon_hud_pitch.GetFloat(),
        tfvr_weapon_hud_yaw.GetFloat(),
        tfvr_weapon_hud_roll.GetFloat()
    );
    profile.placement.flScale = tfvr_weapon_hud_scale.GetFloat();

    profile.layout.bAmmoEnabled = tfvr_weapon_hud_ammo_enabled.GetBool();
    profile.layout.nAmmoX = tfvr_weapon_hud_ammo_x.GetInt();
    profile.layout.nAmmoY = tfvr_weapon_hud_ammo_y.GetInt();
    profile.layout.bAmmoCenter = tfvr_weapon_hud_ammo_center.GetBool();
    profile.layout.nAmmoContentOffsetX = tfvr_weapon_hud_ammo_content_offset_x.GetInt();
    profile.layout.nAmmoContentOffsetY = tfvr_weapon_hud_ammo_content_offset_y.GetInt();
    profile.layout.flAmmoScale = tfvr_weapon_hud_ammo_scale.GetFloat();
    profile.layout.nChargeOffsetX = tfvr_weapon_hud_charge_offset_x.GetInt();
    profile.layout.nChargeOffsetY = tfvr_weapon_hud_charge_offset_y.GetInt();
    profile.layout.nAccountOffsetX = tfvr_weapon_hud_account_offset_x.GetInt();
    profile.layout.nAccountOffsetY = tfvr_weapon_hud_account_offset_y.GetInt();
    profile.layout.nMetersY = tfvr_weapon_hud_meters_y.GetInt();
    profile.layout.nMetersSpacing = tfvr_weapon_hud_meters_spacing.GetInt();
    profile.layout.nMetersWidthOverride = tfvr_weapon_hud_meters_width_override.GetInt();
    profile.layout.nMetersContentOffsetX = tfvr_weapon_hud_meters_content_offset_x.GetInt();

    return profile;
}

static void ApplyWeaponHudFamilyProfile(VRWeaponHudProfile_t& profile, C_TFWeaponBase* pWeapon)
{
    if (!pWeapon || !IsPistolWeaponID(pWeapon->GetWeaponID()))
        return;

    // Preserve the old pistol override as a fallback until the profile file
    // contains explicit pistol tuning.
    profile.placement.vOffset.Init(
        tfvr_weapon_hud_pistol_offset_x.GetFloat(),
        tfvr_weapon_hud_pistol_offset_y.GetFloat(),
        tfvr_weapon_hud_pistol_offset_z.GetFloat()
    );
    profile.placement.angRotation.Init(
        tfvr_weapon_hud_pistol_pitch.GetFloat(),
        tfvr_weapon_hud_pistol_yaw.GetFloat(),
        tfvr_weapon_hud_pistol_roll.GetFloat()
    );
    profile.placement.flScale = tfvr_weapon_hud_pistol_scale.GetFloat();
}

static void ApplyWeaponHudKeyValuesProfile(VRWeaponHudProfile_t& profile, KeyValues* pProfileData)
{
    if (!pProfileData)
        return;

    if (WeaponHudProfileHasKey(pProfileData, "HudSize"))
    {
        Vector vecSize(0, 0, 0);
        UTIL_StringToVector(vecSize.Base(), pProfileData->GetString("HudSize", "0 0 0"));
        if (vecSize.x > 0 && vecSize.y > 0)
        {
            profile.placement.nWidth = (int)vecSize.x;
            profile.placement.nHeight = (int)vecSize.y;
        }
    }

    if (WeaponHudProfileHasKey(pProfileData, "HudOffset"))
        UTIL_StringToVector(profile.placement.vOffset.Base(), pProfileData->GetString("HudOffset", "0 0 0"));
    if (WeaponHudProfileHasKey(pProfileData, "HudAngles"))
        UTIL_StringToVector(profile.placement.angRotation.Base(), pProfileData->GetString("HudAngles", "0 0 0"));
    if (WeaponHudProfileHasKey(pProfileData, "HudScale"))
        profile.placement.flScale = pProfileData->GetFloat("HudScale", profile.placement.flScale);

    if (WeaponHudProfileHasKey(pProfileData, "AmmoEnabled"))
        profile.layout.bAmmoEnabled = pProfileData->GetBool("AmmoEnabled", profile.layout.bAmmoEnabled);
    if (WeaponHudProfileHasKey(pProfileData, "AmmoCenter"))
        profile.layout.bAmmoCenter = pProfileData->GetBool("AmmoCenter", profile.layout.bAmmoCenter);

    ParseWeaponHudProfileVector2(pProfileData, "AmmoOffset", profile.layout.nAmmoX, profile.layout.nAmmoY);
    ParseWeaponHudProfileVector2(pProfileData, "AmmoContentOffset", profile.layout.nAmmoContentOffsetX, profile.layout.nAmmoContentOffsetY);
    ParseWeaponHudProfileVector2(pProfileData, "ChargeOffset", profile.layout.nChargeOffsetX, profile.layout.nChargeOffsetY);
    ParseWeaponHudProfileVector2(pProfileData, "AccountOffset", profile.layout.nAccountOffsetX, profile.layout.nAccountOffsetY);

    if (WeaponHudProfileHasKey(pProfileData, "AmmoScale"))
        profile.layout.flAmmoScale = pProfileData->GetFloat("AmmoScale", profile.layout.flAmmoScale);
    if (WeaponHudProfileHasKey(pProfileData, "MetersY"))
        profile.layout.nMetersY = pProfileData->GetInt("MetersY", profile.layout.nMetersY);
    if (WeaponHudProfileHasKey(pProfileData, "MetersSpacing"))
        profile.layout.nMetersSpacing = pProfileData->GetInt("MetersSpacing", profile.layout.nMetersSpacing);
    if (WeaponHudProfileHasKey(pProfileData, "MetersWidthOverride"))
        profile.layout.nMetersWidthOverride = pProfileData->GetInt("MetersWidthOverride", profile.layout.nMetersWidthOverride);
    if (WeaponHudProfileHasKey(pProfileData, "MetersContentOffsetX"))
        profile.layout.nMetersContentOffsetX = pProfileData->GetInt("MetersContentOffsetX", profile.layout.nMetersContentOffsetX);
}

static void ApplyWeaponHudNamedProfile(VRWeaponHudProfile_t& profile, KeyValues* pProfileRoot, const char* pszProfileName)
{
    if (!pProfileRoot || !pszProfileName || pszProfileName[0] == '\0')
        return;

    ApplyWeaponHudKeyValuesProfile(profile, pProfileRoot->FindKey(pszProfileName, false));
}

static void ApplyWeaponHudDefaultFileProfile(VRWeaponHudProfile_t& profile)
{
    KeyValues* pProfileRoot = GetWeaponHudProfileData();
    if (!pProfileRoot)
        return;

    ApplyWeaponHudNamedProfile(profile, pProfileRoot, "default");
}

static void ApplyWeaponHudSpecificFileProfiles(VRWeaponHudProfile_t& profile, C_TFWeaponBase* pWeapon)
{
    KeyValues* pProfileRoot = GetWeaponHudProfileData();
    if (!pProfileRoot)
        return;

    const char* pszFamilyKey = GetWeaponHudFamilyKey(pWeapon);
    ApplyWeaponHudNamedProfile(profile, pProfileRoot, pszFamilyKey);

    if (pWeapon)
    {
        const char* pszClassname = pWeapon->GetClassname();
        ApplyWeaponHudNamedProfile(profile, pProfileRoot, pszClassname);
    }
}

static void ApplyWeaponHudTuningProfile(VRWeaponHudProfile_t& profile)
{
    if (TFVRWeaponHudTuningAffectsPlacement())
    {
        profile.placement.vOffset.x += tfvr_weapon_hud_tune_offset_x.GetFloat();
        profile.placement.vOffset.y += tfvr_weapon_hud_tune_offset_y.GetFloat();
        profile.placement.vOffset.z += tfvr_weapon_hud_tune_offset_z.GetFloat();
        profile.placement.angRotation.x += tfvr_weapon_hud_tune_pitch.GetFloat();
        profile.placement.angRotation.y += tfvr_weapon_hud_tune_yaw.GetFloat();
        profile.placement.angRotation.z += tfvr_weapon_hud_tune_roll.GetFloat();
        profile.placement.flScale = MAX(0.1f, profile.placement.flScale + tfvr_weapon_hud_tune_scale.GetFloat());
    }

    if (TFVRWeaponHudTuningAffectsLayout())
    {
        profile.layout.nAmmoX += tfvr_weapon_hud_tune_ammo_x.GetInt();
        profile.layout.nAmmoY += tfvr_weapon_hud_tune_ammo_y.GetInt();
        profile.layout.nAmmoContentOffsetX += tfvr_weapon_hud_tune_ammo_content_offset_x.GetInt();
        profile.layout.nAmmoContentOffsetY += tfvr_weapon_hud_tune_ammo_content_offset_y.GetInt();
        profile.layout.flAmmoScale = MAX(0.1f, profile.layout.flAmmoScale + tfvr_weapon_hud_tune_ammo_scale.GetFloat());
        profile.layout.nMetersY += tfvr_weapon_hud_tune_meters_y.GetInt();
    }
}

static VRWeaponHudProfile_t ResolveWeaponHudProfile(C_TFWeaponBase* pWeapon, bool bApplyTuning)
{
    VRWeaponHudProfile_t profile = BuildWeaponHudGlobalDefaults();

    ApplyWeaponHudDefaultFileProfile(profile);
    ApplyWeaponHudFamilyProfile(profile, pWeapon);
    ApplyWeaponHudSpecificFileProfiles(profile, pWeapon);

    if (bApplyTuning)
        ApplyWeaponHudTuningProfile(profile);

    return profile;
}

static const char* GetWeaponHudExportKey(const CCommand& args, C_TFWeaponBase* pWeapon)
{
    const char* pszMode = args.ArgC() > 1 ? args[1] : "weapon";
    if (FStrEq(pszMode, "default"))
        return "default";
    if (FStrEq(pszMode, "family"))
    {
        const char* pszFamilyKey = GetWeaponHudFamilyKey(pWeapon);
        return pszFamilyKey ? pszFamilyKey : "default";
    }
    if (FStrEq(pszMode, "weapon"))
    {
        const char* pszClassname = pWeapon ? pWeapon->GetClassname() : nullptr;
        return pszClassname && pszClassname[0] != '\0' ? pszClassname : "default";
    }

    Msg("Unknown export mode '%s'. Use: weapon, family, or default.\n", pszMode);
    const char* pszClassname = pWeapon ? pWeapon->GetClassname() : nullptr;
    return pszClassname && pszClassname[0] != '\0' ? pszClassname : "default";
}

static void ExportActiveWeaponHudProfile(const CCommand& args)
{
    C_TFWeaponBase* pWeapon = GetActiveHudWeapon();
    if (!pWeapon)
    {
        Msg("tfvr_weapon_hud_export_profile: no active TF weapon\n");
        return;
    }

    VRWeaponHudProfile_t profile = ResolveWeaponHudProfile(pWeapon, true);
    const char* pszClassname = pWeapon->GetClassname();
    const char* pszFamilyKey = GetWeaponHudFamilyKey(pWeapon);
    const char* pszExportKey = GetWeaponHudExportKey(args, pWeapon);

    Msg("=== TF2VR weapon HUD profile ===\n");
    Msg("Weapon: %s (id %d)\n", pszClassname ? pszClassname : "<unknown>", pWeapon->GetWeaponID());
    Msg("Family: %s\n", pszFamilyKey ? pszFamilyKey : "<none>");
    Msg("Export key: %s\n", pszExportKey);
    Msg("Profile file: %s\n", TFVR_WEAPON_HUD_PROFILE_FILE);
    Msg("\"%s\"\n", pszExportKey);
    Msg("{\n");
    Msg("\t\"HudSize\" \"%d %d\"\n", profile.placement.nWidth, profile.placement.nHeight);
    Msg("\t\"HudOffset\" \"%.3f %.3f %.3f\"\n", profile.placement.vOffset.x, profile.placement.vOffset.y, profile.placement.vOffset.z);
    Msg("\t\"HudAngles\" \"%.3f %.3f %.3f\"\n", profile.placement.angRotation.x, profile.placement.angRotation.y, profile.placement.angRotation.z);
    Msg("\t\"HudScale\" \"%.3f\"\n", profile.placement.flScale);
    Msg("\t\"AmmoEnabled\" \"%d\"\n", profile.layout.bAmmoEnabled ? 1 : 0);
    Msg("\t\"AmmoCenter\" \"%d\"\n", profile.layout.bAmmoCenter ? 1 : 0);
    Msg("\t\"AmmoOffset\" \"%d %d\"\n", profile.layout.nAmmoX, profile.layout.nAmmoY);
    Msg("\t\"AmmoContentOffset\" \"%d %d\"\n", profile.layout.nAmmoContentOffsetX, profile.layout.nAmmoContentOffsetY);
    Msg("\t\"AmmoScale\" \"%.3f\"\n", profile.layout.flAmmoScale);
    Msg("\t\"MetersY\" \"%d\"\n", profile.layout.nMetersY);
    Msg("\t\"MetersSpacing\" \"%d\"\n", profile.layout.nMetersSpacing);
    Msg("\t\"MetersWidthOverride\" \"%d\"\n", profile.layout.nMetersWidthOverride);
    Msg("\t\"MetersContentOffsetX\" \"%d\"\n", profile.layout.nMetersContentOffsetX);
    Msg("\t\"ChargeOffset\" \"%d %d\"\n", profile.layout.nChargeOffsetX, profile.layout.nChargeOffsetY);
    Msg("\t\"AccountOffset\" \"%d %d\"\n", profile.layout.nAccountOffsetX, profile.layout.nAccountOffsetY);
    Msg("}\n");
}

static ConCommand tfvr_weapon_hud_export_profile(
    "tfvr_weapon_hud_export_profile",
    ExportActiveWeaponHudProfile,
    "Print active weapon HUD profile for profile file. Args: weapon (default), family, default"
);

static ConCommand tfvr_weapon_hud_print_profile(
    "tfvr_weapon_hud_print_profile",
    ExportActiveWeaponHudProfile,
    "Alias for tfvr_weapon_hud_export_profile"
);

static void ReloadWeaponHudProfiles(const CCommand& /*args*/)
{
    WeaponHudProfileReload();
    Msg("Reloaded %s\n", TFVR_WEAPON_HUD_PROFILE_FILE);
}

static ConCommand tfvr_weapon_hud_reload_profiles(
    "tfvr_weapon_hud_reload_profiles",
    ReloadWeaponHudProfiles,
    "Reload TF2VR weapon HUD profiles from scripts/tfvr_weapon_hud_profiles.txt"
);

//-----------------------------------------------------------------------------
// Check if an item effect meter pointer is still valid
//-----------------------------------------------------------------------------
static bool IsItemEffectMeterValid(CHudElement* pElement)
{
    if (!pElement)
        return false;

    // Check if this pointer is in the current auto list
    for (int i = 0; i < IHudItemEffectMeterAutoList::AutoList().Count(); i++)
    {
        CHudItemEffectMeter* pMeter = static_cast<CHudItemEffectMeter*>(IHudItemEffectMeterAutoList::AutoList()[i]);
        if (static_cast<CHudElement*>(pMeter) == pElement)
            return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
bool VRHudElementSlot_t::ShouldDraw() const
{
    if (!szName || szName[0] == '\0')
        return false;

    CHudElement* pElement = nullptr;

    // Named elements - look up fresh to avoid dangling pointers
    if (V_strcmp(szName, "ItemEffectMeter") != 0)
    {
        pElement = gHUD.FindElement(szName);
    }
    else if (pHudElement && IsItemEffectMeterValid(pHudElement))
    {
        pElement = pHudElement;
    }

    return pElement ? pElement->ShouldDraw() : false;
}

//-----------------------------------------------------------------------------
bool VRHudElementSlot_t::IsValid() const
{
    if (!szName || szName[0] == '\0')
        return false;

    // Named elements - check if element exists
    if (V_strcmp(szName, "ItemEffectMeter") != 0)
    {
        return gHUD.FindElement(szName) != nullptr;
    }

    // Item effect meters - validate against auto list
    return pHudElement && IsItemEffectMeterValid(pHudElement);
}

//-----------------------------------------------------------------------------
// Get a validated panel pointer for a slot, looking up fresh to avoid dangling pointers
//-----------------------------------------------------------------------------
static vgui::Panel* GetSlotPanel(const VRHudElementSlot_t& slot)
{
    if (!slot.szName || slot.szName[0] == '\0')
        return nullptr;

    // Named elements (except generic "ItemEffectMeter") - look up by name
    if (V_strcmp(slot.szName, "ItemEffectMeter") != 0)
    {
        CHudElement* pElement = gHUD.FindElement(slot.szName);
        return pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;
    }

    // Item effect meters - validate pointer is still in the auto list
    if (slot.pHudElement && IsItemEffectMeterValid(slot.pHudElement))
    {
        return dynamic_cast<vgui::Panel*>(slot.pHudElement);
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// The vanilla player status panel may contain CTFPlayerModelPanel, which renders
// a live 3D model from inside VGUI. That path can leave shader state dirty when
// the panel is captured into the VR hand HUD, so hide it for this capture pass.
//-----------------------------------------------------------------------------
struct VRPanelVisibilityRestore_t
{
    vgui::Panel* pPanel;
    bool bWasVisible;
};

static int HideStatusHUDUnsafeChildren(vgui::Panel* pPanel, VRPanelVisibilityRestore_t* pRestore, int nMaxRestore)
{
    if (!pPanel || tfvr_status_hud_allow_player_model.GetBool())
        return 0;

    const char* pszUnsafePanels[] =
    {
        "classmodelpanel",
        "classmodelpanelBG",
    };

    int nRestoreCount = 0;
    for (int i = 0; i < ARRAYSIZE(pszUnsafePanels) && nRestoreCount < nMaxRestore; i++)
    {
        vgui::Panel* pChild = pPanel->FindChildByName(pszUnsafePanels[i], true);
        if (!pChild)
            continue;

        pRestore[nRestoreCount].pPanel = pChild;
        pRestore[nRestoreCount].bWasVisible = pChild->IsVisible();
        nRestoreCount++;

        pChild->SetVisible(false);
    }

    return nRestoreCount;
}

static void RestorePanelVisibility(VRPanelVisibilityRestore_t* pRestore, int nRestoreCount)
{
    for (int i = nRestoreCount - 1; i >= 0; i--)
    {
        if (pRestore[i].pPanel)
            pRestore[i].pPanel->SetVisible(pRestore[i].bWasVisible);
    }
}

//=============================================================================
// CVRHUDCompositor Base Implementation
//=============================================================================

DECLARE_BUILD_FACTORY(CVRHUDCompositor);

//-----------------------------------------------------------------------------
CVRHUDCompositor::CVRHUDCompositor(vgui::Panel* parent, const char* name)
    : BaseClass(parent, name)
{
    m_bInitialized = false;
    m_nCompositorWidth = 350;
    m_nCompositorHeight = 500;
    m_nNumSlots = 0;
    m_bShowDebugBackground = false;

    for (int i = 0; i < MAX_HUD_SLOTS; i++)
    {
        m_HudSlots[i] = VRHudElementSlot_t();
    }

    SetSize(m_nCompositorWidth, m_nCompositorHeight);
    SetPaintBackgroundEnabled(true);
    SetPaintBorderEnabled(false);
    SetVisible(true);
    SetEnabled(true);
    SetMouseInputEnabled(false);
    SetKeyBoardInputEnabled(false);
}

//-----------------------------------------------------------------------------
CVRHUDCompositor::~CVRHUDCompositor()
{
    Shutdown();
}

//-----------------------------------------------------------------------------
bool CVRHUDCompositor::Initialize()
{
    if (m_bInitialized)
        return true;

    m_bInitialized = true;
    return true;
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::Shutdown()
{
    ClearSlots();
    m_bInitialized = false;
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::UpdateLayout()
{
    // Base class does nothing - derived classes override
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::GetCompositorSize(int& width, int& height) const
{
    width = m_nCompositorWidth;
    height = m_nCompositorHeight;
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::SetCompositorSize(int width, int height)
{
    m_nCompositorWidth = width;
    m_nCompositorHeight = height;
    SetSize(width, height);
}

//-----------------------------------------------------------------------------
int CVRHUDCompositor::CalculateCenteredX(int panelWidth) const
{
    return (m_nCompositorWidth - panelWidth) / 2;
}

//-----------------------------------------------------------------------------
int CVRHUDCompositor::CalculateCenteredY(int panelHeight) const
{
    return (m_nCompositorHeight - panelHeight) / 2;
}

//-----------------------------------------------------------------------------
int CVRHUDCompositor::AddSlot(vgui::Panel* pPanel, const char* szName)
{
    return AddSlot(pPanel, nullptr, szName, false);
}

//-----------------------------------------------------------------------------
int CVRHUDCompositor::AddSlot(vgui::Panel* pPanel, CHudElement* pHudElement, const char* szName, bool bDynamic)
{
    if (m_nNumSlots >= MAX_HUD_SLOTS)
    {
        Warning("VR HUD Compositor: Too many slots, max is %d\n", MAX_HUD_SLOTS);
        return -1;
    }

    int index = m_nNumSlots++;
    VRHudElementSlot_t& slot = m_HudSlots[index];
    slot.pPanel = pPanel;
    slot.pHudElement = pHudElement;
    slot.szName = szName;
    slot.bEnabled = true;
    slot.bDynamic = bDynamic;

    return index;
}

//-----------------------------------------------------------------------------
VRHudElementSlot_t* CVRHUDCompositor::GetSlot(int index)
{
    if (index < 0 || index >= m_nNumSlots)
        return nullptr;
    return &m_HudSlots[index];
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::ClearSlots()
{
    for (int i = 0; i < MAX_HUD_SLOTS; i++)
    {
        m_HudSlots[i].pPanel = nullptr;
    }
    m_nNumSlots = 0;
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::PaintBackground()
{
    if (m_bShowDebugBackground)
    {
        vgui::surface()->DrawSetColor(Color(40, 40, 40, 180));
        vgui::surface()->DrawFilledRect(0, 0, m_nCompositorWidth, m_nCompositorHeight);

        vgui::surface()->DrawSetColor(Color(255, 200, 0, 255));
        vgui::surface()->DrawOutlinedRect(0, 0, m_nCompositorWidth, m_nCompositorHeight);
    }
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::Paint()
{
    VPROF("VRHUDCompositor_Paint");

    if (!m_bInitialized)
        return;

    UpdateLayout();

    g_pMatSystemSurface->DisableClipping(true);

    for (int i = 0; i < m_nNumSlots; i++)
    {
        VRHudElementSlot_t& slot = m_HudSlots[i];

        if (!slot.bEnabled)
            continue;

        // For dynamic elements, check ShouldDraw first
        if (slot.bDynamic && !slot.ShouldDraw())
            continue;

        // Get a valid panel pointer (looks up fresh to avoid dangling pointers)
        vgui::Panel* pPanel = GetSlotPanel(slot);
        if (!pPanel)
            continue;

        int panelWidth, panelHeight;
        pPanel->GetSize(panelWidth, panelHeight);

        // Skip if panel has no size
        if (panelWidth <= 0 || panelHeight <= 0)
            continue;

        float scale = clamp(slot.flScale, 0.1f, 4.0f);
        int scaledWidth = (int)(panelWidth * scale);
        int scaledHeight = (int)(panelHeight * scale);

        int offsetX = slot.nOffsetX;
        if (slot.bCenterHorizontally)
        {
            offsetX = CalculateCenteredX(scaledWidth) + slot.nOffsetX;
        }

        int offsetY = slot.nOffsetY;
        if (slot.bCenterVertically)
        {
            offsetY = CalculateCenteredY(scaledHeight) + slot.nOffsetY;
        }

        int finalX = offsetX + (int)(slot.nContentOffsetX * scale);
        int finalY = offsetY + (int)(slot.nContentOffsetY * scale);

        // Temporarily make the panel visible for painting
        bool bWasVisible = pPanel->IsVisible();
        pPanel->SetVisible(true);

        VRPanelVisibilityRestore_t restorePanels[2];
        int nRestoreCount = 0;
        if (V_strcmp(slot.szName, "CTFHudPlayerStatus") == 0)
        {
            nRestoreCount = HideStatusHUDUnsafeChildren(pPanel, restorePanels, ARRAYSIZE(restorePanels));
        }

        PaintPanelAtOffset(pPanel, finalX, finalY, scale);

        RestorePanelVisibility(restorePanels, nRestoreCount);

        // Restore visibility state
        pPanel->SetVisible(bWasVisible);
    }

    g_pMatSystemSurface->DisableClipping(false);
}

//-----------------------------------------------------------------------------
void CVRHUDCompositor::PaintPanelAtOffset(vgui::Panel* pPanel, int targetX, int targetY, float scale)
{
    if (!pPanel)
        return;

    // Get the panel's current absolute screen position
    int panelScreenX = 0, panelScreenY = 0;
    pPanel->LocalToScreen(panelScreenX, panelScreenY);

    // Calculate offset to move panel from its screen position to our target position
    // When rendering via DrawPanelIn3DSpace, the render target starts at (0,0)
    // so targetX/targetY are the absolute position we want within the render target
    int offsetX = targetX - panelScreenX;
    int offsetY = targetY - panelScreenY;

    // Apply the offset and paint
    vgui::surface()->ForceScreenPosOffset(true, offsetX, offsetY);
    vgui::surface()->PaintTraverse(pPanel->GetVPanel());
    vgui::surface()->ForceScreenPosOffset(false, 0, 0);
}

//=============================================================================
// CVRStatusHUDCompositor Implementation
//=============================================================================

DECLARE_BUILD_FACTORY(CVRStatusHUDCompositor);

//-----------------------------------------------------------------------------
CVRStatusHUDCompositor::CVRStatusHUDCompositor(vgui::Panel* parent, const char* name)
    : BaseClass(parent, name)
{
    m_pHealthPanel = nullptr;
    m_pObjectivePanel = nullptr;
    m_pMatchStatusPanel = nullptr;
    m_pMatchStatusElement = nullptr;
    m_nHealthSlotIndex = -1;
    m_nObjectiveSlotIndex = -1;
    m_nMatchStatusSlotIndex = -1;
}

//-----------------------------------------------------------------------------
bool CVRStatusHUDCompositor::Initialize()
{
    if (m_bInitialized)
        return true;

    RefreshHUDReferences();

    // Match status panel at top (team compositions + timer, dynamic - respects ShouldDraw)
    if (m_pMatchStatusPanel)
    {
        m_nMatchStatusSlotIndex = AddSlot(m_pMatchStatusPanel, m_pMatchStatusElement, "CTFHudMatchStatus", true);
    }

    // Health panel
    if (m_pHealthPanel)
    {
        m_nHealthSlotIndex = AddSlot(m_pHealthPanel, m_pHealthPanel, "CTFHudPlayerStatus", false);
    }

    // Objective panel below health
    if (m_pObjectivePanel)
    {
        m_nObjectiveSlotIndex = AddSlot(m_pObjectivePanel, m_pObjectivePanel, "CTFHudObjectiveStatus", false);
    }

    UpdateLayout();

    m_bInitialized = true;
    DevMsg("VR Status HUD Compositor: Initialized with %d HUD elements\n", m_nNumSlots);

    return true;
}

//-----------------------------------------------------------------------------
void CVRStatusHUDCompositor::RefreshHUDReferences()
{
    m_pHealthPanel = GET_HUDELEMENT(CTFHudPlayerStatus);
    m_pObjectivePanel = GET_HUDELEMENT(CTFHudObjectiveStatus);

    // Get match status HUD (team compositions + timer) by name lookup
    CHudElement* pElement = gHUD.FindElement("CTFHudMatchStatus");
    m_pMatchStatusElement = pElement;
    m_pMatchStatusPanel = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    if (m_nHealthSlotIndex >= 0)
        m_HudSlots[m_nHealthSlotIndex].pPanel = m_pHealthPanel;

    if (m_nObjectiveSlotIndex >= 0)
        m_HudSlots[m_nObjectiveSlotIndex].pPanel = m_pObjectivePanel;

    if (m_nMatchStatusSlotIndex >= 0)
    {
        m_HudSlots[m_nMatchStatusSlotIndex].pPanel = m_pMatchStatusPanel;
        m_HudSlots[m_nMatchStatusSlotIndex].pHudElement = m_pMatchStatusElement;
    }
}

//-----------------------------------------------------------------------------
void CVRStatusHUDCompositor::UpdateLayout()
{
    float sf = GetVRScreenScaleFactor();
    SetCompositorSize((int)(tfvr_status_hud_width.GetInt() * sf), (int)(tfvr_status_hud_height.GetInt() * sf));
    m_bShowDebugBackground = tfvr_status_hud_debug_bg.GetBool();

    // Match status panel at top (team compositions + timer)
    if (m_nMatchStatusSlotIndex >= 0)
    {
        VRHudElementSlot_t& slot = m_HudSlots[m_nMatchStatusSlotIndex];
        slot.bEnabled = tfvr_status_hud_matchstatus_enabled.GetBool();
        slot.nOffsetX = (int)(tfvr_status_hud_matchstatus_x.GetInt() * sf);
        slot.nOffsetY = (int)(tfvr_status_hud_matchstatus_y.GetInt() * sf);
        slot.bCenterHorizontally = tfvr_status_hud_matchstatus_center.GetBool();
        slot.bCenterVertically = false;
        slot.nContentOffsetX = (int)(tfvr_status_hud_matchstatus_content_offset_x.GetInt() * sf);
        slot.nContentOffsetY = (int)(tfvr_status_hud_matchstatus_content_offset_y.GetInt() * sf);
        slot.flScale = tfvr_status_hud_matchstatus_scale.GetFloat();
    }

    // Health panel
    if (m_nHealthSlotIndex >= 0)
    {
        VRHudElementSlot_t& slot = m_HudSlots[m_nHealthSlotIndex];
        slot.bEnabled = tfvr_status_hud_health_enabled.GetBool();
        slot.nOffsetX = (int)(tfvr_status_hud_health_x.GetInt() * sf);
        slot.nOffsetY = (int)(tfvr_status_hud_health_y.GetInt() * sf);
        slot.bCenterHorizontally = tfvr_status_hud_health_center.GetBool();
        slot.bCenterVertically = tfvr_status_hud_health_center_v.GetBool();
        slot.nContentOffsetX = (int)(tfvr_status_hud_health_content_offset_x.GetInt() * sf);
        slot.nContentOffsetY = (int)(tfvr_status_hud_health_content_offset_y.GetInt() * sf);
        slot.flScale = tfvr_status_hud_health_scale.GetFloat();
    }

    // Objective panel below health
    if (m_nObjectiveSlotIndex >= 0)
    {
        VRHudElementSlot_t& slot = m_HudSlots[m_nObjectiveSlotIndex];
        slot.bEnabled = tfvr_status_hud_objective_enabled.GetBool();
        slot.nOffsetX = (int)(tfvr_status_hud_objective_x.GetInt() * sf);
        slot.nOffsetY = (int)(tfvr_status_hud_objective_y.GetInt() * sf);
        slot.bCenterHorizontally = tfvr_status_hud_objective_center.GetBool();
        slot.bCenterVertically = tfvr_status_hud_objective_center_v.GetBool();
        slot.nContentOffsetX = (int)(tfvr_status_hud_objective_content_offset_x.GetInt() * sf);
        slot.nContentOffsetY = (int)(tfvr_status_hud_objective_content_offset_y.GetInt() * sf);
        slot.flScale = tfvr_status_hud_objective_scale.GetFloat();
    }
}

//=============================================================================
// CVRWeaponHUDCompositor Implementation
//=============================================================================

DECLARE_BUILD_FACTORY(CVRWeaponHUDCompositor);

//-----------------------------------------------------------------------------
CVRWeaponHUDCompositor::CVRWeaponHUDCompositor(vgui::Panel* parent, const char* name)
    : BaseClass(parent, name)
{
    m_pAmmoPanel = nullptr;
    m_pDemomanPipes = nullptr;
    m_pDemomanPipesElement = nullptr;
    m_pBowCharge = nullptr;
    m_pBowChargeElement = nullptr;
    m_pDemomanCharge = nullptr;
    m_pDemomanChargeElement = nullptr;
    m_pMedicCharge = nullptr;
    m_pMedicChargeElement = nullptr;
    m_pFlameRocketCharge = nullptr;
    m_pFlameRocketChargeElement = nullptr;
    m_pAccountPanel = nullptr;
    m_pAccountPanelElement = nullptr;

    m_nAmmoSlotIndex = -1;
    m_nDemomanPipesSlotIndex = -1;
    m_nBowChargeSlotIndex = -1;
    m_nDemomanChargeSlotIndex = -1;
    m_nMedicChargeSlotIndex = -1;
    m_nFlameRocketChargeSlotIndex = -1;
    m_nAccountPanelSlotIndex = -1;
    m_nFirstMeterSlotIndex = -1;
    m_nNumMeterSlots = 0;
}

//-----------------------------------------------------------------------------
bool CVRWeaponHUDCompositor::Initialize()
{
    if (m_bInitialized)
        return true;

    RefreshHUDReferences();

    // Add ammo panel first - dynamic so it respects ShouldDraw (hides on melee, etc.)
    if (m_pAmmoPanel)
    {
        m_nAmmoSlotIndex = AddSlot(m_pAmmoPanel, m_pAmmoPanel, "CTFHudWeaponAmmo", true);
    }

    // Add class-specific HUD elements (marked as dynamic so they respect ShouldDraw)
    if (m_pDemomanPipes)
    {
        m_nDemomanPipesSlotIndex = AddSlot(m_pDemomanPipes, m_pDemomanPipesElement, "CHudDemomanPipes", true);
    }

    if (m_pBowCharge)
    {
        m_nBowChargeSlotIndex = AddSlot(m_pBowCharge, m_pBowChargeElement, "CHudBowChargeMeter", true);
        // Bow charge meter layers on top of ammo
        m_HudSlots[m_nBowChargeSlotIndex].bLayerOnAmmo = true;
    }

    if (m_pDemomanCharge)
    {
        m_nDemomanChargeSlotIndex = AddSlot(m_pDemomanCharge, m_pDemomanChargeElement, "CHudDemomanChargeMeter", true);
        // Sticky charge meter layers on top of ammo
        m_HudSlots[m_nDemomanChargeSlotIndex].bLayerOnAmmo = true;
    }

    if (m_pMedicCharge)
    {
        m_nMedicChargeSlotIndex = AddSlot(m_pMedicCharge, m_pMedicChargeElement, "CHudMedicChargeMeter", true);
    }

    if (m_pFlameRocketCharge)
    {
        m_nFlameRocketChargeSlotIndex = AddSlot(m_pFlameRocketCharge, m_pFlameRocketChargeElement, "CHudFlameRocketChargeMeter", true);
    }

    if (m_pAccountPanel)
    {
        m_nAccountPanelSlotIndex = AddSlot(m_pAccountPanel, m_pAccountPanelElement, "CHudAccountPanel", true);
    }

    // Mark where dynamic meters start
    m_nFirstMeterSlotIndex = m_nNumSlots;

    // Gather item effect meters
    GatherItemEffectMeters();

    UpdateLayout();

    m_bInitialized = true;
    DevMsg("VR Weapon HUD Compositor: Initialized with %d HUD elements\n", m_nNumSlots);

    return true;
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDCompositor::RefreshHUDReferences()
{
    m_pAmmoPanel = GET_HUDELEMENT(CTFHudWeaponAmmo);

    // Get class-specific HUD elements by name lookup
    // These are registered with DECLARE_HUDELEMENT so we can find them
    // Store both the Panel pointer and CHudElement pointer for ShouldDraw checks
    CHudElement* pElement = nullptr;

    pElement = gHUD.FindElement("CHudDemomanPipes");
    m_pDemomanPipesElement = pElement;
    m_pDemomanPipes = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    pElement = gHUD.FindElement("CHudBowChargeMeter");
    m_pBowChargeElement = pElement;
    m_pBowCharge = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    pElement = gHUD.FindElement("CHudDemomanChargeMeter");
    m_pDemomanChargeElement = pElement;
    m_pDemomanCharge = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    pElement = gHUD.FindElement("CHudMedicChargeMeter");
    m_pMedicChargeElement = pElement;
    m_pMedicCharge = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    pElement = gHUD.FindElement("CHudFlameRocketChargeMeter");
    m_pFlameRocketChargeElement = pElement;
    m_pFlameRocketCharge = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    pElement = gHUD.FindElement("CHudAccountPanel");
    m_pAccountPanelElement = pElement;
    m_pAccountPanel = pElement ? dynamic_cast<vgui::Panel*>(pElement) : nullptr;

    // Update existing slot references (both panel and CHudElement)
    if (m_nAmmoSlotIndex >= 0)
    {
        m_HudSlots[m_nAmmoSlotIndex].pPanel = m_pAmmoPanel;
        m_HudSlots[m_nAmmoSlotIndex].pHudElement = m_pAmmoPanel; // CTFHudWeaponAmmo is a CHudElement
    }
    if (m_nDemomanPipesSlotIndex >= 0)
    {
        m_HudSlots[m_nDemomanPipesSlotIndex].pPanel = m_pDemomanPipes;
        m_HudSlots[m_nDemomanPipesSlotIndex].pHudElement = m_pDemomanPipesElement;
    }
    if (m_nBowChargeSlotIndex >= 0)
    {
        m_HudSlots[m_nBowChargeSlotIndex].pPanel = m_pBowCharge;
        m_HudSlots[m_nBowChargeSlotIndex].pHudElement = m_pBowChargeElement;
    }
    if (m_nDemomanChargeSlotIndex >= 0)
    {
        m_HudSlots[m_nDemomanChargeSlotIndex].pPanel = m_pDemomanCharge;
        m_HudSlots[m_nDemomanChargeSlotIndex].pHudElement = m_pDemomanChargeElement;
    }
    if (m_nMedicChargeSlotIndex >= 0)
    {
        m_HudSlots[m_nMedicChargeSlotIndex].pPanel = m_pMedicCharge;
        m_HudSlots[m_nMedicChargeSlotIndex].pHudElement = m_pMedicChargeElement;
    }
    if (m_nFlameRocketChargeSlotIndex >= 0)
    {
        m_HudSlots[m_nFlameRocketChargeSlotIndex].pPanel = m_pFlameRocketCharge;
        m_HudSlots[m_nFlameRocketChargeSlotIndex].pHudElement = m_pFlameRocketChargeElement;
    }
    if (m_nAccountPanelSlotIndex >= 0)
    {
        m_HudSlots[m_nAccountPanelSlotIndex].pPanel = m_pAccountPanel;
        m_HudSlots[m_nAccountPanelSlotIndex].pHudElement = m_pAccountPanelElement;
    }
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDCompositor::GatherItemEffectMeters()
{
    // Clear existing dynamic meter slots
    m_nNumMeterSlots = 0;

    // Iterate through all item effect meters via the auto list
    for (int i = 0; i < IHudItemEffectMeterAutoList::AutoList().Count(); i++)
    {
        CHudItemEffectMeter* pMeter = static_cast<CHudItemEffectMeter*>(IHudItemEffectMeterAutoList::AutoList()[i]);
        if (!pMeter)
            continue;

        // Only add if we have room
        if (m_nNumSlots >= MAX_HUD_SLOTS)
            break;

        // Store both the panel pointer and the CHudElement pointer for ShouldDraw checks
        int slotIndex = AddSlot(pMeter, pMeter, "ItemEffectMeter", true);
        if (slotIndex >= 0)
        {
            m_nNumMeterSlots++;
        }
    }
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDCompositor::RefreshDynamicElements()
{
    // Remove dynamic slots
    if (m_nFirstMeterSlotIndex >= 0)
    {
        m_nNumSlots = m_nFirstMeterSlotIndex;
        for (int i = m_nFirstMeterSlotIndex; i < MAX_HUD_SLOTS; i++)
        {
            m_HudSlots[i].pPanel = nullptr;
        }
    }

    // Re-gather effect meters
    GatherItemEffectMeters();
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDCompositor::UpdateLayout()
{
    float sf = GetVRScreenScaleFactor();
    VRWeaponHudProfile_t profile = ResolveWeaponHudProfile(GetActiveHudWeapon(), true);
    SetCompositorSize((int)(profile.placement.nWidth * sf), (int)(profile.placement.nHeight * sf));
    m_bShowDebugBackground = tfvr_weapon_hud_debug_bg.GetBool();

    // Ammo panel layout
    if (m_nAmmoSlotIndex >= 0)
    {
        VRHudElementSlot_t& slot = m_HudSlots[m_nAmmoSlotIndex];
        slot.bEnabled = profile.layout.bAmmoEnabled;
        slot.nOffsetX = (int)(profile.layout.nAmmoX * sf);
        slot.nOffsetY = (int)(profile.layout.nAmmoY * sf);
        slot.bCenterHorizontally = profile.layout.bAmmoCenter;
        slot.bCenterVertically = false;
        slot.nContentOffsetX = (int)(profile.layout.nAmmoContentOffsetX * sf);
        slot.nContentOffsetY = (int)(profile.layout.nAmmoContentOffsetY * sf);
        slot.flScale = profile.layout.flAmmoScale;
    }

    // Update meter layout (stacked horizontally below ammo)
    UpdateMeterLayout();
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDCompositor::UpdateMeterLayout()
{
    float sf = GetVRScreenScaleFactor();
    VRWeaponHudProfile_t profile = ResolveWeaponHudProfile(GetActiveHudWeapon(), true);
    int metersY = (int)(profile.layout.nMetersY * sf);
    int spacing = (int)(profile.layout.nMetersSpacing * sf);
    int rawWidthOverride = profile.layout.nMetersWidthOverride;
    int widthOverride = rawWidthOverride > 0 ? (int)(rawWidthOverride * sf) : 0;
    int contentOffsetX = (int)(profile.layout.nMetersContentOffsetX * sf);

    // Get ammo position for layered elements
    int ammoX = 0, ammoY = 0;
    if (m_nAmmoSlotIndex >= 0)
    {
        VRHudElementSlot_t& ammoSlot = m_HudSlots[m_nAmmoSlotIndex];
        ammoX = ammoSlot.nOffsetX + ammoSlot.nContentOffsetX;
        ammoY = ammoSlot.nOffsetY + ammoSlot.nContentOffsetY;
        if (ammoSlot.bCenterHorizontally)
        {
            vgui::Panel* pAmmoPanel = GetSlotPanel(ammoSlot);
            if (pAmmoPanel)
            {
                int ammoW, ammoH;
                pAmmoPanel->GetSize(ammoW, ammoH);
                ammoX = (m_nCompositorWidth - ammoW) / 2 + ammoSlot.nOffsetX + ammoSlot.nContentOffsetX;
            }
        }
    }

    // Build list of visible meter slots and their panels
    struct MeterInfo {
        int slotIndex;
        vgui::Panel* pPanel;
        int width;
        int height;
    };
    CUtlVector<MeterInfo> visibleMeters;
    int maxHeight = 0;

    // First pass: gather visible meters
    for (int i = 0; i < m_nNumSlots; i++)
    {
        VRHudElementSlot_t& slot = m_HudSlots[i];

        // Skip non-meter slots (ammo is handled separately)
        if (i == m_nAmmoSlotIndex)
            continue;

        // Skip elements that layer on ammo (they don't go in horizontal stack)
        if (slot.bLayerOnAmmo)
        {
            slot.bCenterHorizontally = false;
            slot.bCenterVertically = false;
            slot.nOffsetX = ammoX + (int)(profile.layout.nChargeOffsetX * sf);
            slot.nOffsetY = ammoY + (int)(profile.layout.nChargeOffsetY * sf);
            continue;
        }

        // Check if this slot should draw (handles ShouldDraw for dynamic elements)
        if (slot.bDynamic && !slot.ShouldDraw())
            continue;

        // Look up the panel fresh
        vgui::Panel* pPanel = GetSlotPanel(slot);
        if (!pPanel)
            continue;

        int panelWidth, panelHeight;
        pPanel->GetSize(panelWidth, panelHeight);

        if (panelWidth <= 0 || panelHeight <= 0)
            continue;

        MeterInfo info;
        info.slotIndex = i;
        info.pPanel = pPanel;
        info.width = panelWidth;
        info.height = panelHeight;
        visibleMeters.AddToTail(info);

        // Track the tallest meter for bottom alignment
        if (panelHeight > maxHeight)
            maxHeight = panelHeight;
    }

    // Calculate total width
    int totalWidth = 0;
    for (int i = 0; i < visibleMeters.Count(); i++)
    {
        int effectiveWidth = (widthOverride > 0) ? widthOverride : visibleMeters[i].width;
        if (i > 0)
            totalWidth += spacing;
        totalWidth += effectiveWidth;
    }

    // Second pass: position meters (in reverse order for right-to-left display)
    // All meters aligned at same Y (metersY) - vanilla uses top alignment
    int currentX = (m_nCompositorWidth - totalWidth) / 2;

    for (int i = visibleMeters.Count() - 1; i >= 0; i--)
    {
        MeterInfo& info = visibleMeters[i];
        VRHudElementSlot_t& slot = m_HudSlots[info.slotIndex];

        slot.bCenterHorizontally = false;
        slot.bCenterVertically = false;
        slot.nContentOffsetX = contentOffsetX;

        slot.nOffsetX = currentX;
        slot.nOffsetY = metersY;  // Same Y for all meters

        int effectiveWidth = (widthOverride > 0) ? widthOverride : info.width;
        currentX += effectiveWidth + spacing;
    }

    // Apply per-element offsets (metal/account panel can be repositioned independently)
    if (m_nAccountPanelSlotIndex >= 0)
    {
        VRHudElementSlot_t& accountSlot = m_HudSlots[m_nAccountPanelSlotIndex];
        accountSlot.nOffsetX += (int)(profile.layout.nAccountOffsetX * sf);
        accountSlot.nOffsetY += (int)(profile.layout.nAccountOffsetY * sf);
    }
}

//=============================================================================
// CVRHUDManager Base Implementation
//=============================================================================

//-----------------------------------------------------------------------------
CVRHUDManager::CVRHUDManager()
{
    m_bInitialized = false;
    m_bEnabled = true;
    m_vOffset.Init(0, 0, 0);
    m_angRotation.Init(0, 0, 0);
    m_flScale = 15.0f;
}

//-----------------------------------------------------------------------------
CVRHUDManager::~CVRHUDManager()
{
}

//-----------------------------------------------------------------------------
void CVRHUDManager::Shutdown()
{
    m_bInitialized = false;
}

//-----------------------------------------------------------------------------
void CVRHUDManager::Update()
{
    // Base class does nothing
}

//-----------------------------------------------------------------------------
void CVRHUDManager::ResetState()
{
    DevMsg("VR HUD Manager: Resetting state\n");
}

//=============================================================================
// CVRStatusHUDManager Implementation
//=============================================================================

//-----------------------------------------------------------------------------
CVRStatusHUDManager::CVRStatusHUDManager()
{
    m_nAttachedHand = 0;
    m_pCompositor = nullptr;
}

//-----------------------------------------------------------------------------
CVRStatusHUDManager::~CVRStatusHUDManager()
{
    Shutdown();
}

//-----------------------------------------------------------------------------
bool CVRStatusHUDManager::Initialize()
{
    if (m_bInitialized)
        return true;

    vgui::Panel* pParent = g_pClientMode->GetViewport();
    if (!pParent)
    {
        Warning("VR Status HUD Manager: Could not get client mode viewport\n");
        return false;
    }

    m_pCompositor = new CVRStatusHUDCompositor(pParent, "VRStatusHUDCompositor");
    if (!m_pCompositor)
    {
        Warning("VR Status HUD Manager: Could not create compositor panel\n");
        return false;
    }

    m_pCompositor->Initialize();
    m_pCompositor->SetVisible(false);

    m_bInitialized = true;
    DevMsg("VR Status HUD Manager: Initialized successfully\n");

    return true;
}

//-----------------------------------------------------------------------------
void CVRStatusHUDManager::Shutdown()
{
    if (m_pCompositor)
    {
        m_pCompositor->Shutdown();
        m_pCompositor->MarkForDeletion();
        m_pCompositor = nullptr;
    }

    CVRHUDManager::Shutdown();
}

//-----------------------------------------------------------------------------
void CVRStatusHUDManager::Update()
{
    if (!m_bInitialized || !m_pCompositor)
        return;

    m_bEnabled = tfvr_status_hud_enabled.GetBool();
    if (tfvr_status_hud_auto_hand.GetBool())
    {
        // Status HUD lives on the support/off-hand opposite the primary weapon hand.
        m_nAttachedHand = TFVR_IsLeftHanded() ? 1 : 0;
    }
    else
    {
        m_nAttachedHand = clamp(tfvr_status_hud_hand.GetInt(), 0, 1);
    }

    m_vOffset.Init(
        tfvr_status_hud_offset_x.GetFloat(),
        tfvr_status_hud_offset_y.GetFloat(),
        tfvr_status_hud_offset_z.GetFloat()
    );

    m_angRotation.Init(
        tfvr_status_hud_pitch.GetFloat(),
        tfvr_status_hud_yaw.GetFloat(),
        tfvr_status_hud_roll.GetFloat()
    );

    m_flScale = tfvr_status_hud_scale.GetFloat();
}

// Priority for hand HUDs (rendered close to player)
static const int PRIORITY_HAND_HUD = 200;

//-----------------------------------------------------------------------------
void CVRStatusHUDManager::Render()
{
    VPROF("VRStatusHUDManager_Render");

    if (!m_bInitialized || !m_bEnabled || !m_pCompositor)
        return;

    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer || pPlayer->IsObserver() || !pPlayer->IsAlive())
        return;

    VMatrix panelToWorld;
    if (!CalculateHandTransform(panelToWorld))
        return;

    int compositorWidth, compositorHeight;
    m_pCompositor->GetCompositorSize(compositorWidth, compositorHeight);

    float aspectRatio = (float)compositorWidth / (float)compositorHeight;
    float worldHeight = m_flScale;
    float worldWidth = worldHeight * aspectRatio;

    if (tfvr_status_hud_center_on_palm.GetBool())
    {
        ApplyCenteringOffset(panelToWorld, worldWidth, worldHeight);
    }

    // Skip if panel is facing away from camera
    if (IsPanelBackfacing(panelToWorld))
        return;

    // Queue for distance-sorted rendering
    if (g_pVRWorldUIQueue && g_pVRWorldUIQueue->IsInitialized())
    {
        bool bWasVisible = m_pCompositor->IsVisible();
        g_pVRWorldUIQueue->QueuePanel(m_pCompositor, panelToWorld,
                                      compositorWidth, compositorHeight,
                                      worldWidth, worldHeight,
                                      PRIORITY_HAND_HUD, true, bWasVisible);
    }
    else
    {
        // Fallback: render immediately
        m_pCompositor->SetVisible(true);
        g_pMatSystemSurface->DisableClipping(true);
        g_pMatSystemSurface->DrawPanelIn3DSpace(
            m_pCompositor->GetVPanel(),
            panelToWorld,
            compositorWidth,
            compositorHeight,
            worldWidth,
            worldHeight
        );
        g_pMatSystemSurface->DisableClipping(false);
        m_pCompositor->SetVisible(false);
    }
}

//-----------------------------------------------------------------------------
void CVRStatusHUDManager::SetHandAttachment(int hand)
{
    m_nAttachedHand = clamp(hand, 0, 1);
}

//-----------------------------------------------------------------------------
void CVRStatusHUDManager::ResetState()
{
    CVRHUDManager::ResetState();

    if (m_pCompositor)
    {
        m_pCompositor->Initialize();
    }
}

//-----------------------------------------------------------------------------
bool CVRStatusHUDManager::CalculateHandTransform(VMatrix& transform)
{
    if (tfvr_status_hud_use_hand_tracking.GetBool())
    {
        return CalculateHandTrackingTransform(transform);
    }
    else
    {
        return CalculateControllerTransform(transform);
    }
}

//-----------------------------------------------------------------------------
bool CVRStatusHUDManager::CalculateHandTrackingTransform(VMatrix& transform)
{
    if (!g_pOpenXRManager)
        return false;

    bool leftHand = (m_nAttachedHand == 0);
    Vector palmPosition;
    QAngle palmAngles;
    bool palmValid = false;

    // Try palm pose from action system (grip_surface or palm_ext)
    if (g_pOpenXRManager->IsPalmPoseSupported())
    {
        VMatrix palmMatrix;
        bool gotPalm = leftHand ?
            g_pOpenXRManager->GetLeftPalmPose(palmMatrix) :
            g_pOpenXRManager->GetRightPalmPose(palmMatrix);
        if (gotPalm)
        {
            palmPosition = palmMatrix.GetTranslation();
            MatrixAngles(palmMatrix.As3x4(), palmAngles);
            palmValid = true;
        }
    }

    // Fall back to hand tracking palm joint
    COpenXRHandTracker* handTracker = g_pOpenXRManager->GetHandTracker();
    if (!palmValid)
    {
        if (!handTracker)
            return false;
        bool handTracked = leftHand ? handTracker->IsLeftHandTracked() : handTracker->IsRightHandTracked();
        if (!handTracked)
            return false;
        if (!handTracker->GetHandJoint(leftHand, XR_HAND_JOINT_PALM_EXT, palmPosition, palmAngles))
            return false;
    }

    // Wrist from hand tracking (optional, for improved forward direction)
    Vector wristPosition;
    QAngle wristAngles;
    bool wristValid = handTracker &&
        handTracker->GetHandJoint(leftHand, XR_HAND_JOINT_WRIST_EXT, wristPosition, wristAngles);

    Vector handForward, handRight, handUp;
    if (wristValid)
    {
        handForward = (palmPosition - wristPosition).Normalized();
        AngleVectors(palmAngles, nullptr, &handRight, &handUp);
        if (!leftHand)
            handRight = -handRight;
    }
    else
    {
        AngleVectors(palmAngles, &handForward, &handRight, &handUp);
        if (!leftHand)
            handRight = -handRight;
    }

    Vector quadPosition = palmPosition;

    if (!tfvr_status_hud_center_on_palm.GetBool())
    {
        Vector backOfHandOffset = -handForward * 2.0f + handUp * 1.0f;
        quadPosition += backOfHandOffset;
    }

    Vector worldOffset = handRight * m_vOffset.x + handUp * m_vOffset.y + handForward * m_vOffset.z;
    quadPosition += worldOffset;

    transform.Identity();

    matrix3x4_t handMatrix;
    AngleMatrix(palmAngles, Vector(0, 0, 0), handMatrix);
    VMatrix handVMatrix;
    handVMatrix.CopyFrom3x4(handMatrix);
    transform = transform * handVMatrix;

    VMatrix baseRotMatrix;
    matrix3x4_t baseRot3x4;
    bool mirrorHandRotation = !leftHand;
    float baseRoll = tfvr_status_hud_base_roll.GetFloat();
    if (mirrorHandRotation)
        baseRoll = -baseRoll;

    QAngle baseRotation(
        tfvr_status_hud_base_pitch.GetFloat(),
        mirrorHandRotation ? -tfvr_status_hud_base_yaw.GetFloat() : tfvr_status_hud_base_yaw.GetFloat(),
        baseRoll
    );
    AngleMatrix(baseRotation, Vector(0, 0, 0), baseRot3x4);
    baseRotMatrix.CopyFrom3x4(baseRot3x4);
    transform = transform * baseRotMatrix;

    if (m_angRotation.x != 0 || m_angRotation.y != 0 || m_angRotation.z != 0)
    {
        QAngle adjustedRotation = m_angRotation;
        if (mirrorHandRotation)
        {
            adjustedRotation.y = -adjustedRotation.y;
            adjustedRotation.z = -adjustedRotation.z;
        }

        matrix3x4_t rotMatrix;
        AngleMatrix(adjustedRotation, Vector(0, 0, 0), rotMatrix);
        VMatrix rotVMatrix;
        rotVMatrix.CopyFrom3x4(rotMatrix);
        transform = transform * rotVMatrix;
    }

    if (mirrorHandRotation)
    {
        RotatePanelInPlane(transform, tfvr_status_hud_mirror_surface_roll.GetFloat());
    }

    transform.SetTranslation(quadPosition);

    return true;
}

//-----------------------------------------------------------------------------
bool CVRStatusHUDManager::CalculateControllerTransform(VMatrix& transform)
{
    if (!g_pOpenXRManager)
        return false;

    VMatrix handPose;
    bool handValid = false;

    if (m_nAttachedHand == 0)
    {
        if (g_pOpenXRManager->IsLeftControllerPoseValid())
            handValid = g_pOpenXRManager->GetLeftControllerGripPose(handPose);
    }
    else
    {
        if (g_pOpenXRManager->IsRightControllerPoseValid())
            handValid = g_pOpenXRManager->GetRightControllerGripPose(handPose);
    }

    if (!handValid)
        return false;

    Vector handPos = handPose.GetTranslation();
    Vector forward, right, up;
    handPose.GetBasisVectors(forward, right, up);
    if (m_nAttachedHand == 1)
        right = -right;

    Vector quadPos = handPos +
                     right * m_vOffset.x +
                     up * m_vOffset.y +
                     forward * m_vOffset.z;

    transform = handPose;
    transform.SetTranslation(quadPos);

    VMatrix adjustMatrix;
    matrix3x4_t adjustMatrix3x4;
    bool mirrorHandRotation = (m_nAttachedHand == 1);
    QAngle adjustAngles(
        -90,
        mirrorHandRotation ? 90 : -90,
        mirrorHandRotation ? -90 : 90
    );
    QAngle adjustedRotation = m_angRotation;
    if (mirrorHandRotation)
    {
        adjustedRotation.y = -adjustedRotation.y;
        adjustedRotation.z = -adjustedRotation.z;
    }
    adjustAngles += adjustedRotation;
    AngleMatrix(adjustAngles, Vector(0, 0, 0), adjustMatrix3x4);
    adjustMatrix.CopyFrom3x4(adjustMatrix3x4);

    transform = transform * adjustMatrix;
    if (mirrorHandRotation)
    {
        RotatePanelInPlane(transform, tfvr_status_hud_mirror_surface_roll.GetFloat());
    }

    return true;
}

//-----------------------------------------------------------------------------
void CVRStatusHUDManager::ApplyCenteringOffset(VMatrix& transform, float worldWidth, float worldHeight)
{
    Vector panelXAxis(transform[0][0], transform[1][0], transform[2][0]);
    Vector panelYAxis(transform[0][1], transform[1][1], transform[2][1]);

    Vector currentPos = transform.GetTranslation();
    Vector centeringOffset = -panelXAxis * (worldWidth * 0.5f) + panelYAxis * (worldHeight * 0.5f);
    transform.SetTranslation(currentPos + centeringOffset);
}

//=============================================================================
// CVRWeaponHUDManager Implementation
//=============================================================================

//-----------------------------------------------------------------------------
CVRWeaponHUDManager::CVRWeaponHUDManager()
{
    m_pCompositor = nullptr;
    m_hLastWeapon = nullptr;
}

//-----------------------------------------------------------------------------
CVRWeaponHUDManager::~CVRWeaponHUDManager()
{
    Shutdown();
}

//-----------------------------------------------------------------------------
bool CVRWeaponHUDManager::Initialize()
{
    if (m_bInitialized)
        return true;

    vgui::Panel* pParent = g_pClientMode->GetViewport();
    if (!pParent)
    {
        Warning("VR Weapon HUD Manager: Could not get client mode viewport\n");
        return false;
    }

    m_pCompositor = new CVRWeaponHUDCompositor(pParent, "VRWeaponHUDCompositor");
    if (!m_pCompositor)
    {
        Warning("VR Weapon HUD Manager: Could not create compositor panel\n");
        return false;
    }

    m_pCompositor->Initialize();
    m_pCompositor->SetVisible(false);

    m_bInitialized = true;
    DevMsg("VR Weapon HUD Manager: Initialized successfully\n");

    return true;
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDManager::Shutdown()
{
    if (m_pCompositor)
    {
        m_pCompositor->Shutdown();
        m_pCompositor->MarkForDeletion();
        m_pCompositor = nullptr;
    }

    CVRHUDManager::Shutdown();
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDManager::Update()
{
    if (!m_bInitialized || !m_pCompositor)
        return;

    m_bEnabled = tfvr_weapon_hud_enabled.GetBool();

    // Check for weapon change and refresh dynamic elements
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    C_TFWeaponBase* pTFWeapon = pPlayer ? pPlayer->GetActiveTFWeapon() : nullptr;
    VRWeaponHudProfile_t profile = ResolveWeaponHudProfile(pTFWeapon, true);

    m_vOffset = profile.placement.vOffset;
    m_angRotation = profile.placement.angRotation;
    m_flScale = profile.placement.flScale;

    if (pPlayer)
    {
        C_BaseCombatWeapon* pWeapon = pPlayer->GetActiveWeapon();

        if (pWeapon != m_hLastWeapon.Get())
        {
            m_hLastWeapon = pWeapon;
            m_pCompositor->RefreshDynamicElements();
        }
    }
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDManager::Render()
{
    VPROF("VRWeaponHUDManager_Render");

    if (!m_bInitialized || !m_bEnabled || !m_pCompositor)
        return;

    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    if (!pPlayer || pPlayer->IsObserver() || !pPlayer->IsAlive())
        return;

    // VR: Don't show weapon HUD if no weapon is equipped (e.g., during loser/stalemate)
    C_TFVRHand* pWeaponHand = GetCurrentWeaponHand();
    if (!pWeaponHand || !pWeaponHand->GetHeldWeapon())
        return;

    VMatrix panelToWorld;
    if (!CalculateWeaponBoneTransform(panelToWorld))
        return;

    int compositorWidth, compositorHeight;
    m_pCompositor->GetCompositorSize(compositorWidth, compositorHeight);

    float aspectRatio = (float)compositorWidth / (float)compositorHeight;
    float worldHeight = m_flScale;
    float worldWidth = worldHeight * aspectRatio;

    if (tfvr_weapon_hud_center.GetBool())
    {
        ApplyCenteringOffset(panelToWorld, worldWidth, worldHeight);
    }

    // Optional: mirrored weapon-bone frames can make the software normal test
    // disagree with DrawPanelIn3DSpace, so leave this off by default.
    if (tfvr_weapon_hud_backface_cull.GetBool() && IsPanelBackfacing(panelToWorld))
        return;

    // Queue for distance-sorted rendering
    if (g_pVRWorldUIQueue && g_pVRWorldUIQueue->IsInitialized())
    {
        bool bWasVisible = m_pCompositor->IsVisible();
        g_pVRWorldUIQueue->QueuePanel(m_pCompositor, panelToWorld,
                                      compositorWidth, compositorHeight,
                                      worldWidth, worldHeight,
                                      PRIORITY_HAND_HUD, true, bWasVisible);
    }
    else
    {
        // Fallback: render immediately
        m_pCompositor->SetVisible(true);
        g_pMatSystemSurface->DisableClipping(true);
        g_pMatSystemSurface->DrawPanelIn3DSpace(
            m_pCompositor->GetVPanel(),
            panelToWorld,
            compositorWidth,
            compositorHeight,
            worldWidth,
            worldHeight
        );
        g_pMatSystemSurface->DisableClipping(false);
        m_pCompositor->SetVisible(false);
    }
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDManager::ResetState()
{
    CVRHUDManager::ResetState();

    if (m_pCompositor)
    {
        m_pCompositor->Initialize();
        m_pCompositor->RefreshDynamicElements();
    }

    m_hLastWeapon = nullptr;
}

//-----------------------------------------------------------------------------
C_TFVRHand* CVRWeaponHUDManager::GetCurrentWeaponHand(C_TFWeaponBase** ppWeapon) const
{
    C_TFPlayer* pPlayer = C_TFPlayer::GetLocalTFPlayer();
    C_TFWeaponBase* pTFWeapon = pPlayer ? pPlayer->GetActiveTFWeapon() : nullptr;

    if (ppWeapon)
        *ppWeapon = pTFWeapon;

    return pTFWeapon ? TFVR_GetWeaponHand(pTFWeapon) : nullptr;
}

//-----------------------------------------------------------------------------
bool CVRWeaponHUDManager::ShouldMirrorWeaponHUDPlacement(const C_TFWeaponBase* pWeapon) const
{
    if (!pWeapon)
        return false;

    // Placement ConVars are tuned for ordinary right-primary weapons.
    // Whenever the HUD rides a left/off-hand weapon frame, mirror the user
    // offsets and yaw/roll so the panel stays on the same relative weapon side.
    return TFVR_DisplayWeaponOnLeft(pWeapon) || TFVR_WeaponPrefersOffHand(pWeapon);
}

//-----------------------------------------------------------------------------
bool CVRWeaponHUDManager::CalculateWeaponBoneTransform(VMatrix& transform)
{
    C_TFWeaponBase* pTFWeapon = nullptr;
    C_TFVRHand* pHand = GetCurrentWeaponHand(&pTFWeapon);
    if (!pHand)
        return false;

    bool bMirrorPlacement = ShouldMirrorWeaponHUDPlacement(pTFWeapon);
    bool bMirroredWeaponBone = pTFWeapon && TFVR_ShouldMirrorWeaponHand(pTFWeapon);

    bool bGotTransform = false;

    // Try 1: Use cached weapon bone transform from the hand
    matrix3x4_t cachedBoneMatrix;
    if (pHand->GetCachedWeaponBoneTransform(cachedBoneMatrix))
    {
        transform.CopyFrom3x4(cachedBoneMatrix);
        bGotTransform = true;
    }

    // Try 2: Fallback to render weapon's weapon_bone
    if (!bGotTransform)
    {
        C_BaseAnimating* pRenderWeapon = pHand->GetRenderWeapon();
        if (pRenderWeapon)
        {
            // The medigun is authored with a left-hand weapon bone; mirrored
            // right-hand weapons continue to expose the standard weapon_bone.
            int weaponBone = -1;
            if (TFVR_WeaponAuthoredHandIsLeft(pTFWeapon))
                weaponBone = pRenderWeapon->LookupBone("weapon_bone_L");
            if (weaponBone < 0)
                weaponBone = pRenderWeapon->LookupBone("weapon_bone");

            if (weaponBone >= 0)
            {
                matrix3x4_t boneMatrix;
                pRenderWeapon->GetBoneTransform(weaponBone, boneMatrix);
                transform.CopyFrom3x4(boneMatrix);
                bGotTransform = true;
            }
            else
            {
                Vector weaponPos = pRenderWeapon->GetAbsOrigin();
                QAngle weaponAngles = pRenderWeapon->GetAbsAngles();

                matrix3x4_t weaponMatrix;
                AngleMatrix(weaponAngles, weaponPos, weaponMatrix);
                transform.CopyFrom3x4(weaponMatrix);
                bGotTransform = true;
            }
        }
    }

    // Try 3: Fallback to hand position
    if (!bGotTransform)
    {
        Vector handPos = pHand->GetAbsOrigin();
        QAngle handAngles = pHand->GetAbsAngles();

        matrix3x4_t handMatrix;
        AngleMatrix(handAngles, handPos, handMatrix);
        transform.CopyFrom3x4(handMatrix);
        bGotTransform = true;
    }

    if (!bGotTransform)
        return false;

    // Apply user offsets in weapon space. For reflected weapon bones, mirror
    // the local offset as authored data before converting it through the
    // reflected bone frame; this matches the hand mirror instead of layering
    // an unrelated post-flip sign change on one axis.
    if (m_vOffset.x != 0 || m_vOffset.y != 0 || m_vOffset.z != 0)
    {
        Vector weaponForward, weaponRight, weaponUp;
        transform.GetBasisVectors(weaponForward, weaponRight, weaponUp);

        Vector localOffset = m_vOffset;
        if (bMirroredWeaponBone)
        {
            localOffset = MirrorWeaponHudLocalOffset(localOffset);
        }
        else if (bMirrorPlacement)
        {
            localOffset.x = -localOffset.x;
        }

        Vector worldOffset = weaponRight * localOffset.x +
                             weaponUp * localOffset.y +
                             weaponForward * localOffset.z;

        Vector currentPos = transform.GetTranslation();
        transform.SetTranslation(currentPos + worldOffset);
    }

    // Apply rotation adjustments.
    if (m_angRotation.x != 0 || m_angRotation.y != 0 || m_angRotation.z != 0)
    {
        QAngle adjustedRotation = m_angRotation;
        if (bMirrorPlacement)
        {
            adjustedRotation.y = -adjustedRotation.y;
            adjustedRotation.z = -adjustedRotation.z;
        }

        VMatrix rotationMatrix;
        matrix3x4_t rotMatrix;
        AngleMatrix(adjustedRotation, Vector(0, 0, 0), rotMatrix);
        rotationMatrix.CopyFrom3x4(rotMatrix);

        transform = transform * rotationMatrix;
    }

    if (bMirroredWeaponBone && tfvr_weapon_hud_mirrored_bone_roll.GetFloat() != 0.0f)
    {
        RotatePanelInPlane(transform, tfvr_weapon_hud_mirrored_bone_roll.GetFloat());
    }

    return true;
}

//-----------------------------------------------------------------------------
void CVRWeaponHUDManager::ApplyCenteringOffset(VMatrix& transform, float worldWidth, float worldHeight)
{
    Vector panelXAxis(transform[0][0], transform[1][0], transform[2][0]);
    Vector panelYAxis(transform[0][1], transform[1][1], transform[2][1]);

    Vector currentPos = transform.GetTranslation();
    Vector centeringOffset = -panelXAxis * (worldWidth * 0.5f) + panelYAxis * (worldHeight * 0.5f);
    transform.SetTranslation(currentPos + centeringOffset);
}
