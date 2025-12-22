// Purpose: VR Hand entity implementation

#include "cbase.h"
#include "c_tfvr_hand.h"
#include "tf/c_tf_player.h"
#include "tf/tf_weaponbase.h"
#include "tfvr/openxr_manager.h"
#include "tfvr/openxr_hand_tracking.h"
#include "tfvr/tfvr_weapon_base.h"
#include "bone_setup.h"
#include "engine/ivdebugoverlay.h"
#include "filesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// ConVars for debugging and control
ConVar tfvr_hands_enabled("tfvr_hands_enabled", "1", FCVAR_ARCHIVE, "Enable VR hand rendering");
ConVar tfvr_hands_debug("tfvr_hands_debug", "0", FCVAR_NONE, "Show debug info for VR hands");
ConVar tfvr_hands_alpha("tfvr_hands_alpha", "1.0", FCVAR_ARCHIVE, "Alpha transparency for VR hands (0-1)");
ConVar tfvr_hands_finger_tracking("tfvr_hands_finger_tracking", "1", FCVAR_ARCHIVE, "Enable finger tracking animation (0=disable, 1=enable)");
ConVar tfvr_hands_animate_thumb_metacarpal("tfvr_hands_animate_thumb_metacarpal", "0", FCVAR_ARCHIVE, "Animate thumb metacarpal bone (usually should be 0)");

// Finger rotation offset convars (to align OpenXR joint orientation with model bone orientation)
// Separate offsets for left and right hands since they're mirrored
ConVar tfvr_hands_finger_offset_pitch_L("tfvr_hands_finger_offset_pitch_L", "0", FCVAR_ARCHIVE, "Pitch offset for LEFT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_yaw_L("tfvr_hands_finger_offset_yaw_L", "0", FCVAR_ARCHIVE, "Yaw offset for LEFT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_roll_L("tfvr_hands_finger_offset_roll_L", "-90", FCVAR_ARCHIVE, "Roll offset for LEFT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_pitch_R("tfvr_hands_finger_offset_pitch_R", "0", FCVAR_ARCHIVE, "Pitch offset for RIGHT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_yaw_R("tfvr_hands_finger_offset_yaw_R", "0", FCVAR_ARCHIVE, "Yaw offset for RIGHT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_roll_R("tfvr_hands_finger_offset_roll_R", "-90", FCVAR_ARCHIVE, "Roll offset for RIGHT hand finger bones (degrees)");

// Rotation offset convars - left hand
ConVar tfvr_hands_left_offset_pitch("tfvr_hands_left_offset_pitch", "0", FCVAR_ARCHIVE, "Pitch offset for left VR hand (degrees)");
ConVar tfvr_hands_left_offset_yaw("tfvr_hands_left_offset_yaw", "0", FCVAR_ARCHIVE, "Yaw offset for left VR hand (degrees)");
ConVar tfvr_hands_left_offset_roll("tfvr_hands_left_offset_roll", "0", FCVAR_ARCHIVE, "Roll offset for left VR hand (degrees)");

// Rotation offset convars - right hand
ConVar tfvr_hands_right_offset_pitch("tfvr_hands_right_offset_pitch", "0", FCVAR_ARCHIVE, "Pitch offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_yaw("tfvr_hands_right_offset_yaw", "0", FCVAR_ARCHIVE, "Yaw offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_roll("tfvr_hands_right_offset_roll", "180", FCVAR_ARCHIVE, "Roll offset for right VR hand (degrees)");

// Debug convars
ConVar tfvr_debug_weapon_attachment("tfvr_debug_weapon_attachment", "0", FCVAR_NONE, "Draw debug lines showing weapon attachment");
ConVar tfvr_debug_weapon_position("tfvr_debug_weapon_position", "0", FCVAR_NONE, "Print weapon position updates to console");

// Weapon grip offset convars (for standard TF2 weapons without VR data)
ConVar tfvr_weapon_grip_offset_x("tfvr_weapon_grip_offset_x", "0", FCVAR_ARCHIVE, "Default weapon grip offset X (forward)");
ConVar tfvr_weapon_grip_offset_y("tfvr_weapon_grip_offset_y", "0", FCVAR_ARCHIVE, "Default weapon grip offset Y (right)");
ConVar tfvr_weapon_grip_offset_z("tfvr_weapon_grip_offset_z", "0", FCVAR_ARCHIVE, "Default weapon grip offset Z (up)");
ConVar tfvr_weapon_grip_angle_pitch("tfvr_weapon_grip_angle_pitch", "0", FCVAR_ARCHIVE, "Default weapon grip angle pitch");
ConVar tfvr_weapon_grip_angle_yaw("tfvr_weapon_grip_angle_yaw", "0", FCVAR_ARCHIVE, "Default weapon grip angle yaw");
ConVar tfvr_weapon_grip_angle_roll("tfvr_weapon_grip_angle_roll", "0", FCVAR_ARCHIVE, "Default weapon grip angle roll");

// Global storage for active VR hands - since we only support local player, use two pointers
static C_TFVRHand *g_pLocalPlayerLeftHand = NULL;
static C_TFVRHand *g_pLocalPlayerRightHand = NULL;

//-----------------------------------------------------------------------------
// Purpose: Get the hand model path for a specific class
//-----------------------------------------------------------------------------
const char* GetHandModelForClass(int playerClass, bool bLeftHand)
{
	const char *handSuffix = bLeftHand ? "_l" : "_r";
	
	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			return bLeftHand ? "models/weapons/vr_models/vr_scout_hand_l.mdl" : "models/weapons/vr_models/vr_scout_hand_r.mdl";
		case TF_CLASS_SOLDIER:
			return bLeftHand ? "models/weapons/vr_models/vr_soldier_hand_l.mdl" : "models/weapons/vr_models/vr_soldier_hand_r.mdl";
		case TF_CLASS_PYRO:
			return bLeftHand ? "models/weapons/vr_models/vr_pyro_hand_l.mdl" : "models/weapons/vr_models/vr_pyro_hand_r.mdl";
		case TF_CLASS_DEMOMAN:
			return bLeftHand ? "models/weapons/vr_models/vr_demo_hand_l.mdl" : "models/weapons/vr_models/vr_demo_hand_r.mdl";
		case TF_CLASS_HEAVYWEAPONS:
			return bLeftHand ? "models/weapons/vr_models/vr_heavy_hand_l.mdl" : "models/weapons/vr_models/vr_heavy_hand_r.mdl";
		case TF_CLASS_ENGINEER:
			return bLeftHand ? "models/weapons/vr_models/vr_engineer_hand_l.mdl" : "models/weapons/vr_models/vr_engineer_hand_r.mdl";
		case TF_CLASS_MEDIC:
			return bLeftHand ? "models/weapons/vr_models/vr_medic_hand_l.mdl" : "models/weapons/vr_models/vr_medic_hand_r.mdl";
		case TF_CLASS_SNIPER:
			return bLeftHand ? "models/weapons/vr_models/vr_sniper_hand_l.mdl" : "models/weapons/vr_models/vr_sniper_hand_r.mdl";
		case TF_CLASS_SPY:
			return bLeftHand ? "models/weapons/vr_models/vr_spy_hand_l.mdl" : "models/weapons/vr_models/vr_spy_hand_r.mdl";
		default:
			// Default to Scout if unknown class
			return bLeftHand ? "models/weapons/vr_models/vr_scout_hand_l.mdl" : "models/weapons/vr_models/vr_scout_hand_r.mdl";
	}
}

//-----------------------------------------------------------------------------
// Purpose: Get the fallback combined arms model for a class
//-----------------------------------------------------------------------------
const char* GetFallbackModelForClass(int playerClass)
{
	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			return "models/weapons/c_models/c_scout_arms.mdl";
		case TF_CLASS_SOLDIER:
			return "models/weapons/c_models/c_soldier_arms.mdl";
		case TF_CLASS_PYRO:
			return "models/weapons/c_models/c_pyro_arms.mdl";
		case TF_CLASS_DEMOMAN:
			return "models/weapons/c_models/c_demo_arms.mdl";
		case TF_CLASS_HEAVYWEAPONS:
			return "models/weapons/c_models/c_heavy_arms.mdl";
		case TF_CLASS_ENGINEER:
			return "models/weapons/c_models/c_engineer_arms.mdl";
		case TF_CLASS_MEDIC:
			return "models/weapons/c_models/c_medic_arms.mdl";
		case TF_CLASS_SNIPER:
			return "models/weapons/c_models/c_sniper_arms.mdl";
		case TF_CLASS_SPY:
			return "models/weapons/c_models/c_spy_arms.mdl";
		default:
			return "models/weapons/c_models/c_scout_arms.mdl";
	}
}

//-----------------------------------------------------------------------------
// Purpose: Global update function called every frame from VR menu manager
//-----------------------------------------------------------------------------
void UpdateVRHands()
{
	if (g_pLocalPlayerLeftHand)
	{
		g_pLocalPlayerLeftHand->Update();
	}
	if (g_pLocalPlayerRightHand)
	{
		g_pLocalPlayerRightHand->Update();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Clean up all VR hands (called on level shutdown)
//-----------------------------------------------------------------------------
void CleanupAllVRHands()
{
	if (g_pLocalPlayerLeftHand)
	{
		g_pLocalPlayerLeftHand->Shutdown();
		g_pLocalPlayerLeftHand->RemoveFromLeafSystem();
		g_pLocalPlayerLeftHand->SetRemovalFlag(true);
		delete g_pLocalPlayerLeftHand;
		g_pLocalPlayerLeftHand = NULL;
	}
	if (g_pLocalPlayerRightHand)
	{
		g_pLocalPlayerRightHand->Shutdown();
		g_pLocalPlayerRightHand->RemoveFromLeafSystem();
		g_pLocalPlayerRightHand->SetRemovalFlag(true);
		delete g_pLocalPlayerRightHand;
		g_pLocalPlayerRightHand = NULL;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Get the opposite hand
//-----------------------------------------------------------------------------
C_TFVRHand* GetOppositeVRHand(C_TFVRHand *pHand)
{
	if (!pHand)
		return NULL;
	
	if (pHand->IsLeftHand())
		return g_pLocalPlayerRightHand;
	else
		return g_pLocalPlayerLeftHand;
}

//-----------------------------------------------------------------------------
// Purpose: Accessors for the local player's hands
//-----------------------------------------------------------------------------
C_TFVRHand* GetLocalPlayerLeftHand()
{
	return g_pLocalPlayerLeftHand;
}

C_TFVRHand* GetLocalPlayerRightHand()
{
	return g_pLocalPlayerRightHand;
}

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
C_TFVRHand::C_TFVRHand()
{
	m_handSide = VR_HAND_LEFT; // Will be set in Initialize
	m_hOwnerPlayer = NULL;
	m_hHeldWeapon = NULL;
	m_pHandTracker = NULL;
	m_bHandTrackingValid = false;
	m_bBoneMappingSetup = false;
	m_bControllerTracked = false;
	m_bShuttingDown = false;
	m_iLastPlayerClass = TF_CLASS_UNDEFINED;
	m_vecLastValidPosition = vec3_origin;
	m_angLastValidAngles = vec3_angle;
	m_szModelName[0] = '\0';
	m_iHandBone = -1;

	// Initialize bone mapping to invalid
	for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
	{
		m_BoneMapping[i] = -1;
	}

	// This is a client-only entity
	AddEFlags(EFL_NO_GAME_PHYSICS_SIMULATION);
}

//-----------------------------------------------------------------------------
// Purpose: Destructor
//-----------------------------------------------------------------------------
C_TFVRHand::~C_TFVRHand()
{
	Shutdown();
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the hand entity (single hand)
//-----------------------------------------------------------------------------
bool C_TFVRHand::Initialize(C_TFPlayer *pOwner, VRHandSide handSide)
{
	if (!pOwner)
	{
		Warning("C_TFVRHand::Initialize - No owner player!\n");
		return false;
	}

	// Reset shutdown flag in case we're reinitializing
	m_bShuttingDown = false;
	
	m_hOwnerPlayer = pOwner;
	m_handSide = handSide;
	
	// Record current player class
	m_iLastPlayerClass = pOwner->GetPlayerClass()->GetClassIndex();

	// Get hand tracker from OpenXR manager
	if (g_pOpenXRManager)
	{
		m_pHandTracker = g_pOpenXRManager->GetHandTracker();
	}

	if (!m_pHandTracker)
	{
		Warning("C_TFVRHand::Initialize - No hand tracker available!\n");
		return false;
	}

	// Get class-specific hand model path
	const char *handModelPath = GetHandModelForClass(m_iLastPlayerClass, IsLeftHand());
	
	Q_strncpy(m_szModelName, handModelPath, sizeof(m_szModelName));
	
	Msg("VR Hand: Attempting to load model: %s\n", handModelPath);
	
	// Precache the model on client side
	int modelIndex = modelinfo->GetModelIndex(handModelPath);
	if (modelIndex == -1)
	{
		// Model not precached, try to precache it now
		Warning("VR Hand: Model not precached, attempting to precache: %s\n", handModelPath);
		CBaseEntity::PrecacheModel(handModelPath);
		modelIndex = modelinfo->GetModelIndex(handModelPath);
	}
	
	// Set a valid origin first (entities need to be in the world)
	SetAbsOrigin(pOwner->EyePosition());
	SetAbsAngles(vec3_angle);
	
	// Try using SetModel directly
	bool bCustomModelWorked = (modelIndex != -1) && SetModel(m_szModelName);
	
	if (!bCustomModelWorked)
	{
		Warning("VR Hand: Failed to load %s (model index: %d), trying fallback\n", handModelPath, modelIndex);
		
		// Use fallback to combined arms model for this class
		const char *fallbackModel = GetFallbackModelForClass(m_iLastPlayerClass);
		Q_strncpy(m_szModelName, fallbackModel, sizeof(m_szModelName));
		if (!SetModel(m_szModelName))
		{
			Warning("C_TFVRHand::Initialize - Failed to load hand model!\n");
			return false;
		}
		
		Msg("VR Hand (%s): Using fallback combined arms model: %s\n", IsLeftHand() ? "LEFT" : "RIGHT", fallbackModel);
	}
	else
	{
		Msg("VR Hand (%s): Successfully loaded separate hand model: %s\n", IsLeftHand() ? "LEFT" : "RIGHT", handModelPath);
	}
	
	// Verify model pointer is valid
	const model_t *pModel = GetModel();
	CStudioHdr *pStudioHdr = GetModelPtr();
	
	if (!pModel)
	{
		Warning("C_TFVRHand::Initialize - GetModel() returned NULL after SetModel!\n");
		return false;
	}
	
	if (!pStudioHdr)
	{
		Warning("C_TFVRHand::Initialize - GetModelPtr() returned NULL after SetModel!\n");
		// Don't fail here - the studio hdr might not be loaded yet
	}
	
	// Skip partition updates - this is a client-only entity that we update manually
	// UpdatePartitionListEntry() can crash for entities not in the networked entity list

	// Don't collide with anything
	SetSolid(SOLID_NONE);
	AddSolidFlags(FSOLID_NOT_SOLID);

	// Set up rendering - make sure entity is visible
	RemoveEffects(EF_NODRAW); // Make sure we're not hidden
	RemoveEffects(EF_NOSHADOW); // Make sure shadows are enabled
	SetRenderMode(kRenderTransTexture);
	SetRenderColor(255, 255, 255, 255 * tfvr_hands_alpha.GetFloat());
	
	// Mark as always drawing in opaque group (needed for shadows!)
	AddToLeafSystem(RENDER_GROUP_OPAQUE_ENTITY);
	
	// CRITICAL: Don't set an owner entity - this prevents shadow culling based on owner visibility
	// SetOwnerEntity(NULL);  // Make sure we don't have an owner
	
	// Explicitly create shadow
	DestroyShadow();  // Remove any existing shadow first
	CreateShadow();   // Create a new shadow handle

	// Note: We can't look up bones here because the model isn't fully initialized yet
	// Bone lookup will happen in SetupBoneMapping() on first frame
	m_iHandBone = -1;

	// Set to think every frame (do this after model is set)
	SetNextClientThink(CLIENT_THINK_ALWAYS);

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Clean up
//-----------------------------------------------------------------------------
void C_TFVRHand::Shutdown()
{
	m_bShuttingDown = true;
	
	// Unequip any held weapon
	UnequipWeapon();
	
	// Reset bone mapping so it gets recalculated on reinit
	m_bBoneMappingSetup = false;
	m_iHandBone = -1;
	
	m_hOwnerPlayer = NULL;
	m_pHandTracker = NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Spawn the entity
//-----------------------------------------------------------------------------
void C_TFVRHand::Spawn()
{
	BaseClass::Spawn();
	
	// Make sure we're set to think
	SetNextClientThink(CLIENT_THINK_ALWAYS);
}

//-----------------------------------------------------------------------------
// Purpose: Spawn VR hands for a player (two separate hand entities)
//-----------------------------------------------------------------------------
void C_TFVRHand::SpawnVRHands(C_TFPlayer *pPlayer)
{
	if (!pPlayer || !pPlayer->IsLocalPlayer())
		return;

	// Check if VR is active
	if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
		return;

	if (!tfvr_hands_enabled.GetBool())
		return;

	// If hands already exist, reinitialize them
	if (g_pLocalPlayerLeftHand && g_pLocalPlayerRightHand)
	{
		// Reinitialize with new player pointer
		g_pLocalPlayerLeftHand->Shutdown();
		g_pLocalPlayerRightHand->Shutdown();
		
		if (g_pLocalPlayerLeftHand->Initialize(pPlayer, VR_HAND_LEFT) &&
			g_pLocalPlayerRightHand->Initialize(pPlayer, VR_HAND_RIGHT))
		{
			g_pLocalPlayerLeftHand->Spawn();
			g_pLocalPlayerRightHand->Spawn();
			return;
		}
	}

	// Create new left hand entity
	C_TFVRHand *pLeftHand = new C_TFVRHand();
	if (pLeftHand && pLeftHand->Initialize(pPlayer, VR_HAND_LEFT))
	{
		pLeftHand->Spawn();
		g_pLocalPlayerLeftHand = pLeftHand;
	}
	else
	{
		Warning("VR Hands: Failed to create left hand!\n");
		if (pLeftHand)
			delete pLeftHand;
	}

	// Create new right hand entity
	C_TFVRHand *pRightHand = new C_TFVRHand();
	if (pRightHand && pRightHand->Initialize(pPlayer, VR_HAND_RIGHT))
	{
		pRightHand->Spawn();
		g_pLocalPlayerRightHand = pRightHand;
	}
	else
	{
		Warning("VR Hands: Failed to create right hand!\n");
		if (pRightHand)
			delete pRightHand;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Remove VR hands for a player (just hides, doesn't delete)
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveVRHands(C_TFPlayer *pPlayer)
{
	if (!pPlayer)
		return;

	if (g_pLocalPlayerLeftHand)
	{
		g_pLocalPlayerLeftHand->Shutdown();
		g_pLocalPlayerLeftHand->AddEffects(EF_NODRAW);
	}
	
	if (g_pLocalPlayerRightHand)
	{
		g_pLocalPlayerRightHand->Shutdown();
		g_pLocalPlayerRightHand->AddEffects(EF_NODRAW);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called every frame (if the entity is in the think list)
//-----------------------------------------------------------------------------
void C_TFVRHand::ClientThink()
{
	BaseClass::ClientThink();
	
	// VR: Update weapon position every frame with fresh tracking
	if (m_hRenderWeapon.Get())
	{
		// Get latest VR tracking
		UpdateHandTransform();
		
		// Update weapon position
		matrix3x4_t boneArray[MAXSTUDIOBONES];
		SetupBones(boneArray, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
	}
	
	// Invalidate bone cache to ensure fresh rendering
	InvalidateBoneCache();
	
	Update();
}

//-----------------------------------------------------------------------------
// Purpose: Manual update method (called directly)
//-----------------------------------------------------------------------------
void C_TFVRHand::Update()
{
	// Early out if shutting down
	if (m_bShuttingDown)
		return;

	// Validate we still have an owner
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner || !pOwner->IsLocalPlayer())
	{
		// Owner is gone, hide ourselves
		AddEffects(EF_NODRAW);
		return;
	}
	
	// Check if player class has changed - if so, hide hands temporarily to avoid crash
	int currentClass = pOwner->GetPlayerClass()->GetClassIndex();
	if (m_iLastPlayerClass != TF_CLASS_UNDEFINED && currentClass != m_iLastPlayerClass)
	{
		AddEffects(EF_NODRAW);
		m_bShuttingDown = true; // Prevent rendering until reinitialized
		m_iLastPlayerClass = currentClass;
		return;
	}
	m_iLastPlayerClass = currentClass;

	// Check if VR is still active
	if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
	{
		AddEffects(EF_NODRAW);
		return;
	}

	if (!tfvr_hands_enabled.GetBool())
	{
		AddEffects(EF_NODRAW);
		return;
	}

	RemoveEffects(EF_NODRAW);
	RemoveEffects(EF_NOSHADOW);
	
	// Force shadow updates every frame
	AddToLeafSystem(RENDER_GROUP_OPAQUE_ENTITY);
	MarkShadowDirty(true);

	// Update this hand's position and orientation
	UpdateHandTransform();

	// Update bone animation from hand tracking
	UpdateHandBones();
	
	// Update weapon position if we're holding one
	UpdateWeaponTransform();

	// Debug visualization
	if (tfvr_hands_debug.GetBool() && debugoverlay)
	{
		Vector handPos = m_vecLastValidPosition;
		QAngle handAngles = m_angLastValidAngles;
		
		int r = IsLeftHand() ? 0 : 255;
		int g = IsLeftHand() ? 255 : 0;
		
		debugoverlay->AddBoxOverlay(handPos, Vector(-2, -2, -2), Vector(2, 2, 2), 
			handAngles, r, g, 0, 100, 0.0f);
		debugoverlay->AddTextOverlay(handPos, 0.0f, "%s Hand\nTracked: %s", 
			IsLeftHand() ? "Left" : "Right",
			m_bControllerTracked ? "YES" : "NO");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Get wrist transform as a matrix (avoids gimbal lock)
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetWristTransform(VMatrix& outTransform)
{
	if (!m_pHandTracker)
		return false;
	
	// Get wrist position and angles
	Vector wristPos;
	QAngle wristAngles;
	
	if (!m_pHandTracker->GetHandJoint(IsLeftHand(), XR_HAND_JOINT_WRIST_EXT, wristPos, wristAngles))
		return false;
	
	// Convert to matrix
	matrix3x4_t temp;
	AngleMatrix(wristAngles, wristPos, temp);
	outTransform.CopyFrom3x4(temp);
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Update this hand's position from hand tracking wrist position
//          We'll position the hand bones via SetupBones override later
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateHandTransform()
{
	if (m_bShuttingDown)
		return;
		
	if (!g_pOpenXRManager || !m_pHandTracker)
		return;

	// Try to get wrist position using matrix (avoids gimbal lock)
	VMatrix wristMatrix;
	
	bool handValid = GetWristTransform(wristMatrix);
	
	// Update this hand if valid, fallback to controller
	if (handValid)
	{
		m_vecLastValidPosition = wristMatrix.GetTranslation();
		MatrixAngles(wristMatrix.As3x4(), m_angLastValidAngles);
		m_bControllerTracked = true;
	}
	else
	{
		// Fallback to controller pose
		VMatrix controllerPose;
		if (IsLeftHand())
			m_bControllerTracked = g_pOpenXRManager->GetLeftControllerPose(controllerPose);
		else
			m_bControllerTracked = g_pOpenXRManager->GetRightControllerPose(controllerPose);
		
		if (m_bControllerTracked)
		{
			m_vecLastValidPosition = controllerPose.GetTranslation();
			MatrixAngles(controllerPose.As3x4(), m_angLastValidAngles);
		}
	}

	// Position the entity at the hand position
	SetAbsOrigin(m_vecLastValidPosition);
	SetAbsAngles(m_angLastValidAngles);

	// Fade out if controller is not tracked
	if (!m_bControllerTracked)
	{
		SetRenderColor(255, 255, 255, 64); // Fade to 25%
	}
	else
	{
		SetRenderColor(255, 255, 255, 255 * tfvr_hands_alpha.GetFloat());
	}
}

//-----------------------------------------------------------------------------
// Purpose: Update bone transforms from hand tracking data
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateHandBones()
{
	if (!m_pHandTracker)
	{
		if (tfvr_hands_debug.GetBool())
			Warning("VR Hand: No hand tracker in UpdateHandBones\n");
		return;
	}

	// Check if hand tracking is active for this hand
	m_bHandTrackingValid = IsLeftHand() ? m_pHandTracker->IsLeftHandTracked() : m_pHandTracker->IsRightHandTracked();

	// Set up bone mapping if not done yet
	if (!m_bBoneMappingSetup)
	{
		SetupBoneMapping();
	}

	// Bone transforms are applied in SetupBones() override
}

//-----------------------------------------------------------------------------
// Purpose: Recursively append child bones to a list
//-----------------------------------------------------------------------------
static void AppendChildBones_R(CUtlVector<int> *pChildBones, CStudioHdr *pStudioHdr, int nBone)
{
	if (!pChildBones || !pStudioHdr)
		return;

	// Child bones have a larger bone index than their parent
	for (int i = nBone + 1; i < pStudioHdr->numbones(); ++i)
	{
		const mstudiobone_t *pBone = pStudioHdr->pBone(i);
		if (pBone && pBone->parent == nBone)
		{
			pChildBones->AddToTail(i);
			// Recurse to get all descendants
			AppendChildBones_R(pChildBones, pStudioHdr, i);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Override SetupBones to position hand bone at controller location
//-----------------------------------------------------------------------------
bool C_TFVRHand::SetupBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones, int boneMask, float currentTime)
{
	// Early out if shutting down
	if (m_bShuttingDown)
		return false;
	
	// Verify owner is still valid before doing anything
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner)
		return false;
	
	// Update hand transform NOW to get the most recent data (avoid 1-frame lag)
	UpdateHandTransform();
	
	// Let the base class set up the default bones first
	if (!BaseClass::SetupBones(pBoneToWorldOut, nMaxBones, boneMask, currentTime))
		return false;

	if (!pBoneToWorldOut)
		return true;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
		return true;
	
	// Safety check: if bone mapping isn't set up yet, try to set it up now
	if (!m_bBoneMappingSetup)
	{
		SetupBoneMapping();
		// If still not set up, just return (will try again next frame)
		if (!m_bBoneMappingSetup)
			return true;
	}
	
	// Safety check: validate bone index is still valid for this model
	int modelBoneCount = pStudioHdr->numbones();
	if (m_iHandBone >= modelBoneCount)
	{
		// Model changed, need to re-setup bone mapping
		Warning("VR Hand: Bone index invalid for current model, resetting\n");
		m_bBoneMappingSetup = false;
		m_iHandBone = -1;
		return true;
	}

	// Position hand bone at controller and update all children
	if (m_iHandBone >= 0 && m_iHandBone < nMaxBones && m_bControllerTracked)
	{
		// Store the original hand bone transform
		matrix3x4_t originalHandTransform;
		MatrixCopy(pBoneToWorldOut[m_iHandBone], originalHandTransform);

		// Start with the wrist transform
		matrix3x4_t wristTransform;
		AngleMatrix(m_angLastValidAngles, m_vecLastValidPosition, wristTransform);
		
		// Final transform after applying offsets
		matrix3x4_t newHandTransform;
		
		// Apply rotation offset as a local rotation (use appropriate hand offsets)
		ConVar *pOffsetPitch = IsLeftHand() ? &tfvr_hands_left_offset_pitch : &tfvr_hands_right_offset_pitch;
		ConVar *pOffsetYaw = IsLeftHand() ? &tfvr_hands_left_offset_yaw : &tfvr_hands_right_offset_yaw;
		ConVar *pOffsetRoll = IsLeftHand() ? &tfvr_hands_left_offset_roll : &tfvr_hands_right_offset_roll;
		
		if (pOffsetPitch->GetFloat() != 0 || pOffsetYaw->GetFloat() != 0 || pOffsetRoll->GetFloat() != 0)
		{
			matrix3x4_t offsetMatrix;
			QAngle offsetAngles(pOffsetPitch->GetFloat(), pOffsetYaw->GetFloat(), pOffsetRoll->GetFloat());
			AngleMatrix(offsetAngles, offsetMatrix);
			
			// Apply offset as local rotation: final = wrist * offset
			ConcatTransforms(wristTransform, offsetMatrix, newHandTransform);
		}
		else
		{
			MatrixCopy(wristTransform, newHandTransform);
		}
		
		// Apply the new transform to the bone
		MatrixCopy(newHandTransform, pBoneToWorldOut[m_iHandBone]);

		// Calculate the delta transform (from old to new)
		matrix3x4_t deltaTransform;
		matrix3x4_t inverseOriginal;
		MatrixInvert(originalHandTransform, inverseOriginal);
		ConcatTransforms(newHandTransform, inverseOriginal, deltaTransform);

		// Apply the delta transform to all child bones
		CUtlVector<int> vecChildBones;
		AppendChildBones_R(&vecChildBones, pStudioHdr, m_iHandBone);
		for (int i = 0; i < vecChildBones.Count(); ++i)
		{
			int iChildBone = vecChildBones[i];
			if (iChildBone >= 0 && iChildBone < nMaxBones)
			{
				matrix3x4_t originalChildTransform;
				MatrixCopy(pBoneToWorldOut[iChildBone], originalChildTransform);
				
				// Transform child bone by the delta
				ConcatTransforms(deltaTransform, originalChildTransform, pBoneToWorldOut[iChildBone]);
			}
		}
		
		// Apply finger tracking or weapon pose to this hand
		if (m_hHeldWeapon.Get())
		{
			// Override finger tracking with weapon grip pose
			ApplyWeaponPose(pBoneToWorldOut, nMaxBones);
			
			// IMPORTANT: Position weapon immediately after pose is applied
			// This ensures weapon_bone has the correct pose applied
			PositionWeaponFromBones(pBoneToWorldOut, nMaxBones);
		}
		else
		{
			// Use normal finger tracking
			ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Set up mapping between OpenXR joints and Source bones
//-----------------------------------------------------------------------------
void C_TFVRHand::SetupBoneMapping()
{
	// Check if model is loaded
	const model_t *pModel = GetModel();
	CStudioHdr *pStudioHdr = GetModelPtr();
	
	if (!pStudioHdr)
	{
		// Model not loaded yet, will try again next frame
		return;
	}
	
	if (!pStudioHdr->IsValid())
	{
		Warning("VR Hand: StudioHdr is not valid!\n");
		return;
	}

	int numBones = pStudioHdr->numbones();
	
	if (numBones <= 0 || numBones > 256)
	{
		Warning("VR Hand: Invalid bone count: %d\n", numBones);
		return;
	}

	// Find the hand bone in the model
	// Try common bone names for TF2 viewmodel arms
	const char* handSuffix = IsLeftHand() ? "_L" : "_R";
	const char* handSuffixLower = IsLeftHand() ? "_l" : "_r";
	
	const char* boneNames[4];
	boneNames[0] = IsLeftHand() ? "bip_hand_L" : "bip_hand_R";
	boneNames[1] = IsLeftHand() ? "weapon_bone_L" : "weapon_bone_R";
	boneNames[2] = IsLeftHand() ? "ValveBiped.Bip01_L_Hand" : "ValveBiped.Bip01_R_Hand";
	boneNames[3] = IsLeftHand() ? "bip_hand_l" : "bip_hand_r";

	// Try to find hand bone
	for (int i = 0; i < 4; i++)
	{
		m_iHandBone = LookupBone(boneNames[i]);
		if (m_iHandBone != -1)
			break;
	}

	if (m_iHandBone == -1)
	{
		Warning("VR Hand (%s): Could not find hand bone!\n", IsLeftHand() ? "LEFT" : "RIGHT");
	}

	// Map finger bones for hand tracking animation
	// OpenXR joint order: metacarpal (0), proximal (1), intermediate/middle (2), distal (3), tip (4)
	// TF2 bone naming: bip_<finger>_0_<L/R>, bip_<finger>_1_<L/R>, bip_<finger>_2_<L/R>
	
	char boneName[64];
	
	// Thumb (OpenXR has 4 joints: metacarpal, proximal, distal, tip)
	Q_snprintf(boneName, sizeof(boneName), "bip_thumb_0%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_THUMB_METACARPAL_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_thumb_1%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_THUMB_PROXIMAL_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_thumb_2%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_THUMB_DISTAL_EXT] = LookupBone(boneName);
	m_BoneMapping[XR_HAND_JOINT_THUMB_TIP_EXT] = -1; // No tip bone in model
	
	// Index finger
	m_BoneMapping[XR_HAND_JOINT_INDEX_METACARPAL_EXT] = -1; // Usually not animated
	Q_snprintf(boneName, sizeof(boneName), "bip_index_0%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_INDEX_PROXIMAL_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_index_1%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_index_2%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_INDEX_DISTAL_EXT] = LookupBone(boneName);
	m_BoneMapping[XR_HAND_JOINT_INDEX_TIP_EXT] = -1;
	
	// Middle finger
	m_BoneMapping[XR_HAND_JOINT_MIDDLE_METACARPAL_EXT] = -1;
	Q_snprintf(boneName, sizeof(boneName), "bip_middle_0%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_middle_1%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_middle_2%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_MIDDLE_DISTAL_EXT] = LookupBone(boneName);
	m_BoneMapping[XR_HAND_JOINT_MIDDLE_TIP_EXT] = -1;
	
	// Ring finger
	m_BoneMapping[XR_HAND_JOINT_RING_METACARPAL_EXT] = -1;
	Q_snprintf(boneName, sizeof(boneName), "bip_ring_0%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_RING_PROXIMAL_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_ring_1%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_RING_INTERMEDIATE_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_ring_2%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_RING_DISTAL_EXT] = LookupBone(boneName);
	m_BoneMapping[XR_HAND_JOINT_RING_TIP_EXT] = -1;
	
	// Pinky finger
	m_BoneMapping[XR_HAND_JOINT_LITTLE_METACARPAL_EXT] = -1;
	Q_snprintf(boneName, sizeof(boneName), "bip_pinky_0%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_LITTLE_PROXIMAL_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_pinky_1%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT] = LookupBone(boneName);
	Q_snprintf(boneName, sizeof(boneName), "bip_pinky_2%s", handSuffix);
	m_BoneMapping[XR_HAND_JOINT_LITTLE_DISTAL_EXT] = LookupBone(boneName);
	m_BoneMapping[XR_HAND_JOINT_LITTLE_TIP_EXT] = -1;

	m_bBoneMappingSetup = true;
}

//-----------------------------------------------------------------------------
// Purpose: Map an OpenXR joint to a Source bone index
//-----------------------------------------------------------------------------
bool C_TFVRHand::MapOpenXRJointToBone(XrHandJointEXT joint, int &boneIndex)
{
	if (joint < 0 || joint >= XR_HAND_JOINT_COUNT_EXT)
		return false;

	boneIndex = m_BoneMapping[joint];
	
	return (boneIndex >= 0);
}

//-----------------------------------------------------------------------------
// Purpose: Apply finger tracking rotations to bone transforms
//-----------------------------------------------------------------------------
void C_TFVRHand::ApplyFingerTracking(matrix3x4_t *pBoneToWorldOut, int nMaxBones)
{
	if (!tfvr_hands_finger_tracking.GetBool())
		return;
		
	if (!m_pHandTracker)
		return;
	
	// Check if this hand is being tracked
	if (!m_bHandTrackingValid)
		return;
	
	// Get the bone mapping for this hand
	int *boneMapping = m_BoneMapping;
	
	CStudioHdr *pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
		return;
	
	// List of finger joints we want to animate (excluding tips and metacarpals which often aren't in the model)
	XrHandJointEXT fingerJoints[] = {
		// Thumb - metacarpal is optional (controlled by convar)
		XR_HAND_JOINT_THUMB_PROXIMAL_EXT,
		XR_HAND_JOINT_THUMB_DISTAL_EXT,
		// Index
		XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
		XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,
		XR_HAND_JOINT_INDEX_DISTAL_EXT,
		// Middle
		XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,
		XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT,
		XR_HAND_JOINT_MIDDLE_DISTAL_EXT,
		// Ring
		XR_HAND_JOINT_RING_PROXIMAL_EXT,
		XR_HAND_JOINT_RING_INTERMEDIATE_EXT,
		XR_HAND_JOINT_RING_DISTAL_EXT,
		// Pinky
		XR_HAND_JOINT_LITTLE_PROXIMAL_EXT,
		XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT,
		XR_HAND_JOINT_LITTLE_DISTAL_EXT,
	};
	
	// Optionally animate thumb metacarpal
	if (tfvr_hands_animate_thumb_metacarpal.GetBool())
	{
		// Process thumb metacarpal separately
		int thumbMetacarpalBone = boneMapping[XR_HAND_JOINT_THUMB_METACARPAL_EXT];
		if (thumbMetacarpalBone >= 0 && thumbMetacarpalBone < nMaxBones)
		{
			Vector jointPos;
			QAngle jointAngles;
			if (m_pHandTracker->GetHandJoint(IsLeftHand(), XR_HAND_JOINT_THUMB_METACARPAL_EXT, jointPos, jointAngles))
			{
				const mstudiobone_t *pBone = pStudioHdr->pBone(thumbMetacarpalBone);
				if (pBone)
				{
					int parentIndex = pBone->parent;
					if (parentIndex >= 0 && parentIndex < nMaxBones)
					{
						Vector defaultLocalPos = pBone->pos;
						
						QAngle fingerOffset;
						if (IsLeftHand())
						{
							fingerOffset.x = tfvr_hands_finger_offset_pitch_L.GetFloat();
							fingerOffset.y = tfvr_hands_finger_offset_yaw_L.GetFloat();
							fingerOffset.z = tfvr_hands_finger_offset_roll_L.GetFloat();
						}
						else
						{
							fingerOffset.x = tfvr_hands_finger_offset_pitch_R.GetFloat();
							fingerOffset.y = tfvr_hands_finger_offset_yaw_R.GetFloat();
							fingerOffset.z = tfvr_hands_finger_offset_roll_R.GetFloat();
						}
						
						matrix3x4_t parentInverse;
						MatrixInvert(pBoneToWorldOut[parentIndex], parentInverse);
						
						matrix3x4_t trackedWorld;
						AngleMatrix(jointAngles, trackedWorld);
						
						matrix3x4_t trackedLocal;
						ConcatTransforms(parentInverse, trackedWorld, trackedLocal);
						
						matrix3x4_t offsetRotation;
						AngleMatrix(fingerOffset, offsetRotation);
						
						matrix3x4_t localRotation;
						ConcatTransforms(trackedLocal, offsetRotation, localRotation);
						
						MatrixSetColumn(defaultLocalPos, 3, localRotation);
						
						ConcatTransforms(pBoneToWorldOut[parentIndex], localRotation, pBoneToWorldOut[thumbMetacarpalBone]);
					}
				}
			}
		}
	}
	
	// Apply rotation for each finger joint
	for (int i = 0; i < ARRAYSIZE(fingerJoints); i++)
	{
		XrHandJointEXT joint = fingerJoints[i];
		int boneIndex = boneMapping[joint];
		
		// Skip if this joint doesn't map to a bone
		if (boneIndex < 0 || boneIndex >= nMaxBones)
			continue;
		
		// Get the joint's world-space pose from hand tracking
		Vector jointPos;
		QAngle jointAngles;
		if (m_pHandTracker->GetHandJoint(IsLeftHand(), joint, jointPos, jointAngles))
		{
			// Get the parent bone's transform
			const mstudiobone_t *pBone = pStudioHdr->pBone(boneIndex);
			if (!pBone)
				continue;
			
			int parentIndex = pBone->parent;
			if (parentIndex < 0 || parentIndex >= nMaxBones)
				continue;
			
			// Get the bone's default position relative to its parent (from the skeleton)
			Vector defaultLocalPos = pBone->pos;
			
			// Get hand-specific finger offset
			QAngle fingerOffset;
			if (IsLeftHand())
			{
				fingerOffset.x = tfvr_hands_finger_offset_pitch_L.GetFloat();
				fingerOffset.y = tfvr_hands_finger_offset_yaw_L.GetFloat();
				fingerOffset.z = tfvr_hands_finger_offset_roll_L.GetFloat();
			}
			else
			{
				fingerOffset.x = tfvr_hands_finger_offset_pitch_R.GetFloat();
				fingerOffset.y = tfvr_hands_finger_offset_yaw_R.GetFloat();
				fingerOffset.z = tfvr_hands_finger_offset_roll_R.GetFloat();
			}
			
			// Get parent's inverse to convert world rotation to local
			matrix3x4_t parentInverse;
			MatrixInvert(pBoneToWorldOut[parentIndex], parentInverse);
			
			// Create the tracked world-space rotation matrix
			matrix3x4_t trackedWorld;
			AngleMatrix(jointAngles, trackedWorld);
			
			// Convert to local space relative to parent
			matrix3x4_t trackedLocal;
			ConcatTransforms(parentInverse, trackedWorld, trackedLocal);
			
			// Create the offset rotation matrix
			matrix3x4_t offsetRotation;
			AngleMatrix(fingerOffset, offsetRotation);
			
			// Apply offset as a local rotation: final = tracked * offset
			matrix3x4_t localRotation;
			ConcatTransforms(trackedLocal, offsetRotation, localRotation);
			
			// Set the default local position but keep the rotation from tracking
			MatrixSetColumn(defaultLocalPos, 3, localRotation);
			
			// Transform by parent to get world-space transform
			ConcatTransforms(pBoneToWorldOut[parentIndex], localRotation, pBoneToWorldOut[boneIndex]);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Hide the opposite hand by scaling its root bone to zero
//-----------------------------------------------------------------------------
void C_TFVRHand::HideOppositeHand(matrix3x4_t *pBoneToWorldOut, int nMaxBones, CStudioHdr *pStudioHdr)
{
	if (!pStudioHdr)
		return;
	
	// Determine which hand suffix to hide (opposite of current hand)
	const char *oppositeHandSuffix = IsLeftHand() ? "_R" : "_L";
	int suffixLen = Q_strlen(oppositeHandSuffix);
	
	// Find and hide ALL bones that belong to the opposite hand
	// We'll search for bone names that END with the opposite hand suffix
	int modelBoneCount = pStudioHdr->numbones();
	
	for (int i = 0; i < modelBoneCount && i < nMaxBones; i++)
	{
		const char *boneName = pStudioHdr->pBone(i)->pszName();
		int nameLen = Q_strlen(boneName);
		
		// Check if this bone name ends with the opposite hand suffix
		if (nameLen >= suffixLen && 
		    Q_stricmp(boneName + nameLen - suffixLen, oppositeHandSuffix) == 0)
		{
			// Scale this bone to zero
			matrix3x4_t &boneMatrix = pBoneToWorldOut[i];
			
			// Set scale to zero by zeroing out the basis vectors
			boneMatrix[0][0] = 0.0f;
			boneMatrix[0][1] = 0.0f;
			boneMatrix[0][2] = 0.0f;
			
			boneMatrix[1][0] = 0.0f;
			boneMatrix[1][1] = 0.0f;
			boneMatrix[1][2] = 0.0f;
			
			boneMatrix[2][0] = 0.0f;
			boneMatrix[2][1] = 0.0f;
			boneMatrix[2][2] = 0.0f;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Position weapon using bone matrices from SetupBones
//          Called during SetupBones after pose is applied to weapon_bone
//-----------------------------------------------------------------------------
void C_TFVRHand::PositionWeaponFromBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones)
{
	// Position the RENDER weapon based on hand's weapon_bone
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	if (!pRenderWeapon || !pBoneToWorldOut)
		return;
	
	Vector weaponPos;
	QAngle weaponAng;
	
	// Get the hand's weapon_bone world transform
	int handWeaponBone = LookupBone("weapon_bone");
	
	if (handWeaponBone >= 0 && handWeaponBone < nMaxBones)
	{
		// Get weapon_bone transform from the computed bone matrices
		matrix3x4_t handWeaponBoneMatrix;
		MatrixCopy(pBoneToWorldOut[handWeaponBone], handWeaponBoneMatrix);
		
		// Extract position and angles
		Vector bonePos;
		QAngle boneAng;
		MatrixAngles(handWeaponBoneMatrix, boneAng, bonePos);
		
		// Check if weapon has a weapon_bone we need to align
		int weaponWeaponBone = pRenderWeapon->LookupBone("weapon_bone");
		
		if (weaponWeaponBone >= 0)
		{
			// Get weapon_bone position in MODEL SPACE
			CStudioHdr *pWeaponHdr = pRenderWeapon->GetModelPtr();
			if (pWeaponHdr)
			{
				mstudiobone_t *pWeaponBone = pWeaponHdr->pBone(weaponWeaponBone);
				if (pWeaponBone)
				{
					Vector weaponBonePos = pWeaponBone->pos;
					QAngle weaponBoneAng;
					QuaternionAngles(pWeaponBone->quat, weaponBoneAng);
					
					// Build weapon_bone transform
					matrix3x4_t weaponBoneMatrix;
					AngleMatrix(weaponBoneAng, weaponBonePos, weaponBoneMatrix);
					
					// Invert to get transform from weapon_bone space to weapon origin
					matrix3x4_t weaponBoneInverse;
					MatrixInvert(weaponBoneMatrix, weaponBoneInverse);
					
					// Apply: weapon_origin = hand_weapon_bone * weapon_bone_inverse
					matrix3x4_t weaponTransform;
					ConcatTransforms(handWeaponBoneMatrix, weaponBoneInverse, weaponTransform);
					
					MatrixAngles(weaponTransform, weaponAng, weaponPos);
				}
				else
				{
					weaponPos = bonePos;
					weaponAng = boneAng;
				}
			}
			else
			{
				weaponPos = bonePos;
				weaponAng = boneAng;
			}
		}
		else
		{
		// No weapon_bone, use hand's weapon_bone directly
		weaponPos = bonePos;
		weaponAng = boneAng;
	}
	
	// Apply user adjustments in local weapon space
	Vector userOffset(
		tfvr_weapon_grip_offset_x.GetFloat(),
		tfvr_weapon_grip_offset_y.GetFloat(),
		tfvr_weapon_grip_offset_z.GetFloat()
	);
	QAngle userAngles(
		tfvr_weapon_grip_angle_pitch.GetFloat(),
		tfvr_weapon_grip_angle_yaw.GetFloat(),
		tfvr_weapon_grip_angle_roll.GetFloat()
	);
	
	if (userOffset.x != 0 || userOffset.y != 0 || userOffset.z != 0 ||
	    userAngles.x != 0 || userAngles.y != 0 || userAngles.z != 0)
	{
		// Build current weapon transform
		matrix3x4_t weaponTransform;
		AngleMatrix(weaponAng, weaponPos, weaponTransform);
		
		// Build offset matrix in local space
		matrix3x4_t offsetMatrix;
		AngleMatrix(userAngles, userOffset, offsetMatrix);
		
		// Apply offset: final = current * offset (local space)
		matrix3x4_t finalTransform;
		ConcatTransforms(weaponTransform, offsetMatrix, finalTransform);
		
		// Extract final position and angles
		MatrixAngles(finalTransform, weaponAng, weaponPos);
	}
}
	else
	{
		// No weapon_bone on hand, use hand origin
		weaponPos = GetAbsOrigin();
		weaponAng = GetAbsAngles();
	}
	
	// Apply the position to the render weapon
	pRenderWeapon->SetAbsOrigin(weaponPos);
	pRenderWeapon->SetAbsAngles(weaponAng);
}

//-----------------------------------------------------------------------------
// Purpose: Get the weapon's muzzle position and angles in world space
//          Returns false if no weapon is held or muzzle can't be determined
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetWeaponMuzzlePositionAndAngles(Vector &outPos, QAngle &outAngles)
{
	// Use the RENDER weapon for position calculations
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	if (!pRenderWeapon)
		return false;
	
	// CRITICAL: Get the absolute LATEST VR tracking data RIGHT NOW!
	// This ensures we have the most up-to-date hand position
	UpdateHandTransform();
	
	// CRITICAL: Update the render weapon position RIGHT NOW before getting muzzle
	// This ensures we have the latest position based on fresh tracking data
	matrix3x4_t boneArray[MAXSTUDIOBONES];
	SetupBones(boneArray, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
	// SetupBones calls PositionWeaponFromBones which updates render weapon position
	
	// Force the weapon to update its bone matrices based on the position we set
	pRenderWeapon->SetupBones(NULL, -1, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
	
	// Now use the standard GetAttachment - it will work correctly!
	int iMuzzle = pRenderWeapon->LookupAttachment("muzzle");
	if (iMuzzle > 0 && pRenderWeapon->GetAttachment(iMuzzle, outPos, outAngles))
	{
		return true;
	}
	
	// Fallback: no muzzle attachment found, use weapon's forward direction
	outPos = pRenderWeapon->GetAbsOrigin();
	outAngles = pRenderWeapon->GetAbsAngles();
	
	Vector forward, up;
	AngleVectors(outAngles, &forward, NULL, &up);
	outPos += forward * 15.0f + up * 2.0f;
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Get the appropriate hand animation name for a weapon
//-----------------------------------------------------------------------------
const char* GetWeaponPoseAnimation(int playerClass, const char *weaponClass)
{
	// Default fallback
	const char *defaultAnim = "ref";
	
	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			// Scout animations: sg_idle (scattergun), p_idle (pistol), b_idle (bat)
			if (V_stristr(weaponClass, "scattergun")) return "sg_idle";
			if (V_stristr(weaponClass, "pistol")) return "p_idle";
			if (V_stristr(weaponClass, "bat")) return "b_idle";
			if (V_stristr(weaponClass, "wrap")) return "wb_idle"; // Wrap Assassin
			break;
			
		case TF_CLASS_SOLDIER:
			// Soldier: dh_idle (rockets), idle (shotgun), s_idle (shovel)
			if (V_stristr(weaponClass, "rocketlauncher")) return "dh_idle";
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "shovel")) return "s_idle";
			if (V_stristr(weaponClass, "pickaxe")) return "s_idle"; // Equalizer
			if (V_stristr(weaponClass, "buff_item")) return "bb_idle"; // Buff Banner
			break;
			
		case TF_CLASS_PYRO:
			// Pyro: ft_idle (flamethrower), fg_idle (flare gun), fa_idle (fire axe)
			if (V_stristr(weaponClass, "flamethrower")) return "ft_idle";
			if (V_stristr(weaponClass, "flaregun")) return "fg_idle";
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "fireaxe")) return "fa_idle";
			if (V_stristr(weaponClass, "slap")) return "fa_idle"; // Hot Hand
			break;
			
		case TF_CLASS_DEMOMAN:
			// Demo: g_idle (grenade launcher), sb_idle (stickybomb), b_idle (bottle), cm_idle (sword)
			if (V_stristr(weaponClass, "grenadelauncher")) return "g_idle";
			if (V_stristr(weaponClass, "pipebomblauncher")) return "sb_idle";
			if (V_stristr(weaponClass, "bottle")) return "b_idle";
			if (V_stristr(weaponClass, "sword")) return "cm_idle"; // Claymore/Eyelander
			if (V_stristr(weaponClass, "stickbomb")) return "sb_idle";
			break;
			
		case TF_CLASS_HEAVYWEAPONS:
			// Heavy: m_idle (minigun), idle (shotgun), f_idle (fists)
			if (V_stristr(weaponClass, "minigun")) return "m_idle";
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "fists")) return "f_idle";
			if (V_stristr(weaponClass, "gloves")) return "bg_idle"; // KGB, GRU, etc.
			break;
			
		case TF_CLASS_ENGINEER:
			// Engineer: idle (shotgun), pstl_idle (pistol), gun_idle (wrench), pda_idle (PDA)
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "pistol")) return "pstl_idle";
			if (V_stristr(weaponClass, "wrench")) return "gun_idle";
			if (V_stristr(weaponClass, "pda")) return "pda_idle";
			if (V_stristr(weaponClass, "builder")) return "bld_idle";
			if (V_stristr(weaponClass, "sentry_revenge")) return "fj_idle"; // Frontier Justice
			break;
			
		case TF_CLASS_MEDIC:
			// Medic: sg_idle (syringe gun), idle (medigun), bs_idle (bonesaw)
			if (V_stristr(weaponClass, "syringegun")) return "sg_idle";
			if (V_stristr(weaponClass, "medigun")) return "idle";
			if (V_stristr(weaponClass, "bonesaw")) return "bs_idle";
			break;
			
		case TF_CLASS_SNIPER:
			// Sniper: bw_idle (sniper rifle/bow), smg_idle (SMG), m_idle (melee), pj_idle (jarate)
			if (V_stristr(weaponClass, "sniperrifle")) return "bw_idle";
			if (V_stristr(weaponClass, "compound_bow")) return "bw_idle";
			if (V_stristr(weaponClass, "smg")) return "smg_idle";
			if (V_stristr(weaponClass, "club")) return "m_idle";
			if (V_stristr(weaponClass, "jar")) return "pj_idle"; // Jarate
			break;
			
		case TF_CLASS_SPY:
			// Spy: idle (revolver), knife_idle (knife), c_sapper_idle (sapper), offhand_idle (disguise kit)
			if (V_stristr(weaponClass, "revolver")) return "idle";
			if (V_stristr(weaponClass, "knife")) return "knife_idle";
			if (V_stristr(weaponClass, "sapper")) return "c_sapper_idle";
			if (V_stristr(weaponClass, "pda")) return "offhand_idle"; // Disguise kit
			break;
	}
	
	return defaultAnim;
}

//-----------------------------------------------------------------------------
// Purpose: Apply weapon grip pose to fingers (overrides finger tracking)
//        Samples finger bone rotations from the hand model's weapon animation
//-----------------------------------------------------------------------------
void C_TFVRHand::ApplyWeaponPose(matrix3x4_t *pBoneToWorldOut, int nMaxBones)
{
	CStudioHdr *pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
		return;
	
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon)
		return;
	
	// Get the player to determine class
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner)
		return;
	
	int playerClass = pOwner->GetPlayerClass()->GetClassIndex();
	const char *weaponClass = pWeapon->GetClassname();
	
	// Get the appropriate animation name for this weapon and class
	const char *animName = GetWeaponPoseAnimation(playerClass, weaponClass);
	
	// Look up the sequence
	int sequence = LookupSequence(animName);
	if (sequence < 0)
	{
		// Animation not found - try fallback to "ref" pose
		sequence = LookupSequence("ref");
		if (sequence < 0)
		{
			// No ref pose either, just return
			return;
		}
	}
	
	// Get the sequence descriptor
	mstudioseqdesc_t &seqdesc = pStudioHdr->pSeqdesc(sequence);
	
	// Sample the animation at frame 0 (idle pose)
	float cycle = 0.0f;
	
	// Temporary bone arrays for sampling the animation
	Vector pos[MAXSTUDIOBONES];
	Quaternion q[MAXSTUDIOBONES];
	
	// Sample the animation pose
	IBoneSetup boneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, NULL);
	boneSetup.InitPose(pos, q);
	boneSetup.AccumulatePose(pos, q, sequence, cycle, 1.0f, gpGlobals->curtime, NULL);
	
	// Now apply the sampled finger bone rotations to the output bones
	// We also need to apply it to weapon_bone so weapons attach correctly
	
	// List of finger bone prefixes (without L/R suffix) + weapon_bone
	const char *fingerBones[] = {
		"bip_thumb_0", "bip_thumb_1", "bip_thumb_2",
		"bip_index_0", "bip_index_1", "bip_index_2",
		"bip_middle_0", "bip_middle_1", "bip_middle_2",
		"bip_ring_0", "bip_ring_1", "bip_ring_2",
		"bip_pinky_0", "bip_pinky_1", "bip_pinky_2",
		"weapon_bone",  // IMPORTANT: Also apply pose to weapon bone!
	};
	
	for (int i = 0; i < ARRAYSIZE(fingerBones); i++)
	{
		// Try both left and right hand suffixes
		char boneName[64];
		const char* suffix = IsLeftHand() ? "_L" : "_R";
		V_snprintf(boneName, sizeof(boneName), "%s%s", fingerBones[i], suffix);
		
		int boneIndex = LookupBone(boneName);
		if (boneIndex < 0 || boneIndex >= nMaxBones)
		{
			// Try lowercase suffix
			suffix = IsLeftHand() ? "_l" : "_r";
			V_snprintf(boneName, sizeof(boneName), "%s%s", fingerBones[i], suffix);
			boneIndex = LookupBone(boneName);
		}
		
		// Also try without suffix for bones like "weapon_bone" that might not have L/R
		if (boneIndex < 0 || boneIndex >= nMaxBones)
		{
			boneIndex = LookupBone(fingerBones[i]);
		}
		
		if (boneIndex < 0 || boneIndex >= nMaxBones)
			continue;
		
		// Get the bone's parent
		const mstudiobone_t *pBone = pStudioHdr->pBone(boneIndex);
		if (!pBone)
			continue;
		
		int parentIndex = pBone->parent;
		if (parentIndex < 0 || parentIndex >= nMaxBones)
			continue;
		
		// Convert the sampled quaternion to a matrix
		matrix3x4_t localBoneMatrix;
		QuaternionMatrix(q[boneIndex], pos[boneIndex], localBoneMatrix);
		
		// Transform by parent to get world-space transform
		ConcatTransforms(pBoneToWorldOut[parentIndex], localBoneMatrix, pBoneToWorldOut[boneIndex]);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Should this hand be drawn?
//-----------------------------------------------------------------------------
bool C_TFVRHand::ShouldDraw()
{
	// Early out if shutting down
	if (m_bShuttingDown)
		return false;
	
	// Only draw for local player in VR
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner || !pOwner->IsLocalPlayer())
		return false;
	
	// Check VR is active
	if (!g_pOpenXRManager || !g_pOpenXRManager->IsActive())
		return false;
	
	// Check if hands are enabled
	if (!tfvr_hands_enabled.GetBool())
		return false;
	
	// Always draw hands in VR (bypass frustum culling)
	// The hands are almost always in view, and we want smooth rendering
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Override bounds to ensure hands are never culled
//-----------------------------------------------------------------------------
void C_TFVRHand::GetRenderBounds(Vector& mins, Vector& maxs)
{
	// Return huge bounds so the entity is never culled by frustum
	// This ensures hands are always rendered when in VR
	mins = Vector(-10000, -10000, -10000);
	maxs = Vector(10000, 10000, 10000);
}

//-----------------------------------------------------------------------------
// Purpose: Draw the hand model
//-----------------------------------------------------------------------------
int C_TFVRHand::DrawModel(int flags)
{
	// Safety checks before drawing
	if (m_bShuttingDown)
		return 0;
		
	if (!ShouldDraw())
		return 0;
	
	// Verify owner is still valid
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner)
		return 0;
	
	// Verify model is still valid
	const model_t *pModel = GetModel();
	if (!pModel)
		return 0;
	
	CStudioHdr *pStudioHdr = GetModelPtr();
	if (!pStudioHdr || !pStudioHdr->IsValid())
		return 0;
	
	return BaseClass::DrawModel(flags);
}

//-----------------------------------------------------------------------------
// Purpose: Always cast shadows for VR hands
//-----------------------------------------------------------------------------
ShadowType_t C_TFVRHand::ShadowCastType()
{
	// Always cast high-quality shadows, regardless of owner visibility
	return SHADOWS_RENDER_TO_TEXTURE;
}

//-----------------------------------------------------------------------------
// Purpose: Always receive projected textures (shadows) for VR hands
//-----------------------------------------------------------------------------
bool C_TFVRHand::ShouldReceiveProjectedTextures(int flags)
{
	// Always receive shadows
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Equip a weapon to this hand
//-----------------------------------------------------------------------------
void C_TFVRHand::EquipWeapon(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
		return;
	
	// Unequip current weapon if any
	UnequipWeapon();
	
	// Store reference to the actual weapon (for getting properties, firing, etc.)
	m_hHeldWeapon = pWeapon;
	
	// VR NEW APPROACH: Create a separate render-only entity for the weapon visual
	// This way the player's actual weapon can remain in the viewmodel system
	// and we have full control over a separate worldmodel entity for rendering
	
	const char *worldModel = pWeapon->GetWorldModel();
	if (!worldModel || !worldModel[0])
		return;
	
	// Create a simple animating entity for rendering the weapon
	C_BaseAnimating *pRenderWeapon = new C_BaseAnimating;
	if (!pRenderWeapon)
		return;
	
	// Initialize it
	if (!pRenderWeapon->InitializeAsClientEntity(worldModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		pRenderWeapon->Release();
		return;
	}
	
	// Store the render weapon
	m_hRenderWeapon = pRenderWeapon;
	
	// Set it up
	pRenderWeapon->SetModelIndex(modelinfo->GetModelIndex(worldModel));
	pRenderWeapon->SetSequence(0);  // World models don't animate
	pRenderWeapon->SetRenderMode(kRenderNormal);
	pRenderWeapon->SetRenderColor(255, 255, 255, 255);
	pRenderWeapon->RemoveEffects(EF_NODRAW);
	
	// VR: Don't parent - use manual positioning for better control
	// Parenting doesn't work well because hand bones update at different times
	
	// Mark the actual weapon as held (for firing mechanics)
	pWeapon->SetHeldByVRHand(true);
	
	// Try to cast to VR weapon base
	CTFVRWeaponBase *pVRWeapon = dynamic_cast<CTFVRWeaponBase*>(pWeapon);
	if (pVRWeapon)
	{
		// Tell weapon it's been equipped by this hand
		pVRWeapon->SetOwnerHand(this);
		pVRWeapon->OnEquippedByHand();
		
		// Parent weapon to hand for basic following
		pWeapon->FollowEntity(this);
		
		// Set model for correct hand
		pVRWeapon->SetModelForHand(IsRightHand());
	}
	
}

//-----------------------------------------------------------------------------
// Purpose: Unequip the currently held weapon
//-----------------------------------------------------------------------------
void C_TFVRHand::UnequipWeapon()
{
	// Clean up render weapon
	if (m_hRenderWeapon.Get())
	{
		m_hRenderWeapon->Release();
		m_hRenderWeapon = NULL;
	}
	
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon)
		return;
	
	// Clear VR hand flag
	pWeapon->SetHeldByVRHand( false );
	
	// Try to cast to VR weapon base
	CTFVRWeaponBase *pVRWeapon = dynamic_cast<CTFVRWeaponBase*>(pWeapon);
	if (pVRWeapon)
	{
		// Tell weapon it's been dropped
		pVRWeapon->OnDroppedFromHand();
		pVRWeapon->SetOwnerHand(NULL);
	}
	
	m_hHeldWeapon = NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Update the position of the held weapon
//          NOTE: Actual positioning happens in SetupBones() -> PositionWeaponFromBones()
//          This function just maintains visibility and model state
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateWeaponTransform()
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon)
		return;
	
	// Ensure weapon stays visible and uses world model
	pWeapon->RemoveEffects(EF_NODRAW);
	pWeapon->RemoveEffects(EF_BONEMERGE);
	pWeapon->RemoveEffects(EF_BONEMERGE_FASTCULL);
	
	// Double-check the model is correct (in case TF2 tried to switch it)
	const char *worldModel = pWeapon->GetWorldModel();
	if (worldModel && worldModel[0])
	{
		int worldModelIndex = modelinfo->GetModelIndex(worldModel);
		if (worldModelIndex > 0 && pWeapon->GetModelIndex() != worldModelIndex)
		{
			// Model was switched - force it back to world model
			pWeapon->SetModelIndex(worldModelIndex);
			pWeapon->SetSequence(0);
		}
	}
	
	// NOTE: Weapon positioning is now handled in SetupBones() -> PositionWeaponFromBones()
	// This ensures the weapon_bone has the correct pose applied before we read it
}

//-----------------------------------------------------------------------------
// Networking table (client-only entity, no networked properties)
//-----------------------------------------------------------------------------
IMPLEMENT_CLIENTCLASS_DT(C_TFVRHand, DT_TFVRHand, CTFVRHand)
END_RECV_TABLE()

//-----------------------------------------------------------------------------
// Console Commands for Testing
//-----------------------------------------------------------------------------

CON_COMMAND(tfvr_adjust_grip, "Show current grip offset values and how to adjust them")
{
	Msg("=== TF2VR Weapon Grip Adjustment ===\n");
	Msg("Current grip offset:\n");
	Msg("  X (forward): %.2f\n", tfvr_weapon_grip_offset_x.GetFloat());
	Msg("  Y (right):   %.2f\n", tfvr_weapon_grip_offset_y.GetFloat());
	Msg("  Z (up):      %.2f\n", tfvr_weapon_grip_offset_z.GetFloat());
	Msg("\nCurrent grip angles:\n");
	Msg("  Pitch: %.2f\n", tfvr_weapon_grip_angle_pitch.GetFloat());
	Msg("  Yaw:   %.2f\n", tfvr_weapon_grip_angle_yaw.GetFloat());
	Msg("  Roll:  %.2f\n", tfvr_weapon_grip_angle_roll.GetFloat());
	Msg("\nTo adjust:\n");
	Msg("  tfvr_weapon_grip_offset_x <value>  // Move forward(+) or back(-)\n");
	Msg("  tfvr_weapon_grip_offset_y <value>  // Move right(+) or left(-)\n");
	Msg("  tfvr_weapon_grip_offset_z <value>  // Move up(+) or down(-)\n");
	Msg("  tfvr_weapon_grip_angle_pitch <value>\n");
	Msg("  tfvr_weapon_grip_angle_yaw <value>\n");
	Msg("  tfvr_weapon_grip_angle_roll <value>\n");
	Msg("\nChanges apply immediately! Enable tfvr_debug_weapon_attachment 1 to see the result.\n");
}

CON_COMMAND(tfvr_force_weapon_visible, "Force weapon to be visible")
{
	C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
	if (!pRightHand)
	{
		Warning("Right hand not found!\n");
		return;
	}

	C_TFWeaponBase *pWeapon = pRightHand->GetHeldWeapon();
	if (!pWeapon)
	{
		Warning("Right hand is not holding a weapon!\n");
		return;
	}

	// Force weapon visible
	pWeapon->RemoveEffects(EF_NODRAW);
	pWeapon->RemoveEffects(EF_BONEMERGE);
	pWeapon->RemoveEffects(EF_BONEMERGE_FASTCULL);
	pWeapon->RemoveEffects(EF_PARENT_ANIMATES);
	pWeapon->SetRenderMode(kRenderNormal);
	pWeapon->SetRenderColor(255, 255, 255, 255);
	pWeapon->AddToLeafSystem(RENDER_GROUP_OPAQUE_ENTITY);
	
	Msg("Forced weapon '%s' to be visible\n", pWeapon->GetClassname());
	Msg("  Model: %s\n", modelinfo->GetModelName(pWeapon->GetModel()));
	Msg("  Effects: %d\n", pWeapon->GetEffects());
	Msg("  RenderMode: %d\n", pWeapon->GetRenderMode());
	Msg("  Position: %.1f, %.1f, %.1f\n", 
		pWeapon->GetAbsOrigin().x,
		pWeapon->GetAbsOrigin().y,
		pWeapon->GetAbsOrigin().z);
}

CON_COMMAND(tfvr_test_weapon_follow, "Test if weapon is following hand movement")
{
	C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
	if (!pRightHand)
	{
		Warning("Right hand not found!\n");
		return;
	}

	C_TFWeaponBase *pWeapon = pRightHand->GetHeldWeapon();
	if (!pWeapon)
	{
		Warning("Right hand is not holding a weapon!\n");
		return;
	}

	Msg("=== Weapon Follow Test ===\n");
	Msg("Hand position: %.1f, %.1f, %.1f\n", 
		pRightHand->GetAbsOrigin().x,
		pRightHand->GetAbsOrigin().y,
		pRightHand->GetAbsOrigin().z);
	Msg("Weapon position: %.1f, %.1f, %.1f\n",
		pWeapon->GetAbsOrigin().x,
		pWeapon->GetAbsOrigin().y,
		pWeapon->GetAbsOrigin().z);
	Msg("Distance: %.1f\n", (pWeapon->GetAbsOrigin() - pRightHand->GetAbsOrigin()).Length());
	
	int handWeaponBone = pRightHand->LookupBone("weapon_bone");
	Msg("Hand weapon_bone index: %d\n", handWeaponBone);
	
	Msg("\nMove your hand and run this command again to see if weapon follows.\n");
	Msg("Enable tfvr_debug_weapon_position 1 for continuous updates.\n");
}

CON_COMMAND(tfvr_test_equip_bat, "Test equipping the bat to the right hand")
{
	C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
	if (!pPlayer)
	{
		Warning("No local player found!\n");
		return;
	}

	if (!pPlayer->IsInVRMode())
	{
		Warning("Player is not in VR mode!\n");
		return;
	}

	C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
	if (!pRightHand)
	{
		Warning("Right hand not found!\n");
		return;
	}

	// Get the player's active weapon
	CTFWeaponBase *pActiveWeapon = pPlayer->GetActiveTFWeapon();
	if (!pActiveWeapon)
	{
		Warning("No active weapon!\n");
		return;
	}

	// Equip it to the hand
	pRightHand->EquipWeapon(pActiveWeapon);
	Msg("Equipped weapon '%s' to right hand\n", pActiveWeapon->GetClassname());
}

CON_COMMAND(tfvr_test_unequip_weapon, "Test unequipping weapon from right hand")
{
	C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
	if (!pRightHand)
	{
		Warning("Right hand not found!\n");
		return;
	}

	C_TFWeaponBase *pWeapon = pRightHand->GetHeldWeapon();
	if (!pWeapon)
	{
		Warning("Right hand is not holding a weapon!\n");
		return;
	}

	Msg("Unequipping weapon '%s' from right hand\n", pWeapon->GetClassname());
	pRightHand->UnequipWeapon();
}

CON_COMMAND(tfvr_weapon_info, "Display info about the currently held weapon")
{
	C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
	if (!pPlayer)
	{
		Warning("No local player found!\n");
		return;
	}

	Msg("=== VR Weapon System Status ===\n");
	Msg("VR Mode: %s\n", pPlayer->IsInVRMode() ? "ENABLED" : "DISABLED");
	
	CTFWeaponBase *pActiveWeapon = pPlayer->GetActiveTFWeapon();
	if (!pActiveWeapon)
	{
		Warning("No active weapon!\n");
		return;
	}

	Msg("\n=== Active Weapon ===\n");
	Msg("Classname: %s\n", pActiveWeapon->GetClassname());
	Msg("Weapon ID: %d\n", pActiveWeapon->GetWeaponID());
	Msg("Position: %.2f, %.2f, %.2f\n", 
		pActiveWeapon->GetAbsOrigin().x,
		pActiveWeapon->GetAbsOrigin().y,
		pActiveWeapon->GetAbsOrigin().z);
	Msg("Angles: %.2f, %.2f, %.2f\n",
		pActiveWeapon->GetAbsAngles().x,
		pActiveWeapon->GetAbsAngles().y,
		pActiveWeapon->GetAbsAngles().z);

	// Check if it's a VR weapon
	CTFVRWeaponBase *pVRWeapon = dynamic_cast<CTFVRWeaponBase*>(pActiveWeapon);
	if (pVRWeapon)
	{
		Msg("VR Weapon: YES\n");
		Msg("Two-handed: %s\n", pVRWeapon->IsTwoHanded() ? "YES" : "NO");
		
		CTFVRHand *pOwnerHand = pVRWeapon->GetOwnerHand();
		if (pOwnerHand)
		{
			Msg("Held by: %s hand\n", pOwnerHand->IsLeftHand() ? "LEFT" : "RIGHT");
		}
		else
		{
			Msg("Held by: NONE\n");
		}
	}
	else
	{
		Msg("VR Weapon: NO (standard TF2 weapon)\n");
	}

	Msg("\n=== VR Hands ===\n");
	C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
	C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
	
	if (pLeftHand)
	{
		C_TFWeaponBase *pHeldWeapon = pLeftHand->GetHeldWeapon();
		Msg("Left hand: %s\n", pHeldWeapon ? pHeldWeapon->GetClassname() : "EMPTY");
		if (pHeldWeapon)
		{
			Msg("  Position: %.2f, %.2f, %.2f\n",
				pHeldWeapon->GetAbsOrigin().x,
				pHeldWeapon->GetAbsOrigin().y,
				pHeldWeapon->GetAbsOrigin().z);
		}
	}
	else
	{
		Msg("Left hand: NOT FOUND\n");
	}
	
	if (pRightHand)
	{
		C_TFWeaponBase *pHeldWeapon = pRightHand->GetHeldWeapon();
		Msg("Right hand: %s\n", pHeldWeapon ? pHeldWeapon->GetClassname() : "EMPTY");
		if (pHeldWeapon)
		{
			Msg("  Position: %.2f, %.2f, %.2f\n",
				pHeldWeapon->GetAbsOrigin().x,
				pHeldWeapon->GetAbsOrigin().y,
				pHeldWeapon->GetAbsOrigin().z);
		}
		Msg("  Hand position: %.2f, %.2f, %.2f\n",
			pRightHand->GetAbsOrigin().x,
			pRightHand->GetAbsOrigin().y,
			pRightHand->GetAbsOrigin().z);
	}
	else
	{
		Msg("Right hand: NOT FOUND\n");
	}
}
