//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_smg.h"
#include "usercmd.h"

static const float DAMAGE_TO_FILL_MINICRIT_METER = 100.0f;

ConVar tfvr_smg_manual_reload( "tfvr_smg_manual_reload", "1", FCVAR_ARCHIVE, "VR SMG: 1 = manually eject and insert clip; 0 = standard auto reload. Toggled by the TF2VR auto-reload option." );
ConVar tfvr_smg_ammo_eject_duration( "tfvr_smg_ammo_eject_duration", "0.18", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR SMG: seconds for smg_reload frames 8-10." );
ConVar tfvr_smg_ammo_insert_duration( "tfvr_smg_ammo_insert_duration", "0.20", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR SMG: seconds for smg_reload frames 24-26." );
ConVar tfvr_smg_ammo_finish_duration( "tfvr_smg_ammo_finish_duration", "0.25", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR SMG: seconds for the post-insert finish motion." );
ConVar tfvr_smg_ammo_eject_speed( "tfvr_smg_ammo_eject_speed", "30", FCVAR_REPLICATED, "VR SMG: fallback initial speed (u/s) of the dropped clip along its eject direction." );

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include "prediction.h"
#include "tfvr/c_tfvr_hand.h"
// Server specific.
#else
#include "tf_player.h"
#include "tfvr/tfvr_weapon_magazine.h"
#endif
//=============================================================================
//
// Weapon tables.
//

// ---------- Regular SMG -------------

IMPLEMENT_NETWORKCLASS_ALIASED( TFSMG, DT_TFSMG )

BEGIN_NETWORK_TABLE( CTFSMG, DT_TFSMG )
#ifdef CLIENT_DLL
	RecvPropInt( RECVINFO( m_iVRAmmoPhase ) ),
	RecvPropFloat( RECVINFO( m_flVRAmmoPhaseStartTime ) ),
	RecvPropBool( RECVINFO( m_bVRAmmoOut ) ),
	RecvPropBool( RECVINFO( m_bVRAmmoHeld ) ),
#else
	SendPropInt( SENDINFO( m_iVRAmmoPhase ), 3, SPROP_UNSIGNED ),
	SendPropFloat( SENDINFO( m_flVRAmmoPhaseStartTime ), 0, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRAmmoOut ) ),
	SendPropBool( SENDINFO( m_bVRAmmoHeld ) ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFSMG )
#ifdef CLIENT_DLL
	DEFINE_PRED_FIELD( m_iVRAmmoPhase, FIELD_INTEGER, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRAmmoPhaseStartTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRAmmoOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRAmmoHeld, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
#endif
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_smg, CTFSMG );
PRECACHE_WEAPON_REGISTER( tf_weapon_smg );

// Server specific.
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFSMG )
END_DATADESC()
#endif

// ---------- Charged SMG -------------

IMPLEMENT_NETWORKCLASS_ALIASED( TFChargedSMG, DT_WeaponChargedSMG )

BEGIN_NETWORK_TABLE( CTFChargedSMG, DT_WeaponChargedSMG )
// Client specific.
#ifdef CLIENT_DLL
RecvPropFloat( RECVINFO( m_flMinicritCharge ) ),
// Server specific.
#else
SendPropFloat( SENDINFO( m_flMinicritCharge ), 4, SPROP_NOSCALE, 0.0f, DAMAGE_TO_FILL_MINICRIT_METER ),
#endif
END_NETWORK_TABLE()

// Server specific
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFChargedSMG )
END_DATADESC()
#endif

// Client specific
#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA( CTFChargedSMG )
DEFINE_FIELD(  m_flMinicritCharge, FIELD_FLOAT )
END_PREDICTION_DATA()
#endif

LINK_ENTITY_TO_CLASS( tf_weapon_charged_smg, CTFChargedSMG );
PRECACHE_WEAPON_REGISTER( tf_weapon_charged_smg );

//=============================================================================
//
// Weapon SMG functions.

CTFSMG::CTFSMG()
{
	m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_IDLE;
	m_flVRAmmoPhaseStartTime = 0.0f;
	m_bVRAmmoOut = false;
	m_bVRAmmoHeld = false;
#ifdef GAME_DLL
	m_bVRAmmoPhysSpawned = false;
#endif
}

void CTFSMG::Precache()
{
	BaseClass::Precache();

	PrecacheModel( VRSMG_GunModelName() );
	PrecacheModel( VRSMG_AmmoModelName() );
	PrecacheModel( "models/weapons/vr_models/vr_pro_smg/vr_pro_smg.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_pro_smg/vr_pro_smg_ammo.mdl" );
	PrecacheScriptSound( "VR.SMGClipOut" );
	PrecacheScriptSound( "VR.SMGClipIn" );
	PrecacheScriptSound( "VR.ManualReloadAmmoGrab" );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
int	CTFSMG::GetDamageType( void ) const
{
	if ( CanHeadshot() )
	{
		int iDamageType = BaseClass::GetDamageType() | DMG_USE_HITLOCATIONS;
		return iDamageType;
	}

	return BaseClass::GetDamageType();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFSMG::CanFireCriticalShot( bool bIsHeadshot, CBaseEntity *pTarget /*= NULL*/ )
{
	if ( !BaseClass::CanFireCriticalShot( bIsHeadshot, pTarget ) )
		return false;

	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( pPlayer && pPlayer->m_Shared.IsCritBoosted() )
		return true;

	if ( !bIsHeadshot )
		return !CanHeadshot();

	return true;
}

bool CTFSMG::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	ResetVRSMGAmmoState();
	return BaseClass::Holster( pSwitchingTo );
}

bool CTFSMG::ShouldUseVRSMGManualReload() const
{
	if ( !tfvr_smg_manual_reload.GetBool() )
		return false;

	const int iWeaponID = GetWeaponID();
	if ( iWeaponID != TF_WEAPON_SMG && iWeaponID != TF_WEAPON_CHARGED_SMG )
		return false;

	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !pOwner->IsInVRMode() )
		return false;

#ifdef CLIENT_DLL
	if ( IsHeldByVRHand() )
		return true;

	if ( pOwner->IsLocalPlayer() )
	{
		C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
		if ( pRightHand && pRightHand->GetHeldWeapon() == this )
			return true;

		C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
		if ( pLeftHand && pLeftHand->GetHeldWeapon() == this )
			return true;
	}

	return false;
#else
	return true;
#endif
}

bool CTFSMG::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	return ShouldUseVRSMGManualReload();
}

bool CTFSMG::Reload()
{
	if ( ShouldUseVRSMGManualReload() )
		return false;

	return BaseClass::Reload();
}

float CTFSMG::GetVRAmmoEjectDuration() const
{
	return MAX( tfvr_smg_ammo_eject_duration.GetFloat(), 0.05f );
}

float CTFSMG::GetVRAmmoInsertDuration() const
{
	return MAX( tfvr_smg_ammo_insert_duration.GetFloat(), 0.05f );
}

float CTFSMG::GetVRAmmoFinishDuration() const
{
	return MAX( tfvr_smg_ammo_finish_duration.GetFloat(), 0.05f );
}

float CTFSMG::GetVRAmmoPhaseProgress() const
{
	float flDuration = 0.0f;
	switch ( m_iVRAmmoPhase )
	{
	case VR_SMG_AMMO_PHASE_EJECTING:	flDuration = GetVRAmmoEjectDuration(); break;
	case VR_SMG_AMMO_PHASE_INSERTING:	flDuration = GetVRAmmoInsertDuration(); break;
	case VR_SMG_AMMO_PHASE_FINISHING:	flDuration = GetVRAmmoFinishDuration(); break;
	default:
		return 0.0f;
	}

	if ( flDuration <= 0.0f )
		return 1.0f;

	return clamp( ( gpGlobals->curtime - m_flVRAmmoPhaseStartTime ) / flDuration, 0.0f, 1.0f );
}

bool CTFSMG::CanStartVRAmmoPull() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || m_bVRAmmoHeld )
		return false;

	return pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0;
}

bool CTFSMG::CanStartVRAmmoEject() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( m_iVRAmmoPhase != VR_SMG_AMMO_PHASE_IDLE || m_bVRAmmoOut )
		return false;

	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
		return false;

	if ( m_iClip1 >= GetMaxClip1() )
		return false;

	return true;
}

void CTFSMG::ResetVRSMGAmmoState()
{
	m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_IDLE;
	m_flVRAmmoPhaseStartTime = 0.0f;
	m_bVRAmmoOut = false;
	m_bVRAmmoHeld = false;
#ifdef GAME_DLL
	m_bVRAmmoPhysSpawned = false;
#endif
}

void CTFSMG::VRStartAmmoEject()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner || !CanStartVRAmmoEject() )
		return;

#ifdef GAME_DLL
	if ( m_iClip1 > 0 )
		pOwner->GiveAmmo( m_iClip1, m_iPrimaryAmmoType, true );
	m_bVRAmmoPhysSpawned = false;
#endif
	m_iClip1 = 0;
	m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_EJECTING;
	m_flVRAmmoPhaseStartTime = gpGlobals->curtime;
	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, gpGlobals->curtime + GetVRAmmoEjectDuration() );

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.SMGClipOut" );
#endif
}

void CTFSMG::VRStartAmmoInsert()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner || !m_bVRAmmoHeld || !m_bVRAmmoOut || m_iVRAmmoPhase != VR_SMG_AMMO_PHASE_IDLE )
		return;

	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		m_bVRAmmoHeld = false;
		return;
	}

	m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_INSERTING;
	m_flVRAmmoPhaseStartTime = gpGlobals->curtime;

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.SMGClipIn" );
#endif
}

void CTFSMG::VRCommitAmmoInsert()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner || m_iVRAmmoPhase != VR_SMG_AMMO_PHASE_INSERTING )
		return;

	int iFill = MIN( GetMaxClip1(), pOwner->GetAmmoCount( m_iPrimaryAmmoType ) );
	if ( iFill > 0 )
	{
		m_iClip1 = iFill;
		pOwner->RemoveAmmo( iFill, m_iPrimaryAmmoType );
	}

	m_bVRAmmoHeld = false;
	m_bVRAmmoOut = false;
	m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_FINISHING;
	m_flVRAmmoPhaseStartTime = gpGlobals->curtime;
	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, gpGlobals->curtime + GetVRAmmoFinishDuration() );
}

#ifdef GAME_DLL
void CTFSMG::VRSpawnEjectedAmmo()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner )
		return;

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
		return;

	const bool bRightHand = pCmd->vrWeaponHandIsRight;
	Vector vecHandOrigin = bRightHand ? pCmd->rightControllerOrigin : pCmd->leftControllerOrigin;
	QAngle angHand = bRightHand ? pCmd->rightControllerAngles : pCmd->leftControllerAngles;

	if ( vecHandOrigin == vec3_origin )
	{
		vecHandOrigin = GetAbsOrigin();
		angHand = GetAbsAngles();
	}

	Vector vecForward, vecRight, vecUp;
	AngleVectors( angHand, &vecForward, &vecRight, &vecUp );
	Vector vecEjectDir = -vecUp * 0.9f - vecForward * 0.2f;
	VectorNormalize( vecEjectDir );

	Vector vecSpawnPos = pCmd->vrMagSpawnOrigin != vec3_origin
		? pCmd->vrMagSpawnOrigin
		: vecHandOrigin + vecEjectDir * 3.0f;
	QAngle angSpawn = pCmd->vrMagSpawnOrigin != vec3_origin
		? pCmd->vrMagSpawnAngles
		: angHand;

	CTFVRWeaponMagazine *pAmmo = (CTFVRWeaponMagazine *)CreateEntityByName( "tfvr_weapon_magazine" );
	if ( !pAmmo )
		return;

	pAmmo->SetWeaponType( GetWeaponID() );
	pAmmo->SetMagazineModel( VRSMG_AmmoModelForWorldModel( GetWorldModel() ) );
	pAmmo->SetAmmoCount( 0 );
	pAmmo->SetAbsOrigin( vecSpawnPos );
	pAmmo->SetAbsAngles( angSpawn );
	pAmmo->Spawn();

	IPhysicsObject *pPhys = pAmmo->VPhysicsGetObject();
	if ( pPhys )
	{
		Vector vecVel = pCmd->vrMagEjectVel != vec3_origin
			? pCmd->vrMagEjectVel + pOwner->GetAbsVelocity()
			: vecEjectDir * tfvr_smg_ammo_eject_speed.GetFloat() + pOwner->GetAbsVelocity();
		pPhys->SetVelocity( &vecVel, NULL );
	}
}
#endif

void CTFSMG::VRSMGAmmoPostFrame()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner )
	{
		ResetVRSMGAmmoState();
		return;
	}

	if ( m_bVRAmmoOut && m_iVRAmmoPhase == VR_SMG_AMMO_PHASE_IDLE && m_iClip1 > 0 )
		m_bVRAmmoOut = false;

	const float flProgress = GetVRAmmoPhaseProgress();
	switch ( m_iVRAmmoPhase )
	{
	case VR_SMG_AMMO_PHASE_EJECTING:
	{
		const float flEjectFrames = MAX( VRSMG_FramePause() - VRSMG_FrameEjectStart(), 1.0f );
		const float flAmmoFreeProgress = clamp( ( VRSMG_FrameAmmoFree() - VRSMG_FrameEjectStart() ) / flEjectFrames, 0.0f, 1.0f );
		if ( !m_bVRAmmoOut && flProgress >= flAmmoFreeProgress )
		{
			m_bVRAmmoOut = true;
#ifdef GAME_DLL
			if ( !m_bVRAmmoPhysSpawned )
			{
				m_bVRAmmoPhysSpawned = true;
				VRSpawnEjectedAmmo();
			}
#endif
		}

		if ( flProgress >= 1.0f )
		{
			m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_IDLE;
			m_flVRAmmoPhaseStartTime = 0.0f;
		}
		break;
	}
	case VR_SMG_AMMO_PHASE_INSERTING:
		if ( flProgress >= 1.0f )
			VRCommitAmmoInsert();
		break;
	case VR_SMG_AMMO_PHASE_FINISHING:
		if ( flProgress >= 1.0f )
		{
			m_iVRAmmoPhase = VR_SMG_AMMO_PHASE_IDLE;
			m_flVRAmmoPhaseStartTime = 0.0f;
		}
		break;
	default:
		break;
	}

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
		return;

	if ( m_bVRAmmoHeld && !pCmd->vrMagazineHold )
		m_bVRAmmoHeld = false;

	if ( pCmd->vrMagazinePull && CanStartVRAmmoPull() )
	{
		m_bVRAmmoHeld = true;
#ifdef CLIENT_DLL
		if ( prediction->IsFirstTimePredicted() )
			EmitSound( "VR.ManualReloadAmmoGrab" );
#endif
	}

	if ( m_iVRAmmoPhase == VR_SMG_AMMO_PHASE_IDLE )
	{
		if ( pCmd->vrMagazineEject && !m_bVRAmmoOut )
			VRStartAmmoEject();

		if ( m_bVRAmmoOut && m_bVRAmmoHeld && pCmd->vrMagazineInsert )
			VRStartAmmoInsert();
	}
}

void CTFSMG::ItemPostFrame()
{
	if ( ShouldUseVRSMGManualReload() )
	{
		VRSMGAmmoPostFrame();
	}
	else if ( IsVRSMGManualReloadBusy() || m_bVRAmmoOut || m_bVRAmmoHeld )
	{
		ResetVRSMGAmmoState();
	}

	BaseClass::ItemPostFrame();
}

void CTFSMG::ItemBusyFrame()
{
	if ( ShouldUseVRSMGManualReload() )
		VRSMGAmmoPostFrame();

	BaseClass::ItemBusyFrame();
}

void CTFSMG::PrimaryAttack()
{
	if ( ShouldUseVRSMGManualReload() && ( IsVRSMGManualReloadBusy() || m_bVRAmmoOut ) )
	{
		if ( m_flNextEmptySoundTime < gpGlobals->curtime )
		{
			WeaponSound( EMPTY );
			m_flNextEmptySoundTime = gpGlobals->curtime + 0.5f;
		}
		m_flNextPrimaryAttack = gpGlobals->curtime + 0.1f;
		return;
	}

	BaseClass::PrimaryAttack();
}

void CTFSMG::HandleFireOnEmpty()
{
	if ( ShouldUseVRSMGManualReload() )
	{
		if ( CanStartVRAmmoEject() && CanAttack() )
		{
			VRStartAmmoEject();
			return;
		}

		if ( m_flNextEmptySoundTime < gpGlobals->curtime )
		{
			WeaponSound( EMPTY );
			m_flNextEmptySoundTime = gpGlobals->curtime + 0.5f;
		}
		return;
	}

	BaseClass::HandleFireOnEmpty();
}

//-----------------------------------------------------------------------------
// Purpose:	Determine if secondary fire is available.
//-----------------------------------------------------------------------------
bool CTFChargedSMG::CanPerformSecondaryAttack() const
{
	return ( m_flMinicritCharge >= DAMAGE_TO_FILL_MINICRIT_METER && BaseClass::CanPerformSecondaryAttack() );
}

//-----------------------------------------------------------------------------
// Purpose: Determine whether to flash the HUD element showing the charge bar
//-----------------------------------------------------------------------------
bool CTFChargedSMG::ShouldFlashChargeBar()
{
	return m_flMinicritCharge >= DAMAGE_TO_FILL_MINICRIT_METER;
}

//-----------------------------------------------------------------------------
// Purpose: Get HUD charge bar progress amount
//-----------------------------------------------------------------------------
float CTFChargedSMG::GetProgress( void )
{
	// Progress bar shows charge amount if we're charging up, otherwise drains over time if we're mini-crit boosted.
	CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
	if ( pPlayer && pPlayer->m_Shared.InCond( TF_COND_ENERGY_BUFF ) )
	{
		int flBuffDuration = 0;
		CALL_ATTRIB_HOOK_FLOAT( flBuffDuration, minicrit_boost_when_charged );
		if ( flBuffDuration > 0 )
		{
			float flElapsed = gpGlobals->curtime - m_flMinicritStartTime;
			float flRemainingPortion = Clamp( (flBuffDuration - flElapsed) / flBuffDuration, 0.0f, 1.0f );
			return flRemainingPortion;
		}
		else
		{
			return 0.0f;
		}
	}
	else
	{
		return m_flMinicritCharge / DAMAGE_TO_FILL_MINICRIT_METER;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Reset weapon state
//-----------------------------------------------------------------------------
void CTFChargedSMG::WeaponReset()
{
	BaseClass::WeaponReset();
	m_flMinicritCharge = 0.0f;
	m_flMinicritStartTime = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose: Perform secondary attack
//-----------------------------------------------------------------------------
void CTFChargedSMG::SecondaryAttack()
{
	BaseClass::SecondaryAttack();

	m_flMinicritCharge = 0.0f;

	CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
	if ( pPlayer )
	{
		float flBuffDuration = 0;
		CALL_ATTRIB_HOOK_FLOAT( flBuffDuration, minicrit_boost_when_charged );
		if ( flBuffDuration > 0 )
		{
			pPlayer->m_Shared.AddCond( TF_COND_ENERGY_BUFF, flBuffDuration );
			m_flMinicritStartTime = gpGlobals->curtime;
		}
	}
}

#ifdef GAME_DLL
//-----------------------------------------------------------------------------
// Purpose: Update state when we score a hit with this weapon
//-----------------------------------------------------------------------------
void CTFChargedSMG::ApplyOnHitAttributes( CBaseEntity *pVictimBaseEntity, CTFPlayer *pAttacker, const CTakeDamageInfo &info )
{
	BaseClass::ApplyOnHitAttributes( pVictimBaseEntity, pAttacker, info );
	if ( pAttacker )
	{
		CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
		if ( pPlayer && !pPlayer->m_Shared.InCond( TF_COND_ENERGY_BUFF ) )
		{
			float damage = info.GetDamage();
			float flChargeRate = 0.0f;
			CALL_ATTRIB_HOOK_FLOAT( flChargeRate, minicrit_boost_charge_rate );
			m_flMinicritCharge += damage * flChargeRate;
			if ( m_flMinicritCharge > DAMAGE_TO_FILL_MINICRIT_METER )
			{
				m_flMinicritCharge = DAMAGE_TO_FILL_MINICRIT_METER;
			}
		}
	}
}
#endif
