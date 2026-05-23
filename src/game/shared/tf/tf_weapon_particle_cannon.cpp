//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF Rocket Launcher
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_particle_cannon.h"
#include "tf_fx_shared.h"
#include "in_buttons.h"
#include "usercmd.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include <vgui_controls/Panel.h>
#include <vgui/ISurface.h>
#include "soundenvelope.h"
#include "particle_property.h"
#include "c_tf_gamestats.h"
#include "prediction.h"
// Server specific.
#else
#include "tf_gamestats.h"
#include "tf_player.h"
#include "tf_projectile_energy_ball.h"
#endif

//=============================================================================
//
// Particle cannon tables.
//
IMPLEMENT_NETWORKCLASS_ALIASED( TFParticleCannon, DT_ParticleCannon )

ConVar tfvr_mangler_pump_reload( "tfvr_mangler_pump_reload", "1", FCVAR_ARCHIVE, "VR: enable physical pump reload for Cow Mangler" );
ConVar tfvr_mangler_pump_distance( "tfvr_mangler_pump_distance", "10.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units per pump stroke" );
ConVar tfvr_mangler_pump_sign( "tfvr_mangler_pump_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply pump-axis motion (+1 or -1)" );
ConVar tfvr_mangler_pump_debug( "tfvr_mangler_pump_debug", "0", FCVAR_REPLICATED, "VR: 1 = print mangler pump state to console" );

BEGIN_NETWORK_TABLE( CTFParticleCannon, DT_ParticleCannon )
#ifdef CLIENT_DLL
	RecvPropFloat( RECVINFO( m_flChargeBeginTime ) ),
	RecvPropInt( RECVINFO( m_iChargeEffect ) ),
	RecvPropBool( RECVINFO( m_bVRPumpIsArmed ) ),
	RecvPropVector( RECVINFO( m_vecVRPumpLastHandPos ) ),
	RecvPropInt( RECVINFO( m_iVRPumpPhase ) ),
	RecvPropFloat( RECVINFO( m_flVRPumpStrokeDist ) ),
	RecvPropFloat( RECVINFO( m_flNextVRPumpRechargeTime ) ),
#else
	SendPropFloat( SENDINFO( m_flChargeBeginTime ) ),
	SendPropInt( SENDINFO( m_iChargeEffect ) ),
	SendPropBool( SENDINFO( m_bVRPumpIsArmed ) ),
	SendPropVector( SENDINFO( m_vecVRPumpLastHandPos ) ),
	SendPropInt( SENDINFO( m_iVRPumpPhase ) ),
	SendPropFloat( SENDINFO( m_flVRPumpStrokeDist ) ),
	SendPropFloat( SENDINFO( m_flNextVRPumpRechargeTime ) ),
#endif
END_NETWORK_TABLE()

#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA( CTFParticleCannon )
	DEFINE_PRED_FIELD( m_bVRPumpIsArmed, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_vecVRPumpLastHandPos, FIELD_VECTOR, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_iVRPumpPhase, FIELD_INTEGER, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRPumpStrokeDist, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flNextVRPumpRechargeTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
END_PREDICTION_DATA()
#endif

LINK_ENTITY_TO_CLASS( tf_weapon_particle_cannon, CTFParticleCannon );
PRECACHE_WEAPON_REGISTER( tf_weapon_particle_cannon );

// Server specific.
#ifndef CLIENT_DLL
BEGIN_DATADESC( CTFParticleCannon )
END_DATADESC()
#endif

#ifdef GAME_DLL
const float tf_particle_cannon_afterburn_rate = 6.f;
#endif // GAME_DLL

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CTFParticleCannon::CTFParticleCannon() : CTFRocketLauncher()
{
#ifdef CLIENT_DLL
	m_bEffectsThinking = false;
	m_iChargeEffectBase = 0;
#endif

	m_bVRPumpIsArmed = false;
	m_vecVRPumpLastHandPos.Init();
	m_iVRPumpPhase = 0;
	m_flVRPumpStrokeDist = 0.0f;
	m_flNextVRPumpRechargeTime = 0.0f;
	m_iVRPumpSoundVariant = 0;
#ifdef CLIENT_DLL
	m_iVRPumpLastEmittedPhase = 0;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFParticleCannon::GetProjectileSpeed( void )
{
	return 1100.f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFParticleCannon::GetProjectileGravity( void )
{
	return 0.f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFParticleCannon::IsViewModelFlipped( void )
{
	return !BaseClass::IsViewModelFlipped(); // Invert because arrows are backwards by default.
}

//-----------------------------------------------------------------------------
// Purpose: Reset the charge when we holster
//-----------------------------------------------------------------------------
bool CTFParticleCannon::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer && pPlayer->m_Shared.InCond( TF_COND_AIMING ) && !pPlayer->IsRegenerating() )
		return false;

	m_flChargeBeginTime = 0;

	if ( pPlayer )
	{
		pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
		pPlayer->TeamFortress_SetSpeed();
	}

#ifdef CLIENT_DLL
	ParticleProp()->Init( this );
	ParticleProp()->StopParticlesNamed( "drg_cowmangler_idle", true );
	m_bEffectsThinking = false;
#endif

	StopSound( "Weapon_CowMangler.Charging" );

	ResetVRPumpGestureState();
	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose: Reset the charge when we deploy
//-----------------------------------------------------------------------------
bool CTFParticleCannon::Deploy( void )
{
	m_flChargeBeginTime = 0;

#ifdef CLIENT_DLL
	m_bEffectsThinking = true;
	SetContextThink( &CTFParticleCannon::ClientEffectsThink, gpGlobals->curtime + rand() % 5, "PC_EFFECTS_THINK" );
#endif

	ResetVRPumpGestureState();
	return BaseClass::Deploy();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::WeaponReset( void )
{
	BaseClass::WeaponReset();

	m_flChargeBeginTime = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();

	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
	{
		VRPumpReloadPostFrame();
	}

	if ( m_flChargeBeginTime > 0 )
	{
		CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
		if ( !pPlayer )
			return;

		// If we're not holding down the attack button, launch our grenade
		float flTotalChargeTime = gpGlobals->curtime - m_flChargeBeginTime;
		if ( flTotalChargeTime >= GetChargeForceReleaseTime() )
		{
			FireChargedShot();
		}
	}

#ifdef CLIENT_DLL

	if ( !m_bEffectsThinking )
	{
		m_bEffectsThinking = true;
		SetContextThink( &CTFParticleCannon::ClientEffectsThink, gpGlobals->curtime + rand() % 5, "PC_EFFECTS_THINK" );
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::PrimaryAttack( void )
{
	if ( m_flChargeBeginTime > 0 )
		return;

	if ( !Energy_HasEnergy() )
		return;

	m_bChargedShot = false;
	BaseClass::PrimaryAttack();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::SecondaryAttack( void )
{
	// Check for ammunition.
	if ( !Energy_FullyCharged() )
	{
		Reload();
		return;
	}

	// Are we capable of firing again?
	if ( m_flNextPrimaryAttack > gpGlobals->curtime )
		return;

	if ( m_flChargeBeginTime > 0 )
		return;

	if ( !CanAttack() )
	{
		m_flChargeBeginTime = 0;
		return;
	}

	m_bChargedShot = true;

	// Set the weapon mode.
	m_iWeaponMode = TF_WEAPON_PRIMARY_MODE;

	// save that we had the attack button down
	m_flChargeBeginTime = gpGlobals->curtime;

	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		SendWeaponAnim( ACT_PRIMARY_VM_PRIMARYATTACK_3 );
		pPlayer->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_PRIMARY_SUPER );
	}

	WeaponSound( SPECIAL1 );

	pPlayer->m_Shared.AddCond( TF_COND_AIMING );
	pPlayer->TeamFortress_SetSpeed();

	m_iChargeEffect++;
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::CreateChargeEffect()
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( pPlayer )
	{
		DispatchParticleEffect( "drg_cowmangler_muzzleflash_chargeup", PATTACH_POINT_FOLLOW, GetAppropriateWorldOrViewModel(), "muzzle", GetParticleColor( 1 ), GetParticleColor( 2 ) );
	}
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::FireChargedShot()
{
	CTFPlayer *pPlayer = ToTFPlayer( GetPlayerOwner() );
	if ( !pPlayer )
		return;

	if ( !pPlayer->IsAlive() )
		return;

	pPlayer->m_Shared.RemoveCond( TF_COND_AIMING );
	pPlayer->TeamFortress_SetSpeed();

#ifndef CLIENT_DLL
	CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, false );
#else
	C_CTF_GameStats.Event_PlayerFiredWeapon( pPlayer, false );
#endif

//	SendWeaponAnim( ACT_VM_PRIMARYATTACK );
//	pPlayer->SetAnimation( PLAYER_ATTACK1 );

	CBaseEntity* pProj = FireProjectile( pPlayer );
	ModifyProjectile( pProj );

	float flFireDelay = ApplyFireDelay( m_pWeaponInfo->GetWeaponData( m_iWeaponMode ).m_flTimeFireDelay );

	m_flNextPrimaryAttack = gpGlobals->curtime + flFireDelay;

	SetWeaponIdleTime( gpGlobals->curtime + SequenceDuration() );

	m_iReloadMode.Set( TF_RELOAD_START );

	m_flChargeBeginTime = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::ModifyProjectile( CBaseEntity* pProj )
{
#ifdef GAME_DLL
	CTFProjectile_EnergyBall* pEnergyBall = dynamic_cast<CTFProjectile_EnergyBall*>( pProj );
	if ( pEnergyBall == NULL )
		return;

	pEnergyBall->SetChargedShot( m_bChargedShot );
	pEnergyBall->SetColor( 1, GetParticleColor( 1 ) );
	pEnergyBall->SetColor( 2, GetParticleColor( 2 ) );
#endif

	if ( m_bChargedShot )
	{
		Energy_DrainEnergy( Energy_GetMaxEnergy() );
	}
	else
	{
		Energy_DrainEnergy();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFParticleCannon::GetProgress( void )
{
	return Energy_GetEnergy() / Energy_GetMaxEnergy();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
const char *CTFParticleCannon::GetMuzzleFlashParticleEffect( void )
{
	if ( m_bChargedShot )
	{
		return ( GetTeamNumber() == TF_TEAM_RED ) ? "drg_cow_muzzleflash_charged" : "drg_cow_muzzleflash_charged_blue";
	}
	else
	{
		return ( GetTeamNumber() == TF_TEAM_RED ) ? "drg_cow_muzzleflash_normal" : "drg_cow_muzzleflash_normal_blue";
	}
}

#ifndef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::Precache()
{
	BaseClass::Precache();

	PrecacheParticleSystem( "drg_cow_explosioncore_charged" );
	PrecacheParticleSystem( "drg_cow_explosioncore_charged_blue" );
	PrecacheParticleSystem( "drg_cow_explosioncore_normal" );
	PrecacheParticleSystem( "drg_cow_explosioncore_normal_blue" );
	PrecacheParticleSystem( "drg_cow_muzzleflash_charged" );
	PrecacheParticleSystem( "drg_cow_muzzleflash_charged_blue" );
	PrecacheParticleSystem( "drg_cow_muzzleflash_normal" );
	PrecacheParticleSystem( "drg_cow_muzzleflash_normal_blue" );
	PrecacheParticleSystem( "drg_cow_idle" );

	PrecacheScriptSound( "Weapon_CowMangler.ReloadFinal" );

	for ( int i = 1; i <= 4; i++ )
	{
		PrecacheScriptSound( UTIL_VarArgs( "VR.ManglerPumpUp%02d", i ) );
		PrecacheScriptSound( UTIL_VarArgs( "VR.ManglerPumpDown%02d", i ) );
	}
	PrecacheScriptSound( "VR.ManglerPumpUpFinal" );
	PrecacheScriptSound( "VR.ManglerPumpDownFinal" );
}
#endif

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::OnDataChanged( DataUpdateType_t updateType )
{
	BaseClass::OnDataChanged( updateType );

	if ( IsCarrierAlive() && ( WeaponState() == WEAPON_IS_ACTIVE ) )
	{
		if ( m_iChargeEffect != m_iChargeEffectBase )
		{
			CreateChargeEffect();
			m_iChargeEffectBase = m_iChargeEffect;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::ClientEffectsThink( void )
{
	CTFPlayer *pPlayer = GetTFPlayerOwner();
	if ( !pPlayer )
		return;

	if ( !pPlayer->IsLocalPlayer() )
		return;

	if ( !pPlayer->GetViewModel() )
		return;

	if ( !m_bEffectsThinking )
		return;

	SetContextThink( &CTFParticleCannon::ClientEffectsThink, gpGlobals->curtime + 2 + rand() % 5, "PC_EFFECTS_THINK" );

	if ( pPlayer->m_Shared.InCond( TF_COND_TAUNTING ) )
		return;

	const char* mounts[4] =
	{
		"crit_frontspark1",
		"crit_frontspark2",
		"crit_frontspark3",
		"crit_frontspark4"
	};

	int iPoint = rand() % 4;

	C_BaseAnimating *pEffectEnt = GetAppropriateWorldOrViewModel();
	if ( !pEffectEnt )
		pEffectEnt = this;
	pEffectEnt->ParticleProp()->Init( pEffectEnt );
	const char *pszIdleParticle = ( GetTeamNumber() == TF_TEAM_RED ) ? "drg_cow_idle" : "drg_cow_idle_blue";
	CNewParticleEffect* pEffect = pEffectEnt->ParticleProp()->Create( pszIdleParticle, PATTACH_POINT_FOLLOW, mounts[iPoint] );
	if ( pEffect )
	{
		pEffect->SetControlPoint( CUSTOM_COLOR_CP1, GetParticleColor( 1 ) );
		pEffect->SetControlPoint( CUSTOM_COLOR_CP2, GetParticleColor( 2 ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::DispatchMuzzleFlash( const char* effectName, C_BaseEntity* pAttachEnt )
{
	DispatchParticleEffect( effectName, PATTACH_POINT_FOLLOW, pAttachEnt, "muzzle", GetParticleColor( 1 ), GetParticleColor( 2 ) );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::CreateMuzzleFlashEffects( C_BaseEntity *pAttachEnt, int nIndex )
{
	// Don't call direct parent. We don't want back blast effects.
	CTFWeaponBaseGun::CreateMuzzleFlashEffects( pAttachEnt, nIndex );
}

#endif


//-----------------------------------------------------------------------------
// Purpose: Utility function for default colors.
//-----------------------------------------------------------------------------
Vector GetParticleColorForTeam( int iTeam, int iColor )
{
	if ( iColor == 1 )
	{
		if ( iTeam == TF_TEAM_RED )
			return TF_PARTICLE_WEAPON_RED_1;
		else
			return TF_PARTICLE_WEAPON_BLUE_1;
	}
	else
	{
		if ( iTeam == TF_TEAM_RED )
			return TF_PARTICLE_WEAPON_RED_2;
		else
			return TF_PARTICLE_WEAPON_BLUE_2;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFParticleCannon::PlayWeaponShootSound( void )
{
	if ( m_bChargedShot )
	{
//		WeaponSound( BURST );
	}
	else
	{
		WeaponSound( SINGLE );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
char const *CTFParticleCannon::GetShootSound( int iIndex ) const
{
	if ( iIndex == RELOAD )
	{
		if ( ShouldSuppressAutoAndSinglyReloadForVR() )
			return NULL;

		bool bLastReload = (Energy_GetEnergy()+Energy_GetRechargeCost()) == Energy_GetMaxEnergy();
		if ( bLastReload )
		{
			return "Weapon_CowMangler.ReloadFinal";
		}
	}

	return BaseClass::GetShootSound(iIndex);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFParticleCannon::OwnerCanTaunt( void )
{
	if ( m_flChargeBeginTime > 0 )
	{
		return false;
	}
	else
	{
		return true;
	}
}


#ifdef GAME_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFParticleCannon::GetAfterburnRateOnHit() const
{
	return tf_particle_cannon_afterburn_rate;
}
#endif // GAME_DLL

//-----------------------------------------------------------------------------
// VR pump reload (3-stroke: up → down → return)
//-----------------------------------------------------------------------------
extern ConVar tfvr_reload_throttle_scale;

static inline float TFVR_ManglerReloadThrottleScale()
{
	return MAX( 1.0f, tfvr_reload_throttle_scale.GetFloat() );
}

bool CTFParticleCannon::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	if ( GetWeaponID() != TF_WEAPON_PARTICLE_CANNON )
		return false;

	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner || !pOwner->IsInVRMode() )
		return false;

#ifdef CLIENT_DLL
	if ( !tfvr_mangler_pump_reload.GetBool() )
		return false;
	return IsHeldByVRHand();
#else
	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd || !pCmd->vrManualPumpReload )
		return false;
	return true;
#endif
}

bool CTFParticleCannon::Reload( void )
{
	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
		return false;

	return BaseClass::Reload();
}

void CTFParticleCannon::ResetVRPumpGestureState( void )
{
	m_bVRPumpIsArmed = false;
	m_vecVRPumpLastHandPos = vec3_origin;
	m_iVRPumpPhase = 0;
	m_flVRPumpStrokeDist = 0.0f;
	m_iVRPumpSoundVariant = 0;
#ifdef CLIENT_DLL
	m_iVRPumpLastEmittedPhase = 0;
#endif
}

float CTFParticleCannon::GetVRPumpStrokeProgress() const
{
	float dist = tfvr_mangler_pump_distance.GetFloat();
	return ( dist > 0.0f ) ? clamp( (float)m_flVRPumpStrokeDist / dist, 0.0f, 1.0f ) : 0.0f;
}

void CTFParticleCannon::VRCommitPumpRecharge( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

	const CUserCmd *pCmdCommit = pOwner->GetCurrentUserCommand();
	if ( !pCmdCommit || !pCmdCommit->vrWeaponArmed )
		return;

	if ( gpGlobals->curtime < m_flNextVRPumpRechargeTime )
		return;

	if ( Energy_FullyCharged() )
		return;

	Energy_Recharge();
	m_flNextVRPumpRechargeTime = gpGlobals->curtime + GetVRSinglyReloadShellThrottleInterval() * TFVR_ManglerReloadThrottleScale();
}

//-----------------------------------------------------------------------------
// Three-stroke gesture along weapon forward:
//   Phase 1 (up): positive axis motion
//   Phase 2 (down): negative axis motion
//   Phase 3 (return up): positive axis motion, then commit recharge
//-----------------------------------------------------------------------------
void CTFParticleCannon::VRPumpReloadPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

#if defined( CLIENT_DLL )
	if ( !pOwner->IsLocalPlayer() )
		return;
#endif

	const bool bDebug = tfvr_mangler_pump_debug.GetBool();

	const CUserCmd *pCmd = pOwner->GetCurrentUserCommand();
	if ( !pCmd )
	{
		ResetVRPumpGestureState();
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

	if ( Energy_FullyCharged() )
	{
		if ( bDebug && m_iVRPumpPhase != 0 )
			DevMsg( "[VR ManglerPump] Reset: fully charged\n" );
		ResetVRPumpGestureState();
		return;
	}

	if ( gpGlobals->curtime < m_flNextPrimaryAttack || m_flChargeBeginTime > 0 )
	{
		m_bVRPumpIsArmed = false;
		m_iVRPumpPhase = 0;
		m_flVRPumpStrokeDist = 0.0f;
		if ( m_vecVRPumpLastHandPos != vec3_origin )
			m_vecVRPumpLastHandPos = vecHandRelative;
		if ( bDebug )
			DevMsg( "[VR ManglerPump] Paused: fire/charge cooldown\n" );
		return;
	}

	if ( !pCmd->vrWeaponArmed )
	{
		const bool bMidStroke = ( m_iVRPumpPhase != 0 );
		if ( !bMidStroke )
		{
			if ( bDebug && m_vecVRPumpLastHandPos != vec3_origin )
				DevMsg( "[VR ManglerPump] Reset: not armed\n" );
			ResetVRPumpGestureState();
		}
		else
		{
			m_vecVRPumpLastHandPos = vecHandRelative;
			m_flVRPumpStrokeDist = 0.0f;
			if ( bDebug )
				DevMsg( "[VR ManglerPump] Grip lost mid-stroke, holding state\n" );
		}
		return;
	}

	m_bVRPumpIsArmed = true;

	const float flPumpDist     = tfvr_mangler_pump_distance.GetFloat();
	const float flSign         = tfvr_mangler_pump_sign.GetFloat();
	const float flReloadInterval = GetVRSinglyReloadShellThrottleInterval() * TFVR_ManglerReloadThrottleScale();

	QAngle angWeaponHand = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerAngR
		: pCmd->vrRawControllerAngL;

	matrix3x4_t controllerMatrix;
	AngleMatrix( angWeaponHand, controllerMatrix );

	Vector vecPumpAxis;
	MatrixGetColumn( controllerMatrix, 2, vecPumpAxis );
	VectorNormalize( vecPumpAxis );

	if ( m_vecVRPumpLastHandPos == vec3_origin )
	{
		m_vecVRPumpLastHandPos = vecHandRelative;
		m_flVRPumpStrokeDist   = 0.0f;
		if ( bDebug )
			DevMsg( "[VR ManglerPump] Tracking started\n" );
		return;
	}

	// Neutral state: detect initial upward motion to enter phase 1
	if ( m_iVRPumpPhase == 0 )
	{
		if ( m_vecVRPumpLastHandPos != vec3_origin )
		{
			Vector vecFrameDelta = vecHandRelative - m_vecVRPumpLastHandPos;
			float  flFrameDisp   = DotProduct( vecFrameDelta, vecPumpAxis ) * flSign;

			if ( flFrameDisp > 0.0f )
			{
				m_flVRPumpStrokeDist += flFrameDisp;
			}
			else
			{
				m_flVRPumpStrokeDist = MAX( m_flVRPumpStrokeDist + flFrameDisp * 2.0f, 0.0f );
			}

			if ( m_flVRPumpStrokeDist >= 0.5f )
			{
				m_iVRPumpPhase = 1;
				m_vecVRPumpLastHandPos = vecHandRelative;
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist - 0.5f, flPumpDist );

				// Pick sound variant: "final" if this recharge will fill the bar
				bool bLastReload = ( Energy_GetEnergy() + Energy_GetRechargeCost() >= Energy_GetMaxEnergy() );
				m_iVRPumpSoundVariant = bLastReload ? 0 : ( 1 + ( rand() % 4 ) );

				if ( bDebug )
					DevMsg( "[VR ManglerPump] Phase 1 (up) started, variant=%d\n", m_iVRPumpSoundVariant );
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

		float flMinStrokeTime = MAX( flReloadInterval * 0.5f, 0.05f );
		float flMaxDistPerFrame = ( flPumpDist / flMinStrokeTime ) * gpGlobals->frametime;

		if ( m_iVRPumpPhase == 1 )
		{
			// Phase 1: pump UP (positive axis)
			if ( flFrameDisp > 0.0f )
			{
				m_flVRPumpStrokeDist += flFrameDisp;
			}

			if ( m_flVRPumpStrokeDist >= flPumpDist * 0.60f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + flMaxDistPerFrame, flPumpDist );
			}
			m_vecVRPumpLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR ManglerPump] Up: %.2f / %.2f\n", (float)m_flVRPumpStrokeDist, flPumpDist );

			if ( m_flVRPumpStrokeDist >= flPumpDist )
			{
				m_iVRPumpPhase = 2;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR ManglerPump] Phase 1 complete, entering phase 2 (down)\n" );

#ifdef CLIENT_DLL
				if ( m_iVRPumpLastEmittedPhase != 2 )
				{
					m_iVRPumpLastEmittedPhase = 2;
					if ( m_iVRPumpSoundVariant == 0 )
					{
						EmitSound( "VR.ManglerPumpUpFinal" );
					}
					else
					{
						char szSound[64];
						Q_snprintf( szSound, sizeof(szSound), "VR.ManglerPumpUp%02d", m_iVRPumpSoundVariant );
						EmitSound( szSound );
					}
				}
#endif
			}
		}
		else if ( m_iVRPumpPhase == 2 )
		{
			// Phase 2: pump DOWN (negative axis)
			if ( flFrameDisp < 0.0f )
			{
				m_flVRPumpStrokeDist += -flFrameDisp;
			}

			if ( m_flVRPumpStrokeDist >= flPumpDist * 0.60f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + flMaxDistPerFrame, flPumpDist );
			}
			m_vecVRPumpLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR ManglerPump] Down: %.2f / %.2f\n", (float)m_flVRPumpStrokeDist, flPumpDist );

			if ( m_flVRPumpStrokeDist >= flPumpDist )
			{
				m_iVRPumpPhase = 3;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR ManglerPump] Phase 2 complete, entering phase 3 (return)\n" );

#ifdef CLIENT_DLL
				if ( m_iVRPumpLastEmittedPhase != 3 )
				{
					m_iVRPumpLastEmittedPhase = 3;
					if ( m_iVRPumpSoundVariant == 0 )
					{
						EmitSound( "VR.ManglerPumpDownFinal" );
					}
					else
					{
						char szSound[64];
						Q_snprintf( szSound, sizeof(szSound), "VR.ManglerPumpDown%02d", m_iVRPumpSoundVariant );
						EmitSound( szSound );
					}
				}
#endif
			}
		}
		else if ( m_iVRPumpPhase == 3 )
		{
			// Phase 3: return UP (positive axis), no sound, commit on completion
			float flReturnDist = flPumpDist * 0.9f;

			if ( flFrameDisp > 0.0f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + flFrameDisp, flReturnDist );
			}

			if ( m_flVRPumpStrokeDist >= flPumpDist * 0.60f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + flMaxDistPerFrame, flReturnDist );
			}
			m_vecVRPumpLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR ManglerPump] Return: %.2f / %.2f\n", (float)m_flVRPumpStrokeDist, flReturnDist );

			if ( m_flVRPumpStrokeDist >= flReturnDist
				&& gpGlobals->curtime >= m_flNextVRPumpRechargeTime )
			{
				VRCommitPumpRecharge();
				m_iVRPumpPhase = 0;
				m_flVRPumpStrokeDist = 0.0f;
				m_iVRPumpSoundVariant = 0;
#ifdef CLIENT_DLL
				m_iVRPumpLastEmittedPhase = 0;
#endif
				if ( bDebug )
					DevMsg( "[VR ManglerPump] Energy recharged! Ready for next pump.\n" );
			}
		}
	}
}
