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
	COpenXRHandTracker* GetHandTracker() const { return m_pHandTracker; }
	
	// Weapon management
	void EquipWeapon(C_TFWeaponBase *pWeapon);
	void UnequipWeapon();
	C_TFWeaponBase* GetHeldWeapon() const { return m_hHeldWeapon.Get(); }
	C_BaseAnimating* GetRenderWeapon() const { return m_hRenderWeapon.Get(); }
	void UpdateWeaponTransform();
	void UpdateSkins();  // Sync skins for team colors, crit effects, etc.
	void UpdateCritBoostEffect();  // Update crit electricity effect
	
	// Position weapon using already-computed bone matrices (called from SetupBones)
	void PositionWeaponFromBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones);
	
	// Get the weapon's muzzle position and angles in world space
	bool GetWeaponMuzzlePositionAndAngles(Vector &outPos, QAngle &outAngles);
	
	// Get cached weapon bone world transform (for overlays to avoid bone cache issues)
	bool GetCachedWeaponBoneTransform(matrix3x4_t &outTransform) const;
	
	// Weapon pose override
	void ApplyWeaponPose(matrix3x4_t *pBoneToWorldOut, int nMaxBones);
	
	// Fire animation - trigger weapon fire animation
	void PlayWeaponFireAnimation();
	
	// Two-handed weapon support
	// bUseCurrentAnimation: false = use idle (for stable weapon rotation), true = use current (for visual positioning)
	bool GetOffHandGripTarget(Vector &outPos, QAngle &outAngles, bool bUseCurrentAnimation = false);
	float GetTwoHandBlendAmount() const { return m_flTwoHandBlend; }
	void SetTwoHandBlendAmount(float blend) { m_flTwoHandBlend = blend; }
	bool IsTwoHanding() const { return m_flTwoHandBlend > 0.01f; }
	
	// Offhand grip - when grip button is held and within range
	bool IsOffhandGripActive() const { return m_bOffhandGripActive; }
	bool WasOffhandGripActive() const { return m_bWasOffhandGripActive; }  // For blend-out tracking
	float GetGripRotationBlend() const { return m_flGripRotationBlend; }   // Separate blend for weapon rotation
	const Vector& GetOffhandGripForward() const { return m_vecOffhandGripForward; }
	const Vector& GetOffhandGripUp() const { return m_vecOffhandGripUp; }

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
	
	// Melee swing animation cycling (a -> b -> c -> a...)
	int m_iMeleeSwingIndex;        // Current swing variant (0=a, 1=b, 2=c)
	char m_szMeleeSwingBase[64];   // Base swing animation name (e.g., "b_swing_")
	int m_iMeleeSwingCount;        // Number of swing variants available (usually 3)
	
	// Cached transform from idle hand bone to VR controller (calculated once)
	matrix3x4_t m_matIdleHandBoneTransform;  // Hand bone transform from idle pose
	bool m_bHandBoneOffsetValid;             // Whether the offset has been calculated
	
	// Two-handed weapon support
	float m_flTwoHandBlend;  // 0.0 = free hand, 1.0 = fully gripping weapon
	int m_iOffHandBone;      // Bone index for the off-hand (bip_hand_L) on weapon hand's model
	int m_iOffHandMiddleFingerBone;  // Bone index for bip_middle_0_L (for bind pose offset calc)
	
	// Offhand grip state - active when grip button held + within range
	bool m_bOffhandGripActive;         // Is the offhand currently gripping the weapon
	bool m_bWasOffhandGripActive;      // Was grip active last frame (for blend-out tracking)
	float m_flGripRotationBlend;       // Separate blend for weapon rotation (0=no rotation, 1=full grip rotation)
	Vector m_vecOffhandGripForward;    // Desired forward direction (toward offhand)
	Vector m_vecOffhandGripUp;         // Up reference from weapon hand controller
	Vector m_vecCachedGripDelta;       // Cached initial pivot axis from grip start
	Vector m_vecCachedGripYAxis;       // Cached initial weapon Y-axis from grip start
	
	// Cached idle muzzle offset for pistols (to keep aim stable during fire anim)
	Vector m_vIdleMuzzleOffset;        // Muzzle offset relative to weapon_bone in idle pose
	QAngle m_angIdleMuzzleAngles;      // Muzzle angles relative to weapon_bone in idle pose
	bool m_bIdleMuzzleOffsetValid;     // Whether the offset has been calculated
	int m_iCachedMuzzleWeaponID;       // Weapon ID this was cached for (invalidate on weapon change)
	
	// Crit boost effect - attached to hand for proper timing
	CSmartPtr<CNewParticleEffect> m_pCritBoostEffect;
	bool m_bCritBoostActive;
	
	// Cached weapon bone world transform - set during PositionWeaponFromBones
	// This is used by overlays to avoid stale bone cache issues
	matrix3x4_t m_matWeaponBoneWorld;
	bool m_bWeaponBoneWorldValid;
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

