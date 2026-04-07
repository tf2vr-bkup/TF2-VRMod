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
#include "in_buttons.h"
#endif

#include "usercmd.h"

extern ConVar tfvr_reload_throttle_scale;

//=============================================================================
//
// Weapon Shotgun tables.
//

CREATE_SIMPLE_WEAPON_TABLE( TFShotgun, tf_weapon_shotgun_primary )
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
ConVar tfvr_scattergun_lever_reload( "tfvr_scattergun_lever_reload", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Scout scattergun: 1 = load shells with weapon-hand pump (requires two-handing + weapon-hand grip); 0 = standard auto/singly reload" );
ConVar tfvr_scattergun_lever_distance( "tfvr_scattergun_lever_distance", "4.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units of weapon-hand motion along lever axis per pump stroke" );
ConVar tfvr_scattergun_lever_sign( "tfvr_scattergun_lever_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply lever-axis motion (+1 or -1) if pump direction feels inverted" );
ConVar tfvr_scattergun_lever_axis( "tfvr_scattergun_lever_axis", "2", FCVAR_REPLICATED, "VR: controller matrix column for pump axis (0=fwd, 1=right, 2=up)" );
ConVar tfvr_scattergun_lever_debug( "tfvr_scattergun_lever_debug", "0", FCVAR_REPLICATED, "VR: 1 = print scattergun lever reload state to console" );

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
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFShotgun::PrimaryAttack()
{
	if ( !CanAttack() )
		return;

	// Set the weapon mode.
	m_iWeaponMode = TF_WEAPON_PRIMARY_MODE;

	BaseClass::PrimaryAttack();
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

	BaseClass::PrimaryAttack();

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
	if ( !tfvr_scattergun_lever_reload.GetBool() )
		return false;

	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !pOwner->IsInVRMode() )
		return false;

#ifdef CLIENT_DLL
	return IsHeldByVRHand();
#else
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
	if ( !pCmdCommit || !pCmdCommit->vrScattergunLeverArmed )
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

	if ( !pCmd->vrScattergunLeverArmed )
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
				m_flVRLeverStrokeDist = 0.0f;
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
				m_flVRLeverStrokeDist = MIN( m_flVRLeverStrokeDist + MIN( flFrameDisp, flMaxDistPerFrame ), flCompletionDist );
			}
			if ( m_flVRLeverStrokeDist >= flLeverDist * 0.75f )
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
