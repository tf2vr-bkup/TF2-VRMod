//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_pistol.h"
#include "tf_fx_shared.h"
#include "in_buttons.h"
#include "tf_gamerules.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include "c_tf_gamestats.h"
#include "prediction.h"
#include "tfvr/c_tfvr_hand.h"
// Server specific.
#else
#include "tf_player.h"
#include "tf_gamestats.h"
#include "ilagcompensationmanager.h"
#include "tfvr/tfvr_weapon_magazine.h"
#endif

#include "usercmd.h"

// VR manual magazine reload for pistols. 1 = eject/insert mags by hand, 0 = vanilla auto-reload.
ConVar tfvr_pistol_manual_reload( "tfvr_pistol_manual_reload", "1", FCVAR_ARCHIVE, "VR pistols: 1 = manually eject and insert magazines; 0 = standard auto reload. Toggled by the TF2VR auto-reload option." );
ConVar tfvr_pistol_mag_eject_duration( "tfvr_pistol_mag_eject_duration", "0.55", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR pistol: seconds for the mag eject motion (p_reload frames 0-16)" );
ConVar tfvr_pistol_mag_insert_duration( "tfvr_pistol_mag_insert_duration", "0.20", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR pistol: seconds for the mag insert motion (p_reload frames 17-19)" );
ConVar tfvr_pistol_mag_finish_duration( "tfvr_pistol_mag_finish_duration", "0.45", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR pistol: seconds for the reload finish motion (p_reload frame 19 to end)" );
// Replicated so the client-side ballistic handoff mag matches the server prop.
ConVar tfvr_pistol_mag_eject_speed( "tfvr_pistol_mag_eject_speed", "30", FCVAR_REPLICATED, "VR pistol: initial speed (u/s) of the dropped magazine along its eject direction" );
#ifdef GAME_DLL
ConVar tfvr_pistol_mag_eject_drop( "tfvr_pistol_mag_eject_drop", "3", FCVAR_NONE, "VR pistol: how far below the grip (inches) the dropped magazine spawns" );
#endif

//=============================================================================
//
// Weapon Pistol tables.
//
IMPLEMENT_NETWORKCLASS_ALIASED( TFPistol, DT_WeaponPistol )

BEGIN_NETWORK_TABLE( CTFPistol, DT_WeaponPistol )
#if defined( CLIENT_DLL )
	RecvPropInt( RECVINFO( m_iVRMagPhase ) ),
	RecvPropFloat( RECVINFO( m_flVRMagPhaseStartTime ) ),
	RecvPropFloat( RECVINFO( m_flVRMagEarliestReadyTime ) ),
	RecvPropBool( RECVINFO( m_bVRMagOut ) ),
	RecvPropBool( RECVINFO( m_bVRMagazineHeld ) ),
#else
	SendPropInt( SENDINFO( m_iVRMagPhase ), 3, SPROP_UNSIGNED ),
	SendPropFloat( SENDINFO( m_flVRMagPhaseStartTime ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flVRMagEarliestReadyTime ), 0, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRMagOut ) ),
	SendPropBool( SENDINFO( m_bVRMagazineHeld ) ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFPistol )
#ifdef CLIENT_DLL
	DEFINE_PRED_FIELD( m_iVRMagPhase, FIELD_INTEGER, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRMagPhaseStartTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRMagEarliestReadyTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRMagOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRMagazineHeld, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
#endif
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_pistol, CTFPistol );
PRECACHE_WEAPON_REGISTER( tf_weapon_pistol );

// Server specific.
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFPistol )
END_DATADESC()
#endif

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFPistol::CTFPistol()
{
	m_iVRMagPhase = VR_PISTOL_MAG_PHASE_IDLE;
	m_flVRMagPhaseStartTime = 0.0f;
	m_flVRMagEarliestReadyTime = 0.0f;
	m_bVRMagOut = false;
	m_bVRMagazineHeld = false;
#ifdef GAME_DLL
	m_bVRMagPhysSpawned = false;
#endif
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFPistol::Precache()
{
	BaseClass::Precache();

	PrecacheModel( "models/weapons/vr_models/vr_pistol/vr_pistol.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_pistol/vr_pistol_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_winger_pistol/vr_winger_pistol.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_winger_pistol/vr_winger_pistol_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_pep_pistol/vr_pep_pistol.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_pep_pistol/vr_pep_pistol_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_invasion_pistol/vr_invasion_pistol.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_invasion_pistol/vr_invasion_pistol_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_ttg_max_gun/vr_ttg_max_gun.mdl" );
	PrecacheScriptSound( "VR.PistolReloadScout" );
	PrecacheScriptSound( "VR.PistolClipIn" );
}

//-----------------------------------------------------------------------------
// Purpose: Manual reload for the stock Scout and Engineer pistols.
//-----------------------------------------------------------------------------
bool CTFPistol::ShouldUseVRPistolManualReload() const
{
	if ( !VRPistol_IsManualReloadWeaponID( GetWeaponID() ) )
		return false;

	if ( !tfvr_pistol_manual_reload.GetBool() )
		return false;

	const char *pszWorldModel = GetWorldModel();
	if ( !VRPistol_HasVRModelForWorldModel( pszWorldModel ) )
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

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFPistol::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	return ShouldUseVRPistolManualReload();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFPistol::Reload()
{
	if ( ShouldUseVRPistolManualReload() )
		return false;

	return BaseClass::Reload();
}

int CTFPistol::GetVRPistolReloadWeaponID() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( pOwner )
	{
		const int iClass = pOwner->GetPlayerClass()->GetClassIndex();
		if ( iClass == TF_CLASS_ENGINEER )
			return TF_WEAPON_PISTOL;
		if ( iClass == TF_CLASS_SCOUT )
			return TF_WEAPON_PISTOL_SCOUT;
	}

	return GetWeaponID();
}

float CTFPistol::GetVRMagEjectDuration() const
{
	// The convar is tuned for the scout's 16-frame eject; scale by the
	// weapon's actual eject length so animation speed stays consistent
	// (engineer ejects in 4 frames).
	const float flFrameScale = VRPistol_FramePause( GetVRPistolReloadWeaponID() ) / 16.0f;
	return MAX( tfvr_pistol_mag_eject_duration.GetFloat() * flFrameScale, 0.05f );
}

float CTFPistol::GetVRMagInsertDuration() const
{
	return MAX( tfvr_pistol_mag_insert_duration.GetFloat(), 0.05f );
}

float CTFPistol::GetVRMagFinishDuration() const
{
	return MAX( tfvr_pistol_mag_finish_duration.GetFloat(), 0.05f );
}

float CTFPistol::GetVRMagPhaseProgress() const
{
	float flDuration = 0.0f;
	switch ( m_iVRMagPhase )
	{
	case VR_PISTOL_MAG_PHASE_EJECTING:	flDuration = GetVRMagEjectDuration(); break;
	case VR_PISTOL_MAG_PHASE_INSERTING:	flDuration = GetVRMagInsertDuration(); break;
	case VR_PISTOL_MAG_PHASE_FINISHING:	flDuration = GetVRMagFinishDuration(); break;
	default:
		return 0.0f;
	}

	if ( flDuration <= 0.0f )
		return 1.0f;

	return clamp( ( gpGlobals->curtime - m_flVRMagPhaseStartTime ) / flDuration, 0.0f, 1.0f );
}

//-----------------------------------------------------------------------------
// Purpose: Can the off hand pull a fresh mag from the backpack?
//-----------------------------------------------------------------------------
bool CTFPistol::CanStartVRMagPull() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( m_bVRMagazineHeld )
		return false;

	return pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0;
}

//-----------------------------------------------------------------------------
// Purpose: Is there any point in ejecting the mag right now?
//-----------------------------------------------------------------------------
bool CTFPistol::CanStartVRMagEject() const
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( m_iVRMagPhase != VR_PISTOL_MAG_PHASE_IDLE || m_bVRMagOut )
		return false;

	// No reserve ammo: nothing to load, don't start the reload.
	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
		return false;

	// Clip is already full: a reload accomplishes nothing.
	if ( m_iClip1 >= GetMaxClip1() )
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFPistol::ResetVRPistolMagState()
{
	m_iVRMagPhase = VR_PISTOL_MAG_PHASE_IDLE;
	m_flVRMagPhaseStartTime = 0.0f;
	m_flVRMagEarliestReadyTime = 0.0f;
	m_bVRMagOut = false;
	m_bVRMagazineHeld = false;
#ifdef GAME_DLL
	m_bVRMagPhysSpawned = false;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Start the eject motion. Rounds left in the mag return to reserve
//          (arcade accounting), the clip empties, and firing locks out until
//          a fresh mag is seated.
//-----------------------------------------------------------------------------
void CTFPistol::VRStartMagEject()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !CanStartVRMagEject() )
		return;

#ifdef GAME_DLL
	if ( m_iClip1 > 0 )
		pOwner->GiveAmmo( m_iClip1, m_iPrimaryAmmoType, true );
	m_bVRMagPhysSpawned = false;
#endif
	m_iClip1 = 0;
	m_iVRMagPhase = VR_PISTOL_MAG_PHASE_EJECTING;
	m_flVRMagPhaseStartTime = gpGlobals->curtime;
	const float flReadyFrame = VRPistol_FrameReady( GetVRPistolReloadWeaponID() );
	m_flVRMagEarliestReadyTime = flReadyFrame > 0.0f
		? gpGlobals->curtime + flReadyFrame / VR_PISTOL_RELOAD_ANIM_FPS
		: 0.0f;

	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, gpGlobals->curtime + GetVRMagEjectDuration() );

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.PistolReloadScout" );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Off-hand mag reached the magwell: play frames 17-19.
//-----------------------------------------------------------------------------
void CTFPistol::VRStartMagInsert()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !m_bVRMagazineHeld || !m_bVRMagOut || m_iVRMagPhase != VR_PISTOL_MAG_PHASE_IDLE )
		return;

	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		m_bVRMagazineHeld = false;
		return;
	}

	m_iVRMagPhase = VR_PISTOL_MAG_PHASE_INSERTING;
	m_flVRMagPhaseStartTime = gpGlobals->curtime;

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.PistolClipIn" );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Mag is seated: fill the clip from reserve, detach the off hand,
//          and let the weapon hand play out the rest of the reload.
//-----------------------------------------------------------------------------
void CTFPistol::VRCommitMagInsert()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || m_iVRMagPhase != VR_PISTOL_MAG_PHASE_INSERTING )
		return;

	int iFill = MIN( GetMaxClip1(), pOwner->GetAmmoCount( m_iPrimaryAmmoType ) );
	if ( iFill > 0 )
	{
		m_iClip1 = iFill;
		pOwner->RemoveAmmo( iFill, m_iPrimaryAmmoType );
	}

	m_bVRMagazineHeld = false;
	m_bVRMagOut = false;
	m_iVRMagPhase = VR_PISTOL_MAG_PHASE_FINISHING;
	m_flVRMagPhaseStartTime = gpGlobals->curtime;

	const float flReadyTime = Max<float>( gpGlobals->curtime + GetVRMagFinishDuration(), m_flVRMagEarliestReadyTime );
	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, flReadyTime );

	// Sound: VR.PistolClipIn already played at insert start.
}

#ifdef GAME_DLL
//-----------------------------------------------------------------------------
// Purpose: Spawn the cosmetic dropped magazine once the mag clears the gun
//          (p_reload frame 6). Spawns at the weapon-hand controller with a
//          small push along the magwell direction; gravity does the rest.
//-----------------------------------------------------------------------------
void CTFPistol::VRSpawnEjectedMagazine()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
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

	// Fallback eject direction: magwell points down the grip
	// (mostly -up, slightly back). Used only when the client did not
	// report the animation-derived velocity.
	Vector vecEjectDir = -vecUp * 0.9f - vecForward * 0.2f;
	VectorNormalize( vecEjectDir );

	// Prefer the exact bone-derived mag transform reported by the client
	// (sampled one animation frame ahead so the prop starts clear of the
	// gun); fall back to a controller-based estimate when unavailable.
	Vector vecSpawnPos;
	QAngle angSpawn;
	if ( pCmd->vrMagSpawnOrigin != vec3_origin )
	{
		vecSpawnPos = pCmd->vrMagSpawnOrigin;
		angSpawn = pCmd->vrMagSpawnAngles;
	}
	else
	{
		vecSpawnPos = vecHandOrigin + vecEjectDir * MAX( tfvr_pistol_mag_eject_drop.GetFloat(), 0.0f );
		angSpawn = angHand;
	}

	CTFVRWeaponMagazine *pMag = (CTFVRWeaponMagazine *)CreateEntityByName( "tfvr_weapon_magazine" );
	if ( !pMag )
		return;

	pMag->SetWeaponType( GetVRPistolReloadWeaponID() );
	pMag->SetMagazineModel( VRPistol_AmmoModelForWorldModel( GetWorldModel() ) );
	pMag->SetAmmoCount( 0 ); // rounds already returned to reserve
	pMag->SetAbsOrigin( vecSpawnPos );
	pMag->SetAbsAngles( angSpawn );
	pMag->Spawn();

	IPhysicsObject *pPhys = pMag->VPhysicsGetObject();
	if ( pPhys )
	{
		// Prefer the animation-derived velocity (one-frame delta at the
		// authored 30fps) reported by the client; fall back to a small
		// push along the estimated magwell direction.
		Vector vecVel;
		if ( pCmd->vrMagEjectVel != vec3_origin )
			vecVel = pCmd->vrMagEjectVel + pOwner->GetAbsVelocity();
		else
			vecVel = vecEjectDir * tfvr_pistol_mag_eject_speed.GetFloat() + pOwner->GetAbsVelocity();
		pPhys->SetVelocity( &vecVel, NULL );
	}
}
#endif

//-----------------------------------------------------------------------------
// Purpose: Drive the manual reload state machine.
//-----------------------------------------------------------------------------
void CTFPistol::VRPistolMagPostFrame()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
	{
		ResetVRPistolMagState();
		return;
	}

	// Resupply/respawn can refill the clip while the mag is flagged out;
	// a loaded clip means a mag is seated again.
	if ( m_bVRMagOut && m_iVRMagPhase == VR_PISTOL_MAG_PHASE_IDLE && m_iClip1 > 0 )
		m_bVRMagOut = false;

	const float flProgress = GetVRMagPhaseProgress();

	switch ( m_iVRMagPhase )
	{
	case VR_PISTOL_MAG_PHASE_EJECTING:
	{
		const int iReloadWeaponID = GetVRPistolReloadWeaponID();
		const float flMagFreeProgress = VRPistol_FrameMagFree( iReloadWeaponID ) / VRPistol_FramePause( iReloadWeaponID );
		if ( !m_bVRMagOut && flProgress >= flMagFreeProgress )
		{
			m_bVRMagOut = true;
#ifdef GAME_DLL
			if ( !m_bVRMagPhysSpawned )
			{
				m_bVRMagPhysSpawned = true;
				VRSpawnEjectedMagazine();
			}
#endif
		}

		if ( flProgress >= 1.0f )
		{
			// Paused at frame 16, waiting for a fresh mag.
			m_iVRMagPhase = VR_PISTOL_MAG_PHASE_IDLE;
			m_flVRMagPhaseStartTime = 0.0f;
		}
		break;
	}

	case VR_PISTOL_MAG_PHASE_INSERTING:
		if ( flProgress >= 1.0f )
			VRCommitMagInsert();
		break;

	case VR_PISTOL_MAG_PHASE_FINISHING:
		if ( flProgress >= 1.0f && ( m_flVRMagEarliestReadyTime <= 0.0f || gpGlobals->curtime >= m_flVRMagEarliestReadyTime ) )
		{
			m_iVRMagPhase = VR_PISTOL_MAG_PHASE_IDLE;
			m_flVRMagPhaseStartTime = 0.0f;
			m_flVRMagEarliestReadyTime = 0.0f;
		}
		break;

	default:
		break;
	}

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
		return;

	// Drop the held mag if the grip is released (rounds were never deducted).
	if ( m_bVRMagazineHeld && !pCmd->vrMagazineHold )
		m_bVRMagazineHeld = false;

	// Pull a fresh mag from the backpack (allowed at any time).
	if ( pCmd->vrMagazinePull && CanStartVRMagPull() )
	{
		m_bVRMagazineHeld = true;
		PlayVRManualReloadAmmoGrabSound();
	}

	if ( m_iVRMagPhase == VR_PISTOL_MAG_PHASE_IDLE )
	{
		// Manual eject button.
		if ( pCmd->vrMagazineEject && !m_bVRMagOut )
			VRStartMagEject();

		// Mag held to the magwell while the gun is empty.
		if ( m_bVRMagOut && m_bVRMagazineHeld && pCmd->vrMagazineInsert )
			VRStartMagInsert();
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFPistol::ItemPostFrame()
{
	if ( ShouldUseVRPistolManualReload() )
	{
		VRPistolMagPostFrame();
	}
	else if ( IsVRPistolManualReloadBusy() || m_bVRMagOut || m_bVRMagazineHeld )
	{
		ResetVRPistolMagState();
	}

	BaseClass::ItemPostFrame();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFPistol::ItemBusyFrame()
{
	if ( ShouldUseVRPistolManualReload() )
	{
		VRPistolMagPostFrame();
	}

	BaseClass::ItemBusyFrame();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFPistol::PrimaryAttack()
{
	if ( ShouldUseVRPistolManualReload() && ( IsVRPistolManualReloadBusy() || m_bVRMagOut ) )
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

//-----------------------------------------------------------------------------
// Purpose: Dry firing an empty pistol kicks off the manual eject.
//-----------------------------------------------------------------------------
void CTFPistol::HandleFireOnEmpty()
{
	if ( ShouldUseVRPistolManualReload() )
	{
		if ( CanStartVRMagEject() && CanAttack() )
		{
			VRStartMagEject();
			return;
		}

		// Mag already out / mid-reload / nothing to reload with: just click.
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
// Purpose:
//-----------------------------------------------------------------------------
bool CTFPistol::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	const int iOwnerClass = pOwner ? pOwner->GetPlayerClass()->GetClassIndex() : TF_CLASS_UNDEFINED;
	const bool bClassMismatch =
		( GetWeaponID() == TF_WEAPON_PISTOL_SCOUT && iOwnerClass != TF_CLASS_SCOUT )
		|| ( GetWeaponID() == TF_WEAPON_PISTOL && iOwnerClass != TF_CLASS_ENGINEER );

	if ( bClassMismatch )
	{
		// Class changes can briefly leave the old pistol around while the new
		// class is active. Do not carry manual reload state across that swap.
		ResetVRPistolMagState();
	}
	else
	{
		// Keep the mag-out state (the dropped mag is gone), but abandon any
		// in-flight motion and the held spare.
		m_iVRMagPhase = VR_PISTOL_MAG_PHASE_IDLE;
		m_flVRMagPhaseStartTime = 0.0f;
		m_flVRMagEarliestReadyTime = 0.0f;
		m_bVRMagazineHeld = false;
	}

	return BaseClass::Holster( pSwitchingTo );
}

//============================

IMPLEMENT_NETWORKCLASS_ALIASED( TFPistol_Scout, DT_WeaponPistol_Scout )

BEGIN_NETWORK_TABLE( CTFPistol_Scout, DT_WeaponPistol_Scout )
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFPistol_Scout )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_pistol_scout, CTFPistol_Scout );
PRECACHE_WEAPON_REGISTER( tf_weapon_pistol_scout );

//============================

IMPLEMENT_NETWORKCLASS_ALIASED( TFPistol_ScoutPrimary, DT_WeaponPistol_ScoutPrimary )

BEGIN_NETWORK_TABLE( CTFPistol_ScoutPrimary, DT_WeaponPistol_ScoutPrimary )
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFPistol_ScoutPrimary )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_handgun_scout_primary, CTFPistol_ScoutPrimary );
PRECACHE_WEAPON_REGISTER( tf_weapon_handgun_scout_primary );


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CTFPistol_ScoutPrimary::CTFPistol_ScoutPrimary()
{
	m_flPushTime = -1.f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPistol_ScoutPrimary::PlayWeaponShootSound( void )
{
	BaseClass::PlayWeaponShootSound();

	if ( TFGameRules()->GameModeUsesUpgrades() )
	{
		PlayUpgradedShootSound( "Weapon_Upgrade.DamageBonus" );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPistol_ScoutPrimary::SecondaryAttack( void )
{
	CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
	if ( !pOwner )
		return;

	if ( !CanAttack() )
		return;

	if ( m_flNextSecondaryAttack > gpGlobals->curtime )
		return;

	pOwner->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_SECONDARY );
	SendWeaponAnim( ACT_SECONDARY_VM_ALTATTACK );

	m_flNextPrimaryAttack = gpGlobals->curtime + 0.6f;
	m_flNextSecondaryAttack = gpGlobals->curtime + 1.5f;
	m_flPushTime = gpGlobals->curtime + 0.2f;	// Anim delay

	EmitSound( "Weapon_Hands.Push" );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPistol_ScoutPrimary::Push( void )
{
	CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
	if ( !pOwner )
		return;

#ifdef GAME_DLL
	lagcompensation->StartLagCompensation( pOwner, pOwner->GetCurrentCommand() );

	CUtlVector< CTFPlayer* > enemyVector;
	CollectPlayers( &enemyVector, GetEnemyTeam( pOwner->GetTeamNumber() ), COLLECT_ONLY_LIVING_PLAYERS );

	for ( int i = 0; i < enemyVector.Count(); ++i )
	{
		CTFPlayer *pVictim = enemyVector[i];

		if ( !pVictim->IsAlive() )
			continue;

		if ( pVictim == pOwner )
			continue;

		if ( pVictim->InSameTeam( pOwner ) )
			continue;

		if ( TFGameRules() && TFGameRules()->IsTruceActive() && pOwner->IsTruceValidForEnt() )
			continue;

		if ( ( pOwner->GetAbsOrigin()- pVictim->GetAbsOrigin() ).LengthSqr() > ( 128.f * 128.f ) )
			continue;

		if ( !pOwner->FVisible( pVictim, MASK_SOLID ) )
			continue;

		Vector vecEyes = pOwner->EyePosition();
		Vector vecForward;
		AngleVectors( pOwner->EyeAngles(), &vecForward );
		CTraceFilterSimple traceFilter( this, COLLISION_GROUP_NONE );
		const Vector vHull = Vector( 16.f, 16.f, 16.f );
		trace_t trace;

		float flDist = 50.f;
		UTIL_TraceHull( vecEyes, vecEyes + vecForward * flDist,  -vHull, vHull, MASK_SOLID, &traceFilter, &trace );
		
		bool bDebug = false;
		if ( bDebug )
		{
			NDebugOverlay::SweptBox( vecEyes, vecEyes + vecForward * flDist, -vHull, vHull, pOwner->EyeAngles(), 255, 0, 0, 40, 5 );
		}

		if ( trace.m_pEnt && trace.m_pEnt == pVictim && trace.fraction < 1.f )
		{
			Vector vecToVictim = pVictim->GetAbsOrigin() - pOwner->GetAbsOrigin();
			VectorNormalize( vecToVictim );
			pVictim->ApplyGenericPushbackImpulse( vecToVictim * 400.f, pOwner );
			float flDamage = 1.f;
			CTakeDamageInfo info( pVictim, pOwner, this, flDamage, DMG_MELEE | DMG_NEVERGIB | DMG_CLUB, TF_DMG_CUSTOM_NONE );
			CalculateMeleeDamageForce( &info, vecForward, GetAbsOrigin() + vecForward * flDist, 1.f / flDamage * 80.f );
			pVictim->DispatchTraceAttack( info, vecForward, &trace );
			ApplyMultiDamage();

			CPVSFilter filter( vecToVictim );
			EmitSound( "Weapon_Hands.PushImpact" );

			// Make sure we get credit for the push if the target falls to its death
			pVictim->m_AchievementData.AddDamagerToHistory( pOwner );

			break;			
		}
	}

	pOwner->SpeakWeaponFire();
	CTF_GameStats.Event_PlayerFiredWeapon( pOwner, IsCurrentAttackACrit() );

	lagcompensation->FinishLagCompensation( pOwner );
#else
	C_CTF_GameStats.Event_PlayerFiredWeapon( pOwner, IsCurrentAttackACrit() );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  :  - 
//-----------------------------------------------------------------------------
void CTFPistol_ScoutPrimary::ItemPostFrame()
{
	// Check for smack.
	if ( m_flPushTime > -1.f && gpGlobals->curtime > m_flPushTime )
	{
		Push();
		m_flPushTime = -1.f;
	}

	BaseClass::ItemPostFrame();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFPistol_ScoutPrimary::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	m_flPushTime = -1.f;

	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPistol_ScoutPrimary::Precache( void )
{
	PrecacheScriptSound( "Weapon_Hands.Push" );
	PrecacheScriptSound( "Weapon_Hands.PushImpact" );
	
	BaseClass::Precache();
}

//============================

IMPLEMENT_NETWORKCLASS_ALIASED( TFPistol_ScoutSecondary, DT_WeaponPistol_ScoutSecondary )

BEGIN_NETWORK_TABLE( CTFPistol_ScoutSecondary, DT_WeaponPistol_ScoutSecondary )
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFPistol_ScoutSecondary )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_handgun_scout_secondary, CTFPistol_ScoutSecondary );
PRECACHE_WEAPON_REGISTER( tf_weapon_handgun_scout_secondary );

//-----------------------------------------------------------------------------
int	CTFPistol_ScoutSecondary::GetDamageType( void ) const
{
	int iBackheadshot = 0;
	CALL_ATTRIB_HOOK_INT( iBackheadshot, back_headshot );
	if ( iBackheadshot )
	{
		return BaseClass::GetDamageType() | DMG_USE_HITLOCATIONS;	
	}
	return BaseClass::GetDamageType();
}
