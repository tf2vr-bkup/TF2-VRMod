//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#include "cbase.h"
#include "tf_weapon_raygun.h"
#include "tf_fx_shared.h"
#include "in_buttons.h"
#include "usercmd.h"

// Client specific.
#ifdef CLIENT_DLL
#include "c_tf_player.h"
#include "particle_property.h"
#include "prediction.h"
#else
#include "tf_player.h"
#include "ndebugoverlay.h"
#include "particle_parse.h"
#include "tf_fx.h"
#include "tf_gamestats.h"
#include "tf_projectile_energy_ring.h"
#endif


//============================

IMPLEMENT_NETWORKCLASS_ALIASED( TFRaygun, DT_WeaponRaygun )

ConVar tfvr_bison_pump_reload( "tfvr_bison_pump_reload", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: enable physical pump reload for Righteous Bison" );
ConVar tfvr_bison_pump_distance( "tfvr_bison_pump_distance", "4.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units per pump stroke" );
ConVar tfvr_bison_pump_sign( "tfvr_bison_pump_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply pump-axis motion (+1 or -1)" );
ConVar tfvr_bison_pump_debug( "tfvr_bison_pump_debug", "0", FCVAR_REPLICATED, "VR: 1 = print bison pump state to console" );

ConVar tfvr_pomson_pump_reload( "tfvr_pomson_pump_reload", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: enable physical pump reload for Pomson 6000" );
ConVar tfvr_pomson_pump_distance( "tfvr_pomson_pump_distance", "3.0", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: hammer units per pump stroke (Pomson)" );
ConVar tfvr_pomson_pump_sign( "tfvr_pomson_pump_sign", "1", FCVAR_REPLICATED | FCVAR_ARCHIVE, "VR: multiply pump-axis motion (+1 or -1) (Pomson)" );
ConVar tfvr_pomson_pump_debug( "tfvr_pomson_pump_debug", "0", FCVAR_REPLICATED, "VR: 1 = print pomson pump state to console" );

BEGIN_NETWORK_TABLE( CTFRaygun, DT_WeaponRaygun )
#ifdef GAME_DLL
	SendPropBool( SENDINFO( m_bUseNewProjectileCode ) ),
	SendPropBool( SENDINFO( m_bVRPumpIsArmed ) ),
	SendPropVector( SENDINFO( m_vecVRPumpLastHandPos ) ),
	SendPropBool( SENDINFO( m_bVRPumpStrokeOut ) ),
	SendPropBool( SENDINFO( m_bVRPumpStrokeIn ) ),
	SendPropFloat( SENDINFO( m_flVRPumpStrokeDist ) ),
	SendPropFloat( SENDINFO( m_flNextVRPumpRechargeTime ) ),
#else
	RecvPropBool( RECVINFO( m_bUseNewProjectileCode ) ),
	RecvPropBool( RECVINFO( m_bVRPumpIsArmed ) ),
	RecvPropVector( RECVINFO( m_vecVRPumpLastHandPos ) ),
	RecvPropBool( RECVINFO( m_bVRPumpStrokeOut ) ),
	RecvPropBool( RECVINFO( m_bVRPumpStrokeIn ) ),
	RecvPropFloat( RECVINFO( m_flVRPumpStrokeDist ) ),
	RecvPropFloat( RECVINFO( m_flNextVRPumpRechargeTime ) ),
#endif
END_NETWORK_TABLE()

#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA( CTFRaygun )
	DEFINE_PRED_FIELD( m_bVRPumpIsArmed, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_vecVRPumpLastHandPos, FIELD_VECTOR, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRPumpStrokeOut, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bVRPumpStrokeIn, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flVRPumpStrokeDist, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_flNextVRPumpRechargeTime, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
END_PREDICTION_DATA()
#endif

LINK_ENTITY_TO_CLASS( tf_weapon_raygun, CTFRaygun );
PRECACHE_WEAPON_REGISTER( tf_weapon_raygun );

//============================
IMPLEMENT_NETWORKCLASS_ALIASED( TFDRGPomson, DT_WeaponDRGPomson )

BEGIN_NETWORK_TABLE( CTFDRGPomson, DT_WeaponDRGPomson )
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFDRGPomson )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_drg_pomson, CTFDRGPomson );
PRECACHE_WEAPON_REGISTER( tf_weapon_drg_pomson );


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CTFRaygun::CTFRaygun()
{
	m_bUseNewProjectileCode = false;
#ifdef GAME_DLL
	m_bUseNewProjectileCode = true;
#endif
	m_flIrradiateTime = 0.f;
	m_bEffectsThinking = false;

	m_bVRPumpIsArmed = false;
	m_vecVRPumpLastHandPos.Init();
	m_bVRPumpStrokeOut = false;
	m_bVRPumpStrokeIn = false;
	m_flVRPumpStrokeDist = 0.0f;
	m_flNextVRPumpRechargeTime = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFRaygun::Precache()
{
	PrecacheParticleSystem( "drg_bison_impact" );
	PrecacheParticleSystem( "drg_bison_idle" );
	PrecacheParticleSystem( "drg_bison_muzzleflash" );

	PrecacheScriptSound( "VR.BisonPumpOut" );
	PrecacheScriptSound( "VR.BisonPumpIn" );

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
const char *CTFRaygun::GetMuzzleFlashParticleEffect( void )
{
	return "drg_bison_muzzleflash";
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::PrimaryAttack( void )
{
	if ( !Energy_HasEnergy() )
		return;

	BaseClass::PrimaryAttack();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::ModifyProjectile( CBaseEntity* pProj )
{
#ifdef GAME_DLL
	/*
	CTFProjectile_EnergyRing* pEnergyBall = dynamic_cast<CTFProjectile_EnergyRing*>( pProj );
	if ( pEnergyBall == NULL )
		return;

	pEnergyBall->SetColor( 1, GetParticleColor( 1 ) );
	pEnergyBall->SetColor( 2, GetParticleColor( 2 ) );
	*/
#endif

	Energy_DrainEnergy();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFRaygun::GetProgress( void )
{
	return Energy_GetEnergy() / Energy_GetMaxEnergy();
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::DispatchMuzzleFlash( const char* effectName, C_BaseEntity* pAttachEnt )
{
	DispatchParticleEffect( effectName, PATTACH_POINT_FOLLOW, pAttachEnt, "muzzle", GetParticleColor( 1 ), GetParticleColor( 2 ) );
}
#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFRaygun::Holster( CBaseCombatWeapon *pSwitchingTo )
{
#ifdef CLIENT_DLL
	m_bEffectsThinking = false;
#endif

	ResetVRPumpGestureState();
	return BaseClass::Holster( pSwitchingTo );
}

bool CTFRaygun::Reload( void )
{
	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
		return false;

	return BaseClass::Reload();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFRaygun::Deploy( void )
{
#ifdef CLIENT_DLL
	m_bEffectsThinking = true;
	SetContextThink( &CTFRaygun::ClientEffectsThink, gpGlobals->curtime + rand() % 5, "EFFECTS_THINK" );
#endif

	ResetVRPumpGestureState();
	return BaseClass::Deploy();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();

#ifdef CLIENT_DLL
	if ( !m_bEffectsThinking )
	{
		m_bEffectsThinking = true;
		SetContextThink( &CTFRaygun::ClientEffectsThink, gpGlobals->curtime + rand() % 5, "EFFECTS_THINK" );
	}
#endif

	if ( ShouldSuppressAutoAndSinglyReloadForVR() )
	{
		VRPumpReloadPostFrame();
	}
}

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFRaygun::ClientEffectsThink( void )
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

	SetContextThink( &CTFRaygun::ClientEffectsThink, gpGlobals->curtime + 2 + rand() % 5, "EFFECTS_THINK" );

	C_BaseAnimating *pAttachEnt = GetAppropriateWorldOrViewModel();
	if ( !pAttachEnt )
		return;

	CNewParticleEffect* pEffect = pAttachEnt->ParticleProp()->Create( GetIdleParticleEffect(), PATTACH_POINT_FOLLOW, "muzzle" );
	if ( pEffect )
	{
		pEffect->SetControlPoint( CUSTOM_COLOR_CP1, GetParticleColor( 1 ) );
		pEffect->SetControlPoint( CUSTOM_COLOR_CP2, GetParticleColor( 2 ) );
	}
}

#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFRaygun::GetProjectileSpeed( void )
{
	return 1200.f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
float CTFRaygun::GetProjectileGravity( void )
{
	return 0.f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CTFRaygun::IsViewModelFlipped( void )
{
	return !BaseClass::IsViewModelFlipped(); 
}

//-----------------------------------------------------------------------------
// VR pump reload
//-----------------------------------------------------------------------------
extern ConVar tfvr_reload_throttle_scale;

static inline float TFVR_BisonReloadThrottleScale()
{
	return MAX( 1.0f, tfvr_reload_throttle_scale.GetFloat() );
}

bool CTFRaygun::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	if ( GetWeaponID() != TF_WEAPON_RAYGUN )
		return false;
	if ( !tfvr_bison_pump_reload.GetBool() )
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

void CTFRaygun::ResetVRPumpGestureState( void )
{
	m_bVRPumpIsArmed = false;
	m_vecVRPumpLastHandPos = vec3_origin;
	m_bVRPumpStrokeOut = false;
	m_bVRPumpStrokeIn = false;
	m_flVRPumpStrokeDist = 0.0f;
}

float CTFRaygun::GetVRPumpStrokeProgress() const
{
	float dist = ( GetWeaponID() == TF_WEAPON_DRG_POMSON )
		? tfvr_pomson_pump_distance.GetFloat()
		: tfvr_bison_pump_distance.GetFloat();
	return ( dist > 0.0f ) ? clamp( (float)m_flVRPumpStrokeDist / dist, 0.0f, 1.0f ) : 0.0f;
}

void CTFRaygun::VRCommitPumpRecharge( void )
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
	m_flNextVRPumpRechargeTime = gpGlobals->curtime + GetVRSinglyReloadShellThrottleInterval() * TFVR_BisonReloadThrottleScale();
}

//-----------------------------------------------------------------------------
// Two-stroke gesture along weapon forward (pullback, then push forward)
// recharges one unit of energy.
//-----------------------------------------------------------------------------
void CTFRaygun::VRPumpReloadPostFrame( void )
{
	CTFPlayer *pOwner = GetTFPlayerOwner();
	if ( !pOwner )
		return;

#if defined( CLIENT_DLL )
	if ( !pOwner->IsLocalPlayer() )
		return;
#endif

	const bool bIsPomson = ( GetWeaponID() == TF_WEAPON_DRG_POMSON );
	const bool bDebug = bIsPomson ? tfvr_pomson_pump_debug.GetBool() : tfvr_bison_pump_debug.GetBool();
	const char *pszTag = bIsPomson ? "PomsonPump" : "BisonPump";

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
		if ( bDebug && ( m_bVRPumpStrokeOut || m_bVRPumpStrokeIn ) )
			DevMsg( "[VR %s] Reset: fully charged\n", pszTag );
		ResetVRPumpGestureState();
		return;
	}

	if ( gpGlobals->curtime < m_flNextPrimaryAttack )
	{
		m_bVRPumpIsArmed = false;
		m_bVRPumpStrokeOut = false;
		m_bVRPumpStrokeIn = false;
		m_flVRPumpStrokeDist = 0.0f;
		if ( m_vecVRPumpLastHandPos != vec3_origin )
			m_vecVRPumpLastHandPos = vecHandRelative;
		if ( bDebug )
			DevMsg( "[VR %s] Paused: fire cooldown\n", pszTag );
		return;
	}

	if ( !pCmd->vrWeaponArmed )
	{
		const bool bMidStroke = m_bVRPumpStrokeOut || m_bVRPumpStrokeIn;
		if ( !bMidStroke )
		{
			if ( bDebug && m_vecVRPumpLastHandPos != vec3_origin )
				DevMsg( "[VR %s] Reset: not armed\n", pszTag );
			ResetVRPumpGestureState();
		}
		else
		{
			m_vecVRPumpLastHandPos = vecHandRelative;
			m_flVRPumpStrokeDist = 0.0f;
			if ( bDebug )
				DevMsg( "[VR %s] Grip lost mid-stroke, holding state\n", pszTag );
		}
		return;
	}

	m_bVRPumpIsArmed = true;

	const float flPumpDist     = bIsPomson ? tfvr_pomson_pump_distance.GetFloat() : tfvr_bison_pump_distance.GetFloat();
	const float flSign         = bIsPomson ? tfvr_pomson_pump_sign.GetFloat() : tfvr_bison_pump_sign.GetFloat();
	const float flReloadInterval = GetVRSinglyReloadShellThrottleInterval() * TFVR_BisonReloadThrottleScale();

	QAngle angWeaponHand = pCmd->vrWeaponHandIsRight
		? pCmd->vrRawControllerAngR
		: pCmd->vrRawControllerAngL;

	matrix3x4_t controllerMatrix;
	AngleMatrix( angWeaponHand, controllerMatrix );

	Vector vecPumpAxis;
	MatrixGetColumn( controllerMatrix, 0, vecPumpAxis );
	VectorNormalize( vecPumpAxis );

	if ( m_vecVRPumpLastHandPos == vec3_origin )
	{
		m_vecVRPumpLastHandPos = vecHandRelative;
		m_flVRPumpStrokeDist   = 0.0f;
		if ( bDebug )
			DevMsg( "[VR %s] Tracking started\n", pszTag );
		return;
	}

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
					DevMsg( "[VR %s] Pullback started\n", pszTag );
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

			float flMinPullTime = MAX( flReloadInterval * 0.5f, 0.05f );
			float flMaxPullPerFrame = ( flPumpDist / flMinPullTime ) * gpGlobals->frametime;
			if ( m_flVRPumpStrokeDist >= flPumpDist * 0.60f )
			{
				m_flVRPumpStrokeDist = MIN( m_flVRPumpStrokeDist + flMaxPullPerFrame, flPumpDist );
			}
			m_vecVRPumpLastHandPos = vecHandRelative;

			if ( bDebug )
				DevMsg( "[VR %s] Pullback: %.2f / %.2f\n", pszTag, (float)m_flVRPumpStrokeDist, flPumpDist );

			if ( m_flVRPumpStrokeDist >= flPumpDist )
			{
				m_bVRPumpStrokeOut   = false;
				m_bVRPumpStrokeIn    = true;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR %s] Pullback complete, push forward to recharge\n", pszTag );

#ifdef CLIENT_DLL
				if ( prediction->IsFirstTimePredicted() )
				{
					EmitSound( "VR.BisonPumpOut" );
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
			if ( m_flVRPumpStrokeDist >= flPumpDist * 0.60f )
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
				EmitSound( "VR.BisonPumpIn" );
			}
#endif

			if ( bDebug )
				DevMsg( "[VR %s] Push: %.2f / %.2f\n", pszTag, (float)m_flVRPumpStrokeDist, flCompletionDist );

			if ( m_flVRPumpStrokeDist >= flCompletionDist
				&& gpGlobals->curtime >= m_flNextVRPumpRechargeTime )
			{
				VRCommitPumpRecharge();
				m_bVRPumpStrokeIn    = false;
				m_flVRPumpStrokeDist = 0.0f;
				if ( bDebug )
					DevMsg( "[VR %s] Energy recharged! Ready for next pump.\n", pszTag );
			}
		}
	}
}

bool CTFDRGPomson::ShouldSuppressAutoAndSinglyReloadForVR() const
{
	if ( !tfvr_pomson_pump_reload.GetBool() )
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

void CTFDRGPomson::Precache()
{
	BaseClass::Precache();

	PrecacheParticleSystem( "drg_pomson_idle" );
	PrecacheParticleSystem( "drg_pomson_impact_drain" );
	PrecacheParticleSystem( "drg_pomson_projectile" );
	PrecacheParticleSystem( "drg_pomson_muzzleflash" );
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CTFDRGPomson::GetProjectileFireSetup( CTFPlayer *pPlayer, Vector vecOffset, Vector *vecSrc, QAngle *angForward, bool bHitTeammates, float flEndDist )
{
	BaseClass::GetProjectileFireSetup( pPlayer, vecOffset, vecSrc, angForward, bHitTeammates, flEndDist );

	// adjust to line up with the weapon muzzle
	if ( !pPlayer || !pPlayer->IsInVRMode() )
	{
		vecSrc->z -= 13.0f;
	}
}
