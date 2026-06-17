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
#include "tf/tf_weapon_knife.h"
#include "tf/tf_weapon_shotgun.h"
#include "tf/tf_weapon_pistol.h"
#include "tf/tf_weapon_rocketlauncher.h"
#include "tf/tf_weapon_pipebomblauncher.h"
#include "tf/tf_weapon_compound_bow.h"
#include "tf/tf_weapon_raygun.h"
#include "tf/tf_weapon_particle_cannon.h"
#include "tf/tf_weaponbase_melee.h"
#include "c_baseviewmodel.h"
#include "tf/tf_item_wearable.h"
#include "tf/tf_wearable_weapons.h"
#include "econ/econ_entity.h"
#include "econ/econ_item_schema.h"
#include "model_types.h"
#include "particles_new.h"
#include "tfvr/openxr_manager.h"
#include "tfvr/openxr_hand_tracking.h"
#include "tfvr/tfvr_weapon_base.h"
#include "tfvr/c_tfvr_weapon_magazine.h"
#include "cliententitylist.h"
#include "bone_setup.h"
#include "engine/ivdebugoverlay.h"
#include "filesystem.h"
#include "econ/ihasowner.h"
#include "tfvr/vr_hand_render.h"
#include "cl_animevent.h"
#include "eventlist.h"
#include <vgui/IVGui.h>
#include <vgui_controls/EditablePanel.h>
#include <vgui_controls/ProgressBar.h>
#include "ienginevgui.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include "iclientmode.h"
#include "materialsystem/imaterialsystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Forward declarations for Bread Bite variant selection (defined with fists helpers below)
static int GetBreadBiteIdleVariant();
static int GetBreadBiteDrawVariant();
static const char *TFVR_GetRenderableModelName(C_BaseAnimating *pRenderable);
static bool TFVR_ValidateHandRenderable(C_BaseAnimating *pRenderable, const char *pszLabel);

// Left-handed mode: reflect a set of finished world-space bones across the
// plane through the given controller frame (negates the controller-local
// lateral axis). Defined below; forward-declared so the render weapon class
// (defined before the definition) can use it.
static void TFVR_ReflectBonesInControllerFrame(matrix3x4_t *pBones, int nBoneCount, const matrix3x4_t &controllerFrame);
static void SafeQuaternionSlerp(const Quaternion &from, const Quaternion &to, float t, Quaternion &result);

static bool TFVR_IsManualRocketLauncherWeaponID(int iWeaponID)
{
	return iWeaponID == TF_WEAPON_ROCKETLAUNCHER || iWeaponID == TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT;
}

static bool TFVR_ShouldUseBlackBoxReloadLoop(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon || !pWeapon->GetAttributeContainer())
		return false;

	CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
	if (!pItem)
		return false;

	const item_definition_index_t iItemDef = pItem->GetItemDefIndex();
	return iItemDef == 228 || iItemDef == 1085;
}

static bool TFVR_IsManualRocketReloadActive(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon || !TFVR_IsManualRocketLauncherWeaponID(pWeapon->GetWeaponID()))
		return false;

	CTFRocketLauncher *pRocketLauncher = static_cast<CTFRocketLauncher *>(pWeapon);
	return pRocketLauncher->IsVRRocketManualReloadActive();
}

static bool TFVR_HasManualReloadRocketVisual(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon || !TFVR_IsManualRocketLauncherWeaponID(pWeapon->GetWeaponID()))
		return false;

	CTFRocketLauncher *pRocketLauncher = static_cast<CTFRocketLauncher *>(pWeapon);
	return pRocketLauncher->HasVRRocketInHand() || pRocketLauncher->IsVRRocketInserting();
}

// Returns the pistol when it is a VR manual-magazine-reload pistol, else NULL.
static CTFPistol *TFVR_GetManualReloadPistol(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon || !VRPistol_IsManualReloadWeaponID(pWeapon->GetWeaponID()))
		return NULL;

	CTFPistol *pPistol = static_cast<CTFPistol *>(pWeapon);
	if (!pPistol->ShouldUseVRPistolManualReload())
		return NULL;

	return pPistol;
}

static int TFVR_GetPistolVisualWeaponID(C_TFWeaponBase *pWeapon, C_TFPlayer *pOwner)
{
	if (pOwner)
	{
		const int iClass = pOwner->GetPlayerClass()->GetClassIndex();
		if (iClass == TF_CLASS_ENGINEER)
			return TF_WEAPON_PISTOL;
		if (iClass == TF_CLASS_SCOUT)
			return TF_WEAPON_PISTOL_SCOUT;
	}

	return pWeapon ? pWeapon->GetWeaponID() : TF_WEAPON_PISTOL_SCOUT;
}

static void TFVR_SetReloadBodygroup(C_BaseAnimating *pAnimating, bool bVisible)
{
	if (!pAnimating)
		return;

	int iReloadBodygroup = pAnimating->FindBodygroupByName("reload");
	if (iReloadBodygroup >= 0)
		pAnimating->SetBodygroup(iReloadBodygroup, bVisible ? 1 : 0);
}

static void TFVR_HideBoneByName(C_BaseAnimating *pAnimating, CStudioHdr *pStudioHdr, matrix3x4_t *pBones, int nMaxBones, const char *pszBoneName)
{
	if (!pAnimating || !pStudioHdr || !pBones || !pszBoneName)
		return;

	const int iBone = pAnimating->LookupBone(pszBoneName);
	if (iBone < 0 || iBone >= pStudioHdr->numbones() || iBone >= nMaxBones)
		return;

	matrix3x4_t &boneMatrix = pBones[iBone];
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

static bool TFVR_MoveBoneAndChildrenToWorld(C_BaseAnimating *pAnimating, CStudioHdr *pStudioHdr, matrix3x4_t *pBones, int nMaxBones, const char *pszBoneName, const matrix3x4_t &targetWorld)
{
	if (!pAnimating || !pStudioHdr || !pBones || !pszBoneName)
		return false;

	const int iRootBone = pAnimating->LookupBone(pszBoneName);
	const int nBoneCount = MIN(pStudioHdr->numbones(), nMaxBones);
	if (iRootBone < 0 || iRootBone >= nBoneCount)
		return false;

	matrix3x4_t oldRootWorld;
	MatrixCopy(pBones[iRootBone], oldRootWorld);

	matrix3x4_t invOldRoot;
	MatrixInvert(oldRootWorld, invOldRoot);

	bool bMoveBone[MAXSTUDIOBONES];
	memset(bMoveBone, 0, sizeof(bMoveBone));
	bMoveBone[iRootBone] = true;

	for (int i = iRootBone + 1; i < nBoneCount; ++i)
	{
		const mstudiobone_t *pBone = pStudioHdr->pBone(i);
		if (pBone && pBone->parent >= 0 && pBone->parent < nBoneCount && bMoveBone[pBone->parent])
			bMoveBone[i] = true;
	}

	for (int i = iRootBone; i < nBoneCount; ++i)
	{
		if (!bMoveBone[i])
			continue;

		matrix3x4_t relToOldRoot;
		ConcatTransforms(invOldRoot, pBones[i], relToOldRoot);
		ConcatTransforms(targetWorld, relToOldRoot, pBones[i]);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: "Hide" a bone (and its children) by moving it far outside the
//          player's view, then collapsing it there. Scaling at the original
//          location leaves a visible sliver/point for some meshes.
//-----------------------------------------------------------------------------
static void TFVR_MoveBoneBehindHead(C_BaseAnimating *pAnimating, CStudioHdr *pStudioHdr, matrix3x4_t *pBones, int nMaxBones, const char *pszBoneName)
{
	if (!pAnimating || !pStudioHdr || !pBones || !pszBoneName)
		return;

	C_TFPlayer *pLocal = C_TFPlayer::GetLocalTFPlayer();
	if (!pLocal)
	{
		TFVR_HideBoneByName(pAnimating, pStudioHdr, pBones, nMaxBones, pszBoneName);
		return;
	}

	Vector vEye = pLocal->EyePosition();
	Vector vForward, vUp;
	AngleVectors(pLocal->EyeAngles(), &vForward, NULL, &vUp);
	Vector vBehind = vEye - vForward * 512.0f - vUp * 8192.0f;

	matrix3x4_t target;
	SetIdentityMatrix(target);
	MatrixSetColumn(vBehind, 3, target);

	if (!TFVR_MoveBoneAndChildrenToWorld(pAnimating, pStudioHdr, pBones, nMaxBones, pszBoneName, target))
	{
		TFVR_HideBoneByName(pAnimating, pStudioHdr, pBones, nMaxBones, pszBoneName);
		return;
	}

	const int iRootBone = pAnimating->LookupBone(pszBoneName);
	const int nBoneCount = MIN(MIN(pStudioHdr->numbones(), nMaxBones), MAXSTUDIOBONES);
	if (iRootBone < 0 || iRootBone >= nBoneCount)
		return;

	bool bHideBone[MAXSTUDIOBONES];
	memset(bHideBone, 0, sizeof(bHideBone));
	bHideBone[iRootBone] = true;

	for (int i = iRootBone + 1; i < nBoneCount; ++i)
	{
		const mstudiobone_t *pBone = pStudioHdr->pBone(i);
		if (pBone && pBone->parent >= 0 && pBone->parent < nBoneCount && bHideBone[pBone->parent])
			bHideBone[i] = true;
	}

	for (int i = iRootBone; i < nBoneCount; ++i)
	{
		if (!bHideBone[i])
			continue;

		pBones[i][0][0] = 0.0f;
		pBones[i][0][1] = 0.0f;
		pBones[i][0][2] = 0.0f;
		pBones[i][1][0] = 0.0f;
		pBones[i][1][1] = 0.0f;
		pBones[i][1][2] = 0.0f;
		pBones[i][2][0] = 0.0f;
		pBones[i][2][1] = 0.0f;
		pBones[i][2][2] = 0.0f;
		MatrixSetColumn(vBehind, 3, pBones[i]);
	}
}

static float TFVR_GetBowNockVisualProgress( CTFCompoundBow *pBow )
{
	if ( !pBow )
		return 0.0f;

	return SimpleSpline( clamp( pBow->GetVRBowArrowNockProgress(), 0.0f, 1.0f ) );
}

static void TFVR_BuildModelSpacePose( CStudioHdr *pStudioHdr, int numBones, Vector *pos, Quaternion *q, matrix3x4_t *modelSpace )
{
	if ( !pStudioHdr || !pos || !q || !modelSpace )
		return;

	const int nBoneCount = MIN( MIN( numBones, pStudioHdr->numbones() ), MAXSTUDIOBONES );
	for ( int i = 0; i < nBoneCount; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( q[i], pos[i], local );

		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( modelSpace[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, modelSpace[i] );
		else if ( pBone->parent >= 0 && pBone->parent < nBoneCount )
			ConcatTransforms( modelSpace[pBone->parent], local, modelSpace[i] );
		else
			SetIdentityMatrix( modelSpace[i] );
	}
}

static void TFVR_BlendPoseBonesRelativeToReference(
	CStudioHdr *pStudioHdr,
	int numBones,
	Vector *fromPos,
	Quaternion *fromQ,
	Vector *toPos,
	Quaternion *toQ,
	float flBlend,
	int iReferenceBone,
	const int *pTargetBones,
	int nTargetBones,
	Vector *outPos,
	Quaternion *outQ )
{
	if ( !pStudioHdr || !fromPos || !fromQ || !toPos || !toQ || !pTargetBones || !outPos || !outQ )
		return;

	const int nBoneCount = MIN( MIN( numBones, pStudioHdr->numbones() ), MAXSTUDIOBONES );
	if ( iReferenceBone < 0 || iReferenceBone >= nBoneCount )
		return;

	matrix3x4_t fromModel[MAXSTUDIOBONES];
	matrix3x4_t toModel[MAXSTUDIOBONES];
	matrix3x4_t outModel[MAXSTUDIOBONES];
	TFVR_BuildModelSpacePose( pStudioHdr, nBoneCount, fromPos, fromQ, fromModel );
	TFVR_BuildModelSpacePose( pStudioHdr, nBoneCount, toPos, toQ, toModel );
	TFVR_BuildModelSpacePose( pStudioHdr, nBoneCount, outPos, outQ, outModel );

	matrix3x4_t invFromReference;
	matrix3x4_t invToReference;
	MatrixInvert( fromModel[iReferenceBone], invFromReference );
	MatrixInvert( toModel[iReferenceBone], invToReference );

	const float w = clamp( flBlend, 0.0f, 1.0f );
	for ( int n = 0; n < nTargetBones; n++ )
	{
		const int iTargetBone = pTargetBones[n];
		if ( iTargetBone < 0 || iTargetBone >= nBoneCount || iTargetBone == iReferenceBone )
			continue;

		matrix3x4_t fromRelative;
		matrix3x4_t toRelative;
		ConcatTransforms( invFromReference, fromModel[iTargetBone], fromRelative );
		ConcatTransforms( invToReference, toModel[iTargetBone], toRelative );

		Vector fromRelPos, toRelPos, blendedRelPos;
		Quaternion fromRelQ, toRelQ, blendedRelQ;
		MatrixAngles( fromRelative, fromRelQ, fromRelPos );
		MatrixAngles( toRelative, toRelQ, toRelPos );
		VectorLerp( fromRelPos, toRelPos, w, blendedRelPos );
		SafeQuaternionSlerp( fromRelQ, toRelQ, w, blendedRelQ );

		matrix3x4_t blendedRelative;
		matrix3x4_t targetModel;
		QuaternionMatrix( blendedRelQ, blendedRelPos, blendedRelative );
		ConcatTransforms( outModel[iReferenceBone], blendedRelative, targetModel );

		const mstudiobone_t *pTargetBone = pStudioHdr->pBone( iTargetBone );
		if ( !pTargetBone )
			continue;

		matrix3x4_t targetLocal;
		if ( pTargetBone->parent >= 0 && pTargetBone->parent < nBoneCount )
		{
			matrix3x4_t invParent;
			MatrixInvert( outModel[pTargetBone->parent], invParent );
			ConcatTransforms( invParent, targetModel, targetLocal );
		}
		else
		{
			MatrixCopy( targetModel, targetLocal );
		}

		MatrixAngles( targetLocal, outQ[iTargetBone], outPos[iTargetBone] );
		MatrixCopy( targetModel, outModel[iTargetBone] );
	}
}

static bool TFVR_CalculateModelRocketBoneInverse(C_BaseAnimating *pRocket, matrix3x4_t &outInverse)
{
	if (!pRocket)
		return false;

	Vector vecOldOrigin = pRocket->GetAbsOrigin();
	QAngle angOldAngles = pRocket->GetAbsAngles();
	const bool bWasNoDraw = (pRocket->GetEffects() & EF_NODRAW) != 0;

	pRocket->RemoveEffects(EF_NODRAW);
	pRocket->SetAbsOrigin(vec3_origin);
	pRocket->SetAbsAngles(vec3_angle);
	pRocket->InvalidateBoneCache();

	bool bGotInverse = false;
	matrix3x4_t rocketBones[MAXSTUDIOBONES];
	if (pRocket->SetupBones(rocketBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime))
	{
		int iRocketBone = pRocket->LookupBone("rocket");
		if (iRocketBone >= 0 && iRocketBone < MAXSTUDIOBONES)
		{
			MatrixInvert(rocketBones[iRocketBone], outInverse);
			bGotInverse = true;
		}
	}

	pRocket->SetAbsOrigin(vecOldOrigin);
	pRocket->SetAbsAngles(angOldAngles);
	pRocket->InvalidateBoneCache();
	if (bWasNoDraw)
		pRocket->AddEffects(EF_NODRAW);

	return bGotInverse;
}

// Compute the inverse of a named bone's transform in the model's own
// origin-pose, so the entity can be placed such that the bone lands on a
// target world transform: entityWorld = targetBoneWorld * inverse.
static bool TFVR_CalculateModelBoneInverse(C_BaseAnimating *pEntity, const char *pszBoneName, matrix3x4_t &outInverse)
{
	if (!pEntity || !pszBoneName)
		return false;

	Vector vecOldOrigin = pEntity->GetAbsOrigin();
	QAngle angOldAngles = pEntity->GetAbsAngles();
	const bool bWasNoDraw = (pEntity->GetEffects() & EF_NODRAW) != 0;

	pEntity->RemoveEffects(EF_NODRAW);
	pEntity->SetAbsOrigin(vec3_origin);
	pEntity->SetAbsAngles(vec3_angle);
	pEntity->InvalidateBoneCache();

	bool bGotInverse = false;
	matrix3x4_t bones[MAXSTUDIOBONES];
	if (pEntity->SetupBones(bones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime))
	{
		int iBone = pEntity->LookupBone(pszBoneName);
		if (iBone >= 0 && iBone < MAXSTUDIOBONES)
		{
			MatrixInvert(bones[iBone], outInverse);
			bGotInverse = true;
		}
	}

	pEntity->SetAbsOrigin(vecOldOrigin);
	pEntity->SetAbsAngles(angOldAngles);
	pEntity->InvalidateBoneCache();
	if (bWasNoDraw)
		pEntity->AddEffects(EF_NODRAW);

	return bGotInverse;
}

//-----------------------------------------------------------------------------
// Purpose: Custom render weapon class that implements IHasOwner for material proxies
//          This allows crit glow and other effects to work properly
//-----------------------------------------------------------------------------
class C_VRRenderWeapon : public C_BaseAnimating, public IHasOwner
{
	DECLARE_CLASS(C_VRRenderWeapon, C_BaseAnimating);

public:
	C_VRRenderWeapon() : m_hOwnerPlayer(NULL), m_hSourceWeapon(NULL), m_iIdleSequence(0), m_iFireSequence(-1), m_flFireAnimEndCycle(1.0f), m_bPlayingFireAnim(false), m_bAnimateIdle(false), m_pCritBoostEffect(NULL), m_bCritBoostActive(false), m_iFireOnSequence(-1), m_iFireOffSequence(-1), m_iFireLoopSequence(-1), m_eMedigunFireState(MEDIGUN_FIRE_IDLE), m_bMedigunLeverOverride(false), m_iMedigunLeverSeq(-1), m_flMedigunLeverCycle(0.0f), m_bInSetupBones(false), m_bBreadBiteAnims(false), m_iBreadBiteSwingSeq(-1), m_iBreadBiteCritSeq(-1), m_iReloadStartSequence(-1), m_iReloadLoopSequence(-1), m_iReloadEndSequence(-1), m_bPumpReloadActive(false), m_flPumpReloadCycle(0.0f), m_bPumpReloadVmWeaponOverride(false), m_vecPumpReloadVmWeaponDelta(vec3_origin) { m_iBreadBiteIdleSeqs[0] = m_iBreadBiteIdleSeqs[1] = m_iBreadBiteIdleSeqs[2] = -1; }

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
		// Owning hand's finished bones, retained so the bow can copy the hand's
		// animated limb/string bones (weapon_bone_1/2) onto the render weapon.
		matrix3x4_t handBones[MAXSTUDIOBONES];
		bool bHaveHandBones = false;
		C_TFVRHand *pOwningHand = NULL;
		if (!m_bInSetupBones)
		{
			m_bInSetupBones = true;

			// Must pass a real bone array — the hand's SetupBones returns early
			// without running PositionWeaponFromBones if pBoneToWorldOut is NULL
			C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
			if (pRightHand && pRightHand->GetRenderWeapon() == this)
			{
				pOwningHand = pRightHand;
			}
			else
			{
				C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
				if (pLeftHand && pLeftHand->GetRenderWeapon() == this)
				{
					pOwningHand = pLeftHand;
				}
			}

			if (pOwningHand)
				bHaveHandBones = pOwningHand->SetupBones(handBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, currentTime);

			m_bInSetupBones = false;
		}
		bool bResult = BaseClass::SetupBones(pBoneToWorldOut, nMaxBones, boneMask, currentTime);

		CStudioHdr *pHdr = GetModelPtr();

		// Medigun lever bone override: use lever animation's local transform
		// relative to the lever bone's actual parent so it stays attached to the body.
		if (bResult && pBoneToWorldOut && m_bMedigunLeverOverride && m_iMedigunLeverSeq >= 0 && pHdr)
		{
			int leverIdx = LookupBone("vm_weapon_bone_L");
			if (leverIdx < 0)
				leverIdx = LookupBone("vm_weapon_bone_l");
			if (leverIdx >= 0 && leverIdx < nMaxBones)
			{
				const mstudiobone_t *pLeverBone = pHdr->pBone(leverIdx);
				if (pLeverBone && pLeverBone->parent >= 0 && pLeverBone->parent < nMaxBones)
				{
					Vector samplePos[MAXSTUDIOBONES];
					Quaternion sampleQ[MAXSTUDIOBONES];
					float samplePoseParams[MAXSTUDIOPOSEPARAM];
					for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
						samplePoseParams[i] = 0.0f;

					IBoneSetup sampleSetup(pHdr, BONE_USED_BY_ANYTHING, samplePoseParams);
					sampleSetup.InitPose(samplePos, sampleQ);
					sampleSetup.AccumulatePose(samplePos, sampleQ, m_iMedigunLeverSeq,
						m_flMedigunLeverCycle, 1.0f, gpGlobals->curtime, NULL);

					matrix3x4_t leverLocal;
					QuaternionMatrix(sampleQ[leverIdx], samplePos[leverIdx], leverLocal);
					ConcatTransforms(pBoneToWorldOut[pLeverBone->parent], leverLocal, pBoneToWorldOut[leverIdx]);
				}
			}
		}

		// Pump reload: override descendant bones of weapon_bone on the
		// render weapon model so lever/crank meshes animate with the pump.
		if (bResult && pBoneToWorldOut && m_bPumpReloadActive
			&& (m_iReloadLoopSequence >= 0 || m_bPumpReloadVmWeaponOverride) && pHdr)
		{
			if (m_bPumpReloadVmWeaponOverride)
			{
				int vmWeaponIdx = LookupBone("vm_weapon_bone");
				int numHdrBones = MIN(pHdr->numbones(), MAXSTUDIOBONES);
				if (vmWeaponIdx >= 0 && vmWeaponIdx < nMaxBones && vmWeaponIdx < numHdrBones)
				{
					const mstudiobone_t *pVmWeaponBone = pHdr->pBone(vmWeaponIdx);
					if (pVmWeaponBone && pVmWeaponBone->parent >= 0 && pVmWeaponBone->parent < nMaxBones)
					{
						matrix3x4_t oldVmWeaponWorld;
						MatrixCopy(pBoneToWorldOut[vmWeaponIdx], oldVmWeaponWorld);

						matrix3x4_t newVmWeaponWorld;
						MatrixCopy(oldVmWeaponWorld, newVmWeaponWorld);

						Vector axisX, axisY, axisZ, oldOrigin;
						MatrixGetColumn(oldVmWeaponWorld, 0, axisX);
						MatrixGetColumn(oldVmWeaponWorld, 1, axisY);
						MatrixGetColumn(oldVmWeaponWorld, 2, axisZ);
						MatrixGetColumn(oldVmWeaponWorld, 3, oldOrigin);

						Vector newOrigin = oldOrigin
							+ axisX * m_vecPumpReloadVmWeaponDelta.x
							+ axisY * m_vecPumpReloadVmWeaponDelta.y
							+ axisZ * m_vecPumpReloadVmWeaponDelta.z;
						MatrixSetColumn(newOrigin, 3, newVmWeaponWorld);

						matrix3x4_t oldVmWeaponInv;
						MatrixInvert(oldVmWeaponWorld, oldVmWeaponInv);

						bool isDescendant[MAXSTUDIOBONES];
						memset(isDescendant, 0, sizeof(isDescendant));
						isDescendant[vmWeaponIdx] = true;
						for (int i = 0; i < numHdrBones && i < nMaxBones; i++)
						{
							const mstudiobone_t *pBone = pHdr->pBone(i);
							if (!pBone || i == vmWeaponIdx)
								continue;
							if (pBone->parent >= 0 && pBone->parent < numHdrBones
								&& isDescendant[pBone->parent])
							{
								isDescendant[i] = true;
							}
						}

						for (int i = 0; i < numHdrBones && i < nMaxBones; i++)
						{
							if (!isDescendant[i])
								continue;

							if (i == vmWeaponIdx)
							{
								MatrixCopy(newVmWeaponWorld, pBoneToWorldOut[i]);
								continue;
							}

							matrix3x4_t relToVmWeapon;
							ConcatTransforms(oldVmWeaponInv, pBoneToWorldOut[i], relToVmWeapon);
							ConcatTransforms(newVmWeaponWorld, relToVmWeapon, pBoneToWorldOut[i]);
						}
					}
				}
			}
			else if (m_iReloadLoopSequence >= 0)
			{
				int wpnIdx = LookupBone("weapon_bone");
				if (wpnIdx < 0)
					wpnIdx = LookupBone("vm_weapon_bone");
				if (wpnIdx >= 0 && wpnIdx < nMaxBones)
				{
					Vector samplePos[MAXSTUDIOBONES];
					Quaternion sampleQ[MAXSTUDIOBONES];
					float samplePoseParams[MAXSTUDIOPOSEPARAM];
					for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
						samplePoseParams[i] = 0.0f;

					IBoneSetup sampleSetup(pHdr, BONE_USED_BY_ANYTHING, samplePoseParams);
					sampleSetup.InitPose(samplePos, sampleQ);
					sampleSetup.AccumulatePose(samplePos, sampleQ, m_iReloadLoopSequence,
						m_flPumpReloadCycle, 1.0f, gpGlobals->curtime, NULL);

					// Build model-space transforms from the sampled pose
					int numHdrBones = pHdr->numbones();
					matrix3x4_t modelSpace[MAXSTUDIOBONES];
					bool isDescendant[MAXSTUDIOBONES];
					memset(isDescendant, 0, sizeof(isDescendant));

					for (int i = 0; i < numHdrBones && i < MAXSTUDIOBONES; i++)
					{
						matrix3x4_t localMat;
						QuaternionMatrix(sampleQ[i], samplePos[i], localMat);
						const mstudiobone_t *pBone = pHdr->pBone(i);
						if (!pBone || pBone->parent < 0)
						{
							MatrixCopy(localMat, modelSpace[i]);
						}
						else
						{
							ConcatTransforms(modelSpace[pBone->parent], localMat, modelSpace[i]);
							if (pBone->parent == wpnIdx || isDescendant[pBone->parent])
								isDescendant[i] = true;
						}
					}

					matrix3x4_t weaponModelInv;
					MatrixInvert(modelSpace[wpnIdx], weaponModelInv);

					for (int i = 0; i < numHdrBones && i < nMaxBones; i++)
					{
						if (!isDescendant[i])
							continue;

						matrix3x4_t relToWeapon;
						ConcatTransforms(weaponModelInv, modelSpace[i], relToWeapon);
						ConcatTransforms(pBoneToWorldOut[wpnIdx], relToWeapon, pBoneToWorldOut[i]);
					}
				}
			}
		}

		// Medigun body animation: rotation-only on weapon_bone variants.
		// The weapon body should only rotate during fire_on/loop/off, not translate.
		// Use the body animation's rotation with the idle animation's position.
		if (bResult && pBoneToWorldOut && m_eMedigunFireState != MEDIGUN_FIRE_IDLE && pHdr && m_iIdleSequence >= 0)
		{
			int numHdrBones = pHdr->numbones();

			Vector idlePos[MAXSTUDIOBONES];
			Quaternion idleQ[MAXSTUDIOBONES];
			float idlePoseParams[MAXSTUDIOPOSEPARAM];
			for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
				idlePoseParams[i] = 0.0f;
			IBoneSetup idleSetup(pHdr, BONE_USED_BY_ANYTHING, idlePoseParams);
			idleSetup.InitPose(idlePos, idleQ);
			idleSetup.AccumulatePose(idlePos, idleQ, m_iIdleSequence, 0.0f, 1.0f, gpGlobals->curtime, NULL);

			Vector bodyPos[MAXSTUDIOBONES];
			Quaternion bodyQ[MAXSTUDIOBONES];
			float bodyPoseParams[MAXSTUDIOPOSEPARAM];
			for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
				bodyPoseParams[i] = 0.0f;
			IBoneSetup bodySetup(pHdr, BONE_USED_BY_ANYTHING, bodyPoseParams);
			bodySetup.InitPose(bodyPos, bodyQ);
			bodySetup.AccumulatePose(bodyPos, bodyQ, GetSequence(), GetCycle(), 1.0f, gpGlobals->curtime, NULL);

			const char *wpnBoneNames[] = { "weapon_bone", "weapon_bone_L", "weapon_bone_l" };
			for (int n = 0; n < ARRAYSIZE(wpnBoneNames); n++)
			{
				int wpnIdx = LookupBone(wpnBoneNames[n]);
				if (wpnIdx < 0 || wpnIdx >= nMaxBones || wpnIdx >= numHdrBones)
					continue;
				const mstudiobone_t *pWpnBone = pHdr->pBone(wpnIdx);
				if (!pWpnBone || pWpnBone->parent < 0 || pWpnBone->parent >= nMaxBones)
					continue;

				matrix3x4_t rotOnlyLocal;
				QuaternionMatrix(bodyQ[wpnIdx], idlePos[wpnIdx], rotOnlyLocal);
				ConcatTransforms(pBoneToWorldOut[pWpnBone->parent], rotOnlyLocal, pBoneToWorldOut[wpnIdx]);
			}
		}

		bool bLooseBowArrowTargetValid = false;
		bool bHideBowArrow = false;
		matrix3x4_t matLooseBowArrowTarget;
		SetIdentityMatrix(matLooseBowArrowTarget);
		const bool bBowFireStartPoseActive = pOwningHand && pOwningHand->IsBowFireStartPoseActive();

		if (bResult && pBoneToWorldOut && pHdr && m_hSourceWeapon.Get()
			&& m_hSourceWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		{
			CTFCompoundBow *pBow = static_cast<CTFCompoundBow *>(m_hSourceWeapon.Get());
			const bool bArrowFollowsSupportHand = (pBow->HasVRBowArrowInHand() || pBow->IsVRBowArrowNocking())
				&& !pBow->IsVRBowArrowNocked();
			if (bArrowFollowsSupportHand)
			{
				C_TFVRHand *pArrowHand = TFVR_GetSupportHand(m_hSourceWeapon.Get());
				if (pArrowHand)
				{
					matrix3x4_t arrowHandBones[MAXSTUDIOBONES];
					if (pArrowHand->SetupBones(arrowHandBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, currentTime))
					{
						int iHandArrowBone = pArrowHand->LookupBone("weapon_bone_4");
						if (iHandArrowBone < 0)
							iHandArrowBone = pArrowHand->LookupBone("vm_weapon_bone");
						if (iHandArrowBone < 0)
							iHandArrowBone = pArrowHand->LookupBone("weapon_bone");

						if (iHandArrowBone >= 0 && iHandArrowBone < MAXSTUDIOBONES)
						{
							MatrixCopy(arrowHandBones[iHandArrowBone], matLooseBowArrowTarget);
							bLooseBowArrowTargetValid = true;
						}
					}
				}
			}

			const bool bShowArrow = bLooseBowArrowTargetValid
				|| pBow->IsVRBowArrowNocking()
				|| pBow->IsVRBowArrowNocked()
				|| pBow->GetCurrentCharge() > 0.0f
				|| bBowFireStartPoseActive;
			bHideBowArrow = !bShowArrow;
		}

		// Left-handed mode: mirror the weapon mesh across the same controller
		// frame the owning hand used, so the weapon reads as held in the
		// mirrored hand. DrawModel flips culling to match.
		if (bResult && pBoneToWorldOut && pHdr)
		{
			C_TFVRHand *pHand = GetLocalPlayerRightHand();
			if (!pHand || pHand->GetRenderWeapon() != this)
				pHand = GetLocalPlayerLeftHand();
			if (pHand && pHand->GetRenderWeapon() == this && pHand->IsPoseReflected())
			{
				TFVR_ReflectBonesInControllerFrame(pBoneToWorldOut, MIN(nMaxBones, pHdr->numbones()), pHand->GetReflectFrame());
			}
		}

		if (bLooseBowArrowTargetValid)
			TFVR_MoveBoneAndChildrenToWorld(this, pHdr, pBoneToWorldOut, nMaxBones, "weapon_bone_4", matLooseBowArrowTarget);

		// Drive the bow's body/limb/string bones from the hand. The hand model
		// animates these during bw_charge/shake, but the render weapon's own
		// idle sequence does not, so copy the hand's animated bones onto the
		// render weapon. Done after the mirror so the hand's already-final
		// (visible) bones map straight across, like the loose arrow above.
		if (bResult && pBoneToWorldOut && pHdr && bHaveHandBones && pOwningHand
			&& m_hSourceWeapon.Get()
			&& m_hSourceWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		{
			const char *shakeCopyBones[] = { "weapon_bone", "vm_weapon_bone" };
			const char *alwaysCopyBones[] = { "weapon_bone_1", "weapon_bone_2", "weapon_bone_3" };

			const bool bCopyShakeRoot = pOwningHand->IsBowShakeOverlayActive() || bBowFireStartPoseActive;
			const int nShakeCopyCount = bCopyShakeRoot ? ARRAYSIZE(shakeCopyBones) : 0;
			const int nAlwaysCopyCount = ARRAYSIZE(alwaysCopyBones);
			for (int n = 0; n < nShakeCopyCount + nAlwaysCopyCount; n++)
			{
				// Copy the animated root first, then exact child bones. Moving a
				// root after children would preserve old child-relative offsets and
				// erase shake from the limbs/string.
				const char *pszBoneName = n < nShakeCopyCount
					? shakeCopyBones[n]
					: alwaysCopyBones[n - nShakeCopyCount];

				int iHandLimb = pOwningHand->LookupBone(pszBoneName);
				if (iHandLimb < 0 && V_stricmp(pszBoneName, "weapon_bone") == 0)
					iHandLimb = pOwningHand->LookupBone("vm_weapon_bone");
				else if (iHandLimb < 0 && V_stricmp(pszBoneName, "vm_weapon_bone") == 0)
					iHandLimb = pOwningHand->LookupBone("weapon_bone");

				if (iHandLimb >= 0 && iHandLimb < MAXSTUDIOBONES)
					TFVR_MoveBoneAndChildrenToWorld(this, pHdr, pBoneToWorldOut, nMaxBones, pszBoneName, handBones[iHandLimb]);
			}

			// Once fully nocked, drive the arrow (weapon_bone_4) from the weapon
			// hand's blended nock/pull pose so it follows the drawstring instead
			// of staying at the render weapon's idle sequence. While nocking, the
			// arrow follows the support hand above so it stays visually in-hand.
			CTFCompoundBow *pSourceBow = static_cast<CTFCompoundBow *>(m_hSourceWeapon.Get());
			if (!bLooseBowArrowTargetValid
				&& (pSourceBow->IsVRBowArrowNocked() || bBowFireStartPoseActive))
			{
				int iHandArrow = pOwningHand->LookupBone("weapon_bone_4");
				if (iHandArrow >= 0 && iHandArrow < MAXSTUDIOBONES)
					TFVR_MoveBoneAndChildrenToWorld(this, pHdr, pBoneToWorldOut, nMaxBones, "weapon_bone_4", handBones[iHandArrow]);
			}
		}

		if (bHideBowArrow)
			TFVR_MoveBoneBehindHead(this, pHdr, pBoneToWorldOut, nMaxBones, "weapon_bone_4");

		return bResult;
	}

	// Always suppress sound events on the render weapon — the viewmodel
	// is the sole source of weapon sounds (draw, reload, idle growls, etc.)
	// via C_BaseViewModel::FireEvent → EmitSound.  Letting them through
	// here would cause duplicate/desynced audio.
	virtual void FireEvent( const Vector& origin, const QAngle& angles, int event, const char *options ) OVERRIDE
	{
		if ( event == AE_CL_PLAYSOUND || event == CL_EVENT_SOUND )
			return;
		BaseClass::FireEvent( origin, angles, event, options );
	}

	void SetAnimateIdle(bool bAnimate) { m_bAnimateIdle = bAnimate; }
	bool IsPlayingDrawOrFire() const { return m_bPlayingFireAnim; }
	int GetIdleSequence() const { return m_iIdleSequence; }

	C_TFWeaponBase* GetSourceWeapon() const { return m_hSourceWeapon.Get(); }

	// IHasOwner interface
	virtual CBaseEntity *GetOwnerViaInterface(void) OVERRIDE
	{
		return m_hOwnerPlayer.Get();
	}

	virtual IMaterial* GetEconWeaponMaterialOverride( int iTeam ) OVERRIDE
	{
		C_TFWeaponBase *pWeapon = m_hSourceWeapon.Get();
		if ( pWeapon )
			return pWeapon->GetEconWeaponMaterialOverride( pWeapon->GetTeamNumber() );
		return NULL;
	}

	// Fire animation support - set the fire sequence index directly from the hand
	void SetFireSequence(int iSequence)
	{
		m_iFireSequence = iSequence;
		m_flFireAnimEndCycle = 1.0f;

		C_TFWeaponBase *pWeapon = m_hSourceWeapon.Get();
		CStudioHdr *pHdr = GetModelPtr();
		if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW && pHdr && m_iFireSequence >= 0)
		{
			float poseParams[MAXSTUDIOPOSEPARAM] = {};
			int maxFrame = Studio_MaxFrame(pHdr, m_iFireSequence, poseParams);
			if (maxFrame > 0)
				m_flFireAnimEndCycle = clamp(15.0f / (float)maxFrame, 0.0f, 1.0f);
		}
	}

	void SetIdleSequence(int iSequence)
	{
		m_iIdleSequence = iSequence;
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

		// Bread Bite: play the bread creature's own swing/crit animation
		if (m_bBreadBiteAnims)
		{
			bool bIsCrit = false;
			C_TFWeaponBase *pWeapon = m_hSourceWeapon.Get();
			if (pWeapon)
				bIsCrit = pWeapon->IsCurrentAttackACrit();

			int seq = (bIsCrit && m_iBreadBiteCritSeq >= 0) ? m_iBreadBiteCritSeq : m_iBreadBiteSwingSeq;
			if (seq >= 0)
			{
				SetSequence(seq);
				SetCycle(0.0f);
				SetPlaybackRate(1.0f);
				m_bPlayingFireAnim = true;

				if (tfvr_weapon_fire_anim_debug.GetBool())
				{
					DevMsg("VR: Playing bread bite weapon swing (seq %d, crit=%d)\n", seq, bIsCrit);
				}
				return;
			}
		}

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
		m_bMedigunLeverOverride = false;
		m_iMedigunLeverSeq = -1;
		m_flMedigunLeverCycle = 0.0f;

		extern ConVar tfvr_weapon_fire_anim_debug;
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Render weapon medigun sequences - fire_on: %d, fire_loop: %d, fire_off: %d\n",
				m_iFireOnSequence, m_iFireLoopSequence, m_iFireOffSequence);
		}
	}

	// iType: 0 = fire_on, 1 = fire_off
	void SetMedigunLeverBone(bool bActive, int iType, float flCycle)
	{
		m_bMedigunLeverOverride = bActive;
		if (bActive)
			m_iMedigunLeverSeq = (iType == 0) ? m_iFireOnSequence : m_iFireOffSequence;
		else
			m_iMedigunLeverSeq = -1;
		m_flMedigunLeverCycle = flCycle;
	}

	void SetPumpReloadState(bool bActive, float flCycle, const Vector *pVmWeaponBoneDelta = NULL)
	{
		m_bPumpReloadActive = bActive;
		m_flPumpReloadCycle = flCycle;
		m_bPumpReloadVmWeaponOverride = bActive && pVmWeaponBoneDelta;
		if (m_bPumpReloadVmWeaponOverride)
			m_vecPumpReloadVmWeaponDelta = *pVmWeaponBoneDelta;
	}

	int GetMedigunFireOnSeq() const { return m_iFireOnSequence; }
	int GetMedigunFireLoopSeq() const { return m_iFireLoopSequence; }
	int GetMedigunFireOffSeq() const { return m_iFireOffSequence; }

	void SetupBreadBiteAnimations()
	{
		m_bBreadBiteAnims = true;
		m_iBreadBiteSwingSeq = LookupSequence("breadglove_swing_right");
		m_iBreadBiteCritSeq = LookupSequence("breadglove_swing_crit");

		m_iBreadBiteIdleSeqs[0] = LookupSequence("breadglove_idle_A");
		m_iBreadBiteIdleSeqs[1] = LookupSequence("breadglove_idle_B");
		m_iBreadBiteIdleSeqs[2] = LookupSequence("breadglove_idle_C");

		extern ConVar tfvr_weapon_fire_anim_debug;
		if (tfvr_weapon_fire_anim_debug.GetBool())
		{
			DevMsg("VR: Bread Bite weapon model sequences - swing: %d, crit: %d, idles: %d/%d/%d\n",
				m_iBreadBiteSwingSeq, m_iBreadBiteCritSeq,
				m_iBreadBiteIdleSeqs[0], m_iBreadBiteIdleSeqs[1], m_iBreadBiteIdleSeqs[2]);
		}
	}

	void RandomizeBreadBiteIdle()
	{
		if (!m_bBreadBiteAnims)
			return;
		int idx = RandomInt(0, 2);
		if (m_iBreadBiteIdleSeqs[idx] >= 0)
			m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
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

		// Bread creature weapons: render weapon is a passive mesh (rate 0).
		// All bones are driven by the hand via bone merge — no self-animation.
		// Only non-bread animated weapons self-advance here.

		StudioFrameAdvance();

		// Check if fire/draw animation has completed (skip for medigun - driven by hand)
		if (m_bPlayingFireAnim && m_eMedigunFireState == MEDIGUN_FIRE_IDLE && GetCycle() >= m_flFireAnimEndCycle)
		{
			RandomizeBreadBiteIdle();

			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
			}
			SetPlaybackRate(0.0f);
			m_bPlayingFireAnim = false;
		}

		// Loop idle animations for non-bread animated weapons
		if (!m_bAnimateIdle && !m_bPlayingFireAnim && GetPlaybackRate() > 0.0f && GetCycle() >= 1.0f)
		{
			SetCycle(0.0f);
		}

		UpdateCritBoostEffect();
	}

	virtual float FrameAdvance(float flInterval = 0.0f) OVERRIDE
	{
		float flReturn = BaseClass::FrameAdvance(flInterval);

		// Draw/fire completion and idle looping are handled in ClientThink
		// so that rate-setting and state cleanup happen in one place.
		if (!m_bPlayingFireAnim && GetPlaybackRate() > 0.0f && GetCycle() >= 1.0f)
		{
			SetCycle(0.0f);
		}

		return flReturn;
	}

	bool HasFireAnimation() const { return m_iFireSequence >= 0; }

	void PlayDrawAnimation()
	{
		extern ConVar tfvr_weapon_draw_anim;
		extern ConVar tfvr_weapon_draw_anim_debug;

		if (!tfvr_weapon_draw_anim.GetBool())
			return;

		int drawSeq = -1;

		// Bread Bite: use the bread creature's deploy animation
		if (m_bBreadBiteAnims)
		{
			int variant = GetBreadBiteDrawVariant();
			drawSeq = LookupSequence(variant ? "breadglove_draw_B" : "breadglove_draw_A");
		}

		if (drawSeq < 0)
			drawSeq = LookupSequence("bm_draw");
		if (drawSeq < 0)
			drawSeq = LookupSequence("c_breadmonster_sapper_draw");
		if (drawSeq < 0)
			drawSeq = LookupSequence("c_breadmonster_draw");
		if (drawSeq < 0)
			drawSeq = LookupSequence("draw");
		if (drawSeq < 0)
			drawSeq = LookupSequence("deploy");

		if (drawSeq >= 0)
		{
			SetSequence(drawSeq);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
			m_bPlayingFireAnim = true;
		}
	}

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
		if (!TFVR_ValidateHandRenderable(this, "render weapon"))
			return 0;

		int ret = 0;
		bool bInvuln = pOwner->m_Shared.IsInvulnerable();

		// Apply ubercharge material override
		if (bInvuln && (flags & STUDIO_RENDER))
		{
			modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());
		}

		// Left-handed mode: the reflected bones invert winding, so flip culling
		// to clockwise while drawing (matches vanilla's flipped viewmodel).
		C_TFVRHand *pMirrorHand = GetLocalPlayerRightHand();
		if (!pMirrorHand || pMirrorHand->GetRenderWeapon() != this)
			pMirrorHand = GetLocalPlayerLeftHand();
		const bool bMirrored = pMirrorHand && pMirrorHand->GetRenderWeapon() == this && pMirrorHand->IsPoseReflected();
		CMatRenderContextPtr pRenderContext(materials);
		if (bMirrored && (flags & STUDIO_RENDER))
			pRenderContext->CullMode(MATERIAL_CULLMODE_CW);

		ret = BaseClass::DrawModel(flags);

		if (bMirrored && (flags & STUDIO_RENDER))
			pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);

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
	float m_flFireAnimEndCycle;
	bool m_bPlayingFireAnim;
	bool m_bAnimateIdle;
	CSmartPtr<CNewParticleEffect> m_pCritBoostEffect;
	bool m_bCritBoostActive;
	bool m_bInSetupBones;

	// Medigun fire animation state
	int m_iFireOnSequence;
	int m_iFireOffSequence;
	int m_iFireLoopSequence;
	MedigunFireState m_eMedigunFireState;

	// Medigun lever bone override (lever bone scrubbed by progress in SetupBones)
	bool m_bMedigunLeverOverride;
	int  m_iMedigunLeverSeq;
	float m_flMedigunLeverCycle;

	// Bread Bite weapon model animation state
	bool m_bBreadBiteAnims;
	int m_iBreadBiteSwingSeq;
	int m_iBreadBiteCritSeq;
	int m_iBreadBiteIdleSeqs[3];

	// Pump/lever reload animation sequences (cached on weapon model)
	int m_iReloadStartSequence;
	int m_iReloadLoopSequence;
	int m_iReloadEndSequence;

	// Pump reload bone override (weapon_bone_1 scrubbed by pump cycle)
	bool m_bPumpReloadActive;
	float m_flPumpReloadCycle;
	bool m_bPumpReloadVmWeaponOverride;
	Vector m_vecPumpReloadVmWeaponDelta;

public:
	void SetupReloadAnimations( const char *pszPrefix = "sg" )
	{
		char szBuf[64];
		V_snprintf( szBuf, sizeof(szBuf), "%s_reload_start", pszPrefix );
		m_iReloadStartSequence = LookupSequence( szBuf );
		V_snprintf( szBuf, sizeof(szBuf), "%s_reload_loop", pszPrefix );
		m_iReloadLoopSequence  = LookupSequence( szBuf );
		V_snprintf( szBuf, sizeof(szBuf), "%s_reload_end", pszPrefix );
		m_iReloadEndSequence   = LookupSequence( szBuf );
	}

	void SetupReloadLoopAnimation( const char *pszLoopSequence )
	{
		m_iReloadStartSequence = -1;
		m_iReloadLoopSequence = pszLoopSequence ? LookupSequence( pszLoopSequence ) : -1;
		m_iReloadEndSequence = -1;
	}

	int GetReloadSequence(VRReloadAnimState state) const
	{
		switch (state)
		{
		case VR_RELOAD_ANIM_ENTER:
		case VR_RELOAD_ANIM_HOLD:
			return m_iReloadStartSequence;
		case VR_RELOAD_ANIM_PUMPING:
			return m_iReloadLoopSequence;
		case VR_RELOAD_ANIM_EXIT:
			return m_iReloadEndSequence;
		default:
			return -1;
		}
	}
};

// Global helpers for war paint / weapon skin support.
// These allow c_tf_player.cpp to resolve a VR render weapon back to its
// source weapon's CEconItemView / owner without seeing the file-local class.
CEconItemView *GetVRRenderWeaponEconItemView( CBaseEntity *pEntity )
{
	C_VRRenderWeapon *pRenderWeapon = dynamic_cast<C_VRRenderWeapon*>( pEntity );
	if ( pRenderWeapon )
	{
		C_TFWeaponBase *pWeapon = pRenderWeapon->GetSourceWeapon();
		if ( pWeapon )
		{
			IHasAttributes *pAttrib = GetAttribInterface( pWeapon );
			if ( pAttrib )
				return pAttrib->GetAttributeContainer()->GetItem();
		}
	}
	return NULL;
}

C_TFPlayer *GetVRRenderWeaponOwner( CBaseEntity *pEntity )
{
	C_VRRenderWeapon *pRenderWeapon = dynamic_cast<C_VRRenderWeapon*>( pEntity );
	if ( pRenderWeapon )
		return dynamic_cast<C_TFPlayer*>( pRenderWeapon->GetOwnerViaInterface() );
	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Standalone VGUI panel for the VR spy watch cloak meter.
//          Matches vanilla TF2 appearance by using the same scheme and layout.
//          Kept visible but off-screen so VGUI processes scheme/layout;
//          rendered in 3D via DrawPanelIn3DSpace in C_TFVRHand::DrawModel.
//-----------------------------------------------------------------------------
class CVRWatchPanel : public vgui::EditablePanel
{
	DECLARE_CLASS_SIMPLE(CVRWatchPanel, vgui::EditablePanel);
public:
	CVRWatchPanel(vgui::Panel *parent)
		: BaseClass(parent, "CVRWatchPanel",
			vgui::scheme()->LoadSchemeFromFileEx(
				enginevgui->GetPanel(PANEL_CLIENTDLL),
				"resource/PDAControlPanelScheme.res", "TFBase"))
	{
		m_pInvisProgress = new vgui::ProgressBar(this, "InvisProgress");
		SetBounds(0, 0, 280, 100);
		SetPos(-10000, -10000);
		vgui::ivgui()->AddTickSignal(GetVPanel());
	}

	virtual void ApplySchemeSettings(vgui::IScheme *pScheme)
	{
		BaseClass::ApplySchemeSettings(pScheme);
		LoadControlSettings("scripts/screens/pda_spy_invis.res");
	}

	virtual void OnTick()
	{
		C_TFPlayer *pPlayer = C_TFPlayer::GetLocalTFPlayer();
		if (pPlayer && m_pInvisProgress)
		{
			float flMeter = pPlayer->m_Shared.GetSpyCloakMeter();
			m_pInvisProgress->SetProgress(flMeter / 100.0f);
		}
	}

private:
	vgui::ProgressBar *m_pInvisProgress;
};

// Forward declarations
static const char* GetFistsIdleAnimName(C_TFWeaponBase *pWeapon);
static bool IsFistsGloveVariant(C_TFWeaponBase *pWeapon);
static bool IsBareFists(C_TFWeaponBase *pWeapon);

// ConVars for debugging and control
ConVar tfvr_hands_enabled("tfvr_hands_enabled", "1", FCVAR_ARCHIVE, "Enable VR hand rendering");
ConVar tfvr_pistol_reload_wrist_motion("tfvr_pistol_reload_wrist_motion", "0.35", FCVAR_ARCHIVE,
	"VR pistol manual reload: fraction (0-1) of the authored wrist motion applied to the weapon hand during the reload animation. 0 = hand fully pinned to the controller.");
ConVar tfvr_pistol_reload_blend_out("tfvr_pistol_reload_blend_out", "0.5", FCVAR_ARCHIVE,
	"VR pistol manual reload: seconds to blend the weapon hand back to the idle pose after the reload finish motion ends");
ConVar tfvr_hands_debug("tfvr_hands_debug", "0", FCVAR_NONE, "Show debug info for VR hands");
ConVar tfvr_hands_debug_bones("tfvr_hands_debug_bones", "0", FCVAR_NONE, "Draw bone positions on VR hands/weapons (1=hand, 2=weapon, 3=both)");
ConVar tfvr_hands_alpha("tfvr_hands_alpha", "1", FCVAR_ARCHIVE, "Alpha transparency for VR hands (0-1)");
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
ConVar tfvr_hands_left_offset_pitch("tfvr_hands_left_offset_pitch", "0", FCVAR_ARCHIVE, "Pitch offset for left VR hand (degrees)");
ConVar tfvr_hands_left_offset_yaw("tfvr_hands_left_offset_yaw", "125", FCVAR_ARCHIVE, "Yaw offset for left VR hand (degrees)");

// Shadow convars for debugging
ConVar tfvr_hands_shadow_bounds("tfvr_hands_shadow_bounds", "10000", FCVAR_CHEAT, "Render bounds size for VR hands (affects shadow culling)");
ConVar tfvr_hands_shadow_distance("tfvr_hands_shadow_distance", "2000", FCVAR_CHEAT, "Shadow cast distance for VR hands");
ConVar tfvr_hands_shadow_type("tfvr_hands_shadow_type", "2", FCVAR_CHEAT, "Shadow type for VR hands (0=none, 1=simple, 2=texture, 3=texture_dynamic)");
ConVar tfvr_hands_shadow_debug("tfvr_hands_shadow_debug", "0", FCVAR_CHEAT, "Show shadow debug info for VR hands");

static const char *TFVR_GetRenderableModelName(C_BaseAnimating *pRenderable)
{
	if (!pRenderable)
		return "(null entity)";

	const model_t *pModel = pRenderable->GetModel();
	if (!pModel)
		return "(null model)";

	return modelinfo ? modelinfo->GetModelName(pModel) : "(modelinfo unavailable)";
}

static bool TFVR_ValidateHandRenderable(C_BaseAnimating *pRenderable, const char *pszLabel)
{
	if (!pRenderable)
		return false;

	const model_t *pModel = pRenderable->GetModel();
	if (!pModel)
	{
		DevMsg("VR Hand Draw: skipping %s - null model\n", pszLabel);
		return false;
	}

	CStudioHdr *pStudioHdr = pRenderable->GetModelPtr();
	if (!pStudioHdr || !pStudioHdr->IsValid())
	{
		DevMsg("VR Hand Draw: skipping %s - invalid studio header for '%s'\n",
			pszLabel, TFVR_GetRenderableModelName(pRenderable));
		return false;
	}

	return true;
}

// Muzzle position mode for effects (sounds, muzzle flash, tracers)
ConVar tfvr_muzzle_direct_mode("tfvr_muzzle_direct_mode", "0", FCVAR_ARCHIVE, "Use controller pose directly for muzzle position (0=attachment system, 1=direct controller+offset)");

// Left-handed mode hand-mirror tuning. The finished hand bones are reflected
// across a plane through the controller frame; which controller-local axis is
// the mirror plane normal, plus an optional 180-degree spin to cancel residual
// roll/yaw, depends on the hand-pose frame orientation. These let us dial it in.
// The reflection is done in the RAW controller frame (rigidly attached to the
// controller), so the mirrored hand tracks the controller correctly. Reflecting
// across the frame's lateral (Y/left) plane is the anatomical left<->right
// mirror and preserves the aim (X) axis.
//   tfvr_lefthand_mirror_axis: which frame row to negate (0=X aim, 1=Y lateral [default], 2=Z up)
//   tfvr_lefthand_mirror_spin: optional extra 180 spin about a frame axis (0=none,1=X,2=Y,3=Z)
ConVar tfvr_lefthand_mirror_axis("tfvr_lefthand_mirror_axis", "1", FCVAR_ARCHIVE, "Left-handed hand-mirror plane normal axis (0=X,1=Y,2=Z)");
ConVar tfvr_lefthand_mirror_spin("tfvr_lefthand_mirror_spin", "0", FCVAR_ARCHIVE, "Left-handed hand-mirror extra 180-degree spin axis (0=none,1=X,2=Y,3=Z)");

// Two-handed weapon convars
ConVar tfvr_twohand_enabled("tfvr_twohand_enabled", "1", FCVAR_ARCHIVE, "Enable two-handed weapon gripping");
ConVar tfvr_twohand_snap_distance("tfvr_twohand_snap_distance", "8", FCVAR_ARCHIVE, "Distance (inches) at which off-hand snaps to weapon grip");
ConVar tfvr_twohand_blend_distance("tfvr_twohand_blend_distance", "1", FCVAR_ARCHIVE, "Distance (inches) at which off-hand starts blending towards weapon grip");
ConVar tfvr_twohand_debug("tfvr_twohand_debug", "0", FCVAR_CHEAT, "Show two-handed grip debug info");
ConVar tfvr_huntsman_debug("tfvr_huntsman_debug", "0", FCVAR_CHEAT, "Show Huntsman VR arrow/nock debug overlays and logs");
ConVar tfvr_huntsman_aim_min_pull("tfvr_huntsman_aim_min_pull", "5.0", FCVAR_ARCHIVE, "VR Huntsman: minimum draw-hand separation (inches) before two-hand aim rotation engages; prevents the bow spinning erratically right after nocking when the hands are close together");
ConVar tfvr_pomson_grip_debug("tfvr_pomson_grip_debug", "0", FCVAR_CHEAT, "Show Pomson right-hand grip target/easing debug overlays");
ConVar tfvr_shotgun_manual_reload_pose_blend_fraction("tfvr_shotgun_manual_reload_pose_blend_fraction", "0.35", FCVAR_ARCHIVE, "Fraction of pump-shotgun shell insert animation spent easing the offhand into the authored reload pose");
ConVar tfvr_shotgun_manual_reload_pose_blend_out_time("tfvr_shotgun_manual_reload_pose_blend_out_time", "0.12", FCVAR_ARCHIVE, "Seconds spent easing the offhand out of the pump-shotgun shell reload pose");
ConVar tfvr_rocket_manual_reload_radius("tfvr_rocket_manual_reload_radius", "14", FCVAR_ARCHIVE, "Distance in inches from offhand to rocket launcher muzzle required to start manual rocket load");

// Offhand grip convars - grip button must be held to activate
ConVar tfvr_offhand_grip_enabled("tfvr_offhand_grip_enabled", "1", FCVAR_ARCHIVE, "Enable offhand grip for two-handed weapon aiming");
ConVar tfvr_offhand_grip_range("tfvr_offhand_grip_range", "25", FCVAR_ARCHIVE, "Distance (cm) at which offhand grip can activate");
ConVar tfvr_offhand_grip_shotgun_range("tfvr_offhand_grip_shotgun_range", "20", FCVAR_ARCHIVE, "Distance (cm) at which offhand grip can activate for pump-action shotguns");
ConVar tfvr_offhand_grip_release_mult("tfvr_offhand_grip_release_mult", "6", FCVAR_ARCHIVE, "Multiplier for release distance (hysteresis to prevent accidental ungrip)");
ConVar tfvr_offhand_grip_shotgun_release_mult("tfvr_offhand_grip_shotgun_release_mult", "6", FCVAR_ARCHIVE, "Multiplier for pump-action shotgun offhand grip release distance");
ConVar tfvr_offhand_grip_threshold("tfvr_offhand_grip_threshold", "0.5", FCVAR_ARCHIVE, "Grip button threshold (0-1) to activate offhand grip");
ConVar tfvr_offhand_grip_blend_speed("tfvr_offhand_grip_blend_speed", "15", FCVAR_ARCHIVE, "Speed of hand position grip/ungrip transition (higher = faster)");
ConVar tfvr_offhand_grip_rotation_blend_speed("tfvr_offhand_grip_rotation_blend_speed", "8", FCVAR_ARCHIVE, "Speed of weapon rotation grip/ungrip transition (higher = faster)");
ConVar tfvr_offhand_grip_ease_power("tfvr_offhand_grip_ease_power", "1.1", FCVAR_ARCHIVE, "Easing power for grip transitions (1=linear, 2+=ease-out, higher=sharper)");
ConVar tfvr_offhand_grip_no_anchor("tfvr_offhand_grip_no_anchor", "0", FCVAR_CHEAT, "DEBUG: Disable anchor offset when gripping (use controller directly)");
ConVar tfvr_medigun_lever_grip_release_mult("tfvr_medigun_lever_grip_release_mult", "3.5", FCVAR_ARCHIVE, "VR medigun lever: release distance multiplier when grip is active (higher = harder to detach)");
ConVar tfvr_medigun_lever_grip_blend_speed("tfvr_medigun_lever_grip_blend_speed", "25", FCVAR_ARCHIVE, "VR medigun lever: blend speed when snapping to grip point (higher = firmer lock)");
ConVar tfvr_hands_left_offset_roll("tfvr_hands_left_offset_roll", "0", FCVAR_ARCHIVE, "Roll offset for left VR hand (degrees)");

// Rotation offset convars - right hand (DEFAULT/GLOBAL offsets)
ConVar tfvr_hands_right_offset_pitch("tfvr_hands_right_offset_pitch", "0", FCVAR_ARCHIVE, "Pitch offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_yaw("tfvr_hands_right_offset_yaw", "0", FCVAR_ARCHIVE, "Yaw offset for right VR hand (degrees)");
ConVar tfvr_hands_right_offset_roll("tfvr_hands_right_offset_roll", "0", FCVAR_ARCHIVE, "Roll offset for right VR hand (degrees)");
ConVar tfvr_backstab_debug("tfvr_backstab_debug", "0", FCVAR_NONE, "Force backstab-ready visual on for testing");
ConVar tfvr_backstab_speed("tfvr_backstab_speed", "1.0", FCVAR_NONE, "Backstab raise/lower animation speed multiplier");
ConVar tfvr_backstab_duration("tfvr_backstab_duration", "0.35", FCVAR_NONE, "Minimum backstab transition duration in seconds (overrides animation length if shorter)");

// Per-class offset enable - when enabled, class-specific offsets override global offsets
ConVar tfvr_hands_perclass_offsets("tfvr_hands_perclass_offsets", "1", FCVAR_ARCHIVE, "Enable per-class hand offsets (0=use global, 1=use per-class)");

// Per-class offset convars - LEFT hand
// Scout
ConVar tfvr_hands_scout_left_pitch("tfvr_hands_scout_left_pitch", "0", FCVAR_ARCHIVE, "Scout: left hand pitch offset");
ConVar tfvr_hands_scout_left_yaw("tfvr_hands_scout_left_yaw", "90", FCVAR_ARCHIVE, "Scout: left hand yaw offset");
ConVar tfvr_hands_scout_left_roll("tfvr_hands_scout_left_roll", "0", FCVAR_ARCHIVE, "Scout: left hand roll offset");
// Soldier
ConVar tfvr_hands_soldier_left_pitch("tfvr_hands_soldier_left_pitch", "0", FCVAR_ARCHIVE, "Soldier: left hand pitch offset");
ConVar tfvr_hands_soldier_left_yaw("tfvr_hands_soldier_left_yaw", "95", FCVAR_ARCHIVE, "Soldier: left hand yaw offset");
ConVar tfvr_hands_soldier_left_roll("tfvr_hands_soldier_left_roll", "0", FCVAR_ARCHIVE, "Soldier: left hand roll offset");
// Pyro
ConVar tfvr_hands_pyro_left_pitch("tfvr_hands_pyro_left_pitch", "0", FCVAR_ARCHIVE, "Pyro: left hand pitch offset");
ConVar tfvr_hands_pyro_left_yaw("tfvr_hands_pyro_left_yaw", "90", FCVAR_ARCHIVE, "Pyro: left hand yaw offset");
ConVar tfvr_hands_pyro_left_roll("tfvr_hands_pyro_left_roll", "0", FCVAR_ARCHIVE, "Pyro: left hand roll offset");
// Demoman
ConVar tfvr_hands_demo_left_pitch("tfvr_hands_demo_left_pitch", "0", FCVAR_ARCHIVE, "Demo: left hand pitch offset");
ConVar tfvr_hands_demo_left_yaw("tfvr_hands_demo_left_yaw", "95", FCVAR_ARCHIVE, "Demo: left hand yaw offset");
ConVar tfvr_hands_demo_left_roll("tfvr_hands_demo_left_roll", "0", FCVAR_ARCHIVE, "Demo: left hand roll offset");
// Heavy
ConVar tfvr_hands_heavy_left_pitch("tfvr_hands_heavy_left_pitch", "0", FCVAR_ARCHIVE, "Heavy: left hand pitch offset");
ConVar tfvr_hands_heavy_left_yaw("tfvr_hands_heavy_left_yaw", "99", FCVAR_ARCHIVE, "Heavy: left hand yaw offset");
ConVar tfvr_hands_heavy_left_roll("tfvr_hands_heavy_left_roll", "0", FCVAR_ARCHIVE, "Heavy: left hand roll offset");
// Engineer
ConVar tfvr_hands_engi_left_pitch("tfvr_hands_engi_left_pitch", "0", FCVAR_ARCHIVE, "Engineer: left hand pitch offset");
ConVar tfvr_hands_engi_left_yaw("tfvr_hands_engi_left_yaw", "95", FCVAR_ARCHIVE, "Engineer: left hand yaw offset");
ConVar tfvr_hands_engi_left_roll("tfvr_hands_engi_left_roll", "0", FCVAR_ARCHIVE, "Engineer: left hand roll offset");
// Medic
ConVar tfvr_hands_medic_left_pitch("tfvr_hands_medic_left_pitch", "0", FCVAR_ARCHIVE, "Medic: left hand pitch offset");
ConVar tfvr_hands_medic_left_yaw("tfvr_hands_medic_left_yaw", "95", FCVAR_ARCHIVE, "Medic: left hand yaw offset");
ConVar tfvr_hands_medic_left_roll("tfvr_hands_medic_left_roll", "0", FCVAR_ARCHIVE, "Medic: left hand roll offset");
// Sniper
ConVar tfvr_hands_sniper_left_pitch("tfvr_hands_sniper_left_pitch", "0", FCVAR_ARCHIVE, "Sniper: left hand pitch offset");
ConVar tfvr_hands_sniper_left_yaw("tfvr_hands_sniper_left_yaw", "90", FCVAR_ARCHIVE, "Sniper: left hand yaw offset");
ConVar tfvr_hands_sniper_left_roll("tfvr_hands_sniper_left_roll", "0", FCVAR_ARCHIVE, "Sniper: left hand roll offset");
// Spy
ConVar tfvr_hands_spy_left_pitch("tfvr_hands_spy_left_pitch", "0", FCVAR_ARCHIVE, "Spy: left hand pitch offset");
ConVar tfvr_hands_spy_left_yaw("tfvr_hands_spy_left_yaw", "90", FCVAR_ARCHIVE, "Spy: left hand yaw offset");
ConVar tfvr_hands_spy_left_roll("tfvr_hands_spy_left_roll", "0", FCVAR_ARCHIVE, "Spy: left hand roll offset");

// Per-class offset convars - RIGHT hand
// Scout
ConVar tfvr_hands_scout_right_pitch("tfvr_hands_scout_right_pitch", "180", FCVAR_ARCHIVE, "Scout: right hand pitch offset");
ConVar tfvr_hands_scout_right_yaw("tfvr_hands_scout_right_yaw", "-90", FCVAR_ARCHIVE, "Scout: right hand yaw offset");
ConVar tfvr_hands_scout_right_roll("tfvr_hands_scout_right_roll", "0", FCVAR_ARCHIVE, "Scout: right hand roll offset");
// Soldier
ConVar tfvr_hands_soldier_right_pitch("tfvr_hands_soldier_right_pitch", "180", FCVAR_ARCHIVE, "Soldier: right hand pitch offset");
ConVar tfvr_hands_soldier_right_yaw("tfvr_hands_soldier_right_yaw", "-90", FCVAR_ARCHIVE, "Soldier: right hand yaw offset");
ConVar tfvr_hands_soldier_right_roll("tfvr_hands_soldier_right_roll", "0", FCVAR_ARCHIVE, "Soldier: right hand roll offset");
// Pyro
ConVar tfvr_hands_pyro_right_pitch("tfvr_hands_pyro_right_pitch", "180", FCVAR_ARCHIVE, "Pyro: right hand pitch offset");
ConVar tfvr_hands_pyro_right_yaw("tfvr_hands_pyro_right_yaw", "-90", FCVAR_ARCHIVE, "Pyro: right hand yaw offset");
ConVar tfvr_hands_pyro_right_roll("tfvr_hands_pyro_right_roll", "0", FCVAR_ARCHIVE, "Pyro: right hand roll offset");
// Demoman
ConVar tfvr_hands_demo_right_pitch("tfvr_hands_demo_right_pitch", "180", FCVAR_ARCHIVE, "Demo: right hand pitch offset");
ConVar tfvr_hands_demo_right_yaw("tfvr_hands_demo_right_yaw", "-95", FCVAR_ARCHIVE, "Demo: right hand yaw offset");
ConVar tfvr_hands_demo_right_roll("tfvr_hands_demo_right_roll", "0", FCVAR_ARCHIVE, "Demo: right hand roll offset");
// Heavy
ConVar tfvr_hands_heavy_right_pitch("tfvr_hands_heavy_right_pitch", "180", FCVAR_ARCHIVE, "Heavy: right hand pitch offset");
ConVar tfvr_hands_heavy_right_yaw("tfvr_hands_heavy_right_yaw", "-95", FCVAR_ARCHIVE, "Heavy: right hand yaw offset");
ConVar tfvr_hands_heavy_right_roll("tfvr_hands_heavy_right_roll", "0", FCVAR_ARCHIVE, "Heavy: right hand roll offset");
// Engineer
ConVar tfvr_hands_engi_right_pitch("tfvr_hands_engi_right_pitch", "180", FCVAR_ARCHIVE, "Engineer: right hand pitch offset");
ConVar tfvr_hands_engi_right_yaw("tfvr_hands_engi_right_yaw", "-95", FCVAR_ARCHIVE, "Engineer: right hand yaw offset");
ConVar tfvr_hands_engi_right_roll("tfvr_hands_engi_right_roll", "0", FCVAR_ARCHIVE, "Engineer: right hand roll offset");
// Medic
ConVar tfvr_hands_medic_right_pitch("tfvr_hands_medic_right_pitch", "180", FCVAR_ARCHIVE, "Medic: right hand pitch offset");
ConVar tfvr_hands_medic_right_yaw("tfvr_hands_medic_right_yaw", "-95", FCVAR_ARCHIVE, "Medic: right hand yaw offset");
ConVar tfvr_hands_medic_right_roll("tfvr_hands_medic_right_roll", "0", FCVAR_ARCHIVE, "Medic: right hand roll offset");
// Sniper
ConVar tfvr_hands_sniper_right_pitch("tfvr_hands_sniper_right_pitch", "180", FCVAR_ARCHIVE, "Sniper: right hand pitch offset");
ConVar tfvr_hands_sniper_right_yaw("tfvr_hands_sniper_right_yaw", "-95", FCVAR_ARCHIVE, "Sniper: right hand yaw offset");
ConVar tfvr_hands_sniper_right_roll("tfvr_hands_sniper_right_roll", "0", FCVAR_ARCHIVE, "Sniper: right hand roll offset");
// Spy
ConVar tfvr_hands_spy_right_pitch("tfvr_hands_spy_right_pitch", "180", FCVAR_ARCHIVE, "Spy: right hand pitch offset");
ConVar tfvr_hands_spy_right_yaw("tfvr_hands_spy_right_yaw", "-90", FCVAR_ARCHIVE, "Spy: right hand yaw offset");
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
ConVar tfvr_hands_scout_left_y("tfvr_hands_scout_left_y", "5", FCVAR_ARCHIVE, "Scout: left hand position Y");
ConVar tfvr_hands_scout_left_z("tfvr_hands_scout_left_z", "0.8", FCVAR_ARCHIVE, "Scout: left hand position Z");
ConVar tfvr_hands_soldier_left_x("tfvr_hands_soldier_left_x", "0", FCVAR_ARCHIVE, "Soldier: left hand position X");
ConVar tfvr_hands_soldier_left_y("tfvr_hands_soldier_left_y", "3", FCVAR_ARCHIVE, "Soldier: left hand position Y");
ConVar tfvr_hands_soldier_left_z("tfvr_hands_soldier_left_z", "-0.8", FCVAR_ARCHIVE, "Soldier: left hand position Z");
ConVar tfvr_hands_pyro_left_x("tfvr_hands_pyro_left_x", "0", FCVAR_ARCHIVE, "Pyro: left hand position X");
ConVar tfvr_hands_pyro_left_y("tfvr_hands_pyro_left_y", "5", FCVAR_ARCHIVE, "Pyro: left hand position Y");
ConVar tfvr_hands_pyro_left_z("tfvr_hands_pyro_left_z", "0", FCVAR_ARCHIVE, "Pyro: left hand position Z");
ConVar tfvr_hands_demo_left_x("tfvr_hands_demo_left_x", "0", FCVAR_ARCHIVE, "Demo: left hand position X");
ConVar tfvr_hands_demo_left_y("tfvr_hands_demo_left_y", "3", FCVAR_ARCHIVE, "Demo: left hand position Y");
ConVar tfvr_hands_demo_left_z("tfvr_hands_demo_left_z", "-0.8", FCVAR_ARCHIVE, "Demo: left hand position Z");
ConVar tfvr_hands_heavy_left_x("tfvr_hands_heavy_left_x", "1", FCVAR_ARCHIVE, "Heavy: left hand position X");
ConVar tfvr_hands_heavy_left_y("tfvr_hands_heavy_left_y", "5.6", FCVAR_ARCHIVE, "Heavy: left hand position Y");
ConVar tfvr_hands_heavy_left_z("tfvr_hands_heavy_left_z", "-0.8", FCVAR_ARCHIVE, "Heavy: left hand position Z");
ConVar tfvr_hands_engi_left_x("tfvr_hands_engi_left_x", "0", FCVAR_ARCHIVE, "Engineer: left hand position X");
ConVar tfvr_hands_engi_left_y("tfvr_hands_engi_left_y", "3", FCVAR_ARCHIVE, "Engineer: left hand position Y");
ConVar tfvr_hands_engi_left_z("tfvr_hands_engi_left_z", "-0.8", FCVAR_ARCHIVE, "Engineer: left hand position Z");
ConVar tfvr_hands_medic_left_x("tfvr_hands_medic_left_x", "0", FCVAR_ARCHIVE, "Medic: left hand position X");
ConVar tfvr_hands_medic_left_y("tfvr_hands_medic_left_y", "3", FCVAR_ARCHIVE, "Medic: left hand position Y");
ConVar tfvr_hands_medic_left_z("tfvr_hands_medic_left_z", "-0.8", FCVAR_ARCHIVE, "Medic: left hand position Z");
ConVar tfvr_hands_sniper_left_x("tfvr_hands_sniper_left_x", "1", FCVAR_ARCHIVE, "Sniper: left hand position X");
ConVar tfvr_hands_sniper_left_y("tfvr_hands_sniper_left_y", "4", FCVAR_ARCHIVE, "Sniper: left hand position Y");
ConVar tfvr_hands_sniper_left_z("tfvr_hands_sniper_left_z", "0", FCVAR_ARCHIVE, "Sniper: left hand position Z");
ConVar tfvr_hands_spy_left_x("tfvr_hands_spy_left_x", ".5", FCVAR_ARCHIVE, "Spy: left hand position X");
ConVar tfvr_hands_spy_left_y("tfvr_hands_spy_left_y", "3", FCVAR_ARCHIVE, "Spy: left hand position Y");
ConVar tfvr_hands_spy_left_z("tfvr_hands_spy_left_z", "0", FCVAR_ARCHIVE, "Spy: left hand position Z");

// Per-class positional offset convars - RIGHT hand
ConVar tfvr_hands_scout_right_x("tfvr_hands_scout_right_x", "0", FCVAR_ARCHIVE, "Scout: right hand position X");
ConVar tfvr_hands_scout_right_y("tfvr_hands_scout_right_y", "-5", FCVAR_ARCHIVE, "Scout: right hand position Y");
ConVar tfvr_hands_scout_right_z("tfvr_hands_scout_right_z", "0", FCVAR_ARCHIVE, "Scout: right hand position Z");
ConVar tfvr_hands_soldier_right_x("tfvr_hands_soldier_right_x", "0", FCVAR_ARCHIVE, "Soldier: right hand position X");
ConVar tfvr_hands_soldier_right_y("tfvr_hands_soldier_right_y", "-3", FCVAR_ARCHIVE, "Soldier: right hand position Y");
ConVar tfvr_hands_soldier_right_z("tfvr_hands_soldier_right_z", "0.8", FCVAR_ARCHIVE, "Soldier: right hand position Z");
ConVar tfvr_hands_pyro_right_x("tfvr_hands_pyro_right_x", "0", FCVAR_ARCHIVE, "Pyro: right hand position X");
ConVar tfvr_hands_pyro_right_y("tfvr_hands_pyro_right_y", "-5", FCVAR_ARCHIVE, "Pyro: right hand position Y");
ConVar tfvr_hands_pyro_right_z("tfvr_hands_pyro_right_z", "0", FCVAR_ARCHIVE, "Pyro: right hand position Z");
ConVar tfvr_hands_demo_right_x("tfvr_hands_demo_right_x", "0", FCVAR_ARCHIVE, "Demo: right hand position X");
ConVar tfvr_hands_demo_right_y("tfvr_hands_demo_right_y", "-3", FCVAR_ARCHIVE, "Demo: right hand position Y");
ConVar tfvr_hands_demo_right_z("tfvr_hands_demo_right_z", "0.8", FCVAR_ARCHIVE, "Demo: right hand position Z");
ConVar tfvr_hands_heavy_right_x("tfvr_hands_heavy_right_x", "0", FCVAR_ARCHIVE, "Heavy: right hand position X");
ConVar tfvr_hands_heavy_right_y("tfvr_hands_heavy_right_y", "-5.6", FCVAR_ARCHIVE, "Heavy: right hand position Y");
ConVar tfvr_hands_heavy_right_z("tfvr_hands_heavy_right_z", "0.8", FCVAR_ARCHIVE, "Heavy: right hand position Z");
ConVar tfvr_hands_engi_right_x("tfvr_hands_engi_right_x", "0", FCVAR_ARCHIVE, "Engineer: right hand position X");
ConVar tfvr_hands_engi_right_y("tfvr_hands_engi_right_y", "-3", FCVAR_ARCHIVE, "Engineer: right hand position Y");
ConVar tfvr_hands_engi_right_z("tfvr_hands_engi_right_z", "0.8", FCVAR_ARCHIVE, "Engineer: right hand position Z");
ConVar tfvr_hands_medic_right_x("tfvr_hands_medic_right_x", "0", FCVAR_ARCHIVE, "Medic: right hand position X");
ConVar tfvr_hands_medic_right_y("tfvr_hands_medic_right_y", "-3", FCVAR_ARCHIVE, "Medic: right hand position Y");
ConVar tfvr_hands_medic_right_z("tfvr_hands_medic_right_z", "0.8", FCVAR_ARCHIVE, "Medic: right hand position Z");
ConVar tfvr_hands_sniper_right_x("tfvr_hands_sniper_right_x", "0", FCVAR_ARCHIVE, "Sniper: right hand position X");
ConVar tfvr_hands_sniper_right_y("tfvr_hands_sniper_right_y", "-3", FCVAR_ARCHIVE, "Sniper: right hand position Y");
ConVar tfvr_hands_sniper_right_z("tfvr_hands_sniper_right_z", "0.8", FCVAR_ARCHIVE, "Sniper: right hand position Z");
ConVar tfvr_hands_spy_right_x("tfvr_hands_spy_right_x", "1.2", FCVAR_ARCHIVE, "Spy: right hand position X");
ConVar tfvr_hands_spy_right_y("tfvr_hands_spy_right_y", "-3", FCVAR_ARCHIVE, "Spy: right hand position Y");
ConVar tfvr_hands_spy_right_z("tfvr_hands_spy_right_z", "0.8", FCVAR_ARCHIVE, "Spy: right hand position Z");

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
ConVar tfvr_weapon_fire_anim_pos_scale("tfvr_weapon_fire_anim_pos_scale", "1", FCVAR_ARCHIVE, "Extra scale for position offset (on top of main scale)");
ConVar tfvr_weapon_fire_anim_pitch_scale("tfvr_weapon_fire_anim_pitch_scale", "1", FCVAR_ARCHIVE, "Scale/invert pitch rotation (negative to flip)");
ConVar tfvr_weapon_fire_anim_yaw_scale("tfvr_weapon_fire_anim_yaw_scale", "1", FCVAR_ARCHIVE, "Scale/invert yaw rotation (negative to flip)");
ConVar tfvr_weapon_fire_anim_roll_scale("tfvr_weapon_fire_anim_roll_scale", "1", FCVAR_ARCHIVE, "Scale/invert roll rotation (negative to flip)");
ConVar tfvr_weapon_fire_anim_pos_rotation("tfvr_weapon_fire_anim_pos_rotation", "90", FCVAR_ARCHIVE, "Rotation correction for position vector (degrees around Z axis)");
ConVar tfvr_weapon_fire_anim_angle_rotation("tfvr_weapon_fire_anim_angle_rotation", "180", FCVAR_ARCHIVE, "Coordinate space rotation for fire animation (degrees around Z axis)");

// Weapon draw animation convars
ConVar tfvr_weapon_draw_anim("tfvr_weapon_draw_anim", "1", FCVAR_ARCHIVE, "Enable draw animations on VR-held weapons when switching");
ConVar tfvr_weapon_draw_anim_debug("tfvr_weapon_draw_anim_debug", "0", FCVAR_NONE, "Debug draw animation triggering");

// Weapon-specific aim angle corrections (for weapons with incorrect muzzle attachment orientation)
// These are applied as LOCAL rotations relative to the muzzle attachment frame
ConVar tfvr_aim_grenadelauncher_pitch("tfvr_aim_grenadelauncher_pitch", "-90", FCVAR_ARCHIVE, "Pitch correction for grenade launcher aim (degrees, local space)");
ConVar tfvr_aim_grenadelauncher_yaw("tfvr_aim_grenadelauncher_yaw", "0", FCVAR_ARCHIVE, "Yaw correction for grenade launcher aim (degrees, local space)");
ConVar tfvr_aim_grenadelauncher_roll("tfvr_aim_grenadelauncher_roll", "-90", FCVAR_ARCHIVE, "Roll correction for grenade launcher aim (degrees, local space)");
ConVar tfvr_aim_stickybomb_pitch("tfvr_aim_stickybomb_pitch", "-90", FCVAR_ARCHIVE, "Pitch correction for sticky launcher aim (degrees, local space)");
ConVar tfvr_aim_stickybomb_yaw("tfvr_aim_stickybomb_yaw", "0", FCVAR_ARCHIVE, "Yaw correction for sticky launcher aim (degrees, local space)");
ConVar tfvr_aim_stickybomb_roll("tfvr_aim_stickybomb_roll", "-90", FCVAR_ARCHIVE, "Roll correction for sticky launcher aim (degrees, local space)");

// Crusader's Crossbow muzzle offset from weapon_bone (model has no muzzle attachment)
ConVar tfvr_crossbow_muzzle_fwd("tfvr_crossbow_muzzle_fwd", "0", FCVAR_ARCHIVE, "Crossbow muzzle forward offset from weapon_bone (units)");
ConVar tfvr_crossbow_muzzle_right("tfvr_crossbow_muzzle_right", "-9", FCVAR_ARCHIVE, "Crossbow muzzle right offset from weapon_bone (units)");
ConVar tfvr_crossbow_muzzle_up("tfvr_crossbow_muzzle_up", "22", FCVAR_ARCHIVE, "Crossbow muzzle up offset from weapon_bone (units)");
ConVar tfvr_crossbow_muzzle_pitch("tfvr_crossbow_muzzle_pitch", "-90", FCVAR_ARCHIVE, "Crossbow muzzle pitch rotation relative to weapon_bone (degrees)");
ConVar tfvr_crossbow_muzzle_yaw("tfvr_crossbow_muzzle_yaw", "0", FCVAR_ARCHIVE, "Crossbow muzzle yaw rotation relative to weapon_bone (degrees)");
ConVar tfvr_crossbow_muzzle_roll("tfvr_crossbow_muzzle_roll", "0", FCVAR_ARCHIVE, "Crossbow muzzle roll rotation relative to weapon_bone (degrees)");

// Aim stabilization - counteracts grip/trigger-induced palm wobble on weapon aim
ConVar tfvr_aim_stabilize("tfvr_aim_stabilize", "1", FCVAR_ARCHIVE, "Stabilize weapon aim against grip-induced palm movement (0=off, 1=on)");
ConVar tfvr_aim_stabilize_adapt("tfvr_aim_stabilize_adapt", "0.0", FCVAR_ARCHIVE, "Rate at which aim reference adapts to current grip (per second, 0=never)");
ConVar tfvr_aim_stabilize_debug("tfvr_aim_stabilize_debug", "0", FCVAR_NONE, "Show aim stabilization debug info");

// Global storage for active VR hands - since we only support local player, use two pointers
static C_TFVRHand *g_pLocalPlayerLeftHand = NULL;
static C_TFVRHand *g_pLocalPlayerRightHand = NULL;
static const float kPomsonGripLockBlend = 0.995f;

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

static void TFVR_BlendTransforms( const matrix3x4_t &from, const matrix3x4_t &to, float t, matrix3x4_t &out )
{
	t = clamp( t, 0.0f, 1.0f );

	Vector fromPos, toPos, blendedPos;
	Quaternion fromQuat, toQuat, blendedQuat;
	MatrixAngles( from, fromQuat, fromPos );
	MatrixAngles( to, toQuat, toPos );

	VectorLerp( fromPos, toPos, t, blendedPos );
	SafeQuaternionSlerp( fromQuat, toQuat, t, blendedQuat );
	QuaternionMatrix( blendedQuat, blendedPos, out );
}

static void TFVR_GetReflectSigns(float sign[3])
{
	sign[0] = 1.0f;
	sign[1] = 1.0f;
	sign[2] = 1.0f;

	extern ConVar tfvr_lefthand_mirror_axis;
	extern ConVar tfvr_lefthand_mirror_spin;

	const int reflectAxis = clamp( tfvr_lefthand_mirror_axis.GetInt(), 0, 2 );
	sign[reflectAxis] = -sign[reflectAxis];

	const int spin = tfvr_lefthand_mirror_spin.GetInt();
	if ( spin >= 1 && spin <= 3 )
	{
		const int spinAxis = spin - 1; // 1->X,2->Y,3->Z
		for ( int a = 0; a < 3; ++a )
		{
			if ( a != spinAxis )
				sign[a] = -sign[a];
		}
	}
}

static void TFVR_ReflectVectorInControllerFrame(Vector &vec, const matrix3x4_t &controllerFrame)
{
	float sign[3];
	TFVR_GetReflectSigns(sign);

	Vector axis[3];
	MatrixGetColumn(controllerFrame, 0, axis[0]);
	MatrixGetColumn(controllerFrame, 1, axis[1]);
	MatrixGetColumn(controllerFrame, 2, axis[2]);

	Vector reflected = vec3_origin;
	for (int i = 0; i < 3; ++i)
	{
		axis[i].NormalizeInPlace();
		reflected += axis[i] * (DotProduct(vec, axis[i]) * sign[i]);
	}

	vec = reflected;
}

//-----------------------------------------------------------------------------
// Purpose: Reflect a set of finished world-space bones in the given controller
//          frame. The reflection is expressed in the frame's LOCAL space, so it
//          is rigidly attached to the controller: the whole mirrored skeleton
//          follows the controller's motion (roll/pitch/yaw track correctly). A
//          world-fixed reflection would instead reverse the sense of in-plane
//          rotations, making roll/yaw track backwards.
//
//          By default we negate the frame's lateral (Y/left) row, i.e. reflect
//          across the plane that contains the aim (X) and up (Z) axes -- the
//          anatomical left<->right mirror that preserves the aim direction.
//          The axis/spin convars allow tuning. The determinant flips (odd
//          number of negated rows), so the renderer draws with CW culling.
//-----------------------------------------------------------------------------
static void TFVR_ReflectBonesInControllerFrame(matrix3x4_t *pBones, int nBoneCount, const matrix3x4_t &controllerFrame)
{
	if ( !pBones || nBoneCount <= 0 )
		return;

	// Build a per-row sign vector for the improper transform in frame-local
	// space. Start with a single-axis reflection (negate one row -> determinant
	// -1), then optionally compose a 180-degree spin about another local axis
	// (negate the two rows perpendicular to it -> determinant +1, so the product
	// stays an odd reflection and the renderer's CW culling remains correct).
	float sign[3];
	TFVR_GetReflectSigns(sign);

	matrix3x4_t invFrame;
	MatrixInvert( controllerFrame, invFrame );

	for ( int i = 0; i < nBoneCount; ++i )
	{
		matrix3x4_t local;
		ConcatTransforms( invFrame, pBones[i], local );

		for ( int r = 0; r < 3; ++r )
		{
			if ( sign[r] < 0.0f )
			{
				local[r][0] = -local[r][0];
				local[r][1] = -local[r][1];
				local[r][2] = -local[r][2];
				local[r][3] = -local[r][3];
			}
		}

		ConcatTransforms( controllerFrame, local, pBones[i] );
	}
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
// Purpose: Rotate 'transform' about its origin (preserving position) so that the
//          world-space direction of 'localDir' (a unit direction expressed in
//          transform's local frame) points along 'desiredWorld'. Used for the
//          bow: align the arrow axis (not the hand's Y axis) to the draw line so
//          the nock points at the draw hand.
//-----------------------------------------------------------------------------
static void AlignLocalAxisToWorldDir(matrix3x4_t &transform, const Vector &localDir, const Vector &desiredWorld)
{
	Vector pos;
	MatrixGetColumn(transform, 3, pos);

	Vector curWorld;
	VectorRotate(localDir, transform, curWorld);
	if (curWorld.NormalizeInPlace() < 1e-4f)
		return;

	Vector tgt = desiredWorld;
	if (tgt.NormalizeInPlace() < 1e-4f)
		return;

	float dotProduct = clamp(DotProduct(curWorld, tgt), -1.0f, 1.0f);
	if (dotProduct > 0.9999f)
		return;

	Vector rotationAxis;
	if (dotProduct < -0.9999f)
	{
		rotationAxis = CrossProduct(curWorld, Vector(0, 0, 1));
		if (rotationAxis.LengthSqr() < 1e-4f)
			rotationAxis = CrossProduct(curWorld, Vector(1, 0, 0));
	}
	else
	{
		rotationAxis = CrossProduct(curWorld, tgt);
	}
	rotationAxis.NormalizeInPlace();

	float angle = acosf(dotProduct);
	float halfAngle = angle * 0.5f;
	float sinHalf = sinf(halfAngle);

	Quaternion rotQuat;
	rotQuat.x = rotationAxis.x * sinHalf;
	rotQuat.y = rotationAxis.y * sinHalf;
	rotQuat.z = rotationAxis.z * sinHalf;
	rotQuat.w = cosf(halfAngle);

	Quaternion origQuat;
	MatrixQuaternion(transform, origQuat);

	Quaternion newQuat;
	QuaternionMult(rotQuat, origQuat, newQuat);

	QuaternionMatrix(newQuat, pos, transform);
}

//-----------------------------------------------------------------------------
// Purpose: Check if an engineer player has the Gunslinger (robot arm) equipped
//-----------------------------------------------------------------------------
static bool IsPlayerUsingGunslinger(C_TFPlayer *pPlayer)
{
	if (!pPlayer)
		return false;

	if (pPlayer->GetPlayerClass()->GetClassIndex() != TF_CLASS_ENGINEER)
		return false;

	CBaseEntity *pMelee = pPlayer->GetEntityForLoadoutSlot(LOADOUT_POSITION_MELEE);
	if (!pMelee)
		return false;

	return V_stristr(pMelee->GetClassname(), "robot_arm") != NULL;
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
	m_handSide = VR_HAND_LEFT;
	m_hOwnerPlayer = NULL;
	m_hHeldWeapon = NULL;
	m_iLastEquippedWeaponID = -1;
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
	m_bLoopIdleOnHand = false;
	m_bPlayingFireAnim = false;
	m_flFireAnimStartTime = 0.0f;
	m_iDrawSequence = -1;
	m_bPlayingDrawAnim = false;
	m_flDrawAnimStartTime = 0.0f;
	m_eDrawAnimScope = VR_DRAW_ANIM_NONE;
	m_iChargeSequence = -1;
	m_iChargeSequence2 = -1;
	m_iBowIdleSequence = -1;
	m_flBowIdleCycle = 0.0f;
	m_flBowFireEndCycle = 1.0f;
	m_flBowFireFirstFrameCycle = 0.0f;
	m_iBowDrawSequence = -1;
	m_flBowDrawNockCycle = 0.0f;
	m_iBowChargeIdleSequence = -1;
	m_iBowShakeOverlaySequence = -1;
	m_flBowShakeOverlayStartTime = 0.0f;
	m_bBowShakeOverlayActive = false;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;
	m_bPlayingChargeAnim = false;
	m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
	m_iFireOnSequence = -1;
	m_iFireOffSequence = -1;
	m_bMedigunWasHealing = false;
	m_bMedigunLeverActive = false;
	m_iMedigunLeverSeq = -1;
	m_flMedigunLeverCycle = 0.0f;
	m_bMedigunBodyPastHalf = false;
	m_bMedigunGripTargetValid = false;
	SetIdentityMatrix(m_matMedigunGripTarget);
	m_bFlamethrowerWasFiring = false;
	m_flFlamethrowerFireBlend = 0.0f;
	m_vecLastValidPosition = vec3_origin;
	m_angLastValidAngles = vec3_angle;
	m_szModelName[0] = '\0';
	m_bHasGunslinger = false;
	m_bPoseAsLeftHand = (m_handSide == VR_HAND_LEFT);
	m_bReflectPoseActive = false;
	SetIdentityMatrix(m_matReflectFrame);
	SetIdentityMatrix(m_matIdleHandBoneTransform);
	m_vecIdleHandBoneLocalPos.Init();
	m_bHandBoneOffsetValid = false;
	m_iHandBone = -1;
	m_bBisonUseReloadGrip = false;
	m_bManglerUseReloadGrip = false;
	m_bPomsonUseReloadGrip = false;
	m_bRightHandDetached = false;
	m_bPomsonRightGripLatched = false;
	m_bPomsonRightGripWasPressed = false;
	m_bPomsonSuppressPassiveGripPoint = false;
	m_bPomsonSuppressReloadGripPoint = false;
	SetIdentityMatrix(m_matPomsonRightLatchOffset);
	m_bPomsonRightLatchOffsetValid = false;
	SetIdentityMatrix(m_matPomsonRightGripLastWorld);
	m_bPomsonRightGripLastWorldValid = false;
	SetIdentityMatrix(m_matPomsonRightUnlatchStart);
	m_bPomsonRightUnlatchStartValid = false;
	m_bPomsonRightUnlatchUseReloadGrip = false;
	SetIdentityMatrix(m_matPomsonDetachLeftToWeaponBone);
	m_bPomsonDetachLeftToWeaponBoneValid = false;
	SetIdentityMatrix(m_matPomsonDetachLeftToLeftHandBone);
	m_bPomsonDetachLeftToLeftHandBoneValid = false;
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
	SetIdentityMatrix(m_matLiveWeaponBoneWorld);
	m_bLiveWeaponBoneWorldValid = false;
	SetIdentityMatrix(m_matLiveBowStringBoneWorld);
	m_bLiveBowStringBoneWorldValid = false;

	// Cached muzzle position
	m_vecCachedMuzzlePos = vec3_origin;
	m_angCachedMuzzleAngles = vec3_angle;
	m_bCachedMuzzleValid = false;

	// Melee swing cycling
	m_iMeleeSwingIndex = 0;
	m_szMeleeSwingBase[0] = '\0';
	m_iMeleeSwingCount = 0;
	m_bPrevVRSwingActive = false;
	m_iBreadBiteCritSeq = -1;
	m_iBreadBiteIdleSeqs[0] = m_iBreadBiteIdleSeqs[1] = m_iBreadBiteIdleSeqs[2] = -1;
	m_bIsBreadBite = false;
	m_flBreadBiteIdleStartTime = 0.0f;
	m_iBBCrossfadeFromSeq = -1;
	m_flBBCrossfadeFromCycle = 0.0f;
	m_flBBCrossfadeStart = 0.0f;
	m_iBBLastSampledSeq = -1;
	m_flBBLastSampledCycle = 0.0f;
	m_flBBLastCrossfadeCheck = 0.0f;

	// Backstab ready animation
	m_iBackstabUpSequence = -1;
	m_iBackstabDownSequence = -1;
	m_iBackstabIdleSequence = -1;
	m_iBackstabAttackSequence = -1;
	m_bBackstabReady = false;
	m_bBackstabAttacking = false;
	m_flBackstabCycle = 0.0f;
	m_bBackstabRaising = false;
	m_bBackstabLowering = false;
	m_flLastBackstabUpdateTime = 0.0f;
	m_bHasIdleWeaponBone = false;
	SetIdentityMatrix( m_matIdleWeaponBoneLocal );
	SetIdentityMatrix( m_matIdleWeaponBoneWorld );
	m_bOffHandToWeaponBoneValid = false;
	SetIdentityMatrix( m_matOffHandToWeaponBone );

	// Scattergun reload animation
	m_iReloadStartSequence = -1;
	m_iReloadLoopSequence = -1;
	m_iReloadEndSequence = -1;
	m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
	m_flReloadAnimStartTime = 0.0f;
	m_flReloadLoopBottomCycle = 0.0f;
	m_flShotgunPumpStartCycle = 0.0f;
	m_flShotgunPumpEndCycle = 1.0f;
	m_iShotgunManualReloadSequence = -1;
	m_flShotgunManualReloadHoldCycle = 0.0f;
	m_flShotgunManualReloadCommitCycle = 1.0f;
	m_flPistolOneFrameCycle = 0.0f;
	m_flPistolMagFreeCycle = 0.0f;
	m_flPistolPauseCycle = 1.0f;
	m_flPistolInsertTargetCycle = 0.0f;
	m_flPistolFinishEndCycle = 1.0f;
	m_bPistolReloadBlendOut = false;
	m_flPistolReloadBlendOutStartTime = 0.0f;
	m_flPistolReloadAnimWeight = 1.0f;
	m_bShotgunManualReloadPoseActive = false;
	m_bShotgunManualReloadBlendOutActive = false;
	m_flShotgunManualReloadBlendOutStartTime = 0.0f;
	m_nShotgunManualReloadBlendOutBones = 0;
	m_bPlayingReloadAnim = false;
	m_iLeverReloadSequence = -1;
	m_flLeverReloadCycle = 0.0f;

	// Left hand wearables
	m_hLeftHandWatch = NULL;
	SetIdentityMatrix(m_matWatchOffset);
	m_bWatchOffsetValid = false;
	m_pWatchPanel = NULL;
	m_hLeftHandBall = NULL;
	m_iLastBallAmmo = -1;
	m_hLeftHandShield = NULL;
	m_bShieldOffsetValid = false;
	SetIdentityMatrix(m_matShieldOffset);
	m_hManualReloadRocket = NULL;
	m_bManualReloadRocketBoneInverseValid = false;
	SetIdentityMatrix(m_matManualReloadRocketBoneInverse);
	m_hPistolMagazine = NULL;
	m_bPistolMagBoneInverseValid = false;
	SetIdentityMatrix(m_matPistolMagBoneInverse);
	m_vecPistolMagLastWorldPos = vec3_origin;
	m_angPistolMagLastWorldAng = vec3_angle;
	m_vecPistolMagEjectVel = vec3_origin;
	m_bPistolMagLastWorldValid = false;
	m_bPistolMagFalling = false;
	m_flPistolMagFallStartTime = 0.0f;
	m_vecPistolMagFallStartPos = vec3_origin;
	m_vecPistolMagFallVel = vec3_origin;

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
	m_iLastEquippedWeaponID = -1;
	m_hRenderWeapon = NULL;
	m_bHandTrackingValid = false;
	m_bControllerTracked = false;
	m_iFireSequence = -1;
	m_iAltFireSequence = -1;
	m_iIdleSequence = -1;
	m_bAnimateIdle = false;
	m_bLoopIdleOnHand = false;
	m_bPlayingFireAnim = false;
	m_flFireAnimStartTime = 0.0f;
	m_iDrawSequence = -1;
	m_bPlayingDrawAnim = false;
	m_flDrawAnimStartTime = 0.0f;
	m_eDrawAnimScope = VR_DRAW_ANIM_NONE;
	m_iChargeSequence = -1;
	m_iChargeSequence2 = -1;
	m_iBowIdleSequence = -1;
	m_flBowIdleCycle = 0.0f;
	m_flBowFireEndCycle = 1.0f;
	m_flBowFireFirstFrameCycle = 0.0f;
	m_iBowDrawSequence = -1;
	m_flBowDrawNockCycle = 0.0f;
	m_iBowChargeIdleSequence = -1;
	m_iBowShakeOverlaySequence = -1;
	m_flBowShakeOverlayStartTime = 0.0f;
	m_bBowShakeOverlayActive = false;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;
	m_bPlayingChargeAnim = false;
	m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
	m_iFireOnSequence = -1;
	m_iFireOffSequence = -1;
	m_bMedigunWasHealing = false;
	m_bMedigunLeverActive = false;
	m_iMedigunLeverSeq = -1;
	m_flMedigunLeverCycle = 0.0f;
	m_bMedigunBodyPastHalf = false;
	m_bMedigunGripTargetValid = false;
	SetIdentityMatrix(m_matMedigunGripTarget);
	m_bFlamethrowerWasFiring = false;
	m_flFlamethrowerFireBlend = 0.0f;
	m_iOffHandBone = -1;
	m_iOffHandMiddleFingerBone = -1;
	m_flTwoHandBlend = 0.0f;
	m_bOffhandGripActive = false;
	m_bWasOffhandGripActive = false;
	m_flGripRotationBlend = 0.0f;
	m_bPomsonRightGripWasPressed = false;
	m_bPomsonSuppressPassiveGripPoint = false;
	m_bPomsonSuppressReloadGripPoint = false;
	SetIdentityMatrix(m_matPomsonRightLatchOffset);
	m_bPomsonRightLatchOffsetValid = false;
	SetIdentityMatrix(m_matPomsonRightGripLastWorld);
	m_bPomsonRightGripLastWorldValid = false;
	SetIdentityMatrix(m_matPomsonRightUnlatchStart);
	m_bPomsonRightUnlatchStartValid = false;
	m_bPomsonRightUnlatchUseReloadGrip = false;
	SetIdentityMatrix(m_matPomsonDetachLeftToWeaponBone);
	m_bPomsonDetachLeftToWeaponBoneValid = false;
	SetIdentityMatrix(m_matPomsonDetachLeftToLeftHandBone);
	m_bPomsonDetachLeftToLeftHandBoneValid = false;
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
	SetIdentityMatrix(m_matLiveWeaponBoneWorld);
	m_bLiveWeaponBoneWorldValid = false;
	SetIdentityMatrix(m_matLiveBowStringBoneWorld);
	m_bLiveBowStringBoneWorldValid = false;
	m_vecCachedMuzzlePos = vec3_origin;
	m_angCachedMuzzleAngles = vec3_angle;
	m_bCachedMuzzleValid = false;
	m_iMeleeSwingIndex = 0;
	m_szMeleeSwingBase[0] = '\0';
	m_iMeleeSwingCount = 0;
	m_bPrevVRSwingActive = false;
	m_iBreadBiteCritSeq = -1;
	m_iBreadBiteIdleSeqs[0] = m_iBreadBiteIdleSeqs[1] = m_iBreadBiteIdleSeqs[2] = -1;
	m_bIsBreadBite = false;
	m_flBreadBiteIdleStartTime = 0.0f;
	m_iBBCrossfadeFromSeq = -1;
	m_flBBCrossfadeFromCycle = 0.0f;
	m_flBBCrossfadeStart = 0.0f;
	m_iBBLastSampledSeq = -1;
	m_flBBLastSampledCycle = 0.0f;
	m_flBBLastCrossfadeCheck = 0.0f;

	// Reset left hand wearables
	m_hLeftHandWatch = NULL;
	SetIdentityMatrix(m_matWatchOffset);
	m_bWatchOffsetValid = false;
	m_pWatchPanel = NULL;
	m_hLeftHandBall = NULL;
	m_iLastBallAmmo = -1;
	m_hLeftHandShield = NULL;
	m_bShieldOffsetValid = false;
	SetIdentityMatrix(m_matShieldOffset);
	m_hManualReloadRocket = NULL;
	m_bManualReloadRocketBoneInverseValid = false;
	SetIdentityMatrix(m_matManualReloadRocketBoneInverse);

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

	// Check for Gunslinger (engineer robot arm)
	m_bHasGunslinger = IsPlayerUsingGunslinger(pOwner);

	// Left-handed mode: which authored hand side this entity poses as. In
	// right-handed mode this equals the physical side (no behavior change).
	m_bPoseAsLeftHand = ComputePoseAsLeftHand(pOwner->GetActiveTFWeapon());

	// Get class-specific hand model path (may return NULL to force fallback).
	// Use the authored pose side, not the physical side, so a left controller
	// acting as the weapon hand for a right-authored weapon loads the right model.
	const char *handModelPath = GetHandModelForClass(m_iLastPlayerClass, m_bPoseAsLeftHand);

	// Engineer with Gunslinger: use recompiled gunslinger model with blank body
	// bodygroup so only the robot arm renders. Same skeleton so finger tracking works.
	if (m_bHasGunslinger && IsRightHand())
	{
		handModelPath = "models/weapons/vr_models/vr_engineer_gunslinger.mdl";
		Msg("VR Hand (RIGHT): Engineer has Gunslinger - using VR gunslinger model\n");
	}

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

	// Gunslinger bodygroup setup: enable "rightarm" (robot arm mesh).
	// Gunslinger bodygroup setup: hide "body" (blank submodel), show "rightarm" (robot arm)
	if (m_bHasGunslinger && IsRightHand() && pStudioHdr)
	{
		int iBody = FindBodygroupByName("body");
		int iRightArm = FindBodygroupByName("rightarm");

		if (iBody >= 0)
			SetBodygroup(iBody, 1);     // blank submodel hides normal arms
		if (iRightArm >= 0)
			SetBodygroup(iRightArm, 0); // robot arm mesh

		Msg("VR Hand (RIGHT): Gunslinger bodygroups - body=%d->1(hidden), rightarm=%d->0(visible)\n", iBody, iRightArm);
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
// Purpose: Compute which authored hand side this entity should pose as for the
//          given weapon. The weapon hand poses as the weapon's authored hand;
//          the support hand poses as the opposite. With no weapon, default to
//          the physical side (no mirroring).
//-----------------------------------------------------------------------------
bool C_TFVRHand::ComputePoseAsLeftHand( C_TFWeaponBase *pWeapon ) const
{
	if ( !pWeapon )
		return IsLeftHand();

	const bool bDisplayOnLeft = TFVR_DisplayWeaponOnLeft( pWeapon );
	const bool bThisIsWeaponHand = ( bDisplayOnLeft == IsLeftHand() );
	const bool bAuthoredLeft = TFVR_WeaponAuthoredHandIsLeft( pWeapon );

	return bThisIsWeaponHand ? bAuthoredLeft : !bAuthoredLeft;
}

//-----------------------------------------------------------------------------
// Purpose: (Re)load the hand model for the current authored pose side, applying
//          the Gunslinger override and bodygroups. Resets bone mapping so the
//          finger/hand bone suffix is rebuilt for the new model.
//-----------------------------------------------------------------------------
void C_TFVRHand::LoadHandModelForPoseSide()
{
	const char *handModelPath = GetHandModelForClass( m_iLastPlayerClass, m_bPoseAsLeftHand );

	if ( m_bHasGunslinger && IsRightHand() )
		handModelPath = "models/weapons/vr_models/vr_engineer_gunslinger.mdl";

	bool bModelSet = false;
	if ( handModelPath != NULL )
	{
		Q_strncpy( m_szModelName, handModelPath, sizeof( m_szModelName ) );
		if ( modelinfo->GetModelIndex( handModelPath ) == -1 )
			CBaseEntity::PrecacheModel( handModelPath );
		bModelSet = SetModel( m_szModelName );
	}

	if ( !bModelSet )
	{
		const char *fallbackModel = GetFallbackModelForClass( m_iLastPlayerClass );
		Q_strncpy( m_szModelName, fallbackModel, sizeof( m_szModelName ) );
		SetModel( m_szModelName );
	}

	if ( m_bHasGunslinger && IsRightHand() && GetModelPtr() )
	{
		int iBody = FindBodygroupByName( "body" );
		int iRightArm = FindBodygroupByName( "rightarm" );
		if ( iBody >= 0 )
			SetBodygroup( iBody, 1 );
		if ( iRightArm >= 0 )
			SetBodygroup( iRightArm, 0 );
	}

	// Force the finger/hand bone suffix to be re-resolved for the new model.
	m_bBoneMappingSetup = false;
	m_bHandBoneOffsetValid = false;
	m_iHandBone = -1;
}

//-----------------------------------------------------------------------------
// Purpose: Recompute the authored pose side from the active weapon and reload
//          the model if it changed. Returns true if the pose side changed.
//-----------------------------------------------------------------------------
bool C_TFVRHand::RefreshPoseHandSide()
{
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if ( !pOwner )
		return false;

	const bool bDesired = ComputePoseAsLeftHand( pOwner->GetActiveTFWeapon() );
	if ( bDesired == m_bPoseAsLeftHand )
		return false;

	m_bPoseAsLeftHand = bDesired;
	LoadHandModelForPoseSide();
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
		RemoveLeftHandShield();
	}
	RemoveManualReloadRocketModel();
	RemovePistolMagazineModel();

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
			CTFWeaponBase *pActiveWeapon = pPlayer->GetActiveTFWeapon();
			if (pActiveWeapon)
			{
				C_TFVRHand *pWeaponHand = TFVR_GetWeaponHand(pActiveWeapon);
				if (pWeaponHand)
					pWeaponHand->EquipWeapon(pActiveWeapon);
			}
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

	if (g_pLocalPlayerLeftHand && g_pLocalPlayerRightHand)
	{
		CTFWeaponBase *pActiveWeapon = pPlayer->GetActiveTFWeapon();
		if (pActiveWeapon)
		{
			C_TFVRHand *pWeaponHand = TFVR_GetWeaponHand(pActiveWeapon);
			if (pWeaponHand)
				pWeaponHand->EquipWeapon(pActiveWeapon);
		}
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
	DoAnimationEvents(GetModelPtr());
	UpdateBowChargeAnimation();

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

		C_TFWeaponBase *pFireWeapon = GetHeldWeapon();
		const bool bBowFireReachedEndFrame = pFireWeapon
			&& pFireWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW
			&& m_iBowIdleSequence >= 0
			&& GetSequence() == m_iBowIdleSequence
			&& GetCycle() >= m_flBowFireEndCycle;

		// Check if animation cycle has completed (or timed out after 1 second)
		if (bBowFireReachedEndFrame || GetCycle() >= 1.0f || (gpGlobals->curtime - m_flFireAnimStartTime) > 1.0f)
		{
			// Bread Bite: pick a new random idle variant and reset timer
			if (m_bIsBreadBite)
			{
				int idx = RandomInt(0, 2);
				if (m_iBreadBiteIdleSeqs[idx] >= 0)
					m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
				m_flBreadBiteIdleStartTime = gpGlobals->curtime;
			}

			// Return to idle animation
			bool bLiveIdleFire = m_bIsBreadBite || (m_bAnimateIdle && m_bLoopIdleOnHand);
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(bLiveIdleFire ? 1.0f : 0.0f);
			}
			m_bPlayingFireAnim = false;
			m_bBowFireStartPoseValid = false;
			m_iBowFireStartPoseBoneCount = 0;

			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Fire animation completed, returning to idle (seq %d)\n", m_iIdleSequence);
			}
		}
	}

	// Check if draw animation has completed and return to idle
	if (m_bPlayingDrawAnim)
	{
		bool bDrawComplete = false;
		if (m_eDrawAnimScope == VR_DRAW_ANIM_WEAPON_BONE)
		{
			// WEAPON_BONE scope: check elapsed time against draw sequence duration.
			// Bread creatures also use this path — they play the draw sequence on
			// the entity (for vm_weapon bones) but completion uses elapsed time.
			CStudioHdr *pHdr = GetModelPtr();
			float flDuration = pHdr ? SequenceDuration(pHdr, m_iDrawSequence) : 0.0f;
			bDrawComplete = (gpGlobals->curtime - m_flDrawAnimStartTime) >= flDuration
				|| (gpGlobals->curtime - m_flDrawAnimStartTime) > 2.0f;
		}
		else
		{
			bDrawComplete = GetCycle() >= 1.0f
				|| (gpGlobals->curtime - m_flDrawAnimStartTime) > 2.0f;
		}

		if (bDrawComplete)
		{
			if (m_bIsBreadBite)
			{
				int idx = RandomInt(0, 2);
				if (m_iBreadBiteIdleSeqs[idx] >= 0)
					m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
				m_flBreadBiteIdleStartTime = gpGlobals->curtime;
			}

			bool bLiveIdle = m_bIsBreadBite || (m_bAnimateIdle && m_bLoopIdleOnHand);
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(bLiveIdle ? 1.0f : 0.0f);
			}
			m_bPlayingDrawAnim = false;
			InvalidateBoneCache();

			extern ConVar tfvr_weapon_draw_anim_debug;
			if (tfvr_weapon_draw_anim_debug.GetBool())
			{
				DevMsg("VR: Draw animation completed, returning to idle (seq %d)\n", m_iIdleSequence);
			}
		}
	}

	// Bread Bite: cycle through idle animations (A/B/C) while weapon is held.
	// The entity's live sequence/cycle drives the pose (bUseCurrentAnim),
	// so we keep playback rate at 1.0 and switch sequences when one expires.
	if (m_bIsBreadBite && !m_bPlayingFireAnim && !m_bPlayingDrawAnim && !m_bPlayingChargeAnim)
	{
		if (GetPlaybackRate() < 0.01f)
			SetPlaybackRate(1.0f);

		CStudioHdr *pHdr = GetModelPtr();
		float duration = (pHdr && m_iIdleSequence >= 0) ? SequenceDuration(pHdr, m_iIdleSequence) : 1.0f;
		float elapsed = gpGlobals->curtime - m_flBreadBiteIdleStartTime;

		if (elapsed >= duration && duration > 0.0f)
		{
			int idx = RandomInt(0, 2);
			if (m_iBreadBiteIdleSeqs[idx] >= 0)
				m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
			SetSequence(m_iIdleSequence);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
			m_flBreadBiteIdleStartTime = gpGlobals->curtime;
			InvalidateBoneCache();
		}
	}
	// Bread creatures (not Bread Bite): keep idle looping at rate 1.0.
	// Sync the render weapon's cycle to the hand every frame so weapon-only
	// bones (sapper mechanism etc.) stay in lockstep. The render weapon is
	// at rate 0 — it never self-advances.
	if (m_bAnimateIdle && !m_bIsBreadBite
		&& !m_bPlayingFireAnim && !m_bPlayingDrawAnim && !m_bPlayingChargeAnim)
	{
		if (GetPlaybackRate() < 0.01f)
			SetPlaybackRate(1.0f);

		if (m_iIdleSequence >= 0 && GetCycle() >= 0.999f)
		{
			SetSequence(m_iIdleSequence);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
		}

		C_VRRenderWeapon *pRW = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
		if (pRW)
		{
			pRW->SetCycle(GetCycle());
		}
	}

	// VR physical melee swing: detect rising edge on m_bVRSwingActive
	// to trigger the hand's fire animation (drives weapon via bone merge)
	if (!IsLeftHand() && m_bIsBreadBite)
	{
		C_TFWeaponBase *pWeapon = GetHeldWeapon();
		if (pWeapon)
		{
			int wtype = pWeapon->GetTFWpnData().m_iWeaponType;
			if (wtype == TF_WPN_TYPE_MELEE || wtype == TF_WPN_TYPE_MELEE_ALLCLASS)
			{
				CTFWeaponBaseMelee *pMelee = static_cast<CTFWeaponBaseMelee *>(pWeapon);
				bool bSwingNow = pMelee->IsVRSwingActive();
				if (bSwingNow && !m_bPrevVRSwingActive)
				{
					PlayWeaponFireAnimation();
				}
				m_bPrevVRSwingActive = bSwingNow;
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
	DoAnimationEvents(GetModelPtr());
	UpdateBowChargeAnimation();

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

		C_TFWeaponBase *pFireWeapon = GetHeldWeapon();
		const bool bBowFireReachedEndFrame = pFireWeapon
			&& pFireWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW
			&& m_iBowIdleSequence >= 0
			&& GetSequence() == m_iBowIdleSequence
			&& GetCycle() >= m_flBowFireEndCycle;

		// Check if animation cycle has completed (or timed out after 1 second)
		if (bBowFireReachedEndFrame || GetCycle() >= 1.0f || (gpGlobals->curtime - m_flFireAnimStartTime) > 1.0f)
		{
			// Bread Bite: pick a new random idle variant and reset timer
			if (m_bIsBreadBite)
			{
				int idx = RandomInt(0, 2);
				if (m_iBreadBiteIdleSeqs[idx] >= 0)
					m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
				m_flBreadBiteIdleStartTime = gpGlobals->curtime;
			}

			// Return to idle animation
			bool bLiveIdleFire2 = m_bIsBreadBite || (m_bAnimateIdle && m_bLoopIdleOnHand);
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(bLiveIdleFire2 ? 1.0f : 0.0f);
			}
			m_bPlayingFireAnim = false;
			m_bBowFireStartPoseValid = false;
			m_iBowFireStartPoseBoneCount = 0;

			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Fire animation completed, returning to idle (seq %d)\n", m_iIdleSequence);
			}
		}
	}

	// Check if draw animation has completed and return to idle
	if (m_bPlayingDrawAnim)
	{
		bool bDrawComplete = false;
		if (m_eDrawAnimScope == VR_DRAW_ANIM_WEAPON_BONE)
		{
			CStudioHdr *pHdr = GetModelPtr();
			float flDuration = pHdr ? SequenceDuration(pHdr, m_iDrawSequence) : 0.0f;
			bDrawComplete = (gpGlobals->curtime - m_flDrawAnimStartTime) >= flDuration
				|| (gpGlobals->curtime - m_flDrawAnimStartTime) > 2.0f;
		}
		else
		{
			bDrawComplete = GetCycle() >= 1.0f
				|| (gpGlobals->curtime - m_flDrawAnimStartTime) > 2.0f;
		}

		if (bDrawComplete)
		{
			if (m_bIsBreadBite)
			{
				int idx = RandomInt(0, 2);
				if (m_iBreadBiteIdleSeqs[idx] >= 0)
					m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
				m_flBreadBiteIdleStartTime = gpGlobals->curtime;
			}

			bool bLiveIdle2 = m_bIsBreadBite || (m_bAnimateIdle && m_bLoopIdleOnHand);
			if (m_iIdleSequence >= 0)
			{
				SetSequence(m_iIdleSequence);
				SetCycle(0.0f);
				SetPlaybackRate(bLiveIdle2 ? 1.0f : 0.0f);
			}
			m_bPlayingDrawAnim = false;
			InvalidateBoneCache();

			extern ConVar tfvr_weapon_draw_anim_debug;
			if (tfvr_weapon_draw_anim_debug.GetBool())
			{
				DevMsg("VR: Draw animation completed, returning to idle (seq %d)\n", m_iIdleSequence);
			}
		}
	}

	// Bread creatures (not Bread Bite): keep idle looping at rate 1.0.
	// Sync render weapon cycle to hand each frame.
	if (m_bAnimateIdle && !m_bIsBreadBite
		&& !m_bPlayingFireAnim && !m_bPlayingDrawAnim && !m_bPlayingChargeAnim)
	{
		if (GetPlaybackRate() < 0.01f)
			SetPlaybackRate(1.0f);

		if (m_iIdleSequence >= 0 && GetCycle() >= 0.999f)
		{
			SetSequence(m_iIdleSequence);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
		}

		C_VRRenderWeapon *pRW = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
		if (pRW)
		{
			pRW->SetCycle(GetCycle());
		}
	}

	// Bread Bite: keep idle animations cycling smoothly.
	// Sync the idle VARIANT to the viewmodel (for audio alignment) but
	// let StudioFrameAdvance drive the cycle to avoid per-frame jitter.
	if (m_bIsBreadBite && !m_bPlayingFireAnim && !m_bPlayingDrawAnim && !m_bPlayingChargeAnim)
	{
		if (GetPlaybackRate() < 0.01f)
			SetPlaybackRate(1.0f);

		// Check if viewmodel changed to a different bread bite idle
		C_TFPlayer *pSyncOwner = m_hOwnerPlayer.Get();
		C_BaseViewModel *pVM = pSyncOwner ? pSyncOwner->GetViewModel(0) : NULL;
		if (pVM)
		{
			int vmSeq = pVM->GetSequence();
			for (int i = 0; i < 3; i++)
			{
				if (vmSeq == m_iBreadBiteIdleSeqs[i] && vmSeq != GetSequence())
				{
					m_iIdleSequence = vmSeq;
					SetSequence(vmSeq);
					SetCycle(pVM->GetCycle());
					SetPlaybackRate(1.0f);
					break;
				}
			}
		}

		// When the current idle finishes (cycle hit 1.0), immediately
		// start the next variant so there is no visible pause.
		if (GetCycle() >= 0.999f)
		{
			int idx = RandomInt(0, 2);
			if (m_iBreadBiteIdleSeqs[idx] >= 0)
				m_iIdleSequence = m_iBreadBiteIdleSeqs[idx];
			SetSequence(m_iIdleSequence);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
			InvalidateBoneCache();
		}
	}

	// VR physical melee swing: detect rising edge on m_bVRSwingActive
	if (!IsLeftHand() && m_bIsBreadBite)
	{
		C_TFWeaponBase *pWeapon = GetHeldWeapon();
		if (pWeapon)
		{
			int wtype = pWeapon->GetTFWpnData().m_iWeaponType;
			if (wtype == TF_WPN_TYPE_MELEE || wtype == TF_WPN_TYPE_MELEE_ALLCLASS)
			{
				CTFWeaponBaseMelee *pMelee = static_cast<CTFWeaponBaseMelee *>(pWeapon);
				bool bSwingNow = pMelee->IsVRSwingActive();
				if (bSwingNow && !m_bPrevVRSwingActive)
				{
					PlayWeaponFireAnimation();
				}
				m_bPrevVRSwingActive = bSwingNow;
			}
		}
	}

	// Update medigun fire animation state machine
	UpdateMedigunFireAnimation();

	// Update flamethrower fire animation (driven by weapon fire state)
	UpdateFlamethrowerFireAnimation();

	// Update pump/lever reload animation (pump-driven cycle)
	UpdateScattergunReloadAnimation();
	UpdateStickyPumpReloadAnimation();
	UpdateBisonPumpReloadAnimation();
	UpdateManglerPumpReloadAnimation();
	UpdatePomsonPumpReloadAnimation();
	UpdateShotgunPumpActionAnimation();
	// Must run after the functions above: they clear the shared reload anim
	// state for weapons they don't recognize (including the pistol).
	UpdatePistolReloadAnimation();

	// Forward pump reload state to render weapon so weapon_bone_1 animates
	{
		C_VRRenderWeapon *pRW = dynamic_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
		if (pRW)
		{
			C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
			bool bPomsonHeldSlide = pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
				&& m_eReloadAnimState != VR_RELOAD_ANIM_NONE
				&& m_iLeverReloadSequence >= 0;
			bool bPumpShotgun = pWeapon && IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID());
			bool bShotgunManualShell = false;
			if (bPumpShotgun)
			{
				CTFShotgun *pShotgun = static_cast<CTFShotgun *>(pWeapon);
				bShotgunManualShell = pShotgun->IsVRShotgunManualReloadActive();
			}
			if ((m_bPlayingReloadAnim || bPomsonHeldSlide) && m_iLeverReloadSequence >= 0
				&& (!bPumpShotgun || bShotgunManualShell))
			{
				Vector vmWeaponBoneDelta;
				Vector *pVmWeaponBoneDelta = NULL;
				if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
					&& GetSampledBoneModelSpaceDelta("vm_weapon_bone", m_iLeverReloadSequence,
						0.0f, m_flLeverReloadCycle, vmWeaponBoneDelta))
				{
					// Keep the authored slide on one local axis. The Pomson
					// model's vm_weapon_bone axes do not match weapon forward.
					float absX = fabsf(vmWeaponBoneDelta.x);
					float absY = fabsf(vmWeaponBoneDelta.y);
					float absZ = fabsf(vmWeaponBoneDelta.z);
					if (absX >= absY && absX >= absZ)
					{
						vmWeaponBoneDelta.y = 0.0f;
						vmWeaponBoneDelta.z = 0.0f;
					}
					else if (absY >= absZ)
					{
						vmWeaponBoneDelta.x = 0.0f;
						vmWeaponBoneDelta.z = 0.0f;
					}
					else
					{
						vmWeaponBoneDelta.x = 0.0f;
						vmWeaponBoneDelta.y = 0.0f;
					}
					pVmWeaponBoneDelta = &vmWeaponBoneDelta;
				}
				pRW->SetPumpReloadState(true, m_flLeverReloadCycle, pVmWeaponBoneDelta);
			}
			else
				pRW->SetPumpReloadState(false, 0.0f);
		}
	}

	// Update backstab ready state (spy knife only).
	// SetupBones uses this to drive the up/down/idle transition animations.
	if (m_iBackstabUpSequence >= 0 && IsRightHand())
	{
		C_TFKnife *pKnife = dynamic_cast<C_TFKnife*>(m_hHeldWeapon.Get());
		m_bBackstabReady = pKnife && pKnife->IsReadyToBackstab();

		bool bInCooldown = pKnife && pKnife->IsInBackstabCooldown();
		if (bInCooldown && !m_bBackstabAttacking)
		{
			m_bBackstabAttacking = true;
			m_bBackstabRaising = false;
			m_bBackstabLowering = false;
			m_bBackstabReady = false;
			m_flBackstabCycle = 0.0f;
		}
		else if (!bInCooldown && m_bBackstabAttacking)
		{
			m_bBackstabAttacking = false;
			m_flBackstabCycle = 0.0f;
		}

		extern ConVar tfvr_backstab_debug;
		if (tfvr_backstab_debug.GetBool())
			m_bBackstabReady = true;
	}

	// Rebuild both hands together if the local player class changed but the
	// player-level class-change hook has not rebuilt us yet. Direct Scout <->
	// Engineer switches are especially sensitive because the pistol reload
	// sequence name is class-specific (p_reload vs pstl_reload).
	int currentClass = pOwner->GetPlayerClass()->GetClassIndex();
	if (m_iLastPlayerClass != TF_CLASS_UNDEFINED && currentClass != m_iLastPlayerClass)
	{
		Msg("VR Update: %s hand detected class change %d -> %d, rebuilding hands\n",
			IsLeftHand() ? "LEFT" : "RIGHT", m_iLastPlayerClass, currentClass);
		if (IsRightHand())
			C_TFVRHand::SpawnVRHands(pOwner);
		else
			m_iLastPlayerClass = currentClass;
		return;
	}
	m_iLastPlayerClass = currentClass;

	// Check if gunslinger state changed (e.g. resupply locker loadout swap)
	if (currentClass == TF_CLASS_ENGINEER)
	{
		bool bNowGunslinger = IsPlayerUsingGunslinger(pOwner);
		if (bNowGunslinger != m_bHasGunslinger)
		{
			Msg("VR Update: %s hand detected gunslinger change (%d -> %d), reinitializing\n",
				IsLeftHand() ? "LEFT" : "RIGHT", m_bHasGunslinger, bNowGunslinger);
			Shutdown();
			if (Initialize(pOwner, m_handSide))
			{
				Spawn();
			}
			return;
		}
	}

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

	bool bHandledBowReloadGrip = false;
	if (tfvr_twohand_enabled.GetBool())
	{
		C_TFWeaponBase *pActiveWeapon = pOwner ? pOwner->GetActiveTFWeapon() : NULL;
		CTFCompoundBow *pBow = (pActiveWeapon && pActiveWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
			? static_cast<CTFCompoundBow *>(pActiveWeapon) : NULL;
		if (pBow && pBow->IsVRBowArrowNocked())
		{
			C_TFVRHand *pWeaponHand = TFVR_GetWeaponHand(pBow);
			C_TFVRHand *pSupportHand = TFVR_GetSupportHand(pBow);
			if (pSupportHand == this && pWeaponHand && pWeaponHand != this)
			{
				bHandledBowReloadGrip = true;

				// Use the support controller/grip point as the pull anchor. The
				// visible hand pose anchors its arrow grip bone to this same point.
				Vector supportGripPos = m_vecLastValidPosition;

				Vector weaponWristPos = pWeaponHand->GetAbsOrigin();
				COpenXRHandTracker *pWeaponHandTracker = pWeaponHand->GetHandTracker();
				if (pWeaponHandTracker)
				{
					Vector wristPos;
					QAngle wristAngles;
					if (pWeaponHandTracker->GetHandJoint(pWeaponHand->IsLeftHand(), XR_HAND_JOINT_WRIST_EXT,
						wristPos, wristAngles))
					{
						weaponWristPos = wristPos;
					}
				}

				// CRITICAL: the aim/rotation solve must depend ONLY on physical
				// controller positions. Any weapon-derived point (e.g. the nock
				// target, which rides on the weapon via GetLiveWeaponBoneTransform)
				// creates a feedback loop: the target moves with the weapon, the
				// weapon rotates toward the target, the target moves again -> the
				// pose diverges and "freaks out". The bow's aim axis is the draw
				// line from the draw (support) hand toward the bow (weapon) hand.
				Vector solveAnchorPos = weaponWristPos;
				QAngle solveAnchorAngles;
				const bool bGotNockTarget = tfvr_huntsman_debug.GetBool()
					&& pWeaponHand->GetBowManualReloadTarget(solveAnchorPos, solveAnchorAngles);

				// Only update the aim direction once the draw hand is pulled far
				// enough from the bow that the line between them is stable. Right
				// after nocking the hands are close together, so a near-zero draw
				// vector is dominated by tracking noise and makes the bow spin
				// erratically. Below the threshold we hold the last good direction
				// and disengage rotation (see rotTarget below).
				Vector solveDirection = weaponWristPos - supportGripPos;
				const float flDrawSeparation = solveDirection.Length();
				const bool bAimDirValid = flDrawSeparation > tfvr_huntsman_aim_min_pull.GetFloat();
				if (bAimDirValid)
				{
					solveDirection /= flDrawSeparation;
					if (TFVR_ShouldMirrorWeaponHand(pBow))
					{
						matrix3x4_t reflectFrame;
						AngleMatrix(pWeaponHand->m_angLastValidAngles, vec3_origin, reflectFrame);
						TFVR_ReflectVectorInControllerFrame(solveDirection, reflectFrame);
						solveDirection.NormalizeInPlace();
					}
					m_vecOffhandGripForward = solveDirection;
				}

				if (tfvr_huntsman_debug.GetBool())
				{
					debugoverlay->AddBoxOverlay(weaponWristPos,
						Vector(-1.5f, -1.5f, -1.5f), Vector(1.5f, 1.5f, 1.5f),
						vec3_angle, 255, 255, 255, 220, 0.1f);
					debugoverlay->AddTextOverlay(weaponWristPos, 0.1f, "bow weapon wrist");

					debugoverlay->AddBoxOverlay(supportGripPos,
						Vector(-1.5f, -1.5f, -1.5f), Vector(1.5f, 1.5f, 1.5f),
						vec3_angle, 255, 255, 0, 220, 0.1f);
					debugoverlay->AddTextOverlay(supportGripPos, 0.1f, "support wrist");

					if (bGotNockTarget)
					{
						debugoverlay->AddBoxOverlay(solveAnchorPos,
							Vector(-2.0f, -2.0f, -2.0f), Vector(2.0f, 2.0f, 2.0f),
							solveAnchorAngles, 0, 255, 0, 220, 0.1f);
						debugoverlay->AddTextOverlay(solveAnchorPos, 0.1f, "bow nock target");
						debugoverlay->AddLineOverlay(supportGripPos, solveAnchorPos, 255, 0, 0, true, 0.1f);
						debugoverlay->AddLineOverlay(weaponWristPos, solveAnchorPos, 0, 255, 255, true, 0.1f);
					}

					// MAGENTA = the true physical draw line (support -> weapon).
					// This is the real aim axis and should lie along the visible
					// string. The pre-reflection grip-forward vector is NOT drawn
					// because in mirrored mode it points in canonical (reflected)
					// space and is misleading.
					debugoverlay->AddLineOverlay(supportGripPos, weaponWristPos,
						255, 0, 255, true, 0.1f);

					static float s_flLastBowSolveDebug = 0.0f;
					if (gpGlobals->curtime - s_flLastBowSolveDebug > 0.25f)
					{
						DevMsg("HuntsmanVR Solve: weaponHand=%s supportHand=%s mirrored=%d nocked=%d target=%d support=(%.1f %.1f %.1f) weapon=(%.1f %.1f %.1f)",
							pWeaponHand->IsLeftHand() ? "L" : "R", IsLeftHand() ? "L" : "R",
							TFVR_ShouldMirrorWeaponHand(pBow) ? 1 : 0,
							pBow->IsVRBowArrowNocked() ? 1 : 0, bGotNockTarget ? 1 : 0,
							supportGripPos.x, supportGripPos.y, supportGripPos.z,
							weaponWristPos.x, weaponWristPos.y, weaponWristPos.z);
						if (bGotNockTarget)
						{
							DevMsg(" nock=(%.1f %.1f %.1f) wrist_to_nock=%.2f support_to_nock=%.2f\n",
								solveAnchorPos.x, solveAnchorPos.y, solveAnchorPos.z,
								(solveAnchorPos - weaponWristPos).Length(),
								(solveAnchorPos - supportGripPos).Length());
						}
						else
						{
							DevMsg("\n");
						}
						s_flLastBowSolveDebug = gpGlobals->curtime;
					}
				}

				m_bOffhandGripActive = true;
				m_bWasOffhandGripActive = true;

				float easePower = tfvr_offhand_grip_ease_power.GetFloat();
				float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
				float rotBlendSpeed = tfvr_offhand_grip_rotation_blend_speed.GetFloat();
				// Position attach always eases in. Weapon ROTATION only engages once
				// the draw line is long enough to be stable, so the bow eases into
				// aim as the player pulls back instead of spinning at the nock.
				m_flTwoHandBlend = EasedApproach(1.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
				const float flRotTarget = bAimDirValid ? 1.0f : 0.0f;
				m_flGripRotationBlend = EasedApproach(flRotTarget, m_flGripRotationBlend, rotBlendSpeed, gpGlobals->frametime, easePower);

				if (tfvr_twohand_debug.GetBool())
				{
					debugoverlay->AddLineOverlay(weaponWristPos, supportGripPos, 128, 0, 255, true, 0.1f);
					debugoverlay->AddLineOverlay(weaponWristPos, weaponWristPos + m_vecOffhandGripForward * 45.0f,
						255, 0, 255, true, 0.1f);
				}
			}
		}
	}

	if (!bHandledBowReloadGrip && m_bWasOffhandGripActive)
	{
		C_TFWeaponBase *pActiveWeapon = pOwner ? pOwner->GetActiveTFWeapon() : NULL;
		if (pActiveWeapon && pActiveWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW
			&& TFVR_GetSupportHand(pActiveWeapon) == this)
		{
			float easePower = tfvr_offhand_grip_ease_power.GetFloat();
			float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
			float rotBlendSpeed = tfvr_offhand_grip_rotation_blend_speed.GetFloat();
			m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
			m_flGripRotationBlend = EasedApproach(0.0f, m_flGripRotationBlend, rotBlendSpeed, gpGlobals->frametime, easePower);
			m_bOffhandGripActive = false;
			if (m_flTwoHandBlend < 0.001f && m_flGripRotationBlend < 0.001f)
				m_bWasOffhandGripActive = false;
		}
	}

	// Two-handed weapon support - only for left hand
	if (!bHandledBowReloadGrip && IsLeftHand() && tfvr_twohand_enabled.GetBool())
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
				// Gunslinger - robot arm IS the hand, no two-handing
				else if (V_stristr(pWeapon->GetClassname(), "robot_arm"))
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
				// Sapper/builder - one-handed placement tool
				else if (iWeaponID == TF_WEAPON_BUILDER)
				{
					bSkipTwoHand = true;
				}
				// Pistols - passive grip only (no active grip button required, no rotation control)
				else if (iWeaponID == TF_WEAPON_PISTOL ||
				         iWeaponID == TF_WEAPON_PISTOL_SCOUT ||
				         iWeaponID == TF_WEAPON_HANDGUN_SCOUT_SECONDARY)  // Pretty Boy's, Winger
				{
					bPassiveGripOnly = true;

					// No passive grip while a manual mag reload is in
					// progress (or a spare mag is in this hand): the off
					// hand must stay free to fetch and insert the mag.
					CTFPistol *pManualPistol = TFVR_GetManualReloadPistol(pWeapon);
					if (pManualPistol && (pManualPistol->IsVRPistolManualReloadBusy()
						|| pManualPistol->IsVRMagOut()
						|| pManualPistol->HasVRMagazineInHand()))
					{
						bPassiveGripOnly = false;
						bSkipTwoHand = true;
					}
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
			// Stickybomb launcher: instantly detach the off-hand while
			// charging or firing so there is no single-frame pop.
			bool bStickyBusy = false;
			C_TFWeaponBase *pRightWpn = pRightHand->GetHeldWeapon();
			if (pRightWpn && pRightWpn->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
			{
				CTFPipebombLauncher *pSB = static_cast<CTFPipebombLauncher *>(pRightWpn);
				bStickyBusy = (pSB->GetChargeBeginTime() > 0 || gpGlobals->curtime < pSB->m_flNextPrimaryAttack);
			}

			if (bStickyBusy)
			{
				m_bOffhandGripActive = false;
				m_bWasOffhandGripActive = false;
				m_flTwoHandBlend = 0.0f;
				m_flGripRotationBlend = 0.0f;
			}

			// Bison: detach off-hand from reload grip while firing
			bool bBisonBusy = false;
			if (pRightWpn && pRightWpn->GetWeaponID() == TF_WEAPON_RAYGUN
				&& pRightHand->IsBisonOnReloadGrip()
				&& gpGlobals->curtime < pRightWpn->m_flNextPrimaryAttack)
			{
				bBisonBusy = true;
				m_bOffhandGripActive = false;
				m_bWasOffhandGripActive = false;
				m_flTwoHandBlend = 0.0f;
				m_flGripRotationBlend = 0.0f;
			}

			// Mangler: detach off-hand from reload grip while firing/charging.
			// Include the fire animation duration so the hand stays detached
			// until the visual recoil is fully done, preventing a brief
			// interpolation to the wrong grip target.
			bool bManglerBusy = false;
			if (pRightWpn && pRightWpn->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
				&& pRightHand->IsManglerOnReloadGrip())
			{
				CTFParticleCannon *pMangler = static_cast<CTFParticleCannon *>(pRightWpn);
				if (gpGlobals->curtime < pRightWpn->m_flNextPrimaryAttack
					|| pMangler->GetChargeBeginTime() > 0
					|| pRightHand->m_bPlayingFireAnim)
				{
					bManglerBusy = true;
					m_bOffhandGripActive = false;
					m_bWasOffhandGripActive = false;
					m_flTwoHandBlend = 0.0f;
					m_flGripRotationBlend = 0.0f;
				}
			}

			// Manual shotgun shell reload owns the offhand pose.  Do not let the
			// normal active foregrip solver steal the hand while a shell is held
			// or being inserted.
			bool bShotgunManualReloadBusy = false;
			if (pRightWpn && IsPumpActionShotgunWeaponID(pRightWpn->GetWeaponID()))
			{
				CTFShotgun *pShotgun = static_cast<CTFShotgun *>(pRightWpn);
				bShotgunManualReloadBusy = pShotgun->IsVRShotgunManualReloadActive();
				if (bShotgunManualReloadBusy)
				{
					m_bOffhandGripActive = false;
					m_bWasOffhandGripActive = false;
					m_flTwoHandBlend = 0.0f;
					m_flGripRotationBlend = 0.0f;
				}
			}

			// Bison dual grip: pick the closer of idle foregrip vs reload
			// pump handle each frame (only when not actively pumping).
			bool bBisonOnIdleGrip = false;
			if (!bBisonBusy
				&& pRightWpn && pRightWpn->GetWeaponID() == TF_WEAPON_RAYGUN
				&& !pRightHand->m_bPlayingReloadAnim
				&& pRightHand->m_iReloadLoopSequence >= 0)
			{
				Vector leftHandPos = m_vecLastValidPosition;
				if (m_pHandTracker)
				{
					Vector fp; QAngle fa;
					if (m_pHandTracker->GetHandJoint(true, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fp, fa))
						leftHandPos = fp;
				}

				Vector posA, posB;
				QAngle angA, angB;

				bool bOldFlag = pRightHand->m_bBisonUseReloadGrip;

				pRightHand->m_bBisonUseReloadGrip = false;
				bool bGotIdle = pRightHand->GetOffHandGripTarget(posA, angA);

				pRightHand->m_bBisonUseReloadGrip = true;
				bool bGotReload = pRightHand->GetOffHandGripTarget(posB, angB);

				if (bGotIdle && bGotReload)
				{
					float distIdle = (leftHandPos - posA).LengthSqr();
					float distReload = (leftHandPos - posB).LengthSqr();
					pRightHand->m_bBisonUseReloadGrip = (distReload < distIdle);
				}
				else if (bGotReload)
				{
					pRightHand->m_bBisonUseReloadGrip = true;
				}
				else
				{
					pRightHand->m_bBisonUseReloadGrip = bOldFlag;
				}

				// Smooth interpolation when switching between grip points:
				// briefly drop the blend so the hand transitions smoothly
				// instead of snapping.
				if (bOldFlag != pRightHand->m_bBisonUseReloadGrip)
				{
					m_flTwoHandBlend = MAX( m_flTwoHandBlend * 0.3f, 0.0f );
				}

				bBisonOnIdleGrip = !pRightHand->m_bBisonUseReloadGrip;
			}

			// Mangler dual grip: same closest-wins logic as Bison
			bool bManglerOnIdleGrip = false;
			if (!bManglerBusy
				&& pRightWpn && pRightWpn->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
				&& !pRightHand->m_bPlayingReloadAnim
				&& pRightHand->m_iReloadLoopSequence >= 0)
			{
				Vector leftHandPos = m_vecLastValidPosition;
				if (m_pHandTracker)
				{
					Vector fp; QAngle fa;
					if (m_pHandTracker->GetHandJoint(true, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fp, fa))
						leftHandPos = fp;
				}

				Vector posA, posB;
				QAngle angA, angB;

				bool bOldFlag = pRightHand->m_bManglerUseReloadGrip;

				pRightHand->m_bManglerUseReloadGrip = false;
				bool bGotIdle = pRightHand->GetOffHandGripTarget(posA, angA);

				pRightHand->m_bManglerUseReloadGrip = true;
				bool bGotReload = pRightHand->GetOffHandGripTarget(posB, angB);

				if (bGotIdle && bGotReload)
				{
					float distIdle = (leftHandPos - posA).LengthSqr();
					float distReload = (leftHandPos - posB).LengthSqr();
					pRightHand->m_bManglerUseReloadGrip = (distReload < distIdle);
				}
				else if (bGotReload)
				{
					pRightHand->m_bManglerUseReloadGrip = true;
				}
				else
				{
					pRightHand->m_bManglerUseReloadGrip = bOldFlag;
				}

				if (bOldFlag != pRightHand->m_bManglerUseReloadGrip)
				{
					m_flTwoHandBlend = MAX( m_flTwoHandBlend * 0.3f, 0.0f );
				}

				bManglerOnIdleGrip = !pRightHand->m_bManglerUseReloadGrip;
			}

			// Pomson: right-hand detach and dual grip
			bool bPomsonBusy = false;
			bool bPomsonSnapTwoHandOnReattach = false;
			if (pRightWpn && pRightWpn->GetWeaponID() == TF_WEAPON_DRG_POMSON)
			{
				// Detach when firing
				if (pRightHand->m_bRightHandDetached
					&& pRightHand->IsPomsonOnReloadGrip()
					&& gpGlobals->curtime < pRightWpn->m_flNextPrimaryAttack)
				{
					bPomsonBusy = true;
					pRightHand->m_bRightHandDetached = false;
					pRightHand->m_bPomsonUseReloadGrip = false;
					pRightHand->m_bPomsonRightGripLatched = false;
					pRightHand->m_bPomsonRightLatchOffsetValid = false;
					pRightHand->m_bPomsonRightGripLastWorldValid = false;
					pRightHand->m_bPomsonRightUnlatchStartValid = false;
					pRightHand->m_bPomsonRightGripWasPressed = false;
					pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
					pRightHand->m_bPomsonSuppressReloadGripPoint = false;
					pRightHand->m_bPomsonDetachLeftToWeaponBoneValid = false;
					pRightHand->m_bPomsonDetachLeftToLeftHandBoneValid = false;
				}

				// Right-hand detach detection: only when left hand has active grip
				if (!bPomsonBusy && m_bOffhandGripActive)
				{
					float rightGrip = 0.0f;
					if (g_pOpenXRManager)
						rightGrip = g_pOpenXRManager->GetAnalogValue("right_grip");
					extern ConVar tfvr_pomson_pump_weapon_grip_threshold;
					float detachThreshold = tfvr_pomson_pump_weapon_grip_threshold.GetFloat();

					if (pRightHand->m_bRightHandDetached)
					{
						// Already detached: right grip now grabs Pomson idle/reload
						// points.  The idle grip path below can reattach into the
						// normal two-hand solver when the player squeezes it.
					}
					else
					{
						// Not detached: detach if right grip is released
						if (rightGrip < detachThreshold * 0.7f)
						{
							pRightHand->CapturePomsonDetachLeftToWeaponBone();
							pRightHand->m_bRightHandDetached = true;
							pRightHand->m_bPomsonRightGripLatched = false;
							pRightHand->m_bPomsonRightLatchOffsetValid = false;
							pRightHand->m_bPomsonRightGripWasPressed = false;
							pRightHand->m_bPomsonSuppressPassiveGripPoint = true;
							pRightHand->m_bPomsonSuppressReloadGripPoint = false;
							pRightHand->m_flTwoHandBlend = 1.0f;
						}
					}
				}
				else if (!bPomsonBusy && !m_bOffhandGripActive)
				{
					// Left hand not gripping - re-attach right hand
					if (pRightHand->m_bRightHandDetached)
					{
						pRightHand->m_bRightHandDetached = false;
						pRightHand->m_bPomsonUseReloadGrip = false;
						pRightHand->m_bPomsonRightGripLatched = false;
						pRightHand->m_bPomsonRightLatchOffsetValid = false;
						pRightHand->m_bPomsonRightGripLastWorldValid = false;
						pRightHand->m_bPomsonRightUnlatchStartValid = false;
						pRightHand->m_bPomsonRightGripWasPressed = false;
						pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
						pRightHand->m_bPomsonSuppressReloadGripPoint = false;
						pRightHand->m_bPomsonDetachLeftToWeaponBoneValid = false;
						pRightHand->m_bPomsonDetachLeftToLeftHandBoneValid = false;
					}
				}

				// Pomson dual grip: active grip locks the chosen point, while
				// passive hover can migrate to whichever point is currently closer.
				if (pRightHand->m_bRightHandDetached
					&& pRightHand->m_iReloadLoopSequence >= 0)
				{
					Vector rightWristPos = pRightHand->m_vecLastValidPosition;
					Vector rightHandPos = rightWristPos;
					COpenXRHandTracker *pRightTracker = pRightHand->GetHandTracker();
					if (pRightTracker)
					{
						Vector fp; QAngle fa;
						if (pRightTracker->GetHandJoint(false, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fp, fa))
							rightHandPos = fp;
					}

					Vector posA, posB;
					QAngle angA, angB;
					bool bOldFlag = pRightHand->m_bPomsonUseReloadGrip;
					extern ConVar tfvr_pomson_pump_weapon_grip_threshold;
					float rightGrip = g_pOpenXRManager ? g_pOpenXRManager->GetAnalogValue("right_grip") : 0.0f;
					bool bGripPressed = rightGrip >= tfvr_pomson_pump_weapon_grip_threshold.GetFloat();

					pRightHand->m_bPomsonUseReloadGrip = false;
					bool bGotIdle = pRightHand->GetPomsonDetachedRightHandTarget(posA, angA);

					pRightHand->m_bPomsonUseReloadGrip = true;
					bool bUseCurrentPomsonReloadTarget = pRightHand->m_iLeverReloadSequence >= 0
						&& pRightHand->m_eReloadAnimState != VR_RELOAD_ANIM_NONE;
					bool bGotReload = pRightHand->GetPomsonDetachedRightHandTarget(posB, angB, bUseCurrentPomsonReloadTarget);
					float snapDist = tfvr_twohand_snap_distance.GetFloat();
					float gripRange = tfvr_offhand_grip_range.GetFloat() * 0.393701f;
					float distIdle = bGotIdle
						? (rightHandPos - posA).Length()
						: 1.0e30f;
					float distReload = bGotReload
						? (rightHandPos - posB).Length()
						: 1.0e30f;
					bool bSuppressIdlePassive = !bGripPressed
						&& pRightHand->m_bPomsonSuppressPassiveGripPoint
						&& !pRightHand->m_bPomsonSuppressReloadGripPoint;
					bool bSuppressReloadPassive = !bGripPressed
						&& pRightHand->m_bPomsonSuppressPassiveGripPoint
						&& pRightHand->m_bPomsonSuppressReloadGripPoint;
					bool bGotIdlePassive = bGotIdle && !bSuppressIdlePassive;
					bool bGotReloadPassive = bGotReload && !bSuppressReloadPassive;
					bool bReleasedActivePomsonGripThisFrame = pRightHand->m_bPomsonRightGripLatched
						&& pRightHand->m_bPomsonRightGripWasPressed
						&& !bGripPressed;

					if (bReleasedActivePomsonGripThisFrame)
					{
						pRightHand->m_bPomsonUseReloadGrip = bOldFlag;
					}
					else if (pRightHand->m_bPomsonRightUnlatchStartValid)
					{
						float otherDist = bOldFlag ? distIdle : distReload;
						bool bGotOther = bGripPressed
							? (bOldFlag ? bGotIdle : bGotReload)
							: (bOldFlag ? bGotIdlePassive : bGotReloadPassive);
						bool bCanSwitchToOther = bGotOther
							&& otherDist <= (bGripPressed ? gripRange : snapDist);
						pRightHand->m_bPomsonUseReloadGrip = bCanSwitchToOther ? !bOldFlag : bOldFlag;
					}
					else if (pRightHand->m_bPomsonRightGripLatched)
					{
						float currentDist = bOldFlag ? distReload : distIdle;
						float otherDist = bOldFlag ? distIdle : distReload;
						bool bGotCurrent = bOldFlag ? bGotReload : bGotIdle;
						bool bGotOther = bGripPressed
							? (bOldFlag ? bGotIdle : bGotReload)
							: (bOldFlag ? bGotIdlePassive : bGotReloadPassive);
						bool bPassiveSwitchToOther = !bGripPressed
							&& bGotOther
							&& otherDist <= snapDist
							&& (!bGotCurrent || currentDist > snapDist || otherDist < currentDist);

						// Active grip remains locked. Passive grip can migrate
						// when the real hand is closer to the other point in range.
						if (bPassiveSwitchToOther)
						{
							pRightHand->m_bPomsonUseReloadGrip = !bOldFlag;
						}
						else if (bGotCurrent)
						{
							pRightHand->m_bPomsonUseReloadGrip = bOldFlag;
						}
						else if (bGotReload)
						{
							pRightHand->m_bPomsonUseReloadGrip = true;
							pRightHand->m_bPomsonRightGripLatched = false;
							pRightHand->m_bPomsonRightLatchOffsetValid = false;
						}
						else if (bGotIdle)
						{
							pRightHand->m_bPomsonUseReloadGrip = false;
							pRightHand->m_bPomsonRightGripLatched = false;
							pRightHand->m_bPomsonRightLatchOffsetValid = false;
						}
						else
						{
							pRightHand->m_bPomsonUseReloadGrip = bOldFlag;
							pRightHand->m_bPomsonRightGripLatched = false;
							pRightHand->m_bPomsonRightLatchOffsetValid = false;
						}
					}
					else if (bGripPressed && bGotIdle && bGotReload)
					{
						pRightHand->m_bPomsonUseReloadGrip = (distReload < distIdle);
					}
					else if (bGripPressed && bGotIdle)
					{
						pRightHand->m_bPomsonUseReloadGrip = false;
					}
					else if (bGripPressed && bGotReload)
					{
						pRightHand->m_bPomsonUseReloadGrip = true;
					}
					else if (!bGripPressed && bGotIdle && bGotReload)
					{
						if (bGotIdlePassive && bGotReloadPassive)
							pRightHand->m_bPomsonUseReloadGrip = (distReload < distIdle);
						else if (bGotReloadPassive)
							pRightHand->m_bPomsonUseReloadGrip = true;
						else if (bGotIdlePassive)
							pRightHand->m_bPomsonUseReloadGrip = false;
						else
							pRightHand->m_bPomsonUseReloadGrip = bOldFlag;
					}
					else if (!bGripPressed && bGotIdlePassive)
					{
						pRightHand->m_bPomsonUseReloadGrip = false;
					}
					else if (!bGripPressed && bGotReloadPassive)
					{
						pRightHand->m_bPomsonUseReloadGrip = true;
					}
					else if (bGotIdle && bGotReload)
					{
						pRightHand->m_bPomsonUseReloadGrip = (distReload < distIdle);
					}
					else if (bGotReload)
					{
						pRightHand->m_bPomsonUseReloadGrip = true;
					}
					else
					{
						pRightHand->m_bPomsonUseReloadGrip = bOldFlag;
					}

					if (bOldFlag != pRightHand->m_bPomsonUseReloadGrip)
					{
						pRightHand->m_flTwoHandBlend = MAX( pRightHand->m_flTwoHandBlend * 0.3f, 0.0f );
						pRightHand->m_bPomsonRightGripLatched = false;
						pRightHand->m_bPomsonRightLatchOffsetValid = false;
						pRightHand->m_bPomsonRightGripLastWorldValid = false;
						pRightHand->m_bPomsonRightUnlatchStartValid = false;
					}

					if (tfvr_pomson_grip_debug.GetBool())
					{
						static float s_flLastPomsonTargetSelectDebug = 0.0f;
						if (gpGlobals->curtime - s_flLastPomsonTargetSelectDebug > 0.25f)
						{
							DevMsg("PomsonGrip SELECT: selected=%s old=%s latched=%d pressed=%d gotIdle=%d gotReload=%d distIdle=%.3f distReload=%.3f blend=%.3f\n",
								pRightHand->m_bPomsonUseReloadGrip ? "reload" : "idle",
								bOldFlag ? "reload" : "idle",
								pRightHand->m_bPomsonRightGripLatched ? 1 : 0,
								bGripPressed ? 1 : 0,
								bGotIdle ? 1 : 0,
								bGotReload ? 1 : 0,
								distIdle, distReload,
								pRightHand->m_flTwoHandBlend);
							s_flLastPomsonTargetSelectDebug = gpGlobals->curtime;
						}
					}
				}
				else if (!pRightHand->m_bRightHandDetached)
				{
					pRightHand->m_bPomsonUseReloadGrip = false;
					pRightHand->m_bPomsonRightGripLatched = false;
					pRightHand->m_bPomsonRightLatchOffsetValid = false;
					pRightHand->m_bPomsonRightGripLastWorldValid = false;
					pRightHand->m_bPomsonRightUnlatchStartValid = false;
					pRightHand->m_bPomsonRightGripWasPressed = false;
					pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
					pRightHand->m_bPomsonSuppressReloadGripPoint = false;
				}

				if (pRightHand->m_bRightHandDetached)
				{
					Vector targetPos;
					QAngle targetAng;
					float targetBlend = 0.0f;
					bool bRightGripInSnapZone = false;
					bool bRightGripPressedInRange = false;
					bool bCurrentPomsonGripPressed = false;
					float rightGrip = g_pOpenXRManager ? g_pOpenXRManager->GetAnalogValue("right_grip") : 0.0f;
					extern ConVar tfvr_pomson_pump_weapon_grip_threshold;

					bool bUseAnimatedPomsonTarget = pRightHand->m_bPomsonUseReloadGrip
						&& pRightHand->m_iLeverReloadSequence >= 0
						&& pRightHand->m_eReloadAnimState != VR_RELOAD_ANIM_NONE;
					if (pRightHand->GetPomsonDetachedRightHandTarget(targetPos, targetAng, bUseAnimatedPomsonTarget))
					{
						Vector rightWristPos = pRightHand->m_vecLastValidPosition;
						Vector rightHandPos = rightWristPos;
						COpenXRHandTracker *pRightTracker = pRightHand->GetHandTracker();
						if (pRightTracker)
						{
							Vector fp; QAngle fa;
							if (pRightTracker->GetHandJoint(false, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fp, fa))
								rightHandPos = fp;
						}

						float distance = (rightHandPos - targetPos).Length();
						float snapDist = tfvr_twohand_snap_distance.GetFloat();
						float blendDist = tfvr_twohand_blend_distance.GetFloat();
						float gripRange = tfvr_offhand_grip_range.GetFloat() * 0.393701f;

						bool bInSnapZone = distance <= snapDist;
						bRightGripInSnapZone = bInSnapZone;
						bool bGripPressed = rightGrip >= tfvr_pomson_pump_weapon_grip_threshold.GetFloat();
						bCurrentPomsonGripPressed = bGripPressed;
						bool bGripPressedInRange = bGripPressed && distance <= gripRange;
						bRightGripPressedInRange = bGripPressedInRange;
						bool bPassiveTargetSuppressed = !bGripPressed
							&& pRightHand->m_bPomsonSuppressPassiveGripPoint
							&& (pRightHand->m_bPomsonSuppressReloadGripPoint == pRightHand->m_bPomsonUseReloadGrip);
						bool bReleasedActivePomsonGrip = pRightHand->m_bPomsonRightGripLatched
							&& pRightHand->m_bPomsonRightGripWasPressed
							&& !bGripPressed;
						if (bReleasedActivePomsonGrip)
						{
							SetIdentityMatrix(pRightHand->m_matPomsonRightUnlatchStart);
							pRightHand->m_bPomsonRightUnlatchStartValid = true;
							pRightHand->m_bPomsonRightUnlatchUseReloadGrip = pRightHand->m_bPomsonUseReloadGrip;
							pRightHand->m_bPomsonRightGripLatched = false;
							pRightHand->m_bPomsonRightLatchOffsetValid = false;
							pRightHand->m_bPomsonSuppressPassiveGripPoint = true;
							pRightHand->m_bPomsonSuppressReloadGripPoint = pRightHand->m_bPomsonUseReloadGrip;
							bPassiveTargetSuppressed = true;
							targetBlend = 0.0f;
						}
						if (!pRightHand->m_bPomsonRightGripLatched
							&& ((!bPassiveTargetSuppressed && bInSnapZone) || bGripPressedInRange))
						{
							pRightHand->m_bPomsonRightGripLatched = true;
							pRightHand->m_bPomsonRightLatchOffsetValid = false;
							pRightHand->m_bPomsonRightUnlatchStartValid = false;
							if (bGripPressed
								|| pRightHand->m_bPomsonSuppressReloadGripPoint != pRightHand->m_bPomsonUseReloadGrip)
							{
								pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
							}
						}

						// Once snapped, keep targeting the grip point so small
						// controller jitter does not pull the rendered hand away.
						if (pRightHand->m_bPomsonRightGripLatched)
						{
							targetBlend = 1.0f;
							float releaseDist = MAX(blendDist, gripRange) * tfvr_offhand_grip_release_mult.GetFloat();
							bool bPomsonGripCanRelease = pRightHand->m_flTwoHandBlend >= kPomsonGripLockBlend
								|| distance > releaseDist * 1.5f;
							if (distance > releaseDist && !bGripPressed && bPomsonGripCanRelease)
							{
								SetIdentityMatrix(pRightHand->m_matPomsonRightUnlatchStart);
								pRightHand->m_bPomsonRightUnlatchStartValid = true;
								pRightHand->m_bPomsonRightUnlatchUseReloadGrip = pRightHand->m_bPomsonUseReloadGrip;
								pRightHand->m_bPomsonRightGripLatched = false;
								pRightHand->m_bPomsonRightLatchOffsetValid = false;
								targetBlend = 0.0f;
							}
							else if (tfvr_pomson_grip_debug.GetBool()
								&& distance > releaseDist && !bGripPressed && !bPomsonGripCanRelease)
							{
								static float s_flLastPomsonReleaseSuppressedDebug = 0.0f;
								if (gpGlobals->curtime - s_flLastPomsonReleaseSuppressedDebug > 0.25f)
								{
									DevMsg("PomsonGrip RELEASE suppressed until eased: grip=%s blend=%.3f distance=%.3f release=%.3f\n",
										pRightHand->m_bPomsonUseReloadGrip ? "reload" : "idle",
										pRightHand->m_flTwoHandBlend, distance, releaseDist);
									s_flLastPomsonReleaseSuppressedDebug = gpGlobals->curtime;
								}
							}
						}
						else if (distance <= snapDist && !bPassiveTargetSuppressed)
						{
							targetBlend = 1.0f;
						}
					}

					float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
					float easePower = tfvr_offhand_grip_ease_power.GetFloat();
					if (pRightHand->m_bPomsonRightGripLatched && targetBlend >= 1.0f)
					{
						// Pomson latch must reach the authoritative lock in finite
						// time. Exponential approach can visually stall short of
						// the target, leaving the hand in a start-dependent pose.
						float step = MAX(blendSpeed, 1.0f) * gpGlobals->frametime;
						pRightHand->m_flTwoHandBlend = MIN(1.0f, pRightHand->m_flTwoHandBlend + step);
					}
					else
					{
						pRightHand->m_flTwoHandBlend = EasedApproach(targetBlend, pRightHand->m_flTwoHandBlend,
							blendSpeed, gpGlobals->frametime, easePower);
					}
					if (targetBlend >= 1.0f && pRightHand->m_flTwoHandBlend >= kPomsonGripLockBlend)
						pRightHand->m_flTwoHandBlend = 1.0f;
					else if (targetBlend <= 0.0f && pRightHand->m_flTwoHandBlend < 0.001f)
					{
						pRightHand->m_flTwoHandBlend = 0.0f;
						pRightHand->m_bPomsonRightUnlatchStartValid = false;
						pRightHand->m_bPomsonRightGripLastWorldValid = false;
						pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
					}
					pRightHand->m_bPomsonRightGripWasPressed = bCurrentPomsonGripPressed
						&& pRightHand->m_bPomsonRightGripLatched;

					if (!pRightHand->m_bPomsonUseReloadGrip
						&& (pRightHand->m_bPomsonRightGripLatched || bRightGripInSnapZone || bRightGripPressedInRange)
						&& rightGrip >= tfvr_pomson_pump_weapon_grip_threshold.GetFloat())
					{
						bPomsonSnapTwoHandOnReattach = m_bOffhandGripActive;
						pRightHand->m_bRightHandDetached = false;
						pRightHand->m_bPomsonUseReloadGrip = false;
						pRightHand->m_bPomsonRightGripLatched = false;
						pRightHand->m_bPomsonRightLatchOffsetValid = false;
						pRightHand->m_bPomsonRightGripLastWorldValid = false;
						pRightHand->m_bPomsonRightUnlatchStartValid = false;
						pRightHand->m_bPomsonRightGripWasPressed = false;
						pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
						pRightHand->m_bPomsonSuppressReloadGripPoint = false;
						pRightHand->m_bPomsonDetachLeftToWeaponBoneValid = false;
						pRightHand->m_bPomsonDetachLeftToLeftHandBoneValid = false;
						pRightHand->m_flTwoHandBlend = 0.0f;
						pRightHand->m_bPlayingReloadAnim = false;
					}
				}
				else
				{
					pRightHand->m_bPomsonRightGripLatched = false;
					pRightHand->m_bPomsonRightLatchOffsetValid = false;
					pRightHand->m_flTwoHandBlend = 0.0f;
				}
			}
			else
			{
				// Not a Pomson - clear detach state
				if (pRightHand->m_bRightHandDetached)
				{
					pRightHand->m_bRightHandDetached = false;
					pRightHand->m_bPomsonUseReloadGrip = false;
					pRightHand->m_bPomsonRightGripLatched = false;
					pRightHand->m_bPomsonRightLatchOffsetValid = false;
					pRightHand->m_bPomsonRightGripLastWorldValid = false;
					pRightHand->m_bPomsonRightUnlatchStartValid = false;
					pRightHand->m_bPomsonRightGripWasPressed = false;
					pRightHand->m_bPomsonSuppressPassiveGripPoint = false;
					pRightHand->m_bPomsonSuppressReloadGripPoint = false;
					pRightHand->m_bPomsonDetachLeftToWeaponBoneValid = false;
					pRightHand->m_bPomsonDetachLeftToLeftHandBoneValid = false;
					pRightHand->m_flTwoHandBlend = 0.0f;
				}
			}

			Vector gripTargetPos;
			QAngle gripTargetAngles;

			if (!bStickyBusy && !bBisonBusy && !bManglerBusy && !bPomsonBusy && !bShotgunManualReloadBusy && pRightHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles))
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
				float flGripRangeCm = tfvr_offhand_grip_range.GetFloat();
				float flReleaseMult = tfvr_offhand_grip_release_mult.GetFloat();
				if ( pRightWpn && IsPumpActionShotgunWeaponID( pRightWpn->GetWeaponID() ) )
				{
					flGripRangeCm = tfvr_offhand_grip_shotgun_range.GetFloat();
					flReleaseMult = tfvr_offhand_grip_shotgun_release_mult.GetFloat();
				}
				float gripRange = flGripRangeCm * 0.393701f; // cm to inches (Source units)

				// Hysteresis: use larger range to release than to grab (prevents accidental ungrip)
				bool bWasGripActive = m_bOffhandGripActive;
				float effectiveRange = bWasGripActive
					? gripRange * flReleaseMult                              // Larger range to release
					: gripRange;                                              // Normal range to grab
				bool bWithinGripRange = distance <= effectiveRange;

				// Offhand grip is active when grip button is held AND within range
				bool bGripJustActivated = !m_bOffhandGripActive && bGripButtonPressed && bWithinGripRange;
				m_bOffhandGripActive = bGripButtonPressed && bWithinGripRange;

				// If Pomson reattached to the right hand while the left grip was
				// already held, enter the normal active two-hand solver at full
				// blend immediately instead of easing or waiting on range.
				if (bPomsonSnapTwoHandOnReattach && bGripButtonPressed)
				{
					m_bOffhandGripActive = true;
					bGripJustActivated = true;
					m_bWasOffhandGripActive = true;
					m_flTwoHandBlend = 1.0f;
					m_flGripRotationBlend = 1.0f;
				}

				// Pomson right-hand detach: when the right hand is detached, the
				// grip target comes from the right hand's skeleton which is now at
				// the controller, NOT at the weapon.  The range check is meaningless
				// in this state — keep the grip active as long as the left grip
				// button is held so the detach state doesn't immediately collapse.
				if (pRightHand && pRightHand->IsRightHandDetached() && bGripButtonPressed)
				{
					m_bOffhandGripActive = true;
					if (!bGripJustActivated && !m_bWasOffhandGripActive)
						bGripJustActivated = true;
				}

				// Bison idle grip is passive-only: no squeeze required,
				// no rotation influence - hand just follows by proximity.
				// Mangler idle grip supports active grip for a stable hold.
				if (bBisonOnIdleGrip)
				{
					m_bOffhandGripActive = false;
					bGripJustActivated = false;
					m_bWasOffhandGripActive = false;
				}

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
					// Pomson right-hand detach: the weapon follows the left
					// hand.  GetOffHandGripTarget now returns a corrected
					// position, so keep blending m_flTwoHandBlend toward 1.0
					// for left-hand finger posing.  Suppress grip ROTATION
					// since the weapon's aim is inherent to the left hand.
					bool bRightDetached = pRightHand && pRightHand->IsRightHandDetached();
					if (bRightDetached)
					{
						float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(1.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
						m_flGripRotationBlend = 0.0f;
					}
					else
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

					// Smoothly blend weapon rotation (separate speed) with easing.
				// Stickybomb: always suppress rotation (pump-only weapon).
				// Bison: suppress rotation when on reload grip, allow on idle grip.
				C_TFWeaponBase *pGripWeapon = pRightHand ? pRightHand->GetHeldWeapon() : NULL;
				bool bSuppressGripRotation = (pGripWeapon && pGripWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER);
				if (!bSuppressGripRotation && pGripWeapon
					&& pGripWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
					&& pRightHand->IsBisonOnReloadGrip())
				{
					bSuppressGripRotation = true;
				}
				if (!bSuppressGripRotation && pGripWeapon
					&& pGripWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
					&& pRightHand->IsManglerOnReloadGrip())
				{
					bSuppressGripRotation = true;
				}
				if (!bSuppressGripRotation)
				{
					float rotBlendSpeed = tfvr_offhand_grip_rotation_blend_speed.GetFloat();
					m_flGripRotationBlend = EasedApproach(1.0f, m_flGripRotationBlend, rotBlendSpeed, gpGlobals->frametime, easePower);
				}
				else
				{
					m_flGripRotationBlend = 0.0f;
				}

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
					} // else !bRightDetached
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
	bool bSkipRightHandMedigunPassiveGrip = IsRightHand()
		&& m_bRightHandDetached
		&& m_hHeldWeapon.Get()
		&& m_hHeldWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON;
	if (!bHandledBowReloadGrip && IsRightHand() && tfvr_twohand_enabled.GetBool() && !bSkipRightHandMedigunPassiveGrip)
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

			// Use the cached grip target (includes lever + fire animation position)
			// so the snap zone follows the lever when it's pushed forward.
			// Fall back to idle-based target if not cached yet.
			bool bGotGripTarget = false;
			if (pLeftHand->m_bMedigunGripTargetValid)
			{
				MatrixAngles(pLeftHand->m_matMedigunGripTarget, gripTargetAngles, gripTargetPos);
				bGotGripTarget = true;
			}
			else
			{
				bGotGripTarget = pLeftHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles);
			}

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

				float distance = (rightHandPos - gripTargetPos).Length();

				float snapDist = tfvr_twohand_snap_distance.GetFloat();
				float blendDist = tfvr_twohand_blend_distance.GetFloat();

				// Check if grip button is held for active lever grip
				extern ConVar tfvr_medigun_lever_grip_threshold;
				float flGripValue = 0.0f;
				if (g_pOpenXRManager)
					flGripValue = g_pOpenXRManager->GetAnalogValue("right_grip");
				bool bGripButtonHeld = flGripValue >= tfvr_medigun_lever_grip_threshold.GetFloat();

				float gripRange = tfvr_offhand_grip_range.GetFloat() * 0.393701f;
				bool bWasGripActive = m_bOffhandGripActive;

				if (bGripButtonHeld)
				{
					// Active lever grip: firm lock with larger release range
					float effectiveRange = bWasGripActive
						? gripRange * tfvr_medigun_lever_grip_release_mult.GetFloat()
						: gripRange;
					bool bWithinRange = distance <= effectiveRange;

					m_bOffhandGripActive = bWithinRange;
					if (m_bOffhandGripActive)
						m_bWasOffhandGripActive = true;

					if (m_bOffhandGripActive)
					{
						float leverBlendSpeed = tfvr_medigun_lever_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(1.0f, m_flTwoHandBlend, leverBlendSpeed, gpGlobals->frametime, easePower);
					}
					else
					{
						float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);

						if (m_flTwoHandBlend < 0.001f)
							m_bWasOffhandGripActive = false;
					}
				}
				else
				{
					// Passive two-handing: distance-based blending, no grip button
					if (bWasGripActive)
					{
						float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(0.0f, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);

						if (m_flTwoHandBlend < 0.001f)
							m_bWasOffhandGripActive = false;

						m_bOffhandGripActive = false;
					}
					else
					{
						float targetBlend = 0.0f;
						if (distance <= snapDist)
						{
							targetBlend = 1.0f;
						}
						else if (distance <= blendDist)
						{
							targetBlend = 1.0f - ((distance - snapDist) / (blendDist - snapDist));
						}

						float blendSpeed = tfvr_offhand_grip_blend_speed.GetFloat();
						float easePower = tfvr_offhand_grip_ease_power.GetFloat();
						m_flTwoHandBlend = EasedApproach(targetBlend, m_flTwoHandBlend, blendSpeed, gpGlobals->frametime, easePower);
					}
				}

				if (tfvr_twohand_debug.GetBool())
				{
					debugoverlay->AddBoxOverlay(gripTargetPos, Vector(-1,-1,-1), Vector(1,1,1),
						gripTargetAngles, 255, 0, 255, 128, 0.1f);
					debugoverlay->AddBoxOverlay(rightHandPos, Vector(-0.5f,-0.5f,-0.5f), Vector(0.5f,0.5f,0.5f),
						vec3_angle, 255, 0, 0, 128, 0.1f);
					debugoverlay->AddLineOverlay(rightHandPos, gripTargetPos,
						255, 0, 255, true, 0.1f);

					static float lastDebugTime = 0;
					if (gpGlobals->curtime - lastDebugTime > 0.5f)
					{
						DevMsg("Medigun RightGrip: dist=%.1f, gripActive=%d, blend=%.2f, gripBtn=%.2f\n",
							distance, m_bOffhandGripActive ? 1 : 0, m_flTwoHandBlend, flGripValue);
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
	// Per-weapon hand routing is centralized in TFVR_GetWeaponHand, which folds
	// together the global handedness toggle, the medigun (authored-left), and
	// per-weapon default flip (schema m_bFlipViewModel, e.g. the Huntsman).
	{
		C_TFWeaponBase *pActiveWeapon = pOwner->GetActiveTFWeapon();

		// Keep the authored pose side (and hand model) in sync with the active
		// weapon + handedness. Reloads the model in place when it changes.
		RefreshPoseHandSide();

		// Determine if this hand should hold the current weapon
		bool bThisHandShouldEquip = pActiveWeapon
			&& ( TFVR_DisplayWeaponOnLeft(pActiveWeapon) == IsLeftHand() );

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

				if (pCurrentHeld && m_iLastEquippedWeaponID >= 0
					&& pCurrentHeld->GetWeaponID() != m_iLastEquippedWeaponID)
				{
					// Class/loadout changes can mutate or reuse the same client
					// weapon entity with a different weapon ID. Pointer equality
					// would otherwise skip EquipWeapon(), leaving class-specific
					// pistol reload sequences/cycles from the previous class.
					bNeedsWeaponUpdate = true;
				}
				else if (pActiveWeapon != pCurrentHeld)
				{
					// TF2 recreates weapon entities during prediction/networking,
					// which changes the pointer even though it's the same weapon.
					// The old EHANDLE may be stale (entity destroyed), so compare
					// against the stored weapon ID from the last equip.
					if (pActiveWeapon && m_iLastEquippedWeaponID >= 0 &&
						pActiveWeapon->GetWeaponID() == m_iLastEquippedWeaponID &&
						(m_hRenderWeapon.Get() || (m_bHasGunslinger && V_stristr(pActiveWeapon->GetClassname(), "robot_arm"))))
					{
						m_hHeldWeapon = pActiveWeapon;
						pActiveWeapon->SetHeldByVRHand( true );
						C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
						if (pRenderWeapon)
							pRenderWeapon->SetSourceWeapon(pActiveWeapon);
					}
					else
					{
						bNeedsWeaponUpdate = true;
					}
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

	// Update left hand wearables (lifecycle management for watch, ball, shield)
	if (IsLeftHand())
	{
		UpdateLeftHandWatch();
		UpdateLeftHandBall();
		UpdateLeftHandShield();
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
//          Fallback chain:
//          1. grip_surface / palm_ext from action system (works across runtimes)
//          2. XR_HAND_JOINT_PALM_EXT from hand tracking (SteamVR primarily)
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetPalmTransform(VMatrix& outTransform)
{
	// Prefer palm pose from action system (grip_surface or palm_ext)
	if (g_pOpenXRManager && g_pOpenXRManager->IsPalmPoseSupported())
	{
		bool palmPoseValid = IsLeftHand() ?
			g_pOpenXRManager->GetLeftPalmPose(outTransform) :
			g_pOpenXRManager->GetRightPalmPose(outTransform);
		if (palmPoseValid)
			return true;
	}

	// Fall back to hand tracking palm joint
	if (!m_pHandTracker)
		return false;

	Vector palmPos;
	QAngle palmAngles;

	if (!m_pHandTracker->GetHandJoint(IsLeftHand(), XR_HAND_JOINT_PALM_EXT, palmPos, palmAngles))
		return false;

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

	if (!g_pOpenXRManager)
		return;

	// Need either hand tracking or palm pose from the action system
	if (!m_pHandTracker && !g_pOpenXRManager->IsPalmPoseSupported())
		return;

	// GetPalmTransform tries: action system palm (grip_surface/palm_ext) → hand tracking palm
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

	// Normal path: palm → wrist (hand tracking) → grip pose (universal)
	if (handValid)
	{
		m_vecLastValidPosition = palmMatrix.GetTranslation();
		MatrixAngles(palmMatrix.As3x4(), m_angLastValidAngles);
		m_bControllerTracked = true;
	}
	else
	{
		// Try wrist from hand tracking as secondary fallback
		VMatrix wristMatrix;
		if (m_pHandTracker && GetWristTransform(wristMatrix))
		{
			m_vecLastValidPosition = wristMatrix.GetTranslation();
			MatrixAngles(wristMatrix.As3x4(), m_angLastValidAngles);
			m_bControllerTracked = true;
		}
		else
		{
			// Final fallback: grip pose (closer to hand position than aim pose)
			VMatrix gripPose;
			if (IsLeftHand())
				m_bControllerTracked = g_pOpenXRManager->GetLeftControllerGripPose(gripPose);
			else
				m_bControllerTracked = g_pOpenXRManager->GetRightControllerGripPose(gripPose);

			if (m_bControllerTracked)
			{
				m_vecLastValidPosition = gripPose.GetTranslation();
				MatrixAngles(gripPose.As3x4(), m_angLastValidAngles);
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

	C_TFVRHand *pOtherHandForRocket = IsRightHand() ? GetLocalPlayerLeftHand() : GetLocalPlayerRightHand();
	bool bShowRocketReloadBodygroup = TFVR_IsManualRocketReloadActive(m_hHeldWeapon.Get())
		|| (pOtherHandForRocket && TFVR_IsManualRocketReloadActive(pOtherHandForRocket->GetHeldWeapon()));
	TFVR_SetReloadBodygroup(this, bShowRocketReloadBodygroup);

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

		if (m_bRightHandDetached && IsRightHand()
			&& m_flTwoHandBlend >= kPomsonGripLockBlend
			&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			Vector gripTargetPos;
			QAngle gripTargetAngles;
			bool bUsePumpAnimTarget = m_bPomsonUseReloadGrip
				&& m_iLeverReloadSequence >= 0
				&& m_eReloadAnimState != VR_RELOAD_ANIM_NONE;

			if (GetPomsonDetachedRightHandTarget(gripTargetPos, gripTargetAngles, bUsePumpAnimTarget))
			{
				int seqToSample = m_iIdleSequence;
				float cycleToSample = 0.0f;
				if (m_bPomsonUseReloadGrip && m_iReloadLoopSequence >= 0)
				{
					seqToSample = m_iReloadLoopSequence;
					if (bUsePumpAnimTarget)
					{
						seqToSample = m_iLeverReloadSequence;
						cycleToSample = m_flLeverReloadCycle;
					}
				}

				int safeBoneCount = MIN(numBones, MAXSTUDIOBONES);
				if (seqToSample >= 0 && m_iHandBone < safeBoneCount)
				{
					float poseParameters[MAXSTUDIOPOSEPARAM];
					memset(poseParameters, 0, sizeof(poseParameters));

					IBoneSetup gripBoneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters);
					Vector gripPosAnim[MAXSTUDIOBONES];
					Quaternion gripQAnim[MAXSTUDIOBONES];
					for (int i = 0; i < MAXSTUDIOBONES; i++)
					{
						gripPosAnim[i].Init();
						gripQAnim[i].Init(0, 0, 0, 1);
					}

					gripBoneSetup.InitPose(gripPosAnim, gripQAnim);
					gripBoneSetup.AccumulatePose(gripPosAnim, gripQAnim, seqToSample, cycleToSample,
						1.0f, gpGlobals->curtime, NULL);

					matrix3x4_t gripModelBones[MAXSTUDIOBONES];
					for (int i = 0; i < safeBoneCount; i++)
					{
						matrix3x4_t boneToParent;
						QuaternionMatrix(gripQAnim[i], gripPosAnim[i], boneToParent);

						const mstudiobone_t *pBone = pStudioHdr->pBone(i);
						if (!pBone)
						{
							SetIdentityMatrix(gripModelBones[i]);
							continue;
						}

						if (pBone->parent == -1)
							MatrixCopy(boneToParent, gripModelBones[i]);
						else if (pBone->parent >= 0 && pBone->parent < i)
							ConcatTransforms(gripModelBones[pBone->parent], boneToParent, gripModelBones[i]);
						else
							SetIdentityMatrix(gripModelBones[i]);
					}

					matrix3x4_t gripTargetWorld;
					AngleMatrix(gripTargetAngles, gripTargetPos, gripTargetWorld);

					matrix3x4_t gripAnchorDelta;
					matrix3x4_t invGripHand;
					MatrixInvert(gripModelBones[m_iHandBone], invGripHand);
					ConcatTransforms(gripTargetWorld, invGripHand, gripAnchorDelta);

					for (int i = 0; i < safeBoneCount && i < nMaxBones; i++)
					{
						ConcatTransforms(gripAnchorDelta, gripModelBones[i], pBoneToWorldOut[i]);
					}

					// Enforce the invariant used by the Pomson grip debug: the
					// solved hand bone center must match the computed target.
					Vector solvedHandPos;
					MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, solvedHandPos);
					Vector residual = gripTargetPos - solvedHandPos;
					if (residual.LengthSqr() > 0.000001f)
					{
						for (int i = 0; i < safeBoneCount && i < nMaxBones; i++)
						{
							Vector bonePos;
							MatrixGetColumn(pBoneToWorldOut[i], 3, bonePos);
							bonePos += residual;
							MatrixSetColumn(bonePos, 3, pBoneToWorldOut[i]);
						}
					}

					PositionWeaponFromBones(pBoneToWorldOut, nMaxBones);

					if (tfvr_twohand_debug.GetBool() || tfvr_pomson_grip_debug.GetBool())
					{
						MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, solvedHandPos);
						debugoverlay->AddBoxOverlay(gripTargetPos, Vector(-1.5f, -1.5f, -1.5f),
							Vector(1.5f, 1.5f, 1.5f), gripTargetAngles, 0, 255, 0, 180, 0.1f);
						debugoverlay->AddBoxOverlay(solvedHandPos, Vector(-1.25f, -1.25f, -1.25f),
							Vector(1.25f, 1.25f, 1.25f), vec3_angle, 255, 0, 0, 180, 0.1f);
						debugoverlay->AddLineOverlay(gripTargetPos, solvedHandPos, 255, 255, 255, true, 0.1f);

						static float s_flLastPomsonSnapDebug = 0.0f;
						if (tfvr_pomson_grip_debug.GetBool() && gpGlobals->curtime - s_flLastPomsonSnapDebug > 0.25f)
						{
							DevMsg("PomsonGrip SNAP: grip=%s target=(%.2f %.2f %.2f) wrist=(%.2f %.2f %.2f) error=%.3f pump=%d seq=%d cycle=%.3f\n",
								m_bPomsonUseReloadGrip ? "reload" : "idle",
								gripTargetPos.x, gripTargetPos.y, gripTargetPos.z,
								solvedHandPos.x, solvedHandPos.y, solvedHandPos.z,
								(solvedHandPos - gripTargetPos).Length(),
								bUsePumpAnimTarget ? 1 : 0, seqToSample, cycleToSample);
							s_flLastPomsonSnapDebug = gpGlobals->curtime;
						}
					}

					return true;
				}
			}
		}

		// If fire or charge animation is playing, use the actual current sequence
		// so the hand bone movement produces visible position offset on the weapon.
		// Draw animations with WRIST or FULL_ARM scope also drive the skeleton.
		// WEAPON_BONE scope draw anims leave the skeleton at idle (weapon_bone
		// is handled separately in ApplyWeaponPose).
		// Bread Bite: always use entity's live sequence/cycle so
		// StudioFrameAdvance drives the animation and the engine can interpolate.
		bool bUseCurrentAnim = m_bPlayingFireAnim || m_bPlayingChargeAnim
			|| (m_bPlayingDrawAnim && m_eDrawAnimScope >= VR_DRAW_ANIM_WRIST)
			|| m_bIsBreadBite
			|| (m_bAnimateIdle && m_bLoopIdleOnHand);
		int seqToSample;
		float cycleToSample;
		if (bUseCurrentAnim)
		{
			seqToSample = currentSeq;
			cycleToSample = currentCycle;
		}
		else
		{
			seqToSample = m_iIdleSequence;
			C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
			if (pHeldWeapon && pHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW
				&& m_iShotgunManualReloadSequence >= 0)
			{
				CTFCompoundBow *pBow = static_cast<CTFCompoundBow *>(pHeldWeapon);
				if (pBow->IsVRBowArrowNocking())
				{
					seqToSample = m_iShotgunManualReloadSequence;
					cycleToSample = Lerp(TFVR_GetBowNockVisualProgress(pBow),
						m_flShotgunManualReloadHoldCycle,
						m_flShotgunManualReloadCommitCycle);
				}
				else if (pBow->IsVRBowArrowNocked())
				{
					seqToSample = m_iShotgunManualReloadSequence;
					cycleToSample = m_flShotgunManualReloadCommitCycle;
				}
				else if (m_iBowIdleSequence >= 0)
				{
					seqToSample = m_iBowIdleSequence;
					cycleToSample = m_flBowIdleCycle;
				}
				else
				{
					seqToSample = m_iShotgunManualReloadSequence;
					cycleToSample = m_flShotgunManualReloadHoldCycle;
				}
			}
			else if (m_bLoopIdleOnHand && m_iIdleSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				float duration = pHdr ? SequenceDuration(pHdr, m_iIdleSequence) : 1.0f;
				cycleToSample = (duration > 0.0f) ? fmod(gpGlobals->curtime / duration, 1.0f) : 0.0f;
			}
			else
			{
				cycleToSample = 0.0f;
			}
		}

		// Backstab transition state machine:
		//   RAISING    (m_bBackstabReady becomes true):   knife_backstab_up    cycle 0→1
		//   HOLDING    (raising finished):                knife_backstab_idle  cycle 0
		//   LOWERING   (m_bBackstabReady becomes false):  knife_backstab_down  cycle 0→1
		//   ATTACKING  (backstab cooldown active):        knife_backstab       cycle 0→1
		//   IDLE       (lowering/attack finished):        normal weapon idle
		bool bWantsBackstab = m_bBackstabReady && m_iBackstabUpSequence >= 0 && !bUseCurrentAnim;

		// Advance the backstab state machine only once per game frame.
		float flCurTime = gpGlobals->curtime;
		if (flCurTime != m_flLastBackstabUpdateTime)
		{
			m_flLastBackstabUpdateTime = flCurTime;
			float flDt = gpGlobals->frametime;

			if (m_bBackstabAttacking)
			{
				if (m_iBackstabAttackSequence >= 0 && flDt > 0.0f)
				{
					float flSeqDuration = SequenceDuration(pStudioHdr, m_iBackstabAttackSequence);
					if (flSeqDuration > 0.0f)
						m_flBackstabCycle = MIN(m_flBackstabCycle + flDt / flSeqDuration, 1.0f);
					else
						m_flBackstabCycle = 1.0f;
				}
				else
				{
					m_flBackstabCycle = 1.0f;
				}
			}
			else
			{
				bool bMidTransition = m_flBackstabCycle > 0.0f && m_flBackstabCycle < 1.0f;

				if (!bMidTransition)
				{
					if (bWantsBackstab && !m_bBackstabRaising)
					{
						m_bBackstabRaising = true;
						m_bBackstabLowering = false;
						m_flBackstabCycle = 0.0f;
					}
					else if (!bWantsBackstab && (m_bBackstabRaising || (!m_bBackstabLowering && m_flBackstabCycle >= 1.0f)))
					{
						m_bBackstabRaising = false;
						m_bBackstabLowering = true;
						m_flBackstabCycle = 0.0f;
					}
				}

				if (m_bBackstabRaising || m_bBackstabLowering)
				{
					extern ConVar tfvr_backstab_speed;
					float flSpeed = MAX(tfvr_backstab_speed.GetFloat(), 0.01f);
					int transSeq = m_bBackstabRaising ? m_iBackstabUpSequence : m_iBackstabDownSequence;
					if (transSeq >= 0 && flDt > 0.0f)
					{
						extern ConVar tfvr_backstab_duration;
						float flSeqDuration = SequenceDuration(pStudioHdr, transSeq);
						float flMinDuration = tfvr_backstab_duration.GetFloat();
						float flDuration = MAX(flSeqDuration, flMinDuration);

						if (flDuration > 0.0f)
							m_flBackstabCycle = MIN(m_flBackstabCycle + (flDt * flSpeed) / flDuration, 1.0f);
						else
							m_flBackstabCycle = 1.0f;
					}
					else
					{
						m_flBackstabCycle = 1.0f;
					}

					if (m_bBackstabLowering && m_flBackstabCycle >= 1.0f)
					{
						m_bBackstabLowering = false;
						m_flBackstabCycle = 0.0f;
					}
				}
			}
		}

		bool bBackstabPose = m_bBackstabAttacking || m_bBackstabRaising || m_bBackstabLowering;

		int backstabSeqForSample = -1;
		float backstabCycleForSample = 0.0f;
		if (bBackstabPose)
		{
			if (m_bBackstabAttacking && m_iBackstabAttackSequence >= 0)
			{
				backstabSeqForSample = m_iBackstabAttackSequence;
				backstabCycleForSample = m_flBackstabCycle;
			}
			else if (m_bBackstabRaising && m_flBackstabCycle >= 1.0f && m_iBackstabIdleSequence >= 0)
			{
				backstabSeqForSample = m_iBackstabIdleSequence;
				backstabCycleForSample = 0.0f;
			}
			else if (m_bBackstabRaising && m_iBackstabUpSequence >= 0)
			{
				backstabSeqForSample = m_iBackstabUpSequence;
				backstabCycleForSample = m_flBackstabCycle;
			}
			else if (m_bBackstabLowering && m_iBackstabDownSequence >= 0)
			{
				backstabSeqForSample = m_iBackstabDownSequence;
				backstabCycleForSample = m_flBackstabCycle;
			}

			if (backstabSeqForSample >= 0)
			{
				seqToSample = backstabSeqForSample;
				cycleToSample = backstabCycleForSample;
			}
		}

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

		// Bow nock/pull pose. Blend the authored bw_draw nock pose into
		// bw_charge by pull amount so full de-pull returns to the natural nocked
		// rest while full pull reaches bw_charge. Shake offsets use the same
		// blended base, avoiding mismatched bow/string drift.
		{
			C_TFWeaponBase *pBowSmooth = m_hHeldWeapon.Get();
			CTFCompoundBow *pBowS = (pBowSmooth && pBowSmooth->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
				? static_cast<CTFCompoundBow *>(pBowSmooth) : NULL;
			if (pBowS && !m_bPlayingFireAnim
				&& (pBowS->IsVRBowArrowNocking() || pBowS->IsVRBowArrowNocked()))
			{
				float flChargeFraction = clamp(pBowS->GetCurrentCharge() / MAX(pBowS->GetChargeMaxTime(), 0.01f), 0.0f, 1.0f);
				const bool bUseFullChargeReference = true;
				ApplyBowDrawChargeBlend(pStudioHdr, numBones, posAnim, qAnim, flChargeFraction);
				ApplyBowShakeOverlay(pStudioHdr, numBones, posAnim, qAnim, flChargeFraction, bUseFullChargeReference);

				if (m_iBowIdleSequence >= 0 && pBowS->IsVRBowArrowNocking())
				{
					float flNockBlend = TFVR_GetBowNockVisualProgress(pBowS);
					Vector posNockTarget[MAXSTUDIOBONES];
					Quaternion qNockTarget[MAXSTUDIOBONES];
					Vector posBowIdle[MAXSTUDIOBONES];
					Quaternion qBowIdle[MAXSTUDIOBONES];
					for (int i = 0; i < MAXSTUDIOBONES; i++)
					{
						posNockTarget[i] = posAnim[i];
						qNockTarget[i] = qAnim[i];
						posBowIdle[i].Init();
						qBowIdle[i].Init(0, 0, 0, 1);
					}
					boneSetup.InitPose(posBowIdle, qBowIdle);
					boneSetup.AccumulatePose(posBowIdle, qBowIdle, m_iBowIdleSequence, m_flBowIdleCycle, 1.0f, gpGlobals->curtime, NULL);
					for (int i = 0; i < numBones; i++)
					{
						// Ease the arrow with the hand/string. The render weapon copies
						// weapon_bone_4 from this blended hand pose, so skipping it here
						// makes the arrow snap to the bow before the hand finishes.
						VectorLerp(posBowIdle[i], posAnim[i], flNockBlend, posAnim[i]);
						QuaternionSlerp(qBowIdle[i], qAnim[i], flNockBlend, qAnim[i]);
					}

					const int iStringBone = LookupBone("weapon_bone_3");
					if (iStringBone >= 0)
					{
						int targetBones[2];
						int nTargetBones = 0;

						int iHandBone = LookupBone("bip_hand_L");
						if (iHandBone < 0)
							iHandBone = LookupBone("ValveBiped.Bip01_L_Hand");
						if (iHandBone < 0)
							iHandBone = LookupBone("bip_hand_l");
						if (iHandBone >= 0)
							targetBones[nTargetBones++] = iHandBone;

						int iArrowBone = LookupBone("weapon_bone_4");
						if (iArrowBone >= 0 && iHandBone < 0)
							targetBones[nTargetBones++] = iArrowBone;

						TFVR_BlendPoseBonesRelativeToReference(pStudioHdr, numBones,
							posBowIdle, qBowIdle, posNockTarget, qNockTarget, flNockBlend,
							iStringBone, targetBones, nTargetBones, posAnim, qAnim);
					}
				}
			}
		}

		// Bread Bite crossfade: detect sequence changes once per game frame
		// and smoothly blend from the old animation to the new one.
		if (m_bIsBreadBite && gpGlobals->curtime != m_flBBLastCrossfadeCheck)
		{
			m_flBBLastCrossfadeCheck = gpGlobals->curtime;

			if (seqToSample != m_iBBLastSampledSeq && m_iBBLastSampledSeq >= 0)
			{
				m_iBBCrossfadeFromSeq = m_iBBLastSampledSeq;
				m_flBBCrossfadeFromCycle = m_flBBLastSampledCycle;
				m_flBBCrossfadeStart = gpGlobals->curtime;
			}
			m_iBBLastSampledSeq = seqToSample;
			m_flBBLastSampledCycle = cycleToSample;
		}

		if (m_bIsBreadBite && m_iBBCrossfadeFromSeq >= 0)
		{
			const float BB_BLEND_TIME = 0.2f;
			float elapsed = gpGlobals->curtime - m_flBBCrossfadeStart;
			if (elapsed >= BB_BLEND_TIME)
			{
				m_iBBCrossfadeFromSeq = -1;
			}
			else
			{
				float oldCycle = m_flBBCrossfadeFromCycle;
				float oldDur = SequenceDuration(pStudioHdr, m_iBBCrossfadeFromSeq);
				if (oldDur > 0.0f)
					oldCycle = MIN(m_flBBCrossfadeFromCycle + elapsed / oldDur, 1.0f);

				Vector posOld[MAXSTUDIOBONES];
				Quaternion qOld[MAXSTUDIOBONES];
				for (int i = 0; i < MAXSTUDIOBONES; i++)
				{
					posOld[i].Init();
					qOld[i].Init(0, 0, 0, 1);
				}
				boneSetup.InitPose(posOld, qOld);
				boneSetup.AccumulatePose(posOld, qOld, m_iBBCrossfadeFromSeq, oldCycle, 1.0f, gpGlobals->curtime, NULL);

				float blend = SimpleSpline(elapsed / BB_BLEND_TIME);
				for (int i = 0; i < numBones; i++)
				{
					VectorLerp(posOld[i], posAnim[i], blend, posAnim[i]);
					QuaternionSlerp(qOld[i], qAnim[i], blend, qAnim[i]);
				}
			}
		}

		if (m_bPlayingFireAnim && m_bBowFireStartPoseValid
			&& m_iBowIdleSequence >= 0 && seqToSample == m_iBowIdleSequence)
		{
			C_TFWeaponBase *pBowFireWeapon = m_hHeldWeapon.Get();
			if (pBowFireWeapon && pBowFireWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
				ApplyBowFireStartPoseBlend(pStudioHdr, numBones, posAnim, qAnim, cycleToSample);
		}

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

		bool bMedigunFireActive = (m_eMedigunFireState != MEDIGUN_FIRE_IDLE) && m_bPlayingFireAnim;

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

		// Pin hand bone to controller when the sampled animation may move the
		// hand bone away from idle (backstab, Bread Bite, bread creature jars).
		// During draw: also pin bread creature hands so the deploy animation
		// drives vm_weapon bones (creature visuals) without wrist displacement.
		// Bow: the bow-holding hand must never displace during charge/fire — the
		// arrow/charge animation drives the bow bones, but the hand stays locked
		// to the controller. Pin it so only weapon_bone (and the bow) animate.
		C_TFWeaponBase *pHeldForPin = m_hHeldWeapon.Get();
		bool bBowHoldingHand = pHeldForPin && pHeldForPin->GetWeaponID() == TF_WEAPON_COMPOUND_BOW;
		bool bPinToSampled = bBackstabPose || m_bIsBreadBite || m_bBreadCreaturePin
			|| (m_bAnimateIdle && m_bPlayingDrawAnim)
			|| bBowHoldingHand;

		// Cache the hand bone transform from the sampled animation.
		// Used by the normal anchor path for stable weapon positioning.
		// For hands WITH a weapon: cache once (stable idle offset).
		// For hands WITHOUT a weapon (e.g. right hand passive grip): recache every frame
		// because the sampled animation changes as m_flTwoHandBlend transitions.
		if (!m_bHandBoneOffsetValid || !m_hHeldWeapon.Get())
		{
			// For bread creatures with wrist motion (Bread Sapper), sampledBones
			// comes from the creature idle which moves per frame.  Build the
			// weapon POSE skeleton once so the anchor stays fixed relative to
			// the static grip — both hand and weapon then move together.
			if (bUseCurrentAnim && m_bAnimateIdle && m_bLoopIdleOnHand
				&& !bPinToSampled && m_iIdleSequence >= 0)
			{
				Vector posWpn[MAXSTUDIOBONES];
				Quaternion qWpn[MAXSTUDIOBONES];
				for (int i = 0; i < numBones; i++)
				{
					posWpn[i].Init();
					qWpn[i].Init(0, 0, 0, 1);
				}
				boneSetup.InitPose(posWpn, qWpn);
				boneSetup.AccumulatePose(posWpn, qWpn, m_iIdleSequence, 0.0f,
					1.0f, gpGlobals->curtime, NULL);

				matrix3x4_t wpnPoseBones[MAXSTUDIOBONES];
				for (int i = 0; i < numBones; i++)
				{
					matrix3x4_t boneToParent;
					QuaternionMatrix(qWpn[i], posWpn[i], boneToParent);
					const mstudiobone_t *pBone = pStudioHdr->pBone(i);
					if (!pBone)
					{
						SetIdentityMatrix(wpnPoseBones[i]);
						continue;
					}
					if (pBone->parent == -1)
						MatrixCopy(boneToParent, wpnPoseBones[i]);
					else if (pBone->parent >= 0 && pBone->parent < numBones)
						ConcatTransforms(wpnPoseBones[pBone->parent], boneToParent, wpnPoseBones[i]);
					else
						SetIdentityMatrix(wpnPoseBones[i]);
				}
				MatrixCopy(wpnPoseBones[m_iHandBone], m_matIdleHandBoneTransform);
			}
			else
			{
				MatrixCopy(sampledBones[m_iHandBone], m_matIdleHandBoneTransform);
			}
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

		// Apply offhand grip rotation to the weapon hand.  The legacy path was
		// right-hand only; role lookup lets flipped/left-handed weapons consume
		// the support hand's two-hand solve too.
		if (tfvr_offhand_grip_enabled.GetBool())
		{
			C_TFVRHand *pGripHand = NULL;
			C_TFWeaponBase *pGripWeapon = m_hHeldWeapon.Get();
			if (pGripWeapon)
			{
				pGripHand = TFVR_GetSupportHand(pGripWeapon);
				if (pGripHand == this)
					pGripHand = NULL;
			}
			else if (IsRightHand())
			{
				pGripHand = GetLocalPlayerLeftHand();
			}

			float rotationBlend = pGripHand ? pGripHand->GetGripRotationBlend() : 0.0f;
			bool bWasGripActive = pGripHand && pGripHand->WasOffhandGripActive();
			bool bIsGripActive = pGripHand && pGripHand->IsOffhandGripActive();

			// Apply rotation when grip was active (includes blend-out after release)
			// Uses separate rotation blend that starts fresh each time grip activates
			if (rotationBlend > 0.001f && bWasGripActive)
			{
				Vector desiredY = pGripHand->GetOffhandGripForward();

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

					// Bow: align the ARROW axis (weapon_bone -> weapon_bone_4) to
					// the draw line so the nock points at the draw hand, instead of
					// aligning the hand's Y axis. desiredY is the canonical/reflected
					// draw direction (draw hand -> bow grip); the arrow's grip->nock
					// should point the opposite way (toward the draw hand).
					C_TFWeaponBase *pHeldGripWeapon = m_hHeldWeapon.Get();
					bool bBowArrowAlign = false;
					if (pHeldGripWeapon && pHeldGripWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW
						&& m_iHandBone >= 0 && m_iHandBone < numBones)
					{
						int iGripWBone = LookupBone("weapon_bone");
						if (iGripWBone < 0)
							iGripWBone = LookupBone("vm_weapon_bone");
						int iGripArrowBone = LookupBone("weapon_bone_4");
						if (iGripWBone >= 0 && iGripWBone < numBones
							&& iGripArrowBone >= 0 && iGripArrowBone < numBones)
						{
							Vector vWBonePos, vArrowPos;
							MatrixGetColumn(sampledBones[iGripWBone], 3, vWBonePos);
							MatrixGetColumn(sampledBones[iGripArrowBone], 3, vArrowPos);
							Vector vArrowModel = vArrowPos - vWBonePos;
							Vector vArrowLocal;
							VectorIRotate(vArrowModel, sampledBones[m_iHandBone], vArrowLocal);
							if (vArrowLocal.NormalizeInPlace() > 1e-4f)
							{
								Vector desiredArrow = -desiredY;
								AlignLocalAxisToWorldDir(controllerTransform, vArrowLocal, desiredArrow);
								bBowArrowAlign = true;
							}
						}
					}

					// Apply full grip rotation (non-bow weapons align the hand Y).
					if (!bBowArrowAlign)
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

		// Calculate anchor delta from hand bone to controller
		// anchorDelta = controller * inverse(handBone)
		// When backstab or bread bite animation is active, the sampled
		// animation differs from idle, so use the current frame's sampled
		// hand bone to keep the hand pinned to the controller.  Child bones
		// (fingers, vm_weapon chain) still show animation-relative motion.
		matrix3x4_t anchorDelta;

		if (bPinToSampled)
		{
			matrix3x4_t invSampledHand;
			MatrixInvert(sampledBones[m_iHandBone], invSampledHand);
			ConcatTransforms(controllerTransform, invSampledHand, anchorDelta);
		}
		else if (m_bHandBoneOffsetValid)
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

		{
			C_TFWeaponBase *pBowShakeWeapon = m_hHeldWeapon.Get();
			CTFCompoundBow *pBowShake = (pBowShakeWeapon && pBowShakeWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
				? static_cast<CTFCompoundBow *>(pBowShakeWeapon) : NULL;
			if (pBowShake && !m_bPlayingFireAnim
				&& (pBowShake->IsVRBowArrowNocking() || pBowShake->IsVRBowArrowNocked()))
			{
				const float flBowShakeCharge = clamp(pBowShake->GetCurrentCharge() / MAX(pBowShake->GetChargeMaxTime(), 0.01f), 0.0f, 1.0f);
				const bool bUseFullChargeReference = true;
				ApplyBowShakeWorldOverlay(pStudioHdr, pBoneToWorldOut, nMaxBones, anchorDelta, flBowShakeCharge, bUseFullChargeReference);
			}
		}

		// Medigun fire: keep rotation from fire animations but pin position.
		// The idle anchor lets the fire animation's rotation flow through to
		// weapon_bone and fingers, but may also translate the hand.  Correct
		// by sliding the entire skeleton so the hand bone stays at the
		// controller position — rotations are preserved.
		if (bMedigunFireActive && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			Vector handWorldPos;
			MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, handWorldPos);

			Vector controllerPos;
			MatrixGetColumn(controllerTransform, 3, controllerPos);

			Vector correction = controllerPos - handWorldPos;

			for (int i = 0; i < numBones && i < nMaxBones; i++)
			{
				Vector bonePos;
				MatrixGetColumn(pBoneToWorldOut[i], 3, bonePos);
				bonePos += correction;
				MatrixSetColumn(bonePos, 3, pBoneToWorldOut[i]);
			}
		}

		// WRIST scope draw animation: the idle anchor preserves correct motion
		// directions but lets the hand bone displace from the controller (like
		// FULL_ARM). Correct this by translating the entire skeleton so the
		// hand bone snaps back to the controller position.  Rotations are kept
		// so wrist twist and all child-bone motion stay oriented properly.
		if (m_bPlayingDrawAnim && m_eDrawAnimScope == VR_DRAW_ANIM_WRIST
			&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			Vector handWorldPos;
			MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, handWorldPos);

			Vector controllerPos;
			MatrixGetColumn(controllerTransform, 3, controllerPos);

			Vector correction = controllerPos - handWorldPos;

			for (int i = 0; i < numBones && i < nMaxBones; i++)
			{
				Vector bonePos;
				MatrixGetColumn(pBoneToWorldOut[i], 3, bonePos);
				bonePos += correction;
				MatrixSetColumn(bonePos, 3, pBoneToWorldOut[i]);
			}
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
		bool bShotgunManualReloadPoseApplied = false;
		if (m_hHeldWeapon.Get())
		{
			if (!bBackstabPose)
			{
				if (IsBareFists(m_hHeldWeapon.Get()) ||
					(m_bHasGunslinger && V_stristr(m_hHeldWeapon->GetClassname(), "robot_arm")))
				{
					// Bare fists / Gunslinger: use finger tracking so the hand
					// follows the player's real fingers.
					ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
				}
				else if (m_bRightHandDetached
					&& (m_flTwoHandBlend < 0.01f
						|| (!m_bPomsonRightGripLatched && m_bPomsonRightUnlatchStartValid)))
				{
					// Pomson: right hand is free, or is blending out from a
					// released grip point, so the live end pose is the controller hand.
					ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
				}
				else if (m_bRightHandDetached && m_bPomsonUseReloadGrip
					&& (m_flTwoHandBlend >= 0.01f || m_bPomsonRightGripLatched)
					&& !m_bPlayingReloadAnim && m_iReloadLoopSequence >= 0)
				{
					// Pomson detached reload grip idle: use reload loop frame 0
					// so the right hand grabs the pump handle before motion starts.
					ApplyWeaponPose(pBoneToWorldOut, nMaxBones, NULL, m_iReloadLoopSequence, 0.0f);
				}
				else
				{
					// Normal idle: overlay the weapon grip pose onto fingers + weapon_bone
					ApplyWeaponPose(pBoneToWorldOut, nMaxBones);
				}
			}

			if (m_bRightHandDetached && IsRightHand()
				&& m_flTwoHandBlend > 0.001f
				&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
			{
				Vector gripTargetPos;
				QAngle gripTargetAngles;
				bool bUsePumpAnimTarget = m_bPomsonUseReloadGrip
					&& m_iLeverReloadSequence >= 0
					&& m_eReloadAnimState != VR_RELOAD_ANIM_NONE;
				if (GetPomsonDetachedRightHandTarget(gripTargetPos, gripTargetAngles, bUsePumpAnimTarget))
				{
					matrix3x4_t gripTargetWorld;
					AngleMatrix(gripTargetAngles, gripTargetPos, gripTargetWorld);

					if (m_bPomsonRightGripLatched && !m_bPomsonRightLatchOffsetValid)
					{
						// Store the easing start relative to the moving grip target, not
						// world space, so player/weapon motion does not drag the hand behind.
						matrix3x4_t invGripTargetWorld;
						MatrixInvert(gripTargetWorld, invGripTargetWorld);
						ConcatTransforms(invGripTargetWorld, pBoneToWorldOut[m_iHandBone], m_matPomsonRightLatchOffset);
						m_bPomsonRightLatchOffsetValid = true;
						m_bPomsonRightUnlatchStartValid = false;
					}

					matrix3x4_t latchStartWorld;
					bool bHaveLatchStartWorld = m_bPomsonRightGripLatched && m_bPomsonRightLatchOffsetValid;
					if (bHaveLatchStartWorld)
					{
						ConcatTransforms(gripTargetWorld, m_matPomsonRightLatchOffset, latchStartWorld);
					}
					bool bBlendOutFromUnlatch = !m_bPomsonRightGripLatched && m_bPomsonRightUnlatchStartValid;
					matrix3x4_t unlatchStartWorld;
					if (bBlendOutFromUnlatch)
					{
						matrix3x4_t unlatchTargetWorld;
						if (m_bPomsonRightUnlatchUseReloadGrip == m_bPomsonUseReloadGrip)
						{
							MatrixCopy(gripTargetWorld, unlatchTargetWorld);
						}
						else
						{
							Vector unlatchTargetPos;
							QAngle unlatchTargetAngles;
							bool bSavedUseReloadGrip = m_bPomsonUseReloadGrip;
							m_bPomsonUseReloadGrip = m_bPomsonRightUnlatchUseReloadGrip;
							bool bUseUnlatchPumpAnimTarget = m_bPomsonUseReloadGrip
								&& m_iLeverReloadSequence >= 0
								&& m_eReloadAnimState != VR_RELOAD_ANIM_NONE;
							bool bGotUnlatchTarget = GetPomsonDetachedRightHandTarget(
								unlatchTargetPos, unlatchTargetAngles, bUseUnlatchPumpAnimTarget);
							m_bPomsonUseReloadGrip = bSavedUseReloadGrip;

							if (bGotUnlatchTarget)
								AngleMatrix(unlatchTargetAngles, unlatchTargetPos, unlatchTargetWorld);
							else
								MatrixCopy(gripTargetWorld, unlatchTargetWorld);
						}
						MatrixCopy(unlatchTargetWorld, unlatchStartWorld);
					}

					float easePower = tfvr_offhand_grip_ease_power.GetFloat();
					float easedBlend = ApplyEaseOutToBlend(m_flTwoHandBlend, easePower, m_bPomsonRightGripLatched);
					bool bPomsonLatchLocked = m_bPomsonRightGripLatched && m_flTwoHandBlend >= 1.0f;
					if (bPomsonLatchLocked)
						easedBlend = 1.0f;

					Vector livePos;
					QAngle liveAngles;
					if (bHaveLatchStartWorld)
						MatrixAngles(latchStartWorld, liveAngles, livePos);
					else
						MatrixAngles(pBoneToWorldOut[m_iHandBone], liveAngles, livePos);

					Quaternion liveQuat;
					AngleQuaternion(liveAngles, liveQuat);

					Quaternion gripQuat, blendTargetQuat, blendedQuat;
					AngleQuaternion(gripTargetAngles, gripQuat);
					Vector blendTargetPos = gripTargetPos;
					blendTargetQuat = gripQuat;
					if (bBlendOutFromUnlatch)
					{
						MatrixAngles(unlatchStartWorld, blendTargetQuat, blendTargetPos);
					}
					Vector blendedPos;
					Vector desiredHandPos;
					Quaternion desiredHandQuat;
					if (easedBlend >= 1.0f)
					{
						desiredHandPos = blendTargetPos;
						desiredHandQuat = blendTargetQuat;
					}
					else
					{
						VectorLerp(livePos, blendTargetPos, easedBlend, desiredHandPos);
						SafeQuaternionSlerp(liveQuat, blendTargetQuat, easedBlend, desiredHandQuat);
					}
					matrix3x4_t desiredHandWorld;
					QuaternionMatrix(desiredHandQuat, desiredHandPos, desiredHandWorld);

					int seqToSample = m_iIdleSequence;
					float cycleToSample = 0.0f;
					bool bSampleReloadGrip = bBlendOutFromUnlatch
						? m_bPomsonRightUnlatchUseReloadGrip
						: m_bPomsonUseReloadGrip;
					if (bSampleReloadGrip && m_iReloadLoopSequence >= 0)
					{
						seqToSample = m_iReloadLoopSequence;
						bool bSamplePumpAnimTarget = bBlendOutFromUnlatch
							? (m_bPomsonRightUnlatchUseReloadGrip
								&& m_iLeverReloadSequence >= 0
								&& m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
							: bUsePumpAnimTarget;
						if (bSamplePumpAnimTarget)
						{
							seqToSample = m_iLeverReloadSequence;
							cycleToSample = m_flLeverReloadCycle;
						}
					}

					matrix3x4_t debugHardSnapHand;
					bool bDebugHaveHardSnapHand = false;
					bool bAppliedGripPose = false;
					if (seqToSample >= 0)
					{
						float poseParameters[MAXSTUDIOPOSEPARAM];
						memset(poseParameters, 0, sizeof(poseParameters));

						IBoneSetup gripBoneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters);
						Vector gripPosAnim[MAXSTUDIOBONES];
						Quaternion gripQAnim[MAXSTUDIOBONES];
						for (int i = 0; i < MAXSTUDIOBONES; i++)
						{
							gripPosAnim[i].Init();
							gripQAnim[i].Init(0, 0, 0, 1);
						}

						gripBoneSetup.InitPose(gripPosAnim, gripQAnim);
						gripBoneSetup.AccumulatePose(gripPosAnim, gripQAnim, seqToSample, cycleToSample,
							1.0f, gpGlobals->curtime, NULL);

						matrix3x4_t gripModelBones[MAXSTUDIOBONES];
						for (int i = 0; i < numBones && i < MAXSTUDIOBONES; i++)
						{
							matrix3x4_t boneToParent;
							QuaternionMatrix(gripQAnim[i], gripPosAnim[i], boneToParent);

							const mstudiobone_t *pBone = pStudioHdr->pBone(i);
							if (!pBone)
							{
								SetIdentityMatrix(gripModelBones[i]);
								continue;
							}

							if (pBone->parent == -1)
								MatrixCopy(boneToParent, gripModelBones[i]);
							else if (pBone->parent >= 0 && pBone->parent < i)
								ConcatTransforms(gripModelBones[pBone->parent], boneToParent, gripModelBones[i]);
							else
								SetIdentityMatrix(gripModelBones[i]);
						}

						matrix3x4_t invGripHand;
						MatrixInvert(gripModelBones[m_iHandBone], invGripHand);

						matrix3x4_t gripAnchorDelta;
						ConcatTransforms(gripTargetWorld, invGripHand, gripAnchorDelta);
						ConcatTransforms(gripAnchorDelta, gripModelBones[m_iHandBone], debugHardSnapHand);
						bDebugHaveHardSnapHand = true;
						matrix3x4_t blendTargetAnchorDelta;
						if (bBlendOutFromUnlatch)
							ConcatTransforms(unlatchStartWorld, invGripHand, blendTargetAnchorDelta);
						else
							MatrixCopy(gripAnchorDelta, blendTargetAnchorDelta);
						matrix3x4_t latchStartAnchorDelta;
						bool bBlendFromLatchStart = bHaveLatchStartWorld
							&& easedBlend < 1.0f;
						if (bBlendFromLatchStart)
						{
							ConcatTransforms(latchStartWorld, invGripHand, latchStartAnchorDelta);
						}

						matrix3x4_t blendedAnchorDelta;
						if (easedBlend >= 1.0f)
						{
							MatrixCopy(blendTargetAnchorDelta, blendedAnchorDelta);
						}
						else
						{
							matrix3x4_t startAnchorDelta;
							if (bBlendFromLatchStart)
							{
								MatrixCopy(latchStartAnchorDelta, startAnchorDelta);
							}
							else
							{
								// Use the current wrist only to derive a single rigid
								// start anchor. The target pose itself remains the
								// authored grip skeleton, avoiding per-bone drift.
								ConcatTransforms(pBoneToWorldOut[m_iHandBone], invGripHand, startAnchorDelta);
							}

							Vector startAnchorPos, gripAnchorPos, blendedAnchorPos;
							Quaternion startAnchorQuat, gripAnchorQuat, blendedAnchorQuat;
							MatrixAngles(startAnchorDelta, startAnchorQuat, startAnchorPos);
							MatrixAngles(blendTargetAnchorDelta, gripAnchorQuat, gripAnchorPos);
							VectorLerp(startAnchorPos, gripAnchorPos, easedBlend, blendedAnchorPos);
							SafeQuaternionSlerp(startAnchorQuat, gripAnchorQuat, easedBlend, blendedAnchorQuat);
							QuaternionMatrix(blendedAnchorQuat, blendedAnchorPos, blendedAnchorDelta);
						}

						for (int i = 0; i < numBones && i < nMaxBones; i++)
						{
							const mstudiobone_t *pBone = pStudioHdr->pBone(i);
							if (!pBone)
								continue;

							ConcatTransforms(blendedAnchorDelta, gripModelBones[i], pBoneToWorldOut[i]);
						}

						bAppliedGripPose = true;
					}

					if (!bAppliedGripPose)
					{
						if (bPomsonLatchLocked)
						{
							blendedPos = blendTargetPos;
							blendedQuat = blendTargetQuat;
						}
						else
						{
							VectorLerp(livePos, blendTargetPos, easedBlend, blendedPos);
							QuaternionSlerp(liveQuat, blendTargetQuat, easedBlend, blendedQuat);
						}

						Vector posOffset = blendedPos - livePos;

						Quaternion invLiveQuat;
						QuaternionInvert(liveQuat, invLiveQuat);
						Quaternion rotDelta;
						QuaternionMult(blendedQuat, invLiveQuat, rotDelta);

						matrix3x4_t rotDeltaMatrix;
						QuaternionMatrix(rotDelta, vec3_origin, rotDeltaMatrix);

						for (int i = 0; i < numBones && i < nMaxBones; i++)
						{
							Vector bonePos;
							QAngle boneAngles;
							MatrixAngles(pBoneToWorldOut[i], boneAngles, bonePos);

							bonePos += posOffset;

							Vector relPos = bonePos - blendedPos;
							Vector rotatedRelPos;
							VectorRotate(relPos, rotDeltaMatrix, rotatedRelPos);
							bonePos = blendedPos + rotatedRelPos;

							Quaternion boneQuat;
							AngleQuaternion(boneAngles, boneQuat);
							Quaternion rotatedQuat;
							QuaternionMult(rotDelta, boneQuat, rotatedQuat);

							QAngle rotatedAngles;
							QuaternionAngles(rotatedQuat, rotatedAngles);
							AngleMatrix(rotatedAngles, bonePos, pBoneToWorldOut[i]);
						}
					}

					if (m_iHandBone >= 0 && m_iHandBone < nMaxBones)
					{
						matrix3x4_t currentHandInv;
						MatrixInvert(pBoneToWorldOut[m_iHandBone], currentHandInv);

						matrix3x4_t handCorrection;
						ConcatTransforms(desiredHandWorld, currentHandInv, handCorrection);

						for (int i = 0; i < numBones && i < nMaxBones; i++)
						{
							matrix3x4_t correctedBone;
							ConcatTransforms(handCorrection, pBoneToWorldOut[i], correctedBone);
							MatrixCopy(correctedBone, pBoneToWorldOut[i]);
						}

						// Last-resort positional invariant. If any transform math
						// above leaves tiny or non-tiny residual drift, translate
						// the whole solved skeleton so the debug wrist center is
						// exactly the intended eased hand position.
						Vector correctedHandPos;
						MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, correctedHandPos);
						Vector residual = desiredHandPos - correctedHandPos;
						if (residual.LengthSqr() > 0.000001f)
						{
							for (int i = 0; i < numBones && i < nMaxBones; i++)
							{
								Vector bonePos;
								MatrixGetColumn(pBoneToWorldOut[i], 3, bonePos);
								bonePos += residual;
								MatrixSetColumn(bonePos, 3, pBoneToWorldOut[i]);
							}
						}
					}

					if (m_bPomsonRightGripLatched)
					{
						MatrixCopy(pBoneToWorldOut[m_iHandBone], m_matPomsonRightGripLastWorld);
						m_bPomsonRightGripLastWorldValid = true;
					}

					if (tfvr_twohand_debug.GetBool() || tfvr_pomson_grip_debug.GetBool())
					{
						Vector solvedHandPos;
						MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, solvedHandPos);
						Vector hardSnapHandPos = gripTargetPos;
						if (bDebugHaveHardSnapHand)
							MatrixGetColumn(debugHardSnapHand, 3, hardSnapHandPos);
						Vector latchStartPos = vec3_origin;
						if (bHaveLatchStartWorld)
							MatrixGetColumn(latchStartWorld, 3, latchStartPos);
						Vector controllerPos = m_vecLastValidPosition;
						Vector fingerPos = controllerPos;
						if (m_pHandTracker)
						{
							QAngle fingerAngles;
							m_pHandTracker->GetHandJoint(false, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, fingerPos, fingerAngles);
						}

						debugoverlay->AddBoxOverlay(gripTargetPos, Vector(-1.5f, -1.5f, -1.5f),
							Vector(1.5f, 1.5f, 1.5f), gripTargetAngles, 0, 255, 0, 180, 0.1f);
						debugoverlay->AddBoxOverlay(solvedHandPos, Vector(-1.25f, -1.25f, -1.25f),
							Vector(1.25f, 1.25f, 1.25f), vec3_angle, 255, 0, 0, 180, 0.1f);
						if (bDebugHaveHardSnapHand)
						{
							debugoverlay->AddBoxOverlay(hardSnapHandPos, Vector(-1.0f, -1.0f, -1.0f),
								Vector(1.0f, 1.0f, 1.0f), vec3_angle, 255, 255, 255, 180, 0.1f);
							debugoverlay->AddLineOverlay(hardSnapHandPos, solvedHandPos, 255, 0, 255, true, 0.1f);
						}
						if (bHaveLatchStartWorld)
						{
							debugoverlay->AddBoxOverlay(latchStartPos, Vector(-1.0f, -1.0f, -1.0f),
								Vector(1.0f, 1.0f, 1.0f), vec3_angle, 255, 255, 0, 180, 0.1f);
						}
						debugoverlay->AddBoxOverlay(controllerPos, Vector(-0.75f, -0.75f, -0.75f),
							Vector(0.75f, 0.75f, 0.75f), vec3_angle, 255, 128, 0, 160, 0.1f);
						debugoverlay->AddBoxOverlay(fingerPos, Vector(-0.75f, -0.75f, -0.75f),
							Vector(0.75f, 0.75f, 0.75f), vec3_angle, 0, 128, 255, 160, 0.1f);
						debugoverlay->AddLineOverlay(gripTargetPos, solvedHandPos, 255, 255, 255, true, 0.1f);

						int middleBone = LookupBone("bip_middle_0_R");
						if (middleBone < 0)
							middleBone = LookupBone("bip_middle_0_r");
						if (middleBone >= 0 && middleBone < nMaxBones)
						{
							Vector middlePos;
							MatrixGetColumn(pBoneToWorldOut[middleBone], 3, middlePos);
							debugoverlay->AddBoxOverlay(middlePos, Vector(-1.0f, -1.0f, -1.0f),
								Vector(1.0f, 1.0f, 1.0f), vec3_angle, 0, 128, 255, 180, 0.1f);
						}

						static float s_flLastPomsonLatchDebug = 0.0f;
						if (tfvr_pomson_grip_debug.GetBool() && gpGlobals->curtime - s_flLastPomsonLatchDebug > 0.25f)
						{
							DevMsg("PomsonGrip EASE: grip=%s blend=%.3f eased=%.3f latched=%d offset=%d pumpTarget=%d seq=%d cycle=%.3f target=(%.2f %.2f %.2f) easedWrist=(%.2f %.2f %.2f) snapWrist=(%.2f %.2f %.2f) errEase=%.3f errSnap=%.3f ctrlDist=%.3f fingerDist=%.3f latchDist=%.3f\n",
								m_bPomsonUseReloadGrip ? "reload" : "idle",
								m_flTwoHandBlend, easedBlend,
								m_bPomsonRightGripLatched ? 1 : 0,
								m_bPomsonRightLatchOffsetValid ? 1 : 0,
								bUsePumpAnimTarget ? 1 : 0,
								seqToSample, cycleToSample,
								gripTargetPos.x, gripTargetPos.y, gripTargetPos.z,
								solvedHandPos.x, solvedHandPos.y, solvedHandPos.z,
								hardSnapHandPos.x, hardSnapHandPos.y, hardSnapHandPos.z,
								(solvedHandPos - gripTargetPos).Length(),
								(hardSnapHandPos - gripTargetPos).Length(),
								(controllerPos - gripTargetPos).Length(),
								(fingerPos - gripTargetPos).Length(),
								bHaveLatchStartWorld ? (latchStartPos - gripTargetPos).Length() : -1.0f);
							s_flLastPomsonLatchDebug = gpGlobals->curtime;
						}
					}
				}
			}
			// When bBackstabPose is true, the main AccumulatePose already
			// sampled the backstab animation, so the full skeleton
			// (fingers, weapon_bone, everything) already has backstab
			// transforms through the normal hierarchy build.

			// Cache weapon_bone LOCAL to hand bone so HUD elements stay stable
			// during backstab and draw animations.  Only capture from the
			// idle path -- when an animation is actively playing, reconstruct
			// the world transform from the cached local offset + current hand.
			bool bIsPomsonDetachWeapon = (m_hHeldWeapon.Get() && m_hHeldWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON);
			bool bNeedsStableWeaponBone = (m_iBackstabUpSequence >= 0) || m_bPlayingDrawAnim || bIsPomsonDetachWeapon;
			bool bAnimPlaying = bBackstabPose || m_bPlayingDrawAnim;
			// When the right hand is detached, ApplyWeaponPose was skipped so
			// bip_hand_L and weapon_bone are at base-animation positions, not
			// the weapon-posed positions.  Preserve the last good cached values.
			bool bPomsonDetachedSkipCache = bIsPomsonDetachWeapon && m_bRightHandDetached;
			if (!bAnimPlaying && bNeedsStableWeaponBone && !bPomsonDetachedSkipCache)
			{
				int wpnBone = LookupBone("weapon_bone");
				if (wpnBone >= 0 && wpnBone < nMaxBones && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
				{
					matrix3x4_t invHand;
					MatrixInvert(pBoneToWorldOut[m_iHandBone], invHand);
					ConcatTransforms(invHand, pBoneToWorldOut[wpnBone], m_matIdleWeaponBoneLocal);
					m_bHasIdleWeaponBone = true;

					if (m_iOffHandBone >= 0 && m_iOffHandBone < nMaxBones)
					{
						matrix3x4_t invOffHand;
						MatrixInvert(pBoneToWorldOut[m_iOffHandBone], invOffHand);
						ConcatTransforms(invOffHand, pBoneToWorldOut[wpnBone], m_matOffHandToWeaponBone);
						m_bOffHandToWeaponBoneValid = true;
					}
				}
			}

			// Reconstruct the idle weapon_bone world transform every frame
			// from the cached local offset so HUD elements and hitboxes
			// remain stable while animations play.
			// Skip during reload — ApplyWeaponPose already set
			// m_matIdleWeaponBoneWorld from the idle weapon_bone before the
			// reload override moved bip_hand.
			if (m_bHasIdleWeaponBone && !m_bPlayingReloadAnim
				&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
			{
				ConcatTransforms(pBoneToWorldOut[m_iHandBone], m_matIdleWeaponBoneLocal, m_matIdleWeaponBoneWorld);
			}

			// PositionWeaponFromBones forces the render weapon to SetupBones immediately.
			// Publish this frame's mirror before that call so the render weapon and
			// visible hand reflect across the same frame, instead of the weapon using
			// a stale mirror frame from a previous setup.
			m_bReflectPoseActive = ShouldMirrorPose();
			if (m_bReflectPoseActive)
			{
				Vector vecReflectOrigin;
				MatrixGetColumn(controllerTransform, 3, vecReflectOrigin);
				AngleMatrix(m_angLastValidAngles, vecReflectOrigin, m_matReflectFrame);
			}
			else
			{
				SetIdentityMatrix(m_matReflectFrame);
			}

			// Position weapon from current bones (position offset applied later)
			PositionWeaponFromBones(pBoneToWorldOut, nMaxBones);

			// Cache bip_hand_R world transform for the right hand's grip target.
			// At this point all bone modifications (fire anim rotation, lever
			// override, position correction) are applied, so this is exactly
			// where the grab hand should be.
			if (m_iOffHandBone >= 0 && m_iOffHandBone < nMaxBones)
			{
				MatrixCopy(pBoneToWorldOut[m_iOffHandBone], m_matMedigunGripTarget);
				m_bMedigunGripTargetValid = true;
			}
			else
			{
				m_bMedigunGripTargetValid = false;
			}
		}
		else if (IsRightHand() && m_flTwoHandBlend > 0.01f
			&& GetLocalPlayerLeftHand()
			&& IsWeaponMedigun(GetLocalPlayerLeftHand()->GetHeldWeapon()))
		{
			// Right hand passive grip for left hand's medigun
			// Use GetOffHandGripTarget on the left hand to get the grip position
			C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
			bool bUsedGripPose = false;

			if (pLeftHand && IsWeaponMedigun(pLeftHand->GetHeldWeapon()))
			{
				Vector gripTargetPos;
				QAngle gripTargetAngles;

				// Use the cached grip target from the left hand's SetupBones when
				// available — it includes fire animation rotation and lever overrides.
				// Fall back to GetOffHandGripTarget if not yet cached this frame.
				bool bGotGrip = false;
				if (pLeftHand->m_bMedigunGripTargetValid)
				{
					MatrixAngles(pLeftHand->m_matMedigunGripTarget, gripTargetAngles, gripTargetPos);
					bGotGrip = true;
				}
				else
				{
					bGotGrip = pLeftHand->GetOffHandGripTarget(gripTargetPos, gripTargetAngles, true);
				}

				if (bGotGrip)
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
		else if (IsLeftHand() && m_flTwoHandBlend > 0.01f
			&& !(GetLocalPlayerRightHand()
				&& GetLocalPlayerRightHand()->GetHeldWeapon()
				&& GetLocalPlayerRightHand()->GetHeldWeapon()->GetWeaponID() == TF_WEAPON_COMPOUND_BOW))
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
					// Pomson right-hand detach: preserve the left visual hand
					// offset captured at detach, matching the weapon's captured
					// left-controller offset.  Fall back to the current hand
					// position for older/no-capture paths.
					if (pRightHand->IsRightHandDetached()
						&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
					{
						matrix3x4_t detachedLeftHandWorld;
						if (pRightHand->GetPomsonDetachedLeftHandWorld(detachedLeftHandWorld))
							MatrixAngles(detachedLeftHandWorld, gripTargetAngles, gripTargetPos);
						else
							MatrixAngles(pBoneToWorldOut[m_iHandBone], gripTargetAngles, gripTargetPos);
					}

					bUsedGripPose = true;

					// Sample the right hand's CURRENT animation for the left hand finger pose
					// This ensures the left hand always matches the right hand's animation state
					int rightSeq = pRightHand->GetSequence();
					float rightCycle = pRightHand->GetCycle();
					float flGripPoseBlend = m_flTwoHandBlend;

					// Stickybomb/Bison pump: the left hand IS the pump hand, so
					// follow the pump animation for finger pose.  Scattergun
					// pump is a right-hand action — left hand stays at idle.
					int pumpWeaponID = pRightHand->GetHeldWeapon()
						? pRightHand->GetHeldWeapon()->GetWeaponID() : -1;
					bool bPomsonDetached = (pumpWeaponID == TF_WEAPON_DRG_POMSON
						&& pRightHand->IsRightHandDetached());
					bool bIsPumpWeapon = (pumpWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER
						|| pumpWeaponID == TF_WEAPON_RAYGUN
						|| pumpWeaponID == TF_WEAPON_PARTICLE_CANNON
						|| IsPumpActionShotgunWeaponID(pumpWeaponID));

					if (bPomsonDetached && pRightHand->m_iIdleSequence >= 0)
					{
						// The left hand remains the support grip while the right
						// hand pumps, so keep its pose tied to the idle grip.
						const char *pszSeqName = pRightHand->GetSequenceName(pRightHand->m_iIdleSequence);
						if (pszSeqName)
						{
							int leftSeq = LookupSequence(pszSeqName);
							if (leftSeq >= 0)
							{
								rightSeq = leftSeq;
								rightCycle = 0.0f;
							}
						}
					}
					else if (bIsPumpWeapon && pRightHand->m_bPlayingReloadAnim
						&& pRightHand->m_iLeverReloadSequence >= 0)
					{
						if (!IsPumpActionShotgunWeaponID(pumpWeaponID))
						{
							const char *pszSeqName = pRightHand->GetSequenceName(pRightHand->m_iLeverReloadSequence);
							if (pszSeqName)
							{
								int leftSeq = LookupSequence(pszSeqName);
								if (leftSeq >= 0)
								{
									rightSeq = leftSeq;
									rightCycle = pRightHand->m_flLeverReloadCycle;
								}
							}
						}
					}
					else if (bIsPumpWeapon && pRightHand->m_iReloadLoopSequence >= 0
						&& (pumpWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER
							|| (pumpWeaponID == TF_WEAPON_RAYGUN && pRightHand->IsBisonOnReloadGrip())
							|| (pumpWeaponID == TF_WEAPON_PARTICLE_CANNON && pRightHand->IsManglerOnReloadGrip())))
					{
						// Stickybomb / Bison / Mangler reload grip idle: use reload loop at frame 0
						const char *pszSeqName = pRightHand->GetSequenceName(pRightHand->m_iReloadLoopSequence);
						if (pszSeqName)
						{
							int leftSeq = LookupSequence(pszSeqName);
							if (leftSeq >= 0)
							{
								rightSeq = leftSeq;
								rightCycle = 0.0f;
							}
						}
					}

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

					// Use the determined sequence - keeps both hands in sync
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

							VectorLerp(currentPos, gripPos, flGripPoseBlend, blendedPos);
							SafeQuaternionSlerp(currentQuat, gripQuat, flGripPoseBlend, blendedQuat);

							QuaternionMatrix(blendedQuat, blendedPos, pBoneToWorldOut[i]);
						}

						if (tfvr_twohand_debug.GetBool())
						{
							static float lastDebugTime = 0;
							if (gpGlobals->curtime - lastDebugTime > 0.5f)
							{
								DevMsg("TwoHand: Using left hand pose seq=%d cycle=%.2f, blend=%.2f\n",
									rightSeq, rightCycle, flGripPoseBlend);
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
		else if (IsRightHand())
		{
			C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
			C_TFWeaponBase *pLeftWeapon = pLeftHand ? pLeftHand->GetHeldWeapon() : NULL;
			CTFShotgun *pShotgun = (pLeftWeapon && IsPumpActionShotgunWeaponID(pLeftWeapon->GetWeaponID()))
				? static_cast<CTFShotgun *>(pLeftWeapon) : NULL;
			CTFRocketLauncher *pRocketLauncher = (pLeftWeapon
				&& (pLeftWeapon->GetWeaponID() == TF_WEAPON_ROCKETLAUNCHER || pLeftWeapon->GetWeaponID() == TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT))
				? static_cast<CTFRocketLauncher *>(pLeftWeapon) : NULL;
			CTFCompoundBow *pBow = (pLeftWeapon && pLeftWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
				? static_cast<CTFCompoundBow *>(pLeftWeapon) : NULL;
			CTFPistol *pManualPistol = TFVR_GetManualReloadPistol(pLeftWeapon);
			const bool bManualReloadActive = (pShotgun && pShotgun->IsVRShotgunManualReloadActive())
				|| (pRocketLauncher && pRocketLauncher->IsVRRocketManualReloadActive())
				|| (pBow && pBow->IsVRBowArrowPoseActive())
				|| (pManualPistol && pManualPistol->IsVRPistolMagPoseActive());
			if (pLeftHand && bManualReloadActive
				&& pLeftHand->m_iShotgunManualReloadSequence >= 0 && m_iHandBone >= 0)
			{
				int iManualReloadSeq = pLeftHand->m_iShotgunManualReloadSequence;
				if (pBow)
				{
					if (pBow->HasVRBowArrowInHand() && !pBow->IsVRBowArrowNocking() && !pBow->IsVRBowArrowNocked()
						&& pLeftHand->m_iChargeSequence >= 0)
					{
						iManualReloadSeq = pLeftHand->m_iChargeSequence;
					}
					else if (pBow->IsVRBowArrowNocked() && pLeftHand->IsPlayingChargeAnim() && pLeftHand->GetSequence() >= 0)
					{
						iManualReloadSeq = pLeftHand->GetSequence();
					}
				}

				const char *pszSeqName = pLeftHand->GetSequenceName(iManualReloadSeq);
				int iShellSeq = pszSeqName ? LookupSequence(pszSeqName) : -1;
				if (iShellSeq >= 0)
				{
					float flProgress = 0.0f;
					if (pShotgun)
					{
						flProgress = pShotgun->IsVRShotgunShellInserting()
							? pShotgun->GetVRShotgunShellInsertProgress() : 0.0f;
					}
					else if (pRocketLauncher)
					{
						flProgress = pRocketLauncher->IsVRRocketInserting()
							? pRocketLauncher->GetVRRocketVisualInsertProgress() : 0.0f;
					}
					else if (pBow)
					{
						flProgress = TFVR_GetBowNockVisualProgress(pBow);
					}
					else if (pManualPistol)
					{
						flProgress = pManualPistol->IsVRMagInserting()
							? pManualPistol->GetVRMagPhaseProgress() : 0.0f;
					}
					float flCycle = 0.0f;
					if (pBow && pBow->HasVRBowArrowInHand() && !pBow->IsVRBowArrowNocking() && !pBow->IsVRBowArrowNocked())
					{
						flCycle = 0.0f;
					}
					else
					{
						flCycle = (pBow && pBow->IsVRBowArrowNocked() && pLeftHand->IsPlayingChargeAnim())
							? pLeftHand->GetCycle()
							: Lerp(flProgress,
								pLeftHand->m_flShotgunManualReloadHoldCycle,
								pLeftHand->m_flShotgunManualReloadCommitCycle);
					}

					float poseParams[MAXSTUDIOPOSEPARAM];
					memset(poseParams, 0, sizeof(poseParams));
					IBoneSetup shellSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParams);

					Vector shellPos[MAXSTUDIOBONES];
					Quaternion shellQ[MAXSTUDIOBONES];
					for (int i = 0; i < MAXSTUDIOBONES; i++)
					{
						shellPos[i].Init();
						shellQ[i].Init(0, 0, 0, 1);
					}

					shellSetup.InitPose(shellPos, shellQ);
					shellSetup.AccumulatePose(shellPos, shellQ, iShellSeq, flCycle, 1.0f, gpGlobals->curtime, NULL);

					matrix3x4_t shellBones[MAXSTUDIOBONES];
					for (int i = 0; i < numBones; i++)
					{
						matrix3x4_t local;
						QuaternionMatrix(shellQ[i], shellPos[i], local);
						const mstudiobone_t *pBone = pStudioHdr->pBone(i);
						if (!pBone)
						{
							SetIdentityMatrix(shellBones[i]);
							continue;
						}

						if (pBone->parent == -1)
							MatrixCopy(local, shellBones[i]);
						else if (pBone->parent >= 0 && pBone->parent < numBones)
							ConcatTransforms(shellBones[pBone->parent], local, shellBones[i]);
						else
							SetIdentityMatrix(shellBones[i]);
					}

					int iShellAnchorBone = m_iHandBone;
					matrix3x4_t invShellHand;
					MatrixInvert(shellBones[iShellAnchorBone], invShellHand);
					matrix3x4_t shellAnchorWorld;
					const bool bInserting = (pShotgun && pShotgun->IsVRShotgunShellInserting())
						|| (pRocketLauncher && pRocketLauncher->IsVRRocketInserting())
						|| (pBow && (pBow->IsVRBowArrowNocking() || pBow->IsVRBowArrowNocked()))
						|| (pManualPistol && pManualPistol->IsVRMagInserting());
					if (bInserting)
					{
						Vector targetPos;
						QAngle targetAngles;
						bool bGotTarget = false;
						matrix3x4_t bowWorld;
						bool bHaveBowMatrix = false;
						if (pShotgun)
							bGotTarget = pLeftHand->GetShotgunManualReloadTarget(targetPos, targetAngles);
						else if (pBow)
						{
							bGotTarget = pLeftHand->GetBowManualReloadTarget(targetPos, targetAngles, &bowWorld);
							bHaveBowMatrix = bGotTarget;
						}
						else if (pManualPistol)
							bGotTarget = pLeftHand->GetPistolManualReloadTarget(targetPos, targetAngles);
						else
							bGotTarget = pLeftHand->GetRocketManualReloadTarget(targetPos, targetAngles);
						if (bGotTarget)
						{
							// Bow: use the full world MATRIX (preserves the mirror
							// reflection that pos/angles cannot represent). Other
							// weapons build from pos/angles as before.
							matrix3x4_t targetWorld;
							if (bHaveBowMatrix)
								MatrixCopy(bowWorld, targetWorld);
							else
								AngleMatrix(targetAngles, targetPos, targetWorld);

							// In mirrored mode this support hand reflects its whole
							// skeleton across its own controller frame at the end of
							// SetupBones. Reflect the (already weapon-frame-reflected)
							// bow grip matrix through the support frame so the net is a
							// SINGLE mirror: it lands on the visible grip with correct
							// winding/normals instead of a double mirror.
							if (pBow && ShouldMirrorPose())
							{
								Vector vecReflectOrigin;
								MatrixGetColumn(controllerTransform, 3, vecReflectOrigin);
								matrix3x4_t reflectFrame;
								AngleMatrix(m_angLastValidAngles, vecReflectOrigin, reflectFrame);
								TFVR_ReflectBonesInControllerFrame(&targetWorld, 1, reflectFrame);
							}

							float flBlendFraction = MAX(tfvr_shotgun_manual_reload_pose_blend_fraction.GetFloat(), 0.0f);
							if (flBlendFraction > 0.0f && flProgress < flBlendFraction)
							{
								float flBlend = SimpleSpline(clamp(flProgress / flBlendFraction, 0.0f, 1.0f));
								TFVR_BlendTransforms(controllerTransform, targetWorld, flBlend, shellAnchorWorld);
							}
							else
							{
								MatrixCopy(targetWorld, shellAnchorWorld);
							}
						}
						else
							MatrixCopy(controllerTransform, shellAnchorWorld);
					}
					else
					{
						MatrixCopy(controllerTransform, shellAnchorWorld);
					}

					matrix3x4_t shellAnchorDelta;
					ConcatTransforms(shellAnchorWorld, invShellHand, shellAnchorDelta);

					for (int i = 0; i < numBones && i < nMaxBones; i++)
					{
						ConcatTransforms(shellAnchorDelta, shellBones[i], pBoneToWorldOut[i]);
					}

					if (pBow && tfvr_huntsman_debug.GetBool() && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
					{
						Vector solvedHandPos;
						QAngle solvedHandAngles;
						MatrixAngles(pBoneToWorldOut[m_iHandBone], solvedHandAngles, solvedHandPos);

						debugoverlay->AddBoxOverlay(solvedHandPos,
							Vector(-1.25f, -1.25f, -1.25f), Vector(1.25f, 1.25f, 1.25f),
							solvedHandAngles, 0, 128, 255, 220, 0.1f);
						debugoverlay->AddTextOverlay(solvedHandPos, 0.1f, "support visual wrist");

						if (pBow->IsVRBowArrowNocked() && iShellAnchorBone >= 0 && iShellAnchorBone < nMaxBones)
						{
							Vector anchorPos;
							QAngle anchorAngles;
							MatrixAngles(pBoneToWorldOut[iShellAnchorBone], anchorAngles, anchorPos);
							debugoverlay->AddBoxOverlay(anchorPos,
								Vector(-1.0f, -1.0f, -1.0f), Vector(1.0f, 1.0f, 1.0f),
								anchorAngles, 255, 0, 255, 220, 0.1f);
							debugoverlay->AddTextOverlay(anchorPos, 0.1f, "support anchor");
						}
					}

					extern ConVar tfvr_shotgun_pump_debug;
					if (pRocketLauncher && tfvr_shotgun_pump_debug.GetBool())
					{
						static float s_flLastRocketPoseDebugTime = 0.0f;
						if (gpGlobals->curtime - s_flLastRocketPoseDebugTime > 0.25f)
						{
							int iRocketBone = LookupBone("rocket");
							Vector rocketPos(0.0f, 0.0f, 0.0f);
							if (iRocketBone >= 0 && iRocketBone < numBones && iRocketBone < nMaxBones)
								MatrixGetColumn(pBoneToWorldOut[iRocketBone], 3, rocketPos);

							DevMsg("VR Rocket Reload: applied offhand pose model='%s' seq='%s' seqIndex=%d cycle=%.3f progress=%.3f inserting=%d rocketBone=%d rocketPos=(%.1f %.1f %.1f)\n",
								GetModelName(), pszSeqName ? pszSeqName : "<null>", iShellSeq, flCycle, flProgress,
								pRocketLauncher->IsVRRocketInserting() ? 1 : 0, iRocketBone,
								rocketPos.x, rocketPos.y, rocketPos.z);
							s_flLastRocketPoseDebugTime = gpGlobals->curtime;
						}
					}

					bShotgunManualReloadPoseApplied = true;
					m_bShotgunManualReloadPoseActive = true;
					m_bShotgunManualReloadBlendOutActive = false;
					m_nShotgunManualReloadBlendOutBones = MIN(numBones, nMaxBones);
					matrix3x4_t invControllerTransform;
					MatrixInvert(controllerTransform, invControllerTransform);
					for (int i = 0; i < m_nShotgunManualReloadBlendOutBones; i++)
						ConcatTransforms(invControllerTransform, pBoneToWorldOut[i], m_matShotgunManualReloadBlendOutStart[i]);
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
		}
		else if (IsLeftHand())
		{
			C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
			C_TFWeaponBase *pRightWeapon = pRightHand ? pRightHand->GetHeldWeapon() : NULL;
			CTFShotgun *pShotgun = (pRightWeapon && IsPumpActionShotgunWeaponID(pRightWeapon->GetWeaponID()))
				? static_cast<CTFShotgun *>(pRightWeapon) : NULL;
			CTFRocketLauncher *pRocketLauncher = (pRightWeapon
				&& (pRightWeapon->GetWeaponID() == TF_WEAPON_ROCKETLAUNCHER || pRightWeapon->GetWeaponID() == TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT))
				? static_cast<CTFRocketLauncher *>(pRightWeapon) : NULL;
			CTFCompoundBow *pBow = (pRightWeapon && pRightWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
				? static_cast<CTFCompoundBow *>(pRightWeapon) : NULL;
			CTFPistol *pManualPistol = TFVR_GetManualReloadPistol(pRightWeapon);
			const bool bManualReloadActive = (pShotgun && pShotgun->IsVRShotgunManualReloadActive())
				|| (pRocketLauncher && pRocketLauncher->IsVRRocketManualReloadActive())
				|| (pBow && pBow->IsVRBowArrowPoseActive())
				|| (pManualPistol && pManualPistol->IsVRPistolMagPoseActive());
			if (pRightHand && bManualReloadActive
				&& pRightHand->m_iShotgunManualReloadSequence >= 0 && m_iHandBone >= 0)
			{
				int iManualReloadSeq = pRightHand->m_iShotgunManualReloadSequence;
				if (pBow)
				{
					if (pBow->HasVRBowArrowInHand() && !pBow->IsVRBowArrowNocking() && !pBow->IsVRBowArrowNocked()
						&& pRightHand->m_iChargeSequence >= 0)
					{
						iManualReloadSeq = pRightHand->m_iChargeSequence;
					}
					else if (pBow->IsVRBowArrowNocked() && pRightHand->IsPlayingChargeAnim() && pRightHand->GetSequence() >= 0)
					{
						iManualReloadSeq = pRightHand->GetSequence();
					}
				}

				const char *pszSeqName = pRightHand->GetSequenceName(iManualReloadSeq);
				int iShellSeq = pszSeqName ? LookupSequence(pszSeqName) : -1;
				if (iShellSeq >= 0)
				{
					float flProgress = 0.0f;
					if (pShotgun)
					{
						flProgress = pShotgun->IsVRShotgunShellInserting()
							? pShotgun->GetVRShotgunShellInsertProgress() : 0.0f;
					}
					else if (pRocketLauncher)
					{
						flProgress = pRocketLauncher->IsVRRocketInserting()
							? pRocketLauncher->GetVRRocketVisualInsertProgress() : 0.0f;
					}
					else if (pBow)
					{
						flProgress = TFVR_GetBowNockVisualProgress(pBow);
					}
					else if (pManualPistol)
					{
						flProgress = pManualPistol->IsVRMagInserting()
							? pManualPistol->GetVRMagPhaseProgress() : 0.0f;
					}
					float flCycle = 0.0f;
					if (pBow && pBow->HasVRBowArrowInHand() && !pBow->IsVRBowArrowNocking() && !pBow->IsVRBowArrowNocked())
					{
						flCycle = 0.0f;
					}
					else
					{
						flCycle = (pBow && pBow->IsVRBowArrowNocked() && pRightHand->IsPlayingChargeAnim())
							? pRightHand->GetCycle()
							: Lerp(flProgress,
								pRightHand->m_flShotgunManualReloadHoldCycle,
								pRightHand->m_flShotgunManualReloadCommitCycle);
					}

					float poseParams[MAXSTUDIOPOSEPARAM];
					memset(poseParams, 0, sizeof(poseParams));
					IBoneSetup shellSetup(pStudioHdr, BONE_USED_BY_ANYTHING, poseParams);

					Vector shellPos[MAXSTUDIOBONES];
					Quaternion shellQ[MAXSTUDIOBONES];
					for (int i = 0; i < MAXSTUDIOBONES; i++)
					{
						shellPos[i].Init();
						shellQ[i].Init(0, 0, 0, 1);
					}

					shellSetup.InitPose(shellPos, shellQ);
					shellSetup.AccumulatePose(shellPos, shellQ, iShellSeq, flCycle, 1.0f, gpGlobals->curtime, NULL);

					matrix3x4_t shellBones[MAXSTUDIOBONES];
					for (int i = 0; i < numBones; i++)
					{
						matrix3x4_t local;
						QuaternionMatrix(shellQ[i], shellPos[i], local);
						const mstudiobone_t *pBone = pStudioHdr->pBone(i);
						if (!pBone)
						{
							SetIdentityMatrix(shellBones[i]);
							continue;
						}

						if (pBone->parent == -1)
							MatrixCopy(local, shellBones[i]);
						else if (pBone->parent >= 0 && pBone->parent < numBones)
							ConcatTransforms(shellBones[pBone->parent], local, shellBones[i]);
						else
							SetIdentityMatrix(shellBones[i]);
					}

					int iShellAnchorBone = m_iHandBone;
					matrix3x4_t invShellHand;
					MatrixInvert(shellBones[iShellAnchorBone], invShellHand);
					matrix3x4_t shellAnchorDelta;
					matrix3x4_t shellAnchorWorld;
					const bool bInserting = (pShotgun && pShotgun->IsVRShotgunShellInserting())
						|| (pRocketLauncher && pRocketLauncher->IsVRRocketInserting())
						|| (pBow && (pBow->IsVRBowArrowNocking() || pBow->IsVRBowArrowNocked()))
						|| (pManualPistol && pManualPistol->IsVRMagInserting());
					if (bInserting)
					{
						Vector targetPos;
						QAngle targetAngles;
						bool bGotTarget = false;
						matrix3x4_t bowWorld;
						bool bHaveBowMatrix = false;
						if (pShotgun)
							bGotTarget = pRightHand->GetShotgunManualReloadTarget(targetPos, targetAngles);
						else if (pBow)
						{
							bGotTarget = pRightHand->GetBowManualReloadTarget(targetPos, targetAngles, &bowWorld);
							bHaveBowMatrix = bGotTarget;
						}
						else if (pManualPistol)
							bGotTarget = pRightHand->GetPistolManualReloadTarget(targetPos, targetAngles);
						else
							bGotTarget = pRightHand->GetRocketManualReloadTarget(targetPos, targetAngles);
						if (bGotTarget)
						{
							// Bow: use the full world MATRIX (preserves the mirror
							// reflection that pos/angles cannot represent). Other
							// weapons build from pos/angles as before.
							matrix3x4_t targetWorld;
							if (bHaveBowMatrix)
								MatrixCopy(bowWorld, targetWorld);
							else
								AngleMatrix(targetAngles, targetPos, targetWorld);

							// In mirrored mode this support hand reflects its whole
							// skeleton across its own controller frame at the end of
							// SetupBones. Reflect the (already weapon-frame-reflected)
							// bow grip matrix through the support frame so the net is a
							// SINGLE mirror: it lands on the visible grip with correct
							// winding/normals instead of a double mirror.
							if (pBow && ShouldMirrorPose())
							{
								Vector vecReflectOrigin;
								MatrixGetColumn(controllerTransform, 3, vecReflectOrigin);
								matrix3x4_t reflectFrame;
								AngleMatrix(m_angLastValidAngles, vecReflectOrigin, reflectFrame);
								TFVR_ReflectBonesInControllerFrame(&targetWorld, 1, reflectFrame);
							}

							float flBlendFraction = MAX(tfvr_shotgun_manual_reload_pose_blend_fraction.GetFloat(), 0.0f);
							if (flBlendFraction > 0.0f && flProgress < flBlendFraction)
							{
								float flBlend = SimpleSpline(clamp(flProgress / flBlendFraction, 0.0f, 1.0f));
								TFVR_BlendTransforms(controllerTransform, targetWorld, flBlend, shellAnchorWorld);
							}
							else
							{
								MatrixCopy(targetWorld, shellAnchorWorld);
							}
						}
						else
							MatrixCopy(controllerTransform, shellAnchorWorld);
					}
					else
					{
						MatrixCopy(controllerTransform, shellAnchorWorld);
					}
					ConcatTransforms(shellAnchorWorld, invShellHand, shellAnchorDelta);

					for (int i = 0; i < numBones && i < nMaxBones; i++)
					{
						ConcatTransforms(shellAnchorDelta, shellBones[i], pBoneToWorldOut[i]);
					}

					if (pBow && tfvr_huntsman_debug.GetBool() && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
					{
						Vector solvedHandPos;
						QAngle solvedHandAngles;
						MatrixAngles(pBoneToWorldOut[m_iHandBone], solvedHandAngles, solvedHandPos);

						debugoverlay->AddBoxOverlay(solvedHandPos,
							Vector(-1.25f, -1.25f, -1.25f), Vector(1.25f, 1.25f, 1.25f),
							solvedHandAngles, 0, 128, 255, 220, 0.1f);
						debugoverlay->AddTextOverlay(solvedHandPos, 0.1f, "support visual wrist");

						if (pBow->IsVRBowArrowNocked() && iShellAnchorBone >= 0 && iShellAnchorBone < nMaxBones)
						{
							Vector anchorPos;
							QAngle anchorAngles;
							MatrixAngles(pBoneToWorldOut[iShellAnchorBone], anchorAngles, anchorPos);
							debugoverlay->AddBoxOverlay(anchorPos,
								Vector(-1.0f, -1.0f, -1.0f), Vector(1.0f, 1.0f, 1.0f),
								anchorAngles, 255, 0, 255, 220, 0.1f);
							debugoverlay->AddTextOverlay(anchorPos, 0.1f, "support anchor");
						}
					}

					extern ConVar tfvr_shotgun_pump_debug;
					if (pRocketLauncher && tfvr_shotgun_pump_debug.GetBool())
					{
						static float s_flLastRocketPoseDebugTime = 0.0f;
						if (gpGlobals->curtime - s_flLastRocketPoseDebugTime > 0.25f)
						{
							int iRocketBone = LookupBone("rocket");
							Vector rocketPos(0.0f, 0.0f, 0.0f);
							if (iRocketBone >= 0 && iRocketBone < numBones && iRocketBone < nMaxBones)
								MatrixGetColumn(pBoneToWorldOut[iRocketBone], 3, rocketPos);

							DevMsg("VR Rocket Reload: applied offhand pose model='%s' seq='%s' seqIndex=%d cycle=%.3f progress=%.3f inserting=%d rocketBone=%d rocketPos=(%.1f %.1f %.1f)\n",
								GetModelName(), pszSeqName ? pszSeqName : "<null>", iShellSeq, flCycle, flProgress,
								pRocketLauncher->IsVRRocketInserting() ? 1 : 0, iRocketBone,
								rocketPos.x, rocketPos.y, rocketPos.z);
							s_flLastRocketPoseDebugTime = gpGlobals->curtime;
						}
					}

					bShotgunManualReloadPoseApplied = true;
					m_bShotgunManualReloadPoseActive = true;
					m_bShotgunManualReloadBlendOutActive = false;
					m_nShotgunManualReloadBlendOutBones = MIN(numBones, nMaxBones);
					matrix3x4_t invControllerTransform;
					MatrixInvert(controllerTransform, invControllerTransform);
					for (int i = 0; i < m_nShotgunManualReloadBlendOutBones; i++)
						ConcatTransforms(invControllerTransform, pBoneToWorldOut[i], m_matShotgunManualReloadBlendOutStart[i]);
				}
				else
				{
					ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
				}
			}
			else if (pRightWeapon && pRightWeapon->GetWeaponID() == TF_WEAPON_FISTS
				&& !IsBareFists(pRightWeapon))
			{
				// Gloved fists: use the weapon pose so fingers match the glove model
				ApplyWeaponPose(pBoneToWorldOut, nMaxBones, pRightWeapon);
			}
			else
			{
				// Bare fists or no fists: use finger tracking
				ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
			}
		}
		else
		{
			ApplyFingerTracking(pBoneToWorldOut, nMaxBones);
		}

		if (!bShotgunManualReloadPoseApplied)
		{
			if (m_bShotgunManualReloadPoseActive && m_nShotgunManualReloadBlendOutBones > 0)
			{
				m_bShotgunManualReloadPoseActive = false;
				m_bShotgunManualReloadBlendOutActive = true;
				m_flShotgunManualReloadBlendOutStartTime = gpGlobals->curtime;
			}

			if (m_bShotgunManualReloadBlendOutActive)
			{
				float flBlendOutTime = tfvr_shotgun_manual_reload_pose_blend_out_time.GetFloat();
				if (flBlendOutTime <= 0.0f)
				{
					m_bShotgunManualReloadBlendOutActive = false;
					m_nShotgunManualReloadBlendOutBones = 0;
				}
				else
				{
					float flBlend = (gpGlobals->curtime - m_flShotgunManualReloadBlendOutStartTime) / flBlendOutTime;
					if (flBlend >= 1.0f)
					{
						m_bShotgunManualReloadBlendOutActive = false;
						m_nShotgunManualReloadBlendOutBones = 0;
					}
					else
					{
						flBlend = SimpleSpline(clamp(flBlend, 0.0f, 1.0f));
						int nBlendBones = MIN(m_nShotgunManualReloadBlendOutBones, MIN(numBones, nMaxBones));
						for (int i = 0; i < nBlendBones; i++)
						{
							matrix3x4_t trackedBone;
							matrix3x4_t startBoneWorld;
							MatrixCopy(pBoneToWorldOut[i], trackedBone);
							ConcatTransforms(controllerTransform, m_matShotgunManualReloadBlendOutStart[i], startBoneWorld);
							TFVR_BlendTransforms(startBoneWorld, trackedBone, flBlend, pBoneToWorldOut[i]);
						}
					}
				}
			}
		}

		// Position offset was already applied to controllerTransform at the beginning,
		// so all bones and weapon are naturally at the offset position.

		bool bShowManualReloadRocket = false;
		C_TFVRHand *pOtherHandForRocket = GetOppositeVRHand(this);
		C_TFWeaponBase *pOtherWeaponForRocket = pOtherHandForRocket ? pOtherHandForRocket->GetHeldWeapon() : NULL;
		bShowManualReloadRocket = bShotgunManualReloadPoseApplied && TFVR_HasManualReloadRocketVisual(pOtherWeaponForRocket);
		if (bShowManualReloadRocket && pOtherWeaponForRocket && TFVR_IsManualRocketLauncherWeaponID(pOtherWeaponForRocket->GetWeaponID()))
		{
			CTFRocketLauncher *pRocketLauncher = static_cast<CTFRocketLauncher *>(pOtherWeaponForRocket);
			if (pRocketLauncher->IsVRRocketInserting())
			{
				// Rocket insert samples frames 4-14; hide the loose rocket after frame 8.
				const float flHideAfterFrameProgress = (8.0f - 4.0f) / (14.0f - 4.0f);
				bShowManualReloadRocket = pRocketLauncher->GetVRRocketVisualInsertProgress() <= flHideAfterFrameProgress;
			}
		}
		UpdateManualReloadRocketFromBones(pBoneToWorldOut, nMaxBones, bShowManualReloadRocket);

		// Pistol manual reload magazines: gun mag on the weapon hand,
		// held spare mag on the off hand.
		UpdatePistolMagazineFromBones(pBoneToWorldOut, nMaxBones, bShotgunManualReloadPoseApplied);

		// Position shield from the precomputed c_demo_arms offset, applied to
		// bip_hand_L which is already at the controller transform after anchoring.
		// Done here in SetupBones (like the ball) for direct bone access.
		if (IsLeftHand() && m_hLeftHandShield.Get() && m_bShieldOffsetValid
			&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			matrix3x4_t shieldWorld;
			ConcatTransforms(pBoneToWorldOut[m_iHandBone], m_matShieldOffset, shieldWorld);
			Vector shieldPos;
			QAngle shieldAng;
			MatrixAngles(shieldWorld, shieldAng, shieldPos);
			m_hLeftHandShield->SetAbsOrigin(shieldPos);
			m_hLeftHandShield->SetAbsAngles(shieldAng);
		}

		if (IsLeftHand() && m_hLeftHandWatch.Get() && m_bWatchOffsetValid
			&& m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			matrix3x4_t watchWorld;
			ConcatTransforms(pBoneToWorldOut[m_iHandBone], m_matWatchOffset, watchWorld);
			Vector watchPos;
			QAngle watchAng;
			MatrixAngles(watchWorld, watchAng, watchPos);
			m_hLeftHandWatch->SetAbsOrigin(watchPos);
			m_hLeftHandWatch->SetAbsAngles(watchAng);
		}

		// Gunslinger uses the combined c_engineer_gunslinger model:
		// hide left-side bones so only the robot right arm renders
		if (m_bHasGunslinger && IsRightHand())
		{
			HideOppositeHand(pBoneToWorldOut, nMaxBones, pStudioHdr);
		}

		CTFCompoundBow *pBowForArrow = NULL;
		bool bHideBowArrowOnThisHand = false;
		const bool bThisHandHoldsBow = m_hHeldWeapon.Get()
			&& m_hHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW;
		if (bThisHandHoldsBow)
		{
			pBowForArrow = static_cast<CTFCompoundBow *>(m_hHeldWeapon.Get());
		}
		else
		{
			C_TFVRHand *pOtherHand = IsLeftHand() ? GetLocalPlayerRightHand() : GetLocalPlayerLeftHand();
			C_TFWeaponBase *pOtherWeapon = pOtherHand ? pOtherHand->GetHeldWeapon() : NULL;
			if (pOtherWeapon && pOtherWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
			{
				pBowForArrow = static_cast<CTFCompoundBow *>(pOtherWeapon);
			}
		}

		if (pBowForArrow)
		{
			const bool bShowArrowOnWeaponHand = bThisHandHoldsBow
				&& (pBowForArrow->IsVRBowArrowNocking()
					|| pBowForArrow->IsVRBowArrowNocked()
					|| pBowForArrow->GetCurrentCharge() > 0.0f);
			const bool bShowArrowOnSupportHand = !bThisHandHoldsBow
				&& (pBowForArrow->HasVRBowArrowInHand()
					|| pBowForArrow->IsVRBowArrowNocking()
					|| pBowForArrow->IsVRBowArrowNocked());

			bHideBowArrowOnThisHand = !bShowArrowOnWeaponHand && !bShowArrowOnSupportHand;
		}

		// Left-handed mode single mirror: when the physical hand differs from
		// the authored pose side, reflect the entire finished skeleton once
		// across the controller frame so a right-authored pose reads as a left
		// hand (and vice versa). The render weapon mirrors across the same
		// cached frame in its own SetupBones; both flip culling when drawn.
		// The weapon was already positioned from the canonical (pre-reflection)
		// bones above, so we intentionally do NOT re-derive it here.
		m_bReflectPoseActive = ShouldMirrorPose();
		if (m_bReflectPoseActive)
		{
			// Build the reflection frame from the RAW controller aim, not the
			// offset/posed controllerTransform: the per-class hand rotation
			// offsets rotate controllerTransform so its forward column is no
			// longer the aim direction, which corrupts the geometric mirror
			// plane. Use the raw controller orientation (col0 = aim) and the
			// posed hand position (so the plane passes through the hand).
			Vector vecReflectOrigin;
			MatrixGetColumn(controllerTransform, 3, vecReflectOrigin);
			AngleMatrix(m_angLastValidAngles, vecReflectOrigin, m_matReflectFrame);
			TFVR_ReflectBonesInControllerFrame(pBoneToWorldOut, MIN(nMaxBones, pStudioHdr->numbones()), m_matReflectFrame);
		}

		if (bHideBowArrowOnThisHand)
			TFVR_MoveBoneBehindHead(this, pStudioHdr, pBoneToWorldOut, nMaxBones, "weapon_bone_4");
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

	// Find the hand bone in the model.
	// Use the authored pose side (which may differ from the physical side in
	// left-handed mode) so the loaded model's _L/_R skeleton is matched.
	const bool bPoseLeft = m_bPoseAsLeftHand;
	const char* handSuffix = bPoseLeft ? "_L" : "_R";
	const char* handSuffixLower = bPoseLeft ? "_l" : "_r";

	const char* boneNames[4];
	boneNames[0] = bPoseLeft ? "bip_hand_L" : "bip_hand_R";
	boneNames[1] = bPoseLeft ? "weapon_bone_L" : "weapon_bone_R";
	boneNames[2] = bPoseLeft ? "ValveBiped.Bip01_L_Hand" : "ValveBiped.Bip01_R_Hand";
	boneNames[3] = bPoseLeft ? "bip_hand_l" : "bip_hand_r";

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

	if (m_hHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
	{
		CTFCompoundBow *pBow = static_cast<CTFCompoundBow *>(m_hHeldWeapon.Get());
		if (!pBow->IsVRBowArrowNocked())
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

	if (bUseCurrentAnimation && m_bMedigunLeverActive && m_iMedigunLeverSeq >= 0)
	{
		// Medigun lever active: sample the lever animation so the right hand
		// grip target follows the lever push, not the body animation.
		seqToSample = m_iMedigunLeverSeq;
		cycleToSample = m_flMedigunLeverCycle;
	}
	else if (bUseCurrentAnimation && m_bPlayingReloadAnim && m_iLeverReloadSequence >= 0
		&& m_hHeldWeapon.Get()
		&& (m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| m_hHeldWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| m_hHeldWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON))
	{
		// Pump weapons: the pump hand drives the grip target.
		seqToSample = m_iLeverReloadSequence;
		cycleToSample = m_flLeverReloadCycle;
	}
	else if (bUseCurrentAnimation && (m_bPlayingFireAnim || m_bPlayingChargeAnim))
	{
		// Sample current fire/charge animation - grip target will move with recoil/charge
		seqToSample = GetSequence();
		cycleToSample = GetCycle();
	}
	else if (m_iReloadLoopSequence >= 0 && m_hHeldWeapon.Get()
		&& m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
	{
		// Stickybomb launcher: idle has no left-hand grip pose, so sample the
		// reload loop at frame 0 to provide a stable pump-handle grip point.
		seqToSample = m_iReloadLoopSequence;
		cycleToSample = 0.0f;
	}
	else if (m_iReloadLoopSequence >= 0 && m_hHeldWeapon.Get()
		&& m_hHeldWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
		&& m_bBisonUseReloadGrip)
	{
		// Bison: off-hand is closer to the reload grip (pump handle).
		seqToSample = m_iReloadLoopSequence;
		cycleToSample = 0.0f;
	}
	else if (m_iReloadLoopSequence >= 0 && m_hHeldWeapon.Get()
		&& m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
		&& m_bManglerUseReloadGrip)
	{
		// Mangler: off-hand is closer to the reload grip (pump handle).
		seqToSample = m_iReloadLoopSequence;
		cycleToSample = 0.0f;
	}
	else if (m_iReloadLoopSequence >= 0 && m_hHeldWeapon.Get()
		&& m_hHeldWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
		&& m_bPomsonUseReloadGrip)
	{
		// Pomson: right hand is closer to the reload grip.
		seqToSample = m_iReloadLoopSequence;
		cycleToSample = 0.0f;
	}
	else
	{
		// Sample idle animation - grip target stays stable (also Bison/Mangler idle foregrip)
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

	// Medigun lever: if vm_weapon_bone_L exists on the arms model, derive
	// the off-hand grip target from its displacement between idle and the
	// lever animation.  If it doesn't exist, trust that the fire_on animation
	// already moves bip_hand_R correctly (it should on the arms model).
	if (m_bMedigunLeverActive && m_iMedigunLeverSeq >= 0 && m_iOffHandBone >= 0)
	{
		int leverBone = LookupBone("vm_weapon_bone_L");
		if (leverBone < 0)
			leverBone = LookupBone("vm_weapon_bone_l");

		if (leverBone >= 0 && leverBone < numBones)
		{
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

			matrix3x4_t idleBones[MAXSTUDIOBONES];
			for (int i = 0; i < numBones; i++)
			{
				matrix3x4_t btp;
				QuaternionMatrix(qIdle[i], posIdle[i], btp);
				const mstudiobone_t *pBone = pStudioHdr->pBone(i);
				if (!pBone) { SetIdentityMatrix(idleBones[i]); continue; }
				if (pBone->parent == -1)
					MatrixCopy(btp, idleBones[i]);
				else if (pBone->parent >= 0 && pBone->parent < numBones)
					ConcatTransforms(idleBones[pBone->parent], btp, idleBones[i]);
				else
					SetIdentityMatrix(idleBones[i]);
			}

			matrix3x4_t invIdleLever;
			MatrixInvert(idleBones[leverBone], invIdleLever);
			matrix3x4_t leverDelta;
			ConcatTransforms(sampledBones[leverBone], invIdleLever, leverDelta);

			ConcatTransforms(leverDelta, idleBones[m_iOffHandBone], sampledBones[m_iOffHandBone]);
		}
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

	// Apply offhand grip rotation (must match SetupBones for consistency).
	// Use support-hand role lookup so left-handed/flipped weapons sample grip
	// targets in the same solved frame as their final weapon-hand pose.
	C_TFVRHand *pGripHand = NULL;
	if (m_hHeldWeapon.Get())
	{
		pGripHand = TFVR_GetSupportHand(m_hHeldWeapon.Get());
		if (pGripHand == this)
			pGripHand = NULL;
	}
	else if (IsRightHand())
	{
		pGripHand = GetLocalPlayerLeftHand();
	}
	float rotationBlend = pGripHand ? pGripHand->GetGripRotationBlend() : 0.0f;
	bool bWasGripActive = pGripHand && pGripHand->WasOffhandGripActive();
	bool bIsGripActive = pGripHand && pGripHand->IsOffhandGripActive();

	if (rotationBlend > 0.001f && bWasGripActive && tfvr_offhand_grip_enabled.GetBool())
	{
		Vector desiredY = pGripHand->GetOffhandGripForward();

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

	// Pomson right-hand detach: the weapon is driven from the captured
	// left-controller offset, but the animation sampling above still used this
	// (right) hand's controller as the reference.  Override controllerTransform
	// to a virtual right hand derived from the detached weapon_bone so
	// bip_hand_L ends up in the same frame as the visible weapon.
	if (m_bRightHandDetached && m_bHasIdleWeaponBone)
	{
		matrix3x4_t wpnWorld;
		if (GetPomsonDetachedWeaponBoneWorld(wpnWorld))
		{
			matrix3x4_t invLocal;
			MatrixInvert(m_matIdleWeaponBoneLocal, invLocal);
			ConcatTransforms(wpnWorld, invLocal, controllerTransform);
		}
	}

	// Compute anchor delta that maps model-space → world-space.
	// Medigun lever and stickybomb pump sample non-idle sequences whose
	// bip_hand position differs from idle, so anchor from the sampled
	// animation's hand bone.  Everything else (including scattergun)
	// uses the cached idle hand bone for a stable grip target.
	//
	// Pump handles authored relative to weapon_bone need the off-hand target
	// anchored from weapon_bone, not the idle hand pose, or the sampled pump
	// hand drifts away from the live weapon.
	bool bAnchorFromSampled = (m_bMedigunLeverActive && m_iMedigunLeverSeq >= 0);
	bool bAnchorFromWeaponBone = false;
	if (!bAnchorFromSampled && m_hHeldWeapon.Get()
		&& seqToSample == m_iReloadLoopSequence && m_iReloadLoopSequence >= 0)
	{
		if ((m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| IsPumpActionShotgunWeaponID(m_hHeldWeapon->GetWeaponID()))
			&& m_bHasIdleWeaponBone)
		{
			bAnchorFromWeaponBone = true;
		}
		else if (m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| m_hHeldWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| m_hHeldWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON)
		{
			bAnchorFromSampled = true;
		}
	}

	matrix3x4_t anchorDelta;
	if (bAnchorFromWeaponBone)
	{
		int wpnBoneIdx = LookupBone("weapon_bone");
		if (wpnBoneIdx >= 0 && wpnBoneIdx < numBones)
		{
			matrix3x4_t weaponBoneWorld;
			bool bGotLiveWeaponBone = false;
			if (m_hHeldWeapon.Get()
				&& IsPumpActionShotgunWeaponID(m_hHeldWeapon->GetWeaponID())
				&& m_bPlayingFireAnim
				&& GetSequence() >= 0
				&& m_iHandBone >= 0 && m_iHandBone < numBones)
			{
				Vector activePos[MAXSTUDIOBONES];
				Quaternion activeQ[MAXSTUDIOBONES];
				for (int i = 0; i < MAXSTUDIOBONES; i++)
				{
					activePos[i].Init();
					activeQ[i].Init(0, 0, 0, 1);
				}

				boneSetup.InitPose(activePos, activeQ);
				boneSetup.AccumulatePose(activePos, activeQ, GetSequence(), GetCycle(),
					1.0f, gpGlobals->curtime, NULL);

				matrix3x4_t activeModelBones[MAXSTUDIOBONES];
				for (int i = 0; i < numBones; i++)
				{
					matrix3x4_t boneToParent;
					QuaternionMatrix(activeQ[i], activePos[i], boneToParent);

					const mstudiobone_t *pBone = pStudioHdr->pBone(i);
					if (!pBone)
					{
						SetIdentityMatrix(activeModelBones[i]);
						continue;
					}

					if (pBone->parent == -1)
						MatrixCopy(boneToParent, activeModelBones[i]);
					else if (pBone->parent >= 0 && pBone->parent < numBones)
						ConcatTransforms(activeModelBones[pBone->parent], boneToParent, activeModelBones[i]);
					else
						SetIdentityMatrix(activeModelBones[i]);
				}

				matrix3x4_t invActiveHand;
				MatrixInvert(activeModelBones[m_iHandBone], invActiveHand);

				matrix3x4_t activeAnchorDelta;
				ConcatTransforms(controllerTransform, invActiveHand, activeAnchorDelta);
				ConcatTransforms(activeAnchorDelta, activeModelBones[wpnBoneIdx], weaponBoneWorld);
				bGotLiveWeaponBone = true;
			}

			if (!bGotLiveWeaponBone)
			{
				ConcatTransforms(controllerTransform, m_matIdleWeaponBoneLocal, weaponBoneWorld);
			}

			matrix3x4_t invSampledWeaponBone;
			MatrixInvert(sampledBones[wpnBoneIdx], invSampledWeaponBone);
			ConcatTransforms(weaponBoneWorld, invSampledWeaponBone, anchorDelta);
		}
		else
		{
			matrix3x4_t invSampledHandBone;
			MatrixInvert(sampledBones[m_iHandBone], invSampledHandBone);
			ConcatTransforms(controllerTransform, invSampledHandBone, anchorDelta);
		}
	}
	else if (bAnchorFromSampled)
	{
		matrix3x4_t invSampledHandBone;
		MatrixInvert(sampledBones[m_iHandBone], invSampledHandBone);
		ConcatTransforms(controllerTransform, invSampledHandBone, anchorDelta);
	}
	else if (m_bHandBoneOffsetValid)
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

	// Transform the sampled off-hand bone (bip_hand_L) to world space using the anchor delta.
	matrix3x4_t offHandWorld;
	ConcatTransforms(anchorDelta, sampledBones[m_iOffHandBone], offHandWorld);

	// Shotgun pump action is additive: vr_fire can move the support hand,
	// while the manual pump samples a separate subrange of the normal fire anim.
	// Add just the pump stroke's local-space hand displacement on top.
	if (bUseCurrentAnimation
		&& m_hHeldWeapon.Get()
		&& IsPumpActionShotgunWeaponID(m_hHeldWeapon->GetWeaponID())
		&& m_bPlayingReloadAnim
		&& m_iLeverReloadSequence >= 0
		&& m_iOffHandBone >= 0 && m_iOffHandBone < numBones)
	{
		int weaponBoneIdx = LookupBone("weapon_bone");
		if (weaponBoneIdx >= 0 && weaponBoneIdx < numBones)
		{
			Vector pumpStartPos[MAXSTUDIOBONES];
			Quaternion pumpStartQ[MAXSTUDIOBONES];
			Vector pumpCurPos[MAXSTUDIOBONES];
			Quaternion pumpCurQ[MAXSTUDIOBONES];
			for (int i = 0; i < MAXSTUDIOBONES; i++)
			{
				pumpStartPos[i].Init();
				pumpStartQ[i].Init(0, 0, 0, 1);
				pumpCurPos[i].Init();
				pumpCurQ[i].Init(0, 0, 0, 1);
			}

			boneSetup.InitPose(pumpStartPos, pumpStartQ);
			boneSetup.AccumulatePose(pumpStartPos, pumpStartQ, m_iLeverReloadSequence,
				m_flShotgunPumpStartCycle, 1.0f, gpGlobals->curtime, NULL);

			boneSetup.InitPose(pumpCurPos, pumpCurQ);
			boneSetup.AccumulatePose(pumpCurPos, pumpCurQ, m_iLeverReloadSequence,
				m_flLeverReloadCycle, 1.0f, gpGlobals->curtime, NULL);

			matrix3x4_t pumpStartBones[MAXSTUDIOBONES];
			matrix3x4_t pumpCurBones[MAXSTUDIOBONES];
			for (int i = 0; i < numBones; i++)
			{
				matrix3x4_t startLocal;
				matrix3x4_t curLocal;
				QuaternionMatrix(pumpStartQ[i], pumpStartPos[i], startLocal);
				QuaternionMatrix(pumpCurQ[i], pumpCurPos[i], curLocal);

				const mstudiobone_t *pBone = pStudioHdr->pBone(i);
				if (!pBone)
				{
					SetIdentityMatrix(pumpStartBones[i]);
					SetIdentityMatrix(pumpCurBones[i]);
					continue;
				}

				if (pBone->parent == -1)
				{
					MatrixCopy(startLocal, pumpStartBones[i]);
					MatrixCopy(curLocal, pumpCurBones[i]);
				}
				else if (pBone->parent >= 0 && pBone->parent < numBones)
				{
					ConcatTransforms(pumpStartBones[pBone->parent], startLocal, pumpStartBones[i]);
					ConcatTransforms(pumpCurBones[pBone->parent], curLocal, pumpCurBones[i]);
				}
				else
				{
					SetIdentityMatrix(pumpStartBones[i]);
					SetIdentityMatrix(pumpCurBones[i]);
				}
			}

			matrix3x4_t invPumpStartWeapon;
			matrix3x4_t invPumpCurWeapon;
			MatrixInvert(pumpStartBones[weaponBoneIdx], invPumpStartWeapon);
			MatrixInvert(pumpCurBones[weaponBoneIdx], invPumpCurWeapon);

			matrix3x4_t pumpStartHandLocal;
			matrix3x4_t pumpCurHandLocal;
			ConcatTransforms(invPumpStartWeapon, pumpStartBones[m_iOffHandBone], pumpStartHandLocal);
			ConcatTransforms(invPumpCurWeapon, pumpCurBones[m_iOffHandBone], pumpCurHandLocal);

			Vector pumpStartLocalPos;
			Vector pumpCurLocalPos;
			MatrixGetColumn(pumpStartHandLocal, 3, pumpStartLocalPos);
			MatrixGetColumn(pumpCurHandLocal, 3, pumpCurLocalPos);
			Vector pumpLocalDelta = pumpCurLocalPos - pumpStartLocalPos;

			matrix3x4_t weaponWorld;
			ConcatTransforms(anchorDelta, sampledBones[weaponBoneIdx], weaponWorld);

			Vector axisX, axisY, axisZ, offHandPos;
			MatrixGetColumn(weaponWorld, 0, axisX);
			MatrixGetColumn(weaponWorld, 1, axisY);
			MatrixGetColumn(weaponWorld, 2, axisZ);
			MatrixGetColumn(offHandWorld, 3, offHandPos);

			offHandPos += axisX * pumpLocalDelta.x
				+ axisY * pumpLocalDelta.y
				+ axisZ * pumpLocalDelta.z;
			MatrixSetColumn(offHandPos, 3, offHandWorld);
		}
	}

	// Apply middle finger offset for PIVOT and DISTANCE calculations
	// (bUseCurrentAnimation = false).  For VISUAL hand positioning
	// (bUseCurrentAnimation = true), use bip_hand_R directly so the
	// wrist aligns correctly — the distance check already uses the
	// controller's middle finger, so the snap zone is centred properly.
	bool bAppliedFingerOffset = false;
	if (!bUseCurrentAnimation && pGripHand)
	{
		// Calculate where middle finger base SHOULD be using bind pose from the grip hand model.
		// This used to assume the left hand was always the support hand; role lookup keeps
		// flipped/left-handed weapons in the matching support-hand frame.
		CStudioHdr *pLeftStudioHdr = pGripHand->GetModelPtr();
		if (pLeftStudioHdr)
		{
			int leftHandBone = pGripHand->LookupBone(pGripHand->IsLeftHand() ? "bip_hand_L" : "bip_hand_R");
			int leftMiddleFingerBone = pGripHand->LookupBone(pGripHand->IsLeftHand() ? "bip_middle_0_L" : "bip_middle_0_R");

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

bool C_TFVRHand::GetShotgunManualReloadTarget( Vector &outPos, QAngle &outAngles )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !pWeapon || !IsPumpActionShotgunWeaponID( pWeapon->GetWeaponID() ) || m_iShotgunManualReloadSequence < 0 )
		return false;

	CTFShotgun *pShotgun = static_cast< CTFShotgun * >( pWeapon );
	if ( !pShotgun->IsVRShotgunManualReloadActive() )
		return false;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return false;

	const int numBones = pStudioHdr->numbones();
	int iOffHandBone = IsRightHand() ? LookupBone( "bip_hand_L" ) : LookupBone( "bip_hand_R" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "ValveBiped.Bip01_L_Hand" ) : LookupBone( "ValveBiped.Bip01_R_Hand" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "bip_hand_l" ) : LookupBone( "bip_hand_r" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "weapon_bone_L" ) : LookupBone( "weapon_bone_R" );

	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );

	if ( iOffHandBone < 0 || iOffHandBone >= numBones || iOffHandBone >= MAXSTUDIOBONES ||
		iWeaponBone < 0 || iWeaponBone >= numBones || iWeaponBone >= MAXSTUDIOBONES )
		return false;

	float flProgress = pShotgun->IsVRShotgunShellInserting()
		? pShotgun->GetVRShotgunShellInsertProgress() : 0.0f;
	float flCycle = Lerp( flProgress, m_flShotgunManualReloadHoldCycle, m_flShotgunManualReloadCommitCycle );

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iShotgunManualReloadSequence, flCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones && i < MAXSTUDIOBONES; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones && pBone->parent < MAXSTUDIOBONES )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t liveWeaponBone;
	if ( !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t offhandRelativeToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[iOffHandBone], offhandRelativeToWeapon );

	matrix3x4_t offhandWorld;
	ConcatTransforms( liveWeaponBone, offhandRelativeToWeapon, offhandWorld );

	MatrixAngles( offhandWorld, outAngles, outPos );
	return true;
}

bool C_TFVRHand::GetShotgunManualReloadShellTarget( Vector &outPos )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !pWeapon || !IsPumpActionShotgunWeaponID( pWeapon->GetWeaponID() ) || m_iShotgunManualReloadSequence < 0 )
		return false;

	CTFShotgun *pShotgun = static_cast< CTFShotgun * >( pWeapon );
	if ( !pShotgun->IsVRShotgunManualReloadActive() )
		return false;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return false;

	const int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	int iShellBone = pWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_HWG
		? LookupBone( "vm_weapon_bone_2" )
		: LookupBone( "vm_weapon_bone" );
	if ( iShellBone < 0 && pWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_HWG )
		iShellBone = LookupBone( "vm_weapon_bone" );
	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );

	if ( iShellBone < 0 || iShellBone >= numBones || iWeaponBone < 0 || iWeaponBone >= numBones )
		return false;

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iShotgunManualReloadSequence,
		m_flShotgunManualReloadHoldCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t liveWeaponBone;
	if ( !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t shellRelativeToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[iShellBone], shellRelativeToWeapon );

	matrix3x4_t shellWorld;
	ConcatTransforms( liveWeaponBone, shellRelativeToWeapon, shellWorld );
	MatrixGetColumn( shellWorld, 3, outPos );
	return true;
}

bool C_TFVRHand::GetShotgunManualReloadShellPosition( Vector &outPos, bool bUseHeavyShellBone )
{
	int iShellBone = bUseHeavyShellBone ? LookupBone( "vm_weapon_bone_2" ) : LookupBone( "vm_weapon_bone" );
	if ( iShellBone < 0 && bUseHeavyShellBone )
		iShellBone = LookupBone( "vm_weapon_bone" );
	if ( iShellBone < 0 || iShellBone >= MAXSTUDIOBONES )
		return false;

	matrix3x4_t bones[MAXSTUDIOBONES];
	if ( !SetupBones( bones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime ) )
		return false;

	MatrixGetColumn( bones[iShellBone], 3, outPos );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Sample p_reload at flCycle and return vm_weapon_bone relative to
//          weapon_bone (model space). Used to ride the mag on the live gun.
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetPistolReloadMagRelativeToWeapon( float flCycle, matrix3x4_t &outMagRelWeaponBone )
{
	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr || m_iShotgunManualReloadSequence < 0 )
		return false;

	const int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	int iMagBone = LookupBone( "vm_weapon_bone" );
	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iMagBone < 0 || iMagBone >= numBones || iWeaponBone < 0 || iWeaponBone >= numBones )
		return false;

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iShotgunManualReloadSequence,
		clamp( flCycle, 0.0f, 1.0f ), 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );
	ConcatTransforms( invSampledWeaponBone, sampledBones[iMagBone], outMagRelWeaponBone );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: World-space magwell point on the live gun: vm_weapon_bone sampled
//          at the insert target frame, mapped through the live weapon bone.
//          Proximity target for inserting the held magazine.
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetPistolMagazineInsertTarget( Vector &outPos )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	CTFPistol *pPistol = TFVR_GetManualReloadPistol( pWeapon );
	if ( !pPistol || m_iShotgunManualReloadSequence < 0 )
		return false;

	matrix3x4_t magRelWeapon;
	if ( !GetPistolReloadMagRelativeToWeapon( m_flPistolInsertTargetCycle, magRelWeapon ) )
		return false;

	// Use the live (animation-following) weapon bone so the magwell tracks
	// the gun as the reload animation moves it.
	matrix3x4_t liveWeaponBone;
	if ( !GetLiveWeaponBoneTransform( liveWeaponBone )
		&& !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t magWorld;
	ConcatTransforms( liveWeaponBone, magRelWeapon, magWorld );
	MatrixGetColumn( magWorld, 3, outPos );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: World-space target for the off hand during the pistol mag insert:
//          the off-hand bone from p_reload (frames 17-19) mapped through the
//          live weapon bone. Mirrors GetShotgunManualReloadTarget.
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetPistolManualReloadTarget( Vector &outPos, QAngle &outAngles )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	CTFPistol *pPistol = TFVR_GetManualReloadPistol( pWeapon );
	if ( !pPistol || m_iShotgunManualReloadSequence < 0 )
		return false;

	if ( !pPistol->IsVRPistolMagPoseActive() )
		return false;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return false;

	const int numBones = pStudioHdr->numbones();
	int iOffHandBone = IsRightHand() ? LookupBone( "bip_hand_L" ) : LookupBone( "bip_hand_R" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "ValveBiped.Bip01_L_Hand" ) : LookupBone( "ValveBiped.Bip01_R_Hand" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "bip_hand_l" ) : LookupBone( "bip_hand_r" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "weapon_bone_L" ) : LookupBone( "weapon_bone_R" );

	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );

	if ( iOffHandBone < 0 || iOffHandBone >= numBones || iOffHandBone >= MAXSTUDIOBONES ||
		iWeaponBone < 0 || iWeaponBone >= numBones || iWeaponBone >= MAXSTUDIOBONES )
		return false;

	float flProgress = pPistol->IsVRMagInserting() ? pPistol->GetVRMagPhaseProgress() : 0.0f;
	float flCycle = Lerp( flProgress, m_flShotgunManualReloadHoldCycle, m_flShotgunManualReloadCommitCycle );

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iShotgunManualReloadSequence, flCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones && i < MAXSTUDIOBONES; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones && pBone->parent < MAXSTUDIOBONES )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	// Use the live (animation-following) weapon bone so the off-hand target
	// tracks the gun as the reload animation moves it.
	matrix3x4_t liveWeaponBone;
	if ( !GetLiveWeaponBoneTransform( liveWeaponBone )
		&& !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t offhandRelativeToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[iOffHandBone], offhandRelativeToWeapon );

	matrix3x4_t offhandWorld;
	ConcatTransforms( liveWeaponBone, offhandRelativeToWeapon, offhandWorld );

	MatrixAngles( offhandWorld, outAngles, outPos );
	return true;
}

bool C_TFVRHand::GetRocketManualReloadTarget( Vector &outPos, QAngle &outAngles )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !pWeapon || m_iShotgunManualReloadSequence < 0 )
		return false;

	if ( pWeapon->GetWeaponID() != TF_WEAPON_ROCKETLAUNCHER && pWeapon->GetWeaponID() != TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT )
		return false;

	CTFRocketLauncher *pRocketLauncher = static_cast< CTFRocketLauncher * >( pWeapon );
	if ( !pRocketLauncher->IsVRRocketManualReloadActive() )
		return false;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return false;

	const int numBones = pStudioHdr->numbones();
	int iOffHandBone = IsRightHand() ? LookupBone( "bip_hand_L" ) : LookupBone( "bip_hand_R" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "ValveBiped.Bip01_L_Hand" ) : LookupBone( "ValveBiped.Bip01_R_Hand" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "bip_hand_l" ) : LookupBone( "bip_hand_r" );
	if ( iOffHandBone < 0 )
		iOffHandBone = IsRightHand() ? LookupBone( "weapon_bone_L" ) : LookupBone( "weapon_bone_R" );

	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );

	if ( iOffHandBone < 0 || iOffHandBone >= numBones || iOffHandBone >= MAXSTUDIOBONES ||
		iWeaponBone < 0 || iWeaponBone >= numBones || iWeaponBone >= MAXSTUDIOBONES )
		return false;

	float flProgress = pRocketLauncher->IsVRRocketInserting()
		? pRocketLauncher->GetVRRocketVisualInsertProgress() : 0.0f;
	float flCycle = Lerp( flProgress, m_flShotgunManualReloadHoldCycle, m_flShotgunManualReloadCommitCycle );

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iShotgunManualReloadSequence, flCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones && i < MAXSTUDIOBONES; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones && pBone->parent < MAXSTUDIOBONES )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t liveWeaponBone;
	if ( !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t offhandRelativeToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[iOffHandBone], offhandRelativeToWeapon );

	matrix3x4_t offhandWorld;
	ConcatTransforms( liveWeaponBone, offhandRelativeToWeapon, offhandWorld );

	MatrixAngles( offhandWorld, outAngles, outPos );
	return true;
}

bool C_TFVRHand::GetRocketManualReloadRocketTarget( Vector &outPos )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !pWeapon || m_iShotgunManualReloadSequence < 0 )
		return false;

	if ( pWeapon->GetWeaponID() != TF_WEAPON_ROCKETLAUNCHER && pWeapon->GetWeaponID() != TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT )
		return false;

	CTFRocketLauncher *pRocketLauncher = static_cast< CTFRocketLauncher * >( pWeapon );
	if ( !pRocketLauncher->IsVRRocketManualReloadActive() )
		return false;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return false;

	const int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	int iRocketBone = LookupBone( "rocket" );
	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );

	if ( iRocketBone < 0 || iRocketBone >= numBones )
	{
		QAngle targetAngles;
		return GetRocketManualReloadTarget( outPos, targetAngles );
	}

	if ( iWeaponBone < 0 || iWeaponBone >= numBones )
		return false;

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iShotgunManualReloadSequence,
		m_flShotgunManualReloadHoldCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t liveWeaponBone;
	if ( !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t rocketRelativeToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[iRocketBone], rocketRelativeToWeapon );

	matrix3x4_t rocketWorld;
	ConcatTransforms( liveWeaponBone, rocketRelativeToWeapon, rocketWorld );
	MatrixGetColumn( rocketWorld, 3, outPos );
	return true;
}

bool C_TFVRHand::GetRocketManualReloadRocketPosition( Vector &outPos )
{
	int iRocketBone = LookupBone( "rocket" );
	int iProbeBone = iRocketBone;
	if ( iProbeBone < 0 || iProbeBone >= MAXSTUDIOBONES )
		return false;

	matrix3x4_t bones[MAXSTUDIOBONES];
	if ( !SetupBones( bones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime ) )
		return false;

	MatrixGetColumn( bones[iProbeBone], 3, outPos );
	return true;
}

bool C_TFVRHand::GetBowManualReloadTarget( Vector &outPos, QAngle &outAngles, matrix3x4_t *pOutWorld )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_COMPOUND_BOW || m_iShotgunManualReloadSequence < 0 )
		return false;

	CTFCompoundBow *pBow = static_cast< CTFCompoundBow * >( pWeapon );
	if ( !pBow->IsVRBowArrowPoseActive() )
		return false;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return false;

	const int numBones = pStudioHdr->numbones();

	// The drawstring hand is ALWAYS authored as bip_hand_L in the arms grip
	// pose, regardless of handedness/mirroring. Sample that bone's full
	// transform (position + orientation) so the draw hand attaches to the
	// bowstring at its proper authored grip. Fall back to the arrow bone only
	// if the model has no left-hand bone.
	int iTargetBone = LookupBone( "bip_hand_L" );
	if ( iTargetBone < 0 )
		iTargetBone = LookupBone( "ValveBiped.Bip01_L_Hand" );
	if ( iTargetBone < 0 )
		iTargetBone = LookupBone( "bip_hand_l" );
	if ( iTargetBone < 0 )
		iTargetBone = LookupBone( "weapon_bone_4" );

	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );
	int iStringBone = LookupBone( "weapon_bone_3" );

	if ( iTargetBone < 0 || iTargetBone >= numBones || iTargetBone >= MAXSTUDIOBONES ||
		iWeaponBone < 0 || iWeaponBone >= numBones || iWeaponBone >= MAXSTUDIOBONES )
		return false;

	// Sample the pose that the bow hand is actually showing so the arrow nock
	// tracks the pull. While nocking, lerp the draw pose; once nocked, sample
	// the live charge animation (this is read-only for the visual attach and no
	// longer feeds the aim solve, so there is no feedback loop).
	int iSequence = m_iShotgunManualReloadSequence;
	float flCycle = m_flShotgunManualReloadCommitCycle;
	if ( pBow->IsVRBowArrowNocking() )
	{
		flCycle = Lerp( TFVR_GetBowNockVisualProgress( pBow ),
			m_flShotgunManualReloadHoldCycle,
			m_flShotgunManualReloadCommitCycle );
	}
	else if ( pBow->IsVRBowArrowNocked() && m_bPlayingChargeAnim && GetSequence() >= 0 )
	{
		iSequence = GetSequence();
		flCycle = GetCycle();
	}

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, iSequence, flCycle, 1.0f, gpGlobals->curtime, NULL );

	// Match the weapon hand's bow pose: bw_draw at no pull, bw_charge at full
	// pull, with shake offsets layered against the same blended base.
	{
		float flBowChargeFraction = clamp( pBow->GetCurrentCharge() / MAX( pBow->GetChargeMaxTime(), 0.01f ), 0.0f, 1.0f );
		const bool bUseFullChargeReference = true;
		ApplyBowDrawChargeBlend( pStudioHdr, numBones, posAnim, qAnim, flBowChargeFraction );
		ApplyBowShakeOverlay( pStudioHdr, numBones, posAnim, qAnim, flBowChargeFraction, bUseFullChargeReference );
	}

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones && i < MAXSTUDIOBONES; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones && pBone->parent < MAXSTUDIOBONES )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t liveWeaponBone;
	if ( !GetLiveWeaponBoneTransform( liveWeaponBone )
		&& !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return false;
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t offhandWorld;
	if ( iStringBone >= 0 && iStringBone < numBones && iStringBone < MAXSTUDIOBONES
		&& m_bLiveBowStringBoneWorldValid )
	{
		// Keep the authored draw-hand pose, but express it relative to the
		// drawstring nock. This prevents small bow-root dips in bw_draw/bw_charge
		// from dragging the hand away from the string during attach/charge fades.
		matrix3x4_t invSampledStringBone;
		MatrixInvert( sampledBones[iStringBone], invSampledStringBone );

		matrix3x4_t offhandRelativeToString;
		ConcatTransforms( invSampledStringBone, sampledBones[iTargetBone], offhandRelativeToString );
		ConcatTransforms( m_matLiveBowStringBoneWorld, offhandRelativeToString, offhandWorld );
	}
	else
	{
		// Fallback for models without weapon_bone_3.
		matrix3x4_t offhandRelativeToWeapon;
		ConcatTransforms( invSampledWeaponBone, sampledBones[iTargetBone], offhandRelativeToWeapon );
		ConcatTransforms( liveWeaponBone, offhandRelativeToWeapon, offhandWorld );
	}

	if (TFVR_ShouldMirrorWeaponHand(pWeapon))
	{
		matrix3x4_t reflectFrame;
		if (m_bReflectPoseActive)
		{
			MatrixCopy(m_matReflectFrame, reflectFrame);
		}
		else
		{
			AngleMatrix(m_angLastValidAngles, m_vecLastValidPosition, reflectFrame);
		}
		TFVR_ReflectBonesInControllerFrame(&offhandWorld, 1, reflectFrame);
	}

	// Emit the full world matrix so callers can preserve the reflection (when
	// mirrored, offhandWorld is improper and cannot be round-tripped through
	// QAngle without losing the mirror).
	if ( pOutWorld )
		MatrixCopy( offhandWorld, *pOutWorld );

	MatrixAngles( offhandWorld, outAngles, outPos );
	return true;
}

bool C_TFVRHand::GetBowArrowPosition( Vector &outPos )
{
	int iArrowBone = LookupBone( "weapon_bone_4" );
	if ( iArrowBone < 0 )
		iArrowBone = LookupBone( "vm_weapon_bone" );
	if ( iArrowBone < 0 )
		iArrowBone = LookupBone( "weapon_bone" );
	if ( iArrowBone < 0 || iArrowBone >= MAXSTUDIOBONES )
		return false;

	matrix3x4_t bones[MAXSTUDIOBONES];
	if ( !SetupBones( bones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime ) )
		return false;

	MatrixGetColumn( bones[iArrowBone], 3, outPos );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: World position of the bow's nock point for arrow-nock detection.
//          This is the arrow bone (weapon_bone_4) sampled in the RESTING fire
//          pose (bw_fire frame 10), mapped through the live weapon bone, so the
//          detection point is the bow's nock in its idle pose regardless of what
//          the weapon hand happens to be animating this frame.
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetBowNockDetectionPoint( Vector &outPos )
{
	if ( m_iBowIdleSequence < 0 )
		return GetBowArrowPosition( outPos );

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return GetBowArrowPosition( outPos );

	const int numBones = pStudioHdr->numbones();
	// The nock detection point is weapon_bone_3 (the bow's string nock), not
	// weapon_bone_4 (the held arrow). Fall back if the model lacks it.
	int iArrowBone = LookupBone( "weapon_bone_3" );
	if ( iArrowBone < 0 )
		iArrowBone = LookupBone( "weapon_bone_4" );
	int iWeaponBone = LookupBone( "weapon_bone" );
	if ( iWeaponBone < 0 )
		iWeaponBone = LookupBone( "vm_weapon_bone" );
	if ( iArrowBone < 0 || iArrowBone >= numBones || iArrowBone >= MAXSTUDIOBONES ||
		iWeaponBone < 0 || iWeaponBone >= numBones || iWeaponBone >= MAXSTUDIOBONES )
		return GetBowArrowPosition( outPos );

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}
	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iBowIdleSequence, m_flBowIdleCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones && i < MAXSTUDIOBONES; i++ )
	{
		matrix3x4_t local;
		QuaternionMatrix( qAnim[i], posAnim[i], local );
		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}
		if ( pBone->parent == -1 )
			MatrixCopy( local, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones && pBone->parent < MAXSTUDIOBONES )
			ConcatTransforms( sampledBones[pBone->parent], local, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t liveWeaponBone;
	if ( !GetCachedWeaponBoneTransform( liveWeaponBone ) )
	{
		if ( m_bHasIdleWeaponBone )
			MatrixCopy( m_matIdleWeaponBoneWorld, liveWeaponBone );
		else
			return GetBowArrowPosition( outPos );
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[iWeaponBone], invSampledWeaponBone );

	matrix3x4_t arrowRelativeToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[iArrowBone], arrowRelativeToWeapon );

	matrix3x4_t arrowWorld;
	ConcatTransforms( liveWeaponBone, arrowRelativeToWeapon, arrowWorld );

	// In left-handed/mirrored mode the visible bow is reflected across the
	// weapon hand's controller frame, so reflect the detection point too —
	// otherwise it sits on the canonical (unmirrored) side and the off-hand
	// arrow never reaches it.
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( pWeapon && TFVR_ShouldMirrorWeaponHand( pWeapon ) )
	{
		matrix3x4_t reflectFrame;
		if ( m_bReflectPoseActive )
			MatrixCopy( m_matReflectFrame, reflectFrame );
		else
			AngleMatrix( m_angLastValidAngles, m_vecLastValidPosition, reflectFrame );
		TFVR_ReflectBonesInControllerFrame( &arrowWorld, 1, reflectFrame );
	}

	MatrixGetColumn( arrowWorld, 3, outPos );
	return true;
}

void C_TFVRHand::ApplyBowDrawChargeBlend( CStudioHdr *pStudioHdr, int numBones, Vector *posAnim, Quaternion *qAnim, float chargeFraction )
{
	// posAnim/qAnim already hold the bw_charge pose (scrubbed by charge). Blend
	// in the fully-nocked bw_draw frame-35 pose so that at charge 0 we show the
	// authored drawn pose (bow + drawstring + arrow + draw-hand grip), easing
	// into the bw_charge pull as the player draws.
	if ( !pStudioHdr || !posAnim || !qAnim || m_iBowDrawSequence < 0 )
		return;

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posDraw[MAXSTUDIOBONES];
	Quaternion qDraw[MAXSTUDIOBONES];
	Vector posCharge[MAXSTUDIOBONES];
	Quaternion qCharge[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posDraw[i].Init();
		qDraw[i].Init( 0, 0, 0, 1 );
		posCharge[i] = posAnim[i];
		qCharge[i] = qAnim[i];
	}
	boneSetup.InitPose( posDraw, qDraw );
	boneSetup.AccumulatePose( posDraw, qDraw, m_iBowDrawSequence, m_flBowDrawNockCycle, 1.0f, gpGlobals->curtime, NULL );

	const float w = clamp( chargeFraction, 0.0f, 1.0f );
	for ( int i = 0; i < numBones && i < MAXSTUDIOBONES; i++ )
	{
		VectorLerp( posDraw[i], posAnim[i], w, posAnim[i] );
		QuaternionSlerp( qDraw[i], qAnim[i], w, qAnim[i] );
	}

	const int iStringBone = LookupBone( "weapon_bone_3" );
	if ( iStringBone >= 0 )
	{
		int targetBones[2];
		int nTargetBones = 0;

		int iHandBone = LookupBone( "bip_hand_L" );
		if ( iHandBone < 0 )
			iHandBone = LookupBone( "ValveBiped.Bip01_L_Hand" );
		if ( iHandBone < 0 )
			iHandBone = LookupBone( "bip_hand_l" );
		if ( iHandBone >= 0 )
			targetBones[nTargetBones++] = iHandBone;

		int iArrowBone = LookupBone( "weapon_bone_4" );
		if ( iArrowBone >= 0 )
			targetBones[nTargetBones++] = iArrowBone;

		TFVR_BlendPoseBonesRelativeToReference( pStudioHdr, numBones,
			posDraw, qDraw, posCharge, qCharge, w,
			iStringBone, targetBones, nTargetBones, posAnim, qAnim );
	}
}

void C_TFVRHand::CaptureBowFireStartPose( CTFCompoundBow *pBow )
{
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pBow || !pStudioHdr || m_iChargeSequence < 0 || m_iBowIdleSequence < 0 )
		return;

	const int nBoneCount = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	if ( nBoneCount <= 0 )
		return;

	const bool bHasBowPose = pBow->IsVRBowArrowNocking()
		|| pBow->IsVRBowArrowNocked()
		|| m_bPlayingChargeAnim
		|| pBow->GetCurrentCharge() > 0.0f;
	if ( !bHasBowPose )
		return;

	float flChargeFraction = clamp( pBow->GetCurrentCharge() / MAX( pBow->GetChargeMaxTime(), 0.01f ), 0.0f, 1.0f );
	if ( m_bPlayingChargeAnim && GetSequence() == m_iChargeSequence )
		flChargeFraction = MAX( flChargeFraction, clamp( GetCycle(), 0.0f, 1.0f ) );

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posCapture[MAXSTUDIOBONES];
	Quaternion qCapture[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posCapture[i].Init();
		qCapture[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posCapture, qCapture );
	boneSetup.AccumulatePose( posCapture, qCapture, m_iChargeSequence, flChargeFraction, 1.0f, gpGlobals->curtime, NULL );

	const bool bUseFullChargeReference = true;
	ApplyBowDrawChargeBlend( pStudioHdr, nBoneCount, posCapture, qCapture, flChargeFraction );
	ApplyBowShakeOverlay( pStudioHdr, nBoneCount, posCapture, qCapture, flChargeFraction, bUseFullChargeReference );

	for ( int i = 0; i < nBoneCount; i++ )
	{
		m_vecBowFireStartPose[i] = posCapture[i];
		m_qBowFireStartPose[i] = qCapture[i];
	}

	m_iBowFireStartPoseBoneCount = nBoneCount;
	m_bBowFireStartPoseValid = true;
}

void C_TFVRHand::ApplyBowFireStartPoseBlend( CStudioHdr *pStudioHdr, int numBones, Vector *posAnim, Quaternion *qAnim, float fireCycle )
{
	if ( !pStudioHdr || !posAnim || !qAnim || !m_bBowFireStartPoseValid
		|| m_iBowFireStartPoseBoneCount <= 0 )
		return;

	const float flFirstFrameCycle = MAX( m_flBowFireFirstFrameCycle, 0.001f );
	const float flFrameZeroEpsilon = 0.001f;
	if ( fireCycle >= flFirstFrameCycle )
		return;

	const int nBoneCount = MIN( MIN( numBones, pStudioHdr->numbones() ), m_iBowFireStartPoseBoneCount );
	if ( nBoneCount <= 0 )
		return;

	if ( fireCycle <= flFrameZeroEpsilon )
	{
		for ( int i = 0; i < nBoneCount && i < MAXSTUDIOBONES; i++ )
		{
			posAnim[i] = m_vecBowFireStartPose[i];
			qAnim[i] = m_qBowFireStartPose[i];
		}
		return;
	}

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );
	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, m_iBowIdleSequence, flFirstFrameCycle, 1.0f, gpGlobals->curtime, NULL );
}

void C_TFVRHand::ApplyBowShakeOverlay( CStudioHdr *pStudioHdr, int numBones, Vector *posAnim, Quaternion *qAnim, float chargeFraction, bool bUseFullChargeReference )
{
	if ( !pStudioHdr || !posAnim || !qAnim || !m_bBowShakeOverlayActive
		|| m_iChargeSequence < 0 || m_iBowShakeOverlaySequence < 0 )
		return;

	const int nBoneCount = MIN( MIN( numBones, pStudioHdr->numbones() ), MAXSTUDIOBONES );
	if ( nBoneCount <= 0 )
		return;

	float flOverlayDuration = SequenceDuration( pStudioHdr, m_iBowShakeOverlaySequence );
	float flOverlayCycle = 0.0f;
	if ( flOverlayDuration > 0.0f )
	{
		float flElapsed = MAX( gpGlobals->curtime - m_flBowShakeOverlayStartTime, 0.0f );
		flOverlayCycle = (m_iBowShakeOverlaySequence == m_iBowChargeIdleSequence)
			? fmod( flElapsed / flOverlayDuration, 1.0f )
			: clamp( flElapsed / flOverlayDuration, 0.0f, 1.0f );
	}

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posBase[MAXSTUDIOBONES];
	Quaternion qBase[MAXSTUDIOBONES];
	Vector posShake[MAXSTUDIOBONES];
	Quaternion qShake[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posBase[i].Init();
		qBase[i].Init( 0, 0, 0, 1 );
		posShake[i].Init();
		qShake[i].Init( 0, 0, 0, 1 );
	}

	const float flChargeCycle = clamp( chargeFraction, 0.0f, 1.0f );
	const float flShakeWeight = SimpleSpline( flChargeCycle );
	if ( flShakeWeight <= 0.0f )
		return;

	// bw_shake/bw_idle3 are authored around the fully drawn pose. Use full
	// bw_charge as the reference so the overlay contains only shake, not
	// "pull back to full draw" displacement when the player de-pulls.
	const float flReferenceCycle = bUseFullChargeReference ? 1.0f : flChargeCycle;
	boneSetup.InitPose( posBase, qBase );
	boneSetup.AccumulatePose( posBase, qBase, m_iChargeSequence, flReferenceCycle, 1.0f, gpGlobals->curtime, NULL );
	boneSetup.InitPose( posShake, qShake );
	boneSetup.AccumulatePose( posShake, qShake, m_iBowShakeOverlaySequence, flOverlayCycle, 1.0f, gpGlobals->curtime, NULL );

	for ( int i = 0; i < nBoneCount; i++ )
	{
		matrix3x4_t baseLocal;
		matrix3x4_t shakeLocal;
		matrix3x4_t currentLocal;
		QuaternionMatrix( qBase[i], posBase[i], baseLocal );
		QuaternionMatrix( qShake[i], posShake[i], shakeLocal );
		QuaternionMatrix( qAnim[i], posAnim[i], currentLocal );

		matrix3x4_t invBaseLocal;
		matrix3x4_t shakeDelta;
		matrix3x4_t weightedShakeDelta;
		matrix3x4_t resultLocal;
		MatrixInvert( baseLocal, invBaseLocal );
		ConcatTransforms( invBaseLocal, shakeLocal, shakeDelta );

		Quaternion deltaQuat;
		Vector deltaPos;
		MatrixAngles( shakeDelta, deltaQuat, deltaPos );
		deltaPos *= flShakeWeight;
		Quaternion identityQuat;
		identityQuat.Init( 0, 0, 0, 1 );
		Quaternion weightedQuat;
		SafeQuaternionSlerp( identityQuat, deltaQuat, flShakeWeight, weightedQuat );
		QuaternionMatrix( weightedQuat, deltaPos, weightedShakeDelta );

		ConcatTransforms( currentLocal, weightedShakeDelta, resultLocal );
		MatrixAngles( resultLocal, qAnim[i], posAnim[i] );
	}
}

void C_TFVRHand::ApplyBowShakeWorldOverlay( CStudioHdr *pStudioHdr, matrix3x4_t *pBoneToWorldOut, int nMaxBones, const matrix3x4_t &anchorDelta, float chargeFraction, bool bUseFullChargeReference )
{
	if ( !pStudioHdr || !pBoneToWorldOut || !m_bBowShakeOverlayActive
		|| m_iChargeSequence < 0 || m_iBowShakeOverlaySequence < 0 )
		return;

	const int nBoneCount = MIN( MIN( nMaxBones, pStudioHdr->numbones() ), MAXSTUDIOBONES );
	if ( nBoneCount <= 0 )
		return;

	float flOverlayDuration = SequenceDuration( pStudioHdr, m_iBowShakeOverlaySequence );
	float flOverlayCycle = 0.0f;
	if ( flOverlayDuration > 0.0f )
	{
		float flElapsed = MAX( gpGlobals->curtime - m_flBowShakeOverlayStartTime, 0.0f );
		flOverlayCycle = (m_iBowShakeOverlaySequence == m_iBowChargeIdleSequence)
			? fmod( flElapsed / flOverlayDuration, 1.0f )
			: clamp( flElapsed / flOverlayDuration, 0.0f, 1.0f );
	}

	float poseParams[MAXSTUDIOPOSEPARAM];
	memset( poseParams, 0, sizeof( poseParams ) );
	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParams );

	Vector posBase[MAXSTUDIOBONES];
	Quaternion qBase[MAXSTUDIOBONES];
	Vector posShake[MAXSTUDIOBONES];
	Quaternion qShake[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posBase[i].Init();
		qBase[i].Init( 0, 0, 0, 1 );
		posShake[i].Init();
		qShake[i].Init( 0, 0, 0, 1 );
	}

	const float flChargeCycle = clamp( chargeFraction, 0.0f, 1.0f );
	const float flShakeWeight = SimpleSpline( flChargeCycle );
	if ( flShakeWeight <= 0.0f )
		return;

	// Match the local overlay: measure shake from full draw, then layer that
	// offset onto the current controller-anchored pull/de-pull pose.
	const float flReferenceCycle = bUseFullChargeReference ? 1.0f : flChargeCycle;
	boneSetup.InitPose( posBase, qBase );
	boneSetup.AccumulatePose( posBase, qBase, m_iChargeSequence, flReferenceCycle, 1.0f, gpGlobals->curtime, NULL );
	boneSetup.InitPose( posShake, qShake );
	boneSetup.AccumulatePose( posShake, qShake, m_iBowShakeOverlaySequence, flOverlayCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t baseModel[MAXSTUDIOBONES];
	matrix3x4_t shakeModel[MAXSTUDIOBONES];
	TFVR_BuildModelSpacePose( pStudioHdr, nBoneCount, posBase, qBase, baseModel );
	TFVR_BuildModelSpacePose( pStudioHdr, nBoneCount, posShake, qShake, shakeModel );

	for ( int i = 0; i < nBoneCount; i++ )
	{
		matrix3x4_t baseWorld;
		matrix3x4_t shakeWorld;
		matrix3x4_t invBaseWorld;
		matrix3x4_t deltaWorld;
		matrix3x4_t weightedDeltaWorld;
		matrix3x4_t resultWorld;

		ConcatTransforms( anchorDelta, baseModel[i], baseWorld );
		ConcatTransforms( anchorDelta, shakeModel[i], shakeWorld );
		MatrixInvert( baseWorld, invBaseWorld );
		ConcatTransforms( shakeWorld, invBaseWorld, deltaWorld );

		Quaternion deltaQuat;
		Vector deltaPos;
		MatrixAngles( deltaWorld, deltaQuat, deltaPos );
		deltaPos *= flShakeWeight;
		Quaternion identityQuat;
		identityQuat.Init( 0, 0, 0, 1 );
		Quaternion weightedQuat;
		SafeQuaternionSlerp( identityQuat, deltaQuat, flShakeWeight, weightedQuat );
		QuaternionMatrix( weightedQuat, deltaPos, weightedDeltaWorld );

		ConcatTransforms( weightedDeltaWorld, pBoneToWorldOut[i], resultWorld );
		MatrixCopy( resultWorld, pBoneToWorldOut[i] );
	}
}

bool C_TFVRHand::GetSampledBoneLocalTransform( const char *pszBoneName, int iSequence, float flCycle, matrix3x4_t &outLocalTransform )
{
	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr || !pszBoneName || iSequence < 0 )
		return false;

	int boneIndex = LookupBone( pszBoneName );
	int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	if ( boneIndex < 0 || boneIndex >= numBones )
		return false;

	float poseParameters[MAXSTUDIOPOSEPARAM];
	memset( poseParameters, 0, sizeof( poseParameters ) );

	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters );
	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, iSequence, flCycle, 1.0f, gpGlobals->curtime, NULL );
	QuaternionMatrix( qAnim[boneIndex], posAnim[boneIndex], outLocalTransform );
	return true;
}

bool C_TFVRHand::GetSampledBoneModelSpaceDelta( const char *pszBoneName, int iSequence, float flBaseCycle, float flCurrentCycle, Vector &outLocalDelta )
{
	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr || !pszBoneName || iSequence < 0 )
		return false;

	int boneIndex = LookupBone( pszBoneName );
	int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	if ( boneIndex < 0 || boneIndex >= numBones )
		return false;

	int referenceBone = LookupBone( "weapon_bone" );
	if ( referenceBone < 0 )
		referenceBone = LookupBone( "weapon_bone_R" );
	if ( referenceBone < 0 )
		referenceBone = LookupBone( "weapon_bone_L" );
	if ( referenceBone < 0 )
		referenceBone = LookupBone( "weapon_bone_l" );
	if ( referenceBone < 0 || referenceBone >= numBones || referenceBone == boneIndex )
		referenceBone = -1;

	float poseParameters[MAXSTUDIOPOSEPARAM];
	memset( poseParameters, 0, sizeof( poseParameters ) );

	Vector posBase[MAXSTUDIOBONES];
	Quaternion qBase[MAXSTUDIOBONES];
	Vector posCurrent[MAXSTUDIOBONES];
	Quaternion qCurrent[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posBase[i].Init();
		qBase[i].Init( 0, 0, 0, 1 );
		posCurrent[i].Init();
		qCurrent[i].Init( 0, 0, 0, 1 );
	}

	IBoneSetup baseSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters );
	baseSetup.InitPose( posBase, qBase );
	baseSetup.AccumulatePose( posBase, qBase, iSequence, flBaseCycle, 1.0f, gpGlobals->curtime, NULL );

	IBoneSetup currentSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters );
	currentSetup.InitPose( posCurrent, qCurrent );
	currentSetup.AccumulatePose( posCurrent, qCurrent, iSequence, flCurrentCycle, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t baseModelBones[MAXSTUDIOBONES];
	matrix3x4_t currentModelBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones; i++ )
	{
		matrix3x4_t baseLocal;
		matrix3x4_t currentLocal;
		QuaternionMatrix( qBase[i], posBase[i], baseLocal );
		QuaternionMatrix( qCurrent[i], posCurrent[i], currentLocal );

		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( baseModelBones[i] );
			SetIdentityMatrix( currentModelBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
		{
			MatrixCopy( baseLocal, baseModelBones[i] );
			MatrixCopy( currentLocal, currentModelBones[i] );
		}
		else if ( pBone->parent >= 0 && pBone->parent < i )
		{
			ConcatTransforms( baseModelBones[pBone->parent], baseLocal, baseModelBones[i] );
			ConcatTransforms( currentModelBones[pBone->parent], currentLocal, currentModelBones[i] );
		}
		else
		{
			SetIdentityMatrix( baseModelBones[i] );
			SetIdentityMatrix( currentModelBones[i] );
		}
	}

	matrix3x4_t baseBoneSpace;
	matrix3x4_t currentBoneSpace;
	if ( referenceBone >= 0 )
	{
		// Measure the slide relative to the weapon body, not the hand
		// model root. Otherwise whole-weapon pose motion gets applied twice.
		matrix3x4_t invBaseReference;
		matrix3x4_t invCurrentReference;
		MatrixInvert( baseModelBones[referenceBone], invBaseReference );
		MatrixInvert( currentModelBones[referenceBone], invCurrentReference );
		ConcatTransforms( invBaseReference, baseModelBones[boneIndex], baseBoneSpace );
		ConcatTransforms( invCurrentReference, currentModelBones[boneIndex], currentBoneSpace );
	}
	else
	{
		MatrixCopy( baseModelBones[boneIndex], baseBoneSpace );
		MatrixCopy( currentModelBones[boneIndex], currentBoneSpace );
	}

	matrix3x4_t invBaseModel;
	MatrixInvert( baseBoneSpace, invBaseModel );

	matrix3x4_t deltaInBaseBoneSpace;
	ConcatTransforms( invBaseModel, currentBoneSpace, deltaInBaseBoneSpace );
	MatrixGetColumn( deltaInBaseBoneSpace, 3, outLocalDelta );
	return true;
}

bool C_TFVRHand::GetPomsonAdjustedLeftControllerTransform( matrix3x4_t &outLeftControllerTransform ) const
{
	C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
	if ( !pLeftHand )
		return false;

	AngleMatrix( pLeftHand->m_angLastValidAngles, pLeftHand->m_vecLastValidPosition, outLeftControllerTransform );

	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	int ownerClass = pOwner ? pOwner->GetPlayerClass()->GetClassIndex() : TF_CLASS_UNDEFINED;
	QAngle leftRotOffset( 0, 0, 0 );
	if ( !GetPerClassHandOffset( ownerClass, true, leftRotOffset ) )
	{
		leftRotOffset.x = tfvr_hands_left_offset_pitch.GetFloat();
		leftRotOffset.y = tfvr_hands_left_offset_yaw.GetFloat();
		leftRotOffset.z = tfvr_hands_left_offset_roll.GetFloat();
	}
	if ( leftRotOffset.x != 0 || leftRotOffset.y != 0 || leftRotOffset.z != 0 )
	{
		matrix3x4_t offsetMatrix;
		AngleMatrix( leftRotOffset, vec3_origin, offsetMatrix );
		matrix3x4_t temp;
		ConcatTransforms( outLeftControllerTransform, offsetMatrix, temp );
		MatrixCopy( temp, outLeftControllerTransform );
	}

	Vector leftPosOffset( 0, 0, 0 );
	if ( !GetPerClassHandPositionOffset( ownerClass, true, leftPosOffset ) )
	{
		leftPosOffset.x = tfvr_hands_left_offset_x.GetFloat();
		leftPosOffset.y = tfvr_hands_left_offset_y.GetFloat();
		leftPosOffset.z = tfvr_hands_left_offset_z.GetFloat();
	}
	if ( leftPosOffset.x != 0 || leftPosOffset.y != 0 || leftPosOffset.z != 0 )
	{
		Vector px, py, pz;
		MatrixGetColumn( outLeftControllerTransform, 0, px );
		MatrixGetColumn( outLeftControllerTransform, 1, py );
		MatrixGetColumn( outLeftControllerTransform, 2, pz );
		Vector worldOffset = px * leftPosOffset.x + py * leftPosOffset.y + pz * leftPosOffset.z;
		Vector leftPos;
		MatrixGetColumn( outLeftControllerTransform, 3, leftPos );
		leftPos += worldOffset;
		MatrixSetColumn( leftPos, 3, outLeftControllerTransform );
	}

	return true;
}

bool C_TFVRHand::CapturePomsonDetachLeftToWeaponBone()
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !IsRightHand() || !pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_DRG_POMSON )
	{
		m_bPomsonDetachLeftToWeaponBoneValid = false;
		m_bPomsonDetachLeftToLeftHandBoneValid = false;
		return false;
	}

	matrix3x4_t leftControllerTransform;
	matrix3x4_t weaponBoneWorld;
	if ( !GetPomsonAdjustedLeftControllerTransform( leftControllerTransform )
		|| !GetCachedWeaponBoneTransform( weaponBoneWorld ) )
	{
		m_bPomsonDetachLeftToWeaponBoneValid = false;
		m_bPomsonDetachLeftToLeftHandBoneValid = false;
		return false;
	}

	matrix3x4_t invLeftController;
	MatrixInvert( leftControllerTransform, invLeftController );
	ConcatTransforms( invLeftController, weaponBoneWorld, m_matPomsonDetachLeftToWeaponBone );
	m_bPomsonDetachLeftToWeaponBoneValid = true;

	Vector leftGripPos;
	QAngle leftGripAngles;
	if ( GetOffHandGripTarget( leftGripPos, leftGripAngles, true ) )
	{
		matrix3x4_t leftHandWorld;
		AngleMatrix( leftGripAngles, leftGripPos, leftHandWorld );
		ConcatTransforms( invLeftController, leftHandWorld, m_matPomsonDetachLeftToLeftHandBone );
		m_bPomsonDetachLeftToLeftHandBoneValid = true;
	}
	else
	{
		m_bPomsonDetachLeftToLeftHandBoneValid = false;
	}

	return true;
}

bool C_TFVRHand::GetPomsonDetachedLeftHandWorld( matrix3x4_t &outLeftHandWorld )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !IsRightHand() || !m_bRightHandDetached || !pWeapon
		|| pWeapon->GetWeaponID() != TF_WEAPON_DRG_POMSON
		|| !m_bPomsonDetachLeftToLeftHandBoneValid )
	{
		return false;
	}

	matrix3x4_t leftControllerTransform;
	if ( !GetPomsonAdjustedLeftControllerTransform( leftControllerTransform ) )
		return false;

	ConcatTransforms( leftControllerTransform, m_matPomsonDetachLeftToLeftHandBone, outLeftHandWorld );
	return true;
}

bool C_TFVRHand::GetPomsonDetachedWeaponBoneWorld( matrix3x4_t &outWeaponBoneWorld )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !IsRightHand() || !m_bRightHandDetached || !pWeapon
		|| pWeapon->GetWeaponID() != TF_WEAPON_DRG_POMSON )
	{
		return false;
	}

	matrix3x4_t leftControllerTransform;
	if ( !GetPomsonAdjustedLeftControllerTransform( leftControllerTransform ) )
		return false;

	if ( m_bPomsonDetachLeftToWeaponBoneValid )
	{
		ConcatTransforms( leftControllerTransform, m_matPomsonDetachLeftToWeaponBone, outWeaponBoneWorld );
		return true;
	}

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr || m_iIdleSequence < 0 )
		return false;

	int leftGripBone = LookupBone( "bip_hand_L" );
	if ( leftGripBone < 0 )
		leftGripBone = LookupBone( "ValveBiped.Bip01_L_Hand" );
	if ( leftGripBone < 0 )
		leftGripBone = LookupBone( "bip_hand_l" );
	if ( leftGripBone < 0 )
		leftGripBone = LookupBone( "weapon_bone_L" );

	int weaponBone = LookupBone( "weapon_bone" );
	if ( weaponBone < 0 )
		weaponBone = LookupBone( "weapon_bone_R" );
	if ( weaponBone < 0 )
		weaponBone = LookupBone( "weapon_bone_L" );
	if ( weaponBone < 0 )
		weaponBone = LookupBone( "weapon_bone_l" );

	int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	if ( leftGripBone < 0 || leftGripBone >= numBones
		|| weaponBone < 0 || weaponBone >= numBones )
	{
		return false;
	}

	float poseParameters[MAXSTUDIOPOSEPARAM];
	memset( poseParameters, 0, sizeof( poseParameters ) );

	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters );
	Vector posIdle[MAXSTUDIOBONES];
	Quaternion qIdle[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posIdle[i].Init();
		qIdle[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posIdle, qIdle );
	boneSetup.AccumulatePose( posIdle, qIdle, m_iIdleSequence, 0.0f, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t idleBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones; i++ )
	{
		matrix3x4_t boneToParent;
		QuaternionMatrix( qIdle[i], posIdle[i], boneToParent );

		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( idleBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( boneToParent, idleBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones )
			ConcatTransforms( idleBones[pBone->parent], boneToParent, idleBones[i] );
		else
			SetIdentityMatrix( idleBones[i] );
	}

	matrix3x4_t invIdleLeftGrip;
	MatrixInvert( idleBones[leftGripBone], invIdleLeftGrip );

	matrix3x4_t detachedWeaponAnchor;
	ConcatTransforms( leftControllerTransform, invIdleLeftGrip, detachedWeaponAnchor );
	ConcatTransforms( detachedWeaponAnchor, idleBones[weaponBone], outWeaponBoneWorld );
	return true;
}

bool C_TFVRHand::GetPomsonDetachedRightHandTarget( Vector &outPos, QAngle &outAngles, bool bUseCurrentAnimation )
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if ( !IsRightHand() || !m_bRightHandDetached || !pWeapon
		|| pWeapon->GetWeaponID() != TF_WEAPON_DRG_POMSON )
	{
		return false;
	}

	CStudioHdr *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr || m_iIdleSequence < 0 )
		return false;

	int rightHandBone = m_iHandBone;
	if ( rightHandBone < 0 )
		rightHandBone = LookupBone( "bip_hand_R" );
	if ( rightHandBone < 0 )
		rightHandBone = LookupBone( "ValveBiped.Bip01_R_Hand" );
	if ( rightHandBone < 0 )
		rightHandBone = LookupBone( "bip_hand_r" );

	int weaponBone = LookupBone( "weapon_bone" );
	if ( weaponBone < 0 )
		weaponBone = LookupBone( "weapon_bone_R" );
	if ( weaponBone < 0 )
		weaponBone = LookupBone( "weapon_bone_L" );
	if ( weaponBone < 0 )
		weaponBone = LookupBone( "weapon_bone_l" );

	int numBones = MIN( pStudioHdr->numbones(), MAXSTUDIOBONES );
	if ( rightHandBone < 0 || rightHandBone >= numBones
		|| weaponBone < 0 || weaponBone >= numBones )
	{
		return false;
	}

	matrix3x4_t detachedWeaponBoneWorld;
	if ( !GetPomsonDetachedWeaponBoneWorld( detachedWeaponBoneWorld ) )
		return false;

	int seqToSample = m_iIdleSequence;
	float cycleToSample = 0.0f;
	if ( m_bPomsonUseReloadGrip && m_iReloadLoopSequence >= 0 )
	{
		seqToSample = m_iReloadLoopSequence;
		if ( bUseCurrentAnimation && m_iLeverReloadSequence >= 0 )
		{
			seqToSample = m_iLeverReloadSequence;
			cycleToSample = m_flLeverReloadCycle;
		}
	}

	float poseParameters[MAXSTUDIOPOSEPARAM];
	memset( poseParameters, 0, sizeof( poseParameters ) );

	IBoneSetup boneSetup( pStudioHdr, BONE_USED_BY_ANYTHING, poseParameters );

	Vector posAnim[MAXSTUDIOBONES];
	Quaternion qAnim[MAXSTUDIOBONES];
	for ( int i = 0; i < MAXSTUDIOBONES; i++ )
	{
		posAnim[i].Init();
		qAnim[i].Init( 0, 0, 0, 1 );
	}

	boneSetup.InitPose( posAnim, qAnim );
	boneSetup.AccumulatePose( posAnim, qAnim, seqToSample, cycleToSample, 1.0f, gpGlobals->curtime, NULL );

	matrix3x4_t sampledBones[MAXSTUDIOBONES];
	for ( int i = 0; i < numBones; i++ )
	{
		matrix3x4_t boneToParent;
		QuaternionMatrix( qAnim[i], posAnim[i], boneToParent );

		const mstudiobone_t *pBone = pStudioHdr->pBone( i );
		if ( !pBone )
		{
			SetIdentityMatrix( sampledBones[i] );
			continue;
		}

		if ( pBone->parent == -1 )
			MatrixCopy( boneToParent, sampledBones[i] );
		else if ( pBone->parent >= 0 && pBone->parent < numBones )
			ConcatTransforms( sampledBones[pBone->parent], boneToParent, sampledBones[i] );
		else
			SetIdentityMatrix( sampledBones[i] );
	}

	matrix3x4_t invSampledWeaponBone;
	MatrixInvert( sampledBones[weaponBone], invSampledWeaponBone );

	matrix3x4_t rightHandToWeapon;
	ConcatTransforms( invSampledWeaponBone, sampledBones[rightHandBone], rightHandToWeapon );

	matrix3x4_t rightHandWorld;
	ConcatTransforms( detachedWeaponBoneWorld, rightHandToWeapon, rightHandWorld );
	MatrixAngles( rightHandWorld, outAngles, outPos );
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

	m_bLiveBowStringBoneWorldValid = false;

	// Always cache the hand's weapon_bone transform for overlays (HUD compositor etc.),
	// even when there is no render weapon (e.g. bare fists).
	int handWeaponBone = -1;
	if (m_bPoseAsLeftHand && IsWeaponMedigun(m_hHeldWeapon.Get()))
	{
		handWeaponBone = LookupBone("weapon_bone_L");
	}
	if (handWeaponBone < 0)
	{
		handWeaponBone = LookupBone("weapon_bone");
	}

	// Bread Bite: the weapon_bone in the bread glove model is misplaced,
	// so anchor the render weapon to the hand bone instead.
	int handAlignBone = handWeaponBone;
	if (m_bIsBreadBite && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
	{
		handAlignBone = m_iHandBone;
	}

	if (handAlignBone >= 0 && handAlignBone < nMaxBones)
	{
		// Idle-stabilized cache: HUD overlays and aim helpers read this, so
		// it must NOT follow reload/fire animation motion.
		if (m_bHasIdleWeaponBone && !m_bIsBreadBite)
			MatrixCopy(m_matIdleWeaponBoneWorld, m_matWeaponBoneWorld);
		else
			MatrixCopy(pBoneToWorldOut[handAlignBone], m_matWeaponBoneWorld);
		m_bWeaponBoneWorldValid = true;

		// Live (animated) weapon bone: pistol manual reload targets read
		// this so the magwell follows the gun as the reload anim moves it.
		MatrixCopy(pBoneToWorldOut[handAlignBone], m_matLiveWeaponBoneWorld);
		m_bLiveWeaponBoneWorldValid = true;

		if (m_hHeldWeapon.Get() && m_hHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		{
			int iBowStringBone = LookupBone("weapon_bone_3");
			if (iBowStringBone >= 0 && iBowStringBone < nMaxBones)
			{
				MatrixCopy(pBoneToWorldOut[iBowStringBone], m_matLiveBowStringBoneWorld);
				m_bLiveBowStringBoneWorldValid = true;
			}
		}
	}

	// Pomson right-hand detach: position weapon at left hand instead of right hand
	if (m_bRightHandDetached && IsRightHand())
	{
		C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
		if (pRenderWeapon)
		{
			matrix3x4_t detachedWeaponBoneWorld;
			if (!GetPomsonDetachedWeaponBoneWorld(detachedWeaponBoneWorld))
			{
				return;
			}

			MatrixCopy(detachedWeaponBoneWorld, m_matWeaponBoneWorld);
			m_bWeaponBoneWorldValid = true;

			int rwWeaponBone = pRenderWeapon->LookupBone("weapon_bone");
			if (rwWeaponBone < 0)
				rwWeaponBone = pRenderWeapon->LookupBone("vm_weapon_bone");
			if (rwWeaponBone < 0)
				rwWeaponBone = pRenderWeapon->LookupBone("weapon_bone_R");
			if (rwWeaponBone < 0)
				rwWeaponBone = pRenderWeapon->LookupBone("weapon_bone_L");
			if (rwWeaponBone >= 0)
			{
				CStudioHdr *pWeaponHdr = pRenderWeapon->GetModelPtr();
				if (pWeaponHdr)
				{
					matrix3x4_t weaponBoneModelSpace;
					SetIdentityMatrix(weaponBoneModelSpace);

					bool bGotWeaponBoneModelSpace = false;
					int weaponBoneCount = MIN(pWeaponHdr->numbones(), MAXSTUDIOBONES);
					int renderSeq = pRenderWeapon->GetSequence();
					if (renderSeq >= 0 && rwWeaponBone < weaponBoneCount)
					{
						float poseParameters[MAXSTUDIOPOSEPARAM];
						memset(poseParameters, 0, sizeof(poseParameters));

						IBoneSetup weaponBoneSetup(pWeaponHdr, BONE_USED_BY_ANYTHING, poseParameters);
						Vector weaponPosAnim[MAXSTUDIOBONES];
						Quaternion weaponQAnim[MAXSTUDIOBONES];
						for (int i = 0; i < MAXSTUDIOBONES; i++)
						{
							weaponPosAnim[i].Init();
							weaponQAnim[i].Init(0, 0, 0, 1);
						}

						weaponBoneSetup.InitPose(weaponPosAnim, weaponQAnim);
						weaponBoneSetup.AccumulatePose(weaponPosAnim, weaponQAnim, renderSeq, pRenderWeapon->GetCycle(),
							1.0f, gpGlobals->curtime, NULL);

						matrix3x4_t weaponModelBones[MAXSTUDIOBONES];
						for (int i = 0; i < weaponBoneCount && i < MAXSTUDIOBONES; i++)
						{
							matrix3x4_t boneToParent;
							QuaternionMatrix(weaponQAnim[i], weaponPosAnim[i], boneToParent);

							const mstudiobone_t *pBone = pWeaponHdr->pBone(i);
							if (!pBone)
							{
								SetIdentityMatrix(weaponModelBones[i]);
								continue;
							}

							if (pBone->parent == -1)
								MatrixCopy(boneToParent, weaponModelBones[i]);
							else if (pBone->parent >= 0 && pBone->parent < i)
								ConcatTransforms(weaponModelBones[pBone->parent], boneToParent, weaponModelBones[i]);
							else
								SetIdentityMatrix(weaponModelBones[i]);
						}

						MatrixCopy(weaponModelBones[rwWeaponBone], weaponBoneModelSpace);
						bGotWeaponBoneModelSpace = true;
					}

					if (!bGotWeaponBoneModelSpace)
					{
						int currentBone = rwWeaponBone;
						while (currentBone >= 0)
						{
							mstudiobone_t *pBone = pWeaponHdr->pBone(currentBone);
							if (!pBone) break;
							matrix3x4_t localBoneMatrix;
							QAngle localAng;
							QuaternionAngles(pBone->quat, localAng);
							AngleMatrix(localAng, pBone->pos, localBoneMatrix);
							matrix3x4_t temp;
							ConcatTransforms(localBoneMatrix, weaponBoneModelSpace, temp);
							MatrixCopy(temp, weaponBoneModelSpace);
							currentBone = pBone->parent;
						}
					}

					matrix3x4_t weaponBoneInverse;
					MatrixInvert(weaponBoneModelSpace, weaponBoneInverse);
					matrix3x4_t weaponTransform;
					ConcatTransforms(detachedWeaponBoneWorld, weaponBoneInverse, weaponTransform);
					Vector wPos; QAngle wAng;
					MatrixAngles(weaponTransform, wAng, wPos);
					pRenderWeapon->SetAbsOrigin(wPos);
					pRenderWeapon->SetAbsAngles(wAng);
					pRenderWeapon->SetNetworkOrigin(wPos);
					pRenderWeapon->ResetLatched();
					pRenderWeapon->InvalidateBoneCache();
					pRenderWeapon->SetupBones(NULL, -1, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
				}
			}

			// Cache muzzle position for effects
			int muzzleAttach = pRenderWeapon->LookupAttachment("muzzle");
			if (muzzleAttach > 0)
			{
				pRenderWeapon->GetAttachment(muzzleAttach, m_vecCachedMuzzlePos, m_angCachedMuzzleAngles);
				m_bCachedMuzzleValid = true;
			}

			return;
		}
	}

	// Position the RENDER weapon based on hand's alignment bone
	C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
	if (!pRenderWeapon)
		return;

	Vector weaponPos;
	QAngle weaponAng;

	C_TFWeaponBase *pAlignWeapon = m_hHeldWeapon.Get();
	bool bIsFistWeapon = pAlignWeapon && pAlignWeapon->GetWeaponID() == TF_WEAPON_FISTS;
	int weaponWeaponBone = -1;

	if (handAlignBone >= 0 && handAlignBone < nMaxBones)
	{
		matrix3x4_t handWeaponBoneMatrix;
		MatrixCopy(pBoneToWorldOut[handAlignBone], handWeaponBoneMatrix);

		// Extract position and angles
		Vector bonePos;
		QAngle boneAng;
		MatrixAngles(handWeaponBoneMatrix, boneAng, bonePos);

		// Determine which bone on the weapon model to align to the hand's weapon_bone.
		if (m_bIsBreadBite)
		{
			weaponWeaponBone = pRenderWeapon->LookupBone("bip_hand_R");
		}
		else if (m_bPoseAsLeftHand && IsWeaponMedigun(pAlignWeapon))
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
	TFVR_SetReloadBodygroup(pRenderWeapon, TFVR_IsManualRocketReloadActive(pMergeWeapon));
	bool bSkipBoneMerge = pMergeWeapon && pMergeWeapon->GetWeaponID() == TF_WEAPON_FISTS;
	bool bShotgunManualShell = false;
	if (pMergeWeapon && IsPumpActionShotgunWeaponID(pMergeWeapon->GetWeaponID()))
	{
		CTFShotgun *pShotgun = static_cast<CTFShotgun *>(pMergeWeapon);
		bShotgunManualShell = pShotgun->IsVRShotgunManualReloadActive();
	}

	// For pistol/shotgun-type weapons, keep vm_weapon bones in the weapon
	// model's own idle pose so ammo/shells don't float visibly without a
	// covering hand mesh (placeholder until manual reloading is added).
	bool bKeepAmmoRefPose = false;
	if (pMergeWeapon)
	{
		const char *weaponClass = pMergeWeapon->GetClassname();
		if (V_stristr(weaponClass, "pistol") ||
			V_stristr(weaponClass, "shotgun") ||
			V_stristr(weaponClass, "scattergun") ||
			V_stristr(weaponClass, "sentry_revenge") ||
			V_stristr(weaponClass, "handgun_scout"))
		{
			bKeepAmmoRefPose = true;
		}
	}
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
		bool bCopyFromShellHand[MAXSTUDIOBONES];
		memset(bCopyFromShellHand, 0, sizeof(bCopyFromShellHand));
		matrix3x4_t originalWeaponBones[MAXSTUDIOBONES];
		matrix3x4_t shellHandBones[MAXSTUDIOBONES];
		C_TFVRHand *pShellHand = NULL;
		bool bShellHandValid = false;
		if (bShotgunManualShell)
		{
			pShellHand = IsRightHand() ? GetLocalPlayerLeftHand() : GetLocalPlayerRightHand();
			if (pShellHand)
				bShellHandValid = pShellHand->SetupBones(shellHandBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);
		}

		for (int i = 0; i < weaponBoneCount && i < MAXSTUDIOBONES; i++)
		{
			const mstudiobone_t *pWpnBone = pWeaponHdr->pBone(i);
			if (!pWpnBone)
				continue;

			const char *szBoneName = pWpnBone->pszName();

			// Skip weapon_bone and weapon_bone_L/R for non-bread weapons.
			// For bread creature weapons (m_bAnimateIdle) we INCLUDE weapon_bone
			// so that vertices weighted to it stay in sync with the hand.
			// Without this, the render weapon's own animation drives weapon_bone
			// independently, causing the sapper frame to desync from the creature.
			if (!m_bAnimateIdle &&
				(Q_strcmp(szBoneName, "weapon_bone") == 0 ||
				 Q_strcmp(szBoneName, "weapon_bone_L") == 0 ||
				 Q_strcmp(szBoneName, "weapon_bone_R") == 0))
				continue;

			// Skip vm_weapon bones when the weapon model's own animation should
			// drive them: ammo ref pose, or render weapon playing its draw/fire
			// while the hand is NOT also playing the draw (hand paused at idle).
			// When the hand IS playing the draw animation, merge from the hand
			// so the hand drives both visual and audio for the deploy.
			if (Q_strncmp(szBoneName, "vm_weapon", 9) == 0)
			{
				if (bShotgunManualShell && bShellHandValid && pShellHand
					&& Q_strcmp(szBoneName, "vm_weapon_bone") == 0)
				{
					int shellHandBone = pMergeWeapon && pMergeWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_HWG
						? pShellHand->LookupBone("vm_weapon_bone_2")
						: pShellHand->LookupBone(szBoneName);
					if (shellHandBone < 0 && pMergeWeapon && pMergeWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_HWG)
						shellHandBone = pShellHand->LookupBone(szBoneName);
					if (shellHandBone >= 0 && shellHandBone < MAXSTUDIOBONES)
					{
						MatrixCopy(pRenderWeapon->GetBoneForWrite(i), originalWeaponBones[i]);
						bCopiedBone[i] = true;
						bCopyFromShellHand[i] = true;
						continue;
					}
				}
				if ((m_bAnimateIdle && !m_bLoopIdleOnHand) || bKeepAmmoRefPose)
					continue;
				if (m_bLoopIdleOnHand)
				{
				C_VRRenderWeapon *pRW = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
				bool bHandDrivingDraw = m_bPlayingDrawAnim
					&& (m_eDrawAnimScope >= VR_DRAW_ANIM_WRIST || m_bAnimateIdle);
				if (pRW && pRW->IsPlayingDrawOrFire() && !bHandDrivingDraw)
						continue;
				}
			}

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
			if (bCopyFromShellHand[i] && pShellHand)
			{
				int shellHandBone = pMergeWeapon && pMergeWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_HWG
					? pShellHand->LookupBone("vm_weapon_bone_2")
					: pShellHand->LookupBone(szBoneName);
				if (shellHandBone < 0 && pMergeWeapon && pMergeWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_HWG)
					shellHandBone = pShellHand->LookupBone(szBoneName);
				if (shellHandBone >= 0 && shellHandBone < MAXSTUDIOBONES)
					MatrixCopy(shellHandBones[shellHandBone], weaponBone);
			}
			else
			{
				MatrixCopy(pBoneToWorldOut[handBoneIndex], weaponBone);
			}
			copiedCount++;
		}

		// Second pass: rebuild children of copied/adjusted bones that weren't
		// themselves directly copied. Cascades down the hierarchy so
		// grandchildren of merged bones also get repositioned correctly.
		// Source guarantees bones are ordered parent-first, so iterating
		// in index order handles the cascade naturally.
		for (int i = 0; i < weaponBoneCount && i < MAXSTUDIOBONES; i++)
		{
			if (bCopiedBone[i])
				continue;

			const mstudiobone_t *pWpnBone = pWeaponHdr->pBone(i);
			if (!pWpnBone || pWpnBone->parent < 0)
				continue;

			const char *szChildBoneName = pWpnBone->pszName();
			if (bShotgunManualShell && Q_strncmp(szChildBoneName, "vm_weapon", 9) == 0)
				continue;

			if (!bCopiedBone[pWpnBone->parent])
				continue;

			// Save original before overwriting so deeper descendants can use it
			MatrixCopy(pRenderWeapon->GetBoneForWrite(i), originalWeaponBones[i]);

			// local = inv(original_parent) * original_child
			matrix3x4_t invOriginalParent;
			MatrixInvert(originalWeaponBones[pWpnBone->parent], invOriginalParent);

			matrix3x4_t localTransform;
			matrix3x4_t &childBone = pRenderWeapon->GetBoneForWrite(i);
			ConcatTransforms(invOriginalParent, childBone, localTransform);

			// new_child = new_parent * local
			matrix3x4_t &newParentBone = pRenderWeapon->GetBoneForWrite(pWpnBone->parent);
			ConcatTransforms(newParentBone, localTransform, childBone);

			bCopiedBone[i] = true;
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
// Purpose: Live weapon bone transform that follows reload animation motion
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetLiveWeaponBoneTransform(matrix3x4_t &outTransform) const
{
	if (!m_bLiveWeaponBoneWorldValid)
		return false;

	MatrixCopy(m_matLiveWeaponBoneWorld, outTransform);
	return true;
}

static bool IsAllClassMelee(C_TFWeaponBase *pWeapon);

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
		// Use THIS hand's physical controller pose (the muzzle method is called
		// on the weapon hand entity, which may be the left controller).
		const bool bHandPoseValid = IsLeftHand()
			? (g_pOpenXRManager && g_pOpenXRManager->IsLeftControllerPoseValid())
			: (g_pOpenXRManager && g_pOpenXRManager->IsRightControllerPoseValid());
		if (bHandPoseValid)
		{
			VMatrix controllerPose;
			const bool bGotControllerPose = IsLeftHand()
				? g_pOpenXRManager->GetLeftControllerPose(controllerPose)
				: g_pOpenXRManager->GetRightControllerPose(controllerPose);
			if (bGotControllerPose)
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

	// LEFT-HANDED MIRROR: the visible weapon is drawn with the hand mirror
	// reflection applied, so fire must sample that same reflected muzzle to
	// match where the gun visually points. The cached muzzle was captured
	// canonically (before the reflection), so apply the same reflection here.
	// The reflected matrix is improper (negative determinant), so we cannot use
	// MatrixAngles on it; instead we reconstruct a proper orientation from the
	// reflected forward/up direction vectors (valid regardless of determinant).
	if (ShouldMirrorPose() && m_bCachedMuzzleValid)
	{
		matrix3x4_t muzzleWorld;
		AngleMatrix(m_angCachedMuzzleAngles, m_vecCachedMuzzlePos, muzzleWorld);

		TFVR_ReflectBonesInControllerFrame(&muzzleWorld, 1, m_matReflectFrame);

		MatrixGetColumn(muzzleWorld, 3, outPos);

		Vector vecFwd, vecUp;
		MatrixGetColumn(muzzleWorld, 0, vecFwd);
		MatrixGetColumn(muzzleWorld, 2, vecUp);
		if (vecFwd.LengthSqr() < 1e-6f)
			vecFwd.Init(1.0f, 0.0f, 0.0f);
		VectorAngles(vecFwd, vecUp, outAngles);
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
			else if ( weaponType == TF_WPN_TYPE_MELEE && !IsAllClassMelee(pTFWeapon) )
			{
				C_TFPlayer *pOwnerPlayer = m_hOwnerPlayer.Get();
				if ( pOwnerPlayer )
				{
					int iClass = pOwnerPlayer->GetPlayerClass()->GetClassIndex();
					if ( iClass == TF_CLASS_MEDIC || iClass == TF_CLASS_SNIPER )
					{
						// Solemn Vow (bust statue) aligns along Y like all-class melees
						CEconItemView *pItem = pTFWeapon->GetAttributeContainer()->GetItem();
						bool bSolemnVow = pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 413;
						if ( !bSolemnVow )
							iAxis = 2;
					}
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

			// Cache idle muzzle when NOT playing any animation
			if (!m_bIdleMuzzleOffsetValid && !m_bPlayingFireAnim && !m_bPlayingDrawAnim)
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

	// CRUSADER'S CROSSBOW: The workshop model has no muzzle attachment, so we
	// synthesize one from the weapon_bone with a configurable offset and rotation.
	if (pTFWeapon && pTFWeapon->GetWeaponID() == TF_WEAPON_CROSSBOW)
	{
		matrix3x4_t matBone;
		if (GetCachedWeaponBoneTransform(matBone))
		{
			Vector bonePos;
			QAngle boneAngles;
			MatrixAngles(matBone, boneAngles, bonePos);

			Vector forward, right, up;
			AngleVectors(boneAngles, &forward, &right, &up);

			outPos = bonePos
				+ forward * tfvr_crossbow_muzzle_fwd.GetFloat()
				+ right   * tfvr_crossbow_muzzle_right.GetFloat()
				+ up      * tfvr_crossbow_muzzle_up.GetFloat();

			QAngle rotationOffset(
				tfvr_crossbow_muzzle_pitch.GetFloat(),
				tfvr_crossbow_muzzle_yaw.GetFloat(),
				tfvr_crossbow_muzzle_roll.GetFloat());

			matrix3x4_t boneMat, rotMat, resultMat;
			AngleMatrix(boneAngles, vec3_origin, boneMat);
			AngleMatrix(rotationOffset, vec3_origin, rotMat);
			ConcatTransforms(boneMat, rotMat, resultMat);
			MatrixAngles(resultMat, outAngles);

			return true;
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

	// Fallback: Force bone setup and try multiple attachment names.
	// Workshop models (e.g. Crusader's Crossbow) may use non-standard names.
	InvalidateBoneCache();
	matrix3x4_t boneArray[MAXSTUDIOBONES];
	SetupBones(boneArray, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime);

	static const char *s_muzzleNames[] = { "muzzle", "muzzle_flash", "0", "1" };
	for (int i = 0; i < ARRAYSIZE(s_muzzleNames); i++)
	{
		int iAttach = pRenderWeapon->LookupAttachment(s_muzzleNames[i]);
		if (iAttach > 0 && pRenderWeapon->GetAttachment(iAttach, outPos, outAngles))
		{
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

	// Controller aim fallback: use the physical controller's aim pose.
	// This is the most natural fallback for ranged weapons without a
	// muzzle attachment — it matches where the player is pointing.
	if (g_pOpenXRManager)
	{
		VMatrix controllerPose;
		bool bGotPose = IsLeftHand()
			? g_pOpenXRManager->GetLeftControllerPose(controllerPose)
			: g_pOpenXRManager->GetRightControllerPose(controllerPose);
		if (bGotPose)
		{
			outPos = controllerPose.GetTranslation();
			MatrixAngles(controllerPose.As3x4(), outAngles);
			Vector forward;
			AngleVectors(outAngles, &forward, NULL, NULL);
			outPos += forward * 30.0f;
			return true;
		}
	}

	// Final fallback: use hand position with forward offset
	outPos = GetAbsOrigin();
	outAngles = GetAbsAngles();

	Vector forward;
	AngleVectors(outAngles, &forward, NULL, NULL);
	outPos += forward * 30.0f;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Check whether a tf_weapon_fists entity is actually a gloved
//          variant (Apoco-Fists, KGB, GRU, Fists of Steel, Holiday Punch,
//          Bread Bite, etc.) by inspecting the world model path.
//-----------------------------------------------------------------------------
static bool IsFistsGloveVariant(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
		return false;

	const char *worldModel = pWeapon->GetWorldModel();
	if (!worldModel)
		return false;

	return V_stristr(worldModel, "boxing_gloves") ||
		   V_stristr(worldModel, "gru") ||
		   V_stristr(worldModel, "fists_of_steel") ||
		   V_stristr(worldModel, "xms_gloves") ||
		   V_stristr(worldModel, "breadmonster") ||
		   V_stristr(worldModel, "sr3_punch");
}

//-----------------------------------------------------------------------------
// Purpose: Returns true for bare-handed fist variants (default fists and
//          reskins that use f_ animations rather than bg_ glove animations).
//          These show the Heavy's bare hands, making them candidates for
//          finger tracking instead of canned weapon-pose animations.
//          The Eviction Notice uses f_ animations but has visible wrapping
//          geometry that clashes with tracked fingers, so it is excluded.
//-----------------------------------------------------------------------------
static bool IsBareFists(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_FISTS)
		return false;

	if (IsFistsGloveVariant(pWeapon))
		return false;

	const char *worldModel = pWeapon->GetWorldModel();
	if (worldModel && V_stristr(worldModel, "eviction"))
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Check if a fist weapon is specifically the Bread Bite (c_breadmonster_gloves).
//-----------------------------------------------------------------------------
static bool IsBreadBite(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_FISTS)
		return false;
	const char *worldModel = pWeapon->GetWorldModel();
	return worldModel && V_stristr(worldModel, "breadmonster");
}

//-----------------------------------------------------------------------------
// Purpose: Returns 0, 1, or 2 for Bread Bite idle variant (A/B/C).
//          Randomly selected each deploy, consistent within the same frame.
//-----------------------------------------------------------------------------
static int GetBreadBiteIdleVariant()
{
	static int s_nVariant = 0;
	static int s_nLastFrame = -1;
	if (gpGlobals->framecount != s_nLastFrame)
	{
		s_nVariant = RandomInt(0, 2);
		s_nLastFrame = gpGlobals->framecount;
	}
	return s_nVariant;
}

//-----------------------------------------------------------------------------
// Purpose: Returns 0 or 1 for Bread Bite draw variant (A/B).
//          Randomly selected each deploy, consistent within the same frame.
//-----------------------------------------------------------------------------
static int GetBreadBiteDrawVariant()
{
	static int s_nVariant = 0;
	static int s_nLastFrame = -1;
	if (gpGlobals->framecount != s_nLastFrame)
	{
		s_nVariant = RandomInt(0, 1);
		s_nLastFrame = gpGlobals->framecount;
	}
	return s_nVariant;
}

//-----------------------------------------------------------------------------
// Purpose: Determine the correct idle animation for Heavy fist weapons.
//          Bread Bite randomly picks breadglove_idle_A / _B / _C per deploy.
//          Other gloved variants use bg_idle, bare-fist variants use f_idle.
//-----------------------------------------------------------------------------
static const char* GetFistsIdleAnimName(C_TFWeaponBase *pWeapon)
{
	if (IsBreadBite(pWeapon))
	{
		static const char *s_szBreadBiteIdles[] = { "breadglove_idle_A", "breadglove_idle_B", "breadglove_idle_C" };
		return s_szBreadBiteIdles[GetBreadBiteIdleVariant()];
	}
	return IsFistsGloveVariant(pWeapon) ? "bg_idle" : "f_idle";
}

//-----------------------------------------------------------------------------
// Purpose: Determine which spy knife animation prefix to use based on the
//          weapon's world model.  Returns "knife", "eternal", or "acr".
//            knife_*    -- standard butterfly knife (default + most reskins)
//            eternal_*  -- Your Eternal Reward / Wanga Prick
//            acr_*      -- Sharp Dresser (wrist blade)
//-----------------------------------------------------------------------------
const char* GetSpyKnifeAnimPrefix(C_TFWeaponBase *pWeapon)
{
	if (pWeapon)
	{
		const char *worldModel = pWeapon->GetWorldModel();
		if (worldModel)
		{
			// Standard butterfly knife and reskins that use knife_*
			if (V_stristr(worldModel, "c_knife") || V_stristr(worldModel, "roseknife"))
				return "knife";
			if (V_stristr(worldModel, "sharp_dresser"))
				return "acr";
		}

		CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
		if (pItem && pItem->IsValid())
		{
			int iDef = pItem->GetItemDefIndex();
			if (iDef == 4 || iDef == 194 || iDef == 727)  // Knife, Festive Knife, Black Rose
				return "knife";
			if (iDef == 638)                   // Sharp Dresser
				return "acr";
		}
	}
	// Default: most knife variants use eternal_* animations
	return "eternal";
}

//-----------------------------------------------------------------------------
// Purpose: Check if a weapon uses the MELEE_ALLCLASS anim slot (frying pan,
//          saxxy, conscientious objector, ham shank, etc.)
//-----------------------------------------------------------------------------
static bool IsAllClassMelee(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
		return false;

	// Breadmonster items (bread box reskins) have allclass melee anim slot
	// in the schema but are NOT melee weapons — they need class-specific handling.
	const char *worldModel = pWeapon->GetWorldModel();
	if (worldModel && V_stristr(worldModel, "breadmonster"))
		return false;

	CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
	if (pItem && pItem->IsValid())
	{
		CTFItemDefinition *pDef = pItem->GetStaticData();
		if (pDef && pDef->GetAnimSlot() == TF_WPN_TYPE_MELEE_ALLCLASS)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Get the appropriate hand animation name for a weapon
//-----------------------------------------------------------------------------
const char* GetWeaponPoseAnimation(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	// Default fallback
	const char *defaultAnim = "ref";

	// All-class melee weapons use melee_allclass_idle
	if (IsAllClassMelee(pWeapon))
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
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_JAR)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1105)
					return "bm_idle"; // Self-Aware Beauty Mark (bread monster)
				return "pj_idle"; // Jarate
			}
			if (V_stristr(weaponClass, "jar")) return "pj_idle"; // Jarate (fallback)
			if (V_stristr(weaponClass, "cleaver")) return "throw_idle"; // Throwing weapons
			if (V_stristr(weaponClass, "throwable")) return "throw_idle";
			if (V_stristr(weaponClass, "charged_smg")) return "idle"; // Cleaner's Carbine (uses generic idle)
			break;

		case TF_CLASS_SPY:
			// Spy: idle, knife_idle/eternal_idle/acr_idle, c_sapper_idle, offhand_idle, throw_idle
			if (V_stristr(weaponClass, "revolver")) return "idle";
			if (V_stristr(weaponClass, "knife"))
			{
				static char s_szSpyKnifeIdle[64];
				V_snprintf(s_szSpyKnifeIdle, sizeof(s_szSpyKnifeIdle), "%s_idle", GetSpyKnifeAnimPrefix(pWeapon));
				return s_szSpyKnifeIdle;
			}
			if (V_stristr(weaponClass, "sapper") || V_stristr(weaponClass, "builder"))
			{
				const char *worldModel = pWeapon ? pWeapon->GetWorldModel() : NULL;
				if (worldModel && V_stristr(worldModel, "breadmonster"))
					return "c_breadmonster_sapper_drawDeployed";
				return "c_sapper_idle";
			}
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

	if (IsAllClassMelee(pWeapon))
		return NULL;

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
				if (IsBreadBite(pWeapon))
					return NULL;
				outSwingCount = 3;
				return IsFistsGloveVariant(pWeapon) ? "bg_swing_" : "f_swing_";
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
				static char s_szSpySwingBase[64];
				V_snprintf(s_szSpySwingBase, sizeof(s_szSpySwingBase), "%s_stab_", GetSpyKnifeAnimPrefix(pWeapon));
				return s_szSpySwingBase;
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

	// All-class melee weapons use melee_allclass_swing
	if (IsAllClassMelee(pWeapon))
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
			if (V_stristr(weaponClass, "fists")) return IsFistsGloveVariant(pWeapon) ? "bg_swing_a" : "f_swing_a";
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
		if (V_stristr(weaponClass, "compound_bow")) return "bw_fire";
			if (V_stristr(weaponClass, "shotgun")) return "ss_fire";
			if (V_stristr(weaponClass, "jar")) return "throw_fire"; // Jarate
			if (V_stristr(weaponClass, "throwable")) return "throw_fire";
			break;

		case TF_CLASS_SPY:
			// Spy: fire (revolver), knife_stab_a/eternal_stab_a/acr_stab_a (knife)
			if (V_stristr(weaponClass, "revolver")) return "fire";
			if (V_stristr(weaponClass, "knife"))
			{
				static char s_szSpyKnifeFire[64];
				V_snprintf(s_szSpyKnifeFire, sizeof(s_szSpyKnifeFire), "%s_stab_a", GetSpyKnifeAnimPrefix(pWeapon));
				return s_szSpyKnifeFire;
			}
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
// Purpose: Get the draw animation name for a weapon (played when equipping)
//          Returns NULL if no draw animation should play for this weapon.
//          Animation names follow the same convention as fire/idle
//          (e.g. "sg_draw" for scattergun, "m_draw" for minigun).
//-----------------------------------------------------------------------------
const char* GetWeaponDrawAnimation(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	if (IsAllClassMelee(pWeapon))
		return "melee_allclass_draw";

	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			if (V_stristr(weaponClass, "soda_popper")) return "db_draw";
			if (V_stristr(weaponClass, "pep_brawler_blaster")) return "sg_draw";
			if (V_stristr(weaponClass, "scattergun") && pWeapon)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 45)
					return "db_draw";
			}
			if (V_stristr(weaponClass, "scattergun")) return "sg_draw";
			if (V_stristr(weaponClass, "handgun_scout")) return "ss_draw";
			if (V_stristr(weaponClass, "pistol")) return "p_draw";
			if (V_stristr(weaponClass, "bat")) return "b_draw";
			if (V_stristr(weaponClass, "lunchbox_drink")) return "ed_draw"; // Bonk, Crit-a-Cola
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_JAR_MILK)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1121)
					return "bm_draw"; // Mutated Milk (bread monster)
				return "ed_draw"; // Mad Milk
			}
			break;

		case TF_CLASS_SOLDIER:
			if (V_stristr(weaponClass, "rocketlauncher")) return "dh_draw";
			if (V_stristr(weaponClass, "particle_cannon")) return "dh_draw";
			if (V_stristr(weaponClass, "shotgun")) return "draw";
			if (V_stristr(weaponClass, "katana")) return "s_draw";
			if (V_stristr(weaponClass, "sword")) return "s_draw";
			if (V_stristr(weaponClass, "shovel")) return "s_draw";
			if (V_stristr(weaponClass, "pickaxe")) return "s_draw";
			break;

		case TF_CLASS_PYRO:
			if (V_stristr(weaponClass, "flamethrower")) return "ft_draw";
			if (V_stristr(weaponClass, "rocketlauncher_fireball")) return "ft_draw";
			if (V_stristr(weaponClass, "flaregun")) return "fg_draw";
			if (V_stristr(weaponClass, "shotgun")) return "draw";
			if (V_stristr(weaponClass, "fireaxe")) return "fa_draw";
			if (V_stristr(weaponClass, "slap")) return "fa_draw";
			break;

		case TF_CLASS_DEMOMAN:
			if (V_stristr(weaponClass, "grenadelauncher")) return "g_draw";
			if (V_stristr(weaponClass, "cannon")) return "g_draw";
			if (V_stristr(weaponClass, "pipebomblauncher")) return "sb_draw";
			if (V_stristr(weaponClass, "bottle")) return "b_draw";
			if (V_stristr(weaponClass, "sword")) return "cm_draw";
			if (V_stristr(weaponClass, "katana")) return "cm_draw";
			break;

		case TF_CLASS_HEAVYWEAPONS:
			if (V_stristr(weaponClass, "minigun")) return "m_draw";
			if (V_stristr(weaponClass, "shotgun")) return "draw";
			if (V_stristr(weaponClass, "fists"))
			{
				if (IsBreadBite(pWeapon))
					return GetBreadBiteDrawVariant() ? "breadglove_draw_B" : "breadglove_draw_A";
				return IsFistsGloveVariant(pWeapon) ? "bg_draw" : "f_draw";
			}
			break;

		case TF_CLASS_ENGINEER:
			if (V_stristr(weaponClass, "sentry_revenge")) return "fj_draw";
			if (V_stristr(weaponClass, "shotgun")) return "fj_draw";
			if (V_stristr(weaponClass, "pistol")) return "pstl_draw";
			if (V_stristr(weaponClass, "wrench")) return "pdq_draw";
			if (V_stristr(weaponClass, "robot_arm")) return "gun_draw";
			if (V_stristr(weaponClass, "drg_pomson")) return "pomson_draw";
			if (V_stristr(weaponClass, "raygun")) return "pomson_draw";
			break;

		case TF_CLASS_MEDIC:
			if (V_stristr(weaponClass, "syringegun")) return "sg_draw";
			if (V_stristr(weaponClass, "crossbow")) return "sg_draw";
			if (V_stristr(weaponClass, "medigun")) return "draw";
			if (V_stristr(weaponClass, "bonesaw")) return "bs_draw";
			break;

		case TF_CLASS_SNIPER:
			if (V_stristr(weaponClass, "sniperrifle")) return "draw";
			if (V_stristr(weaponClass, "compound_bow")) return "bw_draw";
			if (V_stristr(weaponClass, "smg")) return "smg_draw";
			if (V_stristr(weaponClass, "club")) return "m_draw";
			if (V_stristr(weaponClass, "sword")) return "m_draw";
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_JAR)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1105)
					return "bm_draw"; // Self-Aware Beauty Mark (bread monster)
			}
			break;

		case TF_CLASS_SPY:
			if (V_stristr(weaponClass, "revolver")) return "draw";
			if (V_stristr(weaponClass, "knife"))
			{
				static char s_szSpyKnifeDraw[64];
				V_snprintf(s_szSpyKnifeDraw, sizeof(s_szSpyKnifeDraw), "%s_draw", GetSpyKnifeAnimPrefix(pWeapon));
				return s_szSpyKnifeDraw;
			}
			if (V_stristr(weaponClass, "sapper") || V_stristr(weaponClass, "builder"))
			{
				const char *worldModel = pWeapon ? pWeapon->GetWorldModel() : NULL;
				if (worldModel && V_stristr(worldModel, "breadmonster"))
					return "c_breadmonster_sapper_draw";
				return "c_sapper_draw";
			}
			break;
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Get the draw animation scope for a weapon.
//          Determines how much of the arm chain the draw animation drives.
//          FULL_ARM: entire arm moves (dramatic draws, pumps with arm motion)
//          WRIST: wrist rotation + fingers + weapon_bone (hand pinned to controller)
//          WEAPON_BONE: only the weapon itself animates (subtle weapon motion)
//          NONE: no draw animation
//-----------------------------------------------------------------------------
VRDrawAnimScope GetWeaponDrawAnimScope(int playerClass, const char *weaponClass, C_TFWeaponBase *pWeapon)
{
	// Per-weapon draw animation scope.
	// VR_DRAW_ANIM_WRIST      - hand pinned to controller, all motion from wrist down
	// VR_DRAW_ANIM_FULL_ARM   - hand can displace from controller (dramatic draws)
	// VR_DRAW_ANIM_WEAPON_BONE - only weapon_bone animates, hand stays at idle
	// VR_DRAW_ANIM_NONE       - no draw animation at all

	if (IsAllClassMelee(pWeapon))
		return VR_DRAW_ANIM_NONE;

	switch (playerClass)
	{
		case TF_CLASS_SCOUT:
			if (V_stristr(weaponClass, "soda_popper")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "pep_brawler_blaster")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "scattergun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "handgun_scout")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "pistol")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "bat")) return VR_DRAW_ANIM_WRIST;
			if (V_stristr(weaponClass, "lunchbox_drink")) return VR_DRAW_ANIM_WRIST;
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_JAR_MILK)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
			if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1121)
				return VR_DRAW_ANIM_WEAPON_BONE; // Mutated Milk: hand drives creature, no wrist motion
				return VR_DRAW_ANIM_WRIST; // Mad Milk
			}
			break;

		case TF_CLASS_SOLDIER:
			if (V_stristr(weaponClass, "rocketlauncher")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "particle_cannon")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "shotgun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "katana")) return VR_DRAW_ANIM_WRIST;
			if (V_stristr(weaponClass, "sword")) return VR_DRAW_ANIM_WRIST;
			if (V_stristr(weaponClass, "shovel")) return VR_DRAW_ANIM_WRIST;
			if (V_stristr(weaponClass, "pickaxe")) return VR_DRAW_ANIM_WRIST;
			break;

		case TF_CLASS_PYRO:
			if (V_stristr(weaponClass, "flamethrower")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "rocketlauncher_fireball")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "flaregun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "shotgun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "fireaxe")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "slap")) return VR_DRAW_ANIM_NONE;
			break;

		case TF_CLASS_DEMOMAN:
			if (V_stristr(weaponClass, "grenadelauncher")) return VR_DRAW_ANIM_WEAPON_BONE;
			if (V_stristr(weaponClass, "cannon")) return VR_DRAW_ANIM_WEAPON_BONE;
			if (V_stristr(weaponClass, "pipebomblauncher")) return VR_DRAW_ANIM_WEAPON_BONE;
			if (V_stristr(weaponClass, "bottle")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "sword")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "katana")) return VR_DRAW_ANIM_NONE;
			break;

		case TF_CLASS_HEAVYWEAPONS:
			if (V_stristr(weaponClass, "minigun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "shotgun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "fists"))
				return IsBreadBite(pWeapon) ? VR_DRAW_ANIM_WRIST : VR_DRAW_ANIM_NONE;
			break;

		case TF_CLASS_ENGINEER:
			if (V_stristr(weaponClass, "sentry_revenge")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "shotgun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "pistol")) return VR_DRAW_ANIM_WRIST;
			if (V_stristr(weaponClass, "wrench")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "robot_arm")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "drg_pomson")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "raygun")) return VR_DRAW_ANIM_NONE;
			break;

		case TF_CLASS_MEDIC:
			if (V_stristr(weaponClass, "syringegun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "crossbow")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "medigun")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "bonesaw")) return VR_DRAW_ANIM_NONE;
			break;

		case TF_CLASS_SNIPER:
			if (V_stristr(weaponClass, "sniperrifle")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "compound_bow")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "smg")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "club")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "sword")) return VR_DRAW_ANIM_NONE;
			if (pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_JAR)
			{
				CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
			if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1105)
				return VR_DRAW_ANIM_WEAPON_BONE; // Self-Aware Beauty Mark: hand drives creature, no wrist motion
			}
			break;

		case TF_CLASS_SPY:
			if (V_stristr(weaponClass, "revolver")) return VR_DRAW_ANIM_NONE;
			if (V_stristr(weaponClass, "knife")) return VR_DRAW_ANIM_WRIST;
		if (V_stristr(weaponClass, "sapper") || V_stristr(weaponClass, "builder"))
			return VR_DRAW_ANIM_WEAPON_BONE;
			break;
	}

	return VR_DRAW_ANIM_WRIST;
}

//-----------------------------------------------------------------------------
// Purpose: Apply weapon grip pose to fingers (overrides finger tracking)
//        Samples finger bone rotations from the hand model's weapon animation
//-----------------------------------------------------------------------------
void C_TFVRHand::ApplyWeaponPose(matrix3x4_t *pBoneToWorldOut, int nMaxBones, C_TFWeaponBase *pWeaponOverride, int seqOverride, float cycleOverride)
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

	// Explicit sequence override (e.g. backstab transition animations)
	int sequence = -1;
	const char *usedName = NULL;
	float cycle = 0.0f;

	// Track whether draw animation drives only weapon_bone (not fingers).
	// Bread creatures excluded: their entity plays the draw sequence directly.
	bool bDrawWeaponBoneOnly = m_bPlayingDrawAnim && m_eDrawAnimScope == VR_DRAW_ANIM_WEAPON_BONE
		&& !m_bAnimateIdle;

	if (seqOverride >= 0)
	{
		sequence = seqOverride;
		cycle = cycleOverride;
		usedName = "seqOverride";
	}
	else if (pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW
		&& m_iShotgunManualReloadSequence >= 0
		&& !m_bPlayingChargeAnim
		&& !m_bPlayingFireAnim)
	{
		CTFCompoundBow *pBow = static_cast<CTFCompoundBow *>(pWeapon);
		if (pBow->IsVRBowArrowNocking())
		{
			sequence = m_iShotgunManualReloadSequence;
			cycle = Lerp(TFVR_GetBowNockVisualProgress(pBow),
				m_flShotgunManualReloadHoldCycle,
				m_flShotgunManualReloadCommitCycle);
			usedName = "bow_draw";
		}
		else if (pBow->IsVRBowArrowNocked())
		{
			sequence = m_iShotgunManualReloadSequence;
			cycle = m_flShotgunManualReloadCommitCycle;
			usedName = "bow_draw";
		}
		else if (m_iBowIdleSequence >= 0)
		{
			sequence = m_iBowIdleSequence;
			cycle = m_flBowIdleCycle;
			usedName = "bow_fire_idle";
		}
		else
		{
			sequence = m_iShotgunManualReloadSequence;
			cycle = m_flShotgunManualReloadHoldCycle;
			usedName = "bow_draw";
		}
	}
	else if (m_bPlayingChargeAnim || m_bPlayingFireAnim
		|| (m_bPlayingDrawAnim && m_eDrawAnimScope >= VR_DRAW_ANIM_WRIST))
	{
		int activeSeq = GetSequence();
		if (activeSeq >= 0)
		{
			sequence = activeSeq;
			cycle = GetCycle();
			usedName = m_bPlayingChargeAnim ? "charge_anim"
				: m_bPlayingDrawAnim ? "draw_anim" : "fire_anim";
		}
	}

	if (sequence < 0)
	{
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

	// Bow: blend the nocked bw_draw frame-35 pose into the sampled bw_charge
	// pose by charge fraction so the grip fingers match the unified nock/pull
	// pose (same blend the weapon hand and draw-hand target use).
	{
		C_TFWeaponBase *pPoseWeapon = pWeaponOverride ? pWeaponOverride : m_hHeldWeapon.Get();
		CTFCompoundBow *pPoseBow = (pPoseWeapon && pPoseWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
			? static_cast<CTFCompoundBow *>(pPoseWeapon) : NULL;
		if (pPoseBow && !m_bPlayingFireAnim
			&& (pPoseBow->IsVRBowArrowNocking() || pPoseBow->IsVRBowArrowNocked()))
		{
			float flPoseChargeFraction = clamp(pPoseBow->GetCurrentCharge() / MAX(pPoseBow->GetChargeMaxTime(), 0.01f), 0.0f, 1.0f);
			const bool bUseFullChargeReference = true;
			ApplyBowDrawChargeBlend(pStudioHdr, pStudioHdr->numbones(), pos, q, flPoseChargeFraction);
			ApplyBowShakeOverlay(pStudioHdr, pStudioHdr->numbones(), pos, q, flPoseChargeFraction, bUseFullChargeReference);
		}
		else if (pPoseBow && m_bPlayingFireAnim && m_bBowFireStartPoseValid
			&& m_iBowIdleSequence >= 0 && sequence == m_iBowIdleSequence)
		{
			ApplyBowFireStartPoseBlend(pStudioHdr, pStudioHdr->numbones(), pos, q, cycle);
		}
	}

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

	// List of finger bone prefixes (without L/R suffix) + weapon bones.
	// weapon_bone_1 (scattergun lever) is before weapon_bone so the
	// reload exclusion (decrement count) only removes weapon_bone.
	const char *fingerBones[] = {
		"bip_thumb_0", "bip_thumb_1", "bip_thumb_2",
		"bip_index_0", "bip_index_1", "bip_index_2",
		"bip_middle_0", "bip_middle_1", "bip_middle_2",
		"bip_ring_0", "bip_ring_1", "bip_ring_2",
		"bip_pinky_0", "bip_pinky_1", "bip_pinky_2",
		"weapon_bone_1",
		"handle_bone",
		"weapon_bone",
	};

	int nFingerBoneCount = ARRAYSIZE(fingerBones);

	for (int i = 0; i < nFingerBoneCount; i++)
	{
		// Try both left and right hand suffixes (authored pose side)
		char boneName[64];
		const char* suffix = m_bPoseAsLeftHand ? "_L" : "_R";
		V_snprintf(boneName, sizeof(boneName), "%s%s", fingerBones[i], suffix);

		int boneIndex = LookupBone(boneName);
		if (boneIndex < 0 || boneIndex >= nMaxBones)
		{
			// Try lowercase suffix
			suffix = m_bPoseAsLeftHand ? "_l" : "_r";
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

		// During reload, weapon_bone_1 (lever) is a child of bip_hand_R
		// in the skeleton, but should be positioned relative to weapon_bone
		// so it follows the weapon (held by two-hand grip) rather than
		// the hand (which moves with the controller pump gesture).
		// Both weapon_bone and weapon_bone_1 have local transforms relative
		// to bip_hand_R, so compute lever's offset relative to weapon_bone
		// from the animation, then apply to weapon_bone's actual world pos.
		// Convert the sampled quaternion to a matrix
		matrix3x4_t localBoneMatrix;
		QuaternionMatrix(q[boneIndex], pos[boneIndex], localBoneMatrix);

		// Transform by parent to get world-space transform
		ConcatTransforms(pBoneToWorldOut[parentIndex], localBoneMatrix, pBoneToWorldOut[boneIndex]);
	}

	// Ensure the unsuffixed "weapon_bone" is always updated, since
	// PositionWeaponFromBones reads it by that exact name.  The loop
	// above may have found "weapon_bone_R" first and written to a
	// different bone index, leaving the unsuffixed one at its idle pose.
	{
		int wpnUnsuffixed = LookupBone("weapon_bone");
		if (wpnUnsuffixed >= 0 && wpnUnsuffixed < nMaxBones)
		{
			const mstudiobone_t *pBone = pStudioHdr->pBone(wpnUnsuffixed);
			if (pBone && pBone->parent >= 0 && pBone->parent < nMaxBones)
			{
				matrix3x4_t localBoneMatrix;
				QuaternionMatrix(q[wpnUnsuffixed], pos[wpnUnsuffixed], localBoneMatrix);
				ConcatTransforms(pBoneToWorldOut[pBone->parent], localBoneMatrix, pBoneToWorldOut[wpnUnsuffixed]);
			}
			MatrixCopy(pBoneToWorldOut[wpnUnsuffixed], m_matIdleWeaponBoneWorld);
		}
	}

	// Reload override: sample the reload animation and apply hand + finger
	// bones so the hand follows the pump/lever motion.  If a lever bone
	// exists on the hand model (weapon_bone_1 or handle_bone), also
	// position it relative to weapon_bone so it stays with the weapon body.
	//
	// For left-hand-pump weapons (Stickybomb, Bison, Mangler) this code
	// runs on the RIGHT hand (which holds the weapon).  Only the lever
	// bone override is needed there — skip bip_hand and finger overrides
	// so the right hand keeps its VR controller position and finger tracking.
	bool bAllowReloadAnimDuringFire = false;
	if (m_hHeldWeapon.Get() && (IsPumpActionShotgunWeaponID(m_hHeldWeapon->GetWeaponID())
		|| TFVR_GetManualReloadPistol(m_hHeldWeapon.Get()) != NULL))
	{
		bAllowReloadAnimDuringFire = true;
	}
	if (m_bPlayingReloadAnim
		&& (!m_bPlayingFireAnim || bAllowReloadAnimDuringFire)
		&& m_iLeverReloadSequence >= 0)
	{
		int weaponBoneIdx = LookupBone("weapon_bone");
		if (weaponBoneIdx >= 0 && weaponBoneIdx < nMaxBones)
		{
			Vector reloadPos[MAXSTUDIOBONES];
			Quaternion reloadQ[MAXSTUDIOBONES];
			float reloadPoseParams[MAXSTUDIOPOSEPARAM];
			for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
				reloadPoseParams[i] = 0.0f;

			IBoneSetup reloadBoneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, reloadPoseParams);
			reloadBoneSetup.InitPose(reloadPos, reloadQ);
			reloadBoneSetup.AccumulatePose(reloadPos, reloadQ, m_iLeverReloadSequence,
				m_flLeverReloadCycle, 1.0f, gpGlobals->curtime, NULL);

			// Determine whether this hand is the pump hand.
			// Scattergun: right hand pumps.  Everything else: left hand pumps.
			bool bIsLeftHandPumpWeapon = false;
			if (m_hHeldWeapon.Get())
			{
				int wid = m_hHeldWeapon->GetWeaponID();
				bIsLeftHandPumpWeapon = (wid == TF_WEAPON_PIPEBOMBLAUNCHER
					|| wid == TF_WEAPON_RAYGUN
					|| wid == TF_WEAPON_PARTICLE_CANNON
					|| IsPumpActionShotgunWeaponID(wid));
			}
			bool bThisHandPumps = bIsLeftHandPumpWeapon ? IsLeftHand() : IsRightHand();

			if (bThisHandPumps)
			{
				CTFPistol *pManualPistol = TFVR_GetManualReloadPistol(m_hHeldWeapon.Get());
				const bool bIsManualPistol = pManualPistol != NULL;
				if (bIsManualPistol)
				{
					// Pistol: the hand stays anchored to the controller; the
					// WEAPON moves around the hand per the animation, and the
					// wrist optionally inherits a scaled fraction of the
					// authored hand motion for immersion. Everything is a
					// delta from frame 0 so there is no pop at start/finish.
					const mstudiobone_t *pWpnBone = pStudioHdr->pBone(weaponBoneIdx);
					if (pWpnBone && pWpnBone->parent >= 0 && pWpnBone->parent < nMaxBones)
					{
						Vector basePos[MAXSTUDIOBONES];
						Quaternion baseQ[MAXSTUDIOBONES];
						IBoneSetup baseSetup(pStudioHdr, BONE_USED_BY_ANYTHING, reloadPoseParams);
						baseSetup.InitPose(basePos, baseQ);
						baseSetup.AccumulatePose(basePos, baseQ, m_iLeverReloadSequence,
							0.0f, 1.0f, gpGlobals->curtime, NULL);

						// Scaled wrist-only motion: take the hand bone's
						// model-space delta between frame 0 and the current
						// cycle (so arm/parent posing in the anim is ignored
						// in the output), shrink it, and apply it on top of
						// the controller anchor. The gun follows rigidly so
						// the mag and off-hand targets stay in sync.
						extern ConVar tfvr_pistol_reload_wrist_motion;
						float flWristScale = clamp(tfvr_pistol_reload_wrist_motion.GetFloat(), 0.0f, 1.0f)
							* m_flPistolReloadAnimWeight;

						// Engineer: keep the hand fully pinned for now; Scout
						// keeps the small authored wrist bump.
						if (VRPistol_IsEngineer(TFVR_GetPistolVisualWeaponID(pManualPistol, GetOwnerPlayer())))
							flWristScale = 0.0f;

						if (flWristScale > 0.0f && m_iHandBone >= 0 && m_iHandBone < nMaxBones)
						{
							const int numBonesLocal = MIN(pStudioHdr->numbones(), MAXSTUDIOBONES);
							matrix3x4_t baseModel[MAXSTUDIOBONES];
							matrix3x4_t curModel[MAXSTUDIOBONES];
							for (int i = 0; i < numBonesLocal; i++)
							{
								matrix3x4_t baseLocalBone, curLocalBone;
								QuaternionMatrix(baseQ[i], basePos[i], baseLocalBone);
								QuaternionMatrix(reloadQ[i], reloadPos[i], curLocalBone);

								const mstudiobone_t *pBone = pStudioHdr->pBone(i);
								if (!pBone || pBone->parent < 0)
								{
									MatrixCopy(baseLocalBone, baseModel[i]);
									MatrixCopy(curLocalBone, curModel[i]);
								}
								else if (pBone->parent < i)
								{
									ConcatTransforms(baseModel[pBone->parent], baseLocalBone, baseModel[i]);
									ConcatTransforms(curModel[pBone->parent], curLocalBone, curModel[i]);
								}
								else
								{
									SetIdentityMatrix(baseModel[i]);
									SetIdentityMatrix(curModel[i]);
								}
							}

							// Hand delta in the hand's own frame
							matrix3x4_t invHandBase, handDelta;
							MatrixInvert(baseModel[m_iHandBone], invHandBase);
							ConcatTransforms(invHandBase, curModel[m_iHandBone], handDelta);

							Quaternion qDelta;
							Vector posDelta;
							MatrixAngles(handDelta, qDelta, posDelta);

							Quaternion qScaled;
							QuaternionScale(qDelta, flWristScale, qScaled);
							matrix3x4_t scaledDelta;
							QuaternionMatrix(qScaled, posDelta * flWristScale, scaledDelta);

							matrix3x4_t handLive;
							MatrixCopy(pBoneToWorldOut[m_iHandBone], handLive);
							matrix3x4_t handNew;
							ConcatTransforms(handLive, scaledDelta, handNew);
							MatrixCopy(handNew, pBoneToWorldOut[m_iHandBone]);

							// Gun rides the wrist rigidly; its own animation
							// delta is layered on below.
							matrix3x4_t invHandLive, weaponRelHand;
							MatrixInvert(handLive, invHandLive);
							ConcatTransforms(invHandLive, pBoneToWorldOut[weaponBoneIdx], weaponRelHand);
							ConcatTransforms(handNew, weaponRelHand, pBoneToWorldOut[weaponBoneIdx]);
						}

						matrix3x4_t baseLocal, curLocal;
						QuaternionMatrix(baseQ[weaponBoneIdx], basePos[weaponBoneIdx], baseLocal);
						QuaternionMatrix(reloadQ[weaponBoneIdx], reloadPos[weaponBoneIdx], curLocal);

						// Both expressed under the same parent, which cancels
						// in the delta, so this works whether or not the
						// wrist offset moved the hand above.
						matrix3x4_t baseWorld, curWorld;
						ConcatTransforms(pBoneToWorldOut[pWpnBone->parent], baseLocal, baseWorld);
						ConcatTransforms(pBoneToWorldOut[pWpnBone->parent], curLocal, curWorld);

						matrix3x4_t invBaseWorld, animDelta;
						MatrixInvert(baseWorld, invBaseWorld);
						ConcatTransforms(invBaseWorld, curWorld, animDelta);

						// Blend-out: shrink the gun's animation delta toward
						// identity as the hand returns to idle.
						if (m_flPistolReloadAnimWeight < 1.0f)
						{
							Quaternion qAnimDelta;
							Vector posAnimDelta;
							MatrixAngles(animDelta, qAnimDelta, posAnimDelta);

							Quaternion qWeighted;
							QuaternionScale(qAnimDelta, m_flPistolReloadAnimWeight, qWeighted);
							QuaternionMatrix(qWeighted, posAnimDelta * m_flPistolReloadAnimWeight, animDelta);
						}

						matrix3x4_t newWeaponWorld;
						ConcatTransforms(pBoneToWorldOut[weaponBoneIdx], animDelta, newWeaponWorld);
						MatrixCopy(newWeaponWorld, pBoneToWorldOut[weaponBoneIdx]);
					}
				}
				// Position bip_hand so the reload animation's hand pose is
				// applied relative to weapon_bone (which stays at idle).
				// weapon_bone is a child of bip_hand, so:
				//   bip_hand = weapon_bone_world * inverse(weapon_bone_local_in_reload)
				else if (m_iHandBone >= 0 && m_iHandBone < nMaxBones)
				{
					matrix3x4_t reloadWeaponBoneLocal;
					QuaternionMatrix(reloadQ[weaponBoneIdx], reloadPos[weaponBoneIdx], reloadWeaponBoneLocal);

					matrix3x4_t reloadWeaponBoneLocalInv;
					MatrixInvert(reloadWeaponBoneLocal, reloadWeaponBoneLocalInv);

					ConcatTransforms(pBoneToWorldOut[weaponBoneIdx], reloadWeaponBoneLocalInv, pBoneToWorldOut[m_iHandBone]);
				}

				// Override finger bones with reload animation so the hand
				// grips and moves with the lever.  Processed parent-first
				// so child bones use updated parent transforms.
				const char *reloadFingerPrefixes[] = {
					"bip_thumb_0", "bip_thumb_1", "bip_thumb_2",
					"bip_index_0", "bip_index_1", "bip_index_2",
					"bip_middle_0", "bip_middle_1", "bip_middle_2",
					"bip_ring_0", "bip_ring_1", "bip_ring_2",
					"bip_pinky_0", "bip_pinky_1", "bip_pinky_2",
				};
				for (int i = 0; i < ARRAYSIZE(reloadFingerPrefixes); i++)
				{
					char boneName[64];
					const char *sfx = m_bPoseAsLeftHand ? "_L" : "_R";
					V_snprintf(boneName, sizeof(boneName), "%s%s", reloadFingerPrefixes[i], sfx);
					int boneIndex = LookupBone(boneName);
					if (boneIndex < 0 || boneIndex >= nMaxBones)
					{
						sfx = m_bPoseAsLeftHand ? "_l" : "_r";
						V_snprintf(boneName, sizeof(boneName), "%s%s", reloadFingerPrefixes[i], sfx);
						boneIndex = LookupBone(boneName);
					}
					if (boneIndex < 0 || boneIndex >= nMaxBones)
						boneIndex = LookupBone(reloadFingerPrefixes[i]);
					if (boneIndex < 0 || boneIndex >= nMaxBones)
						continue;

					const mstudiobone_t *pBone = pStudioHdr->pBone(boneIndex);
					if (!pBone || pBone->parent < 0 || pBone->parent >= nMaxBones)
						continue;

					matrix3x4_t localBoneMatrix;
					QuaternionMatrix(reloadQ[boneIndex], reloadPos[boneIndex], localBoneMatrix);

					matrix3x4_t reloadFingerWorld;
					ConcatTransforms(pBoneToWorldOut[pBone->parent], localBoneMatrix, reloadFingerWorld);

					// Pistol blend-out: ease fingers back to their tracked pose
					if (bIsManualPistol && m_flPistolReloadAnimWeight < 1.0f)
						TFVR_BlendTransforms(pBoneToWorldOut[boneIndex], reloadFingerWorld, m_flPistolReloadAnimWeight, pBoneToWorldOut[boneIndex]);
					else
						MatrixCopy(reloadFingerWorld, pBoneToWorldOut[boneIndex]);
				}
			}

			// Lever bone: weapon-specific lookup since both weapon_bone_1
			// and handle_bone exist on the shared hand model.
			int leverBoneIdx = -1;
			if (m_hHeldWeapon.Get() && m_hHeldWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON)
				leverBoneIdx = LookupBone("handle_bone");
			else
				leverBoneIdx = LookupBone("weapon_bone_1");
			if (leverBoneIdx >= 0 && leverBoneIdx < nMaxBones)
			{
				// Build model-space transforms to handle any bone hierarchy
				int numBones = pStudioHdr->numbones();
				matrix3x4_t modelSpace[MAXSTUDIOBONES];
				for (int i = 0; i < numBones && i < MAXSTUDIOBONES; i++)
				{
					matrix3x4_t localMat;
					QuaternionMatrix(reloadQ[i], reloadPos[i], localMat);
					const mstudiobone_t *pBone = pStudioHdr->pBone(i);
					if (!pBone || pBone->parent < 0)
						MatrixCopy(localMat, modelSpace[i]);
					else
						ConcatTransforms(modelSpace[pBone->parent], localMat, modelSpace[i]);
				}

				matrix3x4_t weaponModelInv;
				MatrixInvert(modelSpace[weaponBoneIdx], weaponModelInv);
				matrix3x4_t leverRelativeToWeapon;
				ConcatTransforms(weaponModelInv, modelSpace[leverBoneIdx], leverRelativeToWeapon);
				ConcatTransforms(pBoneToWorldOut[weaponBoneIdx], leverRelativeToWeapon, pBoneToWorldOut[leverBoneIdx]);
			}
		}
	}

	// Medigun body animation: rotation-only on ALL weapon_bone variants.
	// When a body animation is playing (fire_on/loop/off), keep weapon_bone
	// position from the idle pose and only apply the rotation from the body anim.
	// Must cover both unsuffixed and suffixed since PositionWeaponFromBones
	// reads weapon_bone_L on the medigun.
	if (m_eMedigunFireState != MEDIGUN_FIRE_IDLE && m_iIdleSequence >= 0)
	{
		Vector idleBonePos[MAXSTUDIOBONES];
		Quaternion idleBoneQ[MAXSTUDIOBONES];
		float idlePoseParams[MAXSTUDIOPOSEPARAM];
		for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
			idlePoseParams[i] = 0.0f;
		IBoneSetup idleSetup(pStudioHdr, BONE_USED_BY_ANYTHING, idlePoseParams);
		idleSetup.InitPose(idleBonePos, idleBoneQ);
		idleSetup.AccumulatePose(idleBonePos, idleBoneQ, m_iIdleSequence, 0.0f, 1.0f, gpGlobals->curtime, NULL);

		const char *wpnBoneNames[] = { "weapon_bone", "weapon_bone_L", "weapon_bone_l" };
		for (int n = 0; n < ARRAYSIZE(wpnBoneNames); n++)
		{
			int wpnIdx = LookupBone(wpnBoneNames[n]);
			if (wpnIdx < 0 || wpnIdx >= nMaxBones)
				continue;
			const mstudiobone_t *pWpnBone = pStudioHdr->pBone(wpnIdx);
			if (!pWpnBone || pWpnBone->parent < 0 || pWpnBone->parent >= nMaxBones)
				continue;

			matrix3x4_t rotOnlyLocal;
			QuaternionMatrix(q[wpnIdx], idleBonePos[wpnIdx], rotOnlyLocal);
			ConcatTransforms(pBoneToWorldOut[pWpnBone->parent], rotOnlyLocal, pBoneToWorldOut[wpnIdx]);
		}
	}

	// Medigun lever override:
	// - vm_weapon_bone_L (lever): parent-relative local transform (stays attached to body)
	// - bip_hand_R + fingers: model-space approach anchored at bip_hand_L (m_iHandBone)
	//   because BaseClass::SetupBones may leave bip_hand_R's parent at the entity origin,
	//   not at the VR controller position. Anchoring to bip_hand_L (which IS VR-positioned)
	//   ensures the push hand follows the lever correctly.
	if (m_bMedigunLeverActive && !m_bPlayingReloadAnim && m_iMedigunLeverSeq >= 0)
	{
		// Sample lever animation into local-space arrays
		Vector leverLocalPos[MAXSTUDIOBONES];
		Quaternion leverLocalQ[MAXSTUDIOBONES];
		float leverPoseParams[MAXSTUDIOPOSEPARAM];
		for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
			leverPoseParams[i] = 0.0f;

		IBoneSetup leverSetup(pStudioHdr, BONE_USED_BY_ANYTHING, leverPoseParams);
		leverSetup.InitPose(leverLocalPos, leverLocalQ);
		leverSetup.AccumulatePose(leverLocalPos, leverLocalQ, m_iMedigunLeverSeq,
			m_flMedigunLeverCycle, 1.0f, gpGlobals->curtime, NULL);

		// Override vm_weapon_bone_L (lever): parent-relative keeps it attached to body
		const char *leverNames[] = { "vm_weapon_bone_L", "vm_weapon_bone_l" };
		for (int n = 0; n < ARRAYSIZE(leverNames); n++)
		{
			int leverBoneIdx = LookupBone(leverNames[n]);
			if (leverBoneIdx < 0 || leverBoneIdx >= nMaxBones)
				continue;
			const mstudiobone_t *pLeverBone = pStudioHdr->pBone(leverBoneIdx);
			if (!pLeverBone || pLeverBone->parent < 0 || pLeverBone->parent >= nMaxBones)
				continue;
			matrix3x4_t leverLocal;
			QuaternionMatrix(leverLocalQ[leverBoneIdx], leverLocalPos[leverBoneIdx], leverLocal);
			ConcatTransforms(pBoneToWorldOut[pLeverBone->parent], leverLocal, pBoneToWorldOut[leverBoneIdx]);
		}

		// Build full model-space skeleton from lever animation for the hand override
		int numSkelBones = pStudioHdr->numbones();
		matrix3x4_t leverWorld[MAXSTUDIOBONES];
		for (int i = 0; i < numSkelBones; i++)
		{
			matrix3x4_t localMat;
			QuaternionMatrix(leverLocalQ[i], leverLocalPos[i], localMat);
			const mstudiobone_t *pBone = pStudioHdr->pBone(i);
			if (!pBone || pBone->parent < 0)
				MatrixCopy(localMat, leverWorld[i]);
			else
				ConcatTransforms(leverWorld[pBone->parent], localMat, leverWorld[i]);
		}

		// Anchor bip_hand_R + fingers to bip_hand_L (m_iHandBone) which is VR-positioned.
		// leverToVR maps from lever model-space to VR world-space via bip_hand_L.
		if (m_iHandBone >= 0 && m_iHandBone < nMaxBones)
		{
			matrix3x4_t invLeverHandBone;
			MatrixInvert(leverWorld[m_iHandBone], invLeverHandBone);
			matrix3x4_t leverToVR;
			ConcatTransforms(pBoneToWorldOut[m_iHandBone], invLeverHandBone, leverToVR);

			const char *handBones[] = {
				"bip_hand_R", "bip_hand_r",
				"bip_thumb_0_R", "bip_thumb_1_R", "bip_thumb_2_R",
				"bip_index_0_R", "bip_index_1_R", "bip_index_2_R",
				"bip_middle_0_R", "bip_middle_1_R", "bip_middle_2_R",
				"bip_ring_0_R", "bip_ring_1_R", "bip_ring_2_R",
				"bip_pinky_0_R", "bip_pinky_1_R", "bip_pinky_2_R",
				"bip_thumb_0_r", "bip_thumb_1_r", "bip_thumb_2_r",
				"bip_index_0_r", "bip_index_1_r", "bip_index_2_r",
				"bip_middle_0_r", "bip_middle_1_r", "bip_middle_2_r",
				"bip_ring_0_r", "bip_ring_1_r", "bip_ring_2_r",
				"bip_pinky_0_r", "bip_pinky_1_r", "bip_pinky_2_r",
			};
			for (int i = 0; i < ARRAYSIZE(handBones); i++)
			{
				int boneIndex = LookupBone(handBones[i]);
				if (boneIndex < 0 || boneIndex >= nMaxBones)
					continue;

				ConcatTransforms(leverToVR, leverWorld[boneIndex], pBoneToWorldOut[boneIndex]);
			}
		}
	}

	// WEAPON_BONE scope draw animation: the idle pose was applied to fingers
	// above, now override weapon_bone with the draw animation's transform.
	// Since the entity sequence stays at idle for this scope, compute the
	// draw cycle from elapsed time rather than using GetCycle().
	if (bDrawWeaponBoneOnly && m_iDrawSequence >= 0)
	{
		float flDrawDuration = SequenceDuration(pStudioHdr, m_iDrawSequence);
		float flElapsed = gpGlobals->curtime - m_flDrawAnimStartTime;
		float flDrawCycle = (flDrawDuration > 0.0f) ? clamp(flElapsed / flDrawDuration, 0.0f, 1.0f) : 1.0f;

		Vector drawPos[MAXSTUDIOBONES];
		Quaternion drawQ[MAXSTUDIOBONES];

		float drawPoseParams[MAXSTUDIOPOSEPARAM];
		for (int i = 0; i < MAXSTUDIOPOSEPARAM; i++)
			drawPoseParams[i] = 0.0f;

		IBoneSetup drawBoneSetup(pStudioHdr, BONE_USED_BY_ANYTHING, drawPoseParams);
		drawBoneSetup.InitPose(drawPos, drawQ);
		drawBoneSetup.AccumulatePose(drawPos, drawQ, m_iDrawSequence, flDrawCycle, 1.0f, gpGlobals->curtime, NULL);

		// Apply draw animation to weapon_bone only (both suffixed and unsuffixed)
		const char *wpnBoneNames[] = { "weapon_bone_R", "weapon_bone_L", "weapon_bone" };
		for (int n = 0; n < ARRAYSIZE(wpnBoneNames); n++)
		{
			int wpnIdx = LookupBone(wpnBoneNames[n]);
			if (wpnIdx < 0 || wpnIdx >= nMaxBones)
				continue;
			const mstudiobone_t *pWpnBone = pStudioHdr->pBone(wpnIdx);
			if (!pWpnBone || pWpnBone->parent < 0 || pWpnBone->parent >= nMaxBones)
				continue;

			matrix3x4_t drawLocalMatrix;
			QuaternionMatrix(drawQ[wpnIdx], drawPos[wpnIdx], drawLocalMatrix);
			ConcatTransforms(pBoneToWorldOut[pWpnBone->parent], drawLocalMatrix, pBoneToWorldOut[wpnIdx]);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Return the idle weapon_bone position and direction, reconstructed
//          each frame in SetupBones from the cached local offset + current hand.
//          Direction uses the Y axis (column 1) to match the melee weapon_bone
//          convention used by GetWeaponMuzzlePositionAndAngles.
//          Returns true for weapons that aim along the weapon_bone (spy knife,
//          sapper) rather than a muzzle attachment.
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetIdleWeaponBoneTransform( Vector &outPos, QAngle &outAng ) const
{
	if ( !m_bHasIdleWeaponBone )
		return false;

	if ( m_iBackstabUpSequence < 0 )
		return false;

	MatrixGetColumn( m_matIdleWeaponBoneWorld, 3, outPos );
	Vector vecDir;
	MatrixGetColumn( m_matIdleWeaponBoneWorld, 1, vecDir );
	VectorAngles( vecDir, outAng );
	return true;
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

	// Bread Bite: hide both VR hand meshes so only the weapon model shows.
	// SetupBones still runs (triggered by the render weapon) so weapon
	// positioning is unaffected.
	if (m_bIsBreadBite)
		return false;
	if (IsLeftHand())
	{
		C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
		if (pRightHand && pRightHand->m_bIsBreadBite)
			return false;
	}

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
			C_BaseAnimating *pRenderWeapon = m_hRenderWeapon.Get();
			if (TFVR_ValidateHandRenderable(pRenderWeapon, "render weapon"))
			{
				VRHandLayer_AddRenderable(pRenderWeapon);
				VRHandLayer_AddParticleOwner(pRenderWeapon);
			}
		}

		// Source weapon creates particles like loose cannon sparks
		if (m_hHeldWeapon.Get())
			VRHandLayer_AddParticleOwner(m_hHeldWeapon.Get());

		if (m_hLeftHandWatch.Get())
		{
			C_BaseAnimating *pWatch = m_hLeftHandWatch.Get();
			if (TFVR_ValidateHandRenderable(pWatch, "left hand watch"))
			{
				VRHandLayer_AddRenderable(pWatch);
				VRHandLayer_AddParticleOwner(pWatch);
			}
		}
		if (m_hLeftHandBall.Get() && m_iLastBallAmmo > 0)
		{
			VRHandLayer_AddParticleOwner(m_hLeftHandBall.Get());
		}
		if (m_hLeftHandShield.Get())
		{
			C_BaseAnimating *pShield = m_hLeftHandShield.Get();
			if (TFVR_ValidateHandRenderable(pShield, "left hand shield"))
				VRHandLayer_AddRenderable(pShield);
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
	const char *pszHandLabel = IsLeftHand() ? "left hand" : "right hand";

	// Apply ubercharge material override
	if (bInvuln && (flags & STUDIO_RENDER))
	{
		modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());
	}

	// Left-handed mode: reflected bones invert winding, so flip culling to
	// clockwise while drawing the hand mesh (matches vanilla viewmodel flip).
	// Capture the flag in a local BEFORE BaseClass::DrawModel: that call runs
	// SetupBones internally, which can recompute m_bReflectPoseActive (e.g. when
	// the active weapon changes on a class switch). If the set and restore read
	// the member at different times the CW cull mode leaks globally, inverting
	// normals on every model and hiding HUD text.
	const bool bMirrorCull = m_bReflectPoseActive && (flags & STUDIO_RENDER);
	CMatRenderContextPtr pRenderContext(materials);
	if (bMirrorCull)
		pRenderContext->CullMode(MATERIAL_CULLMODE_CW);

	ret = BaseClass::DrawModel(flags);

	if (bMirrorCull)
		pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);

	// Reset material override
	if (bInvuln && (flags & STUDIO_RENDER))
	{
		modelrender->ForcedMaterialOverride(NULL);
	}

	// Draw the ball right after the hand so SetupBones has already
	// positioned it at weapon_bone_L.  The ball has permanent EF_NODRAW
	// to prevent stale-position draws during the world pass, so we
	// temporarily clear it here for the actual draw.
	if (m_hLeftHandBall.Get() && m_iLastBallAmmo > 0)
	{
		C_BaseAnimating *pBall = m_hLeftHandBall.Get();
		if (TFVR_ValidateHandRenderable(pBall, "left hand ball"))
		{
			pBall->RemoveEffects(EF_NODRAW);
			pBall->DrawModel(flags);
			pBall->AddEffects(EF_NODRAW);
		}
	}

	if (m_hManualReloadRocket.Get())
	{
		C_BaseAnimating *pRocket = m_hManualReloadRocket.Get();
		if (TFVR_ValidateHandRenderable(pRocket, "manual reload rocket"))
		{
			pRocket->RemoveEffects(EF_NODRAW);
			pRocket->DrawModel(flags);
			pRocket->AddEffects(EF_NODRAW);
		}
	}

	if (m_hPistolMagazine.Get())
	{
		C_BaseAnimating *pMag = m_hPistolMagazine.Get();
		if (TFVR_ValidateHandRenderable(pMag, "pistol magazine"))
		{
			if (bInvuln && (flags & STUDIO_RENDER))
				modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());

			pMag->RemoveEffects(EF_NODRAW);
			pMag->DrawModel(flags);
			pMag->AddEffects(EF_NODRAW);

			if (bInvuln && (flags & STUDIO_RENDER))
				modelrender->ForcedMaterialOverride(NULL);
		}
	}

	if (m_hLeftHandShield.Get())
	{
		C_BaseAnimating *pShield = m_hLeftHandShield.Get();
		if (TFVR_ValidateHandRenderable(pShield, "left hand shield"))
		{
			if (bInvuln && (flags & STUDIO_RENDER))
				modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());

			pShield->RemoveEffects(EF_NODRAW);
			pShield->DrawModel(flags);
			pShield->AddEffects(EF_NODRAW);

			if (bInvuln && (flags & STUDIO_RENDER))
				modelrender->ForcedMaterialOverride(NULL);
		}
	}

	if (m_hLeftHandWatch.Get())
	{
		C_BaseAnimating *pWatch = m_hLeftHandWatch.Get();
		if (TFVR_ValidateHandRenderable(pWatch, "left hand watch"))
		{
			if (bInvuln && (flags & STUDIO_RENDER))
				modelrender->ForcedMaterialOverride(*pOwner->GetInvulnMaterialRef());

			pWatch->RemoveEffects(EF_NODRAW);
			pWatch->DrawModel(flags);
			pWatch->AddEffects(EF_NODRAW);

			if (bInvuln && (flags & STUDIO_RENDER))
				modelrender->ForcedMaterialOverride(NULL);
		}
	}

	if (m_hLeftHandWatch.Get() && m_pWatchPanel && (flags & STUDIO_RENDER)
		&& pOwner->GetPercentInvisible() < 1.0f)
	{
		C_BaseAnimating *pWatch = m_hLeftHandWatch.Get();
		if (TFVR_ValidateHandRenderable(pWatch, "left hand watch panel"))
		{
			int iLL = pWatch->LookupAttachment("controlpanel0_ll");
			int iUR = pWatch->LookupAttachment("controlpanel0_ur");
			if (iLL > 0 && iUR > 0)
			{
				matrix3x4_t matLL;
				Vector urPos;
				if (!pWatch->GetAttachment(iLL, matLL) || !pWatch->GetAttachment(iUR, urPos))
					return ret;

				Vector llPos, right, up, forward;
				MatrixGetColumn(matLL, 3, llPos);
				MatrixGetColumn(matLL, 0, right);
				MatrixGetColumn(matLL, 1, up);
				MatrixGetColumn(matLL, 2, forward);

				matrix3x4_t invLL;
				MatrixInvert(matLL, invLL);
				Vector lrlocal;
				VectorTransform(urPos, invLL, lrlocal);

				VMatrix panelToWorld;
				panelToWorld.Identity();
				panelToWorld[0][0] = right.x;  panelToWorld[0][1] = up.x;  panelToWorld[0][2] = forward.x;
				panelToWorld[1][0] = right.y;  panelToWorld[1][1] = up.y;  panelToWorld[1][2] = forward.y;
				panelToWorld[2][0] = right.z;  panelToWorld[2][1] = up.z;  panelToWorld[2][2] = forward.z;
				panelToWorld.SetTranslation(llPos);

				int pixelW, pixelH;
				m_pWatchPanel->GetSize(pixelW, pixelH);
				if (pixelW <= 0 || pixelH <= 0)
					return ret;

				g_pMatSystemSurface->DisableClipping(true);
				g_pMatSystemSurface->DrawPanelIn3DSpace(
					m_pWatchPanel->GetVPanel(), panelToWorld,
					pixelW, pixelH,
					lrlocal.x, lrlocal.y);
				g_pMatSystemSurface->DisableClipping(false);
			}
		}
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
	m_iLastEquippedWeaponID = pWeapon->GetWeaponID();

	// Reset aim stabilization so it recaptures reference with this weapon's grip
	m_bAimRefValid = false;

	// Gunslinger (robot_arm): the hand model IS the weapon, no render weapon needed.
	// Melee hit detection is handled by VRPhysicalMeleeUpdate in the shared weapon code.
	// Finger tracking drives the hand pose, no fire animation needed.
	if (m_bHasGunslinger && V_stristr(pWeapon->GetClassname(), "robot_arm"))
	{
		Msg("VR Hand (RIGHT): Gunslinger equipped - no render weapon, hand IS the weapon\n");
		return;
	}

	// VR NEW APPROACH: Create a separate render-only entity for the weapon visual
	// This way the player's actual weapon can remain in the viewmodel system
	// and we have full control over a separate worldmodel entity for rendering

	// Use world model for VR (c_models in TF2 are the world models)
	const char *worldModel = pWeapon->GetWorldModel();

	// Manual-reload pistols use dedicated VR models with the magazine split
	// into its own mesh so the mag can leave the gun.
	if (TFVR_GetManualReloadPistol(pWeapon))
		worldModel = VRPistol_GunModelForWorldModel(pWeapon->GetWorldModel());

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

	// Match team for paint kit / war paint material overrides
	if (pOwner)
		pRenderWeapon->ChangeTeam(pOwner->GetTeamNumber());

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
				extern ConVar tfvr_shotgun_pump_action;

				bool bTryVRFireOverride = true;
				const bool bManualPumpShotgun = IsPumpActionShotgunWeaponID( pWeapon->GetWeaponID() ) && tfvr_shotgun_pump_action.GetBool();
				if ( IsPumpActionShotgunWeaponID( pWeapon->GetWeaponID() ) && !tfvr_shotgun_pump_action.GetBool() )
				{
					bTryVRFireOverride = false;
					if (tfvr_weapon_fire_anim_debug.GetBool())
					{
						DevMsg("VR: Shotgun fire sequence skipping VR override while TFVR auto-reload is on\n");
					}
				}

				if ( bTryVRFireOverride )
				{
					// Manual pump shotguns use one shared custom fire pose.
					// Other weapons keep the normal "vr_" prefix convention.
					char vrFireName[128];
					if ( bManualPumpShotgun )
						V_strncpy(vrFireName, "vr_fire", sizeof(vrFireName));
					else
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
				}

				if (m_iFireSequence < 0)
				{
					if ( bManualPumpShotgun )
					{
						Warning("VR: Manual shotgun pump is enabled, but 'vr_fire' was not found on '%s'; suppressing legacy fire animation\n",
							GetModelName());
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

		// Spy knife backstab animations: up (raise), idle (hold), down (lower), attack (stab).
		m_iBackstabUpSequence = -1;
		m_iBackstabDownSequence = -1;
		m_iBackstabIdleSequence = -1;
		m_iBackstabAttackSequence = -1;
		m_bBackstabReady = false;
		m_bBackstabAttacking = false;
		m_flBackstabCycle = 0.0f;
		m_bBackstabRaising = false;
		m_bBackstabLowering = false;
		m_flLastBackstabUpdateTime = 0.0f;
		m_bHasIdleWeaponBone = false;
		m_bOffHandToWeaponBoneValid = false;
		if (playerClass == TF_CLASS_SPY && V_stristr(weaponClass, "knife"))
		{
			const char *knifePrefix = GetSpyKnifeAnimPrefix(pWeapon);
			char bsName[64];

			V_snprintf(bsName, sizeof(bsName), "%s_backstab_up", knifePrefix);
			m_iBackstabUpSequence = LookupSequence(bsName);

			V_snprintf(bsName, sizeof(bsName), "%s_backstab_down", knifePrefix);
			m_iBackstabDownSequence = LookupSequence(bsName);

			V_snprintf(bsName, sizeof(bsName), "%s_backstab_idle", knifePrefix);
			m_iBackstabIdleSequence = LookupSequence(bsName);

			V_snprintf(bsName, sizeof(bsName), "%s_backstab", knifePrefix);
			m_iBackstabAttackSequence = LookupSequence(bsName);

			extern ConVar tfvr_backstab_debug;
			if (tfvr_backstab_debug.GetBool())
			{
				DevMsg("VR: Backstab sequences (%s): up=%d down=%d idle=%d attack=%d on '%s'\n",
					knifePrefix, m_iBackstabUpSequence, m_iBackstabDownSequence,
					m_iBackstabIdleSequence, m_iBackstabAttackSequence, GetModelName());
			}
		}

		// Scattergun reload animation sequences
		m_iReloadStartSequence = -1;
		m_iReloadLoopSequence = -1;
		m_iReloadEndSequence = -1;
		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_flReloadAnimStartTime = 0.0f;
		m_flReloadLoopBottomCycle = 0.0f;
		m_flShotgunPumpStartCycle = 0.0f;
		m_flShotgunPumpEndCycle = 1.0f;
		m_iShotgunManualReloadSequence = -1;
		m_flShotgunManualReloadHoldCycle = 0.0f;
		m_flShotgunManualReloadCommitCycle = 1.0f;
		m_flPistolOneFrameCycle = 0.0f;
		m_flPistolMagFreeCycle = 0.0f;
		m_flPistolPauseCycle = 1.0f;
		m_flPistolInsertTargetCycle = 0.0f;
		m_flPistolFinishEndCycle = 1.0f;
		m_bPistolReloadBlendOut = false;
		m_flPistolReloadBlendOutStartTime = 0.0f;
		m_flPistolReloadAnimWeight = 1.0f;
		m_bShotgunManualReloadPoseActive = false;
		m_bShotgunManualReloadBlendOutActive = false;
		m_flShotgunManualReloadBlendOutStartTime = 0.0f;
		m_nShotgunManualReloadBlendOutBones = 0;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		if (IsScattergunWeaponID(pWeapon->GetWeaponID()))
		{
			m_iReloadStartSequence = LookupSequence("sg_reload_start");
			m_iReloadLoopSequence  = LookupSequence("sg_reload_loop");
			m_iReloadEndSequence   = LookupSequence("sg_reload_end");

			if (m_iReloadLoopSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iReloadLoopSequence, poseParams);
					if (maxFrame > 0)
						m_flReloadLoopBottomCycle = 5.0f / (float)maxFrame;
				}
			}

			DevMsg("VR: Scattergun reload sequences: start=%d loop=%d end=%d bottomCycle=%.3f on '%s'\n",
				m_iReloadStartSequence, m_iReloadLoopSequence, m_iReloadEndSequence,
				m_flReloadLoopBottomCycle, GetModelName());
		}
		else if (IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID()))
		{
			const int iWeaponID = pWeapon->GetWeaponID();
			const char *shotgunPumpAnimName = (fireAnimName && fireAnimName[0]) ? fireAnimName : "fire";
			const char *shotgunManualReloadAnimName = "reload_loop";
			float flManualReloadStartFrame = 5.0f;
			float flManualReloadEndFrame = 8.0f;
			if (iWeaponID == TF_WEAPON_SHOTGUN_PYRO)
			{
				flManualReloadStartFrame = 11.0f;
				flManualReloadEndFrame = 14.0f;
			}
			else if (iWeaponID == TF_WEAPON_SHOTGUN_HWG)
			{
				flManualReloadStartFrame = 5.0f;
				flManualReloadEndFrame = 8.0f;
			}
			else if (iWeaponID == TF_WEAPON_SHOTGUN_PRIMARY
				|| iWeaponID == TF_WEAPON_SENTRY_REVENGE
				|| iWeaponID == TF_WEAPON_SHOTGUN_BUILDING_RESCUE)
			{
				shotgunManualReloadAnimName = "fj_reload_loop";
				flManualReloadStartFrame = 10.0f;
				flManualReloadEndFrame = 13.0f;
			}

			m_iReloadLoopSequence = LookupSequence(shotgunPumpAnimName);
			m_iShotgunManualReloadSequence = LookupSequence(shotgunManualReloadAnimName);

			if (m_iReloadLoopSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iReloadLoopSequence, poseParams);
					if (maxFrame > 0)
					{
						m_flShotgunPumpStartCycle = clamp( 8.0f / (float)maxFrame, 0.0f, 1.0f );
						m_flShotgunPumpEndCycle = clamp( 16.0f / (float)maxFrame, m_flShotgunPumpStartCycle, 1.0f );
						m_flReloadLoopBottomCycle = (m_flShotgunPumpStartCycle + m_flShotgunPumpEndCycle) * 0.5f;
					}
				}
			}

			if (m_iShotgunManualReloadSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iShotgunManualReloadSequence, poseParams);
					if (maxFrame > 0)
					{
						m_flShotgunManualReloadHoldCycle = clamp( flManualReloadStartFrame / (float)maxFrame, 0.0f, 1.0f );
						m_flShotgunManualReloadCommitCycle = clamp( flManualReloadEndFrame / (float)maxFrame, m_flShotgunManualReloadHoldCycle, 1.0f );
					}
				}
			}

			DevMsg("VR: Shotgun pump sequence '%s': seq=%d start=%.3f mid=%.3f end=%.3f manualReload='%s' seq=%d frames=%.1f-%.1f hold=%.3f commit=%.3f on '%s'\n",
				shotgunPumpAnimName, m_iReloadLoopSequence, m_flShotgunPumpStartCycle,
				m_flReloadLoopBottomCycle, m_flShotgunPumpEndCycle, shotgunManualReloadAnimName, m_iShotgunManualReloadSequence,
				flManualReloadStartFrame, flManualReloadEndFrame,
				m_flShotgunManualReloadHoldCycle, m_flShotgunManualReloadCommitCycle, GetModelName());
		}
		else if ((pWeapon->GetWeaponID() == TF_WEAPON_ROCKETLAUNCHER
			|| pWeapon->GetWeaponID() == TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT))
		{
			const float flManualReloadStartFrame = 4.0f;
			const float flManualReloadEndFrame = 14.0f;
			const char *pszRocketReloadSequence = TFVR_ShouldUseBlackBoxReloadLoop(pWeapon)
				? "dh_reload_loop_alt" : "dh_reload_loop";
			m_iShotgunManualReloadSequence = LookupSequence(pszRocketReloadSequence);

			if (m_iShotgunManualReloadSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iShotgunManualReloadSequence, poseParams);
					if (maxFrame > 0)
					{
						m_flShotgunManualReloadHoldCycle = clamp( flManualReloadStartFrame / (float)maxFrame, 0.0f, 1.0f );
						m_flShotgunManualReloadCommitCycle = clamp( flManualReloadEndFrame / (float)maxFrame, m_flShotgunManualReloadHoldCycle, 1.0f );
					}
				}
			}

			DevMsg("VR: Rocket manual reload sequence '%s': seq=%d frames=%.1f-%.1f hold=%.3f commit=%.3f on '%s'\n",
				pszRocketReloadSequence, m_iShotgunManualReloadSequence, flManualReloadStartFrame, flManualReloadEndFrame,
				m_flShotgunManualReloadHoldCycle, m_flShotgunManualReloadCommitCycle, GetModelName());
		}
		else if (pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		{
			// The nocked/draw-hand attach pose comes from bw_charge (the arrow
			// pull-back pose), NOT bw_draw. bw_draw scrubs a full drawing motion
			// that sweeps/dips the bow, which caused a visible dip at nock. We
			// drive the actual pull with the controller/charge instead, so the
			// nock pose is simply bw_charge frame 0 (arrow nocked, unpulled) and
			// the charge animation scrubs the pull-back from there.
			m_iShotgunManualReloadSequence = LookupSequence("bw_charge");
			m_flShotgunManualReloadHoldCycle = 0.0f;
			m_flShotgunManualReloadCommitCycle = 0.0f;

			DevMsg("VR: Bow manual arrow sequence 'bw_charge': seq=%d nock pose at frame 0 on '%s'\n",
				m_iShotgunManualReloadSequence, GetModelName());
		}
		else if (VRPistol_IsManualReloadWeaponID(pWeapon->GetWeaponID()))
		{
			// Manual magazine reload: hold/commit map to the off-hand insert
			// frames; the eject/pause/finish markers are per-class
			// (scout p_reload vs engineer pstl_reload).
			const int iPistolID = TFVR_GetPistolVisualWeaponID(pWeapon, pOwner);
			const char *pszPistolReloadSeq = VRPistol_ReloadSequenceName(iPistolID);
			m_iShotgunManualReloadSequence = LookupSequence(pszPistolReloadSeq);

			if (m_iShotgunManualReloadSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iShotgunManualReloadSequence, poseParams);
					if (maxFrame > 0)
					{
						m_flPistolOneFrameCycle = 1.0f / (float)maxFrame;
						m_flPistolMagFreeCycle = clamp( VRPistol_FrameMagFree(iPistolID) / (float)maxFrame, 0.0f, 1.0f );
						m_flPistolPauseCycle = clamp( VRPistol_FramePause(iPistolID) / (float)maxFrame, m_flPistolMagFreeCycle, 1.0f );
						m_flShotgunManualReloadHoldCycle = clamp( VRPistol_FrameInsertStart(iPistolID) / (float)maxFrame, 0.0f, 1.0f );
						m_flPistolInsertTargetCycle = clamp( VRPistol_FrameInsertTarget(iPistolID) / (float)maxFrame, 0.0f, 1.0f );
						m_flShotgunManualReloadCommitCycle = clamp( VRPistol_FrameInsertEnd(iPistolID) / (float)maxFrame, m_flShotgunManualReloadHoldCycle, 1.0f );

						const float flFinishFrame = VRPistol_FrameFinishEnd(iPistolID);
						m_flPistolFinishEndCycle = flFinishFrame < 0.0f
							? 1.0f
							: clamp( flFinishFrame / (float)maxFrame, m_flShotgunManualReloadCommitCycle, 1.0f );
					}
				}
			}

			DevMsg("VR: Pistol manual reload sequence '%s': seq=%d magFree=%.3f pause=%.3f hold=%.3f target=%.3f commit=%.3f finish=%.3f on '%s'\n",
				pszPistolReloadSeq, m_iShotgunManualReloadSequence, m_flPistolMagFreeCycle, m_flPistolPauseCycle,
				m_flShotgunManualReloadHoldCycle, m_flPistolInsertTargetCycle, m_flShotgunManualReloadCommitCycle, m_flPistolFinishEndCycle, GetModelName());
		}
		else if (pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
		{
			m_iReloadLoopSequence  = LookupSequence("sb_reload_loop");

			if (m_iReloadLoopSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iReloadLoopSequence, poseParams);
					if (maxFrame > 0)
						m_flReloadLoopBottomCycle = 10.0f / (float)maxFrame;
				}
			}

			DevMsg("VR: Stickybomb reload sequences: loop=%d bottomCycle=%.3f on '%s'\n",
				m_iReloadLoopSequence, m_flReloadLoopBottomCycle, GetModelName());
		}
		else if (pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN)
		{
			m_iReloadLoopSequence  = LookupSequence("bison_reload_loop");

			if (m_iReloadLoopSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iReloadLoopSequence, poseParams);
					if (maxFrame > 0)
						m_flReloadLoopBottomCycle = 5.0f / (float)maxFrame;
				}
			}

			DevMsg("VR: Bison reload sequences: loop=%d bottomCycle=%.3f on '%s'\n",
				m_iReloadLoopSequence, m_flReloadLoopBottomCycle, GetModelName());
		}
		else if (pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON)
		{
			m_iReloadLoopSequence  = LookupSequence("mangler_reload_loop");

			if (m_iReloadLoopSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iReloadLoopSequence, poseParams);
					if (maxFrame > 0)
						m_flReloadLoopBottomCycle = 12.0f / (float)maxFrame;
				}
			}

			DevMsg("VR: Mangler reload sequences: loop=%d bottomCycle=%.3f on '%s'\n",
				m_iReloadLoopSequence, m_flReloadLoopBottomCycle, GetModelName());
		}
		else if (pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON)
		{
			m_iReloadLoopSequence  = LookupSequence("pomson_reload_loop");

			if (m_iReloadLoopSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iReloadLoopSequence, poseParams);
					if (maxFrame > 0)
						m_flReloadLoopBottomCycle = 6.0f / (float)maxFrame;
				}
			}

			DevMsg("VR: Pomson reload sequences: loop=%d bottomCycle=%.3f on '%s'\n",
				m_iReloadLoopSequence, m_flReloadLoopBottomCycle, GetModelName());
		}

		m_bAnimateIdle = false;
		m_bLoopIdleOnHand = false;
		m_bIsBreadBite = false;
		m_bBreadCreaturePin = false;

		// Bread creature weapons: hand plays the animation at rate 1.0,
		// bone merge copies vm_weapon bones to the render weapon each frame.
		// One entity drives both visual and audio — no desync.
		// Jar weapons (Mutated Milk, Beauty Mark) pin the hand to the controller.
		// Bread Sapper lets the hand follow the animation's weapon_bone.
		if (pWeapon->GetWeaponID() == TF_WEAPON_JAR_MILK)
		{
			CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
			if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1121)
			{
				m_bAnimateIdle = true;
				m_bLoopIdleOnHand = true;
				m_bBreadCreaturePin = true;
			}
		}
		if (pWeapon->GetWeaponID() == TF_WEAPON_JAR)
		{
			CEconItemView *pItem = pWeapon->GetAttributeContainer()->GetItem();
			if (pItem && pItem->IsValid() && pItem->GetItemDefIndex() == 1105)
			{
				m_bAnimateIdle = true;
				m_bLoopIdleOnHand = true;
				m_bBreadCreaturePin = true;
			}
		}
		if (IsBreadBite(pWeapon))
		{
			m_bIsBreadBite = true;
		}

		// Sapper/builder: the idle animation lives on the hand model.
		if (playerClass == TF_CLASS_SPY &&
			(V_stristr(weaponClass, "sapper") || V_stristr(weaponClass, "builder")))
		{
			m_bLoopIdleOnHand = true;
			const char *worldModel = pWeapon->GetWorldModel();
			if (worldModel && V_stristr(worldModel, "breadmonster"))
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
		m_iBowIdleSequence = -1;
		m_flBowIdleCycle = 0.0f;
		m_flBowFireEndCycle = 1.0f;
		m_flBowFireFirstFrameCycle = 0.0f;
		m_iBowDrawSequence = -1;
		m_flBowDrawNockCycle = 0.0f;
		m_iBowChargeIdleSequence = -1;
		m_iBowShakeOverlaySequence = -1;
		m_flBowShakeOverlayStartTime = 0.0f;
		m_bBowShakeOverlayActive = false;
		m_bBowFireStartPoseValid = false;
		m_iBowFireStartPoseBoneCount = 0;
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
		if (pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		{
			extern ConVar tfvr_weapon_fire_anim_debug;
			const float flBowIdleFrame = 10.0f;
			const float flBowFireEndFrame = 15.0f;
			m_iBowIdleSequence = LookupSequence("bw_fire");
			if (m_iBowIdleSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iBowIdleSequence, poseParams);
					if (maxFrame > 0)
					{
						m_flBowFireFirstFrameCycle = clamp(1.0f / (float)maxFrame, 0.0f, 1.0f);
						m_flBowIdleCycle = clamp(flBowIdleFrame / (float)maxFrame, 0.0f, 1.0f);
						m_flBowFireEndCycle = clamp(flBowFireEndFrame / (float)maxFrame, m_flBowIdleCycle, 1.0f);
					}
				}
			}

			m_iBowChargeIdleSequence = LookupSequence("bw_idle3");

			// bw_draw frame 35: the fully-nocked drawn pose (bow + drawstring +
			// arrow + draw-hand grip authored together). This is the base of the
			// nock/pull pose; the charge animation blends in on top of it.
			const float flBowDrawNockFrame = 35.0f;
			m_iBowDrawSequence = LookupSequence("bw_draw");
			m_flBowDrawNockCycle = 0.0f;
			if (m_iBowDrawSequence >= 0)
			{
				CStudioHdr *pHdr = GetModelPtr();
				if (pHdr)
				{
					float poseParams[MAXSTUDIOPOSEPARAM] = {};
					int maxFrame = Studio_MaxFrame(pHdr, m_iBowDrawSequence, poseParams);
					if (maxFrame > 0)
						m_flBowDrawNockCycle = clamp(flBowDrawNockFrame / (float)maxFrame, 0.0f, 1.0f);
				}
			}

			if (tfvr_weapon_fire_anim_debug.GetBool())
			{
				DevMsg("VR: Bow idle sequence 'bw_fire' frame %.1f (seq %d cycle %.3f) on model '%s'\n",
					flBowIdleFrame, m_iBowIdleSequence, m_flBowIdleCycle, GetModelName());
				DevMsg("VR: Bow fire end frame %.1f (cycle %.3f) on model '%s'\n",
					flBowFireEndFrame, m_flBowFireEndCycle, GetModelName());
				DevMsg("VR: Bow draw nock 'bw_draw' frame %.1f (seq %d cycle %.3f) on model '%s'\n",
					flBowDrawNockFrame, m_iBowDrawSequence, m_flBowDrawNockCycle, GetModelName());
				DevMsg("VR: Bow charge idle sequence 'bw_idle3' (seq %d) on model '%s'\n",
					m_iBowChargeIdleSequence, GetModelName());
			}
		}

		// Look up draw/deploy animation and scope
		m_iDrawSequence = -1;
		m_bPlayingDrawAnim = false;
		m_eDrawAnimScope = VR_DRAW_ANIM_NONE;
		const char *drawAnimName = GetWeaponDrawAnimation(playerClass, weaponClass, pWeapon);
		VRDrawAnimScope drawScope = GetWeaponDrawAnimScope(playerClass, weaponClass, pWeapon);
		if (drawAnimName && drawAnimName[0] && drawScope != VR_DRAW_ANIM_NONE)
		{
			extern ConVar tfvr_weapon_draw_anim_debug;

			char vrDrawName[128];
			Q_snprintf(vrDrawName, sizeof(vrDrawName), "vr_%s", drawAnimName);
			m_iDrawSequence = LookupSequence(vrDrawName);

			if (m_iDrawSequence >= 0)
			{
				if (tfvr_weapon_draw_anim_debug.GetBool())
				{
					DevMsg("VR: Draw sequence using VR override '%s' (seq %d) on model '%s'\n",
						vrDrawName, m_iDrawSequence, GetModelName());
				}
			}
			else
			{
				m_iDrawSequence = LookupSequence(drawAnimName);
				if (tfvr_weapon_draw_anim_debug.GetBool())
				{
					DevMsg("VR: Draw sequence fallback to '%s' (seq %d) on model '%s'\n",
						drawAnimName, m_iDrawSequence, GetModelName());
				}
			}

			m_eDrawAnimScope = drawScope;
		}

		// Also pass fire sequence to render weapon (in case it has its own animations)
		pRenderWeapon->SetFireSequence(m_iFireSequence);

		// For medigun, also look up fire_on and fire_off sequences
		if (IsWeaponMedigun(pWeapon))
		{
			m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
			m_bMedigunWasHealing = false;
			m_bMedigunLeverActive = false;
			m_iMedigunLeverSeq = -1;
			m_flMedigunLeverCycle = 0.0f;
			m_bMedigunBodyPastHalf = false;

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

	if (IsScattergunWeaponID(pWeapon->GetWeaponID()))
		pRenderWeapon->SetupReloadAnimations( "sg" );
	else if (pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
		pRenderWeapon->SetupReloadAnimations( "sb" );
	else if (pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN)
		pRenderWeapon->SetupReloadAnimations( "bison" );
	else if (pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON)
		pRenderWeapon->SetupReloadAnimations( "mangler" );
	else if (pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON)
		pRenderWeapon->SetupReloadAnimations( "pomson" );
	else if (IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID()))
		pRenderWeapon->SetupReloadLoopAnimation( "reload_loop" );

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

	// Bread creature weapons: set the render weapon to the same idle animation
	// as the hand but at rate 0 (frozen). The hand drives the cycle per-frame
	// via SetCycle in ClientThink. Bone merge copies vm_weapon bones from the
	// hand; weapon-only bones (e.g. sapper mechanism) use the render weapon's
	// own animation at the synced cycle.
	if (m_bAnimateIdle)
	{
		C_TFPlayer *pIdleOwner = GetOwnerPlayer();
		int weaponIdleSeq = -1;
		if (pIdleOwner)
		{
			int pc = pIdleOwner->GetPlayerClass()->GetClassIndex();
			const char *wc = pWeapon->GetClassname();
			const char *idleName = GetWeaponPoseAnimation(pc, wc, pWeapon);
			if (idleName && idleName[0])
			{
				weaponIdleSeq = pRenderWeapon->LookupSequence(idleName);
			}
		}
		if (weaponIdleSeq < 0)
			weaponIdleSeq = pRenderWeapon->LookupSequence("c_sapper_idle");
		if (weaponIdleSeq < 0)
			weaponIdleSeq = pRenderWeapon->LookupSequence("idle");
		if (weaponIdleSeq >= 0)
		{
			pRenderWeapon->SetSequence(weaponIdleSeq);
			pRenderWeapon->SetCycle(0.0f);
			pRenderWeapon->SetPlaybackRate(0.0f);
			pRenderWeapon->SetIdleSequence(weaponIdleSeq);
		}
		else
		{
			pRenderWeapon->SetPlaybackRate(0.0f);
		}
	}

	// Bread creatures (not Bread Bite): start idle on the hand at rate 1.0.
	// Bone merge copies vm_weapon bones to the render weapon, so the hand
	// drives both the creature visual and sound events — keeping them in sync.
	if (m_bAnimateIdle && !m_bIsBreadBite && m_iIdleSequence >= 0)
	{
		SetSequence(m_iIdleSequence);
		SetCycle(0.0f);
		SetPlaybackRate(1.0f);
	}

	// Bread Bite: set up hand-side sequences for swing, crit, and idle cycling
	if (m_bIsBreadBite)
	{
		m_iFireSequence = LookupSequence("breadglove_swing_right");
		m_iBreadBiteCritSeq = LookupSequence("breadglove_swing_crit");
		m_iBreadBiteIdleSeqs[0] = LookupSequence("breadglove_idle_A");
		m_iBreadBiteIdleSeqs[1] = LookupSequence("breadglove_idle_B");
		m_iBreadBiteIdleSeqs[2] = LookupSequence("breadglove_idle_C");
		m_bPrevVRSwingActive = false;
		m_flBreadBiteIdleStartTime = gpGlobals->curtime;

		// Start the first idle playing on the entity with rate 1.0
		// so StudioFrameAdvance advances it and SetupBones uses the live pose
		if (m_iIdleSequence >= 0)
		{
			SetSequence(m_iIdleSequence);
			SetCycle(0.0f);
			SetPlaybackRate(1.0f);
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

			// Cache the idle weapon_bone transform relative to hand bone.
			// This keeps HUD elements stable during draw and backstab animations.
			int wpnBone = LookupBone("weapon_bone");
			if (wpnBone >= 0 && wpnBone < numBones)
			{
				matrix3x4_t invHand;
				MatrixInvert(boneToWorld[m_iHandBone], invHand);
				ConcatTransforms(invHand, boneToWorld[wpnBone], m_matIdleWeaponBoneLocal);
				m_bHasIdleWeaponBone = true;
			}

			if (tfvr_hands_debug.GetBool())
			{
				Vector pos;
				QAngle angles;
				MatrixAngles(m_matIdleHandBoneTransform, angles, pos);
				DevMsg("VR Hand: Sampled idle pose directly - hand bone pos: (%.1f, %.1f, %.1f)\n", pos.x, pos.y, pos.z);
			}
		}
	}

	// Trigger draw animation after all weapon setup is complete.
	// This must come AFTER the idle hand bone offset is cached so that
	// FULL_ARM scope has a valid anchor to compute displacement from.
	// Always call this — even with NONE scope the render weapon may
	// have its own draw animation (bread creatures).
	PlayWeaponDrawAnimation();
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
	m_bPomsonUseReloadGrip = false;
	m_bRightHandDetached = false;
	m_bPomsonRightGripLatched = false;
	m_bPomsonRightLatchOffsetValid = false;
	m_bPomsonRightGripLastWorldValid = false;
	m_bPomsonRightUnlatchStartValid = false;
	m_bPomsonRightUnlatchUseReloadGrip = false;
	m_bPomsonRightGripWasPressed = false;
	m_bPomsonSuppressPassiveGripPoint = false;
	m_bPomsonSuppressReloadGripPoint = false;
	m_bOffHandToWeaponBoneValid = false;
	m_bLiveBowStringBoneWorldValid = false;
	SetIdentityMatrix( m_matPomsonDetachLeftToWeaponBone );
	m_bPomsonDetachLeftToWeaponBoneValid = false;
	SetIdentityMatrix( m_matPomsonDetachLeftToLeftHandBone );
	m_bPomsonDetachLeftToLeftHandBoneValid = false;

	// Reset animation state so next weapon/grip uses fresh lookups
	m_iIdleSequence = -1;
	m_bAnimateIdle = false;
	m_bLoopIdleOnHand = false;
	m_bIsBreadBite = false;
	m_iBreadBiteCritSeq = -1;
	m_iBreadBiteIdleSeqs[0] = m_iBreadBiteIdleSeqs[1] = m_iBreadBiteIdleSeqs[2] = -1;
	m_flBreadBiteIdleStartTime = 0.0f;
	m_iBBCrossfadeFromSeq = -1;
	m_flBBCrossfadeFromCycle = 0.0f;
	m_flBBCrossfadeStart = 0.0f;
	m_iBBLastSampledSeq = -1;
	m_flBBLastSampledCycle = 0.0f;
	m_flBBLastCrossfadeCheck = 0.0f;
	m_iFireSequence = -1;
	m_iAltFireSequence = -1;
	m_iChargeSequence = -1;
	m_iChargeSequence2 = -1;
	m_iBowIdleSequence = -1;
	m_flBowIdleCycle = 0.0f;
	m_flBowFireEndCycle = 1.0f;
	m_flBowFireFirstFrameCycle = 0.0f;
	m_iBowDrawSequence = -1;
	m_flBowDrawNockCycle = 0.0f;
	m_iBowChargeIdleSequence = -1;
	m_iBowShakeOverlaySequence = -1;
	m_flBowShakeOverlayStartTime = 0.0f;
	m_bBowShakeOverlayActive = false;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;
	m_iDrawSequence = -1;
	m_bPlayingFireAnim = false;
	m_bPlayingChargeAnim = false;
	m_bPlayingDrawAnim = false;
	m_flFireAnimStartTime = 0.0f;
	m_flDrawAnimStartTime = 0.0f;
	m_eDrawAnimScope = VR_DRAW_ANIM_NONE;
	m_bHandBoneOffsetValid = false;
	m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
	m_iFireOnSequence = -1;
	m_iFireOffSequence = -1;
	m_bMedigunWasHealing = false;
	m_bMedigunLeverActive = false;
	m_iMedigunLeverSeq = -1;
	m_flMedigunLeverCycle = 0.0f;
	m_bMedigunBodyPastHalf = false;
	m_bFlamethrowerWasFiring = false;
	m_flFlamethrowerFireBlend = 0.0f;

	// Reset reload animation state
	m_iReloadStartSequence = -1;
	m_iReloadLoopSequence = -1;
	m_iReloadEndSequence = -1;
	m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
	m_flReloadAnimStartTime = 0.0f;
	m_flReloadLoopBottomCycle = 0.0f;
	m_iShotgunManualReloadSequence = -1;
	m_flShotgunManualReloadHoldCycle = 0.0f;
	m_flShotgunManualReloadCommitCycle = 1.0f;
	m_flPistolOneFrameCycle = 0.0f;
	m_flPistolMagFreeCycle = 0.0f;
	m_flPistolPauseCycle = 1.0f;
	m_flPistolInsertTargetCycle = 0.0f;
	m_flPistolFinishEndCycle = 1.0f;
	m_bPistolReloadBlendOut = false;
	m_flPistolReloadBlendOutStartTime = 0.0f;
	m_flPistolReloadAnimWeight = 1.0f;
	m_bShotgunManualReloadPoseActive = false;
	m_bShotgunManualReloadBlendOutActive = false;
	m_flShotgunManualReloadBlendOutStartTime = 0.0f;
	m_nShotgunManualReloadBlendOutBones = 0;
	m_bPlayingReloadAnim = false;
	m_iLeverReloadSequence = -1;
	m_flLeverReloadCycle = 0.0f;

	// Clean up the pistol manual reload magazines (gun mag on this hand,
	// held mag on the opposite hand).
	RemovePistolMagazineModel();
	{
		C_TFVRHand *pOtherHand = GetOppositeVRHand(this);
		if (pOtherHand)
			pOtherHand->RemovePistolMagazineModel();
	}

	// Reset two-hand grip state
	m_flTwoHandBlend = 0.0f;
	m_bOffhandGripActive = false;
	m_bWasOffhandGripActive = false;
	m_flGripRotationBlend = 0.0f;

	// Clean up weapon-dependent left hand wearables when right hand unequips.
	// Watch and shield are persistent wearables managed by their Update functions.
	if (IsRightHand())
	{
		C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
		if (pLeftHand)
		{
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
	m_iLastEquippedWeaponID = -1;
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

	// The render weapon handles all visual rendering in VR.
	// Hide the source weapon entity so it doesn't draw on top with
	// its own (server-driven) animation — that causes desync artifacts
	// on bread creatures and doubled geometry on everything else.
	// Also freeze its animation so its StudioFrameAdvance doesn't
	// fire duplicate sound events.
	if (m_hRenderWeapon.Get())
	{
		pWeapon->AddEffects(EF_NODRAW);
		pWeapon->SetPlaybackRate(0.0f);

		// Bread creature weapons: sync the viewmodel's animation to the
		// hand so both fire sound events at the same instant.  The
		// doubled audio reinforces the quiet spatialized sounds rather
		// than creating an echo.  The hand drives both draw and idle.
		if (m_bAnimateIdle)
		{
			C_TFPlayer *pOwner = GetOwnerPlayer();
			C_BaseViewModel *pVM = pOwner ? pOwner->GetViewModel(0) : NULL;
			if (pVM)
			{
				CStudioHdr *pHandHdr = GetModelPtr();
				int handSeq = GetSequence();
				if (pHandHdr && handSeq >= 0 && handSeq < pHandHdr->GetNumSeq())
				{
					mstudioseqdesc_t &seqDesc = pHandHdr->pSeqdesc(handSeq);
					int vmSeq = pVM->LookupSequence(seqDesc.pszLabel());
					if (vmSeq >= 0)
					{
						if (pVM->GetSequence() != vmSeq)
							pVM->SetSequence(vmSeq);
						pVM->SetCycle(GetCycle());
					}
				}
			}
		}
	}
	else
	{
		pWeapon->RemoveEffects(EF_NODRAW);
	}
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
		// Sync team number for paint kit / war paint material overrides
		if (pRenderWeapon->GetTeamNumber() != iTeamNumber)
			pRenderWeapon->ChangeTeam(iTeamNumber);

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
//          When VR lever is active, animations are scrubbed by lever progress
//          instead of driven by heal target boolean.
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
			m_bMedigunLeverActive = false;
			m_bMedigunBodyPastHalf = false;
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

	CWeaponMedigun *pMedigun = static_cast<CWeaponMedigun*>(pWeapon);
	C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());

	// VR lever path: entity sequence drives hand + lever (scrubbed by progress),
	// VR lever path: entity sequence drives the BODY (idle/fire_on/loop/off).
	// SetupBones overrides lever bone + right hand with the lever animation.
	// GetOffHandGripTarget uses the lever state so the right hand follows.
	if (pMedigun->ShouldUseVRLever())
	{
		float flProgress = pMedigun->GetVRLeverProgress();
		bool bEngaged = pMedigun->IsVRLeverEngaged();
		bool bIsHealing = (pMedigun->m_hHealingTarget.Get() != NULL);
		bool bDebug = tfvr_weapon_fire_anim_debug.GetBool();

		// --- Store lever animation state for SetupBones + GetOffHandGripTarget ---
		if (flProgress > 0.0f || bEngaged)
		{
			// Lever is being operated: scrub based on progress
			m_bMedigunLeverActive = true;
			if (!bEngaged)
			{
				m_iMedigunLeverSeq = m_iFireOnSequence;
				m_flMedigunLeverCycle = flProgress;
			}
			else if (flProgress >= 1.0f)
			{
				m_iMedigunLeverSeq = m_iFireOnSequence;
				m_flMedigunLeverCycle = 1.0f;
			}
			else
			{
				m_iMedigunLeverSeq = m_iFireOffSequence;
				m_flMedigunLeverCycle = 1.0f - flProgress;
			}
		}
		else if (m_eMedigunFireState != MEDIGUN_FIRE_IDLE)
		{
			// Lever is at rest but body is still transitioning (fire_off, etc.):
			// pin hand/lever at rest pose so they don't follow the body animation
			m_bMedigunLeverActive = true;
			m_iMedigunLeverSeq = m_iFireOnSequence;
			m_flMedigunLeverCycle = 0.0f;
		}
		else
		{
			m_bMedigunLeverActive = false;
			m_iMedigunLeverSeq = -1;
			m_flMedigunLeverCycle = 0.0f;
		}

		// --- Body animation: entity sequence driven by heal target ---
		bool bNowPastHalf = flProgress >= 0.5f;
		bool bJustCrossedDown = !bNowPastHalf && m_bMedigunBodyPastHalf;
		m_bMedigunBodyPastHalf = bNowPastHalf;

		switch (m_eMedigunFireState)
		{
		case MEDIGUN_FIRE_IDLE:
			if (bIsHealing)
			{
				m_bPlayingDrawAnim = false;
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

				if (bDebug)
					DevMsg("VR Medigun Body: -> FIRE_ON (target acquired)\n");
			}
			break;

		case MEDIGUN_FIRE_ON:
			if (!bIsHealing || bJustCrossedDown)
			{
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
					m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
					m_bPlayingFireAnim = false;
				}

				if (bDebug)
					DevMsg("VR Medigun Body: -> FIRE_OFF (%s)\n",
						bJustCrossedDown ? "lever pulled back" : "lost target");
			}
			else if (GetCycle() >= 1.0f)
			{
				if (m_iFireSequence >= 0)
				{
					SetSequence(m_iFireSequence);
					SetCycle(0.0f);
					SetPlaybackRate(1.0f);
					m_eMedigunFireState = MEDIGUN_FIRE_LOOP;
					InvalidateBoneCache();

					if (pRenderWeapon)
						pRenderWeapon->PlayMedigunSequence(MEDIGUN_FIRE_LOOP);

					if (bDebug)
						DevMsg("VR Medigun Body: -> FIRE_LOOP\n");
				}
			}
			break;

		case MEDIGUN_FIRE_LOOP:
			if (!bIsHealing || bJustCrossedDown)
			{
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
					m_eMedigunFireState = MEDIGUN_FIRE_IDLE;
					m_bPlayingFireAnim = false;
				}

				if (bDebug)
					DevMsg("VR Medigun Body: -> FIRE_OFF (%s)\n",
						bJustCrossedDown ? "lever pulled back" : "lost target");
			}
			break;

		case MEDIGUN_FIRE_OFF:
			if (bIsHealing && bNowPastHalf)
			{
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

				if (bDebug)
					DevMsg("VR Medigun Body: -> FIRE_ON (target during OFF)\n");
			}
			else if (GetCycle() >= 1.0f || (gpGlobals->curtime - m_flFireAnimStartTime) > 2.0f)
			{
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

				if (bDebug)
					DevMsg("VR Medigun Body: -> IDLE (fire_off done)\n");
			}
			break;
		}

		// Sync lever bone override to render weapon
		if (pRenderWeapon)
		{
			if (m_bMedigunLeverActive)
			{
				int iType = (m_iMedigunLeverSeq == m_iFireOffSequence) ? 1 : 0;
				pRenderWeapon->SetMedigunLeverBone(true, iType, m_flMedigunLeverCycle);
			}
			else
			{
				pRenderWeapon->SetMedigunLeverBone(false, 0, 0.0f);
			}
		}

		m_bMedigunWasHealing = bIsHealing;
		return;
	}

	// Clear lever override when not using VR lever
	m_bMedigunLeverActive = false;

	// Non-VR-lever path: original boolean-driven state machine
	bool bIsHealing = (pMedigun->m_hHealingTarget.Get() != NULL);

	switch (m_eMedigunFireState)
	{
	case MEDIGUN_FIRE_IDLE:
		if (bIsHealing)
		{
			m_bPlayingDrawAnim = false;
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
		break;

	case MEDIGUN_FIRE_OFF:
		if (bIsHealing)
		{
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
			m_bPlayingDrawAnim = false;
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
// Purpose: Drive the scattergun lever reload state machine.
//          Stores the lever sequence/cycle for ApplyWeaponPose to sample
//          on weapon_bone_1 only — the entity stays at idle so the rest
//          of the weapon is not affected by the reload animation.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateScattergunReloadAnimation()
{
	if (m_iReloadStartSequence < 0)
		return;

	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || !IsScattergunWeaponID(pWeapon->GetWeaponID()))
	{
		// Don't clear state if another pump function manages it
		if (pWeapon && (pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
			|| IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID())))
			return;

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	CTFScatterGun *pSG = static_cast<CTFScatterGun *>(pWeapon);
	bool bArmed = pSG->IsVRLeverArmed();

	extern ConVar tfvr_scattergun_lever_debug;
	bool bDebug = tfvr_scattergun_lever_debug.GetBool();

	if (m_bPlayingFireAnim)
		return;

	// --- Edge detection: arm / disarm ---
	if (bArmed && m_eReloadAnimState == VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_ENTER;
		m_flReloadAnimStartTime = gpGlobals->curtime;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iReloadStartSequence;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR Lever Anim] Enter reload mode\n");
	}
	else if (!bArmed && m_eReloadAnimState != VR_RELOAD_ANIM_NONE
			&& m_eReloadAnimState != VR_RELOAD_ANIM_EXIT)
	{
		bool bFireCooldown = ( gpGlobals->curtime < pSG->m_flNextPrimaryAttack );
		if (m_iReloadEndSequence >= 0 && !bFireCooldown)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_EXIT;
			m_flReloadAnimStartTime = gpGlobals->curtime;
			m_iLeverReloadSequence = m_iReloadEndSequence;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR Lever Anim] Exit reload mode\n");
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
	}

	// --- State machine: compute lever sequence and cycle ---
	switch (m_eReloadAnimState)
	{
	case VR_RELOAD_ANIM_ENTER:
	{
		CStudioHdr *pHdr = GetModelPtr();
		float duration = pHdr ? SequenceDuration(pHdr, m_iReloadStartSequence) : 0.0f;
		float elapsed = gpGlobals->curtime - m_flReloadAnimStartTime;
		if (elapsed >= duration)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadStartSequence;
			m_flLeverReloadCycle = 1.0f;
			if (bDebug)
				DevMsg("[VR Lever Anim] Holding reload pose\n");
		}
		else
		{
			m_iLeverReloadSequence = m_iReloadStartSequence;
			m_flLeverReloadCycle = (duration > 0.0f) ? (elapsed / duration) : 0.0f;
		}
		break;
	}

	case VR_RELOAD_ANIM_HOLD:
	{
		if (pSG->IsVRLeverPumpingDown() || pSG->IsVRLeverReturning())
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_PUMPING;
			if (bDebug)
				DevMsg("[VR Lever Anim] Pumping started\n");
		}
		m_iLeverReloadSequence = m_iReloadStartSequence;
		m_flLeverReloadCycle = 1.0f;
		break;
	}

	case VR_RELOAD_ANIM_PUMPING:
	{
		if (m_iReloadLoopSequence < 0)
			break;

		float progress = pSG->GetVRLeverStrokeProgress();
		float cycle = 0.0f;

		if (pSG->IsVRLeverPumpingDown())
		{
			cycle = progress * m_flReloadLoopBottomCycle;
		}
		else if (pSG->IsVRLeverReturning())
		{
			cycle = m_flReloadLoopBottomCycle + progress * (1.0f - m_flReloadLoopBottomCycle);
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadStartSequence;
			m_flLeverReloadCycle = 1.0f;
			if (bDebug)
				DevMsg("[VR Lever Anim] Pump complete, back to hold\n");
			break;
		}

		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = clamp(cycle, 0.0f, 1.0f);

		if (bDebug)
			DevMsg("[VR Lever Anim] Pump cycle: %.3f (progress %.2f, down=%d)\n",
				m_flLeverReloadCycle, progress, pSG->IsVRLeverPumpingDown() ? 1 : 0);
		break;
	}

	case VR_RELOAD_ANIM_EXIT:
	{
		CStudioHdr *pHdr = GetModelPtr();
		float duration = pHdr ? SequenceDuration(pHdr, m_iReloadEndSequence) : 0.0f;
		float elapsed = gpGlobals->curtime - m_flReloadAnimStartTime;
		if (elapsed >= duration)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR Lever Anim] Reload exit complete, back to idle\n");
		}
		else
		{
			m_iLeverReloadSequence = m_iReloadEndSequence;
			m_flLeverReloadCycle = (duration > 0.0f) ? (elapsed / duration) : 0.0f;
		}
		break;
	}

	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Shotgun pump action animation. Samples frames 8-16 from the normal
//          (non-vr) fire animation, then clears back to idle when complete.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateShotgunPumpActionAnimation()
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || !IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID()))
	{
		if (pWeapon && (IsScattergunWeaponID(pWeapon->GetWeaponID())
			|| pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON))
			return;

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	if (m_iReloadLoopSequence < 0)
		return;

	CTFShotgun *pShotgun = static_cast<CTFShotgun *>(pWeapon);
	bool bNeedsPump = pShotgun->NeedsVRShotgunPump() || pShotgun->IsReloading();
	bool bArmed = pShotgun->IsVRShotgunPumpArmed();

	extern ConVar tfvr_shotgun_pump_debug;
	bool bDebug = tfvr_shotgun_pump_debug.GetBool();

	if (pShotgun->IsVRShotgunManualReloadActive() && m_iShotgunManualReloadSequence >= 0)
	{
		float flProgress = pShotgun->IsVRShotgunShellInserting()
			? pShotgun->GetVRShotgunShellInsertProgress() : 0.0f;
		m_eReloadAnimState = pShotgun->IsVRShotgunShellInserting()
			? VR_RELOAD_ANIM_PUMPING : VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iShotgunManualReloadSequence;
		m_flLeverReloadCycle = Lerp( flProgress, m_flShotgunManualReloadHoldCycle, m_flShotgunManualReloadCommitCycle );
		return;
	}

	if (!bNeedsPump)
	{
		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR ShotgunPump Anim] Pump complete, back to idle\n");
		}
		return;
	}

	if (bArmed && m_eReloadAnimState == VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = m_flShotgunPumpStartCycle;
		if (bDebug)
			DevMsg("[VR ShotgunPump Anim] Armed\n");
	}
	else if (!bArmed && m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR ShotgunPump Anim] Disarmed, back to idle\n");
		return;
	}

	switch (m_eReloadAnimState)
	{
	case VR_RELOAD_ANIM_HOLD:
	{
		if (pShotgun->IsVRShotgunPumpPullingBack() || pShotgun->IsVRShotgunPumpPushingFwd())
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_PUMPING;
			if (bDebug)
				DevMsg("[VR ShotgunPump Anim] Pumping started\n");
		}
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = m_flShotgunPumpStartCycle;
		break;
	}

	case VR_RELOAD_ANIM_PUMPING:
	{
		float progress = pShotgun->GetVRShotgunPumpStrokeProgress();
		float cycle = m_flShotgunPumpStartCycle;

		if (pShotgun->IsVRShotgunPumpPullingBack())
		{
			cycle = m_flShotgunPumpStartCycle + progress * (m_flReloadLoopBottomCycle - m_flShotgunPumpStartCycle);
		}
		else if (pShotgun->IsVRShotgunPumpPushingFwd())
		{
			cycle = m_flReloadLoopBottomCycle + progress * (m_flShotgunPumpEndCycle - m_flReloadLoopBottomCycle);
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadLoopSequence;
			m_flLeverReloadCycle = m_flShotgunPumpStartCycle;
			break;
		}

		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = clamp(cycle, m_flShotgunPumpStartCycle, m_flShotgunPumpEndCycle);

		if (bDebug)
			DevMsg("[VR ShotgunPump Anim] Pump cycle: %.3f (progress %.2f, pullback=%d)\n",
				m_flLeverReloadCycle, progress, pShotgun->IsVRShotgunPumpPullingBack() ? 1 : 0);
		break;
	}

	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Pistol manual magazine reload animation. The weapon hand samples
//          p_reload at a cycle driven by the weapon's reload phase:
//            EJECTING  : frames 0-16 over the eject duration
//            (mag out) : paused at frame 16
//            INSERTING : frames 16-19 in sync with the off-hand insert
//            FINISHING : frame 19 -> end
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdatePistolReloadAnimation()
{
	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	CTFPistol *pPistol = TFVR_GetManualReloadPistol(pWeapon);
	if (!pPistol)
		return;

	if (m_iShotgunManualReloadSequence < 0)
		return;

	const int iPhase = pPistol->GetVRMagPhase();
	const float flProgress = pPistol->GetVRMagPhaseProgress();

	float flCycle = -1.0f;
	switch (iPhase)
	{
	case VR_PISTOL_MAG_PHASE_EJECTING:
		flCycle = flProgress * m_flPistolPauseCycle;
		break;
	case VR_PISTOL_MAG_PHASE_INSERTING:
		flCycle = Lerp(flProgress, m_flPistolPauseCycle, m_flShotgunManualReloadCommitCycle);
		break;
	case VR_PISTOL_MAG_PHASE_FINISHING:
		flCycle = Lerp(flProgress, m_flShotgunManualReloadCommitCycle, m_flPistolFinishEndCycle);
		break;
	default:
		if (pPistol->IsVRMagOut())
			flCycle = m_flPistolPauseCycle; // paused, waiting for a fresh mag
		break;
	}

	if (flCycle >= 0.0f)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iShotgunManualReloadSequence;
		m_flLeverReloadCycle = clamp(flCycle, 0.0f, 1.0f);
		m_bPistolReloadBlendOut = false;
		m_flPistolReloadAnimWeight = 1.0f;
	}
	else if (m_bPlayingReloadAnim || m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		// Reload finished: blend the weapon hand back to the idle pose
		// instead of snapping (the finish motion can stop mid-sequence,
		// e.g. engineer stops at frame 20).
		const float flBlendTime = MAX(tfvr_pistol_reload_blend_out.GetFloat(), 0.0f);
		if (!m_bPistolReloadBlendOut)
		{
			if (flBlendTime > 0.0f)
			{
				m_bPistolReloadBlendOut = true;
				m_flPistolReloadBlendOutStartTime = gpGlobals->curtime;
				// Freeze the pose at the end of the finish motion while it fades.
				m_flLeverReloadCycle = m_flPistolFinishEndCycle;
			}
		}

		float flWeight = 0.0f;
		if (m_bPistolReloadBlendOut && flBlendTime > 0.0f)
		{
			flWeight = 1.0f - clamp((gpGlobals->curtime - m_flPistolReloadBlendOutStartTime) / flBlendTime, 0.0f, 1.0f);
		}

		if (flWeight > 0.0f)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_bPlayingReloadAnim = true;
			m_iLeverReloadSequence = m_iShotgunManualReloadSequence;
			m_flPistolReloadAnimWeight = flWeight;
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
			m_bPistolReloadBlendOut = false;
			m_flPistolReloadAnimWeight = 1.0f;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Stickybomb launcher VR pump reload animation — mirrors the
//          scattergun lever animation but reads from CTFPipebombLauncher.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateStickyPumpReloadAnimation()
{
	if (m_iReloadLoopSequence < 0)
		return;

	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_PIPEBOMBLAUNCHER)
	{
		if (pWeapon && (IsScattergunWeaponID(pWeapon->GetWeaponID())
			|| pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
			|| IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID())))
			return;

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	CTFPipebombLauncher *pSB = static_cast<CTFPipebombLauncher *>(pWeapon);
	bool bArmed = pSB->IsVRPumpArmed();

	extern ConVar tfvr_sticky_pump_debug;
	bool bDebug = tfvr_sticky_pump_debug.GetBool();

	if (m_bPlayingFireAnim)
		return;

	// --- Edge detection: arm / disarm ---
	if (bArmed && m_eReloadAnimState == VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR StickyPump Anim] Armed – holding at loop start\n");
	}
	else if (!bArmed && m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR StickyPump Anim] Disarmed – back to idle\n");
	}

	// --- State machine (only HOLD and PUMPING, no start/end transitions) ---
	switch (m_eReloadAnimState)
	{
	case VR_RELOAD_ANIM_HOLD:
	{
		if (pSB->IsVRPumpPullingBack() || pSB->IsVRPumpPushingFwd())
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_PUMPING;
			if (bDebug)
				DevMsg("[VR StickyPump Anim] Pumping started\n");
		}
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		break;
	}

	case VR_RELOAD_ANIM_PUMPING:
	{
		float progress = pSB->GetVRPumpStrokeProgress();
		float cycle = 0.0f;

		if (pSB->IsVRPumpPullingBack())
		{
			cycle = progress * m_flReloadLoopBottomCycle;
		}
		else if (pSB->IsVRPumpPushingFwd())
		{
			cycle = m_flReloadLoopBottomCycle + progress * (1.0f - m_flReloadLoopBottomCycle);
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadLoopSequence;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR StickyPump Anim] Pump complete, back to hold\n");
			break;
		}

		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = clamp(cycle, 0.0f, 1.0f);

		if (bDebug)
			DevMsg("[VR StickyPump Anim] Pump cycle: %.3f (progress %.2f, pullback=%d)\n",
				m_flLeverReloadCycle, progress, pSB->IsVRPumpPullingBack() ? 1 : 0);
		break;
	}

	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Righteous Bison VR pump reload animation — mirrors the stickybomb
//          pump animation but reads from CTFRaygun.
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateBisonPumpReloadAnimation()
{
	if (m_iReloadLoopSequence < 0)
		return;

	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_RAYGUN)
	{
		if (pWeapon && (IsScattergunWeaponID(pWeapon->GetWeaponID())
			|| pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
			|| IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID())))
			return;

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	CTFRaygun *pBison = static_cast<CTFRaygun *>(pWeapon);
	bool bArmed = pBison->IsVRPumpArmed();

	extern ConVar tfvr_bison_pump_debug;
	bool bDebug = tfvr_bison_pump_debug.GetBool();

	if (m_bPlayingFireAnim)
		return;

	if (bArmed && m_eReloadAnimState == VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR BisonPump Anim] Armed – holding at loop start\n");
	}
	else if (!bArmed && m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR BisonPump Anim] Disarmed – back to idle\n");
	}

	switch (m_eReloadAnimState)
	{
	case VR_RELOAD_ANIM_HOLD:
	{
		if (pBison->IsVRPumpPullingBack() || pBison->IsVRPumpPushingFwd())
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_PUMPING;
			if (bDebug)
				DevMsg("[VR BisonPump Anim] Pumping started\n");
		}
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		break;
	}

	case VR_RELOAD_ANIM_PUMPING:
	{
		float progress = pBison->GetVRPumpStrokeProgress();
		float cycle = 0.0f;

		if (pBison->IsVRPumpPullingBack())
		{
			cycle = progress * m_flReloadLoopBottomCycle;
		}
		else if (pBison->IsVRPumpPushingFwd())
		{
			cycle = m_flReloadLoopBottomCycle + progress * (1.0f - m_flReloadLoopBottomCycle);
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadLoopSequence;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR BisonPump Anim] Pump complete, back to hold\n");
			break;
		}

		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = clamp(cycle, 0.0f, 1.0f);

		if (bDebug)
			DevMsg("[VR BisonPump Anim] Pump cycle: %.3f (progress %.2f, pullback=%d)\n",
				m_flLeverReloadCycle, progress, pBison->IsVRPumpPullingBack() ? 1 : 0);
		break;
	}

	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Cow Mangler VR pump reload animation — 3-phase (up/down/return).
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateManglerPumpReloadAnimation()
{
	if (m_iReloadLoopSequence < 0)
		return;

	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_PARTICLE_CANNON)
	{
		if (pWeapon && (IsScattergunWeaponID(pWeapon->GetWeaponID())
			|| pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| pWeapon->GetWeaponID() == TF_WEAPON_DRG_POMSON
			|| IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID())))
			return;

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	CTFParticleCannon *pMangler = static_cast<CTFParticleCannon *>(pWeapon);
	bool bArmed = pMangler->IsVRPumpArmed();

	extern ConVar tfvr_mangler_pump_debug;
	bool bDebug = tfvr_mangler_pump_debug.GetBool();

	if (m_bPlayingFireAnim)
		return;

	if (bArmed && m_eReloadAnimState == VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR ManglerPump Anim] Armed – holding at loop start\n");
	}
	else if (!bArmed && m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR ManglerPump Anim] Disarmed – back to idle\n");
	}

	// Cycle boundaries derived from frame layout:
	//   Phase 1 up:     frames 0-12  → cycle 0 to bottomCycle
	//   Phase 2 down:   frames 13-20 → cycle bottomCycle to midCycle
	//   Phase 3 return: frames 20-24 → cycle midCycle to 1.0
	// bottomCycle = 12/maxFrame (stored in m_flReloadLoopBottomCycle)
	// midCycle    = 20/maxFrame = bottomCycle * (20/12)
	float bottomCycle = m_flReloadLoopBottomCycle;
	float midCycle = bottomCycle * (20.0f / 12.0f);

	switch (m_eReloadAnimState)
	{
	case VR_RELOAD_ANIM_HOLD:
	{
		if (pMangler->GetVRPumpPhase() > 0)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_PUMPING;
			if (bDebug)
				DevMsg("[VR ManglerPump Anim] Pumping started\n");
		}
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		break;
	}

	case VR_RELOAD_ANIM_PUMPING:
	{
		float progress = pMangler->GetVRPumpStrokeProgress();
		int phase = pMangler->GetVRPumpPhase();
		float cycle = 0.0f;

		if (phase == 1)
		{
			// Up: 0 → bottomCycle
			cycle = progress * bottomCycle;
		}
		else if (phase == 2)
		{
			// Down: bottomCycle → midCycle
			cycle = bottomCycle + progress * (midCycle - bottomCycle);
		}
		else if (phase == 3)
		{
			// Return: midCycle → 1.0
			cycle = midCycle + progress * (1.0f - midCycle);
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadLoopSequence;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR ManglerPump Anim] Pump complete, back to hold\n");
			break;
		}

		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = clamp(cycle, 0.0f, 1.0f);

		if (bDebug)
			DevMsg("[VR ManglerPump Anim] Pump cycle: %.3f (progress %.2f, phase=%d)\n",
				m_flLeverReloadCycle, progress, phase);
		break;
	}

	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Pomson VR pump reload animation — 2-phase, right hand pumps.
//          Mirrors the Bison pump animation but reads from CTFDRGPomson
//          (which inherits CTFRaygun's VR pump state).
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdatePomsonPumpReloadAnimation()
{
	if (m_iReloadLoopSequence < 0)
		return;

	C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
	if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_DRG_POMSON)
	{
		if (pWeapon && (IsScattergunWeaponID(pWeapon->GetWeaponID())
			|| pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER
			|| pWeapon->GetWeaponID() == TF_WEAPON_RAYGUN
			|| pWeapon->GetWeaponID() == TF_WEAPON_PARTICLE_CANNON
			|| IsPumpActionShotgunWeaponID(pWeapon->GetWeaponID())))
			return;

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	if (gpGlobals->curtime < pWeapon->m_flNextPrimaryAttack
		&& m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		return;
	}

	bool bPumpGripActive = m_bRightHandDetached && m_bPomsonUseReloadGrip
		&& (m_bPomsonRightGripLatched || m_flTwoHandBlend > 0.01f);
	bool bHeldPomsonSlide = m_eReloadAnimState != VR_RELOAD_ANIM_NONE
		&& m_iLeverReloadSequence >= 0;
	if (!bPumpGripActive)
	{
		if (bHeldPomsonSlide)
		{
			// The pump hand is not actively driving the animation. Keep the
			// weapon slide at its last cycle, but let both hands use non-pump
			// poses (including the idle/main grip).
			m_bPlayingReloadAnim = false;
			return;
		}

		if (m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
			m_bPlayingReloadAnim = false;
			m_iLeverReloadSequence = -1;
			m_flLeverReloadCycle = 0.0f;
		}
		return;
	}

	CTFRaygun *pPomson = static_cast<CTFRaygun *>(pWeapon);
	bool bArmed = pPomson->IsVRPumpArmed();

	extern ConVar tfvr_pomson_pump_debug;
	bool bDebug = tfvr_pomson_pump_debug.GetBool();

	if (m_bPlayingFireAnim)
		return;

	if (bPumpGripActive && bHeldPomsonSlide)
	{
		// Re-entering the reload grip should make the right hand follow the
		// held/pumping slide again. Idle/main grip remains slide-only because
		// bPumpGripActive is false there.
		m_bPlayingReloadAnim = true;
	}

	if (bArmed && m_eReloadAnimState == VR_RELOAD_ANIM_NONE)
	{
		m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
		m_bPlayingReloadAnim = true;
		m_bPlayingDrawAnim = false;
		m_bPlayingChargeAnim = false;
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR PomsonPump Anim] Armed – holding at loop start\n");
	}
	else if (!bArmed && m_eReloadAnimState != VR_RELOAD_ANIM_NONE)
	{
		if (m_bRightHandDetached)
		{
			// Letting go of the pump while detached should leave the slide at
			// its last authored cycle. Continue driving the hand only while it
			// is actually on the reload/pump grip.
			m_bPlayingReloadAnim = bPumpGripActive;
			if (m_iLeverReloadSequence < 0)
				m_iLeverReloadSequence = m_iReloadLoopSequence;
			return;
		}

		m_eReloadAnimState = VR_RELOAD_ANIM_NONE;
		m_bPlayingReloadAnim = false;
		m_iLeverReloadSequence = -1;
		m_flLeverReloadCycle = 0.0f;
		if (bDebug)
			DevMsg("[VR PomsonPump Anim] Disarmed – back to idle\n");
	}

	switch (m_eReloadAnimState)
	{
	case VR_RELOAD_ANIM_HOLD:
	{
		if (pPomson->IsVRPumpPullingBack() || pPomson->IsVRPumpPushingFwd())
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_PUMPING;
			if (bDebug)
				DevMsg("[VR PomsonPump Anim] Pumping started\n");
		}
		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = 0.0f;
		break;
	}

	case VR_RELOAD_ANIM_PUMPING:
	{
		float progress = pPomson->GetVRPumpStrokeProgress();
		float cycle = 0.0f;

		if (pPomson->IsVRPumpPullingBack())
		{
			cycle = progress * m_flReloadLoopBottomCycle;
		}
		else if (pPomson->IsVRPumpPushingFwd())
		{
			cycle = m_flReloadLoopBottomCycle + progress * (1.0f - m_flReloadLoopBottomCycle);
		}
		else
		{
			m_eReloadAnimState = VR_RELOAD_ANIM_HOLD;
			m_iLeverReloadSequence = m_iReloadLoopSequence;
			m_flLeverReloadCycle = 0.0f;
			if (bDebug)
				DevMsg("[VR PomsonPump Anim] Pump complete, back to hold\n");
			break;
		}

		m_iLeverReloadSequence = m_iReloadLoopSequence;
		m_flLeverReloadCycle = clamp(cycle, 0.0f, 1.0f);

		if (bDebug)
			DevMsg("[VR PomsonPump Anim] Pump cycle: %.3f (progress %.2f, pullback=%d)\n",
				m_flLeverReloadCycle, progress, pPomson->IsVRPumpPullingBack() ? 1 : 0);
		break;
	}

	default:
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Track minimum clip across all prediction fires this render frame.
//          If a fire's clip is HIGHER than the minimum, a reload must have
//          Trigger fire animation on the hand (animates fingers during firing)
//-----------------------------------------------------------------------------
void C_TFVRHand::PlayWeaponFireAnimation()
{
	extern ConVar tfvr_weapon_fire_anim;
	extern ConVar tfvr_weapon_fire_anim_debug;
	extern ConVar tfvr_shotgun_pump_action;

	if (!tfvr_weapon_fire_anim.GetBool())
		return;

	int sequenceToPlay = m_iFireSequence;
	bool bSuppressRenderWeaponFireAnim = false;

	C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
	C_TFPlayer *pOwner = m_hOwnerPlayer.Get();
	if (pHeldWeapon && pOwner && IsPumpActionShotgunWeaponID(pHeldWeapon->GetWeaponID()))
	{
		if (tfvr_shotgun_pump_action.GetBool())
		{
			int iVRFireSequence = LookupSequence("vr_fire");
			if (iVRFireSequence < 0)
			{
				Warning("VR: Manual shotgun pump is enabled, but 'vr_fire' was not found on '%s'; no shot animation will play\n",
					GetModelName());
				sequenceToPlay = -1;
			}
			else
			{
				sequenceToPlay = iVRFireSequence;
			}

			// The world/render weapon can have its own authored fire sequence,
			// which includes the normal shotgun pump.  The hand-driven vr_fire
			// plus manual pump animation should be the only motion in this mode.
			bSuppressRenderWeaponFireAnim = true;
		}
		else
		{
			const char *pszFireAnim = GetWeaponFireAnimation(
				pOwner->GetPlayerClass()->GetClassIndex(),
				pHeldWeapon->GetClassname(),
				pHeldWeapon );
			if (pszFireAnim && pszFireAnim[0])
			{
				int iNormalFireSequence = LookupSequence(pszFireAnim);
				if (iNormalFireSequence >= 0)
					sequenceToPlay = iNormalFireSequence;
			}
		}
	}

	// Bread Bite: use crit-specific swing when applicable
	if (m_bIsBreadBite)
	{
		bool bIsCrit = pHeldWeapon && pHeldWeapon->IsCurrentAttackACrit();
		if (bIsCrit && m_iBreadBiteCritSeq >= 0)
			sequenceToPlay = m_iBreadBiteCritSeq;
	}

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

	CTFCompoundBow *pBowFire = (pHeldWeapon && pHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		? static_cast<CTFCompoundBow *>(pHeldWeapon) : NULL;
	if (pBowFire && m_iBowIdleSequence >= 0 && sequenceToPlay == m_iBowIdleSequence)
		CaptureBowFireStartPose(pBowFire);
	else
	{
		m_bBowFireStartPoseValid = false;
		m_iBowFireStartPoseBoneCount = 0;
	}

	// Play the fire animation on the HAND model (stops any active charge or draw anim)
	m_bPlayingChargeAnim = false;
	m_bBowShakeOverlayActive = false;
	m_iBowShakeOverlaySequence = -1;
	m_flBowShakeOverlayStartTime = 0.0f;
	m_bPlayingDrawAnim = false;
	SetSequence(sequenceToPlay);
	SetCycle(0.0f);
	SetPlaybackRate(1.0f);
	m_bPlayingFireAnim = true;
	m_flFireAnimStartTime = gpGlobals->curtime;

	// Reset sequence info so DoAnimationEvents dispatches from cycle 0.
	// Also sets up the event index for the new sequence. Normally suppressed
	// on this entity, so the internal event tracking is stale.
	ResetSequenceInfo();
	SetPlaybackRate(1.0f);

	// Force animation frame advance to start immediately
	InvalidateBoneCache();

	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Playing fire animation on hand (sequence %d) at time %.2f\n",
			sequenceToPlay, gpGlobals->curtime);
	}

	// Also trigger animation on the render weapon (if it has one)
	C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
	if (pRenderWeapon && !bSuppressRenderWeaponFireAnim)
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
	m_bPlayingDrawAnim = false;
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

	C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
	CTFCompoundBow *pBow = (pHeldWeapon && pHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		? static_cast<CTFCompoundBow *>(pHeldWeapon) : NULL;
	const float flBowChargeCycle = pBow
		? clamp(pBow->GetCurrentCharge() / MAX(pBow->GetChargeMaxTime(), 0.01f), 0.0f, 1.0f)
		: 0.0f;

	if (m_bPlayingChargeAnim)
	{
		if (pBow && GetSequence() == m_iChargeSequence)
		{
			SetCycle(flBowChargeCycle);
			SetPlaybackRate(0.0f);
			InvalidateBoneCache();
		}
		return;
	}

	SetSequence(m_iChargeSequence);
	SetCycle(pBow ? flBowChargeCycle : 0.0f);
	SetPlaybackRate(pBow ? 0.0f : 1.0f);
	m_bPlayingChargeAnim = true;
	m_bPlayingFireAnim = false;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;
	m_bPlayingDrawAnim = false;

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

	C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
	const bool bBow = pHeldWeapon && pHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW;
	if (bBow)
	{
		if (m_bBowShakeOverlayActive)
			return;

		m_bBowShakeOverlayActive = true;
		m_iBowShakeOverlaySequence = m_iChargeSequence2;
		m_flBowShakeOverlayStartTime = gpGlobals->curtime;
		m_bPlayingChargeAnim = true;
		m_bPlayingFireAnim = false;
		m_bBowFireStartPoseValid = false;
		m_iBowFireStartPoseBoneCount = 0;
		m_bPlayingDrawAnim = false;
		InvalidateBoneCache();
		return;
	}

	SetSequence(m_iChargeSequence2);
	SetCycle(0.0f);
	SetPlaybackRate(1.0f);
	m_bPlayingChargeAnim = true;
	m_bPlayingFireAnim = false;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;
	m_bPlayingDrawAnim = false;

	InvalidateBoneCache();

	if (tfvr_weapon_fire_anim_debug.GetBool())
	{
		DevMsg("VR: Playing charge phase 2 animation on hand (sequence %d)\n", m_iChargeSequence2);
	}
}

void C_TFVRHand::UpdateBowChargeAnimation()
{
	C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
	CTFCompoundBow *pBow = (pHeldWeapon && pHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
		? static_cast<CTFCompoundBow *>(pHeldWeapon) : NULL;
	if (!pBow || !m_bPlayingChargeAnim)
		return;

	if (!pBow->IsVRBowArrowPoseActive() && pBow->GetCurrentCharge() <= 0.001f)
	{
		StopWeaponChargeAnimation();
		return;
	}

	const float flChargeCycle = clamp(pBow->GetCurrentCharge() / MAX(pBow->GetChargeMaxTime(), 0.01f), 0.0f, 1.0f);
	const float flChargeBeginTime = pBow->GetVRBowChargeBeginTime();
	const bool bHeldPastShakeDelay = flChargeBeginTime > 0.0f
		&& (gpGlobals->curtime - flChargeBeginTime) >= pBow->GetChargeForceReleaseTime();
	if (flChargeCycle > 0.001f && bHeldPastShakeDelay && !m_bBowShakeOverlayActive && m_iChargeSequence2 >= 0)
	{
		m_bBowShakeOverlayActive = true;
		m_iBowShakeOverlaySequence = m_iChargeSequence2;
		m_flBowShakeOverlayStartTime = gpGlobals->curtime;
		InvalidateBoneCache();
	}

	const int iSeq = GetSequence();
	if (m_bBowShakeOverlayActive)
	{
		CStudioHdr *pHdr = GetModelPtr();
		if (pHdr && m_iBowShakeOverlaySequence == m_iChargeSequence2 && m_iBowChargeIdleSequence >= 0)
		{
			const float flDuration = SequenceDuration(pHdr, m_iChargeSequence2);
			if (flDuration <= 0.0f || (gpGlobals->curtime - m_flBowShakeOverlayStartTime) >= flDuration)
			{
				m_iBowShakeOverlaySequence = m_iBowChargeIdleSequence;
				m_flBowShakeOverlayStartTime = gpGlobals->curtime;
				InvalidateBoneCache();
			}
		}
	}

	if (iSeq == m_iChargeSequence)
	{
		SetCycle(flChargeCycle);
		SetPlaybackRate(0.0f);
		InvalidateBoneCache();
	}
	else if (iSeq == m_iChargeSequence2 && GetCycle() >= 0.999f && m_iBowChargeIdleSequence >= 0)
	{
		SetSequence(m_iBowChargeIdleSequence);
		SetCycle(0.0f);
		SetPlaybackRate(1.0f);
		InvalidateBoneCache();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Stop charge animation and return to idle
//-----------------------------------------------------------------------------
void C_TFVRHand::StopWeaponChargeAnimation()
{
	if (!m_bPlayingChargeAnim && !m_bBowShakeOverlayActive)
		return;

	m_bPlayingChargeAnim = false;
	m_bBowShakeOverlayActive = false;
	m_iBowShakeOverlaySequence = -1;
	m_flBowShakeOverlayStartTime = 0.0f;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;

	C_TFWeaponBase *pHeldWeapon = m_hHeldWeapon.Get();
	if (pHeldWeapon && pHeldWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW && m_iBowIdleSequence >= 0)
	{
		SetSequence(m_iBowIdleSequence);
		SetCycle(m_flBowIdleCycle);
		SetPlaybackRate(0.0f);
	}
	else if (m_iIdleSequence >= 0)
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
// Purpose: Play the draw/deploy animation on the hand when a weapon is equipped.
//          The draw animation scope (set via m_eDrawAnimScope) determines which
//          bones are driven: FULL_ARM, WRIST, or WEAPON_BONE only.
//-----------------------------------------------------------------------------
void C_TFVRHand::PlayWeaponDrawAnimation()
{
	extern ConVar tfvr_weapon_draw_anim;
	extern ConVar tfvr_weapon_draw_anim_debug;

	if (!tfvr_weapon_draw_anim.GetBool())
		return;

	// Always trigger draw animation on the render weapon, even if the hand
	// doesn't play one (scope NONE). The weapon model still needs its deploy.
	C_VRRenderWeapon *pRenderWeapon = static_cast<C_VRRenderWeapon*>(m_hRenderWeapon.Get());
	if (pRenderWeapon)
	{
		pRenderWeapon->PlayDrawAnimation();
	}

	if (m_iDrawSequence < 0 || m_eDrawAnimScope == VR_DRAW_ANIM_NONE)
	{
		if (tfvr_weapon_draw_anim_debug.GetBool())
		{
			DevMsg("VR: No hand draw animation for this weapon (seq %d, scope %d)\n",
				m_iDrawSequence, (int)m_eDrawAnimScope);
		}
		return;
	}

	// For WEAPON_BONE scope, the main skeleton stays at idle and we only
	// sample the draw animation for weapon_bone in ApplyWeaponPose.
	// For WRIST and FULL_ARM, the draw animation drives the entity sequence.
	// Bread creatures with WEAPON_BONE scope: also play the draw sequence
	// on the entity so vm_weapon bones animate the creature deploy, but
	// bPinToSampled keeps the hand/wrist fixed at the controller.
	if (m_eDrawAnimScope >= VR_DRAW_ANIM_WRIST
		|| (m_bAnimateIdle && m_eDrawAnimScope == VR_DRAW_ANIM_WEAPON_BONE))
	{
		SetSequence(m_iDrawSequence);
		SetCycle(0.0f);
		SetPlaybackRate(1.0f);
	}

	m_bPlayingDrawAnim = true;
	m_bPlayingFireAnim = false;
	m_bBowFireStartPoseValid = false;
	m_iBowFireStartPoseBoneCount = 0;
	m_bPlayingChargeAnim = false;
	m_bBowShakeOverlayActive = false;
	m_iBowShakeOverlaySequence = -1;
	m_flBowShakeOverlayStartTime = 0.0f;
	m_flDrawAnimStartTime = gpGlobals->curtime;

	InvalidateBoneCache();

	if (tfvr_weapon_draw_anim_debug.GetBool())
	{
		const char *scopeNames[] = { "NONE", "WEAPON_BONE", "WRIST", "FULL_ARM" };
		DevMsg("VR: Playing draw animation on hand (sequence %d, scope %s) at time %.2f\n",
			m_iDrawSequence, scopeNames[m_eDrawAnimScope], gpGlobals->curtime);
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
// Purpose: Remove the spy watch from the left hand
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveLeftHandWatch()
{
	if (m_hLeftHandWatch.Get())
	{
		m_hLeftHandWatch->Release();
		m_hLeftHandWatch = NULL;
	}
	m_bWatchOffsetValid = false;
	if (m_pWatchPanel)
	{
		m_pWatchPanel->MarkForDeletion();
		m_pWatchPanel = NULL;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Create a watch model on the left hand
//          Uses EF_NODRAW + explicit DrawModel pattern (same as ball/shield)
//          Position is driven by weapon_bone_L in SetupBones
//-----------------------------------------------------------------------------
void C_TFVRHand::CreateWatchModel(const char *pszWatchModel)
{
	if (!IsLeftHand() || !pszWatchModel || !pszWatchModel[0])
		return;

	RemoveLeftHandWatch();

	C_BaseAnimating *pWatch = new C_BaseAnimating();
	if (!pWatch)
		return;

	if (!pWatch->InitializeAsClientEntity(pszWatchModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		pWatch->Release();
		return;
	}

	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
	{
		pWatch->m_nSkin = (pOwner->GetTeamNumber() == TF_TEAM_RED) ? 0 : 1;
		pWatch->SetOwnerEntity(pOwner);
	}

	pWatch->AddEffects(EF_NODRAW);
	pWatch->SetMoveType(MOVETYPE_NONE);
	pWatch->AddSolidFlags(FSOLID_NOT_SOLID);
	pWatch->SetRenderMode(kRenderTransTexture);

	// Step 1: Get the watch model's weapon_bone_L reference pose inverse.
	// When we SetAbsOrigin(pos), the watch's weapon_bone_L bone renders at
	// (entityTransform * refPose), so we need the inverse to correct the
	// entity position so the bone ends up where we want it.
	m_bWatchOffsetValid = false;
	matrix3x4_t watchRefPoseInverse;
	SetIdentityMatrix(watchRefPoseInverse);
	{
		pWatch->SetAbsOrigin(vec3_origin);
		pWatch->SetAbsAngles(vec3_angle);
		pWatch->InvalidateBoneCache();

		matrix3x4_t watchBones[MAXSTUDIOBONES];
		if (pWatch->SetupBones(watchBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime))
		{
			int iWatchWeaponBone = pWatch->LookupBone("weapon_bone_L");
			if (iWatchWeaponBone >= 0)
			{
				MatrixInvert(watchBones[iWatchWeaponBone], watchRefPoseInverse);
			}
		}
	}

	// Step 2: Sample offhand_idle on the hand to find weapon_bone_L
	// relative to bip_hand_L (same pattern as shield uses c_demo_arms cm_idle).
	CStudioHdr *pHandHdr = GetModelPtr();
	if (pHandHdr)
	{
		int iOffhandIdle = LookupSequence("offhand_idle");
		int iHandBone = LookupBone("bip_hand_L");
		int iWeaponBoneL = LookupBone("weapon_bone_L");

		if (iOffhandIdle >= 0 && iHandBone >= 0 && iWeaponBoneL >= 0)
		{
			float poseParams[MAXSTUDIOPOSEPARAM];
			memset(poseParams, 0, sizeof(poseParams));

			IBoneSetup boneSetup(pHandHdr, BONE_USED_BY_ANYTHING, poseParams);

			Vector pos[MAXSTUDIOBONES];
			Quaternion q[MAXSTUDIOBONES];
			for (int i = 0; i < MAXSTUDIOBONES; i++)
			{
				pos[i].Init();
				q[i].Init(0, 0, 0, 1);
			}
			boneSetup.InitPose(pos, q);
			boneSetup.AccumulatePose(pos, q, iOffhandIdle, 0.0f, 1.0f, gpGlobals->curtime, NULL);

			int numBones = pHandHdr->numbones();
			matrix3x4_t handBones[MAXSTUDIOBONES];
			for (int i = 0; i < numBones; i++)
			{
				matrix3x4_t boneToParent;
				QuaternionMatrix(q[i], pos[i], boneToParent);

				const mstudiobone_t *pBone = pHandHdr->pBone(i);
				if (!pBone)
				{
					SetIdentityMatrix(handBones[i]);
					continue;
				}

				if (pBone->parent == -1)
					MatrixCopy(boneToParent, handBones[i]);
				else if (pBone->parent >= 0 && pBone->parent < numBones)
					ConcatTransforms(handBones[pBone->parent], boneToParent, handBones[i]);
				else
					SetIdentityMatrix(handBones[i]);
			}

			// weapon_bone_L relative to bip_hand_L
			matrix3x4_t invHandBone;
			MatrixInvert(handBones[iHandBone], invHandBone);

			matrix3x4_t weaponBoneToHand;
			ConcatTransforms(invHandBone, handBones[iWeaponBoneL], weaponBoneToHand);

			// Combine: weaponBoneToHand positions the bone relative to
			// the hand, watchRefPoseInverse corrects the entity transform
			// so the mesh lands where the bone should be.
			ConcatTransforms(weaponBoneToHand, watchRefPoseInverse, m_matWatchOffset);
			m_bWatchOffsetValid = true;
		}
	}

	m_hLeftHandWatch = pWatch;

	if (!m_pWatchPanel)
	{
		m_pWatchPanel = new CVRWatchPanel(g_pClientMode->GetViewport());
	}
}

//-----------------------------------------------------------------------------
// Purpose: Update left hand watch visibility based on equipped state
//          Called each frame for spy — scans for TF_WEAPON_INVIS directly
//          so the watch persists across weapon switches
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateLeftHandWatch()
{
	if (!IsLeftHand())
		return;

	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (!pOwner)
	{
		if (m_hLeftHandWatch.Get())
			RemoveLeftHandWatch();
		return;
	}

	if (!pOwner->IsPlayerClass(TF_CLASS_SPY))
	{
		static bool bWarnedClass = false;
		if (!bWarnedClass) { DevMsg("VR Watch: Not spy class (class=%d)\n", pOwner->GetPlayerClass()->GetClassIndex()); bWarnedClass = true; }
		if (m_hLeftHandWatch.Get())
			RemoveLeftHandWatch();
		return;
	}

	DevMsg("VR Watch: Spy detected, checking invis weapon...\n");

	// Find the invis watch weapon directly
	C_BaseCombatWeapon *pInvisWeapon = pOwner->Weapon_OwnsThisID(TF_WEAPON_INVIS);
	if (!pInvisWeapon)
	{
		static bool bWarnedNoInvis = false;
		if (!bWarnedNoInvis) { DevMsg("VR Watch: No TF_WEAPON_INVIS found\n"); bWarnedNoInvis = true; }
		if (m_hLeftHandWatch.Get())
			RemoveLeftHandWatch();
		return;
	}

	C_TFWeaponBase *pTFInvisWeapon = dynamic_cast<C_TFWeaponBase*>(pInvisWeapon);
	if (!pTFInvisWeapon)
	{
		if (m_hLeftHandWatch.Get())
			RemoveLeftHandWatch();
		return;
	}

	// Determine the watch model path
	const char *pszWatchModel = NULL;

	C_TFWearable *pWatchWearable = pTFInvisWeapon->m_hExtraWearableViewModel.Get();
	if (pWatchWearable)
	{
		const model_t *pModel = pWatchWearable->GetModel();
		if (pModel)
			pszWatchModel = modelinfo->GetModelName(pModel);
	}

	if (!pszWatchModel || !pszWatchModel[0])
	{
		CEconItemView *pItem = pTFInvisWeapon->GetAttributeContainer()->GetItem();
		if (pItem && pItem->IsValid())
			pszWatchModel = pItem->GetExtraWearableViewModel();
	}

	if (!pszWatchModel || !pszWatchModel[0])
		pszWatchModel = "models/weapons/c_models/c_spy_watch.mdl";

	// If watch already exists, check whether model changed (loadout update)
	if (m_hLeftHandWatch.Get())
	{
		const model_t *pCurrentModel = m_hLeftHandWatch->GetModel();
		const char *pszCurrentModel = pCurrentModel ? modelinfo->GetModelName(pCurrentModel) : "";
		if (Q_stricmp(pszCurrentModel, pszWatchModel) == 0)
			return;

		RemoveLeftHandWatch();
	}

	DevMsg("VR Watch: Creating watch model '%s'\n", pszWatchModel);
	CreateWatchModel(pszWatchModel);
	DevMsg("VR Watch: Watch created = %s\n", m_hLeftHandWatch.Get() ? "YES" : "NO");
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

	// Position is driven entirely by the hand's SetupBones (weapon_bone_L).
	// Keep EF_NODRAW so the engine never draws the ball during the world
	// pass (where it would use a stale position).  The hand's DrawModel
	// draws the ball explicitly after SetupBones has positioned it.
	pBall->AddEffects(EF_NODRAW);
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
// Purpose: Create the standalone rocket mesh used by manual rocket reload
//-----------------------------------------------------------------------------
bool C_TFVRHand::EnsureManualReloadRocketModel()
{
	if (m_hManualReloadRocket.Get())
		return true;

	static const char *pszRocketModel = "models/weapons/vr_models/vr_rocket.mdl";
	CBaseEntity::PrecacheModel(pszRocketModel);

	C_BaseAnimating *pRocket = new C_BaseAnimating();
	if (!pRocket)
		return false;

	if (!pRocket->InitializeAsClientEntity(pszRocketModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		Warning("VR Rocket Reload: failed to initialize standalone rocket model '%s'\n", pszRocketModel);
		pRocket->Release();
		return false;
	}

	int iModelIndex = modelinfo ? modelinfo->GetModelIndex(pszRocketModel) : -1;
	if (iModelIndex >= 0)
		pRocket->SetModelIndex(iModelIndex);

	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
		pRocket->SetOwnerEntity(pOwner);

	pRocket->AddEffects(EF_NODRAW);
	pRocket->AddEffects(EF_NOINTERP);
	pRocket->SetMoveType(MOVETYPE_NONE);
	pRocket->AddSolidFlags(FSOLID_NOT_SOLID);
	pRocket->SetRenderMode(kRenderNormal);

	int iIdleSeq = pRocket->LookupSequence("vr_rocket_idle");
	if (iIdleSeq < 0)
		iIdleSeq = pRocket->LookupSequence("idle");
	if (iIdleSeq >= 0)
	{
		pRocket->SetSequence(iIdleSeq);
		pRocket->SetCycle(0.0f);
		pRocket->SetPlaybackRate(0.0f);
	}

	m_bManualReloadRocketBoneInverseValid = false;
	SetIdentityMatrix(m_matManualReloadRocketBoneInverse);
	m_bManualReloadRocketBoneInverseValid =
		TFVR_CalculateModelRocketBoneInverse(pRocket, m_matManualReloadRocketBoneInverse);

	m_hManualReloadRocket = pRocket;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Remove the standalone rocket mesh used by manual rocket reload
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveManualReloadRocketModel()
{
	if (m_hManualReloadRocket.Get())
	{
		m_hManualReloadRocket->Release();
		m_hManualReloadRocket = NULL;
	}
	m_bManualReloadRocketBoneInverseValid = false;
	SetIdentityMatrix(m_matManualReloadRocketBoneInverse);
}

//-----------------------------------------------------------------------------
// Purpose: Position the standalone rocket from this hand's sampled rocket bone
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateManualReloadRocketFromBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones, bool bVisible)
{
	if (!bVisible)
	{
		RemoveManualReloadRocketModel();
		return;
	}

	if (!pBoneToWorldOut || !EnsureManualReloadRocketModel())
		return;

	C_BaseAnimating *pRocket = m_hManualReloadRocket.Get();
	if (!pRocket)
		return;

	int iRocketBone = LookupBone("rocket");
	int iAttachBone = iRocketBone;
	if (iAttachBone < 0 || iAttachBone >= nMaxBones)
	{
		pRocket->AddEffects(EF_NODRAW);
		return;
	}

	if (!m_bManualReloadRocketBoneInverseValid)
	{
		m_bManualReloadRocketBoneInverseValid =
			TFVR_CalculateModelRocketBoneInverse(pRocket, m_matManualReloadRocketBoneInverse);
	}

	matrix3x4_t rocketWorld;
	if (m_bManualReloadRocketBoneInverseValid)
		ConcatTransforms(pBoneToWorldOut[iAttachBone], m_matManualReloadRocketBoneInverse, rocketWorld);
	else
		MatrixCopy(pBoneToWorldOut[iAttachBone], rocketWorld);

	Vector rocketPos;
	QAngle rocketAng;
	MatrixAngles(rocketWorld, rocketAng, rocketPos);

	pRocket->SetAbsOrigin(rocketPos);
	pRocket->SetAbsAngles(rocketAng);
	pRocket->SetNetworkOrigin(rocketPos);
	pRocket->ResetLatched();
	pRocket->InvalidateBoneCache();

	extern ConVar tfvr_shotgun_pump_debug;
	if (tfvr_shotgun_pump_debug.GetBool() && debugoverlay)
	{
		Vector handRocketPos;
		MatrixGetColumn(pBoneToWorldOut[iAttachBone], 3, handRocketPos);

		// Cyan = animated rocket bone on the hand model.
		debugoverlay->AddBoxOverlay(handRocketPos,
			Vector(-2.0f, -2.0f, -2.0f), Vector(2.0f, 2.0f, 2.0f),
			vec3_angle, 0, 255, 255, 220, 0.05f);
		debugoverlay->AddTextOverlay(handRocketPos, 0.05f, "hand rocket");

		// Yellow = standalone rocket entity/root origin. If the visible mesh
		// follows this instead of the red/cyan bone markers, the mesh is root-bound.
		debugoverlay->AddBoxOverlay(rocketPos,
			Vector(-1.5f, -1.5f, -1.5f), Vector(1.5f, 1.5f, 1.5f),
			vec3_angle, 255, 255, 0, 220, 0.05f);
		debugoverlay->AddTextOverlay(rocketPos, 0.05f, "rocket root");

		Vector handModelRootPos = vec3_origin;
		if (nMaxBones > 0)
		{
			MatrixGetColumn(pBoneToWorldOut[0], 3, handModelRootPos);
			// Blue = solved root of the offhand model skeleton.
			debugoverlay->AddBoxOverlay(handModelRootPos,
				Vector(-2.5f, -2.5f, -2.5f), Vector(2.5f, 2.5f, 2.5f),
				vec3_angle, 0, 64, 255, 220, 0.05f);
			debugoverlay->AddTextOverlay(handModelRootPos, 0.05f, "hand root");
			debugoverlay->AddLineOverlay(handModelRootPos, handRocketPos, 0, 128, 255, false, 0.05f);
		}

		int iModelRocketBone = pRocket->LookupBone("rocket");
		if (iModelRocketBone >= 0)
		{
			matrix3x4_t rocketBones[MAXSTUDIOBONES];
			if (pRocket->SetupBones(rocketBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime))
			{
				Vector modelRocketPos;
				MatrixGetColumn(rocketBones[iModelRocketBone], 3, modelRocketPos);

				// Red = rocket bone inside the standalone rocket model.
				debugoverlay->AddBoxOverlay(modelRocketPos,
					Vector(-1.0f, -1.0f, -1.0f), Vector(1.0f, 1.0f, 1.0f),
					vec3_angle, 255, 0, 0, 255, 0.05f);
				debugoverlay->AddTextOverlay(modelRocketPos + Vector(0.0f, 0.0f, 2.0f), 0.05f, "model rocket");
				debugoverlay->AddLineOverlay(handRocketPos, modelRocketPos, 255, 255, 255, false, 0.05f);
				debugoverlay->AddLineOverlay(rocketPos, modelRocketPos, 255, 255, 0, false, 0.05f);

				static float s_flLastRocketAlignDebugTime = 0.0f;
				if (gpGlobals->curtime - s_flLastRocketAlignDebugTime > 0.25f)
				{
					Vector delta = modelRocketPos - handRocketPos;
					Vector handBonePos = handRocketPos;
					float flHandBoneDistance = 0.0f;
					if (m_iHandBone >= 0 && m_iHandBone < nMaxBones)
					{
						MatrixGetColumn(pBoneToWorldOut[m_iHandBone], 3, handBonePos);
						flHandBoneDistance = (handRocketPos - handBonePos).Length();
					}

					const char *pszClosestBone = "<none>";
					float flClosestBoneDistance = FLT_MAX;
					const char *pszSuffix = IsLeftHand() ? "_L" : "_R";
					const char *pszBoneBases[] = {
						"bip_hand",
						"bip_thumb_0", "bip_thumb_1", "bip_thumb_2",
						"bip_index_0", "bip_index_1", "bip_index_2",
						"bip_middle_0", "bip_middle_1", "bip_middle_2",
						"bip_ring_0", "bip_ring_1", "bip_ring_2",
						"bip_pinky_0", "bip_pinky_1", "bip_pinky_2",
					};
					char szClosestBone[64];
					szClosestBone[0] = '\0';
					for (int i = 0; i < ARRAYSIZE(pszBoneBases); i++)
					{
						char szBoneName[64];
						Q_snprintf(szBoneName, sizeof(szBoneName), "%s%s", pszBoneBases[i], pszSuffix);
						int iBone = LookupBone(szBoneName);
						if (iBone < 0 || iBone >= nMaxBones)
							continue;

						Vector bonePos;
						MatrixGetColumn(pBoneToWorldOut[iBone], 3, bonePos);
						float flDistance = (handRocketPos - bonePos).Length();
						if (flDistance < flClosestBoneDistance)
						{
							flClosestBoneDistance = flDistance;
							Q_strncpy(szClosestBone, szBoneName, sizeof(szClosestBone));
							pszClosestBone = szClosestBone;
						}
					}

					float flRootToHand = (handBonePos - handModelRootPos).Length();
					float flRootToRocket = (handRocketPos - handModelRootPos).Length();
					DevMsg("VR Rocket Reload: model align inverse=%d handRoot=(%.1f %.1f %.1f) rootToHand=%.2f rootToRocket=%.2f rocketBone=(%.1f %.1f %.1f) model=(%.1f %.1f %.1f) delta=%.2f handBone=(%.1f %.1f %.1f) rocketToHandBone=%.2f closest='%s' closestDist=%.2f entity=(%.1f %.1f %.1f)\n",
						m_bManualReloadRocketBoneInverseValid ? 1 : 0,
						handModelRootPos.x, handModelRootPos.y, handModelRootPos.z,
						flRootToHand, flRootToRocket,
						handRocketPos.x, handRocketPos.y, handRocketPos.z,
						modelRocketPos.x, modelRocketPos.y, modelRocketPos.z,
						delta.Length(),
						handBonePos.x, handBonePos.y, handBonePos.z, flHandBoneDistance,
						pszClosestBone, flClosestBoneDistance,
						rocketPos.x, rocketPos.y, rocketPos.z);
					s_flLastRocketAlignDebugTime = gpGlobals->curtime;
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Create the standalone magazine mesh used by pistol manual reload
//-----------------------------------------------------------------------------
bool C_TFVRHand::EnsurePistolMagazineModel()
{
	const char *pszMagModel = "models/weapons/vr_models/vr_pistol/vr_pistol_ammo.mdl";
	C_TFWeaponBase *pMagWeapon = m_hHeldWeapon.Get();
	if (!TFVR_GetManualReloadPistol(pMagWeapon))
	{
		C_TFVRHand *pOtherHand = GetOppositeVRHand(this);
		pMagWeapon = pOtherHand ? pOtherHand->GetHeldWeapon() : NULL;
	}
	if (TFVR_GetManualReloadPistol(pMagWeapon))
		pszMagModel = VRPistol_AmmoModelForWorldModel(pMagWeapon->GetWorldModel());

	if (m_hPistolMagazine.Get())
	{
		const char *pszCurrentModel = modelinfo && m_hPistolMagazine->GetModel()
			? modelinfo->GetModelName(m_hPistolMagazine->GetModel()) : NULL;
		if (pszCurrentModel && !Q_stricmp(pszCurrentModel, pszMagModel))
			return true;

		RemovePistolMagazineModel();
	}

	CBaseEntity::PrecacheModel(pszMagModel);

	C_BaseAnimating *pMag = new C_BaseAnimating();
	if (!pMag)
		return false;

	if (!pMag->InitializeAsClientEntity(pszMagModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		Warning("VR Pistol Reload: failed to initialize magazine model '%s'\n", pszMagModel);
		pMag->Release();
		return false;
	}

	int iModelIndex = modelinfo ? modelinfo->GetModelIndex(pszMagModel) : -1;
	if (iModelIndex >= 0)
		pMag->SetModelIndex(iModelIndex);

	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
		pMag->SetOwnerEntity(pOwner);

	pMag->AddEffects(EF_NODRAW);
	pMag->AddEffects(EF_NOINTERP);
	pMag->SetMoveType(MOVETYPE_NONE);
	pMag->AddSolidFlags(FSOLID_NOT_SOLID);
	pMag->SetRenderMode(kRenderNormal);

	int iIdleSeq = pMag->LookupSequence("idle");
	if (iIdleSeq >= 0)
	{
		pMag->SetSequence(iIdleSeq);
		pMag->SetCycle(0.0f);
		pMag->SetPlaybackRate(0.0f);
	}

	m_bPistolMagBoneInverseValid = false;
	SetIdentityMatrix(m_matPistolMagBoneInverse);
	m_bPistolMagBoneInverseValid =
		TFVR_CalculateModelBoneInverse(pMag, "vm_weapon_bone", m_matPistolMagBoneInverse);

	m_hPistolMagazine = pMag;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Remove the standalone pistol magazine mesh
//-----------------------------------------------------------------------------
void C_TFVRHand::RemovePistolMagazineModel()
{
	if (m_hPistolMagazine.Get())
	{
		m_hPistolMagazine->Release();
		m_hPistolMagazine = NULL;
	}
	m_bPistolMagBoneInverseValid = false;
	SetIdentityMatrix(m_matPistolMagBoneInverse);
	m_bPistolMagLastWorldValid = false;
	m_bPistolMagFalling = false;
}

//-----------------------------------------------------------------------------
// Purpose: Begin the client-side ballistic bridge: the cosmetic mag detaches
//          from the gun with the launch state the server prop will use.
//-----------------------------------------------------------------------------
void C_TFVRHand::StartPistolMagFall()
{
	m_bPistolMagFalling = true;
	m_flPistolMagFallStartTime = gpGlobals->curtime;
	m_vecPistolMagFallStartPos = m_vecPistolMagLastWorldPos;

	Vector vecPlayerVel = vec3_origin;
	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
		vecPlayerVel = pOwner->GetAbsVelocity();

	// Launch with the animation's own velocity (one-frame delta at the
	// authored 30fps), matching what the server prop will receive.
	m_vecPistolMagFallVel = m_vecPistolMagEjectVel + vecPlayerVel;
}

//-----------------------------------------------------------------------------
// Purpose: Advance the local falling mag and hand off to the server's
//          physics prop once it shows up (or on timeout).
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdatePistolMagFall()
{
	C_BaseAnimating *pMag = m_hPistolMagazine.Get();
	if (!pMag)
	{
		m_bPistolMagFalling = false;
		return;
	}

	const float flAge = gpGlobals->curtime - m_flPistolMagFallStartTime;
	const float flMaxAge = 0.6f;

	// Hand off once the networked prop exists near our simulated mag, or
	// give up after the timeout (dropped packets etc).
	bool bHandOff = flAge > flMaxAge;
	if (!bHandOff)
	{
		Vector vecMagPos = pMag->GetAbsOrigin();
		for (C_BaseEntity *pEnt = ClientEntityList().FirstBaseEntity(); pEnt; pEnt = ClientEntityList().NextBaseEntity(pEnt))
		{
			C_TFVRWeaponMagazine *pProp = dynamic_cast<C_TFVRWeaponMagazine *>(pEnt);
			if (!pProp)
				continue;

			// Only a freshly arrived prop counts - ignore older dropped mags
			// that may be lying nearby from a previous reload.
			if (pProp->GetClientSpawnTime() < m_flPistolMagFallStartTime - 0.25f)
				continue;

			if ((pProp->GetAbsOrigin() - vecMagPos).LengthSqr() <= Square(50.0f))
			{
				bHandOff = true;
				break;
			}
		}
	}

	if (bHandOff)
	{
		m_bPistolMagFalling = false;
		RemovePistolMagazineModel();
		return;
	}

	// Deterministic ballistic position (safe under multiple SetupBones
	// calls per frame - no per-call integration).
	float flGravity = 800.0f;
	static ConVarRef sv_gravity("sv_gravity");
	if (sv_gravity.IsValid())
		flGravity = sv_gravity.GetFloat();

	Vector vecPos = m_vecPistolMagFallStartPos
		+ m_vecPistolMagFallVel * flAge
		- Vector(0.0f, 0.0f, 0.5f * flGravity * flAge * flAge);

	pMag->SetAbsOrigin(vecPos);
	pMag->SetAbsAngles(m_angPistolMagLastWorldAng);
	pMag->SetNetworkOrigin(vecPos);
	pMag->ResetLatched();
	pMag->InvalidateBoneCache();
}

//-----------------------------------------------------------------------------
// Purpose: Last world transform of the gun's magazine mesh (weapon hand),
//          plus the animation-derived eject direction (zero when unknown).
//-----------------------------------------------------------------------------
bool C_TFVRHand::GetPistolGunMagazineWorld( Vector &outPos, QAngle &outAngles, Vector &outEjectVel ) const
{
	if (!m_bPistolMagLastWorldValid)
		return false;

	outPos = m_vecPistolMagLastWorldPos;
	outAngles = m_angPistolMagLastWorldAng;
	outEjectVel = m_vecPistolMagEjectVel;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Position the pistol magazine mesh for this hand:
//          - weapon hand: mag seated in / sliding out of the gun, riding the
//            p_reload vm_weapon_bone mapped through the live weapon bone
//          - off hand: spare mag riding this hand's posed vm_weapon_bone
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdatePistolMagazineFromBones(matrix3x4_t *pBoneToWorldOut, int nMaxBones, bool bManualReloadPoseApplied)
{
	// Client-side ballistic bridge active: the mag is mid-air between the
	// authored eject motion and the server's physics prop.
	if (m_bPistolMagFalling)
	{
		UpdatePistolMagFall();
		return;
	}

	matrix3x4_t magBoneWorld;
	matrix3x4_t magBoneWorldAhead;
	bool bVisible = false;
	bool bIsGunMag = false;
	bool bHaveAhead = false;

	if (pBoneToWorldOut)
	{
		C_TFWeaponBase *pWeapon = m_hHeldWeapon.Get();
		CTFPistol *pOwnPistol = TFVR_GetManualReloadPistol(pWeapon);
		if (pOwnPistol)
		{
			bIsGunMag = true;
			// Weapon hand: the gun's own magazine.
			float flCycle = 0.0f;
			bool bWantMag = false;
			const int iPhase = pOwnPistol->GetVRMagPhase();
			if (iPhase == VR_PISTOL_MAG_PHASE_EJECTING)
			{
				// Mag rides the eject animation until it clears the gun (frame 6),
				// then falls ballistically until the server's physics prop arrives.
				flCycle = pOwnPistol->GetVRMagPhaseProgress() * m_flPistolPauseCycle;
				bWantMag = !pOwnPistol->IsVRMagOut();

				if (!bWantMag && m_hPistolMagazine.Get() && m_bPistolMagLastWorldValid)
				{
					StartPistolMagFall();
					UpdatePistolMagFall();
					return;
				}
			}
			else if (iPhase == VR_PISTOL_MAG_PHASE_FINISHING)
			{
				flCycle = Lerp(pOwnPistol->GetVRMagPhaseProgress(), m_flShotgunManualReloadCommitCycle, m_flPistolFinishEndCycle);
				bWantMag = true;
			}
			else if (iPhase == VR_PISTOL_MAG_PHASE_IDLE)
			{
				flCycle = 0.0f;
				bWantMag = !pOwnPistol->IsVRMagOut();
			}
			// INSERTING: the off hand's held mag is the visible one.

			if (bWantMag && m_iShotgunManualReloadSequence >= 0)
			{
				int iWeaponBone = LookupBone("weapon_bone");
				matrix3x4_t magRelWeapon;
				if (iWeaponBone >= 0 && iWeaponBone < nMaxBones
					&& GetPistolReloadMagRelativeToWeapon(flCycle, magRelWeapon))
				{
					ConcatTransforms(pBoneToWorldOut[iWeaponBone], magRelWeapon, magBoneWorld);
					bVisible = true;

					// While ejecting, also sample one animation frame ahead:
					// the physics prop spawns there (clear of the gun) moving
					// in the direction the animation carries the mag.
					if (iPhase == VR_PISTOL_MAG_PHASE_EJECTING && m_flPistolOneFrameCycle > 0.0f)
					{
						const float flOneFrame = m_flPistolOneFrameCycle;
						matrix3x4_t magRelAhead;
						if (GetPistolReloadMagRelativeToWeapon(flCycle + flOneFrame, magRelAhead))
						{
							ConcatTransforms(pBoneToWorldOut[iWeaponBone], magRelAhead, magBoneWorldAhead);
							bHaveAhead = true;
						}
					}
				}
			}
		}
		else
		{
			// Off hand: spare mag pulled from the backpack.
			C_TFVRHand *pOtherHand = GetOppositeVRHand(this);
			C_TFWeaponBase *pOtherWeapon = pOtherHand ? pOtherHand->GetHeldWeapon() : NULL;
			CTFPistol *pOtherPistol = TFVR_GetManualReloadPistol(pOtherWeapon);
			if (pOtherPistol && bManualReloadPoseApplied && pOtherPistol->IsVRPistolMagPoseActive())
			{
				int iMagBone = LookupBone("vm_weapon_bone");
				if (iMagBone >= 0 && iMagBone < nMaxBones)
				{
					MatrixCopy(pBoneToWorldOut[iMagBone], magBoneWorld);
					bVisible = true;
				}
			}
		}
	}

	if (!bVisible)
	{
		RemovePistolMagazineModel();
		return;
	}

	if (!EnsurePistolMagazineModel())
		return;

	C_BaseAnimating *pMag = m_hPistolMagazine.Get();
	if (!pMag)
		return;

	matrix3x4_t magWorld;
	if (m_bPistolMagBoneInverseValid)
		ConcatTransforms(magBoneWorld, m_matPistolMagBoneInverse, magWorld);
	else
		MatrixCopy(magBoneWorld, magWorld);

	Vector magPos;
	QAngle magAng;
	MatrixAngles(magWorld, magAng, magPos);

	// Remember the gun mag's exact transform so the input layer can report
	// it to the server for the dropped-physics-mag spawn. When an ahead
	// sample exists (ejecting), report THAT transform plus the animation's
	// motion direction so the prop spawns clear of the gun with momentum.
	if (bIsGunMag)
	{
		if (bHaveAhead)
		{
			matrix3x4_t magWorldAhead;
			if (m_bPistolMagBoneInverseValid)
				ConcatTransforms(magBoneWorldAhead, m_matPistolMagBoneInverse, magWorldAhead);
			else
				MatrixCopy(magBoneWorldAhead, magWorldAhead);

			Vector magPosAhead;
			QAngle magAngAhead;
			MatrixAngles(magWorldAhead, magAngAhead, magPosAhead);

			m_vecPistolMagLastWorldPos = magPosAhead;
			m_angPistolMagLastWorldAng = magAngAhead;

			// One frame of displacement at the authored framerate gives the
			// animation's instantaneous mag velocity.
			Vector vecDelta = magPosAhead - magPos;
			if (vecDelta.LengthSqr() > 1e-8f)
				m_vecPistolMagEjectVel = vecDelta * VR_PISTOL_RELOAD_ANIM_FPS;
			else
				m_vecPistolMagEjectVel = vec3_origin;
		}
		else
		{
			m_vecPistolMagLastWorldPos = magPos;
			m_angPistolMagLastWorldAng = magAng;
			m_vecPistolMagEjectVel = vec3_origin;
		}
		m_bPistolMagLastWorldValid = true;
	}

	pMag->SetAbsOrigin(magPos);
	pMag->SetAbsAngles(magAng);
	pMag->SetNetworkOrigin(magPos);
	pMag->ResetLatched();
	pMag->InvalidateBoneCache();
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
	m_iLastBallAmmo = iBallAmmo;

	if (iBallAmmo > 0)
	{
		if (!m_hLeftHandBall.Get())
		{
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
	}
}

//-----------------------------------------------------------------------------
// Purpose: Create and attach a shield model to the left hand
//-----------------------------------------------------------------------------
void C_TFVRHand::AttachShieldToLeftHand(const char *pszShieldModel, int nSkin)
{
	if (!IsLeftHand() || !pszShieldModel || !pszShieldModel[0])
		return;

	RemoveLeftHandShield();

	C_BaseAnimating *pShield = new C_BaseAnimating();
	if (!pShield)
		return;

	if (!pShield->InitializeAsClientEntity(pszShieldModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		pShield->Release();
		return;
	}

	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (pOwner)
	{
		pShield->m_nSkin = nSkin;
		pShield->SetOwnerEntity(pOwner);
	}

	pShield->AddEffects(EF_NODRAW);
	pShield->SetMoveType(MOVETYPE_NONE);
	pShield->AddSolidFlags(FSOLID_NOT_SOLID);

	m_hLeftHandShield = pShield;

	// Step 1: Get the shield model's weapon_targe reference pose.
	// When we SetAbsOrigin(pos), the shield's weapon_targe bone actually
	// renders at (entityTransform * refPose), so we need the inverse to
	// correct the entity position so the bone ends up where we want it.
	matrix3x4_t shieldRefPoseInverse;
	SetIdentityMatrix(shieldRefPoseInverse);
	{
		pShield->SetAbsOrigin(vec3_origin);
		pShield->SetAbsAngles(vec3_angle);
		matrix3x4_t shieldBones[MAXSTUDIOBONES];
		if (pShield->SetupBones(shieldBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime))
		{
			int iShieldTarge = pShield->LookupBone("weapon_targe");
			if (iShieldTarge >= 0)
			{
				MatrixInvert(shieldBones[iShieldTarge], shieldRefPoseInverse);

				Vector refPos;
				QAngle refAng;
				MatrixAngles(shieldBones[iShieldTarge], refAng, refPos);
				DevMsg("VR Shield: shieldRefPose pos=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f)\n",
					refPos.x, refPos.y, refPos.z, refAng.x, refAng.y, refAng.z);
			}
		}
	}

	// Step 2: Create a temporary c_demo_arms entity to read
	// weapon_targe relative to bip_hand_L from its own SetupBones pipeline.
	const char *pszArmsModel = GetFallbackModelForClass(TF_CLASS_DEMOMAN);
	C_BaseAnimating *pArms = new C_BaseAnimating();
	if (pArms && pArms->InitializeAsClientEntity(pszArmsModel, RENDER_GROUP_OPAQUE_ENTITY))
	{
		int iCmIdle = pArms->LookupSequence("cm_idle");
		if (iCmIdle >= 0)
			pArms->ResetSequence(iCmIdle);

		if (pOwner)
			pArms->SetOwnerEntity(pOwner);

		pArms->AddEffects(EF_NODRAW);
		pArms->SetMoveType(MOVETYPE_NONE);
		pArms->AddSolidFlags(FSOLID_NOT_SOLID);
		pArms->SetAbsOrigin(vec3_origin);
		pArms->SetAbsAngles(vec3_angle);

		matrix3x4_t armsBones[MAXSTUDIOBONES];
		if (pArms->SetupBones(armsBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, gpGlobals->curtime))
		{
			int iArmsHand = pArms->LookupBone("bip_hand_L");
			int iArmsTarge = pArms->LookupBone("weapon_targe");
			if (iArmsHand >= 0 && iArmsTarge >= 0)
			{
				matrix3x4_t invArmsHand;
				MatrixInvert(armsBones[iArmsHand], invArmsHand);

				matrix3x4_t targeToHand;
				ConcatTransforms(invArmsHand, armsBones[iArmsTarge], targeToHand);

				// Combine: targeToHand gives us where weapon_targe bone should be
				// relative to bip_hand_L, then shieldRefPoseInverse corrects so the
				// shield ENTITY position makes the bone land in the right spot.
				// Since ref pose rotation is ~identity, this fixes position only.
				ConcatTransforms(targeToHand, shieldRefPoseInverse, m_matShieldOffset);
				m_bShieldOffsetValid = true;

				Vector targeToHandPos, combinedPos;
				MatrixGetColumn(targeToHand, 3, targeToHandPos);
				MatrixGetColumn(m_matShieldOffset, 3, combinedPos);
				DevMsg("VR Shield: targeToHand=(%.1f,%.1f,%.1f) combined=(%.1f,%.1f,%.1f) handIdx=%d targeIdx=%d\n",
					targeToHandPos.x, targeToHandPos.y, targeToHandPos.z,
					combinedPos.x, combinedPos.y, combinedPos.z,
					iArmsHand, iArmsTarge);
			}
		}

		pArms->Release();
	}
	else if (pArms)
	{
		pArms->Release();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Remove the shield model from the left hand
//-----------------------------------------------------------------------------
void C_TFVRHand::RemoveLeftHandShield()
{
	if (m_hLeftHandShield.Get())
	{
		m_hLeftHandShield->Release();
		m_hLeftHandShield = NULL;
	}
	m_bShieldOffsetValid = false;
}

//-----------------------------------------------------------------------------
// Purpose: Update left hand shield visibility based on equipped state
//          Called each frame for demoman
//-----------------------------------------------------------------------------
void C_TFVRHand::UpdateLeftHandShield()
{
	if (!IsLeftHand())
		return;

	C_TFPlayer *pOwner = GetOwnerPlayer();
	if (!pOwner || !pOwner->IsPlayerClass(TF_CLASS_DEMOMAN))
	{
		if (m_hLeftHandShield.Get())
			RemoveLeftHandShield();
		return;
	}

	// Scan the wearable list directly for a shield entity instead of
	// relying on IsShieldEquipped(), which is a networked bool that can
	// briefly toggle false during weapon switches and cause flicker.
	C_TFWearableDemoShield *pEquippedShield = NULL;
	for (int i = 0; i < pOwner->GetNumWearables(); ++i)
	{
		C_TFWearableDemoShield *pShield = dynamic_cast<C_TFWearableDemoShield*>(pOwner->GetWearable(i));
		if (pShield)
		{
			pEquippedShield = pShield;
			break;
		}
	}

	if (!pEquippedShield)
	{
		if (m_hLeftHandShield.Get())
			RemoveLeftHandShield();
		return;
	}

	const model_t *pModel = pEquippedShield->GetModel();
	if (!pModel)
	{
		if (m_hLeftHandShield.Get())
			RemoveLeftHandShield();
		return;
	}

	const char *pszModel = modelinfo->GetModelName(pModel);
	if (!pszModel || !pszModel[0])
	{
		if (m_hLeftHandShield.Get())
			RemoveLeftHandShield();
		return;
	}

	int nSkin = pEquippedShield->m_nSkin;

	// If shield already exists, check whether model or skin changed (loadout update)
	if (m_hLeftHandShield.Get())
	{
		const model_t *pCurrentModel = m_hLeftHandShield->GetModel();
		if (pCurrentModel == pModel && m_hLeftHandShield->m_nSkin == nSkin)
			return;

		RemoveLeftHandShield();
	}

	AttachShieldToLeftHand(pszModel, nSkin);
}
