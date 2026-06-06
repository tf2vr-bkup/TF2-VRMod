//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================

#include "cbase.h"
#include "tf_weapon_shotgun.h"
#include "decals.h"
#include "tf_fx_shared.h"
#include "takedamageinfo.h"
#include "tf_gamerules.h"
#include "in_buttons.h"

// Client specific.
#if defined( CLIENT_DLL )
#include "c_tf_player.h"
#include "prediction.h"
#include "effect_dispatch_data.h"
#include "c_te_effect_dispatch.h"
#include "tfvr/c_tfvr_hand.h"
// Server specific.
#else
#include "tf_player.h"
#include "ilagcompensationmanager.h"
#include "collisionutils.h"
#endif

#include "usercmd.h"

extern ConVar tfvr_reload_throttle_scale;

//=============================================================================
//
// Weapon Shotgun tables.
//

IMPLEMENT_NETWORKCLASS_ALIASED( TFShotgun, DT_TFShotgun )

BEGIN_NETWORK_TABLE( CTFShotgun, DT_TFShotgun )
#if defined( CLIENT_DLL )
	RecvPropBool( RECVINFO( m_bVRShotgunPumpNeedsPump ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunPumpIsArmed ) ),
	RecvPropVector( RECVINFO( m_vecVRShotgunPumpLastHandPos ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunPumpStrokeOut ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunPumpStrokeIn ) ),
	RecvPropFloat( RECVINFO( m_flVRShotgunPumpStrokeDist ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunSuppressAutoReload ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunPumpStartedDuringReload ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunSuppressNextPumpEject ) ),
	RecvPropFloat( RECVINFO( m_flVRShotgunResumeAutoReloadTime ) ),
	RecvPropFloat( RECVINFO( m_flVRShotgunSuppressReloadSoundUntil ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunShellHeld ) ),
	RecvPropBool( RECVINFO( m_bVRShotgunShellInsertActive ) ),
	RecvPropFloat( RECVINFO( m_flVRShotgunShellInsertStartTime ) ),
	RecvPropFloat( RECVINFO( m_flNextVRShotgunShellStartTime ) ),
#else
	SendPropBool( SENDINFO( m_bVRShotgunPumpNeedsPump ) ),
	SendPropBool( SENDINFO( m_bVRShotgunPumpIsArmed ) ),
	SendPropVector( SENDINFO( m_vecVRShotgunPumpLastHandPos ), -1, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRShotgunPumpStrokeOut ) ),
	SendPropBool( SENDINFO( m_bVRShotgunPumpStrokeIn ) ),
	SendPropFloat( SENDINFO( m_flVRShotgunPumpStrokeDist ), 0, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRShotgunSuppressAutoReload ) ),
	SendPropBool( SENDINFO( m_bVRShotgunPumpStartedDuringReload ) ),
	SendPropBool( SENDINFO( m_bVRShotgunSuppressNextPumpEject ) ),
	SendPropFloat( SENDINFO( m_flVRShotgunResumeAutoReloadTime ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flVRShotgunSuppressReloadSoundUntil ), 0, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRShotgunShellHeld ) ),
	SendPropBool( SENDINFO( m_bVRShotgunShellInsertActive ) ),
	SendPropFloat( SENDINFO( m_flVRShotgunShellInsertStartTime ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flNextVRShotgunShellStartTime ), 0, SPROP_NOSCALE ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFShotgun )
#ifdef CLIENT_DLL
	DEFINE_PRED_FIELD( m_bVRShotgunPumpNeedsPump, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunPumpIsArmed, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_vecVRShotgunPumpLastHandPos, FIELD_VECTOR, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunPumpStrokeOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunPumpStrokeIn, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRShotgunPumpStrokeDist, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunSuppressAutoReload, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunPumpStartedDuringReload, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunSuppressNextPumpEject, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRShotgunResumeAutoReloadTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRShotgunSuppressReloadSoundUntil, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunShellHeld, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRShotgunShellInsertActive, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRShotgunShellInsertStartTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flNextVRShotgunShellStartTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
#endif
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_shotgun_primary, CTFShotgun );
PRECACHE_WEAPON_REGISTER( tf_weapon_shotgun_primary );

CREATE_SIMPLE_WEAPON_TABLE( TFShotgun_Soldier, tf_weapon_shotgun_soldier )
CREATE_SIMPLE_WEAPON_TABLE( TFShotgun_HWG, tf_weapon_shotgun_hwg )
CREATE_SIMPLE_WEAPON_TABLE( TFShotgun_Pyro, tf_weapon_shotgun_pyro )

IMPLEMENT_NETWORKCLASS_ALIASED( TFScatterGun, DT_TFScatterGun )

BEGIN_NETWORK_TABLE( CTFScatterGun, DT_TFScatterGun )
#if defined( CLIENT_DLL )
	RecvPropBool( RECVINFO( m_bVRLeverIsArmed ) ),
	RecvPropVector( RECVINFO( m_vecVRLeverLastHandPos ) ),
	RecvPropBool( RECVINFO( m_bVRLeverStrokeOut ) ),
	RecvPropBool( RECVINFO( m_bVRLeverStrokeIn ) ),
	RecvPropFloat( RECVINFO( m_flVRLeverStrokeDist ) ),
	RecvPropFloat( RECVINFO( m_flNextVRLeverShellReadyTime ) ),
#else
	SendPropBool( SENDINFO( m_bVRLeverIsArmed ) ),
	SendPropVector( SENDINFO( m_vecVRLeverLastHandPos ), -1, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRLeverStrokeOut ) ),
	SendPropBool( SENDINFO( m_bVRLeverStrokeIn ) ),
	SendPropFloat( SENDINFO( m_flVRLeverStrokeDist ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flNextVRLeverShellReadyTime ) ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFScatterGun )
#ifdef CLIENT_DLL
	DEFINE_PRED_FIELD( m_bVRLeverIsArmed, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_vecVRLeverLastHandPos, FIELD_VECTOR, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRLeverStrokeOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRLeverStrokeIn, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRLeverStrokeDist, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flNextVRLeverShellReadyTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
#endif
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_scattergun, CTFScatterGun );
PRECACHE_WEAPON_REGISTER( tf_weapon_scattergun );

CREATE_SIMPLE_WEAPON_TABLE( TFShotgun_Revenge, tf_weapon_sentry_revenge )
CREATE_SIMPLE_WEAPON_TABLE( TFSodaPopper, tf_weapon_soda_popper )
CREATE_SIMPLE_WEAPON_TABLE( TFPEPBrawlerBlaster, tf_weapon_pep_brawler_blaster )
CREATE_SIMPLE_WEAPON_TABLE( TFShotgunBuildingRescue, tf_weapon_shotgun_building_rescue )

#define SCATTERGUN_KNOCKBACK_MIN_DMG		30.0f
#define SCATTERGUN_KNOCKBACK_MIN_RANGE_SQ	160000.0f //400x400

static inline float TFVR_ReloadThrottleScale()
{
	return MAX( 1.0f, tfvr_reload_throttle_scale.GetFloat() );
}

// 0 = vanilla TF2 behavior in VR too (auto-reload when idle + normal singly reload / reload key). 1 = manual lever only.
ConVar tfvr_scattergun_lever_reload( "tfvr_scattergun_lever_reload", "1", FCVAR_ARCHIVE, "VR Scout scattergun: 1 = load shells with weapon-hand pump (requires two-handing + weapon-hand grip); 0 = standard auto/singly reload" );
ConVar tfvr_scattergun_lever_distance( "tfvr_scattergun_lever_distance", "10.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units of weapon-hand motion along lever axis per pump stroke" );
ConVar tfvr_scattergun_lever_sign( "tfvr_scattergun_lever_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply lever-axis motion (+1 or -1) if pump direction feels inverted" );
ConVar tfvr_scattergun_lever_axis( "tfvr_scattergun_lever_axis", "2", FCVAR_REPLICATED, "VR: controller matrix column for pump axis (0=fwd, 1=right, 2=up)" );
ConVar tfvr_scattergun_lever_debug( "tfvr_scattergun_lever_debug", "0", FCVAR_REPLICATED, "VR: 1 = print scattergun lever reload state to console" );

ConVar tfvr_shotgun_pump_action( "tfvr_shotgun_pump_action", "1", FCVAR_ARCHIVE, "VR shotguns: require a physical pump between shots; toggled by the TF2VR auto-reload option" );
ConVar tfvr_shotgun_pump_distance( "tfvr_shotgun_pump_distance", "10.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units of off-hand motion per shotgun pump stroke" );
ConVar tfvr_shotgun_pump_sign( "tfvr_shotgun_pump_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply shotgun pump-axis motion (+1 or -1) if pump direction feels inverted" );
ConVar tfvr_shotgun_pump_axis( "tfvr_shotgun_pump_axis", "0", FCVAR_REPLICATED, "VR: shotgun pump axis source (0=two-hand grip direction, 1=raw weapon hand right, 2=raw weapon hand up)" );
ConVar tfvr_shotgun_pump_reload_restart_delay( "tfvr_shotgun_pump_reload_restart_delay", "0.45", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: seconds before auto-reload resumes after a shotgun pump cancels reload" );
ConVar tfvr_shotgun_pump_debug( "tfvr_shotgun_pump_debug", "0", FCVAR_REPLICATED, "VR: 1 = print shotgun pump state to console" );

//=============================================================================
//
// Weapon Shotgun functions.
//
bool CanScatterGunKnockBack( CTFWeaponBase *pWeapon, float flDamage, float flDistanceSq )
{
	int nBulletKnockBack = 0;
	CALL_ATTRIB_HOOK_INT_ON_OTHER( pWeapon, nBulletKnockBack, set_scattergun_has_knockback );
	if ( nBulletKnockBack != 0 )
	{
		if (flDamage > SCATTERGUN_KNOCKBACK_MIN_DMG && flDistanceSq < SCATTERGUN_KNOCKBACK_MIN_RANGE_SQ )
			return true;

		float flKnockbackMult = 1.0f;
		CALL_ATTRIB_HOOK_FLOAT_ON_OTHER( pWeapon, flKnockbackMult, scattergun_knockback_mult );
		if ( flKnockbackMult > 1.0f )
			return true;
	}

	return false;
}
//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFShotgun::CTFShotgun()
{
	m_bReloadsSingly = true;
	m_bVRShotgunPumpNeedsPump = false;
	m_bVRShotgunPumpIsArmed = false;
	m_vecVRShotgunPumpLastHandPos = vec3_origin;
	m_bVRShotgunPumpStrokeOut = false;
	m_bVRShotgunPumpStrokeIn = false;
	m_flVRShotgunPumpStrokeDist = 0.0f;
	m_bVRShotgunSuppressAutoReload = false;
	m_bVRShotgunPumpStartedDuringReload = false;
	m_bVRShotgunSuppressNextPumpEject = false;
	m_flVRShotgunResumeAutoReloadTime = 0.0f;
	m_flVRShotgunSuppressReloadSoundUntil = 0.0f;
	m_bVRShotgunShellHeld = false;
	m_bVRShotgunShellInsertActive = false;
	m_flVRShotgunShellInsertStartTime = 0.0f;
	m_flNextVRShotgunShellStartTime = 0.0f;
	m_iVRShotgunLastClipForManualReload = -1;

	PrecacheScriptSound( "VR.ShotgunCockBack" );
	PrecacheScriptSound( "VR.ShotgunCockForward" );
	PrecacheScriptSound( "VR.ShotgunReload1" );
	PrecacheScriptSound( "VR.ShotgunReload2" );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::PrimaryAttack()
{
	if ( !CanAttack() )
		return;

	if ( ShouldUseVRShotgunPumpAction() && m_bVRShotgunPumpNeedsPump )
	{
		if ( m_flNextEmptySoundTime < gpGlobals->curtime )
		{
			WeaponSound( EMPTY );
			m_flNextEmptySoundTime = gpGlobals->curtime + 0.5f;
		}
		m_flNextPrimaryAttack = gpGlobals->curtime + 0.1f;
		return;
	}

	// Set the weapon mode.
	m_iWeaponMode = TF_WEAPON_PRIMARY_MODE;

	const int iClipBefore = Clip1();
	const bool bKeepHeldVRShell = m_bVRShotgunShellHeld && !m_bVRShotgunShellInsertActive;
	BaseClass::PrimaryAttack();

	if ( Clip1() < iClipBefore )
	{
		ResetVRShotgunManualReloadState();
		m_bVRShotgunShellHeld = bKeepHeldVRShell;
		m_bVRShotgunSuppressAutoReload = false;
		m_bVRShotgunSuppressNextPumpEject = false;
		m_flVRShotgunResumeAutoReloadTime = 0.0f;
		if ( ShouldUseVRShotgunPumpAction() )
		{
			m_bVRShotgunPumpNeedsPump = true;
			ResetVRShotgunPumpGestureState();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::ItemPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( m_flVRShotgunSuppressReloadSoundUntil > gpGlobals->curtime )
	{
		StopVRShotgunReloadSounds();
	}

	if ( ShouldUseVRShotgunPumpAction() )
	{
		VRShotgunManualReloadPostFrame();
	}
	else
	{
		ResetVRShotgunManualReloadState();
	}

	if ( m_bVRShotgunSuppressAutoReload )
	{
		if ( !pOwner
			|| ( pOwner->m_nButtons & IN_RELOAD )
			|| gpGlobals->curtime >= m_flVRShotgunResumeAutoReloadTime )
		{
			m_bVRShotgunSuppressAutoReload = false;
			m_flVRShotgunResumeAutoReloadTime = 0.0f;
		}
	}

	bool bWasReloading = IsReloading();
	if ( bWasReloading || m_bVRShotgunPumpStartedDuringReload )
	{
		VRShotgunPumpActionPostFrame();
		if ( bWasReloading && !IsReloading() )
			return;
	}

	bWasReloading = IsReloading();
	const int iClipBeforeReloadFrame = Clip1();
	BaseClass::ItemPostFrame();

	if ( ShouldUseVRShotgunPumpAction()
		&& bWasReloading
		&& Clip1() > iClipBeforeReloadFrame
		&& m_bVRShotgunSuppressNextPumpEject
		&& Clip1() > 0 )
	{
		const bool bPumpGestureInProgress = m_bVRShotgunPumpStrokeOut || m_bVRShotgunPumpStrokeIn || m_flVRShotgunPumpStrokeDist > 0.0f;
		m_bVRShotgunPumpNeedsPump = true;
		if ( !bPumpGestureInProgress )
			ResetVRShotgunPumpGestureState();
	}

	if ( ShouldUseVRShotgunPumpAction()
		&& bWasReloading
		&& !IsReloading()
		&& !m_bVRShotgunSuppressAutoReload
		&& Clip1() > 0 )
	{
		const bool bPumpGestureInProgress = m_bVRShotgunPumpStrokeOut || m_bVRShotgunPumpStrokeIn || m_flVRShotgunPumpStrokeDist > 0.0f;
		m_bVRShotgunPumpNeedsPump = true;
		if ( !bPumpGestureInProgress )
			ResetVRShotgunPumpGestureState();
	}

	if ( ShouldUseVRShotgunPumpAction() || IsReloading() )
	{
		VRShotgunPumpActionPostFrame();
	}
	else
	{
		ResetVRShotgunPumpGestureState();
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::ItemBusyFrame( void )
{
	if ( m_flVRShotgunSuppressReloadSoundUntil > gpGlobals->curtime )
	{
		StopVRShotgunReloadSounds();
	}

	if ( ShouldUseVRShotgunPumpAction() )
	{
		VRShotgunManualReloadPostFrame();
	}
	else
	{
		ResetVRShotgunManualReloadState();
	}

	if ( m_bVRShotgunSuppressAutoReload && gpGlobals->curtime >= m_flVRShotgunResumeAutoReloadTime )
	{
		m_bVRShotgunSuppressAutoReload = false;
		m_flVRShotgunResumeAutoReloadTime = 0.0f;
	}

	const bool bWasReloading = IsReloading();
	if ( bWasReloading || m_bVRShotgunPumpStartedDuringReload )
	{
		VRShotgunPumpActionPostFrame();
		if ( bWasReloading && !IsReloading() )
			return;
	}

	BaseClass::ItemBusyFrame();

	if ( !bWasReloading && ( ShouldUseVRShotgunPumpAction() || IsReloading() ) )
	{
		VRShotgunPumpActionPostFrame();
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFShotgun::Reload( void )
{
	if ( ShouldUseVRShotgunPumpAction() )
		return false;

	return BaseClass::Reload();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFShotgun::SendWeaponAnim( int iActivity )
{
	if ( iActivity == ACT_RELOAD_FINISH && ShouldUseVRShotgunPumpAction() )
	{
		// Manual pump mode owns the cocking motion/sounds. Suppress the
		// legacy reload-finish pump so it does not double-play.
		return true;
	}

	if ( iActivity == ACT_VM_PRIMARYATTACK && ShouldUseVRShotgunPumpAction() )
	{
#ifdef CLIENT_DLL
		C_TFVRHand *pHand = NULL;
		C_TFVRHand *pRightHand = GetLocalPlayerRightHand();
		if ( pRightHand && pRightHand->GetHeldWeapon() == this )
		{
			pHand = pRightHand;
		}
		else
		{
			C_TFVRHand *pLeftHand = GetLocalPlayerLeftHand();
			if ( pLeftHand && pLeftHand->GetHeldWeapon() == this )
				pHand = pLeftHand;
		}

		if ( pHand )
			pHand->PlayWeaponFireAnimation();
#endif
		return true;
	}

	return BaseClass::SendWeaponAnim( iActivity );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::WeaponSound( WeaponSound_t sound_type, float soundtime )
{
	if ( sound_type == RELOAD && m_flVRShotgunSuppressReloadSoundUntil > gpGlobals->curtime )
		return;

	BaseClass::WeaponSound( sound_type, soundtime );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFShotgun::ShouldUseVRShotgunPumpAction() const
{
	if ( !IsPumpActionShotgunWeaponID( GetWeaponID() ) )
		return false;

	if ( !tfvr_shotgun_pump_action.GetBool() )
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
bool CTFShotgun::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	if ( ShouldUseVRShotgunPumpAction() )
		return true;

	return m_bVRShotgunSuppressAutoReload && gpGlobals->curtime < m_flVRShotgunResumeAutoReloadTime;
}

bool CTFShotgun::CanVRShotgunPumpCancelReload( void )
{
	return IsReloading() && Clip1() > m_iReloadStartClipAmount;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::ResetVRShotgunPumpGestureState( void )
{
	m_bVRShotgunPumpIsArmed = false;
	m_vecVRShotgunPumpLastHandPos = vec3_origin;
	m_bVRShotgunPumpStrokeOut = false;
	m_bVRShotgunPumpStrokeIn = false;
	m_flVRShotgunPumpStrokeDist = 0.0f;
	m_bVRShotgunPumpStartedDuringReload = false;
}

void CTFShotgun::StopVRShotgunReloadSounds( void )
{
	StopWeaponSound( RELOAD );

#ifdef CLIENT_DLL
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !pOwner->IsLocalPlayer() )
		return;

	CUtlVectorFixedGrowable< int, 3 > soundSources;
	soundSources.AddToTail( pOwner->GetSoundSourceIndex() );

	CBaseEntity *pViewModel = pOwner->GetViewModel( 0 );
	if ( pViewModel )
		soundSources.AddToTail( pViewModel->GetSoundSourceIndex() );

	const char *pszReloadSound = GetShootSound( RELOAD );
	if ( pszReloadSound && pszReloadSound[0] )
	{
		for ( int i = 0; i < soundSources.Count(); ++i )
		{
			CBaseEntity::StopSound( soundSources[i], pszReloadSound );
		}
	}

	// Viewmodel animation events play on the owner sound source, not the
	// weapon entity. Stop the common shotgun reload/cock waves there without
	// touching the VR pump sounds emitted from this weapon entity.
	static const char *const s_pszShotgunReloadWaves[] =
	{
		"weapons/shotgun_reload.wav",
		"weapons/shotgun_cock_back.wav",
		"weapons/shotgun_cock_forward.wav",
	};

	static const int s_iStopChannels[] =
	{
		CHAN_AUTO,
		CHAN_WEAPON,
		CHAN_STATIC,
	};

	for ( int i = 0; i < soundSources.Count(); ++i )
	{
		for ( int iWave = 0; iWave < ARRAYSIZE( s_pszShotgunReloadWaves ); ++iWave )
		{
			for ( int iChannel = 0; iChannel < ARRAYSIZE( s_iStopChannels ); ++iChannel )
			{
				CBaseEntity::StopSound( soundSources[i], s_iStopChannels[iChannel], s_pszShotgunReloadWaves[iWave] );
			}
		}
	}
#endif
}

float CTFShotgun::GetVRShotgunPumpStrokeProgress() const
{
	float dist = tfvr_shotgun_pump_distance.GetFloat();
	return ( dist > 0.0f ) ? clamp( (float)m_flVRShotgunPumpStrokeDist / dist, 0.0f, 1.0f ) : 0.0f;
}

float CTFShotgun::GetVRShotgunShellInsertDuration() const
{
	CTFShotgun *pMutableThis = const_cast< CTFShotgun * >( this );
	return pMutableThis->GetVRSinglyReloadShellThrottleInterval() * TFVR_ReloadThrottleScale() * ( 4.0f / 7.0f );
}

float CTFShotgun::GetVRShotgunShellInsertProgress() const
{
	if ( !m_bVRShotgunShellInsertActive )
		return 0.0f;

	float flDuration = GetVRShotgunShellInsertDuration();
	if ( flDuration <= 0.0f )
		return 1.0f;

	return clamp( ( gpGlobals->curtime - m_flVRShotgunShellInsertStartTime ) / flDuration, 0.0f, 1.0f );
}

bool CTFShotgun::CanStartVRShotgunManualReload()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( Clip1() >= GetMaxClip1() )
		return false;

	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
		return false;

	return true;
}

void CTFShotgun::ResetVRShotgunManualReloadState( void )
{
	m_bVRShotgunShellHeld = false;
	m_bVRShotgunShellInsertActive = false;
	m_flVRShotgunShellInsertStartTime = 0.0f;
}

void CTFShotgun::VRStartShotgunShellInsert( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !m_bVRShotgunShellHeld || m_bVRShotgunShellInsertActive )
		return;

	if ( !CanStartVRShotgunManualReload() )
	{
		ResetVRShotgunManualReloadState();
		return;
	}

	if ( gpGlobals->curtime < m_flNextVRShotgunShellStartTime )
		return;

	float flAnimDuration = GetVRShotgunShellInsertDuration();
	float flThrottleDuration = GetVRSinglyReloadShellThrottleInterval() * TFVR_ReloadThrottleScale();
	float flAnimFinishTime = gpGlobals->curtime + flAnimDuration;
	float flThrottleReadyTime = gpGlobals->curtime + flThrottleDuration;

	m_bVRShotgunShellInsertActive = true;
	m_flVRShotgunShellInsertStartTime = gpGlobals->curtime;
	m_flNextVRShotgunShellStartTime = flThrottleReadyTime;

#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.ShotgunReload2" );
#endif

	pOwner->m_flNextAttack = Max<float>( pOwner->m_flNextAttack, flThrottleReadyTime );
	m_flNextPrimaryAttack = Max<float>( m_flNextPrimaryAttack, flThrottleReadyTime );
	SetWeaponIdleTime( flAnimFinishTime );
}

void CTFShotgun::VRCommitShotgunShell( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !m_bVRShotgunShellInsertActive )
		return;

	if ( Clip1() >= GetMaxClip1() || pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 || CheckReloadMisfire() )
	{
		ResetVRShotgunManualReloadState();
		return;
	}

	m_iClip1++;
	pOwner->RemoveAmmo( 1, m_iPrimaryAmmoType );
	m_flNextVRShotgunShellStartTime = Max<float>( m_flNextVRShotgunShellStartTime, gpGlobals->curtime );
	m_bVRShotgunSuppressAutoReload = true;
	m_flVRShotgunResumeAutoReloadTime = Max<float>( m_flNextVRShotgunShellStartTime, gpGlobals->curtime + 0.1f );
	ResetVRShotgunPumpGestureState();
	m_bVRShotgunPumpNeedsPump = true;
	m_bVRShotgunSuppressNextPumpEject = true;

	ResetVRShotgunManualReloadState();
}

void CTFShotgun::VRShotgunManualReloadPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
	{
		ResetVRShotgunManualReloadState();
		return;
	}

	const int nMaxClip = GetMaxClip1();
	const int nClip = Clip1();

	if ( m_bVRShotgunShellInsertActive )
	{
		if ( gpGlobals->curtime - m_flVRShotgunShellInsertStartTime >= GetVRShotgunShellInsertDuration() )
		{
			VRCommitShotgunShell();
		}
		return;
	}

	if ( nClip >= nMaxClip || pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		ResetVRShotgunManualReloadState();
		m_flNextVRShotgunShellStartTime = 0.0f;
		m_iVRShotgunLastClipForManualReload = nClip;
		return;
	}

	if ( m_iVRShotgunLastClipForManualReload >= 0 )
	{
		if ( ( nClip == 0 && m_iVRShotgunLastClipForManualReload > 0 ) ||
			( nMaxClip > 1 && m_iVRShotgunLastClipForManualReload == nMaxClip && nClip == nMaxClip - 1 ) )
		{
			float flDelay = GetVRSinglyReloadStartThrottleInterval() * TFVR_ReloadThrottleScale();
			m_flNextVRShotgunShellStartTime = MAX( m_flNextVRShotgunShellStartTime, gpGlobals->curtime + flDelay );
		}
	}
	m_iVRShotgunLastClipForManualReload = nClip;

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
		return;

	if ( m_bVRShotgunShellHeld && !pCmd->vrShotgunShellHold )
	{
		ResetVRShotgunManualReloadState();
		return;
	}

	if ( m_bVRShotgunShellHeld )
	{
		if ( pCmd->vrShotgunShellInsert )
			VRStartShotgunShellInsert();
		return;
	}

	if ( pCmd->vrShotgunShellPull && CanStartVRShotgunManualReload() )
	{
		m_bVRShotgunShellHeld = true;
#ifdef CLIENT_DLL
		if ( prediction->IsFirstTimePredicted() )
			EmitSound( "VR.ShotgunReload1" );
#endif
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::VRCommitShotgunPumpAction( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

	const CUserCmd *pCmdCommit = pOwner->GetCurrentUserCommand();
	if ( !pCmdCommit )
		return;

	const bool bPumpSatisfiesPendingShot = m_bVRShotgunPumpNeedsPump;
	const bool bHadSuppressNextPumpEject = m_bVRShotgunSuppressNextPumpEject;
	const bool bSuppressEjectAfterFutureReload = bPumpSatisfiesPendingShot && !bHadSuppressNextPumpEject && Clip1() < GetMaxClip1();
	const bool bCanCancelReload = ( IsReloading() && bPumpSatisfiesPendingShot )
		|| ( CanVRShotgunPumpCancelReload() && !bPumpSatisfiesPendingShot )
		|| m_bVRShotgunPumpStartedDuringReload;
	if ( !pCmdCommit->vrWeaponArmed && !bCanCancelReload && !bPumpSatisfiesPendingShot )
		return;

	const bool bPumpEndedReload = bCanCancelReload;
	if ( IsReloading() )
	{
		if ( !bCanCancelReload )
			return;

		AbortReload();
		SendWeaponAnim( ACT_VM_IDLE );
		m_flVRShotgunSuppressReloadSoundUntil = gpGlobals->curtime + 1.5f;
		StopVRShotgunReloadSounds();
		pOwner->m_flNextAttack = gpGlobals->curtime;
		m_flNextPrimaryAttack = Max<float>( gpGlobals->curtime, m_flReloadPriorNextFire );
		SetWeaponIdleTime( gpGlobals->curtime + m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flTimeIdle );
		m_bVRShotgunSuppressAutoReload = true;
		m_flVRShotgunResumeAutoReloadTime = gpGlobals->curtime + Max<float>( tfvr_shotgun_pump_reload_restart_delay.GetFloat(), 0.0f );
	}
	else if ( bPumpEndedReload )
	{
		SendWeaponAnim( ACT_VM_IDLE );
		m_flVRShotgunSuppressReloadSoundUntil = gpGlobals->curtime + 1.5f;
		StopVRShotgunReloadSounds();
	}

	m_bVRShotgunPumpNeedsPump = false;
	if ( bPumpSatisfiesPendingShot )
	{
		m_bVRShotgunSuppressNextPumpEject = bSuppressEjectAfterFutureReload;
	}
	m_bVRShotgunPumpStartedDuringReload = false;
}

//-----------------------------------------------------------------------------
// Purpose: Two-stroke off-hand pump action along the weapon axis.
//-----------------------------------------------------------------------------
void CTFShotgun::VRShotgunPumpActionPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

#if defined( CLIENT_DLL )
	if ( !pOwner->IsLocalPlayer() )
		return;
#endif

	const bool bCanPumpToEndReload = ( !m_bVRShotgunPumpNeedsPump && CanVRShotgunPumpCancelReload() ) || m_bVRShotgunPumpStartedDuringReload;
	if ( IsReloading() && !m_bVRShotgunPumpNeedsPump && !bCanPumpToEndReload )
	{
		if ( tfvr_shotgun_pump_debug.GetBool() && m_vecVRShotgunPumpLastHandPos != vec3_origin )
			DevMsg( "[VR ShotgunPump] Reset: reload has not inserted a shell yet\n" );
		ResetVRShotgunPumpGestureState();
		return;
	}

	if ( !m_bVRShotgunPumpNeedsPump && !bCanPumpToEndReload )
	{
		ResetVRShotgunPumpGestureState();
		return;
	}

	const bool bDebug = tfvr_shotgun_pump_debug.GetBool();

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
	{
		ResetVRShotgunPumpGestureState();
		return;
	}

	Vector vecPumpHandRaw = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerPosL
		: pCmd->vrRawControllerPosR;
	Vector vecWeaponHandRaw = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerPosR
		: pCmd->vrRawControllerPosL;

	if ( vecPumpHandRaw == vec3_origin || vecWeaponHandRaw == vec3_origin )
		return;

	Vector vecHandRelative = vecPumpHandRaw - vecWeaponHandRaw;

	if ( !pCmd->vrWeaponArmed && !bCanPumpToEndReload )
	{
		const bool bMidStroke = m_bVRShotgunPumpStrokeOut || m_bVRShotgunPumpStrokeIn;
		if ( !bMidStroke )
		{
			if ( bDebug && m_vecVRShotgunPumpLastHandPos != vec3_origin )
				DevMsg( "[VR ShotgunPump] Reset: not armed\n" );
			ResetVRShotgunPumpGestureState();
		}
		else
		{
			m_vecVRShotgunPumpLastHandPos = vecHandRelative;
			m_flVRShotgunPumpStrokeDist = 0.0f;
			if ( bDebug )
				DevMsg( "[VR ShotgunPump] Grip lost mid-stroke, holding state\n" );
		}
		return;
	}

	m_bVRShotgunPumpIsArmed = true;

	const float flPumpDist = tfvr_shotgun_pump_distance.GetFloat();
	const float flSign = tfvr_shotgun_pump_sign.GetFloat();
	const float flPumpInterval = MAX( m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flTimeFireDelay * 0.5f, 0.05f );

	matrix3x4_t controllerMatrix;
	QAngle angWeaponHand = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerAngR
		: pCmd->vrRawControllerAngL;
	AngleMatrix( angWeaponHand, controllerMatrix );

	int iAxisCol = clamp( tfvr_shotgun_pump_axis.GetInt(), 0, 2 );
	Vector vecPumpAxis;
	if ( iAxisCol == 0 )
	{
		// The shotgun's effective two-hand-corrected forward is the line from
		// the weapon hand to the pump hand. Both points are raw playspace values,
		// matching the displacement space below without extending CUserCmd.
		vecPumpAxis = vecHandRelative;
	}
	else
	{
		MatrixGetColumn( controllerMatrix, iAxisCol, vecPumpAxis );
	}
	if ( vecPumpAxis.IsZero() )
	{
		MatrixGetColumn( controllerMatrix, 0, vecPumpAxis );
	}
	VectorNormalize( vecPumpAxis );

	if ( m_vecVRShotgunPumpLastHandPos == vec3_origin )
	{
		m_vecVRShotgunPumpLastHandPos = vecHandRelative;
		m_flVRShotgunPumpStrokeDist = 0.0f;
		if ( bDebug )
			DevMsg( "[VR ShotgunPump] Tracking started\n" );
		return;
	}

	if ( !m_bVRShotgunPumpStrokeOut && !m_bVRShotgunPumpStrokeIn )
	{
		Vector vecFrameDelta = vecHandRelative - m_vecVRShotgunPumpLastHandPos;
		float flFrameDisp = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

		if ( flFrameDisp < 0.0f )
		{
			m_flVRShotgunPumpStrokeDist += -flFrameDisp;
		}
		else
		{
			m_flVRShotgunPumpStrokeDist = MAX( m_flVRShotgunPumpStrokeDist - flFrameDisp * 2.0f, 0.0f );
		}

		if ( m_flVRShotgunPumpStrokeDist >= 0.5f )
		{
			m_bVRShotgunPumpStrokeOut = true;
			if ( !m_bVRShotgunPumpNeedsPump && CanVRShotgunPumpCancelReload() )
				m_bVRShotgunPumpStartedDuringReload = true;
			m_vecVRShotgunPumpLastHandPos = vecHandRelative;
			m_flVRShotgunPumpStrokeDist = MIN( m_flVRShotgunPumpStrokeDist - 0.5f, flPumpDist );
			if ( bDebug )
				DevMsg( "[VR ShotgunPump] Pullback started\n" );
		}
		else
		{
			m_vecVRShotgunPumpLastHandPos = vecHandRelative;
		}
	}
	else
	{
		Vector vecFrameDelta = vecHandRelative - m_vecVRShotgunPumpLastHandPos;
		float flFrameDisp = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

		if ( m_bVRShotgunPumpStrokeOut )
		{
			if ( flFrameDisp < 0.0f )
			{
				m_flVRShotgunPumpStrokeDist += -flFrameDisp;
			}

			float flMaxPullPerFrame = ( flPumpDist / flPumpInterval ) * gpGlobals->frametime;
			if ( m_flVRShotgunPumpStrokeDist >= flPumpDist * 0.60f )
			{
				m_flVRShotgunPumpStrokeDist = MIN( m_flVRShotgunPumpStrokeDist + flMaxPullPerFrame, flPumpDist );
			}
			m_vecVRShotgunPumpLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR ShotgunPump] Pullback: %.2f / %.2f\n", (float)m_flVRShotgunPumpStrokeDist, flPumpDist );

			if ( m_flVRShotgunPumpStrokeDist >= flPumpDist )
			{
				m_bVRShotgunPumpStrokeOut = false;
				m_bVRShotgunPumpStrokeIn = true;
				m_flVRShotgunPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR ShotgunPump] Pullback complete, push forward\n" );

#ifdef CLIENT_DLL
				if ( prediction->IsFirstTimePredicted() )
				{
					EmitSound( "VR.ShotgunCockBack" );

					if ( !bCanPumpToEndReload && !m_bVRShotgunSuppressNextPumpEject && ShouldEjectBrass() )
					{
						CEffectData data;
						bool bGotAttachment = false;

						C_TFVRHand *pHand = pCmd->vrWeaponHandIsRight
							? GetLocalPlayerRightHand()
							: GetLocalPlayerLeftHand();
						if ( pHand )
						{
							C_BaseAnimating *pRenderWeapon = pHand->GetRenderWeapon();
							if ( pRenderWeapon )
							{
								int iEjectAttach = pRenderWeapon->LookupAttachment( "eject_brass" );
								if ( iEjectAttach > 0 )
								{
									pRenderWeapon->GetAttachment( iEjectAttach, data.m_vOrigin, data.m_vAngles );
									bGotAttachment = true;
								}
							}
						}

						if ( bGotAttachment )
						{
							data.m_nDamageType = GetAttributeContainer()->GetItem()
								? GetAttributeContainer()->GetItem()->GetItemDefIndex() : 0;
							data.m_nHitBox = GetWeaponID();
							DispatchEffect( "TF_EjectBrass", data );
						}
					}
				}
#endif
			}
		}
		else if ( m_bVRShotgunPumpStrokeIn )
		{
			float flCompletionDist = flPumpDist * 0.9f;
			float flMaxDistPerFrame = ( flCompletionDist / flPumpInterval ) * gpGlobals->frametime;

			float flPrevDist = m_flVRShotgunPumpStrokeDist;
			if ( flFrameDisp > 0.0f )
			{
				m_flVRShotgunPumpStrokeDist = MIN( m_flVRShotgunPumpStrokeDist + flFrameDisp, flCompletionDist );
			}
			if ( m_flVRShotgunPumpStrokeDist >= flPumpDist * 0.60f )
			{
				m_flVRShotgunPumpStrokeDist = MIN( m_flVRShotgunPumpStrokeDist + flMaxDistPerFrame, flCompletionDist );
			}
			m_vecVRShotgunPumpLastHandPos = vecHandRelative;

#ifdef CLIENT_DLL
			float flSoundThreshold = flCompletionDist * 0.15f;
			if ( prediction->IsFirstTimePredicted()
				&& flPrevDist < flSoundThreshold
				&& m_flVRShotgunPumpStrokeDist >= flSoundThreshold )
			{
				EmitSound( "VR.ShotgunCockForward" );
			}
#endif

			if ( bDebug )
				DevMsg( "[VR ShotgunPump] Push: %.2f / %.2f\n", (float)m_flVRShotgunPumpStrokeDist, flCompletionDist );

			if ( m_flVRShotgunPumpStrokeDist >= flCompletionDist )
			{
				VRCommitShotgunPumpAction();
				ResetVRShotgunPumpGestureState();
				if ( bDebug )
					DevMsg( "[VR ShotgunPump] Pump complete, ready to fire\n" );
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFShotgun::UpdatePunchAngles( CTFPlayer *pPlayer )
{
	// Update the player's punch angle.
	QAngle angle = pPlayer->GetPunchAngle();
	float flPunchAngle = m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flPunchAngle;
	angle.x -= SharedRandomInt( "ShotgunPunchAngle", ( flPunchAngle - 1 ), ( flPunchAngle + 1 ) );
	pPlayer->SetPunchAngle( angle );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFShotgun::PlayWeaponShootSound( void )
{
	BaseClass::PlayWeaponShootSound();

	if ( TFGameRules()->GameModeUsesUpgrades() )
	{
		PlayUpgradedShootSound( "Weapon_Upgrade.DamageBonus" );
	}
}

//-----------------------------------------------------------------------------
// CTFShotgun_Revenge
//-----------------------------------------------------------------------------
CTFShotgun_Revenge::CTFShotgun_Revenge()
{
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun_Revenge::Precache()
{
	int iModelIndex = PrecacheModel( TF_WEAPON_TAUNT_FRONTIER_JUSTICE_GUITAR_MODEL );
	PrecacheGibsForModel( iModelIndex );
	PrecacheParticleSystem( "blood_impact_backscatter" );

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFShotgun_Revenge::PrimaryAttack()
{
	if ( !CanAttack() )
		return;

	const int iClipBefore = Clip1();
	BaseClass::PrimaryAttack();
	if ( Clip1() >= iClipBefore )
		return;

	// Do this after the attack, so that we know if we are doing custom damage
	CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
	if ( pOwner )
	{
		int iRevengeCrits = pOwner->m_Shared.GetRevengeCrits();
		pOwner->m_Shared.SetRevengeCrits( iRevengeCrits-1 );
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun_Revenge::SentryKilled( int iCrits )
{
	int val = 0;
	CALL_ATTRIB_HOOK_INT( val, sentry_killed_revenge );
	if ( val == 1 )
	{
		CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
		if ( pOwner )
		{
			pOwner->m_Shared.SetRevengeCrits( pOwner->m_Shared.GetRevengeCrits() + iCrits );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFShotgun_Revenge::Holster( CBaseCombatWeapon *pSwitchingTo )
{
#ifdef GAME_DLL
	CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
	if ( pOwner && pOwner->m_Shared.GetRevengeCrits() )
	{
		pOwner->m_Shared.RemoveCond( TF_COND_CRITBOOSTED );
	}
#endif

	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFShotgun_Revenge::Deploy( void )
{
#ifdef GAME_DLL
	CTFPlayer *pOwner = ToTFPlayer( GetOwner() );
	if ( pOwner && pOwner->m_Shared.GetRevengeCrits() )
	{
		pOwner->m_Shared.AddCond( TF_COND_CRITBOOSTED );
	}
#endif

	return BaseClass::Deploy();
}												

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
int CTFShotgun_Revenge::GetCustomDamageType() const
{
	CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
	if ( pOwner )
	{
		int iRevengeCrits = pOwner->m_Shared.GetRevengeCrits();
		return iRevengeCrits > 0 ? TF_DMG_CUSTOM_SHOTGUN_REVENGE_CRIT : TF_DMG_CUSTOM_NONE;
	}
	return TF_DMG_CUSTOM_NONE;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
int CTFShotgun_Revenge::GetCount( void )
{
	CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
	if ( pOwner )
	{
		return pOwner->m_Shared.GetRevengeCrits();
	}

	return 0;
}

#ifdef CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose:
// ----------------------------------------------------------------------------
void CTFShotgun_Revenge::SetWeaponVisible( bool visible )
{
	if ( !visible )
	{
		CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
		if ( pPlayer && pPlayer->m_Shared.InCond( TF_COND_TAUNTING ) && pPlayer->GetPlayerClass()->GetClassIndex() == TF_CLASS_ENGINEER && pPlayer->m_Shared.GetTauntIndex() == TAUNT_BASE_WEAPON )
		{
			int nModelIndex = modelinfo->GetModelIndex( TF_WEAPON_TAUNT_FRONTIER_JUSTICE_GUITAR_MODEL );
			CUtlVector<breakmodel_t> guitarGibs;
			BuildGibList( guitarGibs, nModelIndex, 1.0f, COLLISION_GROUP_NONE );
			if ( guitarGibs.Count() > 0 )
			{
				Vector vForward, vRight, vUp;
				AngleVectors( GetAbsAngles(), &vForward, &vRight, &vUp );

				Vector vecBreakVelocity = Vector(0,0,200);
				AngularImpulse angularImpulse( RandomFloat( 0.0f, 120.0f ), RandomFloat( 0.0f, 120.0f ), 0.0 );
				Vector vecOrigin = GetAbsOrigin() + vForward*70 + vUp*10;
				QAngle vecAngle = GetAbsAngles();
				breakablepropparams_t breakParams( vecOrigin, vecAngle, vecBreakVelocity, angularImpulse );
				breakParams.impactEnergyScale = 1.0f;

				CreateGibsFromList( guitarGibs, nModelIndex, NULL, breakParams, NULL, -1 , false, true );
			}
		}
	}

	BaseClass::SetWeaponVisible( visible );
}

//-----------------------------------------------------------------------------
// Purpose:
// ----------------------------------------------------------------------------
int CTFShotgun_Revenge::GetWorldModelIndex( void )
{
	// Engineer guitar support.
	CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
	if ( pPlayer && pPlayer->GetPlayerClass() && ( pPlayer->GetPlayerClass()->GetClassIndex() == TF_CLASS_ENGINEER ) && 
			( pPlayer->m_Shared.InCond( TF_COND_TAUNTING ) ) && ( pPlayer->m_Shared.GetTauntIndex() == TAUNT_BASE_WEAPON ) )
	{
		// While we are taunting, replace our normal world model with the guitar.
		m_iWorldModelIndex = modelinfo->GetModelIndex( TF_WEAPON_TAUNT_FRONTIER_JUSTICE_GUITAR_MODEL );
		return m_iWorldModelIndex;
	}

	return BaseClass::GetWorldModelIndex();
}
#endif

#ifdef GAME_DLL
//-----------------------------------------------------------------------------
// Purpose: Reset revenge crits when the shotgun is changed
//-----------------------------------------------------------------------------
void CTFShotgun_Revenge::Detach( void )
{
	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( pPlayer )
	{
		pPlayer->m_Shared.SetRevengeCrits( 0 );
		pPlayer->m_Shared.RemoveCond( TF_COND_CRITBOOSTED );
	}

	BaseClass::Detach();
}
#endif

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFScatterGun::CTFScatterGun()
{
	m_bVRLeverIsArmed = false;
	m_vecVRLeverLastHandPos = vec3_origin;
	m_bVRLeverStrokeOut = false;
	m_bVRLeverStrokeIn = false;
	m_flVRLeverStrokeDist = 0.0f;

	PrecacheScriptSound( "VR.ScattergunPumpDown" );
	PrecacheScriptSound( "VR.ScattergunPumpUp" );
	m_flNextVRLeverShellReadyTime = 0.0f;
	m_iVRLeverLastClipForThrottle = -1;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFScatterGun::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	if ( !IsScattergunWeaponID( GetWeaponID() ) )
		return false;

	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !pOwner->IsInVRMode() )
		return false;

#ifdef CLIENT_DLL
	if ( !tfvr_scattergun_lever_reload.GetBool() )
		return false;
	return IsHeldByVRHand();
#else
	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd || !pCmd->vrManualPumpReload )
		return false;
	return true;
#endif
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFScatterGun::Deploy( void )
{
	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
	{
		AbortReload();
		CTFPlayer *pOwner = GetTFPlayerOwner();
		if ( pOwner && Clip1() == 0 && pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0 )
		{
			const float flDelay = ( GetVRSinglyReloadStartThrottleInterval() + GetVRSinglyReloadShellThrottleInterval() ) * TFVR_ReloadThrottleScale();
			m_flNextVRLeverShellReadyTime = gpGlobals->curtime + flDelay;
		}
		else
		{
			m_flNextVRLeverShellReadyTime = 0.0f;
		}
		m_iVRLeverLastClipForThrottle = Clip1();
	}
	return BaseClass::Deploy();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFScatterGun::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	ResetVRLeverGestureState();
	m_flNextVRLeverShellReadyTime = 0.0f;
	m_iVRLeverLastClipForThrottle = -1;
	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFScatterGun::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();
	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
	{
		CTFPlayer *pOwner = GetTFPlayerOwner();
#if defined( CLIENT_DLL )
		if ( pOwner && pOwner->IsLocalPlayer() )
#endif
		{
			if ( pOwner && pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0 )
			{
				const int iClip = Clip1();

				if ( m_iVRLeverLastClipForThrottle >= 0 )
				{
					const int nMaxClip = GetMaxClip1();
					// Match ReloadSingly: first shell in a sequence costs reload_start + per-shell time (tube went empty,
					// or first shot from a full clip begins a new reload session).
					if ( ( iClip == 0 && m_iVRLeverLastClipForThrottle > 0 ) ||
						( nMaxClip > 1 && m_iVRLeverLastClipForThrottle == nMaxClip && iClip == nMaxClip - 1 ) )
					{
						const float flDelay = ( GetVRSinglyReloadStartThrottleInterval() + GetVRSinglyReloadShellThrottleInterval() ) * TFVR_ReloadThrottleScale();
						m_flNextVRLeverShellReadyTime = MAX( m_flNextVRLeverShellReadyTime, gpGlobals->curtime + flDelay );
					}
					else if ( iClip == nMaxClip )
					{
						m_flNextVRLeverShellReadyTime = 0.0f;
					}
				}
				m_iVRLeverLastClipForThrottle = iClip;
			}
		}

		VRLeverReloadPostFrame();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Off-hand lever motion uses left controller world position from usercmd
//          (same data the server stores from VR input).
//-----------------------------------------------------------------------------
void CTFScatterGun::ResetVRLeverGestureState( void )
{
	m_bVRLeverIsArmed = false;
	m_vecVRLeverLastHandPos = vec3_origin;
	m_bVRLeverStrokeOut = false;
	m_bVRLeverStrokeIn = false;
	m_flVRLeverStrokeDist = 0.0f;
}

float CTFScatterGun::GetVRLeverStrokeProgress() const
{
	float dist = tfvr_scattergun_lever_distance.GetFloat();
	return ( dist > 0.0f ) ? clamp( (float)m_flVRLeverStrokeDist / dist, 0.0f, 1.0f ) : 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFScatterGun::VRCommitLeverShell( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

	const CUserCmd *pCmdCommit = pOwner->GetCurrentUserCommand();
	if ( !pCmdCommit || !pCmdCommit->vrWeaponArmed )
		return;

	if ( gpGlobals->curtime < m_flNextVRLeverShellReadyTime )
		return;

	if ( Clip1() >= GetMaxClip1() )
		return;
	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
		return;
	if ( CheckReloadMisfire() )
		return;

	m_iClip1++;
	pOwner->RemoveAmmo( 1, m_iPrimaryAmmoType );
	m_flNextVRLeverShellReadyTime = gpGlobals->curtime + GetVRSinglyReloadShellThrottleInterval() * TFVR_ReloadThrottleScale();

#ifdef CLIENT_DLL
	if ( ShouldPlayClientReloadSound() )
		WeaponSound( RELOAD );
#else
	WeaponSound( RELOAD );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Two-stroke gesture along weapon "up" (open, then close) loads one shell.
//-----------------------------------------------------------------------------
void CTFScatterGun::VRLeverReloadPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

#if defined( CLIENT_DLL )
	if ( !pOwner->IsLocalPlayer() )
		return;
#endif

	const bool bDebug = tfvr_scattergun_lever_debug.GetBool();

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
	{
		ResetVRLeverGestureState();
		return;
	}

	// Use the weapon-hand position RELATIVE to the off-hand.  Both controllers
	// share the same global scaling / reference-space offset, so the difference
	// is purely physical hand motion — completely immune to locomotion, world-
	// scale changes, and reference-space shifts.
	Vector vecWeaponHandRaw = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerPosR
		: pCmd->vrRawControllerPosL;
	Vector vecOffHandRaw = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerPosL
		: pCmd->vrRawControllerPosR;

	if ( vecWeaponHandRaw == vec3_origin || vecOffHandRaw == vec3_origin )
		return;

	Vector vecHandRelative = vecWeaponHandRaw - vecOffHandRaw;

	if ( Clip1() >= GetMaxClip1() || pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		if ( bDebug && ( m_bVRLeverStrokeOut || m_bVRLeverStrokeIn ) )
			DevMsg( "[VR Lever] Reset: clip full or no reserve ammo\n" );
		ResetVRLeverGestureState();
		return;
	}

	// Block pumping while the weapon's fire cycle is still active.
	// Reset all reload state so the lever visually returns to idle.
	if ( gpGlobals->curtime < m_flNextPrimaryAttack )
	{
		m_bVRLeverIsArmed = false;
		m_bVRLeverStrokeOut = false;
		m_bVRLeverStrokeIn = false;
		m_flVRLeverStrokeDist = 0.0f;
		if ( m_vecVRLeverLastHandPos != vec3_origin )
			m_vecVRLeverLastHandPos = vecHandRelative;
		if ( bDebug )
			DevMsg( "[VR Lever] Paused: fire cooldown\n" );
		return;
	}

	if ( !pCmd->vrWeaponArmed )
	{
		const bool bMidStroke = m_bVRLeverStrokeOut || m_bVRLeverStrokeIn;
		if ( !bMidStroke )
		{
			if ( bDebug && m_vecVRLeverLastHandPos != vec3_origin )
				DevMsg( "[VR Lever] Reset: lever not armed\n" );
			ResetVRLeverGestureState();
		}
		else
		{
			// Mid-stroke: keep stroke state but slide the reference so
			// there's no displacement jump when grip returns.
			m_vecVRLeverLastHandPos = vecHandRelative;
			m_flVRLeverStrokeDist = 0.0f;
			if ( bDebug )
				DevMsg( "[VR Lever] Grip lost mid-stroke, holding state\n" );
		}
		return;
	}

	m_bVRLeverIsArmed = true;

	const float flLeverDist    = tfvr_scattergun_lever_distance.GetFloat();
	const float flSign         = tfvr_scattergun_lever_sign.GetFloat();
	const float flReloadInterval = GetVRSinglyReloadShellThrottleInterval() * TFVR_ReloadThrottleScale();

	// Pump axis from the weapon-hand controller's raw orientation.
	// Column 2 = controller "up" in Source coords — use tfvr_scattergun_lever_axis
	// to try other columns if the mapping feels wrong.
	QAngle angWeaponHand = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerAngR
		: pCmd->vrRawControllerAngL;

	matrix3x4_t controllerMatrix;
	AngleMatrix( angWeaponHand, controllerMatrix );

	int iAxisCol = clamp( tfvr_scattergun_lever_axis.GetInt(), 0, 2 );
	Vector vecPumpAxis;
	MatrixGetColumn( controllerMatrix, iAxisCol, vecPumpAxis );
	VectorNormalize( vecPumpAxis );

	// First frame: seed the reference position.
	if ( m_vecVRLeverLastHandPos == vec3_origin )
	{
		m_vecVRLeverLastHandPos = vecHandRelative;
		m_flVRLeverStrokeDist   = 0.0f;
		if ( bDebug )
			DevMsg( "[VR Lever] Tracking started\n" );
		return;
	}

	// State machine: Neutral -> PumpDown -> PumpReturn -> commit -> Neutral
	if ( !m_bVRLeverStrokeOut && !m_bVRLeverStrokeIn )
	{
		// In neutral, measure per-tick displacement and accumulate downward
		// motion.  The reference is always updated to current so the pump
		// starts from wherever the hand IS, not where it WAS.
		if ( m_vecVRLeverLastHandPos != vec3_origin )
		{
			Vector vecFrameDelta = vecHandRelative - m_vecVRLeverLastHandPos;
			float  flFrameDisp   = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

			if ( flFrameDisp < 0.0f )
			{
				m_flVRLeverStrokeDist += -flFrameDisp;
			}
			else
			{
				m_flVRLeverStrokeDist = MAX( m_flVRLeverStrokeDist - flFrameDisp * 2.0f, 0.0f );
			}

			if ( m_flVRLeverStrokeDist >= 0.5f )
			{
				m_bVRLeverStrokeOut = true;
				m_vecVRLeverLastHandPos = vecHandRelative;
				m_flVRLeverStrokeDist = MIN( m_flVRLeverStrokeDist - 0.5f, flLeverDist );
				if ( bDebug )
					DevMsg( "[VR Lever] Pump down started\n" );
			}
			else
			{
				// Stay in neutral — keep reference current
				m_vecVRLeverLastHandPos = vecHandRelative;
			}
		}
		else
		{
			// First-frame seed
			m_vecVRLeverLastHandPos = vecHandRelative;
			m_flVRLeverStrokeDist = 0.0f;
		}
	}
	else
	{
		// Per-frame accumulation for both strokes, matching the neutral
		// state pattern.  Reference is always updated so overshooting
		// the end of a stroke never penalises the return.
		Vector vecFrameDelta = vecHandRelative - m_vecVRLeverLastHandPos;
		float  flFrameDisp   = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

		if ( m_bVRLeverStrokeOut )
		{
			if ( flFrameDisp < 0.0f )
			{
				m_flVRLeverStrokeDist += -flFrameDisp;
			}

			float flMinPullTime = MAX( flReloadInterval * 0.5f, 0.05f );
			float flMaxPullPerFrame = ( flLeverDist / flMinPullTime ) * gpGlobals->frametime;
			if ( m_flVRLeverStrokeDist >= flLeverDist * 0.60f )
			{
				m_flVRLeverStrokeDist = MIN( m_flVRLeverStrokeDist + flMaxPullPerFrame, flLeverDist );
			}
			m_vecVRLeverLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR Lever] Down: %.2f / %.2f\n", (float)m_flVRLeverStrokeDist, flLeverDist );

			if ( m_flVRLeverStrokeDist >= flLeverDist )
			{
				m_bVRLeverStrokeOut   = false;
				m_bVRLeverStrokeIn    = true;
				m_flVRLeverStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR Lever] Bottom reached, pump back up to load\n" );

#ifdef CLIENT_DLL
				if ( prediction->IsFirstTimePredicted() )
				{
					EmitSound( "VR.ScattergunPumpDown" );

					if ( ShouldEjectBrass() )
					{
						CEffectData data;
						bool bGotAttachment = false;

						C_TFVRHand *pHand = pCmd->vrWeaponHandIsRight
							? GetLocalPlayerRightHand()
							: GetLocalPlayerLeftHand();
						if ( pHand )
						{
							C_BaseAnimating *pRenderWeapon = pHand->GetRenderWeapon();
							if ( pRenderWeapon )
							{
								int iEjectAttach = pRenderWeapon->LookupAttachment( "eject_brass" );
								if ( iEjectAttach > 0 )
								{
									pRenderWeapon->GetAttachment( iEjectAttach, data.m_vOrigin, data.m_vAngles );
									bGotAttachment = true;
								}
							}
						}

						if ( bGotAttachment )
						{
							data.m_nDamageType = GetAttributeContainer()->GetItem()
								? GetAttributeContainer()->GetItem()->GetItemDefIndex() : 0;
							data.m_nHitBox = GetWeaponID();
							DispatchEffect( "TF_EjectBrass", data );
						}
					}
				}
#endif
			}
		}
		else if ( m_bVRLeverStrokeIn )
		{
			float flCompletionDist = flLeverDist * 0.9f;
			float flMinUpPumpTime = MAX( flReloadInterval * 0.5f, 0.05f );
			float flMaxDistPerFrame = ( flCompletionDist / flMinUpPumpTime ) * gpGlobals->frametime;

			float flPrevDist = m_flVRLeverStrokeDist;
			if ( flFrameDisp > 0.0f )
			{
				m_flVRLeverStrokeDist = MIN( m_flVRLeverStrokeDist + flFrameDisp, flCompletionDist );
			}
			if ( m_flVRLeverStrokeDist >= flLeverDist * 0.60f )
			{
				m_flVRLeverStrokeDist = MIN( m_flVRLeverStrokeDist + flMaxDistPerFrame, flCompletionDist );
			}
			m_vecVRLeverLastHandPos = vecHandRelative;

#ifdef CLIENT_DLL
			float flSoundThreshold = flCompletionDist * 0.15f;
			if ( prediction->IsFirstTimePredicted()
				&& flPrevDist < flSoundThreshold
				&& m_flVRLeverStrokeDist >= flSoundThreshold )
			{
				EmitSound( "VR.ScattergunPumpUp" );
			}
#endif

			if ( bDebug )
				DevMsg( "[VR Lever] Up: %.2f / %.2f\n", (float)m_flVRLeverStrokeDist, flCompletionDist );

			if ( m_flVRLeverStrokeDist >= flCompletionDist
				&& gpGlobals->curtime >= m_flNextVRLeverShellReadyTime )
			{
				VRCommitLeverShell();
				m_bVRLeverStrokeIn    = false;
				m_flVRLeverStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR Lever] Shell loaded! Ready for next pump.\n" );
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFScatterGun::Reload( void )
{
	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
		return false;

	int iWeaponMod = 0;
	CALL_ATTRIB_HOOK_INT( iWeaponMod, set_scattergun_no_reload_single );
	if ( iWeaponMod == 1 )
	{
		m_bReloadsSingly = false;
	}

	return BaseClass::Reload();
}

#define JUMP_SPEED	268.3281572999747f
extern float AirBurstDamageForce( const Vector &size, float damage, float scale );

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFScatterGun::FireBullet( CTFPlayer *pPlayer )
{
	if ( HasKnockback() )
	{
		// Perform some knock back.
		CTFPlayer *pOwner = ToTFPlayer( GetPlayerOwner() );
		if ( !pOwner )
			return;

		// No knockback during pre-round freeze.
		if ( TFGameRules() && (TFGameRules()->State_Get() == GR_STATE_PREROUND) )
			return;

		// Knock the firer back!
		if ( !(pOwner->GetFlags() & FL_ONGROUND) && !pPlayer->m_Shared.m_bScattergunJump )
		{
			pPlayer->m_Shared.m_bScattergunJump = true;

			pOwner->m_Shared.StunPlayer( 0.3f, 1.f, TF_STUN_MOVEMENT | TF_STUN_MOVEMENT_FORWARD_ONLY );

			float flForce = AirBurstDamageForce( pOwner->WorldAlignSize(), 60, 6.f );

			// Use weapon firing direction for knockback (VR: controller, non-VR: eye angles)
			QAngle shootAngles = pOwner->Weapon_ShootAngles();
			
			Vector vecForward;
			AngleVectors( shootAngles, &vecForward );
			Vector vecForce = vecForward * -flForce;

			VMatrix mtxPlayer;
			mtxPlayer.SetupMatrixOrgAngles( pOwner->GetAbsOrigin(), shootAngles );
			Vector vecAbsVelocity = pOwner->GetAbsVelocity();
			Vector vecAbsVelocityAsPoint = vecAbsVelocity + pOwner->GetAbsOrigin();
			Vector vecLocalVelocity = mtxPlayer.VMul4x3Transpose( vecAbsVelocityAsPoint );

			vecLocalVelocity.x = -300;

			vecAbsVelocityAsPoint = mtxPlayer.VMul4x3( vecLocalVelocity );
			vecAbsVelocity = vecAbsVelocityAsPoint - pOwner->GetAbsOrigin();
			pOwner->SetAbsVelocity( vecAbsVelocity );

			// Impulse an additional bit of Z push.
			pOwner->ApplyAbsVelocityImpulse( Vector(0,0,50.f) );

			// Slow player movement for a brief period of time.
			pOwner->RemoveFlag( FL_ONGROUND );
		}
	}

	BaseClass::FireBullet( pPlayer );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFScatterGun::ApplyPostHitEffects( const CTakeDamageInfo &inputInfo, CTFPlayer *pPlayer )
{
#ifndef CLIENT_DLL
	if ( !HasKnockback() )
		return;

	CTFPlayer *pAttacker = ToTFPlayer( inputInfo.GetAttacker() );
	if ( !pAttacker )
		return;

	CTFPlayer *pTarget = pPlayer;
	if ( !pTarget )
		return;

	if ( pTarget->m_Shared.GetWeaponKnockbackID() > -1 )
		return;

	if ( pTarget->m_Shared.IsImmuneToPushback() )
		return;

	float flDam = inputInfo.GetDamage();
	Vector vecDir = pAttacker->WorldSpaceCenter() - pTarget->WorldSpaceCenter();
	if ( !CanScatterGunKnockBack( this, flDam, vecDir.LengthSqr() ) )
		return;
	
	VectorNormalize( vecDir );

	float flKnockbackMult = 3.0f;
	CALL_ATTRIB_HOOK_FLOAT_ON_OTHER( this, flKnockbackMult, scattergun_knockback_mult );	

	float flForce = AirBurstDamageForce( pTarget->WorldAlignSize(), flDam, flKnockbackMult );
	Vector vecForce = vecDir * -flForce;
	vecForce.z += JUMP_SPEED;

	pTarget->ApplyGenericPushbackImpulse( vecForce, pAttacker );

	pTarget->m_Shared.StunPlayer( 0.3f, 1.f, TF_STUN_MOVEMENT | TF_STUN_MOVEMENT_FORWARD_ONLY, pAttacker );
	pTarget->m_Shared.SetWeaponKnockbackID( pAttacker->GetUserID() );

#endif
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFScatterGun::FinishReload( void )
{
	CTFPlayer* pOwner = ToTFPlayer( GetOwner() );
	if ( !pOwner )
		return;

	if ( UsesClipsForAmmo1() && !m_bReloadsSingly )
	{
		int primary	= MIN( GetMaxClip1() - m_iClip1, pOwner->GetAmmoCount(m_iPrimaryAmmoType));	
		m_iClip1 += primary;

		// Takes a whole clip worth of ammo to reload, causing us to lose whatever was chambered.
		pOwner->RemoveAmmo( GetMaxClip1(), m_iPrimaryAmmoType);
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFScatterGun::HasKnockback( void )
{
	int iWeaponMod = 0;
	CALL_ATTRIB_HOOK_INT( iWeaponMod, set_scattergun_has_knockback );
	if ( iWeaponMod == 1 )
		return true;
	else
		return false;
}

#ifdef GAME_DLL
//-----------------------------------------------------------------------------
void CTFScatterGun::Equip( CBaseCombatCharacter *pOwner )
{
	CTFPlayer *pPlayer = dynamic_cast<CTFPlayer*>( pOwner );
	if ( pPlayer )
	{
		pPlayer->m_Shared.SetScoutHypeMeter( 0.0f );
	}

	BaseClass::Equip( pOwner );
}
#endif // GAME_DLL
//-----------------------------------------------------------------------------
// CTFSodaPopper
//-----------------------------------------------------------------------------
float CTFSodaPopper::GetProgress( void )
{
	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( !pPlayer )
		return 0.f;

	return pPlayer->m_Shared.GetScoutHypeMeter() * 0.01f;
}

//-----------------------------------------------------------------------------
void CTFSodaPopper::ItemBusyFrame( void )
{
#ifdef GAME_DLL
	CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
	if ( pOwner && pOwner->m_nButtons & IN_ATTACK2 )
	{
		// Check here so we can always activate buff when we want (similar to stickies)
		SecondaryAttack();
	}
#endif

	BaseClass::ItemBusyFrame();
}

//-----------------------------------------------------------------------------
void CTFSodaPopper::SecondaryAttack()
{
	CTFPlayer *pPlayer = GetTFPlayerOwner( );
	if ( !pPlayer || pPlayer->m_Shared.IsHypeBuffed() )
		return;

	if ( pPlayer->m_Shared.GetScoutHypeMeter() >= 100.f )
	{
		pPlayer->m_Shared.AddCond( TF_COND_SODAPOPPER_HYPE );
	}
}

//-----------------------------------------------------------------------------
float CTFPEPBrawlerBlaster::GetProgress( void )
{
	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( !pPlayer )
		return 0.f;

	return pPlayer->m_Shared.GetScoutHypeMeter() * 0.01f;
}

//-----------------------------------------------------------------------------
float CTFShotgunBuildingRescue::GetProjectileSpeed( void )
{
	return RemapValClamped( 0.75f, 0.0f, 1.f, 1800, 2600 ); // Temp, if we want to ramp.
}

//-----------------------------------------------------------------------------
float CTFShotgunBuildingRescue::GetProjectileGravity( void )
{
	return RemapValClamped( 0.75f, 0.0f, 1.f, 0.5f, 0.1f ); // Temp, if we want to ramp.
}

//-----------------------------------------------------------------------------
bool CTFShotgunBuildingRescue::IsViewModelFlipped( void )
{
	return !BaseClass::IsViewModelFlipped(); // Invert because arrows are backwards by default.
}
