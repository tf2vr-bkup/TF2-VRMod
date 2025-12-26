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
class C_TFWeaponBase;

// Hand side enum
enum VRHandSide
{
	VR_HAND_LEFT = 0,
	VR_HAND_RIGHT = 1
};

//-----------------------------------------------------------------------------
// Purpose: Client-side VR hand entity that renders a single animated hand
//          driven by OpenXR hand tracking data
//          NOTE: This now represents a SINGLE hand (left OR right)
//-----------------------------------------------------------------------------
class C_TFVRHand : public C_BaseAnimating
{
	DECLARE_CLASS(C_TFVRHand, C_BaseAnimating);
	DECLARE_CLIENTCLASS();

public:
	C_TFVRHand();
	virtual ~C_TFVRHand();

	// Spawning and lifecycle (spawns two separate hand entities)
	static void SpawnVRHands(C_TFPlayer *pPlayer);
	static void RemoveVRHands(C_TFPlayer *pPlayer);
	
	bool Initialize(C_TFPlayer *pOwner, VRHandSide handSide);
	void Shutdown();

	// Spawn override
	virtual void Spawn() override;

	// Update methods called every frame
	virtual void ClientThink() override;
	void Update(); // Manual update method
	void UpdateHandTransform();
	void UpdateHandBones();
	
	// Helper to get wrist transform as matrix
	bool GetWristTransform(VMatrix& outTransform);

	// Bone setup override to position hand bones
	virtual bool SetupBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones, int boneMask, float currentTime) override;

	// Rendering control
	virtual bool ShouldDraw() override;
	virtual int DrawModel(int flags) override;
	virtual ShadowType_t ShadowCastType() override;  // Always cast shadows
	virtual bool ShouldReceiveProjectedTextures(int flags) override;  // Always receive shadows
	virtual bool IsTransparent() override { return false; }  // Force opaque for shadows
	virtual bool GetShadowCastDistance(float *pDist, ShadowType_t shadowType) const override;
	
	// Override to ensure hands are always in PVS
	virtual void GetRenderBounds(Vector& mins, Vector& maxs) override;

	// Bone mapping
	void SetupBoneMapping();
	bool MapOpenXRJointToBone(XrHandJointEXT joint, int &boneIndex);
	
	// Apply finger tracking to bones
	void ApplyFingerTracking(matrix3x4_t *pBoneToWorldOut, int nMaxBones);
	
	// Hide the opposite hand (scale to zero)
	void HideOppositeHand(matrix3x4_t *pBoneToWorldOut, int nMaxBones, CStudioHdr *pStudioHdr);

	// Accessors
	C_TFPlayer* GetOwnerPlayer() const { return m_hOwnerPlayer.Get(); }
	VRHandSide GetHandSide() const { return m_handSide; }
	bool IsLeftHand() const { return m_handSide == VR_HAND_LEFT; }
	bool IsRightHand() const { return m_handSide == VR_HAND_RIGHT; }
	
	// Weapon management
	void EquipWeapon(C_TFWeaponBase *pWeapon);
	void UnequipWeapon();
	C_TFWeaponBase* GetHeldWeapon() const { return m_hHeldWeapon.Get(); }
	C_BaseAnimating* GetRenderWeapon() const { return m_hRenderWeapon.Get(); }
	void UpdateWeaponTransform();
	void UpdateSkins();  // Sync skins for team colors, crit effects, etc.
	
	// Position weapon using already-computed bone matrices (called from SetupBones)
	void PositionWeaponFromBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones);
	
	// Get the weapon's muzzle position and angles in world space
	bool GetWeaponMuzzlePositionAndAngles(Vector &outPos, QAngle &outAngles);
	
	// Weapon pose override
	void ApplyWeaponPose(matrix3x4_t *pBoneToWorldOut, int nMaxBones);
	
	// Fire animation - trigger weapon fire animation
	void PlayWeaponFireAnimation();
	
	// Two-handed weapon support
	bool GetOffHandGripTarget(Vector &outPos, QAngle &outAngles);  // Get off-hand grip position from animation
	float GetTwoHandBlendAmount() const { return m_flTwoHandBlend; }
	void SetTwoHandBlendAmount(float blend) { m_flTwoHandBlend = blend; }
	bool IsTwoHanding() const { return m_flTwoHandBlend > 0.01f; }

private:
	// Which hand this entity represents
	VRHandSide m_handSide;
	
	// Owner
	CHandle<C_TFPlayer> m_hOwnerPlayer;

	// Held weapon
	CHandle<C_TFWeaponBase> m_hHeldWeapon;  // The actual weapon (for mechanics/firing)
	CHandle<C_BaseAnimating> m_hRenderWeapon;  // Separate render-only entity for visuals

	// Hand tracking
	COpenXRHandTracker *m_pHandTracker;
	bool m_bHandTrackingValid;
	
	// Shutdown flag
	bool m_bShuttingDown;
	
	// Last known player class - to detect class changes
	int m_iLastPlayerClass;

	// Bone index for hand root
	int m_iHandBone;

	// Bone mapping: OpenXR joint index -> Source bone index
	// -1 means no corresponding bone in the model
	// These map ALL XR_HAND_JOINT_COUNT_EXT joints (including fingers)
	int m_BoneMapping[XR_HAND_JOINT_COUNT_EXT];
	bool m_bBoneMappingSetup;

	// Tracking state for this controller
	bool m_bControllerTracked;
	Vector m_vecLastValidPosition;
	QAngle m_angLastValidAngles;

	// Model info
	char m_szModelName[MAX_PATH];
	
	// Fire animation
	int m_iFireSequence;           // Fire animation sequence index
	int m_iIdleSequence;           // Idle animation sequence to return to
	bool m_bPlayingFireAnim;       // Currently playing fire animation
	float m_flFireAnimStartTime;   // When fire animation started
	
	// Cached transform from idle hand bone to VR controller (calculated once)
	matrix3x4_t m_matIdleHandBoneTransform;  // Hand bone transform from idle pose
	bool m_bHandBoneOffsetValid;             // Whether the offset has been calculated
	
	// Two-handed weapon support
	float m_flTwoHandBlend;  // 0.0 = free hand, 1.0 = fully gripping weapon
	int m_iOffHandBone;      // Bone index for the off-hand grip point on weapon hand's model
};

// Global functions
void UpdateVRHands();
void CleanupAllVRHands();

// Helper to get the opposite hand
C_TFVRHand* GetOppositeVRHand(C_TFVRHand *pHand);

// Accessors for the local player's hands
C_TFVRHand* GetLocalPlayerLeftHand();
C_TFVRHand* GetLocalPlayerRightHand();

#endif // C_TFVR_HAND_H

