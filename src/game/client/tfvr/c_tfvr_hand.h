// Purpose: VR Hand entity - client-only animated hands driven by OpenXR hand tracking

#ifndef C_TFVR_HAND_H
#define C_TFVR_HAND_H
#ifdef _WIN32
#pragma once
#endif

#include "cbase.h"
#include "c_baseanimating.h"
#include "openxr/openxr.h"

class C_TFPlayer;
class COpenXRHandTracker;

//-----------------------------------------------------------------------------
// Purpose: Client-side VR hand entity that renders animated hands
//          driven by OpenXR hand tracking data
//          NOTE: This is a SINGLE entity containing BOTH hands (like viewmodel arms)
//-----------------------------------------------------------------------------
class C_TFVRHand : public C_BaseAnimating
{
	DECLARE_CLASS(C_TFVRHand, C_BaseAnimating);
	DECLARE_CLIENTCLASS();

public:
	C_TFVRHand();
	virtual ~C_TFVRHand();

	// Spawning and lifecycle (spawns single entity with both hands)
	static void SpawnVRHands(C_TFPlayer *pPlayer);
	static void RemoveVRHands(C_TFPlayer *pPlayer);
	
	bool Initialize(C_TFPlayer *pOwner);
	void Shutdown();

	// Spawn override
	virtual void Spawn() override;

	// Update methods called every frame
	virtual void ClientThink() override;
	void Update(); // Manual update method
	void UpdateHandTransforms();
	void UpdateHandBones();
	
	// Helper to get wrist transform as matrix
	bool GetWristTransform(bool leftHand, VMatrix& outTransform);

	// Bone setup override to position hand bones
	virtual bool SetupBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones, int boneMask, float currentTime) override;

	// Rendering control
	virtual bool ShouldDraw() override;
	virtual int DrawModel(int flags) override;
	
	// Override to ensure hands are always in PVS
	virtual void GetRenderBounds(Vector& mins, Vector& maxs) override;

	// Bone mapping
	void SetupBoneMapping();
	bool MapOpenXRJointToBone(XrHandJointEXT joint, bool bLeftHand, int &boneIndex);

	// Accessors
	C_TFPlayer* GetOwnerPlayer() const { return m_hOwnerPlayer.Get(); }

private:
	// Owner
	CHandle<C_TFPlayer> m_hOwnerPlayer;

	// Hand tracking
	COpenXRHandTracker *m_pHandTracker;
	bool m_bLeftHandTrackingValid;
	bool m_bRightHandTrackingValid;
	
	// Shutdown flag
	bool m_bShuttingDown;
	
	// Last known player class - to detect class changes
	int m_iLastPlayerClass;

	// Bone indices for hand roots
	int m_iLeftHandBone;
	int m_iRightHandBone;

	// Bone mapping: OpenXR joint index -> Source bone index (separate for each hand)
	// -1 means no corresponding bone in the model
	int m_LeftBoneMapping[XR_HAND_JOINT_COUNT_EXT];
	int m_RightBoneMapping[XR_HAND_JOINT_COUNT_EXT];
	bool m_bBoneMappingSetup;

	// Tracking state for both controllers
	bool m_bLeftControllerTracked;
	bool m_bRightControllerTracked;
	Vector m_vecLeftLastValidPosition;
	QAngle m_angLeftLastValidAngles;
	Vector m_vecRightLastValidPosition;
	QAngle m_angRightLastValidAngles;

	// Model info
	char m_szModelName[MAX_PATH];
};

// Global functions
void UpdateVRHands();
void CleanupAllVRHands();

#endif // C_TFVR_HAND_H

