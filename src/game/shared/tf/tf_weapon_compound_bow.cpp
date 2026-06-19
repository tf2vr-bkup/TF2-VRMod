//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_compound_bow.h"
#include "tf_fx_shared.h"
#include "tf_gamerules.h"
#include "in_buttons.h"
#include "usercmd.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include "c_tf_gamestats.h"
#include "prediction.h"
// Server specific.
#else
#include "tf_player.h"
#include "tf_gamestats.h"
#include "tf_projectile_arrow.h"
#endif

#define COMPOUND_BOW_ATTACHMENT_POINT "muzzle"
//=============================================================================
//
// Weapon tables.
//
IMPLEMENT_NETWORKCLASS_ALIASED( TFCompoundBow, DT_WeaponCompoundBow )

BEGIN_NETWORK_TABLE( CTFCompoundBow, DT_WeaponCompoundBow )
#ifdef CLIENT_DLL
	RecvPropBool( RECVINFO( m_bArrowAlight ) ),
	RecvPropBool( RECVINFO( m_bNoFire ) ),
	RecvPropBool( RECVINFO( m_bVRBowArrowHeld ) ),
	RecvPropBool( RECVINFO( m_bVRBowArrowNockActive ) ),
	RecvPropBool( RECVINFO( m_bVRBowArrowNocked ) ),
	RecvPropFloat( RECVINFO( m_flVRBowArrowNockStartTime ) ),
	RecvPropFloat( RECVINFO( m_flNextVRBowArrowReadyTime ) ),
	RecvPropBool( RECVINFO( m_bVRBowNockInputIsTrigger ) ),
	RecvPropFloat( RECVINFO( m_flVRBowArrowPull ) ),
#else
	SendPropBool( SENDINFO( m_bArrowAlight ) ),
	SendPropBool( SENDINFO( m_bNoFire ) ),
	SendPropBool( SENDINFO( m_bVRBowArrowHeld ) ),
	SendPropBool( SENDINFO( m_bVRBowArrowNockActive ) ),
	SendPropBool( SENDINFO( m_bVRBowArrowNocked ) ),
	SendPropFloat( SENDINFO( m_flVRBowArrowNockStartTime ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flNextVRBowArrowReadyTime ), 0, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bVRBowNockInputIsTrigger ) ),
	SendPropFloat( SENDINFO( m_flVRBowArrowPull ), 0, SPROP_NOSCALE ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFCompoundBow )
#ifdef CLIENT_DLL
	DEFINE_PRED_FIELD( m_flChargeBeginTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bNoFire, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRBowArrowHeld, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRBowArrowNockActive, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRBowArrowNocked, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRBowArrowNockStartTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flNextVRBowArrowReadyTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRBowNockInputIsTrigger, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRBowArrowPull, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
#endif
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_compound_bow, CTFCompoundBow );
PRECACHE_WEAPON_REGISTER( tf_weapon_compound_bow );

// Server specific.
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFCompoundBow )
END_DATADESC()
#endif

#define TF_ARROW_MAX_CHARGE_TIME 5.0f

ConVar tfvr_huntsman_manual_reload( "tfvr_huntsman_manual_reload", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: require grabbing and nocking an arrow before charging/firing" );
ConVar tfvr_huntsman_nock_duration( "tfvr_huntsman_nock_duration", "0.22", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: seconds spent snapping the held arrow into the nock pose" );
ConVar tfvr_huntsman_min_fire_charge( "tfvr_huntsman_min_fire_charge", "0.06", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: releasing the draw below this charge (0..1) cancels and returns the arrow instead of firing" );
ConVar tfvr_huntsman_pull_sound_delta( "tfvr_huntsman_pull_sound_delta", "0.01", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: physical pull fraction change needed to trigger pull/de-pull sounds" );
ConVar tfvr_huntsman_pull_sound_full_delta( "tfvr_huntsman_pull_sound_full_delta", "0.04", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: initial physical pull fraction delta that chooses the full pull sound instead of short" );
ConVar tfvr_huntsman_pull_sound_cooldown( "tfvr_huntsman_pull_sound_cooldown", "0.06", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: minimum seconds between pull direction sounds" );
ConVar tfvr_huntsman_pull_sound_settle_time( "tfvr_huntsman_pull_sound_settle_time", "0.16", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: seconds the drawstring must stop moving before pull sounds can re-arm" );
ConVar tfvr_huntsman_pull_sound_min_travel( "tfvr_huntsman_pull_sound_min_travel", "0.045", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR Huntsman: accumulated pull fraction travel required before pull/de-pull sounds play" );

//=============================================================================
//
// Weapon functions.
//

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFCompoundBow::CTFCompoundBow()
{
	m_flLastDenySoundTime = 0.0f;
	m_bNoFire = false;
	m_bReloadsSingly = false;
	m_bVRBowArrowHeld = false;
	m_bVRBowArrowNockActive = false;
	m_bVRBowArrowNocked = false;
	m_flVRBowArrowNockStartTime = 0.0f;
	m_flNextVRBowArrowReadyTime = 0.0f;
	m_bVRBowNockInputIsTrigger = false;
	m_flVRBowArrowPull = 0.0f;
	m_flVRBowLastPhysicalPullForSound = 0.0f;
	for ( int i = 0; i < ARRAYSIZE( m_flVRBowPullSoundSamples ); i++ )
		m_flVRBowPullSoundSamples[i] = 0.0f;
	m_iVRBowPullSoundSampleCount = ARRAYSIZE( m_flVRBowPullSoundSamples );
	m_iVRBowPullSoundSampleIndex = 0;
	m_flNextVRBowPullSoundTime = 0.0f;
	m_flVRBowPullSoundLastMoveTime = 0.0f;
	m_flVRBowPullSoundPendingMove = 0.0f;
	m_iVRBowPullSoundPendingDirection = 0;
	m_iVRBowPullSoundDirection = 0;
}

void CTFCompoundBow::Precache( void )
{
	PrecacheScriptSound( "Weapon_CompoundBow.SinglePull" );
	PrecacheScriptSound( "VR.CompoundBowArrowGrab" );
	PrecacheScriptSound( "VR.CompoundBowPull" );
	PrecacheScriptSound( "VR.CompoundBowPullShort" );
	PrecacheScriptSound( "VR.CompoundBowPullReverse" );
	PrecacheScriptSound( "ArrowLight" );

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::WeaponReset( void )
{
	LowerBow();

	BaseClass::WeaponReset();

//	GetInternalChargeBeginTime() = 0;	
	m_bArrowAlight = false;
	m_bNoAutoRelease = true;
	m_bNoFire = false;
	ResetVRBowArrowState();
}

#ifdef GAME_DLL


#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::LaunchGrenade( void )
{
	// Get the player owning the weapon.
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( !pPlayer )
		return;

	CalcIsAttackCritical();

	SendWeaponAnim( ACT_VM_PRIMARYATTACK );

	pPlayer->SetAnimation( PLAYER_ATTACK1 );
	pPlayer->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_PRIMARY );

	m_bWantsToShoot = false;

#ifdef GAME_DLL
	CTFProjectile_Arrow *pMainArrow = assert_cast<CTFProjectile_Arrow*>( FireProjectile( pPlayer ) );
	if ( pMainArrow )
	{
		pMainArrow->SetArrowAlight( m_bArrowAlight );

	}

#else
	FireProjectile( pPlayer );
#endif

#if !defined( CLIENT_DLL ) 
	pPlayer->SpeakWeaponFire();
	CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, IsCurrentAttackACrit() );
#endif
#ifdef CLIENT_DLL
	C_CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, IsCurrentAttackACrit() );
#endif

	// Set next attack times.
	float flBaseFireDelay = m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flTimeFireDelay;
	float flFireDelay = ApplyFireDelay( flBaseFireDelay );

	ApplyRefireSpeedModifications( flFireDelay );
	
	float flRateMultiplyer = flBaseFireDelay / flFireDelay;

	// Speed up the reload animation built in to firing
	if ( pPlayer->GetViewModel(0) )
	{
		pPlayer->GetViewModel(0)->SetPlaybackRate( flRateMultiplyer );
	}
	if ( pPlayer->GetViewModel(1) )
	{
		pPlayer->GetViewModel(1)->SetPlaybackRate( flRateMultiplyer );
	}

	m_flNextPrimaryAttack = gpGlobals->curtime + flFireDelay;
	m_flLastDenySoundTime = gpGlobals->curtime;

	float flIdleDelay = 0.5f * flRateMultiplyer;
	SetWeaponIdleTime( m_flNextPrimaryAttack + flIdleDelay );

	pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
	pPlayer->TeamFortress_SetSpeed();

	SetInternalChargeBeginTime( 0 );
	m_bArrowAlight = false;
	ResetVRBowArrowState();

	// The bow doesn't actually reload, it instead uses the AE_WPN_INCREMENTAMMO anim event in the fire to reload the clip.
	// We need to reset this bool each time we fire so that anim event works.
	m_bReloadedThroughAnimEvent = false;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::PrimaryAttack( void )
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( !pPlayer )
		return;

	// Check for ammunition.
	if ( m_iClip1 <= 0 && m_iClip1 != -1 )
		return;

	// Are we capable of firing again?
	if ( m_flNextPrimaryAttack > gpGlobals->curtime )
		return;

	if ( m_bNoFire )
		return;

	if ( !CanAttack() )
	{
		SetInternalChargeBeginTime( 0 );
		return;
	}

	if ( GetInternalChargeBeginTime() <= 0 )
	{
		// Set the weapon mode.
		m_iWeaponMode = TF_WEAPON_PRIMARY_MODE;

		// save that we had the attack button down
		SetInternalChargeBeginTime( gpGlobals->curtime );

		SendWeaponAnim( ACT_VM_PULLBACK );

		float flRateMultiplyer = ApplyFireDelay( 1.0f );
		ApplyRefireSpeedModifications( flRateMultiplyer );
		if ( flRateMultiplyer > 0.0f )
		{
			flRateMultiplyer = 1.0f / flRateMultiplyer;
		}

		// Speed up the reload animation built in to firing
		if ( pPlayer->GetViewModel(0) )
		{
			pPlayer->GetViewModel(0)->SetPlaybackRate( flRateMultiplyer );
		}
		if ( pPlayer->GetViewModel(1) )
		{
			pPlayer->GetViewModel(1)->SetPlaybackRate( flRateMultiplyer );
		}

		bool bPlaySound = !ShouldUseVRBowManualReload();
#ifdef CLIENT_DLL
		bPlaySound = bPlaySound && prediction->IsFirstTimePredicted();
#endif
		if ( bPlaySound )
		{
			// Increase the pitch of the pull sound when the fire rate is higher
			CSoundParameters params;
			if ( CBaseEntity::GetParametersForSound( "Weapon_CompoundBow.SinglePull", params, NULL ) )
			{
				CPASAttenuationFilter filter( pPlayer->GetAbsOrigin(), params.soundlevel );
#ifdef GAME_DLL
				filter.RemoveRecipient( pPlayer );
#endif
				EmitSound_t ep( params );
				ep.m_nPitch *= flRateMultiplyer;

				pPlayer->EmitSound( filter, pPlayer->entindex(), ep );
			}
		}

		// Slow down movement speed while the bow is pulled back.
		pPlayer->m_Shared.AddCond( TF_COND_AIMING );
		pPlayer->TeamFortress_SetSpeed();
	}
	else
	{
		float flTotalChargeTime = gpGlobals->curtime - GetInternalChargeBeginTime();

		if ( flTotalChargeTime >= GetChargeMaxTime() )
		{
			flTotalChargeTime = GetChargeMaxTime();
//			LaunchGrenade();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFCompoundBow::GetChargeMaxTime( void )
{
	// It takes less time to charge if the fire rate is higher
	float flChargeMaxTime = ApplyFireDelay( 1.0f );
	ApplyRefireSpeedModifications( flChargeMaxTime );

	return flChargeMaxTime;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFCompoundBow::GetCurrentCharge( void )
{
	if ( GetInternalChargeBeginTime() == 0 )
		return 0;

	// Time-based cap: the charge can never exceed how long the bow has been
	// drawn (this is the throttle).
	float flCharge = MIN( gpGlobals->curtime - GetInternalChargeBeginTime(), 1.f );

	// VR drawstring control: m_flVRBowArrowPull is already a resisted 0..1 pull
	// fraction. De-pulling lowers it immediately; re-pulling is rate-limited in
	// UpdateVRBowArrowPull. Scale it into charge seconds here.
	if ( ShouldUseVRBowManualReload() )
		flCharge = MIN( flCharge, clamp( m_flVRBowArrowPull, 0.f, 1.f ) * GetChargeMaxTime() );

	return flCharge;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFCompoundBow::GetProjectileDamage( void )
{
	float flDamage = BaseClass::GetProjectileDamage();
	float flBaseDamage = 50.f;
	CALL_ATTRIB_HOOK_FLOAT( flBaseDamage, mult_dmg );
	float flScale = Clamp( GetCurrentCharge() / GetChargeMaxTime(), 0.f, 1.f);
	float flScaleDamage = flDamage * flScale;

	return (flBaseDamage + flScaleDamage);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFCompoundBow::GetProjectileSpeed( void )
{
	return RemapValClamped( GetCurrentCharge(), 0.0f, GetChargeMaxTime(), 1800, 2600 );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFCompoundBow::GetProjectileGravity( void )
{
	return RemapValClamped( GetCurrentCharge(), 0.0f, GetChargeMaxTime(), 0.5, 0.1 );
}


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFCompoundBow::AddPipeBomb( CTFGrenadePipebombProjectile *pBomb )
{
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFCompoundBow::SecondaryAttack( void )
{
	LowerBow();
}

//-----------------------------------------------------------------------------
// Purpose: Un-nocks a ready arrow.
//-----------------------------------------------------------------------------
void CTFCompoundBow::LowerBow( void )
{
	if ( GetCurrentCharge() == 0.f )
		return; // No arrow nocked.

	SetInternalChargeBeginTime( 0 );

	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
		pPlayer->TeamFortress_SetSpeed();
	}

	m_flNextPrimaryAttack = gpGlobals->curtime + 1.f;

	m_bNoFire = true;
	m_bWantsToShoot = false;
	ResetVRBowArrowState();

	SendWeaponAnim( ACT_ITEM2_VM_DRYFIRE );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFCompoundBow::DetonateRemotePipebombs( bool bFizzle )
{
	return false;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CTFCompoundBow::Deploy( void )
{
	const bool bDeployed = BaseClass::Deploy();
	if ( bDeployed )
	{
		m_bNoFire = false;
		m_flNextVRBowArrowReadyTime = 0.0f;
		ResetVRBowArrowState();
	}

	return bDeployed;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFCompoundBow::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
		pPlayer->TeamFortress_SetSpeed();
	}
	m_bNoFire = false;
	SetArrowAlight( false );
	SetInternalChargeBeginTime( 0 );
	ResetVRBowArrowState();

	return BaseClass::Holster( pSwitchingTo );
}


//-----------------------------------------------------------------------------
// Purpose: Play animation appropriate to ball status.
//-----------------------------------------------------------------------------
bool CTFCompoundBow::SendWeaponAnim( int iActivity )
{
	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( !pPlayer )
		return BaseClass::SendWeaponAnim( iActivity );

	if ( iActivity == ACT_VM_PULLBACK )
	{
		iActivity = ACT_ITEM2_VM_CHARGE;
	}

	float flTotalChargeTime = gpGlobals->curtime - GetInternalChargeBeginTime();
	if ( GetCurrentCharge() > 0 )
	{
		switch ( iActivity )
		{
		case ACT_VM_IDLE:
			if ( flTotalChargeTime >= TF_ARROW_MAX_CHARGE_TIME )
			{
				int iAct = GetActivity();
				if ( iAct == ACT_ITEM2_VM_IDLE_3 || iAct == ACT_ITEM2_VM_CHARGE_IDLE_3 )
				{
					iActivity = ACT_ITEM2_VM_IDLE_3;
				}
				else
				{
					iActivity = ACT_ITEM2_VM_CHARGE_IDLE_3;
				}
			}
			else
			{
				iActivity = ACT_ITEM2_VM_IDLE_2;
			}
			break;
		default:
			break;
		}
	}

	return BaseClass::SendWeaponAnim( iActivity );
}

void CTFCompoundBow::ItemBusyFrame( void )
{
	if ( ShouldUseVRBowManualReload() )
	{
		VRBowArrowPostFrame();
	}

	BaseClass::ItemBusyFrame();
}

//-----------------------------------------------------------------------------
// Purpose: Play animation appropriate to ball status.
//-----------------------------------------------------------------------------
void CTFCompoundBow::ItemPostFrame( void )
{
	CTFPlayer *pTFPlayer = GetTFPlayerOwner();
	CBasePlayer *pOwner = pTFPlayer;
	if ( !pOwner )
		return;

	if ( !CanAttack() )
	{
		LowerBow();
	}

	// If we just fired, and we're past the point at which we tried to reload ourselves,
	// and we don't have any ammo in the clip, switch away to another weapon to stop us
	// from playing the "draw another arrow from the quiver" animation.
	if ( m_bReloadedThroughAnimEvent && m_iClip1 <= 0 && pOwner->GetAmmoCount( m_iPrimaryAmmoType ) <= 0 )
	{
		g_pGameRules->SwitchToNextBestWeapon( pOwner, this );
		return;
	}

	const bool bUseVRBowManualReload = ShouldUseVRBowManualReload();
	if ( bUseVRBowManualReload )
	{
		VRBowArrowPostFrame();
	}
	else
	{
		ResetVRBowArrowState();
	}

	int nSavedButtons = pOwner->m_nButtons;
	int nSavedPressed = pOwner->m_afButtonPressed;
	int nSavedReleased = pOwner->m_afButtonReleased;

	if ( bUseVRBowManualReload )
	{
		pOwner->m_nButtons &= ~( IN_ATTACK | IN_ATTACK2 );
		pOwner->m_afButtonPressed &= ~( IN_ATTACK | IN_ATTACK2 );
		pOwner->m_afButtonReleased &= ~( IN_ATTACK | IN_ATTACK2 );

		const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
		const bool bNockHeld = pCmd && ( m_bVRBowNockInputIsTrigger ? pCmd->vrBowArrowTriggerHold : pCmd->vrBowArrowGripHold );
		const bool bStringPulled = m_flVRBowArrowPull > 0.001f;
		const bool bCanPull = CanPullVRBowArrow();
		if ( m_bVRBowArrowNocked && bNockHeld && bStringPulled && bCanPull )
		{
			if ( GetInternalChargeBeginTime() <= 0.0f )
				pOwner->m_afButtonPressed |= IN_ATTACK;
			pOwner->m_nButtons |= IN_ATTACK;
		}
		else if ( m_bVRBowArrowNocked && bNockHeld )
		{
			StopVRBowChargeNoFire();
		}
		else if ( GetInternalChargeBeginTime() > 0.0f )
		{
			// Released the draw. If there's effectively no charge (string not
			// pulled / let go immediately), don't fire — cancel and return the
			// arrow to the backpack. Otherwise release IN_ATTACK to fire.
			if ( GetCurrentCharge() <= tfvr_huntsman_min_fire_charge.GetFloat() )
			{
				LowerBow();
				ResetVRBowArrowState();
			}
			else
			{
				pOwner->m_afButtonReleased |= IN_ATTACK;
			}
		}
	}

	BaseClass::ItemPostFrame();

	if ( bUseVRBowManualReload )
	{
		pOwner->m_nButtons = nSavedButtons;
		pOwner->m_afButtonPressed = nSavedPressed;
		pOwner->m_afButtonReleased = nSavedReleased;
	}

	if ( !(pOwner->m_nButtons & IN_ATTACK) && !(pOwner->m_nButtons & IN_ATTACK2) )
	{
		// Both buttons released. The player can draw the bow again.
		m_bNoFire = false;

		if ( GetActivity() == ACT_ITEM2_VM_PRIMARYATTACK && IsViewModelSequenceFinished() )
		{
			SendWeaponAnim( ACT_VM_IDLE );
		}
	}

	if ( GetCurrentCharge() == 1.f && IsViewModelSequenceFinished() )
	{
		SendWeaponAnim( ACT_VM_IDLE );
	}

	if ( m_bNoFire )
	{
		WeaponIdle();
	}
}

bool CTFCompoundBow::ShouldUseVRBowManualReload()
{
	if ( !tfvr_huntsman_manual_reload.GetBool() )
		return false;

	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !pOwner->IsInVRMode() )
		return false;

#ifdef CLIENT_DLL
	return IsHeldByVRHand() || pOwner->IsLocalPlayer();
#else
	return true;
#endif
}

bool CTFCompoundBow::HasVRBowArrowAmmo()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( m_iClip1 > 0 )
		return true;

	if ( m_iClip1 == -1 && pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0 )
		return true;

	return false;
}

bool CTFCompoundBow::HasVRBowArrowPrepAmmo()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return false;

	if ( HasVRBowArrowAmmo() )
		return true;

	return pOwner->GetAmmoCount( m_iPrimaryAmmoType ) > 0;
}

bool CTFCompoundBow::CanStartVRBowArrowGrab()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !ShouldUseVRBowManualReload() )
		return false;

	if ( m_bNoFire || GetInternalChargeBeginTime() > 0.0f )
		return false;

	if ( !HasVRBowArrowPrepAmmo() )
		return false;

	if ( gpGlobals->curtime < m_flNextVRBowArrowReadyTime )
		return false;

	return true;
}

bool CTFCompoundBow::CanPullVRBowArrow()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !ShouldUseVRBowManualReload() )
		return false;

	if ( m_bNoFire )
		return false;

	if ( !HasVRBowArrowAmmo() )
		return false;

	if ( gpGlobals->curtime < m_flNextPrimaryAttack || gpGlobals->curtime < m_flNextVRBowArrowReadyTime )
		return false;

	return true;
}

float CTFCompoundBow::GetVRBowArrowNockProgress() const
{
	if ( !m_bVRBowArrowNockActive )
		return m_bVRBowArrowNocked ? 1.0f : 0.0f;

	const float flDuration = MAX( tfvr_huntsman_nock_duration.GetFloat(), 0.01f );
	return clamp( ( gpGlobals->curtime - m_flVRBowArrowNockStartTime ) / flDuration, 0.0f, 1.0f );
}

void CTFCompoundBow::ResetVRBowArrowState()
{
	m_bVRBowArrowHeld = false;
	m_bVRBowArrowNockActive = false;
	m_bVRBowArrowNocked = false;
	m_flVRBowArrowNockStartTime = 0.0f;
	m_bVRBowNockInputIsTrigger = false;
	m_flVRBowArrowPull = 0.0f;
	ResetVRBowPullSoundState();
}

void CTFCompoundBow::VRStartBowArrowNock( bool bNockInputIsTrigger )
{
	if ( !m_bVRBowArrowHeld || m_bVRBowArrowNockActive || m_bVRBowArrowNocked )
		return;

	if ( !CanStartVRBowArrowGrab() )
		return;

	m_bVRBowArrowNockActive = true;
	m_flVRBowArrowNockStartTime = gpGlobals->curtime;
	m_bVRBowNockInputIsTrigger = bNockInputIsTrigger;
}

void CTFCompoundBow::VRFinishBowArrowNock()
{
	if ( !m_bVRBowArrowNockActive )
		return;

	m_bVRBowArrowHeld = false;
	m_bVRBowArrowNockActive = false;
	m_bVRBowArrowNocked = true;
	m_flVRBowArrowPull = 0.0f;
	ResetVRBowPullSoundState();
}

void CTFCompoundBow::ResetVRBowPullSoundState()
{
	m_flVRBowLastPhysicalPullForSound = 0.0f;
	for ( int i = 0; i < ARRAYSIZE( m_flVRBowPullSoundSamples ); i++ )
		m_flVRBowPullSoundSamples[i] = 0.0f;
	m_iVRBowPullSoundSampleCount = ARRAYSIZE( m_flVRBowPullSoundSamples );
	m_iVRBowPullSoundSampleIndex = 0;
	m_flNextVRBowPullSoundTime = 0.0f;
	m_flVRBowPullSoundLastMoveTime = 0.0f;
	m_flVRBowPullSoundPendingMove = 0.0f;
	m_iVRBowPullSoundPendingDirection = 0;
	m_iVRBowPullSoundDirection = 0;
}

void CTFCompoundBow::PlayVRBowPullSound( const char *pszSoundName )
{
#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
	{
		StopSound( "VR.CompoundBowPull" );
		StopSound( "VR.CompoundBowPullShort" );
		StopSound( "VR.CompoundBowPullReverse" );
		EmitSound( pszSoundName );
	}
#endif
}

void CTFCompoundBow::PlayVRBowArrowGrabSound()
{
#ifdef CLIENT_DLL
	if ( prediction->IsFirstTimePredicted() )
		EmitSound( "VR.CompoundBowArrowGrab" );
#endif
}

void CTFCompoundBow::UpdateVRBowPullSound( float flPhysicalPull )
{
	const float flFrameDelta = flPhysicalPull - m_flVRBowLastPhysicalPullForSound;
	m_flVRBowLastPhysicalPullForSound = flPhysicalPull;

	if ( m_iVRBowPullSoundSampleCount <= 0 )
	{
		for ( int i = 0; i < ARRAYSIZE( m_flVRBowPullSoundSamples ); i++ )
			m_flVRBowPullSoundSamples[i] = flPhysicalPull;
		m_iVRBowPullSoundSampleCount = 1;
		m_iVRBowPullSoundSampleIndex = 1 % ARRAYSIZE( m_flVRBowPullSoundSamples );
		return;
	}

	const int iOldestSample = m_iVRBowPullSoundSampleIndex;
	const float flWindowStartPull = m_iVRBowPullSoundSampleCount >= ARRAYSIZE( m_flVRBowPullSoundSamples )
		? m_flVRBowPullSoundSamples[iOldestSample]
		: m_flVRBowPullSoundSamples[0];
	m_flVRBowPullSoundSamples[m_iVRBowPullSoundSampleIndex] = flPhysicalPull;
	m_iVRBowPullSoundSampleIndex = ( m_iVRBowPullSoundSampleIndex + 1 ) % ARRAYSIZE( m_flVRBowPullSoundSamples );
	m_iVRBowPullSoundSampleCount = MIN( m_iVRBowPullSoundSampleCount + 1, ARRAYSIZE( m_flVRBowPullSoundSamples ) );

	const float flDelta = flPhysicalPull - flWindowStartPull;
	const float flThreshold = MAX( tfvr_huntsman_pull_sound_delta.GetFloat(), 0.0f );
	const float flMovementThreshold = MAX( flThreshold * 0.1f, 0.001f );
	const int iFrameDirection = flFrameDelta > flMovementThreshold ? 1 : ( flFrameDelta < -flMovementThreshold ? -1 : 0 );
	if ( iFrameDirection != 0 )
	{
		m_flVRBowPullSoundLastMoveTime = gpGlobals->curtime;
		if ( m_iVRBowPullSoundPendingDirection != iFrameDirection )
		{
			m_iVRBowPullSoundPendingDirection = iFrameDirection;
			m_flVRBowPullSoundPendingMove = 0.0f;
		}
		m_flVRBowPullSoundPendingMove += fabsf( flFrameDelta );
	}

	if ( m_iVRBowPullSoundDirection != 0
		&& gpGlobals->curtime - m_flVRBowPullSoundLastMoveTime >= MAX( tfvr_huntsman_pull_sound_settle_time.GetFloat(), 0.0f ) )
	{
		m_iVRBowPullSoundDirection = 0;
		m_iVRBowPullSoundPendingDirection = 0;
		m_flVRBowPullSoundPendingMove = 0.0f;
	}

	const float flMinTravel = MAX( tfvr_huntsman_pull_sound_min_travel.GetFloat(), flThreshold );
	if ( gpGlobals->curtime >= m_flNextVRBowPullSoundTime )
	{
		if ( flPhysicalPull > flThreshold
			&& m_iVRBowPullSoundPendingDirection == 1
			&& m_flVRBowPullSoundPendingMove >= flMinTravel
			&& m_iVRBowPullSoundDirection <= 0 )
		{
			const char *pszSound = flDelta >= tfvr_huntsman_pull_sound_full_delta.GetFloat()
				? "VR.CompoundBowPull"
				: "VR.CompoundBowPullShort";
			PlayVRBowPullSound( pszSound );
			m_iVRBowPullSoundDirection = 1;
			m_iVRBowPullSoundPendingDirection = 0;
			m_flVRBowPullSoundPendingMove = 0.0f;
			m_flNextVRBowPullSoundTime = gpGlobals->curtime + MAX( tfvr_huntsman_pull_sound_cooldown.GetFloat(), 0.0f );
		}
		else if ( m_iVRBowPullSoundPendingDirection == -1
			&& m_flVRBowPullSoundPendingMove >= flMinTravel
			&& m_iVRBowPullSoundDirection >= 0 )
		{
			PlayVRBowPullSound( "VR.CompoundBowPullReverse" );
			m_iVRBowPullSoundDirection = -1;
			m_iVRBowPullSoundPendingDirection = 0;
			m_flVRBowPullSoundPendingMove = 0.0f;
			m_flNextVRBowPullSoundTime = gpGlobals->curtime + MAX( tfvr_huntsman_pull_sound_cooldown.GetFloat(), 0.0f );
		}
	}

	if ( flPhysicalPull <= flThreshold )
	{
		m_iVRBowPullSoundDirection = 0;
		m_iVRBowPullSoundPendingDirection = 0;
		m_flVRBowPullSoundPendingMove = 0.0f;
	}
}

void CTFCompoundBow::UpdateVRBowArrowPull( float flTargetPull )
{
	const float flPhysicalPull = clamp( flTargetPull, 0.0f, 1.0f );
	UpdateVRBowPullSound( flPhysicalPull );

	// De-pulling should feel free and immediate. Re-pulling in the same held
	// session is resisted at the same rate as the bow's base charge delay, so
	// the string cannot jump back to full draw just because it had previously
	// reached that point.
	if ( flPhysicalPull <= m_flVRBowArrowPull )
	{
		m_flVRBowArrowPull = flPhysicalPull;
		return;
	}

	const float flChargeMaxTime = MAX( GetChargeMaxTime(), 0.01f );
	const float flMaxPullDelta = gpGlobals->frametime / flChargeMaxTime;
	m_flVRBowArrowPull = MIN( flPhysicalPull, m_flVRBowArrowPull + flMaxPullDelta );
}

void CTFCompoundBow::StopVRBowChargeNoFire()
{
	if ( GetInternalChargeBeginTime() <= 0.0f )
		return;

	SetInternalChargeBeginTime( 0.0f );

	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
		pPlayer->TeamFortress_SetSpeed();
	}
}

void CTFCompoundBow::VRBowArrowPostFrame()
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
	{
		ResetVRBowArrowState();
		return;
	}

	if ( !HasVRBowArrowPrepAmmo() )
	{
		ResetVRBowArrowState();
		return;
	}

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
		return;

	const bool bGripHeld = pCmd->vrBowArrowGripHold;
	const bool bTriggerHeld = pCmd->vrBowArrowTriggerHold;
	const bool bAnyHeld = bGripHeld || bTriggerHeld;

	if ( m_bVRBowArrowNockActive )
	{
		const bool bNockHeld = m_bVRBowNockInputIsTrigger ? bTriggerHeld : bGripHeld;
		if ( !bNockHeld )
		{
			ResetVRBowArrowState();
			return;
		}

		if ( GetVRBowArrowNockProgress() >= 1.0f )
			VRFinishBowArrowNock();

		return;
	}

	if ( m_bVRBowArrowNocked )
	{
		UpdateVRBowArrowPull( CanPullVRBowArrow() ? pCmd->vrBowArrowPull01 : 0.0f );

		const bool bNockHeld = m_bVRBowNockInputIsTrigger ? bTriggerHeld : bGripHeld;
		if ( !bNockHeld && GetInternalChargeBeginTime() <= 0.0f )
			ResetVRBowArrowState();
		return;
	}

	if ( m_bVRBowArrowHeld )
	{
		if ( !bAnyHeld )
		{
			ResetVRBowArrowState();
			return;
		}

		if ( pCmd->vrBowArrowNock )
			VRStartBowArrowNock( pCmd->vrBowArrowNockIsTrigger );

		return;
	}

	if ( pCmd->vrBowArrowPull && bAnyHeld && CanStartVRBowArrowGrab() )
	{
		m_bVRBowArrowHeld = true;
		PlayVRBowArrowGrabSound();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Held the arrow drawn too long. Give up & play a fail animation.
//-----------------------------------------------------------------------------
void CTFCompoundBow::ForceLaunchGrenade( void ) 
{
	// LowerBow();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::GetProjectileFireSetup( CTFPlayer *pPlayer, Vector vecOffset, Vector *vecSrc, QAngle *angForward, bool bHitTeammates, float flEndDist )
{
	BaseClass::GetProjectileFireSetup( pPlayer, vecOffset, vecSrc, angForward, bHitTeammates, flEndDist );

	if ( pPlayer && ShouldUseVRBowManualReload() && m_bVRBowArrowNocked )
	{
		const CUserCmd *pCmd = pPlayer->GetCurrentUserCommand();
		if ( pCmd && pCmd->vrBowArrowAimOrigin != vec3_origin )
		{
			*vecSrc = pCmd->vrBowArrowAimOrigin;
			*angForward = pCmd->vrBowArrowAimAngles;
		}
	}

	float flTotalChargeTime = gpGlobals->curtime - GetInternalChargeBeginTime();
	if ( flTotalChargeTime >= TF_ARROW_MAX_CHARGE_TIME )
	{
		// We want to fire a really inaccurate shot.
		float frand = (float) rand() / VALVE_RAND_MAX;
		angForward->x += -6 + frand*12.f;
		frand = (float) rand() / VALVE_RAND_MAX;
		angForward->y += -6 + frand*12.f;
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFCompoundBow::ApplyRefireSpeedModifications( float &flBaseRef )
{
	CALL_ATTRIB_HOOK_FLOAT( flBaseRef, fast_reload );

	// Prototype hack
	CTFPlayer *pPlayer = ToTFPlayer( GetOwner() );
	if ( pPlayer )
	{
		int iMaster = 0;
		CALL_ATTRIB_HOOK_INT_ON_OTHER( pPlayer, iMaster, ability_master_sniper );
		if ( iMaster )
		{
			flBaseRef *= RemapValClamped( iMaster, 1, 2, 0.6f, 0.3f );
		}
		else if ( pPlayer->m_Shared.GetCarryingRuneType() == RUNE_HASTE )
		{
			flBaseRef *= 0.4f;
		}
	}
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::StartBurningEffect( void )
{
	// clear any old effect before adding a new one
	if ( m_pBurningArrowEffect )
	{
		StopBurningEffect();
	}

	const char *pszEffect;
	m_hParticleEffectOwner = GetWeaponForEffect();
	if ( m_hParticleEffectOwner )
	{
		if ( m_hParticleEffectOwner != this )
		{
			// We're on the viewmodel
			pszEffect = "v_flaming_arrow";
		}
		else
		{
			pszEffect = "flaming_arrow";
		}

		m_pBurningArrowEffect = m_hParticleEffectOwner->ParticleProp()->Create( pszEffect, PATTACH_POINT_FOLLOW, COMPOUND_BOW_ATTACHMENT_POINT );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::StopBurningEffect( void )
{
	if ( m_pBurningArrowEffect )
	{
		if ( m_hParticleEffectOwner && m_hParticleEffectOwner->ParticleProp() )
		{
			m_hParticleEffectOwner->ParticleProp()->StopEmission( m_pBurningArrowEffect );
		}

		m_pBurningArrowEffect = NULL;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::UpdateOnRemove( void )
{
	StopBurningEffect();
	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::OnDataChanged( DataUpdateType_t type )
{
	BaseClass::OnDataChanged( type );

	// Handle particle effect creation / destruction
	if ( m_bArrowAlight && !m_pBurningArrowEffect )
	{
		if ( GetBaseAnimating()->LookupAttachment( COMPOUND_BOW_ATTACHMENT_POINT ) != INVALID_PARTICLE_ATTACHMENT )
		{
			StartBurningEffect();
			EmitSound( "ArrowLight" );
		}
	}
	else if ( !m_bArrowAlight && m_pBurningArrowEffect )
	{
		StopBurningEffect();
	}
}
#else

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFCompoundBow::GetInitialAfterburnDuration() const
{
	// if the bow is lighting someone on fire it must have
	// been the arrow was lit before it was fired
	return 7.5f;
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFCompoundBow::Reload( void )
{
	if ( m_flNextPrimaryAttack > gpGlobals->curtime )
		return false;
	return BaseClass::Reload();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFCompoundBow::CalcIsAttackCriticalHelper()
{ 
	CTFPlayer *pPlayer = GetTFPlayerOwner();

	// Crit boosted players fire all crits
	if ( pPlayer && pPlayer->m_Shared.IsCritBoosted() )
		return true;

	return false; 
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFCompoundBow::SetArrowAlight( bool bAlight ) 
{ 
	// Don't light arrows if we're still firing one.
	if (GetActivity() != ACT_ITEM2_VM_PRIMARYATTACK ) 
	{
		m_bArrowAlight = bAlight; 
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFCompoundBow::OwnerCanJump( void )
{
	return GetInternalChargeBeginTime() == 0.f;
}
