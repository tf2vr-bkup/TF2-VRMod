//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: TF2VR Physical Magazine Entity Implementation
//
//=============================================================================

#include "cbase.h"
#include "physics.h"
#include "tfvr_weapon_magazine.h"
#include "tf/tf_player.h"
#include "tfvr/tfvr_weapon_gun.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//=============================================================================
//
// TF2VR Magazine tables
//
IMPLEMENT_SERVERCLASS_ST( CTFVRWeaponMagazine, DT_TFVRWeaponMagazine )
	SendPropInt( SENDINFO( m_iWeaponType ) ),
	SendPropInt( SENDINFO( m_iAmmoCount ) ),
END_SEND_TABLE()

BEGIN_DATADESC( CTFVRWeaponMagazine )
	DEFINE_FIELD( m_iWeaponType, FIELD_INTEGER ),
	DEFINE_FIELD( m_iAmmoCount, FIELD_INTEGER ),
	DEFINE_FIELD( m_hOwnerWeapon, FIELD_EHANDLE ),
	DEFINE_FIELD( m_iszMagazineModel, FIELD_STRING ),
	DEFINE_FIELD( m_flRemoveTime, FIELD_TIME ),
	DEFINE_FIELD( m_bFading, FIELD_BOOLEAN ),
	DEFINE_THINKFUNC( FadeThink ),
END_DATADESC()

LINK_ENTITY_TO_CLASS( tfvr_weapon_magazine, CTFVRWeaponMagazine );

static const char *TFVR_WEAPON_MAGAZINE_DEFAULT_MODEL = "models/weapons/vr_models/vr_pistol/vr_pistol_ammo.mdl";
static const Vector TFVR_WEAPON_MAGAZINE_FALLBACK_MINS( -2.0f, -2.0f, -2.0f );
static const Vector TFVR_WEAPON_MAGAZINE_FALLBACK_MAXS( 2.0f, 2.0f, 2.0f );

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CTFVRWeaponMagazine::CTFVRWeaponMagazine()
{
	m_iWeaponType = TF_WEAPON_NONE;
	m_iAmmoCount = 0;
	m_hOwnerWeapon = NULL;
	m_iszMagazineModel = NULL_STRING;
	m_flRemoveTime = 0.0f;
	m_bFading = false;
}

//-----------------------------------------------------------------------------
// Purpose: Destructor
//-----------------------------------------------------------------------------
CTFVRWeaponMagazine::~CTFVRWeaponMagazine()
{
}

//-----------------------------------------------------------------------------
// Purpose: Spawn
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::Spawn()
{
	const char *pszModel = m_iszMagazineModel != NULL_STRING
		? STRING( m_iszMagazineModel )
		: TFVR_WEAPON_MAGAZINE_DEFAULT_MODEL;

	// CPhysicsProp's base precache path expects the normal model name key to
	// already be populated. Runtime-spawned magazines set it explicitly here.
	SetModelName( AllocPooledString( pszModel ) );
	Precache();
	SetModel( pszModel );

	CBaseAnimating::Spawn();

	SetCollisionGroup( COLLISION_GROUP_WEAPON );

	IPhysicsObject *pPhys = VPhysicsInitNormal( SOLID_VPHYSICS, 0, false );
	if ( !pPhys )
	{
		Vector vecMins = CollisionProp()->OBBMins();
		Vector vecMaxs = CollisionProp()->OBBMaxs();
		if ( vecMins == vec3_origin && vecMaxs == vec3_origin )
		{
			vecMins = TFVR_WEAPON_MAGAZINE_FALLBACK_MINS;
			vecMaxs = TFVR_WEAPON_MAGAZINE_FALLBACK_MAXS;
		}

		SetSolid( SOLID_BBOX );
		SetSize( vecMins, vecMaxs );

		pPhys = PhysModelCreateBox( this, WorldAlignMins(), WorldAlignMaxs(), GetAbsOrigin(), false );
		if ( pPhys )
		{
			VPhysicsSetObject( pPhys );
			SetMoveType( MOVETYPE_VPHYSICS );
			pPhys->Wake();
		}
		else
		{
			Warning( "ERROR!: Can't create fallback physics object for %s\n", pszModel );
			SetMoveType( MOVETYPE_NONE );
		}
	}

	m_bAwake = pPhys != NULL;

	// Set lifetime (30 seconds by default)
	SetLifetime( 30.0f );
}

//-----------------------------------------------------------------------------
// Purpose: Precache
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::Precache()
{
	if ( GetModelName() == NULL_STRING )
	{
		const char *pszModel = m_iszMagazineModel != NULL_STRING
			? STRING( m_iszMagazineModel )
			: TFVR_WEAPON_MAGAZINE_DEFAULT_MODEL;
		SetModelName( AllocPooledString( pszModel ) );
	}

	PrecacheModel( TFVR_WEAPON_MAGAZINE_DEFAULT_MODEL );
	PrecacheModel( "models/weapons/vr_models/vr_winger_pistol/vr_winger_pistol_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_pep_pistol/vr_pep_pistol_ammo.mdl" );
	PrecacheModel( "models/weapons/vr_models/vr_invasion_pistol/vr_invasion_pistol_ammo.mdl" );
	if ( m_iszMagazineModel != NULL_STRING )
		PrecacheModel( STRING( m_iszMagazineModel ) );
	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: Override the physical magazine model before Spawn.
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::SetMagazineModel( const char *pszModelName )
{
	m_iszMagazineModel = pszModelName && pszModelName[0]
		? AllocPooledString( pszModelName )
		: NULL_STRING;
}

//-----------------------------------------------------------------------------
// Purpose: Set owner weapon
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::SetOwnerWeapon( CTFVRWeaponGun *pWeapon )
{
	m_hOwnerWeapon = pWeapon;
}

//-----------------------------------------------------------------------------
// Purpose: Get owner weapon
//-----------------------------------------------------------------------------
CTFVRWeaponGun* CTFVRWeaponMagazine::GetOwnerWeapon() const
{
	return m_hOwnerWeapon.Get();
}

//-----------------------------------------------------------------------------
// Purpose: Use - called when player tries to grab magazine
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	CTFPlayer *pPlayer = ToTFPlayer( pActivator );
	if ( !pPlayer )
		return;
	
	if ( !CanBePickedUp( pPlayer ) )
		return;
	
	// TODO: Implement magazine pickup and attachment to hand
	// This will be handled by the hand system when we integrate it
	
	// For now, just remove the magazine
	UTIL_Remove( this );
}

//-----------------------------------------------------------------------------
// Purpose: Can this magazine be picked up by the player?
//-----------------------------------------------------------------------------
bool CTFVRWeaponMagazine::CanBePickedUp( CTFPlayer *pPlayer )
{
	if ( !pPlayer )
		return false;
	
	// Check if player has a weapon that can use this magazine
	// TODO: Implement when we have weapon checking
	
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Physics collision
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::VPhysicsCollision( int index, gamevcollisionevent_t *pEvent )
{
	BaseClass::VPhysicsCollision( index, pEvent );
	
	// Play impact sound
	// TODO: Add magazine-specific impact sounds
}

//-----------------------------------------------------------------------------
// Purpose: Set lifetime before removal
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::SetLifetime( float lifetime )
{
	m_flRemoveTime = gpGlobals->curtime + lifetime;
	SetThink( &CTFVRWeaponMagazine::FadeThink );
	SetNextThink( m_flRemoveTime - 2.0f );  // Start fading 2 seconds before removal
}

//-----------------------------------------------------------------------------
// Purpose: Fade out and remove
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::FadeAndRemove()
{
	// Start fading
	m_bFading = true;
	
	// Fade out over 2 seconds
	SetRenderMode( kRenderTransTexture );
	SetRenderColorA( 255 );
	
	// Remove after fade
	SetThink( &CBaseEntity::SUB_Remove );
	SetNextThink( gpGlobals->curtime + 2.0f );
}

//-----------------------------------------------------------------------------
// Purpose: Think function for fading
//-----------------------------------------------------------------------------
void CTFVRWeaponMagazine::FadeThink()
{
	if ( !m_bFading )
	{
		// Start fading
		FadeAndRemove();
	}
	else
	{
		// Continue fading
		float fadeTime = 2.0f;
		float timeLeft = m_flRemoveTime - gpGlobals->curtime;
		float alpha = (timeLeft / fadeTime) * 255.0f;
		alpha = clamp( alpha, 0.0f, 255.0f );
		
		SetRenderColorA( (byte)alpha );
		
		// Continue thinking
		SetNextThink( gpGlobals->curtime + 0.1f );
	}
}

//=============================================================================
