//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_syringegun.h"
#include "tf_fx_shared.h"
#include "tf_weapon_medigun.h"
#include "usercmd.h"

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

ConVar tfvr_syringegun_manual_reload( "tfvr_syringegun_manual_reload", "1", FCVAR_ARCHIVE, "VR syringe gun: 1 = manually eject and insert ammo; 0 = standard auto reload. Toggled by the TF2VR auto-reload option." );
ConVar tfvr_syringegun_ammo_eject_duration( "tfvr_syringegun_ammo_eject_duration", "0.62", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR syringe gun: seconds for sg_reload frames 0-18." );
ConVar tfvr_syringegun_ammo_insert_duration( "tfvr_syringegun_ammo_insert_duration", "0.20", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR syringe gun: seconds for the hand-loaded ammo insert." );
ConVar tfvr_syringegun_ammo_finish_duration( "tfvr_syringegun_ammo_finish_duration", "0.45", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR syringe gun: seconds for the post-insert finish motion." );
ConVar tfvr_syringegun_ammo_eject_speed( "tfvr_syringegun_ammo_eject_speed", "30", FCVAR_REPLICATED, "VR syringe gun: fallback initial speed (u/s) of the dropped ammo along its eject direction." );

//=============================================================================
//
// Weapon Syringe Gun tables.
//
IMPLEMENT_NETWORKCLASS_ALIASED( TFSyringeGun, DT_WeaponSyringeGun )

BEGIN_NETWORK_TABLE( CTFSyringeGun, DT_WeaponSyringeGun )
#if defined( CLIENT_DLL )
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

BEGIN_PREDICTION_DATA( CTFSyringeGun )
#ifdef CLIENT_DLL
	DEFINE_PRED_FIELD( m_iVRAmmoPhase, FIELD_INTEGER, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRAmmoPhaseStartTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRAmmoOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRAmmoHeld, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
#endif
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_syringegun_medic, CTFSyringeGun );
PRECACHE_WEAPON_REGISTER( tf_weapon_syringegun_medic );

// Server specific.
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFSyringeGun )
END_DATADESC()
#endif

//=============================================================================
//
// Weapon SyringeGun functions.
//
CTFSyringeGun::CTFSyringeGun()
{
	m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_IDLE;
	m_flVRAmmoPhaseStartTime = 0.0f;
	m_bVRAmmoOut = false;
	m_bVRAmmoHeld = false;
#ifdef GAME_DLL
	m_bVRAmmoPhysSpawned = false;
#endif
}

void CTFSyringeGun::Precache()
{
	BaseClass::Precache();

	PrecacheModel( VRSyringeGun_GunModelName() );
	PrecacheModel( VRSyringeGun_AmmoModelName() );
	PrecacheModel( "models/weapons/vr_models/vr_proto_syringegun/vr_proto_syringegun.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_proto_syringegun/vr_proto_syringegun_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_leechgun/vr_leechgun.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_leechgun/vr_leechgun_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_crusaders_crossbow/vr_crusaders_crossbow.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_crusaders_crossbow/vr_crusaders_crossbow_ammo.mdl" );
	PrecacheScriptSound( "VR.SyringeGunEject" );
	PrecacheScriptSound( "VR.SyringeGunReload" );
	PrecacheScriptSound( "VR.SyringeGunAmmoGrab" );

#ifndef CLIENT_DLL
	PrecacheParticleSystem( "nailtrails_medic_blue_crit" );
	PrecacheParticleSystem( "nailtrails_medic_blue" );
	PrecacheParticleSystem( "nailtrails_medic_red_crit" );
	PrecacheParticleSystem( "nailtrails_medic_red" );
#endif
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFSyringeGun::Deploy()
{
	CBaseEntity *pOwner = GetOwnerEntity();
	if ( pOwner )
	{
		ToTFPlayer( pOwner )->TeamFortress_SetSpeed();
	}
	
	return BaseClass::Deploy();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFSyringeGun::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	CBaseEntity *pOwner = GetOwnerEntity();
	if ( pOwner )
	{
		ToTFPlayer( pOwner )->TeamFortress_SetSpeed();
	}

	m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_IDLE;
	m_flVRAmmoPhaseStartTime = 0.0f;
	m_bVRAmmoHeld = false;

	return BaseClass::Holster( pSwitchingTo );
}

bool CTFSyringeGun::ShouldUseVRSyringeGunManualReload() const
{
	if ( !tfvr_syringegun_manual_reload.GetBool() )
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

bool CTFSyringeGun::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	return ShouldUseVRSyringeGunManualReload();
}

bool CTFSyringeGun::Reload()
{
	if ( ShouldUseVRSyringeGunManualReload() )
		return false;

	return BaseClass::Reload();
}

float CTFSyringeGun::GetVRAmmoEjectDuration() const
{
	return MAX( tfvr_syringegun_ammo_eject_duration.GetFloat(), 0.05f );
}

float CTFSyringeGun::GetVRAmmoInsertDuration() const
{
	return MAX( tfvr_syringegun_ammo_insert_duration.GetFloat(), 0.05f );
}

float CTFSyringeGun::GetVRAmmoFinishDuration() const
{
	return MAX( tfvr_syringegun_ammo_finish_duration.GetFloat(), 0.05f );
}

float CTFSyringeGun::GetVRAmmoPhaseProgress() const
{
	float flDuration = 0.0f;
	switch ( m_iVRAmmoPhase )
	{
	case VR_SYRINGEGUN_AMMO_PHASE_EJECTING:	flDuration = GetVRAmmoEjectDuration(); break;
	case VR_SYRINGEGUN_AMMO_PHASE_INSERTING:	flDuration = GetVRAmmoInsertDuration(); break;
	case VR_SYRINGEGUN_AMMO_PHASE_FINISHING:	flDuration = GetVRAmmoFinishDuration(); break;
	default:
		return 0.0f;
	}

	if ( flDuration <= 0.0f )
		return 1.0f;

	return clamp( ( gpGlobals->curtime - m_flVRAmmoPhaseStartTime ) / flDuration, 0.0f, 1.0f );
}

bool CTFSyringeGun::CanStartVRAmmoPull() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || m_bVRAmmoHeld )
		return false;

	return pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0;
}

bool CTFSyringeGun::CanStartVRAmmoEject() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( m_iVRAmmoPhase != VR_SYRINGEGUN_AMMO_PHASE_IDLE || m_bVRAmmoOut )
		return false;

	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
		return false;

	if ( m_iClip1 >= GetMaxClip1() )
		return false;

	return true;
}

void CTFSyringeGun::ResetVRSyringeGunAmmoState()
{
	m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_IDLE;
	m_flVRAmmoPhaseStartTime = 0.0f;
	m_bVRAmmoOut = false;
	m_bVRAmmoHeld = false;
#ifdef GAME_DLL
	m_bVRAmmoPhysSpawned = false;
#endif
}

void CTFSyringeGun::VRStartAmmoEject()
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
	m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_EJECTING;
	m_flVRAmmoPhaseStartTime = gpGlobals->curtime;
	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, gpGlobals->curtime + GetVRAmmoEjectDuration() );

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.SyringeGunEject" );
#endif
}

void CTFSyringeGun::VRStartAmmoInsert()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner || !m_bVRAmmoHeld || !m_bVRAmmoOut || m_iVRAmmoPhase != VR_SYRINGEGUN_AMMO_PHASE_IDLE )
		return;

	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		m_bVRAmmoHeld = false;
		return;
	}

	m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_INSERTING;
	m_flVRAmmoPhaseStartTime = gpGlobals->curtime;

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.SyringeGunReload" );
#endif
}

void CTFSyringeGun::VRCommitAmmoInsert()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner || m_iVRAmmoPhase != VR_SYRINGEGUN_AMMO_PHASE_INSERTING )
		return;

	int iFill = MIN( GetMaxClip1(), pOwner->GetAmmoCount( m_iPrimaryAmmoType ) );
	if ( iFill > 0 )
	{
		m_iClip1 = iFill;
		pOwner->RemoveAmmo( iFill, m_iPrimaryAmmoType );
	}

	m_bVRAmmoHeld = false;
	m_bVRAmmoOut = false;
	m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_FINISHING;
	m_flVRAmmoPhaseStartTime = gpGlobals->curtime;
	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, gpGlobals->curtime + GetVRAmmoFinishDuration() );
}

#ifdef GAME_DLL
void CTFSyringeGun::VRSpawnEjectedAmmo()
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
	pAmmo->SetMagazineModel( VRSyringeGun_AmmoModelForWorldModel( GetWorldModel() ) );
	pAmmo->SetAmmoCount( 0 );
	pAmmo->SetAbsOrigin( vecSpawnPos );
	pAmmo->SetAbsAngles( angSpawn );
	pAmmo->Spawn();

	IPhysicsObject *pPhys = pAmmo->VPhysicsGetObject();
	if ( pPhys )
	{
		Vector vecVel = pCmd->vrMagEjectVel != vec3_origin
			? pCmd->vrMagEjectVel + pOwner->GetAbsVelocity()
			: vecEjectDir * tfvr_syringegun_ammo_eject_speed.GetFloat() + pOwner->GetAbsVelocity();
		pPhys->SetVelocity( &vecVel, NULL );
	}
}
#endif

void CTFSyringeGun::VRSyringeGunAmmoPostFrame()
{
	CTFPlayer *pOwner = ToTFPlayer( GetOwnerEntity() );
	if ( !pOwner )
	{
		ResetVRSyringeGunAmmoState();
		return;
	}

	if ( m_bVRAmmoOut && m_iVRAmmoPhase == VR_SYRINGEGUN_AMMO_PHASE_IDLE && m_iClip1 > 0 )
		m_bVRAmmoOut = false;

	const float flProgress = GetVRAmmoPhaseProgress();
	switch ( m_iVRAmmoPhase )
	{
	case VR_SYRINGEGUN_AMMO_PHASE_EJECTING:
	{
		const float flAmmoFreeProgress = VRSyringeGun_FrameAmmoFree() / VRSyringeGun_FramePause();
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
			m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_IDLE;
			m_flVRAmmoPhaseStartTime = 0.0f;
		}
		break;
	}
	case VR_SYRINGEGUN_AMMO_PHASE_INSERTING:
		if ( flProgress >= 1.0f )
			VRCommitAmmoInsert();
		break;
	case VR_SYRINGEGUN_AMMO_PHASE_FINISHING:
		if ( flProgress >= 1.0f )
		{
			m_iVRAmmoPhase = VR_SYRINGEGUN_AMMO_PHASE_IDLE;
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
			EmitSound( "VR.SyringeGunAmmoGrab" );
#endif
	}

	if ( m_iVRAmmoPhase == VR_SYRINGEGUN_AMMO_PHASE_IDLE )
	{
		if ( pCmd->vrMagazineEject && !m_bVRAmmoOut )
			VRStartAmmoEject();

		if ( m_bVRAmmoOut && m_bVRAmmoHeld && pCmd->vrMagazineInsert )
			VRStartAmmoInsert();
	}
}

void CTFSyringeGun::ItemPostFrame()
{
	if ( ShouldUseVRSyringeGunManualReload() )
	{
		VRSyringeGunAmmoPostFrame();
	}
	else if ( IsVRSyringeGunManualReloadBusy() || m_bVRAmmoOut || m_bVRAmmoHeld )
	{
		ResetVRSyringeGunAmmoState();
	}

	BaseClass::ItemPostFrame();
}

void CTFSyringeGun::ItemBusyFrame()
{
	if ( ShouldUseVRSyringeGunManualReload() )
		VRSyringeGunAmmoPostFrame();

	BaseClass::ItemBusyFrame();
}

void CTFSyringeGun::PrimaryAttack()
{
	if ( ShouldUseVRSyringeGunManualReload() && ( IsVRSyringeGunManualReloadBusy() || m_bVRAmmoOut ) )
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

void CTFSyringeGun::HandleFireOnEmpty()
{
	if ( ShouldUseVRSyringeGunManualReload() )
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

void CTFSyringeGun::RemoveProjectileAmmo( CTFPlayer *pPlayer )
{
	float flUberChargeAmmoPerShot = UberChargeAmmoPerShot();
	if ( flUberChargeAmmoPerShot > 0.0f )
	{
#ifndef CLIENT_DLL
		if ( !pPlayer )
			return;

		CWeaponMedigun *pMedigun = static_cast< CWeaponMedigun * >( pPlayer->Weapon_OwnsThisID( TF_WEAPON_MEDIGUN ) );
		if ( !pMedigun )
			return;

		pMedigun->SubtractCharge( flUberChargeAmmoPerShot );
#endif
		return;
	}

	return BaseClass::RemoveProjectileAmmo( pPlayer );
}

bool CTFSyringeGun::HasPrimaryAmmo( void )
{
	float flUberChargeAmmoPerShot = UberChargeAmmoPerShot();
	if ( flUberChargeAmmoPerShot > 0.0f )
	{
		CTFPlayer *pPlayer = ToTFPlayer( GetOwnerEntity() );
		if ( !pPlayer )
			return false;

		CWeaponMedigun *pMedigun = static_cast< CWeaponMedigun * >( pPlayer->Weapon_OwnsThisID( TF_WEAPON_MEDIGUN ) );
		if ( !pMedigun )
			return false;

		float flCharge = pMedigun->GetChargeLevel();
		return flUberChargeAmmoPerShot <= flCharge;
	}

	return BaseClass::HasPrimaryAmmo();
}
