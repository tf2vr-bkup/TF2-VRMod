// Purpose: VR Hand entity implementation

#include "cbase.h"
#include "c_tfvr_hand.h"
#include "tf/c_tf_player.h"
#include "tf/tf_weaponbase.h"
#include "tf/tf_shareddefs.h"
#include "tf/tf_gamerules.h"
#include "tf/tf_weapon_minigun.h"
#include "tf/tf_weapon_medigun.h"
#include "tf/tf_weapon_grenadelauncher.h"
#include "tf/tf_weapon_flamethrower.h"
#include "tf/tf_weapon_bat.h"
#include "tf/tf_item_wearable.h"
#include "econ/econ_entity.h"
#include "econ/econ_item_schema.h"
#include "model_types.h"
#include "particles_new.h"
#include "tfvr/openxr_manager.h"
#include "tfvr/openxr_hand_tracking.h"
#include "tfvr/tfvr_weapon_base.h"
#include "bone_setup.h"
#include "engine/ivdebugoverlay.h"
#include "filesystem.h"
#include "econ/ihasowner.h"
#include "tfvr/vr_hand_render.h"
#include "cl_animevent.h"
#include "eventlist.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Purpose: Custom render weapon class that implements IHasOwner for material proxies
//          This allows crit glow and other effects to work properly
//-----------------------------------------------------------------------------
class C_VRRenderWeapon : public C_BaseAnimating, public IHasOwner
{
	DECLARE_CLASS(C_VRRenderWeapon, C_BaseAnimating);
	
public:
	C_VRRenderWeapon() : m_hOwnerPlayer(NULL), m_hSourceWeapon(NULL), m_iIdleSequence(0), m_iFireSequence(-1), m_bPlayingFireAnim(false), m_pCritBoostEffect(NULL), m_bCritBoostActive(false), m_iFireOnSequence(-1), m_iFireOffSequence(-1), m_iFireLoopSequence(-1), m_eMedigunFireState(MEDIGUN_FIRE_IDLE), m_bInSetupBones(false) {}
	
	void SetOwnerPlayer(C_TFPlayer *pPlayer) { m_hOwnerPlayer = pPlayer; }
	void SetSourceWeapon(C_TFWeaponBase *pWeapon) { m_hSourceWeapon = pWeapon; }

	// Ensure VR hand has positioned us before any bone/attachment queries.
	// This fixes particle effects, muzzle flashes, and other systems that query
	// attachment positions before the hand's ClientThink has run this frame.
	// Ensure the owning VR hand has positioned us before any bone/attachment queries.
	// This fixes particle effects, muzzle flashes, and other systems that query
	// attachment positions before the hand's ClientThink has run this frame.
	virtual bool SetupBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones, int boneMask, float currentTime) OVERRIDE
	{
		if (!m_bInSetupBones)
		{
			m_bInSetupBones = true;

			// Must pass a real bone array — the hand's SetupBones returns early
			// without running PositionWeaponFromBones if pBoneToWorldOut is NULL
			matrix3x4_t handBones[MAXSTUDIOBONES];

			C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
			if (pRightHand && pRightHand->GetRenderWeapon() == this)
			{
				pRightHand->SetupBones(handBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, currentTime);
			}
			else
			{
				C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
				if (pLeftHand && pLeftHand->GetRenderWeapon() == this)
				{
					pLeftHand->SetupBones(handBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, currentTime);
				}
			}
			m_bInSetupBones = false;
		}
		return BaseClass::SetupBones(pBoneToWorldOut, nMaxBones, boneMask, currentTime);
	}
	
	// Suppress sound events on the render weapon -- the viewmodel's FireEvent
	// redirects all weapon sounds (draw, reload, idle, etc.) to this entity's
	// position already, so letting them through here would cause duplication.
	virtual void FireEvent( const Vector& origin, const QAngle& angles, int event, const char *options ) OVERRIDE
	{
		if ( event == AE_CL_PLAYSOUND || event == CL_EVENT_SOUND )
			return;
		BaseClass::FireEvent( origin, angles, event, options );
	}

	// IHasOwner interface
	virtual CBaseEntity *GetOwnerViaInterface(void) OVERRIDE
	{
		return m_hOwnerPlayer.Get();
	}
	
	// Fire animation support - set the fire sequence index directly from the hand
	void SetFireSequence(int iSequence)
	{
		m_iFireSequence = iSequence;
	}
	
	void SetupAnimations()
	{
		extern ConVar tfvr_weapon_fire_anim_debug;
		
		// Look up idle sequence on weapon model
		m_iIdleSequence = LookupSequence("idle");
		if (m_iIdleSequence < 0)
			m_iIdleSequence = LookupSequence("idle01");
		if (m_iIdleSequence < 0)
			m_iIdleSequence = LookupSequence("seq_idle");
		if (m_iIdleSequence < 0)
			m_iIdleSequence = 0;  // Fallback to first sequence
		
		// Set the initial animation so animated parts (levers, etc.) start in the correct position
		SetSequence(m_iIdleSequence);
		SetCycle(0.0f);
		SetPlaybackRate(1.0f);
		
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Weapon model animation setup - fire seq: %d (from hand), idle seq: %d\n", 
				m_iFireSequence, m_iIdleSequence);
		}
	}
	
	void PlayFireAnimation()
	{
		extern ConVar tfvr_weapon_fire_anim;
		extern ConVar tfvr_weapon_fire_anim_debug;
		
		if (!tfvr_weapon_fire_anim.GetBool())
			return;
		
		// Try to find a fire animation on the weapon model itself
		// Common names: "fire", "shoot", "ref"
		int fireSeq = -1;
		if (m_iFireSequence >= 0)
		{
			fireSeq = m_iFireSequence;
		}
		else
		{
			// Try common fire animation names for world models
			fireSeq = LookupSequence("fire");
			if (fireSeq < 0)
				fireSeq = LookupSequence("shoot");
			if (fireSeq < 0)
				fireSeq = LookupSequence("ref"); // Reference pose animation
		}
		
		if (fireSeq >= 0)
		{
			SetSequence(fireSeq);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
			m_bPlayingFireAnim = true;
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Playing weapon fire animation (sequence %d)\n", fireSeq);
			}
		}
		else if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: No fire animation found for this weapon model\n");
		}
	}
	
	void SetupMedigunAnimations()
	{
		m_iFireOnSequence = LookupSequence("fire_on");
		m_iFireLoopSequence = LookupSequence("fire_loop");
		m_iFireOffSequence = LookupSequence("fire_off");
		m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
		
		extern ConVar tfvr_weapon_fire_anim_debug;
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Render weapon medigun sequences - fire_on: %d, fire_loop: %d, fire_off: %d\n",
				m_iFireOnSequence, m_iFireLoopSequence, m_iFireOffSequence);
		}
	}
	
	void PlayMedigunSequence(MedigunFireState state)
	{
		int seq = -1;
		switch (state)
		{
		case MEDIGUN_FIRE_ON:   seq = m_iFireOnSequence; break;
		case MEDIGUN_FIRE_LOOP: seq = m_iFireLoopSequence; break;
		case MEDIGUN_FIRE_OFF:  seq = m_iFireOffSequence; break;
		case MEDIGUN_FIRE_IDLE:
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
			}
			m_bPlayingFireAnim = false;
			m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
			return;
		}
		
		if (seq >= 0)
		{
			SetSequence(seq);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
			m_bPlayingFireAnim = true;
			m_eMedigunFireState = state;
		}
	}
	
	// ClientThink - called every frame when SetNextClientThink(CLIENT_THINK_ALWAYS) is set
	virtual void ClientThink() OVERRIDE
	{
		BaseClass::ClientThink();
		
		// Advance animations
		StudioFrameAdvance();
		
		// Check if fire animation has completed (skip for medigun - driven by hand)
		if (m_bPlayingFireAnim && m_eMedigunFireState == MEDIGUN_FIRE_IDLE && GetCycle() >= 1.0f)
		{
			// Return to idle
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
			}
			m_bPlayingFireAnim = false;
		}
		
		// Update crit boost effect each frame
		UpdateCritBoostEffect();
	}
	
	// Override to return to idle after fire animation completes (backup for when FrameAdvance is called directly)
	virtual float FrameAdvance(float flInterval = 0.0f) OVERRIDE
	{
		float flReturn = BaseClass::FrameAdvance(flInterval);
		
		// Check if fire animation has completed (skip for medigun - driven by hand)
		if (m_bPlayingFireAnim && m_eMedigunFireState == MEDIGUN_FIRE_IDLE && GetCycle() >= 1.0f)
		{
			// Return to idle
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
			}
			m_bPlayingFireAnim = false;
		}
		
		return flReturn;
	}
	
	bool HasFireAnimation() const { return m_iFireSequence >= 0; }
	
	// Copy attached models (festivizers, etc.) from the source weapon
	void CopyAttachedModels(C_TFWeaponBase *pSourceWeapon)
	{
		m_vecAttachedModels.Purge();
		
		if (!pSourceWeapon)
			return;
		
		// Copy from source weapon's attached models
		for (int i = 0; i < pSourceWeapon->m_vecAttachedModels.Count(); i++)
		{
			m_vecAttachedModels.AddToTail(pSourceWeapon->m_vecAttachedModels[i]);
		}
	}
	
	// Sync particle effects from the source weapon to this VR render weapon
	void SyncParticleEffects()
	{
		C_TFWeaponBase *pSourceWeapon = m_hSourceWeapon.Get();
		if (!pSourceWeapon)
			return;
		
		// Get the particle systems from the source weapon
		CUtlVector<const attachedparticlesystem_t *> vecParticleSystems;
		pSourceWeapon->GetEconParticleSystems(&vecParticleSystems);
		
		if (vecParticleSystems.Count() == 0)
			return;
		
		// We can't have fastcull on if we want particles attached to us
		RemoveEffects(EF_BONEMERGE_FASTCULL);
		
		FOR_EACH_VEC(vecParticleSystems, i)
		{
			const attachedparticlesystem_t *pSystem = vecParticleSystems[i];
			if (!pSystem || !pSystem->pszSystemName || !pSystem->pszSystemName[0])
				continue;
			
			// Skip custom type particles (weapons handle them in custom ways)
			if (pSystem->iCustomType)
				continue;
			
			// Check if this particle system exists
			if (g_pParticleSystemMgr->FindParticleSystem(pSystem->pszSystemName) == NULL)
				continue;
			
			// Get attachment point
			const char *pszAttachmentName = pSystem->pszControlPoints[0];
			int iAttachment = INVALID_PARTICLE_ATTACHMENT;
			if (pszAttachmentName && pszAttachmentName[0])
			{
				iAttachment = LookupAttachment(pszAttachmentName);
			}
			
			// Create the particle effect
			CNewParticleEffect *pEffect = NULL;
			if (iAttachment != INVALID_PARTICLE_ATTACHMENT)
			{
				pEffect = ParticleProp()->Create(pSystem->pszSystemName, PATTACH_POINT_FOLLOW, pszAttachmentName);
			}
			else if (pSystem->bFollowRootBone)
			{
				pEffect = ParticleProp()->Create(pSystem->pszSystemName, PATTACH_ROOTBONE_FOLLOW);
			}
			else
			{
				pEffect = ParticleProp()->Create(pSystem->pszSystemName, PATTACH_ABSORIGIN_FOLLOW);
			}
			
			if (pEffect)
			{
				// Add additional control points if defined
				for (int j = 1; j < ARRAYSIZE(pSystem->pszControlPoints); ++j)
				{
					const char *pszControlPointName = pSystem->pszControlPoints[j];
					if (pszControlPointName && pszControlPointName[0] != '\0')
					{
						ParticleProp()->AddControlPoint(pEffect, j, this, PATTACH_POINT_FOLLOW, pszControlPointName);
					}
				}
			}
		}
	}
	
	// Stop all particle effects on this render weapon
	void StopParticleEffects()
	{
		ParticleProp()->StopEmission();
		
		// Also clean up crit boost effect
		if (m_pCritBoostEffect.IsValid())
		{
			m_pCritBoostEffect->StopEmission();
			m_pCritBoostEffect = NULL;
		}
	}
	
	// Crit boost is now handled by the hand entity for proper timing
	void UpdateCritBoostEffect() {}
	
	// Returns true when owner is cloaking (for transparency rendering)
	virtual bool IsTransparent() OVERRIDE
	{
		C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
		if (pOwner)
		{
			return pOwner->GetPercentInvisible() > 0.0f;
		}
		return false;
	}
	
	// Override DrawModel to apply ubercharge effect
	virtual int DrawModel(int flags) OVERRIDE
	{
		// VR hand layer: skip during world pass, the owning hand will register us
		if (VRHandLayer_ShouldSkipDraw())
		{
			VRHandLayer_AddRenderable(this);
			return 0;
		}

		C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
		if (!pOwner)
			return 0;
		
		int ret = 0;
		bool bInvuln = pOwner->m_Shared.IsInvulnerable();
		
		// Apply ubercharge material override
		if (bInvuln && (flags & STUDIO_RENDER))
		{
			modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());
		}
		
		ret = BaseClass::DrawModel(flags);
		
		// Reset material override
		if (bInvuln && (flags & STUDIO_RENDER))
		{
			modelrender->ForcedMaterialOverride(NULL);
		}
		
		return ret;
	}
	
	// Override to draw attached models (festivizers, etc.)
	virtual bool OnInternalDrawModel(ClientModelRenderInfo_t *pInfo) OVERRIDE
	{
		if (!BaseClass::OnInternalDrawModel(pInfo))
			return false;
		
		// Draw attached models (festivizers, bot-killers, etc.)
		for (int i = 0; i < m_vecAttachedModels.Count(); i++)
		{
			const AttachedModelData_t& attachedModel = m_vecAttachedModels[i];
			
			// Use world model display flag since we're in VR world space
			if (attachedModel.m_pModel && (attachedModel.m_iModelDisplayFlags & kAttachedModelDisplayFlag_WorldModel))
			{
				ClientModelRenderInfo_t infoAttached = *pInfo;
				
				infoAttached.pRenderable = this;
				infoAttached.instance = MODEL_INSTANCE_INVALID;
				infoAttached.entity_index = this->index;
				infoAttached.pModel = attachedModel.m_pModel;
				infoAttached.pModelToWorld = &infoAttached.modelToWorld;
				
				// Turns the origin + angles into a matrix
				AngleMatrix(infoAttached.angles, infoAttached.origin, infoAttached.modelToWorld);
				
				DrawModelState_t state;
				matrix3x4_t *pBoneToWorld = NULL;
				bool bMarkAsDrawn = modelrender->DrawModelSetup(infoAttached, &state, NULL, &pBoneToWorld);
				DoInternalDrawModel(&infoAttached, (bMarkAsDrawn && (infoAttached.flags & STUDIO_RENDER)) ? &state : NULL, pBoneToWorld);
			}
		}
		
		return true;
	}
	
	// Storage for attached models (copied from source weapon)
	CUtlVector<AttachedModelData_t> m_vecAttachedModels;
	
	// Override StandardBlendingRules to apply weapon's procedural bone rotations
	// The weapon itself updates these values in ItemPreFrame, we just read and apply them
	virtual void StandardBlendingRules( CStudioHdr *hdr, Vector pos[], Quaternion q[], float currentTime, int boneMask ) OVERRIDE
	{
		BaseClass::StandardBlendingRules(hdr, pos, q, currentTime, boneMask);
		
		// Apply procedural bone rotations from the source weapon
		// The weapon has already computed these values, we just apply them to our bones
		C_TFWeaponBase *pWeapon = m_hSourceWeapon.Get();
		if (pWeapon && hdr)
		{
			// Handle minigun barrel rotation
			CTFMinigun *pMinigun = dynamic_cast<CTFMinigun*>(pWeapon);
			if (pMinigun)
			{
				int iBarrelBone = Studio_BoneIndexByName(hdr, "barrel");
				if (iBarrelBone >= 0)
				{
					// Get the already-computed barrel angle from the weapon
					float flBarrelAngle = pMinigun->GetBarrelRotation();
					AngleQuaternion(RadianEuler(0, 0, flBarrelAngle), q[iBarrelBone]);
				}
			}
			
			// Handle grenade launcher barrel rotation
			CTFGrenadeLauncher *pGrenadeLauncher = dynamic_cast<CTFGrenadeLauncher*>(pWeapon);
			if (pGrenadeLauncher)
			{
				// The grenade launcher uses ViewModelAttachmentBlending which is called
				// by the weapon's StandardBlendingRules, but we need to avoid double-updating
				// Just call ViewModelAttachmentBlending directly on our render weapon
				pWeapon->ViewModelAttachmentBlending(hdr, pos, q, currentTime, boneMask);
			}
		}
	}
	
	
private:
	CHandle<C_TFPlayer> m_hOwnerPlayer;
	CHandle<C_TFWeaponBase> m_hSourceWeapon;
	int m_iIdleSequence;
	int m_iFireSequence;
	bool m_bPlayingFireAnim;
	CSmartPtr<CNewParticleEffect> m_pCritBoostEffect;
	bool m_bCritBoostActive;
	bool m_bInSetupBones;
	
	// Medigun fire animation state
	int m_iFireOnSequence;
	int m_iFireOffSequence;
	int m_iFireLoopSequence;
	MedigunFireState m_eMedigunFireState;
};

// Forward declarations
static const char* GetFistsIdleAnimName(C_TFWeaponBase *pWeapon);

// ConVars for debugging and control
ConVar tfvr_hands_enabled("tfvr_hands_enabled", "1", FCVAR_ARCHIVE, "Enable VR hand rendering");
ConVar tfvr_hands_debug("tfvr_hands_debug", "0", FCVAR_NONE, "Show debug info for VR hands");
ConVar tfvr_hands_debug_bones("tfvr_hands_debug_bones", "0", FCVAR_NONE, "Draw bone positions on VR hands/weapons (1=hand, 2=weapon, 3=both)");
ConVar tfvr_hands_alpha("tfvr_hands_alpha", "1.0", FCVAR_ARCHIVE, "Alpha transparency for VR hands (0-1)");
ConVar tfvr_hands_finger_tracking("tfvr_hands_finger_tracking", "1", FCVAR_ARCHIVE, "Enable finger tracking animation (0=disable, 1=enable)");
ConVar tfvr_hands_animate_thumb_metacarpal("tfvr_hands_animate_thumb_metacarpal", "0", FCVAR_ARCHIVE, "Animate thumb metacarpal bone (usually should be 0)");

// Finger rotation offset convars (to align OpenXR joint orientation with model bone orientation)
// Separate offsets for left and right hands since they're mirrored
ConVar tfvr_hands_finger_offset_pitch_L("tfvr_hands_finger_offset_pitch_L", "0", FCVAR_ARCHIVE, "Pitch offset for LEFT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_yaw_L("tfvr_hands_finger_offset_yaw_L", "90", FCVAR_ARCHIVE, "Yaw offset for LEFT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_roll_L("tfvr_hands_finger_offset_roll_L", "0", FCVAR_ARCHIVE, "Roll offset for LEFT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_pitch_R("tfvr_hands_finger_offset_pitch_R", "180", FCVAR_ARCHIVE, "Pitch offset for RIGHT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_yaw_R("tfvr_hands_finger_offset_yaw_R", "-90", FCVAR_ARCHIVE, "Yaw offset for RIGHT hand finger bones (degrees)");
ConVar tfvr_hands_finger_offset_roll_R("tfvr_hands_finger_offset_roll_R", "0", FCVAR_ARCHIVE, "Roll offset for RIGHT hand finger bones (degrees)");

// Rotation offset convars - left hand
ConVar tfvr_hands_left_offset_pitch("tfvr_hands_left_offset_pitch", "-75", FCVAR_ARCHIVE, "Pitch offset for left VR hand (degrees)");
ConVar tfvr_hands_left_offset_yaw("tfvr_hands_left_offset_yaw", "125", FCVAR_ARCHIVE, "Yaw offset for left VR hand (degrees)");

// Shadow convars for debugging
ConVar tfvr_hands_shadow_bounds("tfvr_hands_shadow_bounds", "10000", FCVAR_CHEAT, "Render bounds size for VR hands (affects shadow culling)");
ConVar tfvr_hands_shadow_distance("tfvr_hands_shadow_distance", "2000", FCVAR_CHEAT, "Shadow cast distance for VR hands");
ConVar tfvr_hands_shadow_type("tfvr_hands_shadow_type", "2", FCVAR_CHEAT, "Shadow type for VR hands (0=none, 1=simple, 2=texture, 3=texture_dynamic)");
ConVar tfvr_hands_shadow_debug("tfvr_hands_shadow_debug", "0", FCVAR_CHEAT, "Show shadow debug info for VR hands");

// Muzzle position mode for effects (sounds, muzzle flash, tracers)
ConVar tfvr_muzzle_direct_mode("tfvr_muzzle_direct_mode", "0", FCVAR_ARCHIVE, "Use controller pose directly for muzzle position (0=attachment system, 1=direct controller+offset)");

// Two-handed weapon convars
ConVar tfvr_twohand_enabled("tfvr_twohand_enabled", "1", FCVAR_ARCHIVE, "Enable two-handed weapon gripping");
ConVar tfvr_twohand_snap_distance("tfvr_twohand_snap_distance", "8", FCVAR_ARCHIVE, "Distance (inches) at which off-hand snaps to weapon grip");
ConVar tfvr_twohand_blend_distance("tfvr_twohand_blend_distance", "1", FCVAR_ARCHIVE, "Distance (inches) at which off-hand starts blending towards weapon grip");
ConVar tfvr_twohand_debug("tfvr_twohand_debug", "0", FCVAR_CHEAT, "Show two-handed grip debug info");

// Offhand grip convars - grip button must be held to activate
ConVar tfvr_offhand_grip_enabled("tfvr_offhand_grip_enabled", "1", FCVAR_ARCHIVE, "Enable offhand grip for two-handed weapon aiming");
ConVar tfvr_offhand_grip_range("tfvr_offhand_grip_range", "25", FCVAR_ARCHIVE, "Distance (cm) at which offhand grip can activate");
ConVar tfvr_offhand_grip_release_mult("tfvr_offhand_grip_release_mult", "2", FCVAR_ARCHIVE, "Multiplier for release distance (hysteresis to prevent accidental ungrip)");
ConVar tfvr_offhand_grip_threshold("tfvr_offhand_grip_threshold", "0.5", FCVAR_ARCHIVE, "Grip button threshold (0-1) to activate offhand grip");
ConVar tfvr_offhand_grip_blend_speed("tfvr_offhand_grip_blend_speed", "15", FCVAR_ARCHIVE, "Speed of hand position grip/ungrip transition (higher = faster)");
ConVar tfvr_offhand_grip_rotation_blend_speed("tfvr_offhand_grip_rotation_blend_speed", "8", FCVAR_ARCHIVE, "Speed of weapon rotation grip/ungrip transition (higher = faster)");
ConVar tfvr_offhand_grip_ease_power("tfvr_offhand_grip_ease_power", "1.1", FCVAR_ARCHIVE, "Easing power for grip transitions (1=linear, 2+=ease-out, higher=sharper)");
ConVar tfvr_offhand_grip_no_anchor("tfvr_offhand_grip_no_anchor", "0", FCVAR_CHEAT, "DEBUG: Disable anchor offset when gripping (use controller directly)");
ConVar tfvr_hands_left_offset_roll("tfvr_hands_left_offset_roll", "0", FCVAR_ARCHIVE, "Roll offset for left VR hand (degrees)");

// Rotation offset convars - right hand (DEFAULT/GLOBAL offsets)
ConVar tfvr_hands_right_offset_pitch("tfvr_hands_right_offset_pitch", "110", FCVAR_ARCHIVE, "Pitch offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_yaw("tfvr_hands_right_offset_yaw", "-125", FCVAR_ARCHIVE, "Yaw offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_roll("tfvr_hands_right_offset_roll", "0", FCVAR_ARCHIVE, "Roll offset for right VR hand (degrees)");

// Per-class offset enable - when enabled, class-specific offsets override global offsets
ConVar tfvr_hands_perclass_offsets("tfvr_hands_perclass_offsets", "0", FCVAR_ARCHIVE, "Enable per-class hand offsets (0=use global, 1=use per-class)");

// Per-class offset convars - LEFT hand
// Scout
ConVar tfvr_hands_scout_left_pitch("tfvr_hands_scout_left_pitch", "-75", FCVAR_ARCHIVE, "Scout: left hand pitch offset");
ConVar tfvr_hands_scout_left_yaw("tfvr_hands_scout_left_yaw", "125", FCVAR_ARCHIVE, "Scout: left hand yaw offset");
ConVar tfvr_hands_scout_left_roll("tfvr_hands_scout_left_roll", "0", FCVAR_ARCHIVE, "Scout: left hand roll offset");
// Soldier
ConVar tfvr_hands_soldier_left_pitch("tfvr_hands_soldier_left_pitch", "-75", FCVAR_ARCHIVE, "Soldier: left hand pitch offset");
ConVar tfvr_hands_soldier_left_yaw("tfvr_hands_soldier_left_yaw", "125", FCVAR_ARCHIVE, "Soldier: left hand yaw offset");
ConVar tfvr_hands_soldier_left_roll("tfvr_hands_soldier_left_roll", "0", FCVAR_ARCHIVE, "Soldier: left hand roll offset");
// Pyro
ConVar tfvr_hands_pyro_left_pitch("tfvr_hands_pyro_left_pitch", "-75", FCVAR_ARCHIVE, "Pyro: left hand pitch offset");
ConVar tfvr_hands_pyro_left_yaw("tfvr_hands_pyro_left_yaw", "125", FCVAR_ARCHIVE, "Pyro: left hand yaw offset");
ConVar tfvr_hands_pyro_left_roll("tfvr_hands_pyro_left_roll", "0", FCVAR_ARCHIVE, "Pyro: left hand roll offset");
// Demoman
ConVar tfvr_hands_demo_left_pitch("tfvr_hands_demo_left_pitch", "-75", FCVAR_ARCHIVE, "Demo: left hand pitch offset");
ConVar tfvr_hands_demo_left_yaw("tfvr_hands_demo_left_yaw", "125", FCVAR_ARCHIVE, "Demo: left hand yaw offset");
ConVar tfvr_hands_demo_left_roll("tfvr_hands_demo_left_roll", "0", FCVAR_ARCHIVE, "Demo: left hand roll offset");
// Heavy
ConVar tfvr_hands_heavy_left_pitch("tfvr_hands_heavy_left_pitch", "-75", FCVAR_ARCHIVE, "Heavy: left hand pitch offset");
ConVar tfvr_hands_heavy_left_yaw("tfvr_hands_heavy_left_yaw", "125", FCVAR_ARCHIVE, "Heavy: left hand yaw offset");
ConVar tfvr_hands_heavy_left_roll("tfvr_hands_heavy_left_roll", "0", FCVAR_ARCHIVE, "Heavy: left hand roll offset");
// Engineer
ConVar tfvr_hands_engi_left_pitch("tfvr_hands_engi_left_pitch", "-75", FCVAR_ARCHIVE, "Engineer: left hand pitch offset");
ConVar tfvr_hands_engi_left_yaw("tfvr_hands_engi_left_yaw", "125", FCVAR_ARCHIVE, "Engineer: left hand yaw offset");
ConVar tfvr_hands_engi_left_roll("tfvr_hands_engi_left_roll", "0", FCVAR_ARCHIVE, "Engineer: left hand roll offset");
// Medic
ConVar tfvr_hands_medic_left_pitch("tfvr_hands_medic_left_pitch", "-75", FCVAR_ARCHIVE, "Medic: left hand pitch offset");
ConVar tfvr_hands_medic_left_yaw("tfvr_hands_medic_left_yaw", "125", FCVAR_ARCHIVE, "Medic: left hand yaw offset");
ConVar tfvr_hands_medic_left_roll("tfvr_hands_medic_left_roll", "0", FCVAR_ARCHIVE, "Medic: left hand roll offset");
// Sniper
ConVar tfvr_hands_sniper_left_pitch("tfvr_hands_sniper_left_pitch", "-75", FCVAR_ARCHIVE, "Sniper: left hand pitch offset");
ConVar tfvr_hands_sniper_left_yaw("tfvr_hands_sniper_left_yaw", "125", FCVAR_ARCHIVE, "Sniper: left hand yaw offset");
ConVar tfvr_hands_sniper_left_roll("tfvr_hands_sniper_left_roll", "0", FCVAR_ARCHIVE, "Sniper: left hand roll offset");
// Spy
ConVar tfvr_hands_spy_left_pitch("tfvr_hands_spy_left_pitch", "-75", FCVAR_ARCHIVE, "Spy: left hand pitch offset");
ConVar tfvr_hands_spy_left_yaw("tfvr_hands_spy_left_yaw", "125", FCVAR_ARCHIVE, "Spy: left hand yaw offset");
ConVar tfvr_hands_spy_left_roll("tfvr_hands_spy_left_roll", "0", FCVAR_ARCHIVE, "Spy: left hand roll offset");

// Per-class offset convars - RIGHT hand
// Scout
ConVar tfvr_hands_scout_right_pitch("tfvr_hands_scout_right_pitch", "110", FCVAR_ARCHIVE, "Scout: right hand pitch offset");
ConVar tfvr_hands_scout_right_yaw("tfvr_hands_scout_right_yaw", "-125", FCVAR_ARCHIVE, "Scout: right hand yaw offset");
ConVar tfvr_hands_scout_right_roll("tfvr_hands_scout_right_roll", "0", FCVAR_ARCHIVE, "Scout: right hand roll offset");
// Soldier
ConVar tfvr_hands_soldier_right_pitch("tfvr_hands_soldier_right_pitch", "110", FCVAR_ARCHIVE, "Soldier: right hand pitch offset");
ConVar tfvr_hands_soldier_right_yaw("tfvr_hands_soldier_right_yaw", "-125", FCVAR_ARCHIVE, "Soldier: right hand yaw offset");
ConVar tfvr_hands_soldier_right_roll("tfvr_hands_soldier_right_roll", "0", FCVAR_ARCHIVE, "Soldier: right hand roll offset");
// Pyro
ConVar tfvr_hands_pyro_right_pitch("tfvr_hands_pyro_right_pitch", "110", FCVAR_ARCHIVE, "Pyro: right hand pitch offset");
ConVar tfvr_hands_pyro_right_yaw("tfvr_hands_pyro_right_yaw", "-125", FCVAR_ARCHIVE, "Pyro: right hand yaw offset");
ConVar tfvr_hands_pyro_right_roll("tfvr_hands_pyro_right_roll", "0", FCVAR_ARCHIVE, "Pyro: right hand roll offset");
// Demoman
ConVar tfvr_hands_demo_right_pitch("tfvr_hands_demo_right_pitch", "110", FCVAR_ARCHIVE, "Demo: right hand pitch offset");
ConVar tfvr_hands_demo_right_yaw("tfvr_hands_demo_right_yaw", "-125", FCVAR_ARCHIVE, "Demo: right hand yaw offset");
ConVar tfvr_hands_demo_right_roll("tfvr_hands_demo_right_roll", "0", FCVAR_ARCHIVE, "Demo: right hand roll offset");
// Heavy
ConVar tfvr_hands_heavy_right_pitch("tfvr_hands_heavy_right_pitch", "110", FCVAR_ARCHIVE, "Heavy: right hand pitch offset");
ConVar tfvr_hands_heavy_right_yaw("tfvr_hands_heavy_right_yaw", "-125", FCVAR_ARCHIVE, "Heavy: right hand yaw offset");
ConVar tfvr_hands_heavy_right_roll("tfvr_hands_heavy_right_roll", "0", FCVAR_ARCHIVE, "Heavy: right hand roll offset");
// Engineer
ConVar tfvr_hands_engi_right_pitch("tfvr_hands_engi_right_pitch", "110", FCVAR_ARCHIVE, "Engineer: right hand pitch offset");
ConVar tfvr_hands_engi_right_yaw("tfvr_hands_engi_right_yaw", "-125", FCVAR_ARCHIVE, "Engineer: right hand yaw offset");
ConVar tfvr_hands_engi_right_roll("tfvr_hands_engi_right_roll", "0", FCVAR_ARCHIVE, "Engineer: right hand roll offset");
// Medic
ConVar tfvr_hands_medic_right_pitch("tfvr_hands_medic_right_pitch", "110", FCVAR_ARCHIVE, "Medic: right hand pitch offset");
ConVar tfvr_hands_medic_right_yaw("tfvr_hands_medic_right_yaw", "-125", FCVAR_ARCHIVE, "Medic: right hand yaw offset");
ConVar tfvr_hands_medic_right_roll("tfvr_hands_medic_right_roll", "0", FCVAR_ARCHIVE, "Medic: right hand roll offset");
// Sniper
ConVar tfvr_hands_sniper_right_pitch("tfvr_hands_sniper_right_pitch", "110", FCVAR_ARCHIVE, "Sniper: right hand pitch offset");
ConVar tfvr_hands_sniper_right_yaw("tfvr_hands_sniper_right_yaw", "-125", FCVAR_ARCHIVE, "Sniper: right hand yaw offset");
ConVar tfvr_hands_sniper_right_roll("tfvr_hands_sniper_right_roll", "0", FCVAR_ARCHIVE, "Sniper: right hand roll offset");
// Spy
ConVar tfvr_hands_spy_right_pitch("tfvr_hands_spy_right_pitch", "110", FCVAR_ARCHIVE, "Spy: right hand pitch offset");
ConVar tfvr_hands_spy_right_yaw("tfvr_hands_spy_right_yaw", "-125", FCVAR_ARCHIVE, "Spy: right hand yaw offset");
ConVar tfvr_hands_spy_right_roll("tfvr_hands_spy_right_roll", "0", FCVAR_ARCHIVE, "Spy: right hand roll offset");

// Global positional offset convars (palm-relative: X=toward fingers, Y=out of palm, Z=toward thumb)
ConVar tfvr_hands_left_offset_x("tfvr_hands_left_offset_x", "0", FCVAR_ARCHIVE, "Left hand position offset X (toward fingers)");
ConVar tfvr_hands_left_offset_y("tfvr_hands_left_offset_y", "0", FCVAR_ARCHIVE, "Left hand position offset Y (out of palm)");
ConVar tfvr_hands_left_offset_z("tfvr_hands_left_offset_z", "0", FCVAR_ARCHIVE, "Left hand position offset Z (toward thumb)");
ConVar tfvr_hands_right_offset_x("tfvr_hands_right_offset_x", "0", FCVAR_ARCHIVE, "Right hand position offset X (toward fingers)");
ConVar tfvr_hands_right_offset_y("tfvr_hands_right_offset_y", "0", FCVAR_ARCHIVE, "Right hand position offset Y (out of palm)");
ConVar tfvr_hands_right_offset_z("tfvr_hands_right_offset_z", "0", FCVAR_ARCHIVE, "Right hand position offset Z (toward thumb)");

// Per-class positional offset convars - LEFT hand (X=toward fingers, Y=out of palm, Z=toward thumb)
ConVar tfvr_hands_scout_left_x("tfvr_hands_scout_left_x", "0", FCVAR_ARCHIVE, "Scout: left hand position X");
ConVar tfvr_hands_scout_left_y("tfvr_hands_scout_left_y", "0", FCVAR_ARCHIVE, "Scout: left hand position Y");
ConVar tfvr_hands_scout_left_z("tfvr_hands_scout_left_z", "0", FCVAR_ARCHIVE, "Scout: left hand position Z");
ConVar tfvr_hands_soldier_left_x("tfvr_hands_soldier_left_x", "0", FCVAR_ARCHIVE, "Soldier: left hand position X");
ConVar tfvr_hands_soldier_left_y("tfvr_hands_soldier_left_y", "0", FCVAR_ARCHIVE, "Soldier: left hand position Y");
ConVar tfvr_hands_soldier_left_z("tfvr_hands_soldier_left_z", "0", FCVAR_ARCHIVE, "Soldier: left hand position Z");
ConVar tfvr_hands_pyro_left_x("tfvr_hands_pyro_left_x", "0", FCVAR_ARCHIVE, "Pyro: left hand position X");
ConVar tfvr_hands_pyro_left_y("tfvr_hands_pyro_left_y", "0", FCVAR_ARCHIVE, "Pyro: left hand position Y");
ConVar tfvr_hands_pyro_left_z("tfvr_hands_pyro_left_z", "0", FCVAR_ARCHIVE, "Pyro: left hand position Z");
ConVar tfvr_hands_demo_left_x("tfvr_hands_demo_left_x", "0", FCVAR_ARCHIVE, "Demo: left hand position X");
ConVar tfvr_hands_demo_left_y("tfvr_hands_demo_left_y", "0", FCVAR_ARCHIVE, "Demo: left hand position Y");
ConVar tfvr_hands_demo_left_z("tfvr_hands_demo_left_z", "0", FCVAR_ARCHIVE, "Demo: left hand position Z");
ConVar tfvr_hands_heavy_left_x("tfvr_hands_heavy_left_x", "0", FCVAR_ARCHIVE, "Heavy: left hand position X");
ConVar tfvr_hands_heavy_left_y("tfvr_hands_heavy_left_y", "0", FCVAR_ARCHIVE, "Heavy: left hand position Y");
ConVar tfvr_hands_heavy_left_z("tfvr_hands_heavy_left_z", "0", FCVAR_ARCHIVE, "Heavy: left hand position Z");
ConVar tfvr_hands_engi_left_x("tfvr_hands_engi_left_x", "0", FCVAR_ARCHIVE, "Engineer: left hand position X");
ConVar tfvr_hands_engi_left_y("tfvr_hands_engi_left_y", "0", FCVAR_ARCHIVE, "Engineer: left hand position Y");
ConVar tfvr_hands_engi_left_z("tfvr_hands_engi_left_z", "0", FCVAR_ARCHIVE, "Engineer: left hand position Z");
ConVar tfvr_hands_medic_left_x("tfvr_hands_medic_left_x", "0", FCVAR_ARCHIVE, "Medic: left hand position X");
ConVar tfvr_hands_medic_left_y("tfvr_hands_medic_left_y", "0", FCVAR_ARCHIVE, "Medic: left hand position Y");
ConVar tfvr_hands_medic_left_z("tfvr_hands_medic_left_z", "0", FCVAR_ARCHIVE, "Medic: left hand position Z");
ConVar tfvr_hands_sniper_left_x("tfvr_hands_sniper_left_x", "0", FCVAR_ARCHIVE, "Sniper: left hand position X");
ConVar tfvr_hands_sniper_left_y("tfvr_hands_sniper_left_y", "0", FCVAR_ARCHIVE, "Sniper: left hand position Y");
ConVar tfvr_hands_sniper_left_z("tfvr_hands_sniper_left_z", "0", FCVAR_ARCHIVE, "Sniper: left hand position Z");
ConVar tfvr_hands_spy_left_x("tfvr_hands_spy_left_x", "0", FCVAR_ARCHIVE, "Spy: left hand position X");
ConVar tfvr_hands_spy_left_y("tfvr_hands_spy_left_y", "0", FCVAR_ARCHIVE, "Spy: left hand position Y");
ConVar tfvr_hands_spy_left_z("tfvr_hands_spy_left_z", "0", FCVAR_ARCHIVE, "Spy: left hand position Z");

// Per-class positional offset convars - RIGHT hand
ConVar tfvr_hands_scout_right_x("tfvr_hands_scout_right_x", "0", FCVAR_ARCHIVE, "Scout: right hand position X");
ConVar tfvr_hands_scout_right_y("tfvr_hands_scout_right_y", "0", FCVAR_ARCHIVE, "Scout: right hand position Y");
ConVar tfvr_hands_scout_right_z("tfvr_hands_scout_right_z", "0", FCVAR_ARCHIVE, "Scout: right hand position Z");
ConVar tfvr_hands_soldier_right_x("tfvr_hands_soldier_right_x", "0", FCVAR_ARCHIVE, "Soldier: right hand position X");
ConVar tfvr_hands_soldier_right_y("tfvr_hands_soldier_right_y", "0", FCVAR_ARCHIVE, "Soldier: right hand position Y");
ConVar tfvr_hands_soldier_right_z("tfvr_hands_soldier_right_z", "0", FCVAR_ARCHIVE, "Soldier: right hand position Z");
ConVar tfvr_hands_pyro_right_x("tfvr_hands_pyro_right_x", "0", FCVAR_ARCHIVE, "Pyro: right hand position X");
ConVar tfvr_hands_pyro_right_y("tfvr_hands_pyro_right_y", "0", FCVAR_ARCHIVE, "Pyro: right hand position Y");
ConVar tfvr_hands_pyro_right_z("tfvr_hands_pyro_right_z", "0", FCVAR_ARCHIVE, "Pyro: right hand position Z");
ConVar tfvr_hands_demo_right_x("tfvr_hands_demo_right_x", "0", FCVAR_ARCHIVE, "Demo: right hand position X");
ConVar tfvr_hands_demo_right_y("tfvr_hands_demo_right_y", "0", FCVAR_ARCHIVE, "Demo: right hand position Y");
ConVar tfvr_hands_demo_right_z("tfvr_hands_demo_right_z", "0", FCVAR_ARCHIVE, "Demo: right hand position Z");
ConVar tfvr_hands_heavy_right_x("tfvr_hands_heavy_right_x", "0", FCVAR_ARCHIVE, "Heavy: right hand position X");
ConVar tfvr_hands_heavy_right_y("tfvr_hands_heavy_right_y", "0", FCVAR_ARCHIVE, "Heavy: right hand position Y");
ConVar tfvr_hands_heavy_right_z("tfvr_hands_heavy_right_z", "0", FCVAR_ARCHIVE, "Heavy: right hand position Z");
ConVar tfvr_hands_engi_right_x("tfvr_hands_engi_right_x", "0", FCVAR_ARCHIVE, "Engineer: right hand position X");
ConVar tfvr_hands_engi_right_y("tfvr_hands_engi_right_y", "0", FCVAR_ARCHIVE, "Engineer: right hand position Y");
ConVar tfvr_hands_engi_right_z("tfvr_hands_engi_right_z", "0", FCVAR_ARCHIVE, "Engineer: right hand position Z");
ConVar tfvr_hands_medic_right_x("tfvr_hands_medic_right_x", "0", FCVAR_ARCHIVE, "Medic: right hand position X");
ConVar tfvr_hands_medic_right_y("tfvr_hands_medic_right_y", "0", FCVAR_ARCHIVE, "Medic: right hand position Y");
ConVar tfvr_hands_medic_right_z("tfvr_hands_medic_right_z", "0", FCVAR_ARCHIVE, "Medic: right hand position Z");
ConVar tfvr_hands_sniper_right_x("tfvr_hands_sniper_right_x", "0", FCVAR_ARCHIVE, "Sniper: right hand position X");
ConVar tfvr_hands_sniper_right_y("tfvr_hands_sniper_right_y", "0", FCVAR_ARCHIVE, "Sniper: right hand position Y");
ConVar tfvr_hands_sniper_right_z("tfvr_hands_sniper_right_z", "0", FCVAR_ARCHIVE, "Sniper: right hand position Z");
ConVar tfvr_hands_spy_right_x("tfvr_hands_spy_right_x", "0", FCVAR_ARCHIVE, "Spy: right hand position X");
ConVar tfvr_hands_spy_right_y("tfvr_hands_spy_right_y", "0", FCVAR_ARCHIVE, "Spy: right hand position Y");
ConVar tfvr_hands_spy_right_z("tfvr_hands_spy_right_z", "0", FCVAR_ARCHIVE, "Spy: right hand position Z");

//-----------------------------------------------------------------------------
// Purpose: Get hand offset angles for a specific class and hand side
//          Returns true if per-class offsets are enabled and valid
//-----------------------------------------------------------------------------
static bool GetPerClassHandOffset(int playerClass, bool bLeftHand, QAngle &outOffset)
{
	if (!tfvr_hands_perclass_offsets.GetBool())
		return false;
	
	ConVar *pPitch = NULL;
	ConVar *pYaw = NULL;
	ConVar *pRoll = NULL;
	
	if (bLeftHand)
	{
		switch (playerClass)
		{
			case TF_CLASS_SCOUT:
				pPitch = &tfvr_hands_scout_left_pitch;
				pYaw = &tfvr_hands_scout_left_yaw;
				pRoll = &tfvr_hands_scout_left_roll;
				break;
			case TF_CLASS_SOLDIER:
				pPitch = &tfvr_hands_soldier_left_pitch;
				pYaw = &tfvr_hands_soldier_left_yaw;
				pRoll = &tfvr_hands_soldier_left_roll;
				break;
			case TF_CLASS_PYRO:
				pPitch = &tfvr_hands_pyro_left_pitch;
				pYaw = &tfvr_hands_pyro_left_yaw;
				pRoll = &tfvr_hands_pyro_left_roll;
				break;
			case TF_CLASS_DEMOMAN:
				pPitch = &tfvr_hands_demo_left_pitch;
				pYaw = &tfvr_hands_demo_left_yaw;
				pRoll = &tfvr_hands_demo_left_roll;
				break;
			case TF_CLASS_HEAVYWEAPONS:
				pPitch = &tfvr_hands_heavy_left_pitch;
				pYaw = &tfvr_hands_heavy_left_yaw;
				pRoll = &tfvr_hands_heavy_left_roll;
				break;
			case TF_CLASS_ENGINEER:
				pPitch = &tfvr_hands_engi_left_pitch;
				pYaw = &tfvr_hands_engi_left_yaw;
				pRoll = &tfvr_hands_engi_left_roll;
				break;
			case TF_CLASS_MEDIC:
				pPitch = &tfvr_hands_medic_left_pitch;
				pYaw = &tfvr_hands_medic_left_yaw;
				pRoll = &tfvr_hands_medic_left_roll;
				break;
			case TF_CLASS_SNIPER:
				pPitch = &tfvr_hands_sniper_left_pitch;
				pYaw = &tfvr_hands_sniper_left_yaw;
				pRoll = &tfvr_hands_sniper_left_roll;
				break;
			case TF_CLASS_SPY:
				pPitch = &tfvr_hands_spy_left_pitch;
				pYaw = &tfvr_hands_spy_left_yaw;
				pRoll = &tfvr_hands_spy_left_roll;
				break;
			default:
				return false;
		}
	}
	else
	{
		switch (playerClass)
		{
			case TF_CLASS_SCOUT:
				pPitch = &tfvr_hands_scout_right_pitch;
				pYaw = &tfvr_hands_scout_right_yaw;
				pRoll = &tfvr_hands_scout_right_roll;
				break;
			case TF_CLASS_SOLDIER:
				pPitch = &tfvr_hands_soldier_right_pitch;
				pYaw = &tfvr_hands_soldier_right_yaw;
				pRoll = &tfvr_hands_soldier_right_roll;
				break;
			case TF_CLASS_PYRO:
				pPitch = &tfvr_hands_pyro_right_pitch;
				pYaw = &tfvr_hands_pyro_right_yaw;
				pRoll = &tfvr_hands_pyro_right_roll;
				break;
			case TF_CLASS_DEMOMAN:
				pPitch = &tfvr_hands_demo_right_pitch;
				pYaw = &tfvr_hands_demo_right_yaw;
				pRoll = &tfvr_hands_demo_right_roll;
				break;
			case TF_CLASS_HEAVYWEAPONS:
				pPitch = &tfvr_hands_heavy_right_pitch;
				pYaw = &tfvr_hands_heavy_right_yaw;
				pRoll = &tfvr_hands_heavy_right_roll;
				break;
			case TF_CLASS_ENGINEER:
				pPitch = &tfvr_hands_engi_right_pitch;
				pYaw = &tfvr_hands_engi_right_yaw;
				pRoll = &tfvr_hands_engi_right_roll;
				break;
			case TF_CLASS_MEDIC:
				pPitch = &tfvr_hands_medic_right_pitch;
				pYaw = &tfvr_hands_medic_right_yaw;
				pRoll = &tfvr_hands_medic_right_roll;
				break;
			case TF_CLASS_SNIPER:
				pPitch = &tfvr_hands_sniper_right_pitch;
				pYaw = &tfvr_hands_sniper_right_yaw;
				pRoll = &tfvr_hands_sniper_right_roll;
				break;
			case TF_CLASS_SPY:
				pPitch = &tfvr_hands_spy_right_pitch;
				pYaw = &tfvr_hands_spy_right_yaw;
				pRoll = &tfvr_hands_spy_right_roll;
				break;
			default:
				return false;
		}
	}
	
	if (pPitch && pYaw && pRoll)
	{
		outOffset.x = pPitch->GetFloat();
		outOffset.y = pYaw->GetFloat();
		outOffset.z = pRoll->GetFloat();
		return true;
	}
	
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Get hand position offset for a specific class and hand side
//          Returns true if per-class offsets are enabled and valid
//          Position is in palm-relative space: X=toward fingers, Y=out of palm, Z=toward thumb
//-----------------------------------------------------------------------------
static bool GetPerClassHandPositionOffset(int playerClass, bool bLeftHand, Vector &outOffset)
{
	if (!tfvr_hands_perclass_offsets.GetBool())
		return false;
	
	ConVar *pX = NULL;
	ConVar *pY = NULL;
	ConVar *pZ = NULL;
	
	if (bLeftHand)
	{
		switch (playerClass)
		{
			case TF_CLASS_SCOUT:
				pX = &tfvr_hands_scout_left_x;
				pY = &tfvr_hands_scout_left_y;
				pZ = &tfvr_hands_scout_left_z;
				break;
			case TF_CLASS_SOLDIER:
				pX = &tfvr_hands_soldier_left_x;
				pY = &tfvr_hands_soldier_left_y;
				pZ = &tfvr_hands_soldier_left_z;
				break;
			case TF_CLASS_PYRO:
				pX = &tfvr_hands_pyro_left_x;
				pY = &tfvr_hands_pyro_left_y;
				pZ = &tfvr_hands_pyro_left_z;
				break;
			case TF_CLASS_DEMOMAN:
				pX = &tfvr_hands_demo_left_x;
				pY = &tfvr_hands_demo_left_y;
				pZ = &tfvr_hands_demo_left_z;
				break;
			case TF_CLASS_HEAVYWEAPONS:
				pX = &tfvr_hands_heavy_left_x;
				pY = &tfvr_hands_heavy_left_y;
				pZ = &tfvr_hands_heavy_left_z;
				break;
			case TF_CLASS_ENGINEER:
				pX = &tfvr_hands_engi_left_x;
				pY = &tfvr_hands_engi_left_y;
				pZ = &tfvr_hands_engi_left_z;
				break;
			case TF_CLASS_MEDIC:
				pX = &tfvr_hands_medic_left_x;
				pY = &tfvr_hands_medic_left_y;
				pZ = &tfvr_hands_medic_left_z;
				break;
			case TF_CLASS_SNIPER:
				pX = &tfvr_hands_sniper_left_x;
				pY = &tfvr_hands_sniper_left_y;
				pZ = &tfvr_hands_sniper_left_z;
				break;
			case TF_CLASS_SPY:
				pX = &tfvr_hands_spy_left_x;
				pY = &tfvr_hands_spy_left_y;
				pZ = &tfvr_hands_spy_left_z;
				break;
			default:
				return false;
		}
	}
	else
	{
		switch (playerClass)
		{
			case TF_CLASS_SCOUT:
				pX = &tfvr_hands_scout_right_x;
				pY = &tfvr_hands_scout_right_y;
				pZ = &tfvr_hands_scout_right_z;
				break;
			case TF_CLASS_SOLDIER:
				pX = &tfvr_hands_soldier_right_x;
				pY = &tfvr_hands_soldier_right_y;
				pZ = &tfvr_hands_soldier_right_z;
				break;
			case TF_CLASS_PYRO:
				pX = &tfvr_hands_pyro_right_x;
				pY = &tfvr_hands_pyro_right_y;
				pZ = &tfvr_hands_pyro_right_z;
				break;
			case TF_CLASS_DEMOMAN:
				pX = &tfvr_hands_demo_right_x;
				pY = &tfvr_hands_demo_right_y;
				pZ = &tfvr_hands_demo_right_z;
				break;
			case TF_CLASS_HEAVYWEAPONS:
				pX = &tfvr_hands_heavy_right_x;
				pY = &tfvr_hands_heavy_right_y;
				pZ = &tfvr_hands_heavy_right_z;
				break;
			case TF_CLASS_ENGINEER:
				pX = &tfvr_hands_engi_right_x;
				pY = &tfvr_hands_engi_right_y;
				pZ = &tfvr_hands_engi_right_z;
				break;
			case TF_CLASS_MEDIC:
				pX = &tfvr_hands_medic_right_x;
				pY = &tfvr_hands_medic_right_y;
				pZ = &tfvr_hands_medic_right_z;
				break;
			case TF_CLASS_SNIPER:
				pX = &tfvr_hands_sniper_right_x;
				pY = &tfvr_hands_sniper_right_y;
				pZ = &tfvr_hands_sniper_right_z;
				break;
			case TF_CLASS_SPY:
				pX = &tfvr_hands_spy_right_x;
				pY = &tfvr_hands_spy_right_y;
				pZ = &tfvr_hands_spy_right_z;
				break;
			default:
				return false;
		}
	}
	
	if (pX && pY && pZ)
	{
		outOffset.x = pX->GetFloat();
		outOffset.y = pY->GetFloat();
		outOffset.z = pZ->GetFloat();
		return true;
	}
	
	return false;
}

// Debug convars
ConVar tfvr_debug_weapon_attachment("tfvr_debug_weapon_attachment", "0", FCVAR_NONE, "Draw debug lines showing weapon attachment");
ConVar tfvr_debug_weapon_position("tfvr_debug_weapon_position", "0", FCVAR_NONE, "Print weapon position updates to console");

// Weapon grip offset convars (for standard TF2 weapons without VR data)
ConVar tfvr_weapon_grip_offset_x("tfvr_weapon_grip_offset_x", "0", FCVAR_ARCHIVE, "Default weapon grip offset X (forward)");
ConVar tfvr_weapon_grip_offset_y("tfvr_weapon_grip_offset_y", "0", FCVAR_ARCHIVE, "Default weapon grip offset Y (right)");
ConVar tfvr_weapon_grip_offset_z("tfvr_weapon_grip_offset_z", "0", FCVAR_ARCHIVE, "Default weapon grip offset Z (up)");
ConVar tfvr_weapon_grip_angle_pitch("tfvr_weapon_grip_angle_pitch", "-90", FCVAR_ARCHIVE, "Default weapon grip angle pitch");
ConVar tfvr_weapon_grip_angle_yaw("tfvr_weapon_grip_angle_yaw", "0", FCVAR_ARCHIVE, "Default weapon grip angle yaw");
ConVar tfvr_weapon_grip_angle_roll("tfvr_weapon_grip_angle_roll", "-90", FCVAR_ARCHIVE, "Default weapon grip angle roll");

// Weapon fire animation convars
ConVar tfvr_weapon_fire_anim("tfvr_weapon_fire_anim", "1", FCVAR_ARCHIVE, "Enable fire animations on VR-held weapons");
ConVar tfvr_weapon_fire_anim_debug("tfvr_weapon_fire_anim_debug", "0", FCVAR_NONE, "Debug fire animation triggering");
ConVar tfvr_weapon_fire_anim_scale("tfvr_weapon_fire_anim_scale", "1.0", FCVAR_ARCHIVE, "Scale factor for fire animation recoil (0=off, 1=normal, 2=double)");
ConVar tfvr_weapon_fire_anim_pos_scale("tfvr_weapon_fire_anim_pos_scale", "1.0", FCVAR_ARCHIVE, "Extra scale for position offset (on top of main scale)");
ConVar tfvr_weapon_fire_anim_pitch_scale("tfvr_weapon_fire_anim_pitch_scale", "1.0", FCVAR_ARCHIVE, "Scale/invert pitch rotation (negative to flip)");
ConVar tfvr_weapon_fire_anim_yaw_scale("tfvr_weapon_fire_anim_yaw_scale", "1.0", FCVAR_ARCHIVE, "Scale/invert yaw rotation (negative to flip)");
ConVar tfvr_weapon_fire_anim_roll_scale("tfvr_weapon_fire_anim_roll_scale", "1.0", FCVAR_ARCHIVE, "Scale/invert roll rotation (negative to flip)");
ConVar tfvr_weapon_fire_anim_pos_rotation("tfvr_weapon_fire_anim_pos_rotation", "90", FCVAR_ARCHIVE, "Rotation correction for position vector (degrees around Z axis)");
ConVar tfvr_weapon_fire_anim_angle_rotation("tfvr_weapon_fire_anim_angle_rotation", "180", FCVAR_ARCHIVE, "Coordinate space rotation for fire animation (degrees around Z axis)");

// Weapon-specific aim angle corrections (for weapons with incorrect muzzle attachment orientation)
// These are applied as LOCAL rotations relative to the muzzle attachment frame
ConVar tfvr_aim_grenadelauncher_pitch("tfvr_aim_grenadelauncher_pitch", "0", FCVAR_ARCHIVE, "Pitch correction for grenade launcher aim (degrees, local space)");
ConVar tfvr_aim_grenadelauncher_yaw("tfvr_aim_grenadelauncher_yaw", "90", FCVAR_ARCHIVE, "Yaw correction for grenade launcher aim (degrees, local space)");
ConVar tfvr_aim_grenadelauncher_roll("tfvr_aim_grenadelauncher_roll", "0", FCVAR_ARCHIVE, "Roll correction for grenade launcher aim (degrees, local space)");
ConVar tfvr_aim_stickybomb_pitch("tfvr_aim_stickybomb_pitch", "0", FCVAR_ARCHIVE, "Pitch correction for sticky launcher aim (degrees, local space)");
ConVar tfvr_aim_stickybomb_yaw("tfvr_aim_stickybomb_yaw", "90", FCVAR_ARCHIVE, "Yaw correction for sticky launcher aim (degrees, local space)");
ConVar tfvr_aim_stickybomb_roll("tfvr_aim_stickybomb_roll", "0", FCVAR_ARCHIVE, "Roll correction for sticky launcher aim (degrees, local space)");

// Aim stabilization - counteracts grip/trigger-induced palm wobble on weapon aim
ConVar tfvr_aim_stabilize("tfvr_aim_stabilize", "1", FCVAR_ARCHIVE, "Stabilize weapon aim against grip-induced palm movement (0=off, 1=on)");
ConVar tfvr_aim_stabilize_adapt("tfvr_aim_stabilize_adapt", "0.1", FCVAR_ARCHIVE, "Rate at which aim reference adapts to current grip (per second, 0=never)");
ConVar tfvr_aim_stabilize_debug("tfvr_aim_stabilize_debug", "0", FCVAR_NONE, "Show aim stabilization debug info");

// Global storage for active VR hands - since we only support local player, use two pointers
static C_TFVRHand *g_pLocalPlayerLeftHand = NULL;
static C_TFVRHand *g_pLocalPlayerRightHand = NULL;

//-----------------------------------------------------------------------------
// Purpose: Eased approach - moves current toward target with ease-out curve
//          Fast at start of motion, slow at end (no abrupt stops)
//-----------------------------------------------------------------------------
static float EasedApproach(float target, float current, float speed, float frametime, float easePower = 2.5f)
{
	float delta = target - current;
	if (fabsf(delta) < 0.0001f)
		return target;
	
	// Convert speed to per-frame blend factor
	float blendRate = clamp(speed * frametime, 0.0f, 1.0f);
	
	// Apply ease-out curve: fast start, slow end
	float easedBlend = 1.0f - powf(1.0f - blendRate, easePower);
	
	return current + delta * easedBlend;
}

//-----------------------------------------------------------------------------
// Purpose: Apply ease-out curve to a 0-1 blend value (symmetric for both directions)
//-----------------------------------------------------------------------------
static float ApplyEaseOutToBlend(float t, float easePower, bool bBlendingUp)
{
	t = clamp(t, 0.0f, 1.0f);
	
	if (bBlendingUp)
		return 1.0f - powf(1.0f - t, easePower);  // Ease-out going up
	else
		return powf(t, easePower);  // Ease-out going down
}

//-----------------------------------------------------------------------------
// Purpose: Slerp that always takes shortest path (avoids quaternion hemisphere issues)
//-----------------------------------------------------------------------------
static void SafeQuaternionSlerp(const Quaternion &from, const Quaternion &to, float t, Quaternion &result)
{
	t = clamp(t, 0.0f, 1.0f);
	
	Quaternion alignedTo;
	QuaternionAlign(from, to, alignedTo);
	QuaternionSlerp(from, alignedTo, t, result);
	QuaternionNormalize(result);
}

//-----------------------------------------------------------------------------
// Purpose: Check if a weapon is a medigun (any variant)
//-----------------------------------------------------------------------------
static bool IsWeaponMedigun(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
		return false;
	return pWeapon->GetWeaponID() == TF_WEAPON_MEDIGUN;
}

//-----------------------------------------------------------------------------
// Purpose: Check if a weapon is a flamethrower (any variant, not Dragon's Fury)
//-----------------------------------------------------------------------------
static bool IsWeaponFlamethrower(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
		return false;
	return pWeapon->GetWeaponID() == TF_WEAPON_FLAMETHROWER;
}

//-----------------------------------------------------------------------------
// Purpose: Apply two-hand grip rotation to a transform matrix
//          Uses minimal rotation (quaternion swing) to point Y axis toward desiredY
//          This preserves the controller's roll perfectly - only pitch/yaw changes
//-----------------------------------------------------------------------------
static void ApplyTwoHandGripRotation(matrix3x4_t &transform, const Vector &desiredY)
{
	// Preserve position
	Vector pos(transform[0][3], transform[1][3], transform[2][3]);
	
	// Get the current Y axis (where weapon currently points)
	Vector currentY;
	MatrixGetColumn(transform, 1, currentY);
	
	// Calculate the rotation needed to go from currentY to desiredY
	// This is the "minimal rotation" - only changes direction, not roll
	float dotProduct = DotProduct(currentY, desiredY);
	
	// Clamp dot product to avoid numerical issues with acos
	dotProduct = clamp(dotProduct, -1.0f, 1.0f);
	
	// If already pointing the right way (or very close), no rotation needed
	if (dotProduct > 0.9999f)
	{
		// Just update position in case it changed
		transform[0][3] = pos.x; transform[1][3] = pos.y; transform[2][3] = pos.z;
		return;
	}
	
	// If pointing exactly opposite, pick an arbitrary perpendicular axis
	Vector rotationAxis;
	if (dotProduct < -0.9999f)
	{
		// Find any perpendicular vector
		Vector controllerUp;
		MatrixGetColumn(transform, 2, controllerUp);
		rotationAxis = controllerUp;
	}
	else
	{
		// Normal case: rotation axis is perpendicular to both vectors
		rotationAxis = CrossProduct(currentY, desiredY);
		rotationAxis.NormalizeInPlace();
	}
	
	// Calculate rotation angle
	float angle = acosf(dotProduct);
	
	// Build rotation quaternion from axis-angle
	float halfAngle = angle * 0.5f;
	float sinHalf = sinf(halfAngle);
	float cosHalf = cosf(halfAngle);
	
	Quaternion rotQuat;
	rotQuat.x = rotationAxis.x * sinHalf;
	rotQuat.y = rotationAxis.y * sinHalf;
	rotQuat.z = rotationAxis.z * sinHalf;
	rotQuat.w = cosHalf;
	
	// Get original rotation as quaternion
	Quaternion origQuat;
	MatrixQuaternion(transform, origQuat);
	
	// Apply the swing rotation: newQuat = rotQuat * origQuat
	Quaternion newQuat;
	QuaternionMult(rotQuat, origQuat, newQuat);
	
	// Convert back to matrix
	QuaternionMatrix(newQuat, pos, transform);
}

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
	m_iFireSequence = -1;
	m_iAltFireSequence = -1;
	m_iIdleSequence = -1;
	m_bAnimateIdle = false;
	m_bPlayingFireAnim = false;
	m_flFireAnimStartTime = 0.0f;
	m_iChargeSequence = -1;
	m_iChargeSequence2 = -1;
	m_bPlayingChargeAnim = false;
	m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
	m_iFireOnSequence = -1;
	m_iFireOffSequence = -1;
	m_bMedigunWasHealing = false;
	m_bFlamethrowerWasFiring = false;
	m_flFlamethrowerFireBlend = 0.0f;
	m_vecLastValidPosition = vec3_origin;
	m_angLastValidAngles = vec3_angle;
	m_szModelName[0] = '\0';
	SetIdentityMatrix(m_matIdleHandBoneTransform);
	m_vecIdleHandBoneLocalPos.Init();
	m_bHandBoneOffsetValid = false;
	m_iHandBone = -1;
	m_flTwoHandBlend = 0.0f;
	m_iOffHandBone = -1;
	m_iOffHandMiddleFingerBone = -1;
	m_bOffhandGripActive = false;
	m_bWasOffhandGripActive = false;
	m_flGripRotationBlend = 0.0f;
	m_vecOffhandGripForward = Vector(1, 0, 0);
	m_vecOffhandGripUp = Vector(0, 0, 1);
	m_vecCachedGripDelta = vec3_origin;
	m_vecCachedGripYAxis = Vector(0, 1, 0);
	
	// Aim stabilization
	SetIdentityMatrix(m_matAimRefPalmOffset);
	m_bAimRefValid = false;
	
	// Idle muzzle caching for pistols
	m_vIdleMuzzleOffset = vec3_origin;
	m_angIdleMuzzleAngles = vec3_angle;
	m_bIdleMuzzleOffsetValid = false;
	m_iCachedMuzzleWeaponID = -1;
	
	// Crit boost effect
	m_pCritBoostEffect = NULL;
	m_bCritBoostActive = false;
	
	// Cached weapon bone world transform
	SetIdentityMatrix(m_matWeaponBoneWorld);
	m_bWeaponBoneWorldValid = false;
	
	// Cached muzzle position
	m_vecCachedMuzzlePos = vec3_origin;
	m_angCachedMuzzleAngles = vec3_angle;
	m_bCachedMuzzleValid = false;
	
	// Melee swing cycling
	m_iMeleeSwingIndex = 0;
	m_szMeleeSwingBase[0] = '\0';
	m_iMeleeSwingCount = 0;
	
	// Left hand wearables
	m_hLeftHandWatch = NULL;
	m_bOwnWatchModel = false;
	m_hLeftHandBall = NULL;
	m_iLastBallAmmo = -1;

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
// Purpose: IHasOwner interface - allows material proxies to find owner for cloak effects
//-----------------------------------------------------------------------------
CBaseEntity *C_TFVRHand::GetOwnerViaInterface(void)
{
	return m_hOwnerPlayer.Get();
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
	
	// Reset ALL state for clean reinitialize (match constructor)
	m_hHeldWeapon = NULL;
	m_hRenderWeapon = NULL;
	m_bHandTrackingValid = false;
	m_bControllerTracked = false;
	m_iFireSequence = -1;
	m_iAltFireSequence = -1;
	m_iIdleSequence = -1;
	m_bAnimateIdle = false;
	m_bPlayingFireAnim = false;
	m_flFireAnimStartTime = 0.0f;
	m_iChargeSequence = -1;
	m_iChargeSequence2 = -1;
	m_bPlayingChargeAnim = false;
	m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
	m_iFireOnSequence = -1;
	m_iFireOffSequence = -1;
	m_bMedigunWasHealing = false;
	m_bFlamethrowerWasFiring = false;
	m_flFlamethrowerFireBlend = 0.0f;
	m_iOffHandBone = -1;
	m_iOffHandMiddleFingerBone = -1;
	m_flTwoHandBlend = 0.0f;
	m_bOffhandGripActive = false;
	m_bWasOffhandGripActive = false;
	m_flGripRotationBlend = 0.0f;
	m_bBoneMappingSetup = false;
	m_bHandBoneOffsetValid = false;
	m_iHandBone = -1;
	m_bIdleMuzzleOffsetValid = false;
	m_iCachedMuzzleWeaponID = -1;
	SetIdentityMatrix(m_matIdleHandBoneTransform);
	m_vecIdleHandBoneLocalPos.Init();
	m_vecOffhandGripForward = Vector(1, 0, 0);
	m_vecOffhandGripUp = Vector(0, 0, 1);
	m_vecCachedGripDelta = vec3_origin;
	m_vecCachedGripYAxis = Vector(0, 1, 0);
	m_vIdleMuzzleOffset = vec3_origin;
	m_angIdleMuzzleAngles = vec3_angle;
	m_bCritBoostActive = false;
	SetIdentityMatrix(m_matWeaponBoneWorld);
	m_bWeaponBoneWorldValid = false;
	m_vecCachedMuzzlePos = vec3_origin;
	m_angCachedMuzzleAngles = vec3_angle;
	m_bCachedMuzzleValid = false;
	m_iMeleeSwingIndex = 0;
	m_szMeleeSwingBase[0] = '\0';
	m_iMeleeSwingCount = 0;
	
	// Reset left hand wearables
	m_hLeftHandWatch = NULL;
	m_bOwnWatchModel = false;
	m_hLeftHandBall = NULL;
	m_iLastBallAmmo = -1;
	
	m_hOwnerPlayer = pOwner;
	m_handSide = handSide;
	
	// Set owner entity so material proxies (like spy_invis for cloak) can find the owner player
	SetOwnerEntity(pOwner);
	
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

	// Get class-specific hand model path (may return NULL to force fallback)
	const char *handModelPath = GetHandModelForClass(m_iLastPlayerClass, IsLeftHand());
	
	// Set a valid origin first (entities need to be in the world)
	SetAbsOrigin(pOwner->EyePosition());
	SetAbsAngles(vec3_angle);
	
	bool bCustomModelWorked = false;
	
	// If we have a custom hand model path, try to load it
	if (handModelPath != NULL)
	{
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
		
		// Try using SetModel directly
		bCustomModelWorked = (modelIndex != -1) && SetModel(m_szModelName);
		
		if (bCustomModelWorked)
		{
			Msg("VR Hand (%s): Successfully loaded separate hand model: %s\n", IsLeftHand() ? "LEFT" : "RIGHT", handModelPath);
		}
		else
		{
			Warning("VR Hand: Failed to load %s (model index: %d), trying fallback\n", handModelPath, modelIndex);
		}
	}
	
	// Use fallback to combined arms model if custom model failed or wasn't specified
	if (!bCustomModelWorked)
	{
		const char *fallbackModel = GetFallbackModelForClass(m_iLastPlayerClass);
		Q_strncpy(m_szModelName, fallbackModel, sizeof(m_szModelName));
		if (!SetModel(m_szModelName))
		{
			Warning("C_TFVRHand::Initialize - Failed to load hand model!\n");
			return false;
		}
		
		Msg("VR Hand (%s): Using fallback combined arms model: %s\n", IsLeftHand() ? "LEFT" : "RIGHT", fallbackModel);
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
	
	// Set initial skin based on team
	int iTeamNumber = pOwner->GetTeamNumber();
	m_nSkin = (iTeamNumber == TF_TEAM_BLUE) ? 1 : 0;
	Msg("VR Initialize: %s hand complete - Class=%d, Team=%d, Skin=%d, m_bShuttingDown=%s\n", 
		IsLeftHand() ? "LEFT" : "RIGHT", m_iLastPlayerClass, iTeamNumber, m_nSkin,
		m_bShuttingDown ? "true" : "false");

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Clean up
//-----------------------------------------------------------------------------
void C_TFVRHand::Shutdown()
{
	Msg("VR Shutdown: %s hand shutting down (owner=%p, already shutting down=%s)\n", 
		IsLeftHand() ? "LEFT" : "RIGHT",
		m_hOwnerPlayer.Get(),
		m_bShuttingDown ? "YES" : "NO");
	
	// Stack trace for debugging
	// Print who called us
	if (m_bShuttingDown)
	{
		Msg("VR Shutdown: WARNING - already shut down! Double shutdown detected.\n");
	}
	
	m_bShuttingDown = true;
	
	// Unequip any held weapon
	UnequipWeapon();
	
	// Clean up left hand wearables if this is the left hand
	if (IsLeftHand())
	{
		RemoveLeftHandWatch();
		RemoveLeftHandBall();
	}
	
	// Reset bone mapping so it gets recalculated on reinit
	m_bBoneMappingSetup = false;
	m_bHandBoneOffsetValid = false;
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

	Msg("VR SpawnVRHands: Called for class %d\n", pPlayer->GetPlayerClass()->GetClassIndex());

	// If hands already exist, reinitialize them
	if (g_pLocalPlayerLeftHand && g_pLocalPlayerRightHand)
	{
		Msg("VR SpawnVRHands: Reinitializing existing hands\n");
		
		// Reinitialize with new player pointer
		g_pLocalPlayerLeftHand->Shutdown();
		g_pLocalPlayerRightHand->Shutdown();
		
		if (g_pLocalPlayerLeftHand->Initialize(pPlayer, VR_HAND_LEFT) &&
			g_pLocalPlayerRightHand->Initialize(pPlayer, VR_HAND_RIGHT))
		{
			g_pLocalPlayerLeftHand->Spawn();
			g_pLocalPlayerRightHand->Spawn();
			Msg("VR SpawnVRHands: Reinit successful\n");
			return;
		}
		Msg("VR SpawnVRHands: Reinit FAILED\n");
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

	// IMPORTANT: Only shutdown hands if they're actually owned by this player
	// This prevents an old player entity's UpdateOnRemove from shutting down
	// hands that have already been transferred to a new player entity
	if (g_pLocalPlayerLeftHand)
	{
		C_TFPlayer* pCurrentOwner = g_pLocalPlayerLeftHand->GetOwnerPlayer();
		if (pCurrentOwner == pPlayer)
		{
			Msg("VR RemoveVRHands: Shutting down hands for player %p\n", pPlayer);
			g_pLocalPlayerLeftHand->Shutdown();
			g_pLocalPlayerLeftHand->AddEffects(EF_NODRAW);
			
			if (g_pLocalPlayerRightHand)
			{
				g_pLocalPlayerRightHand->Shutdown();
				g_pLocalPlayerRightHand->AddEffects(EF_NODRAW);
			}
		}
		else
		{
			Msg("VR RemoveVRHands: Skipping - hands owned by different player (owner=%p, caller=%p)\n", 
				pCurrentOwner, pPlayer);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called every frame (if the entity is in the think list)
//-----------------------------------------------------------------------------
void C_TFVRHand::ClientThink()
{
	BaseClass::ClientThink();
	
	extern ConVar tfvr_weapon_fire_anim_debug;
	static float s_flLastDebugTime = 0.0f;
	
	// Debug output every 0.5 seconds to confirm ClientThink is being called
	if (tfvr_weapon_fire_anim_debug.GetBool() && (gpGlobals->curtime - s_flLastDebugTime) > 0.5f)
	{
		DevMsg("VR: ClientThink called - sequence: %d, cycle: %.2f, playingFireAnim: %d\n", 
			GetSequence(), GetCycle(), m_bPlayingFireAnim);
		s_flLastDebugTime = gpGlobals->curtime;
	}
	
	// Advance animation frame
	StudioFrameAdvance();
	
	// Check if fire animation has completed and return to idle
	// Skip for medigun (handled by UpdateMedigunFireAnimation) and
	// flamethrower primary fire (handled by UpdateFlamethrowerFireAnimation).
	// Flamethrower alt-fire (airblast) IS allowed through as a one-shot.
	bool bFlamethrowerPrimaryActive = (m_flFlamethrowerFireBlend > 0.0f);
	if (m_bPlayingFireAnim && m_eMedigunFireState == MEDIGUN_FIRE_IDLE
		&& !bFlamethrowerPrimaryActive)
	{
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Fire anim playing - cycle: %.2f, time: %.2f, elapsed: %.2f\n", 
				GetCycle(), gpGlobals->curtime, gpGlobals->curtime - m_flFireAnimStartTime);
		}
		
		// Check if animation cycle has completed (or timed out after 1 second)
		if (GetCycle() >= 1.0f || (gpGlobals->curtime - m_flFireAnimStartTime) > 1.0f)
		{
			// Return to idle animation
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(0.0f);
			}
			m_bPlayingFireAnim = false;
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Fire animation completed, returning to idle (seq %d)\n", m_iIdleSequence);
			}
		}
	}
	
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
	{
		static float lastShutdownMsg = 0;
		if (gpGlobals->curtime - lastShutdownMsg > 1.0f)
		{
			Msg("VR Update: %s hand is shutdown, skipping\n", IsLeftHand() ? "LEFT" : "RIGHT");
			lastShutdownMsg = gpGlobals->curtime;
		}
		return;
	}

	// Validate we still have an owner
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner || !pOwner->IsLocalPlayer())
	{
		// Owner is gone, hide ourselves
		AddEffects(EF_NODRAW);
		return;
	}
	
	// Advance animation frame
	StudioFrameAdvance();
	
	// Check if fire animation has completed and return to idle
	// Skip for medigun (handled by UpdateMedigunFireAnimation) and
	// flamethrower primary fire (handled by UpdateFlamethrowerFireAnimation).
	// Flamethrower alt-fire (airblast) IS allowed through as a one-shot.
	bool bFlamethrowerPrimaryActive = (m_flFlamethrowerFireBlend > 0.0f);
	if (m_bPlayingFireAnim && m_eMedigunFireState == MEDIGUN_FIRE_IDLE
		&& !bFlamethrowerPrimaryActive)
	{
		extern ConVar tfvr_weapon_fire_anim_debug;
		
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Fire anim playing - cycle: %.2f, time: %.2f, elapsed: %.2f\n", 
				GetCycle(), gpGlobals->curtime, gpGlobals->curtime - m_flFireAnimStartTime);
		}
		
		// Check if animation cycle has completed (or timed out after 1 second)
		if (GetCycle() >= 1.0f || (gpGlobals->curtime - m_flFireAnimStartTime) > 1.0f)
		{
			// Return to idle animation
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(0.0f);
			}
			m_bPlayingFireAnim = false;
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Fire animation completed, returning to idle (seq %d)\n", m_iIdleSequence);
			}
		}
	}
	
	// Update medigun fire animation state machine
	UpdateMedigunFireAnimation();
	
	// Update flamethrower fire animation (driven by weapon fire state)
	UpdateFlamethrowerFireAnimation();
	
	// Check if player class has changed - if so, hide hands temporarily to avoid crash
	int currentClass = pOwner->GetPlayerClass()->GetClassIndex();
	if (m_iLastPlayerClass != TF_CLASS_UNDEFINED && currentClass != m_iLastPlayerClass)
	{
		Msg("VR Update: %s hand detected class change %d -> %d, shutting down\n", 
			IsLeftHand() ? "LEFT" : "RIGHT", m_iLastPlayerClass, currentClass);
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
	
	// Two-handed weapon support - only for left hand
	if (IsLeftHand() && tfvr_twohand_enabled.GetBool())
	{
		// Get the right hand to check for grip target
		C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
		
		// Check if we should skip two-handing for this weapon
		// Some weapons don't have proper off-hand grip points
		bool bSkipTwoHand = false;
		bool bPassiveGripOnly = false;  // Passive grip = position snap only, no rotation control
		if (pRightHand)
		{
			C_TFWeaponBase *pWeapon = pRightHand->GetHeldWeapon();
			if (pWeapon)
			{
				int iWeaponID = pWeapon->GetWeaponID();
				// Heavy fists/gloves - left hand does its own melee
				if (iWeaponID == TF_WEAPON_FISTS)
				{
					bSkipTwoHand = true;
				}
				// Scout melee weapons - no off-hand grip
				else if (iWeaponID == TF_WEAPON_BAT ||
					iWeaponID == TF_WEAPON_BAT_WOOD ||      // Sandman
					iWeaponID == TF_WEAPON_BAT_GIFTWRAP ||  // Wrap Assassin
					iWeaponID == TF_WEAPON_BAT_FISH)        // Holy Mackerel, etc.
				{
					bSkipTwoHand = true;
				}
				// Throwables are one-handed - no off-hand grip
				else if (iWeaponID == TF_WEAPON_JAR ||
						 iWeaponID == TF_WEAPON_JAR_MILK ||
						 iWeaponID == TF_WEAPON_CLEAVER ||
						 iWeaponID == TF_WEAPON_JAR_GAS ||
						 iWeaponID == TF_WEAPON_THROWABLE)
				{
					bSkipTwoHand = true;
				}
				// Pistols - passive grip only (no active grip button required, no rotation control)
				else if (iWeaponID == TF_WEAPON_PISTOL ||
				         iWeaponID == TF_WEAPON_PISTOL_SCOUT ||
				         iWeaponID == TF_WEAPON_HANDGUN_SCOUT_SECONDARY)  // Pretty Boy's, Winger
				{
					bPassiveGripOnly = true;
				}
			}
		}
		
		if (bSkipTwoHand)
		{
			// Reset two-hand state for weapons without off-hand grip
			m_flTwoHandBlend = 0.0f;
			m_bOffhandGripActive = false;
			m_bWasOffhandGripActive = false;
			m_flGripRotationBlend = 0.0f;
		}
		else if (bPassiveGripOnly && pRightHand && pRightHand->GetHeldWeapon())
		{
			// Passive grip only (pistols): position snapping without rotation control
			// No grip button required, just distance-based blending
			Vector gripTargetPos;
			QAngle gripTargetAngles;
			
			if (pRightHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles))
			{
				// Get our current hand position - use middle finger proximal for snap detection
				Vector leftHandPos = m_vecLastValidPosition;
				if (m_pHandTracker)
				{
					Vector fingerPos;
					QAngle fingerAngles;
					if (m_pHandTracker->GetHandJoint(true, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fingerPos, fingerAngles))
					{
						leftHandPos = fingerPos;
					}
				}
				
				// Calculate distance to grip target
				float distance = (leftHandPos - gripTargetPos).Length();
				
				float snapDist = tfvr_twohand_snap_distance.GetFloat();
				float blendDist = tfvr_twohand_blend_distance.GetFloat();
				
				// Calculate blend amount based on distance only (no grip button required)
				float targetBlend = 0.0f;
				if (distance <= snapDist)
				{
					targetBlend = 1.0f;
				}
				else if (distance <= blendDist)
				{
					targetBlend = 1.0f - ((distance - snapDist) / (blendDist - snapDist));
				}
				
				// Smoothly interpolate towards target blend
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				m_flTwoHandBlend = EasedApproach(targetBlend, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
				
				// Passive grip does NOT set m_bOffhandGripActive or m_flGripRotationBlend
				// This means the weapon rotation is NOT influenced by the left hand
				m_bOffhandGripActive = false;
				m_bWasOffhandGripActive = false;
				m_flGripRotationBlend = 0.0f;
				
				if (tfvr_twohand_debug.GetBool())
				{
					// ORANGE box = pistol passive grip target
					debugoverlay->AddBoxOverlay(gripTargetPos, Vector(-1,-1,-1), Vector(1,1,1), 
						gripTargetAngles, 255, 128, 0, 128, 0.1f);
					
					// RED box = left hand middle finger position
					debugoverlay->AddBoxOverlay(leftHandPos, Vector(-0.5f,-0.5f,-0.5f), Vector(0.5f,0.5f,0.5f), 
						vec3_angle, 255, 0, 0, 128, 0.1f);
					
					// ORANGE line = distance being measured
					debugoverlay->AddLineOverlay(leftHandPos, gripTargetPos, 
						255, 128, 0, true, 0.1f);
				}
			}
			else
			{
				// No grip target, blend out
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
			}
		}
		else if (pRightHand && pRightHand->GetHeldWeapon())
		{
			Vector gripTargetPos;
			QAngle gripTargetAngles;
			
			if (pRightHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles))
			{
				// Get our current hand position - use OpenXR middle finger base for aiming target
				// This provides better pivot point alignment than the wrist
				Vector leftHandPos = m_vecLastValidPosition;
				if (m_pHandTracker)
				{
					Vector fingerBasePos;
					QAngle fingerBaseAngles;
					if (m_pHandTracker->GetHandJoint(true, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fingerBasePos, fingerBaseAngles))
					{
						leftHandPos = fingerBasePos;
					}
				}
				
				// Calculate distance to grip target
				float distance = (leftHandPos - gripTargetPos).Length();
				
				float snapDist = tfvr_twohand_snap_distance.GetFloat();
				float blendDist = tfvr_twohand_blend_distance.GetFloat();
				
				// Check if offhand grip button is pressed (for active two-hand aiming)
				float gripValue = 0.0f;
				if (g_pOpenXRManager && tfvr_offhand_grip_enabled.GetBool())
				{
					gripValue = g_pOpenXRManager->GetAnalogValue("left_grip");
				}
				
				bool bGripButtonPressed = gripValue >= tfvr_offhand_grip_threshold.GetFloat();
				float gripRange = tfvr_offhand_grip_range.GetFloat() * 0.393701f; // cm to inches (Source units)
				
				// Hysteresis: use larger range to release than to grab (prevents accidental ungrip)
				bool bWasGripActive = m_bOffhandGripActive;
				float effectiveRange = bWasGripActive 
					? gripRange * tfvr_offhand_grip_release_mult.GetFloat()  // Larger range to release
					: gripRange;                                              // Normal range to grab
				bool bWithinGripRange = distance <= effectiveRange;
				
				// Offhand grip is active when grip button is held AND within range
				bool bGripJustActivated = !m_bOffhandGripActive && bGripButtonPressed && bWithinGripRange;
				m_bOffhandGripActive = bGripButtonPressed && bWithinGripRange;
				
				// Track if grip was ever active (for blend-out tracking)
				if (m_bOffhandGripActive)
					m_bWasOffhandGripActive = true;
				
				// When grip just activates, clear stale direction but DON'T reset blend value
				// If we're mid-blend-out, we want to continue from current rotation, not jump
				if (bGripJustActivated)
				{
					// Clear the cached direction so it gets recalculated fresh this frame
					m_vecOffhandGripForward = vec3_origin;
					
					// Only reset rotation blend if we weren't already blending
					// (i.e., this is a fresh grip, not a re-grip during blend-out)
					if (!m_bWasOffhandGripActive)
					{
						m_flGripRotationBlend = 0.0f;
					}
					// Otherwise, keep current blend value - we'll blend UP from here
				}
				
				// If offhand grip is active, calculate the weapon rotation offset
				if (m_bOffhandGripActive)
				{
					// Get OpenXR positions: right wrist, left middle finger base
					Vector rightWristOpenXR = pRightHand->GetAbsOrigin(); // fallback
					Vector leftFingerBaseOpenXR = leftHandPos; // already has left middle finger base from above
					
					// Get right wrist from OpenXR
					COpenXRHandTracker* pRightHandTracker = pRightHand->GetHandTracker();
					if (pRightHandTracker)
					{
						Vector wristPos;
						QAngle wristAngles;
						if (pRightHandTracker->GetHandJoint(false, XR_HAND_JOINT_WRIST_EXT, wristPos, wristAngles))
						{
							rightWristOpenXR = wristPos;
						}
					}
					
					// Target direction: right wrist to left middle finger base
					Vector wristToFingerBase = leftFingerBaseOpenXR - rightWristOpenXR;
					float aimDistance = wristToFingerBase.Length();
					Vector aimDirection = (aimDistance > 0.1f) ? wristToFingerBase / aimDistance : wristToFingerBase;
					
					// Calculate weapon direction using feedback correction
					// This ensures the grip target aligns with the left middle finger base
					Vector gripTargetPos;
					QAngle gripTargetAngles;
					Vector dirToOffhand = aimDirection;
					
					if (pRightHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles))
					{
						Vector toGrip = gripTargetPos - rightWristOpenXR;
						if (toGrip.LengthSqr() > 0.1f)
						{
							Vector pivotAxis = toGrip;
							pivotAxis.NormalizeInPlace();
							
							// Error = where we want to point minus where grip target currently points
							Vector error = aimDirection - pivotAxis;
							
							// Start from last frame's direction (or aim direction if invalid)
							Vector currentY = m_vecOffhandGripForward;
							if (currentY.LengthSqr() < 0.1f)
								currentY = aimDirection;
							currentY.NormalizeInPlace();
							
							// Apply full error correction (rotation blend handles smoothing)
							dirToOffhand = currentY + error;
							dirToOffhand.NormalizeInPlace();
						}
					}
					
					// Get the right wrist's up direction for roll control
					Vector rightWristUp(0, 0, 1); // fallback to world up
					if (pRightHandTracker)
					{
						Vector wristPos;
						QAngle wristAngles;
						if (pRightHandTracker->GetHandJoint(false, XR_HAND_JOINT_WRIST_EXT, wristPos, wristAngles))
						{
							AngleVectors(wristAngles, nullptr, nullptr, &rightWristUp);
						}
					}
					
					// Store vectors
					m_vecOffhandGripForward = dirToOffhand;
					m_vecOffhandGripUp = rightWristUp;
					
					// Smoothly blend toward full grip (hand position) with easing
					float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
					float easePower = tfvr_offhand_grip_ease_power.GetFloat();
					m_flTwoHandBlend = EasedApproach(1.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
					
					// Smoothly blend weapon rotation (separate speed) with easing
					float rotBlendSpeed = tfvr_offhand_grip_rotation_blend_speed.GetFloat();
					m_flGripRotationBlend = EasedApproach(1.0f, m_flGripRotationBlend, rotBlendSpeed, gpGlobals->frametime, easePower);
					
					if (tfvr_twohand_debug.GetBool())
					{
						// GREEN line = right wrist to left finger base (desired pivot direction)
						debugoverlay->AddLineOverlay(rightWristOpenXR, leftFingerBaseOpenXR, 
							0, 255, 0, true, 0.1f);
						
						// CYAN line = current pivot axis (wrist to grip target)
						debugoverlay->AddLineOverlay(rightWristOpenXR, gripTargetPos, 
							0, 255, 255, true, 0.1f);
						
						// BLUE box = grip target position
						debugoverlay->AddBoxOverlay(gripTargetPos, Vector(-1,-1,-1), Vector(1,1,1), 
							vec3_angle, 0, 128, 255, 128, 0.1f);
						
						// YELLOW box = OpenXR left middle finger base
						debugoverlay->AddBoxOverlay(leftFingerBaseOpenXR, Vector(-1,-1,-1), Vector(1,1,1), 
							vec3_angle, 255, 255, 0, 128, 0.1f);
						
						// WHITE box = OpenXR right wrist
						debugoverlay->AddBoxOverlay(rightWristOpenXR, Vector(-1,-1,-1), Vector(1,1,1), 
							vec3_angle, 255, 255, 255, 128, 0.1f);
					}
				}
				else
				{
					// Clear cached offset when grip ends so next grip recalculates
					if (bWasGripActive)
					{
						m_vecCachedGripDelta = vec3_origin;
						m_vecCachedGripYAxis = Vector(0, 1, 0);
					}
					
					// If we WERE actively gripping but now released, blend out to 0
					// This ensures smooth ungrip transition
					if (m_bWasOffhandGripActive)
					{
						float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
						
						// Blend out weapon rotation (separate speed) with easing
						float rotBlendSpeed = tfvr_offhand_grip_rotation_blend_speed.GetFloat();
						m_flGripRotationBlend = EasedApproach(0.0f, m_flGripRotationBlend, rotBlendSpeed, gpGlobals->frametime, easePower);
						
						// Clear when BOTH blends are done
						if (m_flTwoHandBlend < 0.001f && m_flGripRotationBlend < 0.001f)
							m_bWasOffhandGripActive = false;
						
						if (tfvr_twohand_debug.GetBool())
						{
							static float lastDebugTime = 0;
							if (gpGlobals->curtime - lastDebugTime > 0.2f)
							{
								DevMsg("TwoHand: Blending out from grip, HandBlend=%.2f, RotBlend=%.2f\n", 
									m_flTwoHandBlend, m_flGripRotationBlend);
								lastDebugTime = gpGlobals->curtime;
							}
						}
					}
					else
					{
						// Passive two-handing: Calculate blend amount based on distance
						float targetBlend = 0.0f;
						if (distance <= snapDist)
						{
							// Full grip when within snap distance
							targetBlend = 1.0f;
						}
						else if (distance <= blendDist)
						{
							// Interpolate between snap and blend distance
							targetBlend = 1.0f - ((distance - snapDist) / (blendDist - snapDist));
						}
						
						// Smoothly interpolate towards target blend with easing
						float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(targetBlend, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
						
						if (tfvr_twohand_debug.GetBool())
						{
							static float lastDebugTime = 0;
							if (gpGlobals->curtime - lastDebugTime > 0.2f)
							{
								DevMsg("TwoHand: Distance=%.1f, TargetBlend=%.2f, CurrentBlend=%.2f, GripValue=%.2f\n", 
									distance, targetBlend, m_flTwoHandBlend, gripValue);
								lastDebugTime = gpGlobals->curtime;
							}
							
							// Draw yellow line for passive mode
							debugoverlay->AddLineOverlay(leftHandPos, gripTargetPos, 
								255, 255, 0, true, 0.1f);
						}
					}
				}
			}
			else
			{
				// No valid grip target, blend back to free hand
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
				m_bOffhandGripActive = false;
				if (m_flTwoHandBlend < 0.001f)
					m_bWasOffhandGripActive = false;  // Clear when fully blended out
			}
		}
		else
		{
			// No weapon in right hand, blend back to free hand
			float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
			float easePower = tfvr_offhand_grip_ease_power.GetFloat();
			m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
			m_bOffhandGripActive = false;
			if (m_flTwoHandBlend < 0.001f)
				m_bWasOffhandGripActive = false;  // Clear when fully blended out
		}
	}
	
	// Right-hand passive grip for left-hand medigun
	// When left hand holds medigun, right hand can do passive grip (position only, no weapon rotation)
	if (IsRightHand() && tfvr_twohand_enabled.GetBool())
	{
		C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
		
		// Only active when left hand is holding medigun
		bool bLeftHasMedigun = pLeftHand && IsWeaponMedigun(pLeftHand->GetHeldWeapon());
		
		if (tfvr_twohand_debug.GetBool())
		{
			static float lastDebugTime = 0;
			if (gpGlobals->curtime - lastDebugTime > 2.0f)
			{
				C_TFWeaponBase *pLeftWeapon = pLeftHand ? pLeftHand->GetHeldWeapon() : NULL;
				Msg("RightHand Update: pLeftHand=%p, LeftWeapon=%p (%s), IsMedigun=%d\n",
					pLeftHand, pLeftWeapon, 
					pLeftWeapon ? pLeftWeapon->GetClassname() : "none",
					bLeftHasMedigun ? 1 : 0);
				lastDebugTime = gpGlobals->curtime;
			}
		}
		
		if (bLeftHasMedigun)
		{
			Vector gripTargetPos;
			QAngle gripTargetAngles;
			
			if (tfvr_twohand_debug.GetBool())
			{
				static float lastDebugTime = 0;
				if (gpGlobals->curtime - lastDebugTime > 2.0f)
				{
					Msg("About to call GetOffHandGripTarget on left hand...\n");
					lastDebugTime = gpGlobals->curtime;
				}
			}
			
			bool bGotGripTarget = pLeftHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles);
			
			if (tfvr_twohand_debug.GetBool())
			{
				static float lastDebugTime2 = 0;
				if (gpGlobals->curtime - lastDebugTime2 > 2.0f)
				{
					Msg("GetOffHandGripTarget returned: %s, pos=(%.1f, %.1f, %.1f)\n", 
						bGotGripTarget ? "true" : "false",
						gripTargetPos.x, gripTargetPos.y, gripTargetPos.z);
					
					// Also show the left hand's actual position for comparison
					Vector leftHandPos = pLeftHand->GetAbsOrigin();
					Msg("  Left hand VR position: (%.1f, %.1f, %.1f)\n",
						leftHandPos.x, leftHandPos.y, leftHandPos.z);
					
					lastDebugTime2 = gpGlobals->curtime;
				}
			}
			
			if (bGotGripTarget)
			{
				// Get our current hand position - use middle finger proximal for snap detection
				Vector rightHandPos = m_vecLastValidPosition;
				if (m_pHandTracker)
				{
					Vector fingerPos;
					QAngle fingerAngles;
					if (m_pHandTracker->GetHandJoint(false, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fingerPos, fingerAngles))
					{
						rightHandPos = fingerPos;
					}
				}
				
				// Calculate distance to grip target (bip_hand_R position)
				float distance = (rightHandPos - gripTargetPos).Length();
				
				float snapDist = tfvr_twohand_snap_distance.GetFloat();
				float blendDist = tfvr_twohand_blend_distance.GetFloat();
				
				// Passive two-handing: Calculate blend amount based on distance only
				// No grip button required for passive grip
				float targetBlend = 0.0f;
				if (distance <= snapDist)
				{
					targetBlend = 1.0f;
				}
				else if (distance <= blendDist)
				{
					targetBlend = 1.0f - ((distance - snapDist) / (blendDist - snapDist));
				}
				
				// Smoothly interpolate towards target blend
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				m_flTwoHandBlend = EasedApproach(targetBlend, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
				
				// NOTE: Passive grip does NOT set m_bOffhandGripActive or m_flGripRotationBlend
				// This means the weapon rotation is NOT influenced by the right hand
				
				if (tfvr_twohand_debug.GetBool())
				{
					// MAGENTA box = medigun grip target (bip_hand_R from left hand model)
					debugoverlay->AddBoxOverlay(gripTargetPos, Vector(-1,-1,-1), Vector(1,1,1), 
						gripTargetAngles, 255, 0, 255, 128, 0.1f);
					
					// RED box = right hand middle finger position
					debugoverlay->AddBoxOverlay(rightHandPos, Vector(-0.5f,-0.5f,-0.5f), Vector(0.5f,0.5f,0.5f), 
						vec3_angle, 255, 0, 0, 128, 0.1f);
					
					// MAGENTA line = distance being measured
					debugoverlay->AddLineOverlay(rightHandPos, gripTargetPos, 
						255, 0, 255, true, 0.1f);
					
					static float lastDebugTime = 0;
					if (gpGlobals->curtime - lastDebugTime > 0.5f)
					{
						DevMsg("Medigun RightGrip: dist=%.1f, snapDist=%.1f, blend=%.2f, target=%.2f\n", 
							distance, snapDist, m_flTwoHandBlend, targetBlend);
						lastDebugTime = gpGlobals->curtime;
					}
				}
			}
			else
			{
				// No valid grip target, blend back
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
			}
		}
		else
		{
			// Left hand doesn't have medigun, reset right hand grip state
			if (m_flTwoHandBlend > 0.001f)
			{
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
			}
		}
	}
	
	// Check if the player's active weapon has changed
	// Per-weapon hand routing: medigun -> left hand, all other weapons -> right hand
	{
		C_TFWeaponBase *pActiveWeapon = pOwner->GetActiveTFWeapon();
		bool bIsMedigun = IsWeaponMedigun(pActiveWeapon);
		
		// Determine if this hand should hold the current weapon
		bool bThisHandShouldEquip = (IsRightHand() && !bIsMedigun) || (IsLeftHand() && bIsMedigun);
		
		// VR: Unequip weapons on round loss or stalemate (matches vanilla TF2 behavior)
		bool bShouldUnequipForRoundEnd = false;
		
		if (pOwner->m_Shared.IsLoser())
		{
			bShouldUnequipForRoundEnd = true;
		}
		else if (TFGameRules())
		{
			// Stalemate - everyone loses their weapons
			if (TFGameRules()->InStalemate())
			{
				bShouldUnequipForRoundEnd = true;
			}
			// Backup check: round has been won and we're not on the winning team
			else if (TFGameRules()->RoundHasBeenWon())
			{
				int iWinningTeam = TFGameRules()->GetWinningTeam();
				if (pOwner->GetTeamNumber() != iWinningTeam)
				{
					bShouldUnequipForRoundEnd = true;
				}
			}
		}
		
		// If we should unequip due to round end, do so and skip normal weapon logic
		if (bShouldUnequipForRoundEnd)
		{
			if (m_hHeldWeapon.Get() || m_hRenderWeapon.Get())
			{
				UnequipWeapon();
			}
		}
		// Unequip if this hand has a weapon but shouldn't (weapon type changed)
		else if (m_hHeldWeapon.Get() && !bThisHandShouldEquip)
		{
			UnequipWeapon();
		}
		else if (bThisHandShouldEquip)
		{
			// Normal weapon update logic
			C_TFWeaponBase *pCurrentHeld = m_hHeldWeapon.Get();
			
			// VR: Hide throwable weapons immediately when ammo hits 0
			// (i.e. the player just threw). The hand reverts to empty
			// pose until the auto weapon-switch kicks in.
			extern ConVar tfvr_physical_throw;
			bool bThrowableNoAmmo = false;
			if (tfvr_physical_throw.GetBool() && pActiveWeapon)
			{
				int wid = pActiveWeapon->GetWeaponID();
				if (wid == TF_WEAPON_JAR || wid == TF_WEAPON_JAR_MILK ||
					wid == TF_WEAPON_CLEAVER || wid == TF_WEAPON_JAR_GAS ||
					wid == TF_WEAPON_THROWABLE)
				{
					if (pOwner->GetAmmoCount(pActiveWeapon->GetPrimaryAmmoType()) <= 0)
						bThrowableNoAmmo = true;
				}
			}

			if (bThrowableNoAmmo)
			{
				if (pCurrentHeld)
					UnequipWeapon();
			}
			else
			{
				// Detect weapon change: either different weapon, or current held weapon is now invalid
				bool bNeedsWeaponUpdate = false;

				if (pActiveWeapon != pCurrentHeld)
				{
					bNeedsWeaponUpdate = true;
				}
				else if (pCurrentHeld && !pCurrentHeld->GetOwner())
				{
					// Held weapon is orphaned (regenerated/respawned), need to refresh
					bNeedsWeaponUpdate = true;
				}

				if (bNeedsWeaponUpdate)
				{
					if (pActiveWeapon)
					{
						EquipWeapon(pActiveWeapon);
					}
					else
					{
						UnequipWeapon();
					}
				}
			}
		}
	}

	// Update this hand's position and orientation
	UpdateHandTransform();

	// Update bone animation from hand tracking
	UpdateHandBones();
	
	// Update weapon position if we're holding one
	UpdateWeaponTransform();
	
	// Sync skins for hands and weapons (team colors, crit effects, etc.)
	UpdateSkins();
	
	// Update crit boost effect on right hand
	if (IsRightHand())
	{
		UpdateCritBoostEffect();
	}

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
	
	// Debug bone visualization - show all bones in the hand model skeleton
	if (tfvr_hands_debug_bones.GetInt() > 0 && debugoverlay)
	{
		// Draw hand model bones (mode 1 or 3)
		if ((tfvr_hands_debug_bones.GetInt() & 1) && GetModelPtr())
		{
			// Ensure bones are set up
			SetupBones(NULL, -1, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
			
			CStudioHdr *pStudioHdr = GetModelPtr();
			int numBones = pStudioHdr->numbones();
			
			for (int i = 0; i < numBones; i++)
			{
				const mstudiobone_t *pBone = pStudioHdr->pBone(i);
				if (!pBone)
					continue;
				
				// Get bone position in world space
				matrix3x4_t boneMatrix;
				GetBoneTransform(i, boneMatrix);
				
				Vector bonePos;
				MatrixPosition(boneMatrix, bonePos);
				
				// Skip bones at origin (not set up properly)
				if (bonePos.IsZero())
					continue;
				
				// Draw bone as small box
				int r = IsLeftHand() ? 100 : 255;
				int g = IsLeftHand() ? 255 : 100;
				int b = 100;
				
				debugoverlay->AddBoxOverlay(bonePos, Vector(-0.5, -0.5, -0.5), Vector(0.5, 0.5, 0.5), 
					vec3_angle, r, g, b, 150, 0.0f);
				
				// Draw bone name
				debugoverlay->AddTextOverlay(bonePos, 0.0f, "%d: %s", i, pBone->pszName());
				
				// Draw line to parent bone
				int parentIdx = pBone->parent;
				if (parentIdx >= 0 && parentIdx < numBones)
				{
					matrix3x4_t parentMatrix;
					GetBoneTransform(parentIdx, parentMatrix);
					
					Vector parentPos;
					MatrixPosition(parentMatrix, parentPos);
					
					if (!parentPos.IsZero())
					{
						debugoverlay->AddLineOverlay(parentPos, bonePos, r, g, b, true, 0.0f);
					}
				}
			}
		}
		
		// Draw weapon model bones (mode 2 or 3)
		C_VRRenderWeapon *pRenderWeapon = dynamic_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
		if ((tfvr_hands_debug_bones.GetInt() & 2) && pRenderWeapon && pRenderWeapon->GetModelPtr())
		{
			// Ensure bones are set up
			pRenderWeapon->SetupBones(NULL, -1, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
			
			CStudioHdr *pStudioHdr = pRenderWeapon->GetModelPtr();
			int numBones = pStudioHdr->numbones();
			
			for (int i = 0; i < numBones; i++)
			{
				const mstudiobone_t *pBone = pStudioHdr->pBone(i);
				if (!pBone)
					continue;
				
				// Get bone position in world space
				matrix3x4_t boneMatrix;
				pRenderWeapon->GetBoneTransform(i, boneMatrix);
				
				Vector bonePos;
				MatrixPosition(boneMatrix, bonePos);
				
				// Skip bones at origin (not set up properly)
				if (bonePos.IsZero())
					continue;
				
				// Draw weapon bones in different color (yellow/orange)
				int r = 255;
				int g = 200;
				int b = 0;
				
				debugoverlay->AddBoxOverlay(bonePos, Vector(-0.3, -0.3, -0.3), Vector(0.3, 0.3, 0.3), 
					vec3_angle, r, g, b, 150, 0.0f);
				
				// Draw bone name
				debugoverlay->AddTextOverlay(bonePos, 0.0f, "W%d: %s", i, pBone->pszName());
				
				// Draw line to parent bone
				int parentIdx = pBone->parent;
				if (parentIdx >= 0 && parentIdx < numBones)
				{
					matrix3x4_t parentMatrix;
					pRenderWeapon->GetBoneTransform(parentIdx, parentMatrix);
					
					Vector parentPos;
					MatrixPosition(parentMatrix, parentPos);
					
					if (!parentPos.IsZero())
					{
						debugoverlay->AddLineOverlay(parentPos, bonePos, r, g, b, true, 0.0f);
					}
				}
			}
		}
	}
	
	// Update left hand wearables (scout ball visibility based on ammo, watch position)
	if (IsLeftHand())
	{
		UpdateLeftHandBall();
		
		// Update watch position if we have one
		C_BaseAnimating *pWatch = m_hLeftHandWatch.Get();
		if (pWatch)
		{
			// Get the weapon_bone_L attachment position for watch
			int iAttachment = LookupAttachment("weapon_bone_L");
			if (iAttachment > 0)
			{
				Vector attachPos;
				QAngle attachAng;
				if (GetAttachment(iAttachment, attachPos, attachAng))
				{
					pWatch->SetAbsOrigin(attachPos);
					pWatch->SetAbsAngles(attachAng);
				}
			}
		}
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
// Purpose: Get palm transform as a matrix - CANONICAL reference for offsets
//          The OpenXR palm joint has standardized orientation:
//          +X points along metacarpals (toward fingers)
//          +Y points out of palm (palm normal)
//          +Z points toward thumb side
//          This is consistent across runtimes and input types.
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetPalmTransform(VMatrix& outTransform)
{
	if (!m_pHandTracker)
		return false;
	
	// Get palm position and angles
	Vector palmPos;
	QAngle palmAngles;
	
	if (!m_pHandTracker->GetHandJoint(IsLeftHand(), XR_HAND_JOINT_PALM_EXT, palmPos, palmAngles))
		return false;
	
	// Convert to matrix
	matrix3x4_t temp;
	AngleMatrix(palmAngles, palmPos, temp);
	outTransform.CopyFrom3x4(temp);
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Update this hand's position from hand tracking palm position
//          Uses PALM joint as canonical reference for consistent offsets
//          across different controllers and OpenXR runtimes.
//          We'll position the hand bones via SetupBones override later.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateHandTransform()
{
	if (m_bShuttingDown)
		return;
		
	if (!g_pOpenXRManager || !m_pHandTracker)
		return;

	// Try to get PALM position using matrix (canonical reference)
	// Palm joint has standardized orientation per OpenXR spec:
	// +X toward fingers, +Y out of palm, +Z toward thumb
	VMatrix palmMatrix;
	
	bool handValid = GetPalmTransform(palmMatrix);
	
	// Aim stabilization: when a weapon is equipped, SteamVR's hand tracking skeleton
	// shifts the palm/wrist orientation when grip/trigger analog values change (because
	// it synthesizes a hand pose from controller inputs). This wobbles the weapon aim.
	// Fix: derive the palm transform from the controller aim pose (which is physically
	// stable) plus a fixed offset captured at equip time. Finger bones are already
	// overridden by the weapon grip animation, so nothing is lost visually.
	if (handValid && m_hHeldWeapon.Get() && tfvr_aim_stabilize.GetBool())
	{
		VMatrix controllerPose;
		bool bControllerValid = IsLeftHand() ?
			g_pOpenXRManager->GetLeftControllerPose(controllerPose) :
			g_pOpenXRManager->GetRightControllerPose(controllerPose);
		
		if (bControllerValid)
		{
			if (!m_bAimRefValid)
			{
				// First frame with weapon: capture palm position relative to controller
				matrix3x4_t controllerInv;
				MatrixInvert(controllerPose.As3x4(), controllerInv);
				ConcatTransforms(controllerInv, palmMatrix.As3x4(), m_matAimRefPalmOffset);
				m_bAimRefValid = true;
			}
			else
			{
				// Slowly adapt reference so it doesn't lock permanently to the
				// initial grip state (handles player re-gripping the controller)
				float adaptRate = tfvr_aim_stabilize_adapt.GetFloat();
				if (adaptRate > 0.0f && gpGlobals->frametime > 0.0f)
				{
					matrix3x4_t controllerInv;
					MatrixInvert(controllerPose.As3x4(), controllerInv);
					matrix3x4_t currentPalmOffset;
					ConcatTransforms(controllerInv, palmMatrix.As3x4(), currentPalmOffset);
					
					float lerpAmount = 1.0f - expf(-adaptRate * gpGlobals->frametime);
					
					Quaternion refQuat, curQuat, blendedQuat;
					Vector refPos, curPos, blendedPos;
					MatrixAngles(m_matAimRefPalmOffset, refQuat, refPos);
					MatrixAngles(currentPalmOffset, curQuat, curPos);
					QuaternionSlerp(refQuat, curQuat, lerpAmount, blendedQuat);
					VectorLerp(refPos, curPos, lerpAmount, blendedPos);
					QuaternionMatrix(blendedQuat, blendedPos, m_matAimRefPalmOffset);
				}
			}
			
			// Stabilized palm = controller * refPalmOffset
			matrix3x4_t stabilizedPalm;
			ConcatTransforms(controllerPose.As3x4(), m_matAimRefPalmOffset, stabilizedPalm);
			
			MatrixAngles(stabilizedPalm, m_angLastValidAngles, m_vecLastValidPosition);
			m_bControllerTracked = true;
			
			if (tfvr_aim_stabilize_debug.GetBool())
			{
				Vector rawPos = palmMatrix.GetTranslation();
				float posDelta = (m_vecLastValidPosition - rawPos).Length();
				if (posDelta > 0.1f)
				{
					DevMsg("AimStab: palm shift %.2f units corrected\n", posDelta);
				}
			}
			
			goto applyTransform;
		}
	}
	
	// Normal path: use palm, fallback to wrist, then controller
	if (handValid)
	{
		m_vecLastValidPosition = palmMatrix.GetTranslation();
		MatrixAngles(palmMatrix.As3x4(), m_angLastValidAngles);
		m_bControllerTracked = true;
	}
	else
	{
		// Try wrist as secondary fallback
		VMatrix wristMatrix;
		if (GetWristTransform(wristMatrix))
		{
			m_vecLastValidPosition = wristMatrix.GetTranslation();
			MatrixAngles(wristMatrix.As3x4(), m_angLastValidAngles);
			m_bControllerTracked = true;
		}
		else
		{
			// Final fallback to controller pose
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
	}
	
applyTransform:

	// Position the entity at the palm position
	// The animation bones will be positioned relative to this
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
// Purpose: Update this hand's position using FRESHLY SAMPLED XR pose
//          This bypasses the cached poses and directly queries OpenXR
//          for the most up-to-date controller position. Use this when
//          spawning effects that need to match current visual position.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateHandTransformFresh()
{
	if (m_bShuttingDown)
		return;
		
	if (!g_pOpenXRManager)
		return;

	Vector freshPos;
	QAngle freshAngles;
	bool bGotPose = false;
	
	if (IsLeftHand())
		bGotPose = g_pOpenXRManager->SampleFreshLeftControllerPose(freshPos, freshAngles);
	else
		bGotPose = g_pOpenXRManager->SampleFreshRightControllerPose(freshPos, freshAngles);
	
	if (bGotPose)
	{
		m_vecLastValidPosition = freshPos;
		m_angLastValidAngles = freshAngles;
		m_bControllerTracked = true;
		
		SetAbsOrigin(m_vecLastValidPosition);
		SetAbsAngles(m_angLastValidAngles);
	}
	// If fresh sampling failed, fall back to regular update
	else
	{
		UpdateHandTransform();
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
		m_bHandBoneOffsetValid = false;
		m_iHandBone = -1;
		return true;
	}

	// Position bones for VR hand
	if (m_iHandBone >= 0 && m_iHandBone < nMaxBones && m_bControllerTracked)
	{
		// Sample the current animation directly using IBoneSetup
		// This bypasses any entity interpolation for instant pose changes
		int numBones = pStudioHdr->numbones();
		int currentSeq = GetSequence();
		float currentCycle = GetCycle();
		
		// If fire or charge animation is playing, use the actual current sequence
		// so the hand bone movement produces visible position offset on the weapon.
		// Otherwise use idle sequence for the weapon pose.
		bool bUseCurrentAnim = m_bPlayingFireAnim || m_bPlayingChargeAnim;
		int seqToSample = bUseCurrentAnim ? currentSeq : m_iIdleSequence;
		float cycleToSample = bUseCurrentAnim ? currentCycle : 0.0f;
		
		if (seqToSample < 0)
		{
			// No idle sequence set - check if we're gripping the medigun
			// If so, use the "idle" animation which has grip poses
			if (IsRightHand() && m_flTwoHandBlend > 0.01f)
			{
				C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
				if (pLeftHand && IsWeaponMedigun(pLeftHand->GetHeldWeapon()))
				{
					seqToSample = LookupSequence("idle");
				}
			}

			// Left hand with fists: use the correct fist idle so vm_weapon_bone
			// chain is in the right configuration for glove bone merging
			if (seqToSample < 0 && IsLeftHand())
			{
				C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
				C_TFWeaponBase *pRightWeapon = pRightHand ? pRightHand->GetHeldWeapon() : NULL;
				if (pRightWeapon && pRightWeapon->GetWeaponID() == TF_WEAPON_FISTS)
				{
					seqToSample = LookupSequence(GetFistsIdleAnimName(pRightWeapon));
				}
			}

			if (seqToSample < 0)
				seqToSample = 0;  // Fallback to first sequence
		}
		
		// Sample animation directly
		float poseParameters[MAXSTUDIOPOSEPARAM];
		memset(poseParameters, 0, sizeof(poseParameters));
		
		IBoneSetup boneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters);
		
		Vector posAnim[MAXSTUDIOBONES];
		Quaternion qAnim[MAXSTUDIOBONES];
		for (int i = 0; i < MAXSTUDIOBONES; i++)
		{
			posAnim[i].Init();
			qAnim[i].Init(0, 0, 0, 1);
		}
		boneSetup.InitPose(posAnim, qAnim);
		boneSetup.AccumulatePose(posAnim, qAnim, seqToSample, cycleToSample, 1.0f, gpGlobals->curtime, NULL);
		
		// Flamethrower blend: smoothly lerp between idle and fire poses using
		// m_flFlamethrowerFireBlend (ramped in UpdateFlamethrowerFireAnimation).
		// This handles both ease-in (starting fire) and ease-out (stopping fire).
		if (m_bPlayingFireAnim && m_flFlamethrowerFireBlend > 0.0f
			&& m_flFlamethrowerFireBlend < 1.0f
			&& IsWeaponFlamethrower(m_hHeldWeapon.Get()))
		{
			float flBlend = SimpleSpline(m_flFlamethrowerFireBlend);

			Vector posIdle[MAXSTUDIOBONES];
			Quaternion qIdle[MAXSTUDIOBONES];
			for (int i = 0; i < MAXSTUDIOBONES; i++)
			{
				posIdle[i].Init();
				qIdle[i].Init(0, 0, 0, 1);
			}
			boneSetup.InitPose(posIdle, qIdle);
			if (m_iIdleSequence >= 0)
				boneSetup.AccumulatePose(posIdle, qIdle, m_iIdleSequence, 0.0f, 1.0f, gpGlobals->curtime, NULL);

			for (int i = 0; i < numBones; i++)
			{
				VectorLerp(posIdle[i], posAnim[i], flBlend, posAnim[i]);
				QuaternionSlerp(qIdle[i], qAnim[i], flBlend, qAnim[i]);
			}
		}
		
		// During continuous-fire weapon animations (medigun, flamethrower), override
		// the hand bone's LOCAL position with the idle value. This prevents the fire
		// animation from translating the hand bone, which would shift the entire
		// skeleton (including weapon_bone) and break weapon alignment.
		// Rotation is preserved so the fire animation's grip squeeze still shows.
		bool bMedigunFireActive = (m_eMedigunFireState != MEDIGUN_FIRE_IDLE) && m_bPlayingFireAnim;
		if (bMedigunFireActive && m_bHandBoneOffsetValid)
		{
			posAnim[m_iHandBone] = m_vecIdleHandBoneLocalPos;
		}
		
		// Build bone transforms from sampled animation
		matrix3x4_t sampledBones[MAXSTUDIOBONES];
		for (int i = 0; i < numBones; i++)
		{
			matrix3x4_t boneToParent;
			QuaternionMatrix(qAnim[i], posAnim[i], boneToParent);
			
			const mstudiobone_t *pBone = pStudioHdr->pBone(i);
			if (!pBone)
			{
				SetIdentityMatrix(sampledBones[i]);
				continue;
			}
			
			if (pBone->parent == -1)
				MatrixCopy(boneToParent, sampledBones[i]);
			else if (pBone->parent >= 0 && pBone->parent < numBones)
				ConcatTransforms(sampledBones[pBone->parent], boneToParent, sampledBones[i]);
			else
				SetIdentityMatrix(sampledBones[i]);
		}
		
		// Cache the hand bone transform from the sampled animation.
		// Used by the normal anchor path for stable weapon positioning.
		// For hands WITH a weapon: cache once (stable idle offset).
		// For hands WITHOUT a weapon (e.g. right hand passive grip): recache every frame
		// because the sampled animation changes as m_flTwoHandBlend transitions.
		if (!m_bHandBoneOffsetValid || !m_hHeldWeapon.Get())
		{
			MatrixCopy(sampledBones[m_iHandBone], m_matIdleHandBoneTransform);
			m_bHandBoneOffsetValid = true;
		}
		
		// Get VR controller transform (where we want the hand bone to be)
		matrix3x4_t controllerTransform;
		AngleMatrix(m_angLastValidAngles, m_vecLastValidPosition, controllerTransform);
		
		// NOTE: Two-handed weapon blending for left hand is handled LATER in SetupBones
		// (around line 1830+) where the full skeleton is blended toward the grip pose.
		// We don't pre-blend the controllerTransform here to avoid double-blending
		// which causes micro-stuttering during movement.
		
		// Apply hand rotation offsets - supports per-class or global offsets
		// These offsets are now relative to the PALM joint which has standardized OpenXR orientation
		QAngle offsetAngles(0, 0, 0);
		
		// Try per-class offsets first
		C_TFPlayer *pOffsetOwner = m_hOwnerPlayer.Get();
		int playerClass = pOffsetOwner ? pOffsetOwner->GetPlayerClass()->GetClassIndex() : TF_CLASS_UNDEFINED;
		
		if (!GetPerClassHandOffset(playerClass, IsLeftHand(), offsetAngles))
		{
			// Fall back to global offsets
			ConVar *pOffsetPitch = IsLeftHand() ? &tfvr_hands_left_offset_pitch : &tfvr_hands_right_offset_pitch;
			ConVar *pOffsetYaw = IsLeftHand() ? &tfvr_hands_left_offset_yaw : &tfvr_hands_right_offset_yaw;
			ConVar *pOffsetRoll = IsLeftHand() ? &tfvr_hands_left_offset_roll : &tfvr_hands_right_offset_roll;
			
			offsetAngles.x = pOffsetPitch->GetFloat();
			offsetAngles.y = pOffsetYaw->GetFloat();
			offsetAngles.z = pOffsetRoll->GetFloat();
		}
		
		if (offsetAngles.x != 0 || offsetAngles.y != 0 || offsetAngles.z != 0)
		{
			matrix3x4_t offsetMatrix;
			AngleMatrix(offsetAngles, vec3_origin, offsetMatrix);
			
			matrix3x4_t temp;
			ConcatTransforms(controllerTransform, offsetMatrix, temp);
			MatrixCopy(temp, controllerTransform);
		}
		
		// Apply position offset to controllerTransform BEFORE grip calculations
		// This ensures all bones and weapon are at the offset position naturally.
		// For left hand when gripping: the offset is applied here, and GetOffHandGripTarget
		// also includes offset, so we need to subtract left offset when gripping (handled below).
		Vector posOffset(0, 0, 0);
		if (!GetPerClassHandPositionOffset(playerClass, IsLeftHand(), posOffset))
		{
			if (IsLeftHand())
			{
				posOffset.x = tfvr_hands_left_offset_x.GetFloat();
				posOffset.y = tfvr_hands_left_offset_y.GetFloat();
				posOffset.z = tfvr_hands_left_offset_z.GetFloat();
			}
			else
			{
				posOffset.x = tfvr_hands_right_offset_x.GetFloat();
				posOffset.y = tfvr_hands_right_offset_y.GetFloat();
				posOffset.z = tfvr_hands_right_offset_z.GetFloat();
			}
		}
		
		Vector worldPosOffset(0, 0, 0);
		if (posOffset.x != 0 || posOffset.y != 0 || posOffset.z != 0)
		{
			Vector palmX, palmY, palmZ;
			MatrixGetColumn(controllerTransform, 0, palmX);
			MatrixGetColumn(controllerTransform, 1, palmY);
			MatrixGetColumn(controllerTransform, 2, palmZ);
			worldPosOffset = palmX * posOffset.x + palmY * posOffset.y + palmZ * posOffset.z;
			
			// Apply to controllerTransform position
			Vector ctrlPos;
			MatrixGetColumn(controllerTransform, 3, ctrlPos);
			ctrlPos += worldPosOffset;
			MatrixSetColumn(ctrlPos, 3, controllerTransform);
		}
		
		// Apply offhand grip rotation to weapon hand (right hand)
		// Only applies during active grip (button held), not passive two-handing
		// Uses separate m_flGripRotationBlend for smooth rotation transitions
		if (IsRightHand() && tfvr_offhand_grip_enabled.GetBool())
		{
			C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
			float rotationBlend = pLeftHand ? pLeftHand->GetGripRotationBlend() : 0.0f;
			bool bWasGripActive = pLeftHand && pLeftHand->WasOffhandGripActive();
			bool bIsGripActive = pLeftHand && pLeftHand->IsOffhandGripActive();
			
			// Apply rotation when grip was active (includes blend-out after release)
			// Uses separate rotation blend that starts fresh each time grip activates
			if (rotationBlend > 0.001f && bWasGripActive)
			{
				Vector desiredY = pLeftHand->GetOffhandGripForward();
				
				// Skip if we don't have a valid direction yet (just activated, will be set next frame)
				if (desiredY.LengthSqr() < 0.1f)
				{
					// No valid direction - skip rotation this frame
				}
				else
				{
					// Capture pre-rotation state
					Quaternion preGripQuat;
					Vector preGripPos;
					MatrixAngles(controllerTransform, preGripQuat, preGripPos);
					
					// Apply full grip rotation
					ApplyTwoHandGripRotation(controllerTransform, desiredY);
					
					// Capture post-rotation state
					Quaternion gripQuat;
					Vector gripPos;
					MatrixAngles(controllerTransform, gripQuat, gripPos);
					
					// Blend between pre and post rotation with easing
					float easePower = tfvr_offhand_grip_ease_power.GetFloat();
					float easedRotBlend = ApplyEaseOutToBlend(rotationBlend, easePower, bIsGripActive);
					
					Quaternion blendedQuat;
					SafeQuaternionSlerp(preGripQuat, gripQuat, easedRotBlend, blendedQuat);
					QuaternionMatrix(blendedQuat, preGripPos, controllerTransform);
					
					if (tfvr_twohand_debug.GetBool())
					{
						Vector pos(controllerTransform[0][3], controllerTransform[1][3], controllerTransform[2][3]);
						debugoverlay->AddLineOverlay(pos, pos + desiredY * 45.0f, 0, 255, 255, true, 0.1f);
					}
				}
			}
		}
		
		// Calculate anchor delta from cached idle hand bone to controller
		// anchorDelta = controller * inverse(idleHandBone)
		// Position suppression for medigun fire is handled above by overriding
		// the hand bone's local position in the animation data before building the hierarchy.
		matrix3x4_t anchorDelta;
		
		if (m_bHandBoneOffsetValid)
		{
			matrix3x4_t invIdleHandBone;
			MatrixInvert(m_matIdleHandBoneTransform, invIdleHandBone);
			ConcatTransforms(controllerTransform, invIdleHandBone, anchorDelta);
		}
		else
		{
			matrix3x4_t invSampledHandBone;
			MatrixInvert(sampledBones[m_iHandBone], invSampledHandBone);
			ConcatTransforms(controllerTransform, invSampledHandBone, anchorDelta);
		}
		
		// Apply anchor delta to ALL sampled bones and write to output
		for (int i = 0; i < numBones && i < nMaxBones; i++)
		{
			ConcatTransforms(anchorDelta, sampledBones[i], pBoneToWorldOut[i]);
		}
		
		// Calculate position offset for this hand (will be applied at different times for left vs right)
		// Position is palm-relative: X=toward fingers, Y=out of palm, Z=toward thumb
		// DEBUG: Show all three axes of the hand bone
		if (IsRightHand() && tfvr_twohand_debug.GetBool() && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			Vector finalBonePos, boneX, boneY, boneZ;
			MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, finalBonePos);
			MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 0, boneX); // Forward?
			MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 1, boneY); // Right/Left?
			MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 2, boneZ); // Up?
			
			// X axis = YELLOW
			debugoverlay->AddLineOverlay(finalBonePos, finalBonePos + boneX * 25.0f, 
				255, 255, 0, true, 0.1f);
			// Y axis = ORANGE  
			debugoverlay->AddLineOverlay(finalBonePos, finalBonePos + boneY * 25.0f, 
				255, 128, 0, true, 0.1f);
			// Z axis = WHITE
			debugoverlay->AddLineOverlay(finalBonePos, finalBonePos + boneZ * 25.0f, 
				255, 255, 255, true, 0.1f);
		}
		
		// Apply finger tracking or weapon pose to this hand
		if (m_hHeldWeapon.Get())
		{
			// Override finger tracking with weapon grip pose
			ApplyWeaponPose(pBoneToWorldOut, nMaxBones);
			
			// Position weapon from current bones (position offset applied later)
			PositionWeaponFromBones(pBoneToWorldOut, nMaxBones);
		}
		else if (IsRightHand() && m_flTwoHandBlend > 0.01f)
		{
			// Right hand passive grip for left hand's medigun
			// Use GetOffHandGripTarget on the left hand to get the grip position
			C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
			bool bUsedGripPose = false;
			
			if (pLeftHand && IsWeaponMedigun(pLeftHand->GetHeldWeapon()))
			{
				Vector gripTargetPos;
				QAngle gripTargetAngles;
				
				// GetOffHandGripTarget on LEFT hand returns the bip_hand_R position in world space
				if (pLeftHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles, true))
				{
					// Apply easing to the blend
					float easePower = tfvr_offhand_grip_ease_power.GetFloat();
					float easedBlend = ApplyEaseOutToBlend(m_flTwoHandBlend, easePower, true);
					
					// Get current hand position
					Vector currentPos;
					QAngle currentAngles;
					MatrixAngles(pBoneToWorldOut[m_iHandBone], currentAngles, currentPos);
					
					// Blend position
					Vector blendedPos;
					VectorLerp(currentPos, gripTargetPos, easedBlend, blendedPos);
					
					// Blend rotation
					Quaternion currentQuat, gripQuat, blendedQuat;
					AngleQuaternion(currentAngles, currentQuat);
					AngleQuaternion(gripTargetAngles, gripQuat);
					QuaternionSlerp(currentQuat, gripQuat, easedBlend, blendedQuat);
					
					// Calculate position offset to apply to all bones
					Vector posOffset = blendedPos - currentPos;
					
					// Calculate rotation delta to apply around the hand position
					Quaternion invCurrentQuat;
					QuaternionInvert(currentQuat, invCurrentQuat);
					Quaternion rotDelta;
					QuaternionMult(blendedQuat, invCurrentQuat, rotDelta);
					
					matrix3x4_t rotDeltaMatrix;
					QuaternionMatrix(rotDelta, vec3_origin, rotDeltaMatrix);
					
					// Apply offset and rotation to ALL bones (moves entire skeleton as one unit)
					for (int i = 0; i < numBones; i++)
					{
						Vector bonePos;
						QAngle boneAngles;
						MatrixAngles(pBoneToWorldOut[i], boneAngles, bonePos);
						
						// Move bone by the position offset
						bonePos += posOffset;
						
						// Rotate position around the NEW hand position
						Vector relPos = bonePos - blendedPos;
						Vector rotatedRelPos;
						VectorRotate(relPos, rotDeltaMatrix, rotatedRelPos);
						bonePos = blendedPos + rotatedRelPos;
						
						// Rotate the bone's orientation
						Quaternion boneQuat;
						AngleQuaternion(boneAngles, boneQuat);
						Quaternion rotatedQuat;
						QuaternionMult(rotDelta, boneQuat, rotatedQuat);
						
						QAngle rotatedAngles;
						QuaternionAngles(rotatedQuat, rotatedAngles);
						
						AngleMatrix(rotatedAngles, bonePos, pBoneToWorldOut[i]);
					}
					
					bUsedGripPose = true;
					
					if (tfvr_twohand_debug.GetBool())
					{
						static float lastDebugTime = 0;
						if (gpGlobals->curtime - lastDebugTime > 0.5f)
						{
							DevMsg("TwoHand RightGrip: blend=%.2f, target=(%.1f, %.1f, %.1f), current=(%.1f, %.1f, %.1f)\n", 
								m_flTwoHandBlend, gripTargetPos.x, gripTargetPos.y, gripTargetPos.z,
								currentPos.x, currentPos.y, currentPos.z);
							lastDebugTime = gpGlobals->curtime;
						}
					}
				}
			}
			
			// Only apply finger tracking if we didn't get a grip pose
			if (!bUsedGripPose)
			{
				ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
			}
			// When gripping, DON'T apply finger tracking - use the sampled grip pose
		}
		else if (IsLeftHand() && m_flTwoHandBlend > 0.01f)
		{
			// When two-handing:
			// 1. Get the grip target (bip_hand_L position) from the right hand
			// 2. Sample the SAME animation on the LEFT hand model (which has finger bones)
			// 3. Reposition the left hand skeleton so its wrist matches the grip target
			
			C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
			bool bUsedGripPose = false;
			
			if (pRightHand && pRightHand->GetHeldWeapon())
			{
				// Get the grip target position/rotation from the right hand
				Vector gripTargetPos;
				QAngle gripTargetAngles;
				
				// Pass true to get the animated grip position (follows fire animation recoil)
				// This is for visual positioning - weapon rotation uses separate call with false
				if (pRightHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles, true))
				{
					bUsedGripPose = true;
					
					// Sample the right hand's CURRENT animation for the left hand finger pose
					// This ensures the left hand always matches the right hand's animation state
					int rightSeq = pRightHand->GetSequence();
					float rightCycle = pRightHand->GetCycle();
					
					// Sample animation on our (left hand) model using the right hand's state
					float poseParams[MAXSTUDIOPOSEPARAM];
					memset(poseParams, 0, sizeof(poseParams));
					
					IBoneSetup gripBoneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParams);
					
					Vector gripPosAnim[MAXSTUDIOBONES];
					Quaternion gripQAnim[MAXSTUDIOBONES];
					for (int i = 0; i < MAXSTUDIOBONES; i++)
					{
						gripPosAnim[i].Init();
						gripQAnim[i].Init(0, 0, 0, 1);
					}
					gripBoneSetup.InitPose(gripPosAnim, gripQAnim);
					
					// Always use the right hand's current animation - the left hand model has
					// the same animations, so this keeps both hands perfectly in sync
					gripBoneSetup.AccumulatePose(gripPosAnim, gripQAnim, rightSeq, rightCycle, 1.0f, gpGlobals->curtime, NULL);
					
					// Build grip pose skeleton in model space
					matrix3x4_t gripSampledBones[MAXSTUDIOBONES];
					for (int i = 0; i < numBones; i++)
					{
						matrix3x4_t boneToParent;
						QuaternionMatrix(gripQAnim[i], gripPosAnim[i], boneToParent);
						
						const mstudiobone_t *pBone = pStudioHdr->pBone(i);
						if (!pBone)
						{
							SetIdentityMatrix(gripSampledBones[i]);
							continue;
						}
						
						if (pBone->parent == -1)
							MatrixCopy(boneToParent, gripSampledBones[i]);
						else if (pBone->parent >= 0 && pBone->parent < numBones)
							ConcatTransforms(gripSampledBones[pBone->parent], boneToParent, gripSampledBones[i]);
						else
							SetIdentityMatrix(gripSampledBones[i]);
					}
					
					// Find our hand bone (bip_hand_L) and calculate offset to move it to grip target
					// The left hand model uses _L bones as its primary bones
					int leftHandBone = m_iHandBone;  // This should be bip_hand_L on the left hand model
					
					if (leftHandBone >= 0)
					{
						// Create grip target transform
						matrix3x4_t gripTargetTransform;
						AngleMatrix(gripTargetAngles, gripTargetPos, gripTargetTransform);
						
						// Calculate grip anchor delta = gripTarget * inverse(sampledHandBone)
						// This will move the sampled skeleton so the hand bone matches the grip target
						matrix3x4_t invGripSampledHand;
						MatrixInvert(gripSampledBones[leftHandBone], invGripSampledHand);
						
						matrix3x4_t gripAnchorDelta;
						ConcatTransforms(gripTargetTransform, invGripSampledHand, gripAnchorDelta);
						
						// Build the grip pose in world space
						matrix3x4_t gripWorldBones[MAXSTUDIOBONES];
						for (int i = 0; i < numBones; i++)
						{
							ConcatTransforms(gripAnchorDelta, gripSampledBones[i], gripWorldBones[i]);
						}
						
						// Now blend from current pose (pBoneToWorldOut) to grip pose (gripWorldBones)
						for (int i = 0; i < numBones && i < nMaxBones; i++)
						{
							Vector gripPos, currentPos, blendedPos;
							Quaternion gripQuat, currentQuat, blendedQuat;
							
							MatrixAngles(gripWorldBones[i], gripQuat, gripPos);
							MatrixAngles(pBoneToWorldOut[i], currentQuat, currentPos);
							
							VectorLerp(currentPos, gripPos, m_flTwoHandBlend, blendedPos);
							SafeQuaternionSlerp(currentQuat, gripQuat, m_flTwoHandBlend, blendedQuat);
							
							QuaternionMatrix(blendedQuat, blendedPos, pBoneToWorldOut[i]);
						}
						
						if (tfvr_twohand_debug.GetBool())
						{
							static float lastDebugTime = 0;
							if (gpGlobals->curtime - lastDebugTime > 0.5f)
							{
								DevMsg("TwoHand: Using left hand pose seq=%d cycle=%.2f, blend=%.2f\n", 
									rightSeq, rightCycle, m_flTwoHandBlend);
								lastDebugTime = gpGlobals->curtime;
							}
						}
					}
				}
			}
			
			// If we didn't get a grip pose, use finger tracking
			if (!bUsedGripPose)
			{
				ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
			}
		}
		else if (IsLeftHand() && m_hLeftHandBall.Get() && m_iLastBallAmmo > 0)
		{
			// Left hand is holding a ball - apply wb_idle pose
			// Sample the animation and build the full skeleton like we do for the base pose,
			// but using the wb_idle animation instead of the reference pose
			int iGripSeq = LookupSequence("wb_idle");
			if (iGripSeq >= 0)
			{
				// Sample the grip animation
				float poseParams[MAXSTUDIOPOSEPARAM];
				memset(poseParams, 0, sizeof(poseParams));
				
				IBoneSetup gripBoneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParams);
				
				Vector gripPos[MAXSTUDIOBONES];
				Quaternion gripQ[MAXSTUDIOBONES];
				for (int i = 0; i < MAXSTUDIOBONES; i++)
				{
					gripPos[i].Init();
					gripQ[i].Init(0, 0, 0, 1);
				}
				gripBoneSetup.InitPose(gripPos, gripQ);
				gripBoneSetup.AccumulatePose(gripPos, gripQ, iGripSeq, 0.0f, 1.0f, gpGlobals->curtime, NULL);
				
				// Build the grip pose skeleton in model space (same as we do for sampledBones earlier)
				matrix3x4_t gripBones[MAXSTUDIOBONES];
				for (int i = 0; i < numBones; i++)
				{
					matrix3x4_t boneToParent;
					QuaternionMatrix(gripQ[i], gripPos[i], boneToParent);
					
					const mstudiobone_t *pBone = pStudioHdr->pBone(i);
					if (!pBone)
					{
						SetIdentityMatrix(gripBones[i]);
						continue;
					}
					
					if (pBone->parent == -1)
						MatrixCopy(boneToParent, gripBones[i]);
					else if (pBone->parent >= 0 && pBone->parent < numBones)
						ConcatTransforms(gripBones[pBone->parent], boneToParent, gripBones[i]);
					else
						SetIdentityMatrix(gripBones[i]);
				}
				
				// Calculate anchor delta for this grip pose
				// Same as we do for the base pose: anchorDelta = controller * inverse(handBone)
				if (m_iHandBone >= 0 && m_iHandBone < numBones)
				{
					matrix3x4_t invGripHandBone;
					MatrixInvert(gripBones[m_iHandBone], invGripHandBone);
					
					matrix3x4_t gripAnchorDelta;
					ConcatTransforms(controllerTransform, invGripHandBone, gripAnchorDelta);
					
					// Transform grip skeleton to world space
					for (int i = 0; i < numBones && i < nMaxBones; i++)
					{
						ConcatTransforms(gripAnchorDelta, gripBones[i], pBoneToWorldOut[i]);
					}
					
					// Position the ball directly from the weapon_bone_L bone
					// Do it here in SetupBones to avoid lag from Update() timing
					C_BaseAnimating *pBall = m_hLeftHandBall.Get();
					if (pBall && m_iLastBallAmmo > 0)
					{
						int iWeaponBoneL = LookupBone("weapon_bone_L");
						if (iWeaponBoneL >= 0 && iWeaponBoneL < nMaxBones)
						{
							Vector ballPos;
							QAngle ballAng;
							MatrixAngles(pBoneToWorldOut[iWeaponBoneL], ballAng, ballPos);
							pBall->SetAbsOrigin(ballPos);
							pBall->SetAbsAngles(ballAng);
						}
					}
				}
			}
			else
			{
				// Fallback to finger tracking
				ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
			}
		}
		else if (IsLeftHand())
		{
			C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
			C_TFWeaponBase *pRightWeapon = pRightHand ? pRightHand->GetHeldWeapon() : NULL;
			if (pRightWeapon && pRightWeapon->GetWeaponID() == TF_WEAPON_FISTS)
			{
				ApplyWeaponPose(pBoneToWorldOut, nMaxBones, pRightWeapon);
			}
			else
			{
				ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
			}
		}
		else
		{
			ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
		}
		
		// Position offset was already applied to controllerTransform at the beginning,
		// so all bones and weapon are naturally at the offset position.
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
// Purpose: Get the off-hand grip target position from the weapon hand's animation
//          This returns the position where the off-hand should go when two-handing
//          Works bidirectionally:
//          - Called on RIGHT hand: returns LEFT hand grip target (bip_hand_L)
//          - Called on LEFT hand: returns RIGHT hand grip target (bip_hand_R)
//          bUseCurrentAnimation: false = sample idle (for stable weapon rotation)
//                                true = sample current animation (for visual positioning)
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetOffHandGripTarget(Vector &outPos, QAngle &outAngles, bool bUseCurrentAnimation)
{
	// Need a held weapon for two-handing
	if (!m_hHeldWeapon.Get())
	{
		if (tfvr_twohand_debug.GetBool() && IsLeftHand())
		{
			static float lastDebugTime = 0;
			if (gpGlobals->curtime - lastDebugTime > 2.0f)
			{
				Msg("GetOffHandGripTarget: LEFT hand has no held weapon!\n");
				lastDebugTime = gpGlobals->curtime;
			}
		}
		return false;
	}
	
	CStudioHdr *pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
	{
		if (tfvr_twohand_debug.GetBool() && IsLeftHand())
		{
			static float lastDebugTime = 0;
			if (gpGlobals->curtime - lastDebugTime > 2.0f)
			{
				Msg("GetOffHandGripTarget: LEFT hand has no model ptr!\n");
				lastDebugTime = gpGlobals->curtime;
			}
		}
		return false;
	}
		
	UpdateHandTransform();
	
	// Look up the off-hand bones based on which hand we are
	// Right hand looks for left-hand bones, left hand looks for right-hand bones
	if (m_iOffHandBone < 0)
	{
		if (IsRightHand())
		{
			// Right hand provides left-hand grip target
			m_iOffHandBone = LookupBone("bip_hand_L");
			if (m_iOffHandBone < 0)
				m_iOffHandBone = LookupBone("ValveBiped.Bip01_L_Hand");
			if (m_iOffHandBone < 0)
				m_iOffHandBone = LookupBone("bip_hand_l");
			if (m_iOffHandBone < 0)
				m_iOffHandBone = LookupBone("weapon_bone_L");
			
			// Also look up middle finger base for bind pose offset calculation
			m_iOffHandMiddleFingerBone = LookupBone("bip_middle_0_L");
		}
		else
		{
			// Left hand provides right-hand grip target (for medigun)
			// The LEFT HAND MODEL has bip_hand_R bones for the grip pose
			// We need to look on THIS hand's model, not the weapon
			m_iOffHandBone = LookupBone("bip_hand_R");
			if (m_iOffHandBone < 0)
				m_iOffHandBone = LookupBone("ValveBiped.Bip01_R_Hand");
			if (m_iOffHandBone < 0)
				m_iOffHandBone = LookupBone("bip_hand_r");
			if (m_iOffHandBone < 0)
				m_iOffHandBone = LookupBone("weapon_bone_R");
			
			if (tfvr_twohand_debug.GetBool())
			{
				static float lastDebugTime = 0;
				if (gpGlobals->curtime - lastDebugTime > 2.0f)
				{
					DevMsg("TwoHand Medigun: Looking for bip_hand_R on left hand model, found bone %d\n", m_iOffHandBone);
					lastDebugTime = gpGlobals->curtime;
				}
			}
			
			// Also look up middle finger base for bind pose offset calculation
			m_iOffHandMiddleFingerBone = LookupBone("bip_middle_0_R");
		}
		
		if (m_iOffHandBone < 0)
		{
			if (tfvr_twohand_debug.GetBool())
			{
				static float lastWarnTime = 0;
				if (gpGlobals->curtime - lastWarnTime > 5.0f)
				{
					DevMsg("TwoHand: Could not find off-hand bone on weapon hand model\n");
					lastWarnTime = gpGlobals->curtime;
				}
			}
			return false;
		}
		
		if (tfvr_twohand_debug.GetBool() && IsLeftHand())
		{
			static float lastDebugTime = 0;
			if (gpGlobals->curtime - lastDebugTime > 2.0f)
			{
				Msg("GetOffHandGripTarget: LEFT hand bone lookup OK: m_iHandBone=%d, m_iOffHandBone=%d\n", 
					m_iHandBone, m_iOffHandBone);
				lastDebugTime = gpGlobals->curtime;
			}
		}
	}
	
	// We need to calculate the off-hand position using the same transform logic
	// that SetupBones uses for the right hand. The bone cache doesn't have our
	// VR transforms applied, so we need to:
	// 1. Sample the current animation to get the off-hand bone relative to the right hand
	// 2. Apply the current VR controller transform (anchor delta)
	
	if (m_iHandBone < 0)
	{
		if (tfvr_twohand_debug.GetBool() && IsLeftHand())
		{
			static float lastDebugTime = 0;
			if (gpGlobals->curtime - lastDebugTime > 2.0f)
			{
				Msg("GetOffHandGripTarget: LEFT hand m_iHandBone=%d (invalid!)\n", m_iHandBone);
				lastDebugTime = gpGlobals->curtime;
			}
		}
		return false;
	}
	
	int numBones = pStudioHdr->numbones();
	
	// Sample the current animation using the entity's pose parameters
	float poseParameters[MAXSTUDIOPOSEPARAM];
	for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
	{
		poseParameters[i] = GetPoseParameter(i);
	}
	
	IBoneSetup boneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters);
	
	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for (int i = 0; i < MAXSTUDIOBONES; i++)
	{
		posAnim[i].Init();
		qAnim[i].Init(0, 0, 0, 1);
	}
	boneSetup.InitPose(posAnim, qAnim);
	
	// Choose which animation to sample based on parameter:
	// - bUseCurrentAnimation=false: Use IDLE for stable weapon rotation (pivot axis calculation)
	// - bUseCurrentAnimation=true: Use current animation for visual positioning (follows recoil)
	int seqToSample;
	float cycleToSample;
	
	if (bUseCurrentAnimation && (m_bPlayingFireAnim || m_bPlayingChargeAnim))
	{
		// Sample current fire/charge animation - grip target will move with recoil/charge
		seqToSample = GetSequence();
		cycleToSample = GetCycle();
	}
	else
	{
		// Sample idle animation - grip target stays stable
		seqToSample = m_iIdleSequence >= 0 ? m_iIdleSequence : GetSequence();
		cycleToSample = 0.0f;
	}
	boneSetup.AccumulatePose(posAnim, qAnim, seqToSample, cycleToSample, 1.0f, gpGlobals->curtime, NULL);
	
	// Apply flamethrower blend to offhand grip target so it eases in/out with the fire anim
	if (bUseCurrentAnimation && m_bPlayingFireAnim
		&& m_flFlamethrowerFireBlend > 0.0f && m_flFlamethrowerFireBlend < 1.0f
		&& IsWeaponFlamethrower(m_hHeldWeapon.Get()))
	{
		float flBlend = SimpleSpline(m_flFlamethrowerFireBlend);

		Vector posIdle[MAXSTUDIOBONES];
		Quaternion qIdle[MAXSTUDIOBONES];
		for (int i = 0; i < numBones; i++)
		{
			posIdle[i].Init();
			qIdle[i].Init(0, 0, 0, 1);
		}
		boneSetup.InitPose(posIdle, qIdle);
		if (m_iIdleSequence >= 0)
			boneSetup.AccumulatePose(posIdle, qIdle, m_iIdleSequence, 0.0f, 1.0f, gpGlobals->curtime, NULL);

		for (int i = 0; i < numBones; i++)
		{
			VectorLerp(posIdle[i], posAnim[i], flBlend, posAnim[i]);
			QuaternionSlerp(qIdle[i], qAnim[i], flBlend, qAnim[i]);
		}
	}

	if (tfvr_twohand_debug.GetBool() && IsLeftHand())
	{
		static float lastDebugTime = 0;
		if (gpGlobals->curtime - lastDebugTime > 2.0f)
		{
			Msg("TwoHand: Left hand sampling seq %d (idle=%d), m_iHandBone=%d, m_iOffHandBone=%d\n", 
				seqToSample, m_iIdleSequence, m_iHandBone, m_iOffHandBone);
			lastDebugTime = gpGlobals->curtime;
		}
	}
	
	// Build bone transforms from sampled animation (local to parent)
	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for (int i = 0; i < numBones; i++)
	{
		matrix3x4_t boneToParent;
		QuaternionMatrix(qAnim[i], posAnim[i], boneToParent);
		
		const mstudiobone_t *pBone = pStudioHdr->pBone(i);
		if (!pBone)
		{
			SetIdentityMatrix(sampledBones[i]);
			continue;
		}
		
		if (pBone->parent == -1)
			MatrixCopy(boneToParent, sampledBones[i]);
		else if (pBone->parent >= 0 && pBone->parent < numBones)
			ConcatTransforms(sampledBones[pBone->parent], boneToParent, sampledBones[i]);
		else
			SetIdentityMatrix(sampledBones[i]);
	}
	
	// Debug: show bip_hand_R position in model space (before VR transform)
	if (tfvr_twohand_debug.GetBool() && IsLeftHand() && m_iOffHandBone >= 0)
	{
		static float lastDebugTime = 0;
		if (gpGlobals->curtime - lastDebugTime > 2.0f)
		{
			Vector offHandModelPos;
			QAngle offHandModelAngles;
			MatrixAngles(sampledBones[m_iOffHandBone], offHandModelAngles, offHandModelPos);
			
			Vector handBoneModelPos;
			QAngle handBoneModelAngles;
			MatrixAngles(sampledBones[m_iHandBone], handBoneModelAngles, handBoneModelPos);
			
			Msg("TwoHand ModelSpace: bip_hand_L=(%.1f,%.1f,%.1f), bip_hand_R=(%.1f,%.1f,%.1f)\n",
				handBoneModelPos.x, handBoneModelPos.y, handBoneModelPos.z,
				offHandModelPos.x, offHandModelPos.y, offHandModelPos.z);
			lastDebugTime = gpGlobals->curtime;
		}
	}
	
	// Get the current VR controller transform (where the right hand actually is)
	matrix3x4_t controllerTransform;
	AngleMatrix(m_angLastValidAngles, m_vecLastValidPosition, controllerTransform);
	
	// Apply hand rotation offsets (must match SetupBones logic exactly)
	// Uses per-class offsets when enabled, falls back to global offsets
	// Use THIS hand's offsets (not the target grip hand)
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	int ownerClass = pOwner ? pOwner->GetPlayerClass()->GetClassIndex() : TF_CLASS_UNDEFINED;
	bool bIsLeftHand = IsLeftHand();
	
	QAngle rotOffset(0, 0, 0);
	if (!GetPerClassHandOffset(ownerClass, bIsLeftHand, rotOffset))
	{
		if (bIsLeftHand)
		{
			rotOffset.x = tfvr_hands_left_offset_pitch.GetFloat();
			rotOffset.y = tfvr_hands_left_offset_yaw.GetFloat();
			rotOffset.z = tfvr_hands_left_offset_roll.GetFloat();
		}
		else
		{
			rotOffset.x = tfvr_hands_right_offset_pitch.GetFloat();
			rotOffset.y = tfvr_hands_right_offset_yaw.GetFloat();
			rotOffset.z = tfvr_hands_right_offset_roll.GetFloat();
		}
	}
	
	if (rotOffset.x != 0 || rotOffset.y != 0 || rotOffset.z != 0)
	{
		matrix3x4_t offsetMatrix;
		AngleMatrix(rotOffset, vec3_origin, offsetMatrix);
		
		matrix3x4_t temp;
		ConcatTransforms(controllerTransform, offsetMatrix, temp);
		MatrixCopy(temp, controllerTransform);
	}
	
	// Apply hand position offsets (same as in SetupBones)
	Vector posOffset(0, 0, 0);
	if (!GetPerClassHandPositionOffset(ownerClass, bIsLeftHand, posOffset))
	{
		if (bIsLeftHand)
		{
			posOffset.x = tfvr_hands_left_offset_x.GetFloat();
			posOffset.y = tfvr_hands_left_offset_y.GetFloat();
			posOffset.z = tfvr_hands_left_offset_z.GetFloat();
		}
		else
		{
			posOffset.x = tfvr_hands_right_offset_x.GetFloat();
			posOffset.y = tfvr_hands_right_offset_y.GetFloat();
			posOffset.z = tfvr_hands_right_offset_z.GetFloat();
		}
	}
	
	if (posOffset.x != 0 || posOffset.y != 0 || posOffset.z != 0)
	{
		Vector palmX, palmY, palmZ;
		MatrixGetColumn(controllerTransform, 0, palmX);
		MatrixGetColumn(controllerTransform, 1, palmY);
		MatrixGetColumn(controllerTransform, 2, palmZ);
		
		Vector worldOffset = palmX * posOffset.x + palmY * posOffset.y + palmZ * posOffset.z;
		
		Vector ctrlPos;
		MatrixGetColumn(controllerTransform, 3, ctrlPos);
		ctrlPos += worldOffset;
		MatrixSetColumn(ctrlPos, 3, controllerTransform);
	}
	
	// Apply offhand grip rotation (must match SetupBones for consistency)
	C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
	float rotationBlend = pLeftHand ? pLeftHand->GetGripRotationBlend() : 0.0f;
	bool bWasGripActive = pLeftHand && pLeftHand->WasOffhandGripActive();
	bool bIsGripActive = pLeftHand && pLeftHand->IsOffhandGripActive();
	
	if (rotationBlend > 0.001f && bWasGripActive && tfvr_offhand_grip_enabled.GetBool())
	{
		Vector desiredY = pLeftHand->GetOffhandGripForward();
		
		if (desiredY.LengthSqr() >= 0.1f)
		{
			Quaternion preGripQuat;
			Vector preGripPos;
			MatrixAngles(controllerTransform, preGripQuat, preGripPos);
			
			ApplyTwoHandGripRotation(controllerTransform, desiredY);
			
			Quaternion gripQuat;
			Vector gripPos;
			MatrixAngles(controllerTransform, gripQuat, gripPos);
			
			float easePower = tfvr_offhand_grip_ease_power.GetFloat();
			float easedRotBlend = ApplyEaseOutToBlend(rotationBlend, easePower, bIsGripActive);
			
			Quaternion blendedQuat;
			SafeQuaternionSlerp(preGripQuat, gripQuat, easedRotBlend, blendedQuat);
			QuaternionMatrix(blendedQuat, preGripPos, controllerTransform);
		}
	}
	
	// Use cached idle hand bone for anchor (standard path)
	matrix3x4_t anchorDelta;
	if (m_bHandBoneOffsetValid)
	{
		matrix3x4_t invIdleHandBone;
		MatrixInvert(m_matIdleHandBoneTransform, invIdleHandBone);
		ConcatTransforms(controllerTransform, invIdleHandBone, anchorDelta);
	}
	else
	{
		matrix3x4_t invSampledHandBone;
		MatrixInvert(sampledBones[m_iHandBone], invSampledHandBone);
		ConcatTransforms(controllerTransform, invSampledHandBone, anchorDelta);
	}
	
	// Transform the sampled off-hand bone (bip_hand_L) to world space using the anchor delta
	// This bone position is correct even though its children are broken in animation data
	matrix3x4_t offHandWorld;
	ConcatTransforms(anchorDelta, sampledBones[m_iOffHandBone], offHandWorld);
	
	// Only apply middle finger offset for PIVOT calculation (bUseCurrentAnimation = false)
	// For VISUAL hand positioning (bUseCurrentAnimation = true), use bip_hand_L directly
	// so the hand wrist attaches to the correct point on the weapon
	bool bAppliedFingerOffset = false;
	if (!bUseCurrentAnimation && pLeftHand)
	{
		// Calculate where middle finger base SHOULD be using bind pose from the LEFT hand model
		// The right hand model's skeleton may have broken bind pose data for left hand bones,
		// so we sample the offset from the left hand model which has correct left hand skeleton
		CStudioHdr *pLeftStudioHdr = pLeftHand->GetModelPtr();
		if (pLeftStudioHdr)
		{
			// Look up the equivalent bones on the left hand model
			// Left hand uses _L bones as its primary bones
			int leftHandBone = pLeftHand->LookupBone("bip_hand_L");
			int leftMiddleFingerBone = pLeftHand->LookupBone("bip_middle_0_L");
			
			if (leftHandBone >= 0 && leftMiddleFingerBone >= 0 && 
				leftMiddleFingerBone < pLeftStudioHdr->numbones())
			{
				const mstudiobone_t *pMiddleFingerBone = pLeftStudioHdr->pBone(leftMiddleFingerBone);
				if (pMiddleFingerBone)
				{
					// Build the full local transform from bip_hand_L to bip_middle_0_L
					// by walking up the bone hierarchy and accumulating transforms
					matrix3x4_t bindPoseLocal;
					SetIdentityMatrix(bindPoseLocal);
					
					int currentBone = leftMiddleFingerBone;
					int maxIterations = 10; // Safety limit
					
					while (currentBone != leftHandBone && currentBone >= 0 && maxIterations-- > 0)
					{
						const mstudiobone_t *pCurrentBone = pLeftStudioHdr->pBone(currentBone);
						if (!pCurrentBone)
							break;
						
						// Get this bone's local transform
						matrix3x4_t boneLocal;
						QuaternionMatrix(pCurrentBone->quat, pCurrentBone->pos, boneLocal);
						
						// Prepend to accumulated transform (child * accumulated = new accumulated)
						matrix3x4_t temp;
						ConcatTransforms(boneLocal, bindPoseLocal, temp);
						MatrixCopy(temp, bindPoseLocal);
						
						currentBone = pCurrentBone->parent;
					}
					
					// Only apply if we successfully traced back to the hand bone
					if (currentBone == leftHandBone)
					{
						ConcatTransforms(offHandWorld, bindPoseLocal, offHandWorld);
						bAppliedFingerOffset = true;
						
						if (tfvr_twohand_debug.GetBool())
						{
							static float lastBoneDebugTime = 0;
							if (gpGlobals->curtime - lastBoneDebugTime > 2.0f)
							{
								DevMsg("TwoHand: Applied finger offset (hand=%d -> middle=%d)\n", 
									leftHandBone, leftMiddleFingerBone);
								lastBoneDebugTime = gpGlobals->curtime;
							}
						}
					}
					else if (tfvr_twohand_debug.GetBool())
					{
						static float lastWarnTime = 0;
						if (gpGlobals->curtime - lastWarnTime > 2.0f)
						{
							DevMsg("TwoHand: WARN - Could not trace middle finger (%d) back to hand (%d), stopped at %d\n", 
								leftMiddleFingerBone, leftHandBone, currentBone);
							lastWarnTime = gpGlobals->curtime;
						}
					}
				}
			}
		}
	}
	
	// Extract position and angles
	MatrixGetColumn(offHandWorld, 3, outPos);
	MatrixAngles(offHandWorld, outAngles);
	
	// Position offset is now applied to controllerTransform early (same as SetupBones),
	// so the grip target automatically includes the offset. No extra adjustment needed here.
	
	if (tfvr_twohand_debug.GetBool())
	{
		static float lastDebugTime = 0;
		if (gpGlobals->curtime - lastDebugTime > 0.5f)
		{
			const char *mode = bUseCurrentAnimation ? "hand bone (visual)" : 
				(bAppliedFingerOffset ? "MIDDLE FINGER (pivot)" : "hand bone (pivot FALLBACK)");
			DevMsg("TwoHand: Off-hand grip target at (%.1f, %.1f, %.1f) [%s]\n", 
				outPos.x, outPos.y, outPos.z, mode);
			lastDebugTime = gpGlobals->curtime;
		}
		
		// Draw debug box at grip target (GREEN = middle finger base target)
		Vector boxMins(-2, -2, -2);
		Vector boxMaxs(2, 2, 2);
		debugoverlay->AddBoxOverlay(outPos, boxMins, boxMaxs, vec3_angle, 0, 255, 0, 128, 0.1f);
	}
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Position weapon using bone matrices from SetupBones
//          Called during SetupBones after pose is applied to weapon_bone
//-----------------------------------------------------------------------------
void C_TFVRHand::PositionWeaponFromBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones)
{
	if (!pBoneToWorldOut)
		return;

	// Always cache the hand's weapon_bone transform for overlays (HUD compositor etc.),
	// even when there is no render weapon (e.g. bare fists).
	int handWeaponBone = -1;
	if (IsLeftHand() && IsWeaponMedigun(m_hHeldWeapon.Get()))
	{
		handWeaponBone = LookupBone("weapon_bone_L");
	}
	if (handWeaponBone < 0)
	{
		handWeaponBone = LookupBone("weapon_bone");
	}

	if (handWeaponBone >= 0 && handWeaponBone < nMaxBones)
	{
		MatrixCopy(pBoneToWorldOut[handWeaponBone], m_matWeaponBoneWorld);
		m_bWeaponBoneWorldValid = true;
	}

	// Position the RENDER weapon based on hand's weapon_bone
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	if (!pRenderWeapon)
		return;

	Vector weaponPos;
	QAngle weaponAng;

	C_TFWeaponBase *pAlignWeapon = m_hHeldWeapon.Get();
	bool bIsFistWeapon = pAlignWeapon && pAlignWeapon->GetWeaponID() == TF_WEAPON_FISTS;
	int weaponWeaponBone = -1;

	if (handWeaponBone >= 0 && handWeaponBone < nMaxBones)
	{
		matrix3x4_t handWeaponBoneMatrix;
		MatrixCopy(pBoneToWorldOut[handWeaponBone], handWeaponBoneMatrix);
		
		// Extract position and angles
		Vector bonePos;
		QAngle boneAng;
		MatrixAngles(handWeaponBoneMatrix, boneAng, bonePos);
		
		// Determine which bone on the weapon model to align to the hand's weapon_bone.
		if (IsLeftHand() && IsWeaponMedigun(pAlignWeapon))
		{
			weaponWeaponBone = pRenderWeapon->LookupBone("weapon_bone_L");
		}

		if (weaponWeaponBone < 0)
		{
			weaponWeaponBone = pRenderWeapon->LookupBone("weapon_bone");
		}
		
		if (tfvr_debug_weapon_position.GetBool())
		{
			C_TFWeaponBase *pDebugWeapon = m_hHeldWeapon.Get();
			Msg("Weapon: %s, weapon_bone index: %d, hand weapon_bone: %d, IsLeftHand: %d\n", 
				pDebugWeapon ? pDebugWeapon->GetClassname() : "null",
				weaponWeaponBone,
				handWeaponBone,
				IsLeftHand() ? 1 : 0);
			Msg("Hand anim sequence: %d, Hand model: %s\n",
				GetSequence(), 
				GetModelName() ? GetModelName() : "null");
		}
		
		if (weaponWeaponBone >= 0)
		{
			// Get the weapon's weapon_bone transform in MODEL SPACE (bind pose)
			// We need to walk up the bone hierarchy to get the full transform
			CStudioHdr *pWeaponHdr = pRenderWeapon->GetModelPtr();
			if (pWeaponHdr)
			{
				// Build the full model-space transform by walking up the bone hierarchy
				matrix3x4_t weaponBoneModelSpace;
				SetIdentityMatrix(weaponBoneModelSpace);
				
				int currentBone = weaponWeaponBone;
				int boneCount = 0;
				while (currentBone >= 0)
				{
					mstudiobone_t *pBone = pWeaponHdr->pBone(currentBone);
					if (!pBone)
						break;
					
					if (tfvr_debug_weapon_position.GetBool())
					{
						QAngle debugAng;
						QuaternionAngles(pBone->quat, debugAng);
						Msg("  Bone[%d] '%s': pos=(%.1f, %.1f, %.1f) ang=(%.1f, %.1f, %.1f) parent=%d\n",
							currentBone, pBone->pszName(),
							pBone->pos.x, pBone->pos.y, pBone->pos.z,
							debugAng.x, debugAng.y, debugAng.z,
							pBone->parent);
					}
					
					// Get this bone's local transform
					matrix3x4_t localBoneMatrix;
					QAngle localAng;
					QuaternionAngles(pBone->quat, localAng);
					AngleMatrix(localAng, pBone->pos, localBoneMatrix);
					
					// Prepend to the chain: modelSpace = local * child
					matrix3x4_t temp;
					ConcatTransforms(localBoneMatrix, weaponBoneModelSpace, temp);
					MatrixCopy(temp, weaponBoneModelSpace);
					
					// Move to parent
					currentBone = pBone->parent;
					boneCount++;
				}
				
				// Invert to get transform from weapon_bone space to weapon origin
				matrix3x4_t weaponBoneInverse;
				MatrixInvert(weaponBoneModelSpace, weaponBoneInverse);
				
				// Apply: weapon_origin = hand_weapon_bone * weapon_bone_inverse
				matrix3x4_t weaponTransform;
				ConcatTransforms(handWeaponBoneMatrix, weaponBoneInverse, weaponTransform);
				
				MatrixAngles(weaponTransform, weaponAng, weaponPos);
				
				if (tfvr_debug_weapon_position.GetBool())
				{
					Vector debugBonePos;
					QAngle debugBoneAng;
					MatrixAngles(weaponBoneModelSpace, debugBoneAng, debugBonePos);
					Msg("Final weapon_bone model-space (%d bones): pos=(%.1f, %.1f, %.1f) ang=(%.1f, %.1f, %.1f)\n",
						boneCount, debugBonePos.x, debugBonePos.y, debugBonePos.z,
						debugBoneAng.x, debugBoneAng.y, debugBoneAng.z);
					Msg("Hand weapon_bone world: pos=(%.1f, %.1f, %.1f) ang=(%.1f, %.1f, %.1f)\n",
						bonePos.x, bonePos.y, bonePos.z,
						boneAng.x, boneAng.y, boneAng.z);
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
			if (tfvr_debug_weapon_position.GetBool())
			{
				Msg("No weapon_bone found, using hand weapon_bone directly\n");
			}
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
	
	// Also set network origin so interpolation/particle systems see the same position
	pRenderWeapon->SetNetworkOrigin(weaponPos);
	
	// CRITICAL: Reset interpolation to prevent lag between hand and weapon
	// This ensures the weapon snaps to position immediately without lerping
	pRenderWeapon->ResetLatched();
	pRenderWeapon->InvalidateBoneCache();
	
	// Force weapon to setup bones so we can modify them
	pRenderWeapon->SetupBones(NULL, -1, BONE_USED_BY_ANYTHING, gpGlobals->curtime);

	// For fist/glove weapons, do a full bone merge like HLMV: copy ALL matching
	// bones from the VR hand models to the glove model. The glove has no
	// animation of its own - its bones are entirely driven by the arm model.
	// Right-hand bones come from this hand's pBoneToWorldOut, left-hand bones
	// come from the left VR hand.
	if (bIsFistWeapon)
	{
		CStudioHdr *pGloveHdr = pRenderWeapon->GetModelPtr();
		if (pGloveHdr)
		{
			int boneCount = pGloveHdr->numbones();

			// Get left VR hand bones (must pass real buffer for VR positioning)
			C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
			matrix3x4_t leftHandBones[MAXSTUDIOBONES];
			bool bLeftValid = false;
			if (pLeftHand)
				bLeftValid = pLeftHand->SetupBones(leftHandBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);

			// Identify which glove bones belong to the left hierarchy
			// so we route them to the left VR hand, not the right.
			int gloveBipL = pRenderWeapon->LookupBone("bip_hand_L");
			bool bIsLeftSide[MAXSTUDIOBONES];
			memset(bIsLeftSide, 0, sizeof(bIsLeftSide));
			if (gloveBipL >= 0)
			{
				bIsLeftSide[gloveBipL] = true;
				for (int i = 0; i < boneCount && i < MAXSTUDIOBONES; i++)
				{
					const mstudiobone_t *pBone = pGloveHdr->pBone(i);
					if (pBone && pBone->parent >= 0 && bIsLeftSide[pBone->parent])
						bIsLeftSide[i] = true;
				}
			}

			// Save originals and track which bones get overwritten
			bool bCopied[MAXSTUDIOBONES];
			memset(bCopied, 0, sizeof(bCopied));
			matrix3x4_t origBones[MAXSTUDIOBONES];

			// First pass: copy matching bones from the correct VR hand
			for (int i = 0; i < boneCount && i < MAXSTUDIOBONES; i++)
			{
				const mstudiobone_t *pBone = pGloveHdr->pBone(i);
				if (!pBone) continue;
				const char *szName = pBone->pszName();

				MatrixCopy(pRenderWeapon->GetBone(i), origBones[i]);

				if (bIsLeftSide[i])
				{
					// Left-side bone: look up on left VR hand only.
					// Try exact name first, then strip _L suffix since the left
					// hand model uses unsuffixed names (e.g. vm_weapon_bone).
					if (!bLeftValid || !pLeftHand)
						continue;

					int vrBone = pLeftHand->LookupBone(szName);
					if (vrBone < 0)
					{
						int len = Q_strlen(szName);
						if (len > 2 && szName[len-2] == '_' && szName[len-1] == 'L')
						{
							char strippedName[128];
							V_strncpy(strippedName, szName, sizeof(strippedName));
							strippedName[len-2] = '\0';
							vrBone = pLeftHand->LookupBone(strippedName);
						}
					}

					if (vrBone >= 0 && vrBone < MAXSTUDIOBONES)
					{
						MatrixCopy(leftHandBones[vrBone], pRenderWeapon->GetBoneForWrite(i));
						bCopied[i] = true;
					}
				}
				else
				{
					// Right-side bone: look up on right VR hand
					int vrBone = LookupBone(szName);
					if (vrBone >= 0 && vrBone < nMaxBones)
					{
						MatrixCopy(pBoneToWorldOut[vrBone], pRenderWeapon->GetBoneForWrite(i));
						bCopied[i] = true;
					}
				}
			}

			// Second pass: rebuild non-copied children of copied bones
			// using their local transforms from the original animation
			for (int i = 0; i < boneCount && i < MAXSTUDIOBONES; i++)
			{
				if (bCopied[i]) continue;
				const mstudiobone_t *pBone = pGloveHdr->pBone(i);
				if (!pBone || pBone->parent < 0 || !bCopied[pBone->parent])
					continue;

				matrix3x4_t invOrigParent;
				MatrixInvert(origBones[pBone->parent], invOrigParent);

				matrix3x4_t localTransform;
				ConcatTransforms(invOrigParent, origBones[i], localTransform);

				matrix3x4_t &newParent = pRenderWeapon->GetBoneForWrite(pBone->parent);
				matrix3x4_t &childBone = pRenderWeapon->GetBoneForWrite(i);
				ConcatTransforms(newParent, localTransform, childBone);

				bCopied[i] = true;
			}
		}
	}
	
	// Now copy animated bone transforms from hand to weapon.
	// Skip for fist/glove weapons - these are rigid models positioned via
	// vm_weapon_bone alignment. Merging vm_weapon_bone chain transforms from
	// the VR hand model would distort the glove mesh.
	C_TFWeaponBase *pMergeWeapon = m_hHeldWeapon.Get();
	bool bSkipBoneMerge = pMergeWeapon && pMergeWeapon->GetWeaponID() == TF_WEAPON_FISTS;

	CStudioHdr *pWeaponHdr = pRenderWeapon->GetModelPtr();
	if (pWeaponHdr && pBoneToWorldOut && !bSkipBoneMerge)
	{
		extern ConVar tfvr_weapon_fire_anim_debug;
		static int lastCopiedCount = -1;
		int copiedCount = 0;
		
		// Bone-merge: copy ALL bones that exist in both the hand model and weapon model.
		// This covers vm_weapon_bone_*, weapon_bone_*, lever, and any other shared bones
		// that the hand animation drives on the weapon.
		int weaponBoneCount = pWeaponHdr->numbones();
		
		// Identify which bones will be overwritten and save their original transforms
		bool bCopiedBone[MAXSTUDIOBONES];
		memset(bCopiedBone, 0, sizeof(bCopiedBone));
		matrix3x4_t originalWeaponBones[MAXSTUDIOBONES];
		
		for (int i = 0; i < weaponBoneCount && i < MAXSTUDIOBONES; i++)
		{
			const mstudiobone_t *pWpnBone = pWeaponHdr->pBone(i);
			if (!pWpnBone)
				continue;
			
			const char *szBoneName = pWpnBone->pszName();
			
			// Skip weapon_bone and weapon_bone_L/R - used for weapon positioning
			if (Q_strcmp(szBoneName, "weapon_bone") == 0 ||
				Q_strcmp(szBoneName, "weapon_bone_L") == 0 ||
				Q_strcmp(szBoneName, "weapon_bone_R") == 0)
				continue;

			// For weapons with animated idles (e.g. Mutated Milk bread creature),
			// don't overwrite vm_weapon bones - let the weapon model's own
			// animation drive them so the creature visually animates.
			if (m_bAnimateIdle && Q_strncmp(szBoneName, "vm_weapon", 9) == 0)
				continue;
			
			// Skip hand/arm bones - these exist in c_models but should NOT be
			// merged from the hand model. The weapon model's own hand bones
			// control handle mesh positioning for the grip system.
			if (Q_strncmp(szBoneName, "bip_hand", 8) == 0 ||
				Q_strncmp(szBoneName, "bip_thumb", 9) == 0 ||
				Q_strncmp(szBoneName, "bip_index", 9) == 0 ||
				Q_strncmp(szBoneName, "bip_middle", 10) == 0 ||
				Q_strncmp(szBoneName, "bip_ring", 8) == 0 ||
				Q_strncmp(szBoneName, "bip_pinky", 9) == 0 ||
				Q_strncmp(szBoneName, "ValveBiped", 10) == 0 ||
				Q_strncmp(szBoneName, "bip_wrst", 8) == 0 ||
				Q_strncmp(szBoneName, "bip_elbow", 9) == 0 ||
				Q_strncmp(szBoneName, "bip_bicep", 9) == 0 ||
				Q_strncmp(szBoneName, "bip_shoulder", 12) == 0)
				continue;
			
			int handBoneIndex = LookupBone(szBoneName);
			if (handBoneIndex >= 0 && handBoneIndex < nMaxBones)
			{
				// Save original weapon transform before overwriting
				MatrixCopy(pRenderWeapon->GetBoneForWrite(i), originalWeaponBones[i]);
				bCopiedBone[i] = true;
			}
		}
		
		// First pass: copy shared bones from hand to weapon
		for (int i = 0; i < weaponBoneCount && i < MAXSTUDIOBONES; i++)
		{
			if (!bCopiedBone[i])
				continue;
			
			const mstudiobone_t *pWpnBone = pWeaponHdr->pBone(i);
			const char *szBoneName = pWpnBone->pszName();
			int handBoneIndex = LookupBone(szBoneName);
			
			matrix3x4_t &weaponBone = pRenderWeapon->GetBoneForWrite(i);
			MatrixCopy(pBoneToWorldOut[handBoneIndex], weaponBone);
			copiedCount++;
		}
		
		// Second pass: rebuild children of copied bones that weren't themselves copied.
		// Extract the child's local transform from the ORIGINAL weapon animation, then
		// recompute it relative to the new (hand-driven) parent transform.
		for (int i = 0; i < weaponBoneCount && i < MAXSTUDIOBONES; i++)
		{
			if (bCopiedBone[i])
				continue;
			
			const mstudiobone_t *pWpnBone = pWeaponHdr->pBone(i);
			if (!pWpnBone || pWpnBone->parent < 0)
				continue;
			
			if (!bCopiedBone[pWpnBone->parent])
				continue;
			
			// local = inv(original_parent) * original_child
			matrix3x4_t invOriginalParent;
			MatrixInvert(originalWeaponBones[pWpnBone->parent], invOriginalParent);
			
			matrix3x4_t localTransform;
			matrix3x4_t &childBone = pRenderWeapon->GetBoneForWrite(i);
			ConcatTransforms(invOriginalParent, childBone, localTransform);
			
			// new_child = new_parent * local
			matrix3x4_t &newParentBone = pRenderWeapon->GetBoneForWrite(pWpnBone->parent);
			ConcatTransforms(newParentBone, localTransform, childBone);
		}
		
		if (tfvr_weapon_fire_anim_debug.GetBool() && copiedCount != lastCopiedCount)
		{
			DevMsg("VR Weapon: Copied %d animated bone transforms from hand to weapon\n", copiedCount);
			lastCopiedCount = copiedCount;
		}
	}
	
	// Cache the muzzle position NOW while bones are fresh
	// This will be used by tracers/effects that query muzzle position later
	m_bCachedMuzzleValid = false;
	int iMuzzle = pRenderWeapon->LookupAttachment("muzzle");
	if (iMuzzle > 0)
	{
		if (pRenderWeapon->GetAttachment(iMuzzle, m_vecCachedMuzzlePos, m_angCachedMuzzleAngles))
		{
			m_bCachedMuzzleValid = true;
		}
	}
	
}

//-----------------------------------------------------------------------------
// Purpose: Get cached weapon bone world transform (for overlays to avoid bone cache issues)
//          This is set during PositionWeaponFromBones and reflects the current frame's position
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetCachedWeaponBoneTransform(matrix3x4_t &outTransform) const
{
	if (!m_bWeaponBoneWorldValid)
		return false;
	
	MatrixCopy(m_matWeaponBoneWorld, outTransform);
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Get the weapon's muzzle position and angles in world space
//          Returns false if no weapon is held or muzzle can't be determined
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetWeaponMuzzlePositionAndAngles(Vector &outPos, QAngle &outAngles)
{
	// Use the RENDER weapon for position calculations
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	C_TFWeaponBase *pTFWeapon = m_hHeldWeapon.Get();
	if (!pRenderWeapon)
		return false;
	
	// CRITICAL: Get the absolute LATEST VR tracking data RIGHT NOW!
	// This ensures we have the most up-to-date hand position
	UpdateHandTransform();
	
	// DIRECT CONTROLLER MODE: Use controller pose directly with fixed offset
	// This bypasses all bone/attachment systems which may have timing issues
	// Toggle with tfvr_muzzle_direct_mode ConVar
	if (tfvr_muzzle_direct_mode.GetBool())
	{
		if (g_pOpenXRManager && g_pOpenXRManager->IsRightControllerPoseValid())
		{
			VMatrix controllerPose;
			if (g_pOpenXRManager->GetRightControllerPose(controllerPose))
			{
				// Get controller position and orientation
				outPos = controllerPose.GetTranslation();
				MatrixAngles(controllerPose.As3x4(), outAngles);
				
				// Apply a fixed forward offset to approximate muzzle position
				// The controller aim pose points forward, so we offset along that direction
				Vector forward, right, up;
				AngleVectors(outAngles, &forward, &right, &up);
				
				// Offset forward by ~30 units (approximate distance from grip to muzzle)
				// This can be tuned per-weapon if needed
				outPos += forward * 30.0f;
				
				return true;
			}
		}
		
		// Fallback to cached hand position if controller pose not available
		outPos = m_vecLastValidPosition;
		outAngles = m_angLastValidAngles;
		Vector forward;
		AngleVectors(outAngles, &forward, NULL, NULL);
		outPos += forward * 30.0f;
		return true;
	}
	
	// ATTACHMENT MODE (tfvr_muzzle_direct_mode 0): Use bone/attachment system
	
	// Check weapon type for special handling
	int weaponType = -1;
	if (pTFWeapon)
	{
		weaponType = pTFWeapon->GetTFWpnData().m_iWeaponType;
	}
	
	// MELEE WEAPONS: Use the weapon_bone transform for position and direction.
	// Most weapons align along the Y axis (green in HLMV / column 1), but
	// class-specific medic and sniper melees use the Z axis (blue / column 2).
	// All-class melees (frying pan, saxxy, etc.) always use Y.
	// tfvr_melee_bone_axis overrides auto-detection when >= 0.
	if (weaponType == TF_WPN_TYPE_MELEE || weaponType == TF_WPN_TYPE_MELEE_ALLCLASS)
	{
		static ConVarRef s_meleeAxis("tfvr_melee_bone_axis");

		matrix3x4_t matBone;
		if (GetCachedWeaponBoneTransform(matBone))
		{
			MatrixGetColumn(matBone, 3, outPos);

			int iAxis = 1;
			if ( s_meleeAxis.IsValid() && s_meleeAxis.GetInt() >= 0 )
			{
				iAxis = clamp( s_meleeAxis.GetInt(), 0, 2 );
			}
			else if ( weaponType == TF_WPN_TYPE_MELEE )
			{
				C_TFPlayer *pOwnerPlayer = m_hOwnerPlayer.Get();
				if ( pOwnerPlayer )
				{
					int iClass = pOwnerPlayer->GetPlayerClass()->GetClassIndex();
					if ( iClass == TF_CLASS_MEDIC || iClass == TF_CLASS_SNIPER )
						iAxis = 2;
				}
			}

			Vector vecDir;
			MatrixGetColumn(matBone, iAxis, vecDir);
			VectorAngles(vecDir, outAngles);
			return true;
		}
		// Fallback to cached hand position
		outPos = m_vecLastValidPosition;
		outAngles = m_angLastValidAngles;
		return true;
	}
	
	// PISTOL (Scout/Engineer): Use cached idle muzzle to prevent fire anim from moving aim
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (pOwner && pTFWeapon)
	{
		int playerClass = pOwner->GetPlayerClass()->GetClassIndex();
		int weaponID = pTFWeapon->GetWeaponID();
		
		// Check if this is a pistol for Scout or Engineer
		// Includes: stock pistol, scout pistol, Pretty Boy's Pocket Pistol, Winger
		bool bIsPistol = (weaponID == TF_WEAPON_PISTOL || 
		                  weaponID == TF_WEAPON_PISTOL_SCOUT || 
		                  weaponID == TF_WEAPON_HANDGUN_SCOUT_SECONDARY);
		bool bIsScoutOrEngineer = (playerClass == TF_CLASS_SCOUT || playerClass == TF_CLASS_ENGINEER);
		
		if (bIsPistol && bIsScoutOrEngineer)
		{
			// Invalidate cache if weapon changed
			if (m_iCachedMuzzleWeaponID != weaponID)
			{
				m_bIdleMuzzleOffsetValid = false;
				m_iCachedMuzzleWeaponID = weaponID;
			}
			
			// Cache idle muzzle when NOT playing fire animation
			if (!m_bIdleMuzzleOffsetValid && !m_bPlayingFireAnim)
			{
				// Update bones to get idle pose transforms
				matrix3x4_t boneArray[MAXSTUDIOBONES];
				SetupBones(boneArray, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
				pRenderWeapon->SetupBones(NULL, -1, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
				
				int iMuzzle = pRenderWeapon->LookupAttachment("muzzle");
				if (iMuzzle > 0)
				{
					Vector muzzlePos;
					QAngle muzzleAngles;
					pRenderWeapon->GetAttachment(iMuzzle, muzzlePos, muzzleAngles);
					
					// Cache muzzle position relative to the VR HAND entity (not weapon)
					// The hand entity position is stable (from controller tracking)
					matrix3x4_t handTransform;
					AngleMatrix(GetAbsAngles(), GetAbsOrigin(), handTransform);
					
					matrix3x4_t invHandTransform;
					MatrixInvert(handTransform, invHandTransform);
					
					// Store muzzle offset in hand-local space
					VectorTransform(muzzlePos, invHandTransform, m_vIdleMuzzleOffset);
					
					// Store FULL muzzle orientation in hand-local space (preserves roll)
					// Convert muzzle angles to matrix, then transform to hand-local space
					matrix3x4_t muzzleWorldMatrix;
					AngleMatrix(muzzleAngles, muzzlePos, muzzleWorldMatrix);
					
					matrix3x4_t muzzleLocalMatrix;
					ConcatTransforms(invHandTransform, muzzleWorldMatrix, muzzleLocalMatrix);
					
					// Extract the local angles (includes roll)
					MatrixAngles(muzzleLocalMatrix, m_angIdleMuzzleAngles);
					
					m_bIdleMuzzleOffsetValid = true;
				}
			}
			
			// Use cached idle muzzle (always, once cached)
			if (m_bIdleMuzzleOffsetValid)
			{
				// Get current hand transform (this is stable, doesn't change with fire anim)
				matrix3x4_t handTransform;
				AngleMatrix(GetAbsAngles(), GetAbsOrigin(), handTransform);
				
				// Apply cached offset to get stable muzzle position
				VectorTransform(m_vIdleMuzzleOffset, handTransform, outPos);
				
				// Apply cached orientation (includes roll) relative to hand
				matrix3x4_t localMuzzleMatrix;
				AngleMatrix(m_angIdleMuzzleAngles, vec3_origin, localMuzzleMatrix);
				
				matrix3x4_t worldMuzzleMatrix;
				ConcatTransforms(handTransform, localMuzzleMatrix, worldMuzzleMatrix);
				
				// Extract world angles (preserves roll)
				MatrixAngles(worldMuzzleMatrix, outAngles);
				
				return true;
			}
		}
	}
	
	// STANDARD WEAPONS: Query the render weapon's muzzle attachment directly.
	// The render weapon's SetupBones override forces the hand to position it
	// with fresh tracking data before computing bone/attachment transforms,
	// so the result always reflects the current controller pose.
	{
		int iMuzzle = pRenderWeapon->LookupAttachment("muzzle");
		if (iMuzzle > 0 && pRenderWeapon->GetAttachment(iMuzzle, outPos, outAngles))
		{
			// Apply weapon-specific aim angle corrections as LOCAL rotations
			// Using matrix multiplication so the correction stays in the muzzle's
			// local coordinate frame regardless of hand roll/pitch/yaw
			if (pTFWeapon)
			{
				int weaponID = pTFWeapon->GetWeaponID();
				QAngle correctionAngles(0, 0, 0);
				bool bNeedsCorrection = false;

				if (weaponID == TF_WEAPON_GRENADELAUNCHER || weaponID == TF_WEAPON_CANNON)
				{
					correctionAngles.Init(tfvr_aim_grenadelauncher_pitch.GetFloat(),
					                      tfvr_aim_grenadelauncher_yaw.GetFloat(),
					                      tfvr_aim_grenadelauncher_roll.GetFloat());
					bNeedsCorrection = true;
				}
				else if (weaponID == TF_WEAPON_PIPEBOMBLAUNCHER)
				{
					correctionAngles.Init(tfvr_aim_stickybomb_pitch.GetFloat(),
					                      tfvr_aim_stickybomb_yaw.GetFloat(),
					                      tfvr_aim_stickybomb_roll.GetFloat());
					bNeedsCorrection = true;
				}

				if (bNeedsCorrection)
				{
					matrix3x4_t muzzleMat, correctionMat, resultMat;
					AngleMatrix(outAngles, vec3_origin, muzzleMat);
					AngleMatrix(correctionAngles, vec3_origin, correctionMat);
					ConcatTransforms(muzzleMat, correctionMat, resultMat);
					MatrixAngles(resultMat, outAngles);
				}
			}

			return true;
		}
	}
	
	// Fallback: Force bone setup and query attachment directly
	// This ensures bones are current before querying
	InvalidateBoneCache();
	matrix3x4_t boneArray[MAXSTUDIOBONES];
	SetupBones(boneArray, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
	
	int iMuzzle = pRenderWeapon->LookupAttachment("muzzle");
	if (iMuzzle > 0)
	{
		Vector muzzlePos;
		QAngle muzzleAngles;
		if (pRenderWeapon->GetAttachment(iMuzzle, muzzlePos, muzzleAngles))
		{
			outPos = muzzlePos;
			outAngles = muzzleAngles;
			
			// Apply weapon-specific aim angle corrections as LOCAL rotations
			if (pTFWeapon)
			{
				int weaponID = pTFWeapon->GetWeaponID();
				QAngle correctionAngles(0, 0, 0);
				bool bNeedsCorrection = false;

				if (weaponID == TF_WEAPON_GRENADELAUNCHER || weaponID == TF_WEAPON_CANNON)
				{
					correctionAngles.Init(tfvr_aim_grenadelauncher_pitch.GetFloat(),
					                      tfvr_aim_grenadelauncher_yaw.GetFloat(),
					                      tfvr_aim_grenadelauncher_roll.GetFloat());
					bNeedsCorrection = true;
				}
				else if (weaponID == TF_WEAPON_PIPEBOMBLAUNCHER)
				{
					correctionAngles.Init(tfvr_aim_stickybomb_pitch.GetFloat(),
					                      tfvr_aim_stickybomb_yaw.GetFloat(),
					                      tfvr_aim_stickybomb_roll.GetFloat());
					bNeedsCorrection = true;
				}

				if (bNeedsCorrection)
				{
					matrix3x4_t muzzleMat, correctionMat, resultMat;
					AngleMatrix(outAngles, vec3_origin, muzzleMat);
					AngleMatrix(correctionAngles, vec3_origin, correctionMat);
					ConcatTransforms(muzzleMat, correctionMat, resultMat);
					MatrixAngles(resultMat, outAngles);
				}
			}
			
			return true;
		}
	}
	
	// Final fallback: use controller position with forward offset
	outPos = GetAbsOrigin();
	outAngles = GetAbsAngles();
	
	Vector forward;
	AngleVectors(outAngles, &forward, NULL, NULL);
	outPos += forward * 30.0f;
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Determine the correct idle animation for Heavy fist weapons.
//          Gloved variants use bg_idle, bare-fist variants use f_idle.
//-----------------------------------------------------------------------------
static const char* GetFistsIdleAnimName(C_TFWeaponBase *pWeapon)
{
	if (pWeapon)
	{
		const char *worldModel = pWeapon->GetWorldModel();
		if (worldModel)
		{
			if (V_stristr(worldModel, "boxing_gloves") ||
				V_stristr(worldModel, "gru") ||
				V_stristr(worldModel, "fists_of_steel") ||
				V_stristr(worldModel, "xms_gloves") ||
				V_stristr(worldModel, "breadmonster"))
			{
				return "bg_idle";
			}
		}
	}
	return "f_idle";
}

//-----------------------------------------------------------------------------
// Purpose: Get the appropriate hand animation name for a weapon
//-----------------------------------------------------------------------------
const char* GetWeaponPoseAnimation(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	// Default fallback
	const char *defaultAnim = "ref";
	
	// Check if this is an all-class melee weapon (frying pan, saxxy, etc.)
	// These should use the class's default melee animation
	// NOTE: melee_allclass_idle animation crashes in AccumulatePose, so we use class-specific melee anims
	bool bIsAllClassMelee = false;
	if (pWeapon)
	{
		const char *worldModel = pWeapon->GetWorldModel();
		if (worldModel)
		{
			if (V_stristr(worldModel, "frying_pan") ||
				V_stristr(worldModel, "saxxy") ||
				V_stristr(worldModel, "golden_wrench") ||
				V_stristr(worldModel, "necro_smasher") ||
				V_stristr(worldModel, "crossing_guard") ||
				V_stristr(worldModel, "freedom_staff") ||
				V_stristr(worldModel, "ham_shank") ||
				V_stristr(worldModel, "memory_maker") ||
				V_stristr(worldModel, "prinny_machete") ||
				V_stristr(worldModel, "conscientious"))
			{
				bIsAllClassMelee = true;
			}
		}
	}
	
	// For all-class melee weapons, use melee_allclass_idle
	// AccumulatePose will detect invalid animation data and skip it gracefully
	if (bIsAllClassMelee)
	{
		return "melee_allclass_idle";
	}
	
	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			// Scout: sg_idle, p_idle, b_idle, wb_idle, ss_idle (shortstop), db_idle (double-barrel), ed_idle (drinks/milk), cleave_idle (guillotine), bm_idle
			if (V_stristr(weaponClass, "soda_popper")) return "db_idle"; // Soda Popper (double-barrel)
			if (V_stristr(weaponClass, "pep_brawler_blaster")) return "sg_idle"; // Baby Face's Blaster
			// Check item def index for Force-A-Nature (item def 45) - double-barrel scattergun variant
			if (V_stristr(weaponClass, "scattergun") && pWeapon)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 45)
					return "db_idle"; // Force-A-Nature (double-barrel)
			}
			if (V_stristr(weaponClass, "scattergun")) return "sg_idle";
			if (V_stristr(weaponClass, "handgun_scout")) return "ss_idle"; // Shortstop
			if (V_stristr(weaponClass, "pistol")) return "p_idle";
			if (V_stristr(weaponClass, "wrap")) return "wb_idle"; // Wrap Assassin (melee with ball)
			if (V_stristr(weaponClass, "bat")) return "b_idle";
			if (V_stristr(weaponClass, "lunchbox_drink")) return "ed_idle"; // Bonk/Crit-a-Cola
			// Use weapon ID to distinguish throwables - cleaver vs jars
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_CLEAVER) return "cleave_idle"; // Flying Guillotine
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_JAR_MILK)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1121)
					return "bm_idle"; // Mutated Milk (bread monster)
				return "ed_idle"; // Mad Milk
			}
			if (V_stristr(weaponClass, "jar")) return "ed_idle"; // Jarate and other jars
			if (V_stristr(weaponClass, "throwable")) return "throw_idle"; // Generic throwables
			if (V_stristr(weaponClass, "spellbook")) return "bm_idle";
			break;
			
		case TF_CLASS_SOLDIER:
			// Soldier: dh_idle, idle, s_idle, bb_idle, wh_idle, bison_idle, bet_idle, throw_idle
			if (V_stristr(weaponClass, "rocketlauncher")) return "dh_idle";
			if (V_stristr(weaponClass, "particle_cannon")) return "dh_idle"; // Cow Mangler
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "katana")) return "s_idle"; // Half-Zatoichi
			if (V_stristr(weaponClass, "sword")) return "s_idle"; // Any sword weapons
			if (V_stristr(weaponClass, "shovel")) return "s_idle";
			if (V_stristr(weaponClass, "pickaxe")) return "s_idle"; // Equalizer
			if (V_stristr(weaponClass, "buff_item")) return "bb_idle"; // Buff Banner/Battalion's Backup/Concheror
			if (V_stristr(weaponClass, "whip")) return "wh_idle"; // Disciplinary Action
			if (V_stristr(weaponClass, "raygun")) return "bison_idle"; // Righteous Bison
			if (V_stristr(weaponClass, "parachute")) return "bet_idle"; // B.A.S.E. Jumper
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
			
		case TF_CLASS_PYRO:
			// Pyro: ft_idle, fg_idle, fa_idle, idle, mm_idle, throw_idle
			if (V_stristr(weaponClass, "flamethrower")) return "ft_idle";
			if (V_stristr(weaponClass, "rocketlauncher_fireball")) return "ft_idle"; // Dragon's Fury
			if (V_stristr(weaponClass, "flaregun")) return "fg_idle";
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "fireaxe")) return "fa_idle";
			if (V_stristr(weaponClass, "slap")) return "fa_idle"; // Hot Hand
			if (V_stristr(weaponClass, "jar_gas")) return "mm_idle"; // Gas Passer
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
			
		case TF_CLASS_DEMOMAN:
			// Demo: g_idle, sb_idle, b_idle, cm_idle, throw_idle
			if (V_stristr(weaponClass, "grenadelauncher")) return "g_idle";
			if (V_stristr(weaponClass, "cannon")) return "g_idle"; // Loose Cannon
			if (V_stristr(weaponClass, "pipebomblauncher")) return "sb_idle";
			if (V_stristr(weaponClass, "stickbomb")) return "sb_idle";
			if (V_stristr(weaponClass, "bottle")) return "b_idle";
			if (V_stristr(weaponClass, "sword")) return "cm_idle"; // Eyelander, Half-Zatoichi, etc.
			if (V_stristr(weaponClass, "katana")) return "cm_idle";
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
			
		case TF_CLASS_HEAVYWEAPONS:
			// Heavy: m_idle, idle, f_idle, bg_idle, sw_idle, throw_idle, breadglove_idle_*
			if (V_stristr(weaponClass, "minigun")) return "m_idle";
			if (V_stristr(weaponClass, "shotgun")) return "idle";
			if (V_stristr(weaponClass, "fists")) return GetFistsIdleAnimName(pWeapon);
			if (V_stristr(weaponClass, "steak")) return "sw_idle"; // Buffalo Steak Sandvich
			if (V_stristr(weaponClass, "lunchbox")) return "sw_idle"; // Sandvich, Dalokohs Bar, etc.
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
			
		case TF_CLASS_ENGINEER:
			// Engineer: fj_idle, pstl_idle, gun_idle, pdq_idle_tap, pda_idle, bld_idle, wgl_idle, spk_idle, pomson_idle, box_idle, throw_idle
			// Check specific weapons before generic shotgun
			if (V_stristr(weaponClass, "sentry_revenge")) return "fj_idle"; // Frontier Justice
			if (V_stristr(weaponClass, "shotgun")) return "fj_idle"; // All other shotguns also use FJ pose
			if (V_stristr(weaponClass, "pistol")) return "pstl_idle";
			if (V_stristr(weaponClass, "wrench")) return "pdq_idle_tap"; // Wrench
			if (V_stristr(weaponClass, "robot_arm")) return "pdq_idle_tap"; // Gunslinger
			if (V_stristr(weaponClass, "pda_engineer_build")) return "bld_idle";
			if (V_stristr(weaponClass, "pda_engineer_destroy")) return "pda_idle";
			if (V_stristr(weaponClass, "laser_pointer")) return "wgl_idle"; // Wrangler
			if (V_stristr(weaponClass, "drg_pomson")) return "pomson_idle"; // Pomson 6000
			if (V_stristr(weaponClass, "raygun")) return "pomson_idle"; // Rescue Ranger
			if (V_stristr(weaponClass, "mechanical_arm")) return "spk_idle"; // Short Circuit
			if (V_stristr(weaponClass, "builder")) return "box_idle"; // Toolbox
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
			
		case TF_CLASS_MEDIC:
			// Medic: sg_idle, idle, bs_idle, throw_idle
			if (V_stristr(weaponClass, "syringegun")) return "sg_idle";
			if (V_stristr(weaponClass, "crossbow")) return "sg_idle"; // Crusader's Crossbow
			if (V_stristr(weaponClass, "medigun")) return "idle";
			if (V_stristr(weaponClass, "bonesaw")) return "bs_idle";
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
			
		case TF_CLASS_SNIPER:
			// Sniper: bw_idle, smg_idle, m_idle, pj_idle, idle, throw_idle, bm_idle, rifolver_idle
			if (V_stristr(weaponClass, "sniperrifle")) return "idle"; // Sniper rifles use generic rifle idle
			if (V_stristr(weaponClass, "compound_bow")) return "bw_idle"; // Huntsman uses bow idle
			if (V_stristr(weaponClass, "smg")) return "smg_idle";
			if (V_stristr(weaponClass, "club")) return "m_idle"; // Kukri, Bushwacka, Shahanshah, etc.
			if (V_stristr(weaponClass, "sword")) return "m_idle";
			if (V_stristr(weaponClass, "jar")) return "pj_idle"; // Jarate
			if (V_stristr(weaponClass, "cleaver")) return "throw_idle"; // Throwing weapons
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			if (V_stristr(weaponClass, "charged_smg")) return "idle"; // Cleaner's Carbine (uses generic idle)
			break;
			
		case TF_CLASS_SPY:
			// Spy: idle, knife_idle, c_sapper_idle, offhand_idle, eternal_idle, acr_idle, throw_idle, c_dart_gun_idle
			if (V_stristr(weaponClass, "revolver")) return "idle";
			if (V_stristr(weaponClass, "knife")) return "knife_idle";
			if (V_stristr(weaponClass, "sapper")) return "c_sapper_idle";
			if (V_stristr(weaponClass, "pda_spy")) return "offhand_idle"; // Disguise kit
			if (V_stristr(weaponClass, "invis")) return "offhand_idle"; // Invis watch
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			break;
	}
	
	// Check for other universal weapon types (melee_allclass is handled at the start via GetActivityWeaponRole)
	if (V_stristr(weaponClass, "spellbook")) return "bm_idle";
	
	return defaultAnim;
}

//-----------------------------------------------------------------------------
// Purpose: Get the melee swing animation BASE name (without a/b/c suffix)
//         Returns NULL if not a melee weapon with swing cycling, or the base name if it is
//         e.g., returns "b_swing_" for bat, which will have "b_swing_a", "b_swing_b", "b_swing_c"
//         Also returns the number of swing variants (usually 3 for a/b/c)
//-----------------------------------------------------------------------------
const char* GetMeleeSwingBaseName(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon, int &outSwingCount)
{
	outSwingCount = 0;
	
	// Check if this is an all-class melee weapon
	if (pWeapon)
	{
		const char *worldModel = pWeapon->GetWorldModel();
		if (worldModel)
		{
			if (V_stristr(worldModel, "frying_pan") ||
				V_stristr(worldModel, "saxxy") ||
				V_stristr(worldModel, "golden_wrench") ||
				V_stristr(worldModel, "necro_smasher") ||
				V_stristr(worldModel, "crossing_guard") ||
				V_stristr(worldModel, "freedom_staff") ||
				V_stristr(worldModel, "ham_shank") ||
				V_stristr(worldModel, "memory_maker") ||
				V_stristr(worldModel, "prinny_machete") ||
				V_stristr(worldModel, "conscientious"))
			{
				// All-class melee uses a single animation, no cycling
				return NULL;
			}
		}
	}
	
	// Per-class melee swing bases
	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			if (V_stristr(weaponClass, "bat"))
			{
				outSwingCount = 3;
				return "b_swing_";
			}
			break;
			
		case TF_CLASS_SOLDIER:
			if (V_stristr(weaponClass, "katana") ||
				V_stristr(weaponClass, "sword") ||
				V_stristr(weaponClass, "shovel") ||
				V_stristr(weaponClass, "pickaxe"))
			{
				outSwingCount = 3;
				return "s_swing_";
			}
			break;
			
		case TF_CLASS_PYRO:
			if (V_stristr(weaponClass, "fireaxe") ||
				V_stristr(weaponClass, "slap"))
			{
				outSwingCount = 3;
				return "fa_swing_";
			}
			break;
			
		case TF_CLASS_DEMOMAN:
			if (V_stristr(weaponClass, "bottle"))
			{
				outSwingCount = 3;
				return "b_swing_";
			}
			if (V_stristr(weaponClass, "sword") ||
				V_stristr(weaponClass, "katana"))
			{
				outSwingCount = 3;
				return "cm_swing_";
			}
			break;
			
		case TF_CLASS_HEAVYWEAPONS:
			if (V_stristr(weaponClass, "fists"))
			{
				outSwingCount = 3;
				return "f_swing_";
			}
			if (V_stristr(weaponClass, "gloves"))
			{
				outSwingCount = 3;
				return "bg_swing_";
			}
			break;
			
		case TF_CLASS_ENGINEER:
			if (V_stristr(weaponClass, "wrench"))
			{
				outSwingCount = 3;
				return "pdq_swing_";
			}
			if (V_stristr(weaponClass, "robot_arm"))
			{
				outSwingCount = 3;
				return "gun_swing_";
			}
			if (V_stristr(weaponClass, "mechanical_arm"))
			{
				outSwingCount = 3;
				return "spk_swing_";
			}
			break;
			
		case TF_CLASS_MEDIC:
			if (V_stristr(weaponClass, "bonesaw"))
			{
				outSwingCount = 3;
				return "bs_swing_";
			}
			break;
			
		case TF_CLASS_SNIPER:
			if (V_stristr(weaponClass, "club") ||
				V_stristr(weaponClass, "sword"))
			{
				outSwingCount = 3;
				return "m_swing_";
			}
			break;
			
		case TF_CLASS_SPY:
			if (V_stristr(weaponClass, "knife"))
			{
				outSwingCount = 3;
				return "knife_stab_";
			}
			break;
	}
	
	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Get the appropriate fire animation name for a weapon
//-----------------------------------------------------------------------------
const char* GetWeaponFireAnimation(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	// Default fallback - no fire animation
	const char *defaultAnim = NULL;
	
	// Check if this is an all-class melee weapon
	bool bIsAllClassMelee = false;
	if (pWeapon)
	{
		const char *worldModel = pWeapon->GetWorldModel();
		if (worldModel)
		{
			if (V_stristr(worldModel, "frying_pan") ||
				V_stristr(worldModel, "saxxy") ||
				V_stristr(worldModel, "golden_wrench") ||
				V_stristr(worldModel, "necro_smasher") ||
				V_stristr(worldModel, "crossing_guard") ||
				V_stristr(worldModel, "freedom_staff") ||
				V_stristr(worldModel, "ham_shank") ||
				V_stristr(worldModel, "memory_maker") ||
				V_stristr(worldModel, "prinny_machete") ||
				V_stristr(worldModel, "conscientious"))
			{
				bIsAllClassMelee = true;
			}
		}
	}
	
	// For all-class melee weapons, use melee_allclass_swing
	if (bIsAllClassMelee)
	{
		return "melee_allclass_swing";
	}
	
	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			// Scout fire animations: sg_fire, SS_fire, p_fire, b_swing_*, wb_fire, db_fire, cleave_throw, spell_fire, bm_fire
			if (V_stristr(weaponClass, "soda_popper")) return "db_fire"; // Soda Popper (double-barrel)
			if (V_stristr(weaponClass, "pep_brawler_blaster")) return "sg_fire"; // Baby Face's Blaster
			// Check item def index for Force-A-Nature (item def 45) - double-barrel scattergun variant
			if (V_stristr(weaponClass, "scattergun") && pWeapon)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 45)
					return "db_fire"; // Force-A-Nature (double-barrel)
			}
			if (V_stristr(weaponClass, "scattergun")) return "sg_fire";
			if (V_stristr(weaponClass, "handgun_scout")) return "SS_fire"; // Shortstop
			if (V_stristr(weaponClass, "pistol")) return "p_fire";
			if (V_stristr(weaponClass, "wrap")) return "wb_fire"; // Wrap Assassin
			if (V_stristr(weaponClass, "bat")) return "b_swing_a"; // Could cycle through a/b/c
			// Use weapon ID to distinguish throwables - cleaver vs jars
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_CLEAVER) return "cleave_throw"; // Flying Guillotine
			if (V_stristr(weaponClass, "jar")) return "throw_fire"; // Jarate, Mad Milk, other jars
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			if (V_stristr(weaponClass, "spellbook")) return "spell_fire";
			break;
			
		case TF_CLASS_SOLDIER:
			// Soldier: dh_fire, fire (shotgun), s_swing_*, bison_fire, throw_fire
			if (V_stristr(weaponClass, "rocketlauncher")) return "dh_fire";
			if (V_stristr(weaponClass, "particle_cannon")) return "dh_fire"; // Cow Mangler
			if (V_stristr(weaponClass, "shotgun")) return "fire";
			if (V_stristr(weaponClass, "katana")) return "s_swing_a";
			if (V_stristr(weaponClass, "sword")) return "s_swing_a";
			if (V_stristr(weaponClass, "shovel")) return "s_swing_a";
			if (V_stristr(weaponClass, "pickaxe")) return "s_swing_a";
			if (V_stristr(weaponClass, "whip")) return "wh_fire"; // Disciplinary Action
			if (V_stristr(weaponClass, "raygun")) return "bison_fire"; // Righteous Bison
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_PYRO:
			// Pyro: ft_fire, fg_fire, fa_swing_*, fire (shotgun), mm_throw, throw_fire
			if (V_stristr(weaponClass, "flamethrower")) return "ft_fire";
			if (V_stristr(weaponClass, "rocketlauncher_fireball")) return "ft_fire"; // Dragon's Fury
			if (V_stristr(weaponClass, "flaregun")) return "fg_fire";
			if (V_stristr(weaponClass, "shotgun")) return "fire";
			if (V_stristr(weaponClass, "fireaxe")) return "fa_swing_a";
			if (V_stristr(weaponClass, "slap")) return "fa_swing_a"; // Hot Hand
			if (V_stristr(weaponClass, "jar_gas")) return "mm_throw"; // Gas Passer
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_DEMOMAN:
			// Demo: g_fire, sb_fire, b_swing_*, cm_swing_*, throw_fire
			if (V_stristr(weaponClass, "grenadelauncher")) return "g_fire";
			if (V_stristr(weaponClass, "cannon")) return "g_fire"; // Loose Cannon
			if (V_stristr(weaponClass, "pipebomblauncher")) return "sb_fire";
			if (V_stristr(weaponClass, "stickbomb")) return "sb_fire";
			if (V_stristr(weaponClass, "bottle")) return "b_swing_a";
			if (V_stristr(weaponClass, "sword")) return "cm_swing_a"; // Eyelander, etc.
			if (V_stristr(weaponClass, "katana")) return "cm_swing_a";
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_HEAVYWEAPONS:
			// Heavy: m_fire, fire (shotgun), f_swing_*, bg_swing_*, throw_fire
			if (V_stristr(weaponClass, "minigun")) return "m_fire";
			if (V_stristr(weaponClass, "shotgun")) return "fire";
			if (V_stristr(weaponClass, "fists")) return "f_swing_a";
			if (V_stristr(weaponClass, "gloves")) return "bg_swing_a"; // KGB, GRU, etc.
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_ENGINEER:
			// Engineer: fj_fire, pstl_fire, pdq_swing_a, gun_swing_a, wgl_fire, spk_swing_a, pomson_fire, throw_fire
			if (V_stristr(weaponClass, "sentry_revenge")) return "fj_fire"; // Frontier Justice
			if (V_stristr(weaponClass, "shotgun")) return "fj_fire";
			if (V_stristr(weaponClass, "pistol")) return "pstl_fire";
			if (V_stristr(weaponClass, "wrench")) return "pdq_swing_a";
			if (V_stristr(weaponClass, "robot_arm")) return "gun_swing_a"; // Gunslinger
			if (V_stristr(weaponClass, "laser_pointer")) return "wgl_fire"; // Wrangler (no fire anim, just idle)
			if (V_stristr(weaponClass, "drg_pomson")) return "pomson_fire"; // Pomson 6000
			if (V_stristr(weaponClass, "raygun")) return "pomson_fire"; // Rescue Ranger
			if (V_stristr(weaponClass, "mechanical_arm")) return "spk_swing_a"; // Short Circuit
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_MEDIC:
			// Medic: sg_fire (syringe), bs_swing_a (bonesaw), fire_loop (medigun)
			if (V_stristr(weaponClass, "syringegun")) return "sg_fire";
			if (V_stristr(weaponClass, "crossbow")) return "sg_fire"; // Uses same anim as syringe gun
			if (V_stristr(weaponClass, "medigun")) return "fire_loop";
			if (V_stristr(weaponClass, "bonesaw")) return "bs_swing_a";
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_SNIPER:
			// Sniper: sr_fire, smg_fire, m_swing_*, cs_fire, ss_fire, throw_fire
			if (V_stristr(weaponClass, "sniperrifle")) return "fire"; // Sniper rifles use generic fire
			if (V_stristr(weaponClass, "smg")) return "smg_fire";
			if (V_stristr(weaponClass, "club")) return "m_swing_a";
			if (V_stristr(weaponClass, "sword")) return "m_swing_a"; // Bushwacka
			if (V_stristr(weaponClass, "crossbow")) return "cs_fire"; // Huntsman
			if (V_stristr(weaponClass, "compound_bow")) return "cs_fire";
			if (V_stristr(weaponClass, "shotgun")) return "ss_fire";
			if (V_stristr(weaponClass, "jar")) return "throw_fire"; // Jarate
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
			
		case TF_CLASS_SPY:
			// Spy: fire (revolver), knife_stab_a (knife)
			if (V_stristr(weaponClass, "revolver")) return "fire";
			if (V_stristr(weaponClass, "knife")) return "knife_stab_a";
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;
	}
	
	return defaultAnim; // No fire animation for this weapon
}

//-----------------------------------------------------------------------------
// Purpose: Get the appropriate alt-fire (secondary attack) animation name
//          Currently only the flamethrower airblast has a distinct alt-fire anim.
//-----------------------------------------------------------------------------
const char* GetWeaponAltFireAnimation(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	if (playerClass == TF_CLASS_PYRO)
	{
		if (V_stristr(weaponClass, "flamethrower")) return "ft_alt_fire";
		if (V_stristr(weaponClass, "rocketlauncher_fireball")) return "ft_alt_fire"; // Dragon's Fury
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Get the charge/pullback animation for weapons that charge up
//          (sticky launcher, huntsman, loose cannon, etc.)
//-----------------------------------------------------------------------------
const char* GetWeaponChargeAnimation(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	switch (playerClass)
	{
		case TF_CLASS_DEMOMAN:
			if (V_stristr(weaponClass, "pipebomblauncher")) return "sb_autofire";
			if (V_stristr(weaponClass, "cannon")) return "g_auto_fire"; // Loose Cannon
			break;
		case TF_CLASS_SNIPER:
			if (V_stristr(weaponClass, "compound_bow")) return "bw_charge"; // Initial bow draw
			break;
	}
	return NULL;
}

// Second phase charge animation (e.g. huntsman arm-strain shake at max charge)
const char* GetWeaponChargeAnimation2(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	switch (playerClass)
	{
		case TF_CLASS_SNIPER:
			if (V_stristr(weaponClass, "compound_bow")) return "bw_shake";
			break;
	}
	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Apply weapon grip pose to fingers (overrides finger tracking)
//        Samples finger bone rotations from the hand model's weapon animation
//-----------------------------------------------------------------------------
void C_TFVRHand::ApplyWeaponPose(matrix3x4_t *pBoneToWorldOut, int nMaxBones, C_TFWeaponBase *pWeaponOverride)
{
	CStudioHdr *pStudioHdr = GetModelPtr();
	if (!pStudioHdr)
		return;
	
	C_TFWeaponBase *pWeapon = pWeaponOverride ? pWeaponOverride : m_hHeldWeapon.Get();
	if (!pWeapon)
		return;
	
	// Get the player to determine class
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (!pOwner)
		return;
	
	int playerClass = pOwner->GetPlayerClass()->GetClassIndex();
	const char *weaponClass = pWeapon->GetClassname();
	
	// When a charge animation is active, sample from it instead of the idle pose
	// so weapon_bone (and fingers) reflect the charge animation's movement
	int sequence = -1;
	const char *usedName = NULL;
	float cycle = 0.0f;

	if (m_bPlayingChargeAnim || m_bPlayingFireAnim)
	{
		int activeSeq = GetSequence();
		if (activeSeq >= 0)
		{
			sequence = activeSeq;
			cycle = GetCycle();
			usedName = m_bPlayingChargeAnim ? "charge_anim" : "fire_anim";
		}
	}

	if (sequence < 0)
	{
		// Default: use idle pose animation
		const char *animName = GetWeaponPoseAnimation(playerClass, weaponClass, pWeapon);

		char vrAnimName[128];
		Q_snprintf(vrAnimName, sizeof(vrAnimName), "vr_%s", animName);
		sequence = LookupSequence(vrAnimName);

		usedName = vrAnimName;
		if (sequence < 0)
		{
			usedName = animName;
			sequence = LookupSequence(animName);
		}
		if (sequence < 0)
		{
			usedName = "ref";
			sequence = LookupSequence("ref");
			if (sequence < 0)
			{
				return;
			}
		}
	}

	// Get the sequence descriptor
	mstudioseqdesc_t &seqdesc = pStudioHdr->pSeqdesc(sequence);
	
	// Temporary bone arrays for sampling the animation
	Vector pos[MAXSTUDIOBONES];
	Quaternion q[MAXSTUDIOBONES];
	
	// Create pose parameter array with default values
	// This is needed for sequences with blendlayers (like melee_allclass which uses r_handposes)
	float poseParameters[MAXSTUDIOPOSEPARAM];
	for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
	{
		poseParameters[i] = 0.0f;
	}
	
	// Sample the animation pose
	IBoneSetup boneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters);
	boneSetup.InitPose(pos, q);
	
	// Debug output
	if (tfvr_debug_weapon_position.GetBool())
	{
		Msg("ANIM DEBUG: '%s' seq=%d groupsize=[%d,%d] flags=0x%x numblends=%d numautolayers=%d\n",
			usedName, sequence, seqdesc.groupsize[0], seqdesc.groupsize[1],
			seqdesc.flags, seqdesc.numblends, seqdesc.numautolayers);
	}
	
	// AccumulatePose samples the animation and applies it to the bone arrays
	// Now that we properly pass poseParameters, this is safe for all sequences
	// including $declaresequence animations that get their data via $includemodel
	boneSetup.AccumulatePose(pos, q, sequence, cycle, 1.0f, gpGlobals->curtime, NULL);
	
	// Debug: Check weapon_bone values after AccumulatePose
	if (tfvr_debug_weapon_position.GetBool())
	{
		int weaponBoneIdx = LookupBone("weapon_bone");
		if (weaponBoneIdx >= 0)
		{
			QAngle weaponBoneAng;
			QuaternionAngles(q[weaponBoneIdx], weaponBoneAng);
			Msg("ANIM DEBUG: weapon_bone[%d] after AccumulatePose: pos=(%.2f, %.2f, %.2f) ang=(%.1f, %.1f, %.1f)\n",
				weaponBoneIdx, pos[weaponBoneIdx].x, pos[weaponBoneIdx].y, pos[weaponBoneIdx].z,
				weaponBoneAng.x, weaponBoneAng.y, weaponBoneAng.z);
			
			// Also log bind pose for comparison
			const mstudiobone_t *pWpnBone = pStudioHdr->pBone(weaponBoneIdx);
			if (pWpnBone)
			{
				QAngle bindAng;
				QuaternionAngles(pWpnBone->quat, bindAng);
				Msg("ANIM DEBUG: weapon_bone bind pose: pos=(%.2f, %.2f, %.2f) ang=(%.1f, %.1f, %.1f)\n",
					pWpnBone->pos.x, pWpnBone->pos.y, pWpnBone->pos.z,
					bindAng.x, bindAng.y, bindAng.z);
			}
		}
	}
	
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
	
	// Don't draw merc hands during preamble (no class selected) - show controller models instead
	if (pOwner->GetPlayerClass() && pOwner->GetPlayerClass()->GetClassIndex() == TF_CLASS_UNDEFINED)
		return false;
	
	// Don't draw merc hands when dead - show controller models instead
	if (pOwner->IsPlayerDead())
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
	// Use convar to control bounds size for debugging
	float boundSize = tfvr_hands_shadow_bounds.GetFloat();
	mins = Vector(-boundSize, -boundSize, -boundSize);
	maxs = Vector(boundSize, boundSize, boundSize);
}

//-----------------------------------------------------------------------------
// Purpose: Draw the hand model with ubercharge effect support
// Note: Cloak is handled by the vm_invis material proxy in the hand materials
//-----------------------------------------------------------------------------
int C_TFVRHand::DrawModel(int flags)
{
	// Safety checks before drawing
	if (m_bShuttingDown)
		return 0;
		
	if (!ShouldDraw())
		return 0;

	// VR hand layer: skip during world pass, register self and attachables for hand layer
	if (VRHandLayer_ShouldSkipDraw())
	{
		VRHandLayer_AddRenderable(this);
		VRHandLayer_AddParticleOwner(this);

		if (m_hRenderWeapon.Get())
		{
			VRHandLayer_AddRenderable(m_hRenderWeapon.Get());
			VRHandLayer_AddParticleOwner(m_hRenderWeapon.Get());
		}

		// Source weapon creates particles like loose cannon sparks
		if (m_hHeldWeapon.Get())
			VRHandLayer_AddParticleOwner(m_hHeldWeapon.Get());

		if (m_hLeftHandWatch.Get())
		{
			VRHandLayer_AddRenderable(m_hLeftHandWatch.Get());
			VRHandLayer_AddParticleOwner(m_hLeftHandWatch.Get());
		}
		if (m_hLeftHandBall.Get())
		{
			VRHandLayer_AddRenderable(m_hLeftHandBall.Get());
			VRHandLayer_AddParticleOwner(m_hLeftHandBall.Get());
		}

		return 0;
	}
	
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
	
	int ret = 0;
	bool bInvuln = pOwner->m_Shared.IsInvulnerable();
	
	// Apply ubercharge material override
	if (bInvuln && (flags & STUDIO_RENDER))
	{
		modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());
	}
	
	ret = BaseClass::DrawModel(flags);
	
	// Reset material override
	if (bInvuln && (flags & STUDIO_RENDER))
	{
		modelrender->ForcedMaterialOverride(NULL);
	}
	
	return ret;
}

//-----------------------------------------------------------------------------
// Purpose: Always cast shadows for VR hands
//-----------------------------------------------------------------------------
ShadowType_t C_TFVRHand::ShadowCastType()
{
	// Allow runtime control of shadow type for debugging
	int shadowType = tfvr_hands_shadow_type.GetInt();
	
	if (tfvr_hands_shadow_debug.GetBool())
	{
		static float lastPrintTime = 0;
		if (gpGlobals->curtime - lastPrintTime > 2.0f)
		{
			Msg("VR Hand (%s): Shadow type=%d, bounds=%.0f, distance=%.0f, origin=(%.1f, %.1f, %.1f)\n", 
				IsLeftHand() ? "LEFT" : "RIGHT",
				shadowType,
				tfvr_hands_shadow_bounds.GetFloat(),
				tfvr_hands_shadow_distance.GetFloat(),
				GetAbsOrigin().x, GetAbsOrigin().y, GetAbsOrigin().z);
			lastPrintTime = gpGlobals->curtime;
		}
	}
	
	switch (shadowType)
	{
		case 0: return SHADOWS_NONE;
		case 1: return SHADOWS_SIMPLE;
		case 2: return SHADOWS_RENDER_TO_TEXTURE;
		case 3: return SHADOWS_RENDER_TO_TEXTURE_DYNAMIC;
		default: return SHADOWS_RENDER_TO_TEXTURE;
	}
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
// Purpose: Returns true when the owner is cloaking (for transparency rendering)
//-----------------------------------------------------------------------------
bool C_TFVRHand::IsTransparent()
{
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (pOwner)
	{
		return pOwner->GetPercentInvisible() > 0.0f;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Override shadow cast distance
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetShadowCastDistance(float *pDist, ShadowType_t shadowType) const
{
	*pDist = tfvr_hands_shadow_distance.GetFloat();
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
	
	// Reset aim stabilization so it recaptures reference with this weapon's grip
	m_bAimRefValid = false;
	
	// VR NEW APPROACH: Create a separate render-only entity for the weapon visual
	// This way the player's actual weapon can remain in the viewmodel system
	// and we have full control over a separate worldmodel entity for rendering
	
	// Use world model for VR (c_models in TF2 are the world models)
	const char *worldModel = pWeapon->GetWorldModel();
	
	if (!worldModel || !worldModel[0])
		return;
	
	// Get owner player for effects
	C_TFPlayer *pOwner = GetOwnerPlayer();
	
	// Create our custom render weapon that implements IHasOwner for material proxies
	// (This allows crit glow and other effects to work properly)
	C_VRRenderWeapon *pRenderWeapon = new C_VRRenderWeapon;
	if (!pRenderWeapon)
		return;
	
	// Initialize it
	if (!pRenderWeapon->InitializeAsClientEntity(worldModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		pRenderWeapon->Release();
		return;
	}
	
	// Set owner for material proxies (crit glow, uber, cloak, etc.)
	pRenderWeapon->SetOwnerPlayer(pOwner);
	pRenderWeapon->SetOwnerEntity(pOwner);
	
	// Set source weapon so we can call its ViewModelAttachmentBlending
	pRenderWeapon->SetSourceWeapon(pWeapon);
	
	// Make weapon think every frame so animations can advance
	pRenderWeapon->SetNextClientThink(CLIENT_THINK_ALWAYS);
	
	// Store the render weapon
	m_hRenderWeapon = pRenderWeapon;
	
	// Set it up
	pRenderWeapon->SetModelIndex(modelinfo->GetModelIndex(worldModel));
	pRenderWeapon->SetRenderMode(kRenderNormal);
	pRenderWeapon->SetRenderColor(255, 255, 255, 255);
	pRenderWeapon->RemoveEffects(EF_NODRAW);
	pRenderWeapon->RemoveEffects(EF_NOSHADOW); // Ensure shadows are enabled
	pRenderWeapon->AddEffects(EF_NOINTERP); // Disable interpolation - VR positions are set directly each frame
	pRenderWeapon->AddToLeafSystem(RENDER_GROUP_OPAQUE_ENTITY); // Add to render system for shadows
	pRenderWeapon->CreateShadow(); // Create shadow handle for dynamic shadows
	
	// Determine the correct fire animation for this weapon and hand
	// NOTE: We look it up on the HAND model, not the weapon model
	if (pOwner)
	{
		int playerClass = pOwner->GetPlayerClass()->GetClassIndex();
		const char *weaponClass = pWeapon->GetClassname();
		const char *fireAnimName = GetWeaponFireAnimation(playerClass, weaponClass, pWeapon);
		const char *idleAnimName = GetWeaponPoseAnimation(playerClass, weaponClass, pWeapon);
		
		// Look up the fire sequence on the HAND model
		m_iFireSequence = -1;
		m_iIdleSequence = -1;
		
		// Check if this is a melee weapon with swing cycling
		int swingCount = 0;
		const char *swingBase = GetMeleeSwingBaseName(playerClass, weaponClass, pWeapon, swingCount);
		if (swingBase && swingCount > 0)
		{
			// Store melee swing info for cycling during attacks
			V_strncpy(m_szMeleeSwingBase, swingBase, sizeof(m_szMeleeSwingBase));
			m_iMeleeSwingCount = swingCount;
			m_iMeleeSwingIndex = 0; // Reset swing index on weapon equip
			
			// Look up the first swing animation as the default fire sequence
			char firstSwing[128];
			V_snprintf(firstSwing, sizeof(firstSwing), "%sa", swingBase);
			m_iFireSequence = LookupSequence(firstSwing);
			
			extern ConVar tfvr_weapon_fire_anim_debug;
			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Melee swing cycling setup - base: '%s', count: %d, first swing: '%s' (seq %d)\n", 
					swingBase, swingCount, firstSwing, m_iFireSequence);
			}
		}
		else
		{
			// Not a melee with swing cycling, use normal fire animation lookup
			m_szMeleeSwingBase[0] = '\0';
			m_iMeleeSwingCount = 0;
			
			if (fireAnimName && fireAnimName[0])
			{
				extern ConVar tfvr_weapon_fire_anim_debug;
				
				// Try VR-specific override first (e.g. "vr_m_fire" before "m_fire")
				char vrFireName[128];
				Q_snprintf(vrFireName, sizeof(vrFireName), "vr_%s", fireAnimName);
				m_iFireSequence = LookupSequence(vrFireName);
				
				if (m_iFireSequence >= 0)
				{
					if (tfvr_weapon_fire_anim_debug.GetBool())
					{
						DevMsg("VR: Fire sequence using VR override '%s' (seq %d) on model '%s'\n",
							vrFireName, m_iFireSequence, GetModelName());
					}
				}
				else
				{
					m_iFireSequence = LookupSequence(fireAnimName);
					if (tfvr_weapon_fire_anim_debug.GetBool())
					{
						DevMsg("VR: Fire sequence fallback to '%s' (seq %d) on model '%s'\n",
							fireAnimName, m_iFireSequence, GetModelName());
					}
				}
			}
		}
		
		if (idleAnimName && idleAnimName[0])
		{
			char vrIdleName[128];
			Q_snprintf(vrIdleName, sizeof(vrIdleName), "vr_%s", idleAnimName);
			m_iIdleSequence = LookupSequence(vrIdleName);
			if (m_iIdleSequence >= 0)
			{
				DevMsg("VR: Idle sequence using VR override '%s' (seq %d) on model '%s'\n",
					vrIdleName, m_iIdleSequence, GetModelName());
			}
			else
			{
				m_iIdleSequence = LookupSequence(idleAnimName);
				DevMsg("VR: Idle sequence fallback to '%s' (seq %d) on model '%s'\n",
					idleAnimName, m_iIdleSequence, GetModelName());
			}
		}

		// Mutated Milk (item def 1121) has an animated bread creature driven by
		// the weapon model's own idle. Skip vm_weapon bone merge so it can play.
		m_bAnimateIdle = false;
		if (pWeapon->GetWeaponID() == TF_WEAPON_JAR_MILK)
		{
			CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
			if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1121)
				m_bAnimateIdle = true;
		}
		
		// Look up alt-fire (secondary attack) animation if one exists
		m_iAltFireSequence = -1;
		const char *altFireAnimName = GetWeaponAltFireAnimation(playerClass, weaponClass, pWeapon);
		if (altFireAnimName && altFireAnimName[0])
		{
			extern ConVar tfvr_weapon_fire_anim_debug;

			char vrAltFireName[128];
			Q_snprintf(vrAltFireName, sizeof(vrAltFireName), "vr_%s", altFireAnimName);
			m_iAltFireSequence = LookupSequence(vrAltFireName);

			if (m_iAltFireSequence >= 0)
			{
				if (tfvr_weapon_fire_anim_debug.GetBool())
				{
					DevMsg("VR: Alt-fire sequence using VR override '%s' (seq %d) on model '%s'\n",
						vrAltFireName, m_iAltFireSequence, GetModelName());
				}
			}
			else
			{
				m_iAltFireSequence = LookupSequence(altFireAnimName);
				if (tfvr_weapon_fire_anim_debug.GetBool())
				{
					DevMsg("VR: Alt-fire sequence fallback to '%s' (seq %d) on model '%s'\n",
						altFireAnimName, m_iAltFireSequence, GetModelName());
				}
			}
		}

		// Look up charge/pullback animation if one exists
		m_iChargeSequence = -1;
		m_iChargeSequence2 = -1;
		m_bPlayingChargeAnim = false;
		const char *chargeAnimName = GetWeaponChargeAnimation(playerClass, weaponClass, pWeapon);
		if (chargeAnimName && chargeAnimName[0])
		{
			extern ConVar tfvr_weapon_fire_anim_debug;

			m_iChargeSequence = LookupSequence(chargeAnimName);

			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Charge sequence '%s' (seq %d) on model '%s'\n",
					chargeAnimName, m_iChargeSequence, GetModelName());
			}
		}
		const char *chargeAnimName2 = GetWeaponChargeAnimation2(playerClass, weaponClass, pWeapon);
		if (chargeAnimName2 && chargeAnimName2[0])
		{
			extern ConVar tfvr_weapon_fire_anim_debug;

			m_iChargeSequence2 = LookupSequence(chargeAnimName2);

			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Charge sequence 2 '%s' (seq %d) on model '%s'\n",
					chargeAnimName2, m_iChargeSequence2, GetModelName());
			}
		}

		// Also pass fire sequence to render weapon (in case it has its own animations)
		pRenderWeapon->SetFireSequence(m_iFireSequence);
		
		// For medigun, also look up fire_on and fire_off sequences
		if (IsWeaponMedigun(pWeapon))
		{
			m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
			m_bMedigunWasHealing = false;
			
			// fire_on (healing beam starts)
			char vrName[128];
			Q_snprintf(vrName, sizeof(vrName), "vr_fire_on");
			m_iFireOnSequence = LookupSequence(vrName);
			if (m_iFireOnSequence < 0)
				m_iFireOnSequence = LookupSequence("fire_on");
			
			// fire_off (healing beam ends)
			Q_snprintf(vrName, sizeof(vrName), "vr_fire_off");
			m_iFireOffSequence = LookupSequence(vrName);
			if (m_iFireOffSequence < 0)
				m_iFireOffSequence = LookupSequence("fire_off");
			
			extern ConVar tfvr_weapon_fire_anim_debug;
			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Medigun fire sequences - fire_on: %d, fire_loop: %d, fire_off: %d\n",
					m_iFireOnSequence, m_iFireSequence, m_iFireOffSequence);
			}
			
			// Set up render weapon medigun sequences
			pRenderWeapon->SetupMedigunAnimations();
		}
	}
	
	// Set up idle animations for the weapon model
	pRenderWeapon->SetupAnimations();

	// For fist/glove weapons, override with the correct idle pose so the
	// vm_weapon_bone chain positions the mesh correctly.
	if (pWeapon->GetWeaponID() == TF_WEAPON_FISTS)
	{
		const char *fistsAnim = GetFistsIdleAnimName(pWeapon);
		int gloveIdleSeq = pRenderWeapon->LookupSequence(fistsAnim);
		if (gloveIdleSeq >= 0)
		{
			pRenderWeapon->SetSequence(gloveIdleSeq);
			pRenderWeapon->SetCycle(0.0f);
			pRenderWeapon->SetPlaybackRate(0.0f);
			DevMsg("VR: Fist idle sequence '%s' set to %d\n", fistsAnim, gloveIdleSeq);
		}
	}

	// Sync broken bodygroup for breakable melee weapons (bottle, sign, etc.)
	// so re-equipping an already-broken weapon shows the correct model
	if (pWeapon->IsBroken())
	{
		pRenderWeapon->SetBodygroup(0, 1);
	}
	
	// Copy attached models (festivizers, bot-killers, etc.) from the source weapon
	pRenderWeapon->CopyAttachedModels(pWeapon);
	
	// Sync particle effects (unusual effects, pipe smoke, etc.) from the source weapon
	pRenderWeapon->SyncParticleEffects();
	
	// CRITICAL: Disable interpolation so weapon follows hand without lag
	pRenderWeapon->SetPredictionEligible(false);
	
	// Set initial skin for team colors (will be updated each frame for crit effects, etc.)
	pRenderWeapon->m_nSkin = pWeapon->GetSkin();
	
	// VR: Attach extra wearables (bot-killer skulls, etc.) if the weapon has them
	// NOTE: Festivizers are handled via m_vecAttachedModels (copied in CopyAttachedModels above)
	C_TFWearable *pExtraWearable = pWeapon->m_hExtraWearable.Get();
	if (pExtraWearable)
	{
		pExtraWearable->FollowEntity(pRenderWeapon, true);
		pExtraWearable->ValidateModelIndex();
		pExtraWearable->UpdateVisibility();
		pExtraWearable->CreateShadow();
	}
	
	// Handle ExtraWearableViewModel for the current weapon (non-watch items)
	C_TFWearable *pExtraWearableVM = pWeapon->m_hExtraWearableViewModel.Get();
	if (pExtraWearableVM)
	{
		// Attach to weapon as normal (these are usually weapon attachments, not watches)
		pExtraWearableVM->FollowEntity(pRenderWeapon, true);
		pExtraWearableVM->UpdateVisibility();
		DevMsg("VR: ExtraWearableViewModel found on weapon, attaching to render weapon\n");
	}
	
	// Spy watch handling - the watch is on the INVIS weapon, not the knife/revolver
	// We need to find the spy's invis watch weapon and get its wearable (or create one)
	if (pOwner && pOwner->IsPlayerClass(TF_CLASS_SPY))
	{
		C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
		if (pLeftHand)
		{
			// Look for the invis watch weapon
			C_BaseCombatWeapon *pInvisWeapon = pOwner->Weapon_OwnsThisID(TF_WEAPON_INVIS);
			
			if (pInvisWeapon)
			{
				C_TFWeaponBase *pTFInvisWeapon = dynamic_cast<C_TFWeaponBase*>(pInvisWeapon);
				
				if (pTFInvisWeapon)
				{
					// First try to use the networked wearable
					C_TFWearable *pWatchWearable = pTFInvisWeapon->m_hExtraWearableViewModel.Get();
					
					if (pWatchWearable)
					{
						pLeftHand->AttachWatchToLeftHand(pWatchWearable);
					}
					else
					{
						// Networked wearable not available - try to get model from item schema
						CEconItemView *pItem = pTFInvisWeapon->GetAttributeContainer()->GetItem();
						const char *pszWatchModel = NULL;
						
						if (pItem && pItem->IsValid())
						{
							pszWatchModel = pItem->GetExtraWearableViewModel();
						}
						
						// Use default spy watch model if item doesn't define one
						if (!pszWatchModel || !pszWatchModel[0])
						{
							pszWatchModel = "models/weapons/c_models/c_spy_watch/c_spy_watch.mdl";
						}
						
						// Create our own watch model on the left hand
						pLeftHand->CreateWatchModel(pszWatchModel);
					}
				}
			}
		}
	}
	
	// Handle Scout ball weapons (Sandman, Wrap Assassin)
	// The ball model is shown on the LEFT hand when ammo is available
	int iWeaponID = pWeapon->GetWeaponID();
	if (iWeaponID == TF_WEAPON_BAT_WOOD || iWeaponID == TF_WEAPON_BAT_GIFTWRAP)
	{
		C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
		if (pLeftHand)
		{
			// Initialize ball tracking - actual ball creation happens in UpdateLeftHandBall
			pLeftHand->m_iLastBallAmmo = -1;  // Force update on next frame
		}
	}
	
	// NOTE: attach_to_hands weapons (Heavy boxing gloves, etc.) contain BOTH hands
	// in a single model mesh. They bone-merge with the hand model, so we don't
	// need to create a duplicate - the single render weapon shows both gloves.
	
	// Re-parent stat-trak addons to VR render weapon if they exist
	if (pWeapon->m_viewmodelStatTrakAddon.Get())
	{
		pWeapon->m_viewmodelStatTrakAddon->FollowEntity(pRenderWeapon, true);
	}
	if (pWeapon->m_worldmodelStatTrakAddon.Get())
	{
		pWeapon->m_worldmodelStatTrakAddon->FollowEntity(pRenderWeapon, true);
	}
	
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
	
	// Reset animation state and force idle pose
	m_bPlayingFireAnim = false;
	if (m_iIdleSequence >= 0)
	{
		SetSequence(m_iIdleSequence);
		SetCycle(0.0f);
		SetPlaybackRate(0.0f);
	}
	
	// Force snap to new pose - disable all interpolation
	ResetLatched();
	InvalidateBoneCache();
	
	// Additional interpolation reset
	m_flAnimTime = gpGlobals->curtime;
	m_flSimulationTime = gpGlobals->curtime;
	
	// Disable interpolation temporarily by adding EF_NOINTERP
	AddEffects(EF_NOINTERP);
	
	// Sample the idle animation DIRECTLY using IBoneSetup
	// This bypasses any entity animation state and gives us the pure idle pose
	CStudioHdr *pStudioHdr = GetModelPtr();
	if (pStudioHdr && m_iHandBone >= 0 && m_iIdleSequence >= 0)
	{
		int numBones = pStudioHdr->numbones();
		if (m_iHandBone < numBones)
		{
			// Initialize pose parameters
			float poseParameters[MAXSTUDIOPOSEPARAM];
			memset(poseParameters, 0, sizeof(poseParameters));
			
			// Sample the idle animation at cycle 0
			IBoneSetup boneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters);
			
			Vector posAnim[MAXSTUDIOBONES];
			Quaternion qAnim[MAXSTUDIOBONES];
			for (int i = 0; i < MAXSTUDIOBONES; i++)
			{
				posAnim[i].Init();
				qAnim[i].Init(0, 0, 0, 1);
			}
			boneSetup.InitPose(posAnim, qAnim);
			boneSetup.AccumulatePose(posAnim, qAnim, m_iIdleSequence, 0.0f, 1.0f, gpGlobals->curtime, NULL);
			
			// Cache the hand bone's LOCAL position (parent-space) from the idle animation.
			// Used to suppress position changes during medigun fire animations.
			m_vecIdleHandBoneLocalPos = posAnim[m_iHandBone];
			
			// Build world-space matrix for hand bone
			// Need to walk up hierarchy to get correct world transform
			matrix3x4_t boneToWorld[MAXSTUDIOBONES];
			for (int i = 0; i < numBones; i++)
			{
				matrix3x4_t boneToParent;
				QuaternionMatrix(qAnim[i], posAnim[i], boneToParent);
				
				const mstudiobone_t *pBone = pStudioHdr->pBone(i);
				if (!pBone)
				{
					SetIdentityMatrix(boneToWorld[i]);
					continue;
				}
				
				if (pBone->parent == -1)
					MatrixCopy(boneToParent, boneToWorld[i]);
				else if (pBone->parent >= 0 && pBone->parent < numBones)
					ConcatTransforms(boneToWorld[pBone->parent], boneToParent, boneToWorld[i]);
				else
					SetIdentityMatrix(boneToWorld[i]);
			}
			
			// Cache the LOCAL hand bone transform (this is relative to model origin)
			// Since we sampled the animation at origin, the bone transform IS the local offset
			MatrixCopy(boneToWorld[m_iHandBone], m_matIdleHandBoneTransform);
			m_bHandBoneOffsetValid = true;
			
			if (tfvr_hands_debug.GetBool())
			{
				Vector pos;
				QAngle angles;
				MatrixAngles(m_matIdleHandBoneTransform, angles, pos);
				DevMsg("VR Hand: Sampled idle pose directly - hand bone pos: (%.1f, %.1f, %.1f)\n", pos.x, pos.y, pos.z);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Unequip the currently held weapon
//-----------------------------------------------------------------------------
void C_TFVRHand::UnequipWeapon()
{
	// Invalidate cached state
	m_bAimRefValid = false;
	m_bIdleMuzzleOffsetValid = false;
	m_iCachedMuzzleWeaponID = -1;
	m_iOffHandBone = -1;
	m_iOffHandMiddleFingerBone = -1;
	
	// Reset animation state so next weapon/grip uses fresh lookups
	m_iIdleSequence = -1;
	m_bAnimateIdle = false;
	m_iFireSequence = -1;
	m_iAltFireSequence = -1;
	m_iChargeSequence = -1;
	m_iChargeSequence2 = -1;
	m_bPlayingFireAnim = false;
	m_bPlayingChargeAnim = false;
	m_flFireAnimStartTime = 0.0f;
	m_bHandBoneOffsetValid = false;
	m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
	m_iFireOnSequence = -1;
	m_iFireOffSequence = -1;
	m_bMedigunWasHealing = false;
	m_bFlamethrowerWasFiring = false;
	m_flFlamethrowerFireBlend = 0.0f;

	// Reset two-hand grip state
	m_flTwoHandBlend = 0.0f;
	m_bOffhandGripActive = false;
	m_bWasOffhandGripActive = false;
	m_flGripRotationBlend = 0.0f;
	
	// Clean up left hand wearables when right hand unequips weapon
	if (IsRightHand())
	{
		C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
		if (pLeftHand)
		{
			pLeftHand->RemoveLeftHandWatch();
			pLeftHand->RemoveLeftHandBall();
		}
	}
	
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	
	// VR: Reattach extra wearables back to the original weapon before cleaning up
	// This ensures festivizers/bot-killer skulls follow the weapon when it's dropped or switched
	if (pWeapon && m_hRenderWeapon.Get())
	{
		C_TFWearable *pExtraWearable = pWeapon->m_hExtraWearable.Get();
		if (pExtraWearable)
		{
			// Re-parent to the original weapon (with bonemerge for proper attachment)
			pExtraWearable->FollowEntity(pWeapon, true);
		}
		
		C_TFWearable *pExtraWearableVM = pWeapon->m_hExtraWearableViewModel.Get();
		if (pExtraWearableVM)
		{
			pExtraWearableVM->FollowEntity(pWeapon, true);
		}
	}
	
	// Clean up crit boost effect on the hand
	if (m_pCritBoostEffect.IsValid())
	{
		m_pCritBoostEffect->StopEmission();
		m_pCritBoostEffect = NULL;
	}
	m_bCritBoostActive = false;
	
	// Clean up render weapon
	if (m_hRenderWeapon.Get())
	{
		// Stop any particle effects on the render weapon before releasing it
		C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
		if (pRenderWeapon)
		{
			pRenderWeapon->StopParticleEffects();
		}
		m_hRenderWeapon->Release();
		m_hRenderWeapon = NULL;
	}
	
	// Reset off-hand bone lookup for next weapon
	m_iOffHandBone = -1;
	m_iOffHandMiddleFingerBone = -1;
	
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

// Debug output for skin issues
static ConVar tfvr_debug_skins("tfvr_debug_skins", "0", FCVAR_NONE, "Debug VR hand/weapon skin changes");

//-----------------------------------------------------------------------------
// Purpose: Update skins for hands and weapons based on team, crit state, etc.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateSkins()
{
	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (!pOwner)
		return;
	
	int iTeamNumber = pOwner->GetTeamNumber();
	
	// Determine hand skin based on team
	// TF2 hand models typically use skin 0 for RED, skin 1 for BLU
	int nHandSkin = (iTeamNumber == TF_TEAM_BLUE) ? 1 : 0;
	
	// Apply hand skin if changed
	if (m_nSkin != nHandSkin)
	{
		if (tfvr_debug_skins.GetBool())
		{
			Msg("VR Hand (%s): Changing skin from %d to %d (team=%d, TF_TEAM_RED=%d, TF_TEAM_BLUE=%d)\n",
				IsLeftHand() ? "LEFT" : "RIGHT", m_nSkin, nHandSkin, iTeamNumber, TF_TEAM_RED, TF_TEAM_BLUE);
		}
		m_nSkin = nHandSkin;
	}
	
	// Update render weapon skin
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
	
	if (pRenderWeapon && pHeldWeapon)
	{
		// Get the weapon's proper skin (handles team colors, item skins, etc.)
		int nWeaponSkin = pHeldWeapon->GetSkin();
		
		// Apply to render weapon if changed
		if (pRenderWeapon->m_nSkin != nWeaponSkin)
		{
			pRenderWeapon->m_nSkin = nWeaponSkin;
		}

		// Sync broken bodygroup for breakable melee weapons (bottle, sign, etc.)
		// The server sets m_bBroken on crit hits; we mirror it to the VR render weapon
		int iDesiredBody = pHeldWeapon->IsBroken() ? 1 : 0;
		if (pRenderWeapon->GetBody() != iDesiredBody)
		{
			pRenderWeapon->SetBodygroup(0, iDesiredBody);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Update crit boost effect on the hand/weapon
//          This is attached to the HAND for proper update timing (no frame lag)
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateCritBoostEffect()
{
	C_TFPlayer *pPlayer = m_hOwnerPlayer.Get();
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	
	if (!pPlayer || !pWeapon)
	{
		// No player or weapon, remove effect if present
		if (m_pCritBoostEffect.IsValid())
		{
			m_pCritBoostEffect->StopEmission();
			m_pCritBoostEffect = NULL;
			m_bCritBoostActive = false;
		}
		return;
	}
	
	// Check if we should display crit boost effect
	bool bShouldDisplay = pPlayer->m_Shared.IsCritBoosted()
		|| pPlayer->m_Shared.InCond(TF_COND_ENERGY_BUFF)
		|| pPlayer->m_Shared.InCond(TF_COND_SNIPERCHARGE_RAGE_BUFF);
	
	// Check if weapon can be crit boosted
	bShouldDisplay &= pWeapon->CanBeCritBoosted();
	
	// Never show crit boost effects when stealthed
	bShouldDisplay &= !pPlayer->m_Shared.IsStealthed();
	
	// If effect exists and should stay, nothing to do here
	// Position is corrected in PositionWeaponFromBones via SetControlPoint
	if (bShouldDisplay && m_bCritBoostActive && m_pCritBoostEffect.IsValid())
	{
		return;
	}
	
	// Only do create/destroy work if state changed
	if (bShouldDisplay == m_bCritBoostActive)
		return;
	
	m_bCritBoostActive = bShouldDisplay;
	
	// Remove effect if we shouldn't display it
	if (!bShouldDisplay)
	{
		if (m_pCritBoostEffect.IsValid())
		{
			m_pCritBoostEffect->StopEmission();
			m_pCritBoostEffect = NULL;
		}
		return;
	}
	
	const char *pEffectName = (pPlayer->GetTeamNumber() == TF_TEAM_RED) 
		? "critgun_weaponmodel_red" 
		: "critgun_weaponmodel_blu";
	
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	if (pRenderWeapon)
	{
		m_pCritBoostEffect = pRenderWeapon->ParticleProp()->Create(pEffectName, PATTACH_ABSORIGIN_FOLLOW);
	}
	else
	{
		m_pCritBoostEffect = ParticleProp()->Create(pEffectName, PATTACH_CUSTOMORIGIN);
		if (m_pCritBoostEffect.IsValid())
		{
			m_pCritBoostEffect->SetControlPoint(0, m_vecLastValidPosition);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Drive medigun fire animations based on healing state
//          fire_on -> fire_loop (while healing) -> fire_off -> idle
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateMedigunFireAnimation()
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || !IsWeaponMedigun(pWeapon))
	{
		if (m_eMedigunFireState != MEDIGUN_FIRE_IDLE)
		{
			m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
			m_bMedigunWasHealing = false;
			m_bPlayingFireAnim = false;
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(0.0f);
			}
		}
		return;
	}
	
	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;
	
	if (!tfvr_weapon_fire_anim.GetBool())
		return;
	
	// Check medigun healing state via public m_hHealingTarget
	CWeaponMedigun *pMedigun = static_cast<CWeaponMedigun*>(pWeapon);
	bool bIsHealing = (pMedigun->m_hHealingTarget.Get() != NULL);
	
	C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
	
	switch (m_eMedigunFireState)
	{
	case MEDIGUN_FIRE_IDLE:
		if (bIsHealing)
		{
			if (m_iFireOnSequence >= 0)
			{
				SetSequence(m_iFireOnSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_bPlayingFireAnim = true;
				m_flFireAnimStartTime = gpGlobals->curtime;
				m_eMedigunFireState = MEDIGUN_FIRE_ON;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_ON);
			}
			else if (m_iFireSequence >= 0)
			{
				SetSequence(m_iFireSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_bPlayingFireAnim = true;
				m_flFireAnimStartTime = gpGlobals->curtime;
				m_eMedigunFireState = MEDIGUN_FIRE_LOOP;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_LOOP);
			}
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR Medigun: Healing started - state -> %s\n",
					m_eMedigunFireState == MEDIGUN_FIRE_ON ? "FIRE_ON" : "FIRE_LOOP");
			}
		}
		break;
		
	case MEDIGUN_FIRE_ON:
		if (!bIsHealing)
		{
			// Healing stopped during fire_on - transition to fire_off
			if (m_iFireOffSequence >= 0)
			{
				SetSequence(m_iFireOffSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_flFireAnimStartTime = gpGlobals->curtime;
				m_eMedigunFireState = MEDIGUN_FIRE_OFF;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_OFF);
			}
			else
			{
				if (m_iIdleSequence >= 0)
				{
					SetSequence(m_iIdleSequence);
					SetCycle(0.0f);
					SetPlaybackRate(0.0f);
				}
				m_bPlayingFireAnim = false;
				m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_IDLE);
			}
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
				DevMsg("VR Medigun: Healing stopped during fire_on - state -> %s\n",
					m_eMedigunFireState == MEDIGUN_FIRE_OFF ? "FIRE_OFF" : "IDLE");
		}
		else if (GetCycle() >= 1.0f)
		{
			// fire_on completed, transition to fire_loop
			if (m_iFireSequence >= 0)
			{
				SetSequence(m_iFireSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_eMedigunFireState = MEDIGUN_FIRE_LOOP;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_LOOP);
			}
			else
			{
				m_eMedigunFireState = MEDIGUN_FIRE_LOOP;
			}
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
				DevMsg("VR Medigun: fire_on completed - state -> FIRE_LOOP\n");
		}
		break;
		
	case MEDIGUN_FIRE_LOOP:
		if (!bIsHealing)
		{
			// Healing stopped - play fire_off
			if (m_iFireOffSequence >= 0)
			{
				SetSequence(m_iFireOffSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_flFireAnimStartTime = gpGlobals->curtime;
				m_eMedigunFireState = MEDIGUN_FIRE_OFF;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_OFF);
			}
			else
			{
				if (m_iIdleSequence >= 0)
				{
					SetSequence(m_iIdleSequence);
					SetCycle(0.0f);
					SetPlaybackRate(0.0f);
				}
				m_bPlayingFireAnim = false;
				m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_IDLE);
			}
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
				DevMsg("VR Medigun: Healing stopped - state -> %s\n",
					m_eMedigunFireState == MEDIGUN_FIRE_OFF ? "FIRE_OFF" : "IDLE");
		}
		// fire_loop loops automatically via Source engine sequence flags
		break;
		
	case MEDIGUN_FIRE_OFF:
		if (bIsHealing)
		{
			// Healing restarted during fire_off - go back to fire_on
			if (m_iFireOnSequence >= 0)
			{
				SetSequence(m_iFireOnSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_flFireAnimStartTime = gpGlobals->curtime;
				m_eMedigunFireState = MEDIGUN_FIRE_ON;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_ON);
			}
			else if (m_iFireSequence >= 0)
			{
				SetSequence(m_iFireSequence);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_flFireAnimStartTime = gpGlobals->curtime;
				m_eMedigunFireState = MEDIGUN_FIRE_LOOP;
				InvalidateBoneCache();
				
				if (pRenderWeapon)
					pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_LOOP);
			}
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
				DevMsg("VR Medigun: Healing restarted during fire_off - state -> %s\n",
					m_eMedigunFireState == MEDIGUN_FIRE_ON ? "FIRE_ON" : "FIRE_LOOP");
		}
		else if (GetCycle() >= 1.0f || (gpGlobals->curtime - m_flFireAnimStartTime) > 2.0f)
		{
			// fire_off completed - return to idle
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(0.0f);
			}
			m_bPlayingFireAnim = false;
			m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
			InvalidateBoneCache();
			
			if (pRenderWeapon)
				pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_IDLE);
			
			if (tfvr_weapon_fire_anim_debug.GetBool())
				DevMsg("VR Medigun: fire_off completed - state -> IDLE\n");
		}
		break;
	}
	
	m_bMedigunWasHealing = bIsHealing;
}

//-----------------------------------------------------------------------------
// Purpose: Drive flamethrower fire animation based on weapon fire state.
//          The flamethrower overrides PrimaryAttack() and never calls
//          FireProjectile()/DoFireEffects(), so we poll m_iWeaponState
//          each frame and play/hold/stop the fire animation accordingly.
//          ft_fire loops while firing, returns to ft_idle when stopped.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateFlamethrowerFireAnimation()
{
	const float flEaseInTime = 0.16f;
	const float flEaseOutTime = 0.16f;

	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || !IsWeaponFlamethrower(pWeapon))
	{
		if (m_flFlamethrowerFireBlend > 0.0f || m_bFlamethrowerWasFiring)
		{
			m_bFlamethrowerWasFiring = false;
			m_flFlamethrowerFireBlend = 0.0f;
			m_bPlayingFireAnim = false;
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(0.0f);
			}
		}
		return;
	}

	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;

	if (!tfvr_weapon_fire_anim.GetBool())
		return;

	CTFFlameThrower *pFlameThrower = static_cast<CTFFlameThrower*>(pWeapon);
	int nWeaponState = pFlameThrower->GetWeaponState();
	bool bIsFiring = (nWeaponState == FT_STATE_FIRING ||
	                  nWeaponState == FT_STATE_STARTFIRING);
	bool bIsAirblasting = (nWeaponState == FT_STATE_SECONDARY);

	if (bIsFiring && !m_bFlamethrowerWasFiring)
	{
		// Just started firing - set fire sequence, blend will ramp up
		if (m_iFireSequence >= 0)
		{
			SetSequence(m_iFireSequence);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
			m_bPlayingFireAnim = true;
			m_flFireAnimStartTime = gpGlobals->curtime;
			InvalidateBoneCache();

			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR Flamethrower: Started firing - playing fire anim (seq %d)\n", m_iFireSequence);
			}

			C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
			if (pRenderWeapon)
				pRenderWeapon->PlayFireAnimation();
		}
	}
	else if (bIsFiring && m_bPlayingFireAnim)
	{
		if (GetCycle() >= 1.0f)
		{
			SetCycle(0.0f);
		}
	}
	else if (!bIsFiring && !bIsAirblasting && m_bFlamethrowerWasFiring)
	{
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR Flamethrower: Stopped firing - blending out (blend %.2f)\n", m_flFlamethrowerFireBlend);
		}
	}

	// During airblast, don't touch sequences -- PlayWeaponAltFireAnimation
	// owns the animation state and the generic completion check handles returning to idle.
	// Reset blend so it doesn't block the generic one-shot completion check.
	if (bIsAirblasting)
	{
		m_bFlamethrowerWasFiring = false;
		m_flFlamethrowerFireBlend = 0.0f;
		return;
	}

	// Ramp blend weight toward target
	float flDt = gpGlobals->frametime;
	if (bIsFiring)
	{
		m_flFlamethrowerFireBlend += flDt / flEaseInTime;
	}
	else
	{
		m_flFlamethrowerFireBlend -= flDt / flEaseOutTime;
	}
	m_flFlamethrowerFireBlend = clamp(m_flFlamethrowerFireBlend, 0.0f, 1.0f);

	// Keep fire sequence active while blending out so SetupBones can sample it.
	// Don't touch anything if alt-fire (airblast) is playing.
	if (m_flFlamethrowerFireBlend > 0.0f && GetSequence() != m_iAltFireSequence)
	{
		m_bPlayingFireAnim = true;
		if (!bIsFiring && m_iFireSequence >= 0 && GetSequence() != m_iFireSequence)
		{
			SetSequence(m_iFireSequence);
			SetCycle(0.0f);
			SetPlaybackRate(0.0f);
		}
	}
	else if (!bIsFiring && m_bPlayingFireAnim
		&& GetSequence() != m_iAltFireSequence)
	{
		// Blend-out complete, fully return to idle.
		// Skip if alt-fire (airblast) is playing -- let the generic completion handle it.
		m_bPlayingFireAnim = false;
		if (m_iIdleSequence >= 0)
		{
			SetSequence(m_iIdleSequence);
			SetCycle(0.0f);
			SetPlaybackRate(0.0f);
		}
		InvalidateBoneCache();
	}

	m_bFlamethrowerWasFiring = bIsFiring;
}

//-----------------------------------------------------------------------------
// Purpose: Trigger fire animation on the hand (animates fingers during firing)
//-----------------------------------------------------------------------------
void C_TFVRHand::PlayWeaponFireAnimation()
{
	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;
	
	if (!tfvr_weapon_fire_anim.GetBool())
		return;
	
	int sequenceToPlay = m_iFireSequence;
	
	// Check if this is a melee weapon with swing cycling
	if (m_iMeleeSwingCount > 0 && m_szMeleeSwingBase[0] != '\0')
	{
		// Build the animation name with the current swing variant
		static const char *swingVariants[] = { "a", "b", "c" };
		char animName[128];
		
		// Check if this attack is a crit
		C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
		bool bIsCrit = pWeapon && pWeapon->IsCurrentAttackACrit();
		
		// Build animation name: base + variant (+ crit suffix if applicable)
		// e.g., "b_swing_a" or "b_swing_a_crit" (if crit animations exist)
		int swingVariant = m_iMeleeSwingIndex % m_iMeleeSwingCount;
		V_snprintf(animName, sizeof(animName), "%s%s", m_szMeleeSwingBase, swingVariants[swingVariant]);
		
		// Look up the sequence
		int swingSequence = LookupSequence(animName);
		
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Melee swing - base: '%s', variant: %d, anim: '%s', seq: %d, crit: %d\n", 
				m_szMeleeSwingBase, swingVariant, animName, swingSequence, bIsCrit);
		}
		
		// If this is a crit, try to find a crit variant
		if (bIsCrit)
		{
			char critAnimName[128];
			V_snprintf(critAnimName, sizeof(critAnimName), "%s%s_crit", m_szMeleeSwingBase, swingVariants[swingVariant]);
			int critSequence = LookupSequence(critAnimName);
			if (critSequence >= 0)
			{
				swingSequence = critSequence;
				if (tfvr_weapon_fire_anim_debug.GetBool())
				{
					DevMsg("VR: Using crit swing animation '%s' (seq %d)\n", critAnimName, critSequence);
				}
			}
			else if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Crit swing animation '%s' not found, using normal swing\n", critAnimName);
			}
		}
		
		if (swingSequence >= 0)
		{
			sequenceToPlay = swingSequence;
		}
		else if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Melee swing animation '%s' not found, using default fire sequence %d\n", animName, m_iFireSequence);
		}
		
		// Cycle to next swing variant for next attack
		m_iMeleeSwingIndex = (m_iMeleeSwingIndex + 1) % m_iMeleeSwingCount;
	}
	
	if (sequenceToPlay < 0)
	{
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: No fire animation sequence set for this hand\n");
		}
		return;
	}
	
	// Play the fire animation on the HAND model (stops any active charge anim)
	m_bPlayingChargeAnim = false;
	SetSequence(sequenceToPlay);
	SetCycle(0.0f);
	SetPlaybackRate(1.0f);
	m_bPlayingFireAnim = true;
	m_flFireAnimStartTime = gpGlobals->curtime;
	
	// Force animation frame advance to start immediately
	InvalidateBoneCache();
	
	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Playing fire animation on hand (sequence %d) at time %.2f\n", 
			sequenceToPlay, gpGlobals->curtime);
	}
	
	// Also trigger animation on the render weapon (if it has one)
	C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
	if (pRenderWeapon)
	{
		pRenderWeapon->PlayFireAnimation();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Trigger alt-fire (secondary attack) animation on the hand
//          Used for flamethrower airblast, etc.
//-----------------------------------------------------------------------------
void C_TFVRHand::PlayWeaponAltFireAnimation()
{
	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;

	if (!tfvr_weapon_fire_anim.GetBool())
		return;

	if (m_iAltFireSequence < 0)
	{
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: No alt-fire animation sequence set for this hand, falling back to primary fire\n");
		}
		PlayWeaponFireAnimation();
		return;
	}

	SetSequence(m_iAltFireSequence);
	SetCycle(0.0f);
	SetPlaybackRate(1.0f);
	m_bPlayingFireAnim = true;
	m_flFireAnimStartTime = gpGlobals->curtime;

	InvalidateBoneCache();

	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Playing alt-fire animation on hand (sequence %d) at time %.2f\n",
			m_iAltFireSequence, gpGlobals->curtime);
	}

	C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
	if (pRenderWeapon)
	{
		pRenderWeapon->PlayFireAnimation();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Play charge/pullback animation (looping) while weapon is charging
//-----------------------------------------------------------------------------
void C_TFVRHand::PlayWeaponChargeAnimation()
{
	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;

	if (!tfvr_weapon_fire_anim.GetBool())
		return;

	if (m_iChargeSequence < 0)
	{
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: No charge animation sequence set for this hand\n");
		}
		return;
	}

	if (m_bPlayingChargeAnim)
		return;

	SetSequence(m_iChargeSequence);
	SetCycle(0.0f);
	SetPlaybackRate(1.0f);
	m_bPlayingChargeAnim = true;
	m_bPlayingFireAnim = false;

	InvalidateBoneCache();

	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Playing charge animation on hand (sequence %d)\n", m_iChargeSequence);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Play second-phase charge animation (e.g. huntsman max-charge shake)
//-----------------------------------------------------------------------------
void C_TFVRHand::PlayWeaponChargeAnimation2()
{
	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;

	if (!tfvr_weapon_fire_anim.GetBool())
		return;

	if (m_iChargeSequence2 < 0)
		return;

	SetSequence(m_iChargeSequence2);
	SetCycle(0.0f);
	SetPlaybackRate(1.0f);
	m_bPlayingChargeAnim = true;
	m_bPlayingFireAnim = false;

	InvalidateBoneCache();

	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Playing charge phase 2 animation on hand (sequence %d)\n", m_iChargeSequence2);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Stop charge animation and return to idle
//-----------------------------------------------------------------------------
void C_TFVRHand::StopWeaponChargeAnimation()
{
	if (!m_bPlayingChargeAnim)
		return;

	m_bPlayingChargeAnim = false;

	if (m_iIdleSequence >= 0)
	{
		SetSequence(m_iIdleSequence);
		SetCycle(0.0f);
		SetPlaybackRate(0.0f);
	}

	InvalidateBoneCache();

	extern ConVar tfvr_weapon_fire_anim_debug;
	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Stopped charge animation, returning to idle\n");
	}
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

//=============================================================================
// LEFT HAND WEARABLES - Spy watches, Scout balls, etc.
//=============================================================================

//-----------------------------------------------------------------------------
// Purpose: Attach a spy watch wearable to the left hand
//          Called from right hand's EquipWeapon when spy has cloak weapon
//-----------------------------------------------------------------------------
void C_TFVRHand::AttachWatchToLeftHand(C_BaseAnimating *pWatch)
{
	if (!IsLeftHand() || !pWatch)
		return;
	
	// Remove any existing watch
	RemoveLeftHandWatch();
	
	// Check if watch has weapon_bone_L for bone merging
	int iWatchBone = pWatch->LookupBone("weapon_bone_L");
	int iHandBone = LookupBone("weapon_bone_L");
	
	if (iWatchBone >= 0 && iHandBone >= 0)
	{
		// Watch has matching bone - use bone merge
		pWatch->FollowEntity(this, true);
		pWatch->AddEffects(EF_BONEMERGE);
	}
	else
	{
		// No matching bone - try to parent to attachment or use fallback
		int iAttach = LookupAttachment("weapon_bone_L");
		if (iAttach > 0)
		{
			pWatch->SetParent(this, iAttach);
		}
		else
		{
			// Fall back to bone merge
			pWatch->FollowEntity(this, true);
			pWatch->AddEffects(EF_BONEMERGE);
		}
	}
	
	pWatch->RemoveEffects(EF_NODRAW);
	pWatch->UpdateVisibility();
	
	m_hLeftHandWatch = pWatch;
	m_bOwnWatchModel = false;  // We don't own this - the weapon system does
}

//-----------------------------------------------------------------------------
// Purpose: Remove the spy watch from the left hand
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveLeftHandWatch()
{
	if (m_hLeftHandWatch.Get())
	{
		if (m_bOwnWatchModel)
		{
			// We created this model - delete it
			m_hLeftHandWatch->Release();
		}
		// else: The weapon system owns this wearable, don't delete
		
		m_hLeftHandWatch = NULL;
		m_bOwnWatchModel = false;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Create our own watch model (when networked wearable isn't available)
//-----------------------------------------------------------------------------
void C_TFVRHand::CreateWatchModel(const char *pszWatchModel)
{
	if (!IsLeftHand() || !pszWatchModel || !pszWatchModel[0])
		return;
	
	// Remove any existing watch
	RemoveLeftHandWatch();
	
	// Create new watch entity - use C_BaseAnimating (NOT CTFViewModel which is hidden in VR)
	C_BaseAnimating *pWatch = new C_BaseAnimating();
	if (!pWatch)
		return;
	
	if (!pWatch->InitializeAsClientEntity(pszWatchModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		pWatch->Release();
		return;
	}
	
	// Set owner for material proxies (cloak, etc.)
	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
	{
		pWatch->m_nSkin = (pOwner->GetTeamNumber() == TF_TEAM_RED) ? 0 : 1;
		pWatch->SetOwnerEntity(pOwner);
	}
	
	// Use FollowEntity with bone merge
	pWatch->FollowEntity(this, true);
	pWatch->AddEffects(EF_BONEMERGE | EF_BONEMERGE_FASTCULL);
	pWatch->SetMoveType(MOVETYPE_NONE);
	pWatch->AddSolidFlags(FSOLID_NOT_SOLID);
	
	m_hLeftHandWatch = pWatch;
	m_bOwnWatchModel = true;  // We own this, need to delete on cleanup
}

//-----------------------------------------------------------------------------
// Purpose: Create and attach a ball model to the left hand (Sandman/Wrap Assassin)
//-----------------------------------------------------------------------------
void C_TFVRHand::AttachBallToLeftHand(const char *pszBallModel)
{
	if (!IsLeftHand() || !pszBallModel || !pszBallModel[0])
		return;
	
	// Remove any existing ball
	RemoveLeftHandBall();
	
	// Create the ball entity - use C_BaseAnimating (NOT CTFViewModel which is hidden in VR)
	C_BaseAnimating *pBall = new C_BaseAnimating();
	if (!pBall)
		return;
	
	if (!pBall->InitializeAsClientEntity(pszBallModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		pBall->Release();
		return;
	}
	
	// Set skin based on team
	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
	{
		pBall->m_nSkin = (pOwner->GetTeamNumber() == TF_TEAM_BLUE) ? 1 : 0;
		pBall->SetOwnerEntity(pOwner);
	}
	
	// Use FollowEntity with bone merge
	pBall->FollowEntity(this, true);
	pBall->AddEffects(EF_BONEMERGE | EF_BONEMERGE_FASTCULL);
	pBall->SetMoveType(MOVETYPE_NONE);
	pBall->AddSolidFlags(FSOLID_NOT_SOLID);
	
	m_hLeftHandBall = pBall;
}

//-----------------------------------------------------------------------------
// Purpose: Remove the ball model from the left hand
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveLeftHandBall()
{
	if (m_hLeftHandBall.Get())
	{
		m_hLeftHandBall->Release();
		m_hLeftHandBall = NULL;
	}
	m_iLastBallAmmo = -1;
}

//-----------------------------------------------------------------------------
// Purpose: Update left hand ball visibility and position based on ammo
//          Called each frame for scout with Sandman/Wrap Assassin
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateLeftHandBall()
{
	if (!IsLeftHand())
		return;
	
	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (!pOwner)
		return;
	
	// Get the right hand to check what weapon is equipped
	C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
	if (!pRightHand)
		return;
	
	C_TFWeaponBase *pWeapon = pRightHand->GetHeldWeapon();
	if (!pWeapon)
	{
		if (m_hLeftHandBall.Get())
			RemoveLeftHandBall();
		return;
	}
	
	// Check if weapon is Sandman or Wrap Assassin
	int iWeaponID = pWeapon->GetWeaponID();
	bool bIsBallWeapon = (iWeaponID == TF_WEAPON_BAT_WOOD || iWeaponID == TF_WEAPON_BAT_GIFTWRAP);
	
	if (!bIsBallWeapon)
	{
		if (m_hLeftHandBall.Get())
			RemoveLeftHandBall();
		return;
	}
	
	// Check ammo (TF_AMMO_GRENADES1 is used for the ball)
	int iBallAmmo = pOwner->GetAmmoCount(TF_AMMO_GRENADES1);
	
	// Only update if ammo changed
	if (iBallAmmo == m_iLastBallAmmo)
		return;
	
	m_iLastBallAmmo = iBallAmmo;
	
	if (iBallAmmo > 0)
	{
		// Has ball - make sure it's visible
		if (!m_hLeftHandBall.Get())
		{
			// Create the ball from weapon's model
			CTFBat_Wood *pBat = dynamic_cast<CTFBat_Wood*>(pWeapon);
			if (pBat)
			{
				const char *pszBallModel = pBat->GetBallViewModelName();
				if (pszBallModel && pszBallModel[0])
				{
					AttachBallToLeftHand(pszBallModel);
				}
			}
		}
		else
		{
			m_hLeftHandBall->RemoveEffects(EF_NODRAW);
		}
	}
	else
	{
		// No ball - hide it
		if (m_hLeftHandBall.Get())
		{
			m_hLeftHandBall->AddEffects(EF_NODRAW);
		}
	}
}

