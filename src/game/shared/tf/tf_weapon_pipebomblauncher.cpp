//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================

#include "cbase.h"
#include "tf_weapon_pipebomblauncher.h"
#include "tf_fx_shared.h"
#include "tf_weapon_grenade_pipebomb.h"
#include "in_buttons.h"
#include "datacache/imdlcache.h"
#include "tf_gamerules.h"
#include "usercmd.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include <vgui_controls/Panel.h>
#include <vgui/ISurface.h>
#include "prediction.h"
#include "c_tf_gamestats.h"
// Server specific.
#else
#include "tf_player.h"
#include "tf_gamestats.h"
#endif

#define TF_PIPEBOMB_HIGHLIGHT 1
#define TF_PIPEBOMB_DETONATE  2

#define TF_WEAPON_PIPEBOMBD_MODEL		"models/weapons/w_models/w_stickybomb_d.mdl"

#define TF_WEAPON_PIPEBOMB_LAUNCHER_CHARGE_SOUND	"Weapon_StickyBombLauncher.ChargeUp"

extern ConVar tfvr_reload_throttle_scale;

static inline float TFVR_ReloadThrottleScale()
{
	return MAX( 1.0f, tfvr_reload_throttle_scale.GetFloat() );
}

ConVar tfvr_sticky_pump_reload( "tfvr_sticky_pump_reload", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Demoman stickybomb launcher: 1 = load shells with weapon-forward pump (requires two-handing + weapon-hand grip); 0 = standard auto/singly reload" );
ConVar tfvr_sticky_pump_distance( "tfvr_sticky_pump_distance", "4.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units of weapon-hand motion along weapon forward per pump stroke" );
ConVar tfvr_sticky_pump_sign( "tfvr_sticky_pump_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply pump-axis motion (+1 or -1) if pump direction feels inverted" );
ConVar tfvr_sticky_pump_debug( "tfvr_sticky_pump_debug", "0", FCVAR_REPLICATED, "VR: 1 = print stickybomb pump reload state to console" );

//=============================================================================
//
// Weapon Pipebomb Launcher tables.
//
IMPLEMENT_NETWORKCLASS_ALIASED( TFPipebombLauncher, DT_WeaponPipebombLauncher )

BEGIN_NETWORK_TABLE_NOBASE( CTFPipebombLauncher, DT_PipebombLauncherLocalData )
#ifdef CLIENT_DLL
	RecvPropInt( RECVINFO( m_iPipebombCount ) ),
	RecvPropFloat( RECVINFO( m_flChargeBeginTime ) ),
#else
	SendPropInt( SENDINFO( m_iPipebombCount ), 5, SPROP_UNSIGNED ),
	SendPropFloat( SENDINFO( m_flChargeBeginTime ) ),
#endif
END_NETWORK_TABLE()


BEGIN_NETWORK_TABLE( CTFPipebombLauncher, DT_WeaponPipebombLauncher )
#ifdef CLIENT_DLL
	RecvPropDataTable( "PipebombLauncherLocalData", 0, 0, &REFERENCE_RECV_TABLE( DT_PipebombLauncherLocalData ) ),
	RecvPropBool( RECVINFO( m_bVRPumpIsArmed ) ),
	RecvPropVector( RECVINFO( m_vecVRPumpLastHandPos ) ),
	RecvPropBool( RECVINFO( m_bVRPumpStrokeOut ) ),
	RecvPropBool( RECVINFO( m_bVRPumpStrokeIn ) ),
	RecvPropFloat( RECVINFO( m_flVRPumpStrokeDist ) ),
	RecvPropFloat( RECVINFO( m_flNextVRPumpShellReadyTime ) ),
#else
	SendPropDataTable( "PipebombLauncherLocalData", 0, &REFERENCE_SEND_TABLE( DT_PipebombLauncherLocalData ), SendProxy_SendLocalWeaponDataTable ),
	SendPropBool( SENDINFO( m_bVRPumpIsArmed ) ),
	SendPropVector( SENDINFO( m_vecVRPumpLastHandPos ), -1, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRPumpStrokeOut ) ),
	SendPropBool( SENDINFO( m_bVRPumpStrokeIn ) ),
	SendPropFloat( SENDINFO( m_flVRPumpStrokeDist ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flNextVRPumpShellReadyTime ) ),
#endif	
END_NETWORK_TABLE()

#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA( CTFPipebombLauncher )
	DEFINE_FIELD( m_flChargeBeginTime, FIELD_FLOAT ),
	DEFINE_PRED_FIELD( m_bVRPumpIsArmed, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_vecVRPumpLastHandPos, FIELD_VECTOR, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRPumpStrokeOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRPumpStrokeIn, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRPumpStrokeDist, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flNextVRPumpShellReadyTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
END_PREDICTION_DATA()
#endif

LINK_ENTITY_TO_CLASS( tf_weapon_pipebomblauncher, CTFPipebombLauncher );
PRECACHE_WEAPON_REGISTER( tf_weapon_pipebomblauncher );

// Server specific.
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFPipebombLauncher )
END_DATADESC()
#endif

//=============================================================================
//
// Weapon Pipebomb Launcher functions.
//

//-----------------------------------------------------------------------------
// Purpose: 
// Input  :  - 
//-----------------------------------------------------------------------------
CTFPipebombLauncher::CTFPipebombLauncher()
{
	m_bReloadsSingly = true;
	m_flLastDenySoundTime = 0.0f;
	m_bNoAutoRelease = false;
	m_bWantsToShoot = false;
#ifdef CLIENT_DLL
	m_flNextBombCheckTime = 0;
	m_bBombThinking = false;
#endif

	m_bVRPumpIsArmed = false;
	m_vecVRPumpLastHandPos = vec3_origin;
	m_bVRPumpStrokeOut = false;
	m_bVRPumpStrokeIn = false;
	m_flVRPumpStrokeDist = 0.0f;
	m_flNextVRPumpShellReadyTime = 0.0f;
	m_iVRPumpLastClipForThrottle = -1;

	PrecacheScriptSound( "Weapon_StickyBombLauncher.BoltBack" );
	PrecacheScriptSound( "Weapon_StickyBombLauncher.BoltForward" );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  :  - 
//-----------------------------------------------------------------------------
CTFPipebombLauncher::~CTFPipebombLauncher()
{
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::Spawn( void )
{
	m_iAltFireHint = HINT_ALTFIRE_PIPEBOMBLAUNCHER;
	BaseClass::Spawn();
}

//-----------------------------------------------------------------------------
// Purpose: Reset the charge when we holster
//-----------------------------------------------------------------------------
bool CTFPipebombLauncher::Holster( CBaseCombatWeapon *pSwitchingTo )
{
#ifdef CLIENT_DLL
	if ( m_flChargeBeginTime > 0.f )
	{
		StopSound( TF_WEAPON_PIPEBOMB_LAUNCHER_CHARGE_SOUND );
	}
#endif
	m_flChargeBeginTime = 0;

	ResetVRPumpGestureState();
	m_flNextVRPumpShellReadyTime = 0.0f;
	m_iVRPumpLastClipForThrottle = -1;

	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose: Reset the charge when we deploy
//-----------------------------------------------------------------------------
bool CTFPipebombLauncher::Deploy( void )
{
	m_flChargeBeginTime = 0;

	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
	{
		AbortReload();
		CTFPlayer *pOwner = GetTFPlayerOwner();
		if ( pOwner && Clip1() == 0 && pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0 )
		{
			const float flDelay = ( GetVRSinglyReloadStartThrottleInterval() + GetVRSinglyReloadShellThrottleInterval() ) * TFVR_ReloadThrottleScale();
			m_flNextVRPumpShellReadyTime = gpGlobals->curtime + flDelay;
		}
		else
		{
			m_flNextVRPumpShellReadyTime = 0.0f;
		}
		m_iVRPumpLastClipForThrottle = Clip1();
	}

	return BaseClass::Deploy();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::WeaponReset( void )
{
	BaseClass::WeaponReset();

#ifndef CLIENT_DLL
	DetonateRemotePipebombs( true );
#endif

	m_flChargeBeginTime = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();

	if ( m_flChargeBeginTime > 0 )
	{
		CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
		if ( !pPlayer )
			return;

		// If we're not holding down the attack button, launch our grenade
		if ( m_iClip1 > 0  && !(pPlayer->m_nButtons & IN_ATTACK) && (pPlayer->m_afButtonReleased & IN_ATTACK) )
		{
			LaunchGrenade();
		}
		else if ( !m_bNoAutoRelease )
		{
			float flTotalChargeTime = gpGlobals->curtime - m_flChargeBeginTime;
			if ( flTotalChargeTime >= GetChargeForceReleaseTime() )
			{
				ForceLaunchGrenade();
			}
		}
	}

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

				if ( m_iVRPumpLastClipForThrottle >= 0 )
				{
					const int nMaxClip = GetMaxClip1();
					if ( ( iClip == 0 && m_iVRPumpLastClipForThrottle > 0 ) ||
						( nMaxClip > 1 && m_iVRPumpLastClipForThrottle == nMaxClip && iClip == nMaxClip - 1 ) )
					{
						const float flDelay = ( GetVRSinglyReloadStartThrottleInterval() + GetVRSinglyReloadShellThrottleInterval() ) * TFVR_ReloadThrottleScale();
						m_flNextVRPumpShellReadyTime = MAX( m_flNextVRPumpShellReadyTime, gpGlobals->curtime + flDelay );
					}
					else if ( iClip == nMaxClip )
					{
						m_flNextVRPumpShellReadyTime = 0.0f;
					}
				}
				m_iVRPumpLastClipForThrottle = iClip;
			}
		}

		VRPumpReloadPostFrame();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::PrimaryAttack( void )
{
	// Check for ammunition.
	if ( m_iClip1 <= 0 && m_iClip1 != -1 )
		return;

	// Are we capable of firing again?
	if ( m_flNextPrimaryAttack > gpGlobals->curtime )
		return;

	if ( !CanAttack() )
	{
		m_flChargeBeginTime = 0;
		return;
	}

	if ( m_flChargeBeginTime <= 0 )
	{
		// Set the weapon mode.
		m_iWeaponMode = TF_WEAPON_PRIMARY_MODE;

		// save that we had the attack button down
		m_flChargeBeginTime = gpGlobals->curtime;

		SendWeaponAnim( ACT_VM_PULLBACK );

#ifdef CLIENT_DLL
		EmitSound( TF_WEAPON_PIPEBOMB_LAUNCHER_CHARGE_SOUND );
#endif // CLIENT_DLL
	}
	else
	{
		float flTotalChargeTime = gpGlobals->curtime - m_flChargeBeginTime;

		if ( flTotalChargeTime >= GetChargeMaxTime() )
		{
			LaunchGrenade();
		}
	}

#ifdef CLIENT_DLL
	if ( GetDetonateMode() == TF_DETONATE_MODE_DOT && !m_bBombThinking )
	{
		m_bBombThinking = true;
		SetContextThink( &CTFPipebombLauncher::BombHighlightThink, gpGlobals->curtime + 0.1f, "BOMB_HIGHLIGHT_THINK" );
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
#ifdef CLIENT_DLL
void CTFPipebombLauncher::BombHighlightThink( void )
{
	ModifyPipebombsInView( TF_PIPEBOMB_HIGHLIGHT );
	if ( GetOwner() )
	{
		SetContextThink( &CTFPipebombLauncher::BombHighlightThink, gpGlobals->curtime + 0.1f, "BOMB_HIGHLIGHT_THINK" );
	}
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::WeaponIdle( void )
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( !pPlayer )
		return;

	if ( m_flChargeBeginTime > 0 && m_iClip1 > 0 && (pPlayer->m_afButtonReleased & IN_ATTACK) )
	{
		if ( m_iClip1 > 0 )
		{
			m_bWantsToShoot = true;
		}
	}

	if ( m_bWantsToShoot )
	{
		LaunchGrenade();
	}
	else
	{
		BaseClass::WeaponIdle();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::LaunchGrenade( void )
{
	// Get the player owning the weapon.
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( !pPlayer )
		return;

	m_bWantsToShoot = false;

	CalcIsAttackCritical();

	SendWeaponAnim( ACT_VM_PRIMARYATTACK );

	pPlayer->SetAnimation( PLAYER_ATTACK1 );
	pPlayer->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_PRIMARY );

	CTFGrenadePipebombProjectile *pProjectile = static_cast<CTFGrenadePipebombProjectile*>( FireProjectile( pPlayer ) );
	if ( pProjectile )
	{
		// Save the charge time to scale the detonation timer.
		pProjectile->SetChargeTime( gpGlobals->curtime - m_flChargeBeginTime );

#ifdef GAME_DLL
		if ( GetDetonateMode() == TF_DETONATE_MODE_AIR )
		{
			pProjectile->m_bWallShatter = true;
		}
		else if ( GetDetonateMode() == TF_DETONATE_MODE_DOT )
		{
			pProjectile->m_bDefensiveBomb = true;
			pProjectile->SetModel( TF_WEAPON_PIPEBOMBD_MODEL );
		}

		float flChargeDmg = 1.0f;
		CALL_ATTRIB_HOOK_FLOAT( flChargeDmg, stickybomb_charge_damage_increase );
		if ( flChargeDmg != 1.0f )
		{
			float flDamage = pProjectile->GetDamage();
			flDamage += flDamage * ( flChargeDmg - 1.0f ) * GetCurrentCharge();
			pProjectile->SetDamage( flDamage );
		}
#endif	// GAME_DLL
	}

#ifdef CLIENT_DLL
	C_CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, IsCurrentAttackACrit() );
	StopSound( TF_WEAPON_PIPEBOMB_LAUNCHER_CHARGE_SOUND );
#else
	pPlayer->SpeakWeaponFire();
	CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, IsCurrentAttackACrit() );
#endif

	// Set next attack times.

	float flFireDelay = ApplyFireDelay( m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flTimeFireDelay );

	m_flNextPrimaryAttack = gpGlobals->curtime + flFireDelay;
	m_flLastDenySoundTime = gpGlobals->curtime;

	SetWeaponIdleTime( gpGlobals->curtime + SequenceDuration() );

	// Check the reload mode and behave appropriately.
	if ( m_bReloadsSingly )
	{
		m_iReloadMode.Set( TF_RELOAD_START );
	}

	m_flChargeBeginTime = 0;

	if ( TFGameRules()->GameModeUsesUpgrades() )
	{
		PlayUpgradedShootSound( "Weapon_Upgrade.DamageBonus" );
	}
}

float CTFPipebombLauncher::GetProjectileSpeed( void )
{
	float flForwardSpeed = RemapValClamped( ( gpGlobals->curtime - m_flChargeBeginTime ),
		0.0f,
		GetChargeMaxTime(),
		TF_PIPEBOMB_MIN_CHARGE_VEL,
		TF_PIPEBOMB_MAX_CHARGE_VEL );

	return flForwardSpeed;
}

void CTFPipebombLauncher::AddPipeBomb( CTFGrenadePipebombProjectile *pBomb )
{
	PipebombHandle hHandle;
	hHandle = pBomb;
	m_Pipebombs.AddToTail( hHandle );
}

//-----------------------------------------------------------------------------
// Purpose: Add pipebombs to our list as they're fired
//-----------------------------------------------------------------------------
CBaseEntity *CTFPipebombLauncher::FireProjectile( CTFPlayer *pPlayer )
{
	CBaseEntity *pProjectile = BaseClass::FireProjectile( pPlayer );
	if ( pProjectile )
	{
#ifdef GAME_DLL
		// If we've gone over the max pipebomb count, detonate the oldest
		int nMaxPipebombs = TF_WEAPON_PIPEBOMB_COUNT;
		CALL_ATTRIB_HOOK_INT( nMaxPipebombs, add_max_pipebombs );
		if ( m_Pipebombs.Count() >= nMaxPipebombs )
		{
			CTFGrenadePipebombProjectile *pTemp = m_Pipebombs[0];
			if ( pTemp )
			{
				pTemp->SetTimer( gpGlobals->curtime ); // explode NOW
			}

			m_Pipebombs.Remove(0);
		}

		CTFGrenadePipebombProjectile *pPipebomb = (CTFGrenadePipebombProjectile*)pProjectile;

		PipebombHandle hHandle;
		hHandle = pPipebomb;
		m_Pipebombs.AddToTail( hHandle );

		m_iPipebombCount = m_Pipebombs.Count();
#endif
	}

	return pProjectile;
}

//-----------------------------------------------------------------------------
// Purpose: Detonate this demoman's pipebombs if secondary fire is down.
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::ItemBusyFrame( void )
{
#ifdef GAME_DLL
	CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
	if ( pOwner && pOwner->m_nButtons & IN_ATTACK2 )
	{
		// We need to do this to catch the case of player trying to detonate
		// pipebombs while in the middle of reloading.
		SecondaryAttack();
	}
#endif

	BaseClass::ItemBusyFrame();
}

//-----------------------------------------------------------------------------
// Purpose: Detonate active pipebombs
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::SecondaryAttack( void )
{
	if ( !CanAttack() )
		return;

	if ( m_iPipebombCount )
	{
		// Get a valid player.
		CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
		if ( !pPlayer )
			return;

		//If one or more pipebombs failed to detonate then play a sound.
		if ( DetonateRemotePipebombs( false ) == true )
		{
			if ( m_flLastDenySoundTime <= gpGlobals->curtime )
			{
				// Deny!
				m_flLastDenySoundTime = gpGlobals->curtime + 1;
				WeaponSound( SPECIAL2 );
				return;
			}
		}
		else
		{
			// Play a detonate sound.
			WeaponSound( SPECIAL3 );

#ifdef GAME_DLL
			IGameEvent *pDetEvent = gameeventmanager->CreateEvent( "demoman_det_stickies" );

			if ( pDetEvent )
			{
				pDetEvent->SetInt( "player", pPlayer->entindex() );

				// Send the event
				gameeventmanager->FireEvent( pDetEvent );
			}
#endif
		}
	}
}

//=============================================================================
//
// Server specific functions.
//
#ifdef GAME_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::UpdateOnRemove(void)
{
	// If we just died, we want to fizzle our pipebombs.
	// If the player switched classes, our pipebombs have already been removed.
	DetonateRemotePipebombs( true );

	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::ApplyPostHitEffects( const CTakeDamageInfo &inputInfo, CTFPlayer *pPlayer )
{
	if ( !GetTFPlayerOwner() )
		return;

	if ( pPlayer->m_Shared.GetWeaponKnockbackID() == -1 )
	{
		pPlayer->m_Shared.SetWeaponKnockbackID( GetTFPlayerOwner()->GetUserID() );
	}
}

#endif


//-----------------------------------------------------------------------------
// Purpose: If a pipebomb has been removed, remove it from our list
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::DeathNotice( CBaseEntity *pVictim )
{
	Assert( dynamic_cast<CTFGrenadePipebombProjectile*>(pVictim) );

	PipebombHandle hHandle;
	hHandle = (CTFGrenadePipebombProjectile*)pVictim;
	m_Pipebombs.FindAndRemove( hHandle );

	m_iPipebombCount = m_Pipebombs.Count();
}


//-----------------------------------------------------------------------------
// Purpose: Remove *with* explosions
//-----------------------------------------------------------------------------
bool CTFPipebombLauncher::DetonateRemotePipebombs( bool bFizzle )
{
	if ( GetDetonateMode() == TF_DETONATE_MODE_DOT && !bFizzle )
	{
		return ModifyPipebombsInView( TF_PIPEBOMB_DETONATE );
	}

	bool bFailedToDetonate = false;

	int count = m_Pipebombs.Count();

	for ( int i = 0; i < count; i++ )
	{
		CTFGrenadePipebombProjectile *pTemp = m_Pipebombs[i];
		if ( pTemp )
		{
			//This guy will die soon enough.
			if ( pTemp->IsEffectActive( EF_NODRAW ) )
				continue;
#ifdef GAME_DLL
			if ( bFizzle )
			{
				pTemp->Fizzle();
			}
#endif

			if ( bFizzle == false )
			{
				if ( ( gpGlobals->curtime - pTemp->m_flCreationTime ) < pTemp->GetLiveTime() )
				{
					if ( pTemp->GetLiveTime() <= 0.5f )
					{
						pTemp->SetDetonateOnPulse( true );
					}
					bFailedToDetonate = true;
					continue;
				}
			}
#ifdef GAME_DLL
			if ( CanDestroyStickies() )
			{
				pTemp->DetonateStickies();
			}
			pTemp->Detonate();
#endif
		}
	}

	return bFailedToDetonate;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFPipebombLauncher::ModifyPipebombsInView( int iEffect )
{
	CTFPlayer* pPlayer = ToTFPlayer( GetOwner() );
	if ( !pPlayer )
		return true;

	// Dot product from the view angle to determine which bombs to detonate.
	bool bFailedToDetonate = true;
	int count = m_Pipebombs.Count();
	
	// VR: Use weapon aim direction instead of eye/head direction
	Vector vecAimOrigin;
	Vector vecAimForward;
	if ( pPlayer->IsInVRMode() )
	{
		// Use weapon shoot position and angles (controller-based in VR)
		vecAimOrigin = pPlayer->Weapon_ShootPosition();
		AngleVectors( pPlayer->Weapon_ShootAngles(), &vecAimForward, NULL, NULL );
	}
	else
	{
		// Standard: use eye position and angles
		vecAimOrigin = pPlayer->EyePosition();
		AngleVectors( pPlayer->EyeAngles(), &vecAimForward, NULL, NULL );
	}
	vecAimForward.NormalizeInPlace();
	
	for ( int i=0; i<count; ++i )
	{
		CTFGrenadePipebombProjectile *pTemp = m_Pipebombs[i];
		if ( !pTemp || pTemp->IsEffectActive( EF_NODRAW ) )
			continue;

		Vector vecToTarget;
		vecToTarget = pTemp->WorldSpaceCenter() - vecAimOrigin;
		vecToTarget.NormalizeInPlace();

		Vector vecPlayerForward = vecAimForward;

		bool bArmed = ( ( gpGlobals->curtime - pTemp->m_flCreationTime ) > pTemp->GetLiveTime() );
		float flDist = pPlayer->GetAbsOrigin().DistTo( pTemp->GetAbsOrigin() );
		float flDot = DotProduct( vecToTarget, vecPlayerForward );

		// Detonate sticky bombs directly under the crosshair or under our feet (to allow sticky jumping)
		if ( flDot > 0.975f || flDist < pTemp->GetDamageRadius() )
		{
			switch ( iEffect )
			{
			case TF_PIPEBOMB_HIGHLIGHT:
#ifdef CLIENT_DLL
				pTemp->SetHighlight( true );
#endif
				break;
			case TF_PIPEBOMB_DETONATE:
				if ( bArmed )
				{
					bFailedToDetonate = false;
#ifdef GAME_DLL
					if ( CanDestroyStickies() )
					{
						pTemp->DetonateStickies();
					}
#endif
					pTemp->Detonate();
				}
				break;
			}
		}
		else if ( iEffect == TF_PIPEBOMB_HIGHLIGHT )
		{
#ifdef CLIENT_DLL
			pTemp->SetHighlight( false );
#endif
		}
	}

	return bFailedToDetonate;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFPipebombLauncher::Reload( void )
{
	if ( m_flChargeBeginTime > 0 )
		return false;

	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
		return false;

	return BaseClass::Reload();
}

//=============================================================================
//
// VR physical pump reload
//
//=============================================================================

//-----------------------------------------------------------------------------
bool CTFPipebombLauncher::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	if ( GetWeaponID() != TF_WEAPON_PIPEBOMBLAUNCHER )
		return false;
	if ( !tfvr_sticky_pump_reload.GetBool() )
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
void CTFPipebombLauncher::ResetVRPumpGestureState( void )
{
	m_bVRPumpIsArmed = false;
	m_vecVRPumpLastHandPos = vec3_origin;
	m_bVRPumpStrokeOut = false;
	m_bVRPumpStrokeIn = false;
	m_flVRPumpStrokeDist = 0.0f;
}

//-----------------------------------------------------------------------------
float CTFPipebombLauncher::GetVRPumpStrokeProgress() const
{
	float dist = tfvr_sticky_pump_distance.GetFloat();
	return ( dist > 0.0f ) ? clamp( (float)m_flVRPumpStrokeDist / dist, 0.0f, 1.0f ) : 0.0f;
}

//-----------------------------------------------------------------------------
void CTFPipebombLauncher::VRCommitPumpShell( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

	const CUserCmd *pCmdCommit = pOwner->GetCurrentUserCommand();
	if ( !pCmdCommit || !pCmdCommit->vrWeaponArmed )
		return;

	if ( gpGlobals->curtime < m_flNextVRPumpShellReadyTime )
		return;

	if ( Clip1() >= GetMaxClip1() )
		return;
	if ( pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
		return;
	if ( CheckReloadMisfire() )
		return;

	m_iClip1++;
	pOwner->RemoveAmmo( 1, m_iPrimaryAmmoType );
	m_flNextVRPumpShellReadyTime = gpGlobals->curtime + GetVRSinglyReloadShellThrottleInterval() * TFVR_ReloadThrottleScale();

#ifdef CLIENT_DLL
	if ( ShouldPlayClientReloadSound() )
		WeaponSound( RELOAD );
#else
	WeaponSound( RELOAD );
#endif
}

//-----------------------------------------------------------------------------
// Two-stroke gesture along weapon forward (pullback, then push forward) loads
// one shell.  Mirrors CTFScatterGun::VRLeverReloadPostFrame but uses weapon
// forward (controller matrix column 0) as the pump axis.
//-----------------------------------------------------------------------------
void CTFPipebombLauncher::VRPumpReloadPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

#if defined( CLIENT_DLL )
	if ( !pOwner->IsLocalPlayer() )
		return;
#endif

	const bool bDebug = tfvr_sticky_pump_debug.GetBool();

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
	{
		ResetVRPumpGestureState();
		return;
	}

	// The pump hand is the OFF-hand (left for a right-hand weapon).
	// Track pump-hand motion relative to the weapon hand (stable reference).
	Vector vecPumpHandRaw = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerPosL
		: pCmd->vrRawControllerPosR;
	Vector vecWeaponHandRaw = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerPosR
		: pCmd->vrRawControllerPosL;

	if ( vecPumpHandRaw == vec3_origin || vecWeaponHandRaw == vec3_origin )
		return;

	Vector vecHandRelative = vecPumpHandRaw - vecWeaponHandRaw;

	if ( Clip1() >= GetMaxClip1() || pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		if ( bDebug && ( m_bVRPumpStrokeOut || m_bVRPumpStrokeIn ) )
			DevMsg( "[VR StickyPump] Reset: clip full or no reserve ammo\n" );
		ResetVRPumpGestureState();
		return;
	}

	// Block pumping while fire or charge cycle is active
	if ( gpGlobals->curtime < m_flNextPrimaryAttack || m_flChargeBeginTime > 0 )
	{
		m_bVRPumpIsArmed = false;
		m_bVRPumpStrokeOut = false;
		m_bVRPumpStrokeIn = false;
		m_flVRPumpStrokeDist = 0.0f;
		if ( m_vecVRPumpLastHandPos != vec3_origin )
			m_vecVRPumpLastHandPos = vecHandRelative;
		if ( bDebug )
			DevMsg( "[VR StickyPump] Paused: fire/charge cooldown\n" );
		return;
	}

	if ( !pCmd->vrWeaponArmed )
	{
		const bool bMidStroke = m_bVRPumpStrokeOut || m_bVRPumpStrokeIn;
		if ( !bMidStroke )
		{
			if ( bDebug && m_vecVRPumpLastHandPos != vec3_origin )
				DevMsg( "[VR StickyPump] Reset: not armed\n" );
			ResetVRPumpGestureState();
		}
		else
		{
			m_vecVRPumpLastHandPos = vecHandRelative;
			m_flVRPumpStrokeDist = 0.0f;
			if ( bDebug )
				DevMsg( "[VR StickyPump] Grip lost mid-stroke, holding state\n" );
		}
		return;
	}

	m_bVRPumpIsArmed = true;

	const float flPumpDist     = tfvr_sticky_pump_distance.GetFloat();
	const float flSign         = tfvr_sticky_pump_sign.GetFloat();
	const float flReloadInterval = GetVRSinglyReloadShellThrottleInterval() * TFVR_ReloadThrottleScale();

	// Pump axis = weapon forward (controller matrix column 0)
	QAngle angWeaponHand = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerAngR
		: pCmd->vrRawControllerAngL;

	matrix3x4_t controllerMatrix;
	AngleMatrix( angWeaponHand, controllerMatrix );

	Vector vecPumpAxis;
	MatrixGetColumn( controllerMatrix, 0, vecPumpAxis );
	VectorNormalize( vecPumpAxis );

	// First frame: seed the reference position
	if ( m_vecVRPumpLastHandPos == vec3_origin )
	{
		m_vecVRPumpLastHandPos = vecHandRelative;
		m_flVRPumpStrokeDist   = 0.0f;
		if ( bDebug )
			DevMsg( "[VR StickyPump] Tracking started\n" );
		return;
	}

	// State machine: Neutral -> PullBack -> PushForward -> commit -> Neutral
	if ( !m_bVRPumpStrokeOut && !m_bVRPumpStrokeIn )
	{
		if ( m_vecVRPumpLastHandPos != vec3_origin )
		{
			Vector vecFrameDelta = vecHandRelative - m_vecVRPumpLastHandPos;
			float  flFrameDisp   = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

			if ( flFrameDisp < 0.0f )
			{
				m_flVRPumpStrokeDist += -flFrameDisp;
			}
			else
			{
				m_flVRPumpStrokeDist = MAX( m_flVRPumpStrokeDist - flFrameDisp * 2.0f, 0.0f );
			}

			if ( m_flVRPumpStrokeDist >= 0.5f )
			{
				m_bVRPumpStrokeOut = true;
				m_vecVRPumpLastHandPos = vecHandRelative;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR StickyPump] Pullback started\n" );
			}
			else
			{
				m_vecVRPumpLastHandPos = vecHandRelative;
			}
		}
		else
		{
			m_vecVRPumpLastHandPos = vecHandRelative;
			m_flVRPumpStrokeDist = 0.0f;
		}
	}
	else
	{
		Vector vecFrameDelta = vecHandRelative - m_vecVRPumpLastHandPos;
		float  flFrameDisp   = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

		if ( m_bVRPumpStrokeOut )
		{
			if ( flFrameDisp < 0.0f )
			{
				m_flVRPumpStrokeDist += -flFrameDisp;
			}
			m_vecVRPumpLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR StickyPump] Pullback: %.2f / %.2f\n", (float)m_flVRPumpStrokeDist, flPumpDist );

			if ( m_flVRPumpStrokeDist >= flPumpDist )
			{
				m_bVRPumpStrokeOut   = false;
				m_bVRPumpStrokeIn    = true;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR StickyPump] Pullback complete, push forward to load\n" );

#ifdef CLIENT_DLL
				if ( prediction->IsFirstTimePredicted() )
				{
					EmitSound( "Weapon_StickyBombLauncher.BoltBack" );
				}
#endif
			}
		}
		else if ( m_bVRPumpStrokeIn )
		{
			float flCompletionDist = flPumpDist * 0.9f;
			float flMinPushTime = MAX( flReloadInterval * 0.5f, 0.05f );
			float flMaxDistPerFrame = ( flCompletionDist / flMinPushTime ) * gpGlobals->frametime;

			float flPrevDist = m_flVRPumpStrokeDist;
			if ( flFrameDisp > 0.0f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + MIN( flFrameDisp, flMaxDistPerFrame ), flCompletionDist );
			}
			if ( m_flVRPumpStrokeDist >= flPumpDist * 0.75f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + flMaxDistPerFrame, flCompletionDist );
			}
			m_vecVRPumpLastHandPos = vecHandRelative;

#ifdef CLIENT_DLL
			float flSoundThreshold = flCompletionDist * 0.15f;
			if ( prediction->IsFirstTimePredicted()
				&& flPrevDist < flSoundThreshold
				&& m_flVRPumpStrokeDist >= flSoundThreshold )
			{
				EmitSound( "Weapon_StickyBombLauncher.BoltForward" );
			}
#endif

			if ( bDebug )
				DevMsg( "[VR StickyPump] Push: %.2f / %.2f\n", (float)m_flVRPumpStrokeDist, flCompletionDist );

			if ( m_flVRPumpStrokeDist >= flCompletionDist
				&& gpGlobals->curtime >= m_flNextVRPumpShellReadyTime )
			{
				VRCommitPumpShell();
				m_bVRPumpStrokeIn    = false;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR StickyPump] Shell loaded! Ready for next pump.\n" );
			}
		}
	}
}
