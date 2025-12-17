// Purpose: VR Hand entity implementation

#include "cbase.h"
#include "c_tfvr_hand.h"
#include "tf/c_tf_player.h"
#include "tf/tf_weaponbase.h"
#include "tfvr/openxr_manager.h"
#include "tfvr/openxr_hand_tracking.h"
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

// Global storage for active VR hands - since we only support local player, use two pointers
static C_TFVRHand *g_pLocalPlayerLeftHand = NULL;
static C_TFVRHand *g_pLocalPlayerRightHand = NULL;

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

	// For now, use the test scout model (contains both hands, but we'll only use one)
	// TODO: Load per-class models based on player class and create separate left/right models
	
	// Try custom VR model first
	Q_strncpy(m_szModelName, "models/weapons/vr_models/vr_scout_arms.mdl", sizeof(m_szModelName));
	
	// Set a valid origin first (entities need to be in the world)
	SetAbsOrigin(pOwner->EyePosition());
	SetAbsAngles(vec3_angle);
	
	// Try using SetModel directly
	bool bCustomModelWorked = SetModel(m_szModelName);
	
	if (!bCustomModelWorked)
	{
		// Use fallback
		Q_strncpy(m_szModelName, "models/weapons/c_models/c_scout_arms.mdl", sizeof(m_szModelName));
		if (!SetModel(m_szModelName))
		{
			Warning("C_TFVRHand::Initialize - Failed to load hand model!\n");
			return false;
		}
		
		if (tfvr_hands_debug.GetBool())
			Msg("VR Hand (%s): Using fallback model\n", IsLeftHand() ? "LEFT" : "RIGHT");
	}
	else if (tfvr_hands_debug.GetBool())
	{
		Msg("VR Hand (%s): Using custom VR model\n", IsLeftHand() ? "LEFT" : "RIGHT");
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
	SetRenderMode(kRenderTransTexture);
	SetRenderColor(255, 255, 255, 255 * tfvr_hands_alpha.GetFloat());
	
	// Mark as always drawing
	AddToLeafSystem(RENDER_GROUP_OPAQUE_ENTITY);

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
	// TODO: Implement bone animation from OpenXR hand tracking
	// For now, just use the default pose from the model
	
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

	// TODO: In next phase, drive bone transforms from hand tracking data
	// For now, we'll just position the hand root bone at controller position
	// This happens in SetupBones() override
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
		
		// Apply finger tracking to this hand
		ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
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
// Purpose: Equip a weapon to this hand
//-----------------------------------------------------------------------------
void C_TFVRHand::EquipWeapon(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
		return;
	
	// Unequip current weapon if any
	UnequipWeapon();
	
	// Set new weapon
	m_hHeldWeapon = pWeapon;
	
	// TODO: Set weapon owner hand, position weapon, etc.
	// This will be implemented when we create the VR weapon base class
}

//-----------------------------------------------------------------------------
// Purpose: Unequip the currently held weapon
//-----------------------------------------------------------------------------
void C_TFVRHand::UnequipWeapon()
{
	if (!m_hHeldWeapon.Get())
		return;
	
	// TODO: Clear weapon owner hand, reset weapon position, etc.
	// This will be implemented when we create the VR weapon base class
	
	m_hHeldWeapon = NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Update the position of the held weapon
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateWeaponTransform()
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon)
		return;
	
	// TODO: Position weapon based on grip transform
	// This will be implemented when we create the VR weapon base class
}

// Implement empty network table (client-only entity)
IMPLEMENT_CLIENTCLASS_DT(C_TFVRHand, DT_TFVRHand, CTFVRHand)
END_RECV_TABLE()

