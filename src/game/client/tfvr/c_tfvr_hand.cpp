// Purpose: VR Hand entity implementation

#include "cbase.h"
#include "c_tfvr_hand.h"
#include "tf/c_tf_player.h"
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

// Rotation offset convars - left hand
ConVar tfvr_hands_left_offset_pitch("tfvr_hands_left_offset_pitch", "0", FCVAR_ARCHIVE, "Pitch offset for left VR hand (degrees)");
ConVar tfvr_hands_left_offset_yaw("tfvr_hands_left_offset_yaw", "0", FCVAR_ARCHIVE, "Yaw offset for left VR hand (degrees)");
ConVar tfvr_hands_left_offset_roll("tfvr_hands_left_offset_roll", "0", FCVAR_ARCHIVE, "Roll offset for left VR hand (degrees)");

// Rotation offset convars - right hand
ConVar tfvr_hands_right_offset_pitch("tfvr_hands_right_offset_pitch", "0", FCVAR_ARCHIVE, "Pitch offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_yaw("tfvr_hands_right_offset_yaw", "0", FCVAR_ARCHIVE, "Yaw offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_roll("tfvr_hands_right_offset_roll", "180", FCVAR_ARCHIVE, "Roll offset for right VR hand (degrees)");

// Global storage for active VR hands - since we only support local player, just use a single pointer
static C_TFVRHand *g_pLocalPlayerVRHands = NULL;

//-----------------------------------------------------------------------------
// Purpose: Global update function called every frame from VR menu manager
//-----------------------------------------------------------------------------
void UpdateVRHands()
{
	if (g_pLocalPlayerVRHands)
	{
		g_pLocalPlayerVRHands->Update();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Clean up all VR hands (called on level shutdown)
//-----------------------------------------------------------------------------
void CleanupAllVRHands()
{
	if (g_pLocalPlayerVRHands)
	{
		g_pLocalPlayerVRHands->Shutdown();
		g_pLocalPlayerVRHands->RemoveFromLeafSystem();
		g_pLocalPlayerVRHands->SetRemovalFlag(true);
		delete g_pLocalPlayerVRHands;
		g_pLocalPlayerVRHands = NULL;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
C_TFVRHand::C_TFVRHand()
{
	m_hOwnerPlayer = NULL;
	m_pHandTracker = NULL;
	m_bLeftHandTrackingValid = false;
	m_bRightHandTrackingValid = false;
	m_bBoneMappingSetup = false;
	m_bLeftControllerTracked = false;
	m_bRightControllerTracked = false;
	m_bShuttingDown = false;
	m_iLastPlayerClass = TF_CLASS_UNDEFINED;
	m_vecLeftLastValidPosition = vec3_origin;
	m_angLeftLastValidAngles = vec3_angle;
	m_vecRightLastValidPosition = vec3_origin;
	m_angRightLastValidAngles = vec3_angle;
	m_szModelName[0] = '\0';
	m_iLeftHandBone = -1;
	m_iRightHandBone = -1;

	// Initialize bone mapping to invalid for both hands
	for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
	{
		m_LeftBoneMapping[i] = -1;
		m_RightBoneMapping[i] = -1;
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
// Purpose: Initialize the hand entity (contains both hands in one model)
//-----------------------------------------------------------------------------
bool C_TFVRHand::Initialize(C_TFPlayer *pOwner)
{
	if (!pOwner)
	{
		Warning("C_TFVRHand::Initialize - No owner player!\n");
		return false;
	}

	// Reset shutdown flag in case we're reinitializing
	m_bShuttingDown = false;
	
	m_hOwnerPlayer = pOwner;
	
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

	// For now, use the test scout model (contains both hands)
	// TODO: Load per-class models based on player class
	
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
			Msg("VR Hands: Using fallback model\n");
	}
	else if (tfvr_hands_debug.GetBool())
	{
		Msg("VR Hands: Using custom VR model\n");
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
	m_iLeftHandBone = -1;
	m_iRightHandBone = -1;

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
	
	// Reset bone mapping so it gets recalculated on reinit
	m_bBoneMappingSetup = false;
	m_iLeftHandBone = -1;
	m_iRightHandBone = -1;
	
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
// Purpose: Spawn VR hands for a player (single entity with both hands)
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
	if (g_pLocalPlayerVRHands)
	{
		// Reinitialize with new player pointer
		g_pLocalPlayerVRHands->Shutdown();
		if (g_pLocalPlayerVRHands->Initialize(pPlayer))
		{
			g_pLocalPlayerVRHands->Spawn();
			return;
		}
	}

	// Create new entity if none exists
	C_TFVRHand *pHands = new C_TFVRHand();
	if (pHands && pHands->Initialize(pPlayer))
	{
		// Call Spawn to properly initialize the entity
		pHands->Spawn();
		
		g_pLocalPlayerVRHands = pHands;
	}
	else
	{
		Warning("VR Hands: Failed to create VR hands!\n");
		if (pHands)
			delete pHands;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Remove VR hands for a player (just hides, doesn't delete)
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveVRHands(C_TFPlayer *pPlayer)
{
	if (!pPlayer)
		return;

	if (g_pLocalPlayerVRHands)
	{
		g_pLocalPlayerVRHands->Shutdown();
		g_pLocalPlayerVRHands->AddEffects(EF_NODRAW);
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

	// Update both hand positions and orientations
	UpdateHandTransforms();

	// Update bone animation from hand tracking
	UpdateHandBones();

	// Debug visualization
	if (tfvr_hands_debug.GetBool() && debugoverlay)
	{
		// Show debug info for both hands
		Vector leftPos = m_vecLeftLastValidPosition;
		Vector rightPos = m_vecRightLastValidPosition;
		
		debugoverlay->AddBoxOverlay(leftPos, Vector(-2, -2, -2), Vector(2, 2, 2), 
			m_angLeftLastValidAngles, 0, 255, 0, 100, 0.0f);
		debugoverlay->AddTextOverlay(leftPos, 0.0f, "Left Hand\nTracked: %s", 
			m_bLeftControllerTracked ? "YES" : "NO");
		
		debugoverlay->AddBoxOverlay(rightPos, Vector(-2, -2, -2), Vector(2, 2, 2), 
			m_angRightLastValidAngles, 255, 0, 0, 100, 0.0f);
		debugoverlay->AddTextOverlay(rightPos, 0.0f, "Right Hand\nTracked: %s", 
			m_bRightControllerTracked ? "YES" : "NO");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Get wrist transform as a matrix (avoids gimbal lock)
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetWristTransform(bool leftHand, VMatrix& outTransform)
{
	if (!m_pHandTracker)
		return false;
	
	// Get wrist position and angles
	Vector wristPos;
	QAngle wristAngles;
	
	if (!m_pHandTracker->GetHandJoint(leftHand, XR_HAND_JOINT_WRIST_EXT, wristPos, wristAngles))
		return false;
	
	// Convert to matrix
	matrix3x4_t temp;
	AngleMatrix(wristAngles, wristPos, temp);
	outTransform.CopyFrom3x4(temp);
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Update both hand positions from hand tracking wrist positions
//          We'll position the hand bones via SetupBones override later
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateHandTransforms()
{
	if (m_bShuttingDown)
		return;
		
	if (!g_pOpenXRManager || !m_pHandTracker)
		return;

	// Try to get wrist positions using matrices (avoids gimbal lock)
	VMatrix leftWristMatrix, rightWristMatrix;
	
	bool leftHandValid = GetWristTransform(true, leftWristMatrix);
	bool rightHandValid = GetWristTransform(false, rightWristMatrix);
	
	// Update left hand if valid, fallback to controller
	if (leftHandValid)
	{
		m_vecLeftLastValidPosition = leftWristMatrix.GetTranslation();
		MatrixAngles(leftWristMatrix.As3x4(), m_angLeftLastValidAngles);
		m_bLeftControllerTracked = true;
	}
	else
	{
		// Fallback to controller pose
		VMatrix leftControllerPose;
		m_bLeftControllerTracked = g_pOpenXRManager->GetLeftControllerPose(leftControllerPose);
		
		if (m_bLeftControllerTracked)
		{
			m_vecLeftLastValidPosition = leftControllerPose.GetTranslation();
			MatrixAngles(leftControllerPose.As3x4(), m_angLeftLastValidAngles);
		}
	}

	// Update right hand if valid, fallback to controller
	if (rightHandValid)
	{
		m_vecRightLastValidPosition = rightWristMatrix.GetTranslation();
		MatrixAngles(rightWristMatrix.As3x4(), m_angRightLastValidAngles);
		m_bRightControllerTracked = true;
	}
	else
	{
		// Fallback to controller pose
		VMatrix rightControllerPose;
		m_bRightControllerTracked = g_pOpenXRManager->GetRightControllerPose(rightControllerPose);
		
		if (m_bRightControllerTracked)
		{
			m_vecRightLastValidPosition = rightControllerPose.GetTranslation();
			MatrixAngles(rightControllerPose.As3x4(), m_angRightLastValidAngles);
		}
	}

	// Position the entity at the midpoint between hands for now
	// (The actual hand bones will be positioned in SetupBones)
	Vector midpoint = (m_vecLeftLastValidPosition + m_vecRightLastValidPosition) * 0.5f;
	SetAbsOrigin(midpoint);
	SetAbsAngles(vec3_angle); // No rotation on the entity itself

	// Fade out if both controllers are not tracked
	if (!m_bLeftControllerTracked && !m_bRightControllerTracked)
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
			Warning("VR Hands: No hand tracker in UpdateHandBones\n");
		return;
	}

	// Check if hand tracking is active for both hands
	m_bLeftHandTrackingValid = m_pHandTracker->IsLeftHandTracked();
	m_bRightHandTrackingValid = m_pHandTracker->IsRightHandTracked();

	// Set up bone mapping if not done yet
	if (!m_bBoneMappingSetup)
	{
		SetupBoneMapping();
	}

	// TODO: In next phase, drive bone transforms from hand tracking data
	// For now, we'll just position the hand root bones at controller positions
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
// Purpose: Override SetupBones to position hand bones at controller locations
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
	
	// Update hand transforms NOW to get the most recent data (avoid 1-frame lag)
	UpdateHandTransforms();
	
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
	
	// Safety check: validate bone indices are still valid for this model
	int modelBoneCount = pStudioHdr->numbones();
	if (m_iLeftHandBone >= modelBoneCount || m_iRightHandBone >= modelBoneCount)
	{
		// Model changed, need to re-setup bone mapping
		Warning("VR Hands: Bone indices invalid for current model, resetting\n");
		m_bBoneMappingSetup = false;
		m_iLeftHandBone = -1;
		m_iRightHandBone = -1;
		return true;
	}

	// Position left hand bone at left controller and update all children
	if (m_iLeftHandBone >= 0 && m_iLeftHandBone < nMaxBones && m_bLeftControllerTracked)
	{
		// Store the original hand bone transform
		matrix3x4_t originalHandTransform;
		MatrixCopy(pBoneToWorldOut[m_iLeftHandBone], originalHandTransform);

		// Start with the wrist transform
		matrix3x4_t wristTransform;
		AngleMatrix(m_angLeftLastValidAngles, m_vecLeftLastValidPosition, wristTransform);
		
		// Final transform after applying offsets
		matrix3x4_t newHandTransform;
		
		// Apply rotation offset as a local rotation (use LEFT hand offsets)
		if (tfvr_hands_left_offset_pitch.GetFloat() != 0 || tfvr_hands_left_offset_yaw.GetFloat() != 0 || tfvr_hands_left_offset_roll.GetFloat() != 0)
		{
			matrix3x4_t offsetMatrix;
			QAngle offsetAngles(tfvr_hands_left_offset_pitch.GetFloat(), 
			                    tfvr_hands_left_offset_yaw.GetFloat(), 
			                    tfvr_hands_left_offset_roll.GetFloat());
			AngleMatrix(offsetAngles, offsetMatrix);
			
			// Apply offset as local rotation: final = wrist * offset
			ConcatTransforms(wristTransform, offsetMatrix, newHandTransform);
		}
		else
		{
			MatrixCopy(wristTransform, newHandTransform);
		}
		
		// Apply the new transform to the bone
		MatrixCopy(newHandTransform, pBoneToWorldOut[m_iLeftHandBone]);

		// Calculate the delta transform (from old to new)
		matrix3x4_t deltaTransform;
		matrix3x4_t inverseOriginal;
		MatrixInvert(originalHandTransform, inverseOriginal);
		ConcatTransforms(newHandTransform, inverseOriginal, deltaTransform);

		// Apply the delta transform to all child bones
		CUtlVector<int> vecChildBones;
		AppendChildBones_R(&vecChildBones, pStudioHdr, m_iLeftHandBone);
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
	}

	// Position right hand bone at right controller and update all children
	if (m_iRightHandBone >= 0 && m_iRightHandBone < nMaxBones && m_bRightControllerTracked)
	{
		// Store the original hand bone transform
		matrix3x4_t originalHandTransform;
		MatrixCopy(pBoneToWorldOut[m_iRightHandBone], originalHandTransform);

		// Start with the wrist transform
		matrix3x4_t wristTransform;
		AngleMatrix(m_angRightLastValidAngles, m_vecRightLastValidPosition, wristTransform);
		
		// Final transform after applying offsets
		matrix3x4_t newHandTransform;
		
		// Apply rotation offset as a local rotation (use RIGHT hand offsets)
		if (tfvr_hands_right_offset_pitch.GetFloat() != 0 || tfvr_hands_right_offset_yaw.GetFloat() != 0 || tfvr_hands_right_offset_roll.GetFloat() != 0)
		{
			matrix3x4_t offsetMatrix;
			QAngle offsetAngles(tfvr_hands_right_offset_pitch.GetFloat(), 
			                    tfvr_hands_right_offset_yaw.GetFloat(), 
			                    tfvr_hands_right_offset_roll.GetFloat());
			AngleMatrix(offsetAngles, offsetMatrix);
			
			// Apply offset as local rotation: final = wrist * offset
			ConcatTransforms(wristTransform, offsetMatrix, newHandTransform);
		}
		else
		{
			MatrixCopy(wristTransform, newHandTransform);
		}
		
		// Apply the new transform to the bone
		MatrixCopy(newHandTransform, pBoneToWorldOut[m_iRightHandBone]);

		// Calculate the delta transform (from old to new)
		matrix3x4_t deltaTransform;
		matrix3x4_t inverseOriginal;
		MatrixInvert(originalHandTransform, inverseOriginal);
		ConcatTransforms(newHandTransform, inverseOriginal, deltaTransform);

		// Apply the delta transform to all child bones
		CUtlVector<int> vecChildBones;
		AppendChildBones_R(&vecChildBones, pStudioHdr, m_iRightHandBone);
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
		Warning("VR Hands: StudioHdr is not valid!\n");
		return;
	}

	int numBones = pStudioHdr->numbones();
	
	if (numBones <= 0 || numBones > 256)
	{
		Warning("VR Hands: Invalid bone count: %d\n", numBones);
		return;
	}

	// Find the hand bones in the model
	// Try common bone names for TF2 viewmodel arms
	const char* leftBoneNames[] = { "bip_hand_L", "weapon_bone_L", "ValveBiped.Bip01_L_Hand", "bip_hand_l" };
	const char* rightBoneNames[] = { "bip_hand_R", "weapon_bone_R", "ValveBiped.Bip01_R_Hand", "bip_hand_r" };

	// Try to find left hand bone
	for (int i = 0; i < ARRAYSIZE(leftBoneNames); i++)
	{
		m_iLeftHandBone = LookupBone(leftBoneNames[i]);
		if (m_iLeftHandBone != -1)
			break;
	}

	// Try to find right hand bone
	for (int i = 0; i < ARRAYSIZE(rightBoneNames); i++)
	{
		m_iRightHandBone = LookupBone(rightBoneNames[i]);
		if (m_iRightHandBone != -1)
			break;
	}

	if (m_iLeftHandBone == -1 || m_iRightHandBone == -1)
	{
		Warning("VR Hands: Could not find hand bones! Left: %d, Right: %d\n", 
			m_iLeftHandBone, m_iRightHandBone);
	}

	// TODO: Map individual finger bones for hand tracking animation
	// For now, we just need the hand root bones

	m_bBoneMappingSetup = true;
}

//-----------------------------------------------------------------------------
// Purpose: Map an OpenXR joint to a Source bone index
//-----------------------------------------------------------------------------
bool C_TFVRHand::MapOpenXRJointToBone(XrHandJointEXT joint, bool bLeftHand, int &boneIndex)
{
	if (joint < 0 || joint >= XR_HAND_JOINT_COUNT_EXT)
		return false;

	if (bLeftHand)
	{
		boneIndex = m_LeftBoneMapping[joint];
	}
	else
	{
		boneIndex = m_RightBoneMapping[joint];
	}
	
	return (boneIndex >= 0);
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

// Implement empty network table (client-only entity)
IMPLEMENT_CLIENTCLASS_DT(C_TFVRHand, DT_TFVRHand, CTFVRHand)
END_RECV_TABLE()

