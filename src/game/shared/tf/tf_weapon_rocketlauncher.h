//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF Rocket Launcher
//
//=============================================================================
#ifndef TF_WEAPON_ROCKETLAUNCHER_H
#define TF_WEAPON_ROCKETLAUNCHER_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_gun.h"
#include "tf_weaponbase_rocket.h"
#include "tf_weapon_sniperrifle.h"

#include "tf_flame.h"

enum VRCrossbowAmmoPhase
{
	VR_CROSSBOW_AMMO_PHASE_IDLE = 0,
	VR_CROSSBOW_AMMO_PHASE_EJECTING,
	VR_CROSSBOW_AMMO_PHASE_INSERTING,
	VR_CROSSBOW_AMMO_PHASE_FINISHING,
};

#ifdef CLIENT_DLL
#include "particle_property.h"
#endif

// Client specific.
#ifdef CLIENT_DLL
#define CTFRocketLauncher C_TFRocketLauncher
#define CTFRocketLauncher_DirectHit C_TFRocketLauncher_DirectHit
#define CTFRocketLauncher_AirStrike C_TFRocketLauncher_AirStrike
#define CTFRocketLauncher_Mortar C_TFRocketLauncher_Mortar
#define CTFCrossbow C_TFCrossbow
#endif // CLIENT_DLL

//=============================================================================
//
// TF Weapon Rocket Launcher.
//
class CTFRocketLauncher : public CTFWeaponBaseGun
{
public:

	DECLARE_CLASS( CTFRocketLauncher, CTFWeaponBaseGun );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

	// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFRocketLauncher();
	~CTFRocketLauncher();

#ifndef CLIENT_DLL
	virtual void	Precache();
#endif
	virtual void	ModifyEmitSoundParams( EmitSound_t &params );

	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_ROCKETLAUNCHER; }

	virtual void	Misfire( void );
	virtual CBaseEntity *FireProjectile( CTFPlayer *pPlayer );
	virtual void	ItemPostFrame( void );
	virtual void	ItemBusyFrame( void ) OVERRIDE;
	virtual bool	Reload( void ) OVERRIDE;
	virtual bool	DefaultReload( int iClipSize1, int iClipSize2, int iActivity );
	virtual bool	ShouldSuppressAutoAndSinglyReloadForVR() const OVERRIDE;

	virtual int		GetWeaponProjectileType( void ) const OVERRIDE;

	virtual bool	IsBlastImpactWeapon( void ) const { return !IsEnergyWeapon(); }

	virtual bool	CheckReloadMisfire( void ) OVERRIDE;

	virtual bool	ShouldBlockPrimaryFire() OVERRIDE;

	bool  HasVRRocketInHand() const { return m_bVRRocketHeld; }
	bool  IsVRRocketInserting() const { return m_bVRRocketInsertActive; }
	bool  IsVRRocketManualReloadActive() const { return m_bVRRocketHeld || m_bVRRocketInsertActive; }
	float GetVRRocketInsertProgress() const;
	float GetVRRocketVisualInsertProgress() const;

#ifdef CLIENT_DLL
	virtual void CreateMuzzleFlashEffects( C_BaseEntity *pAttachEnt, int nIndex );
#endif

	virtual bool	CanInspect() const OVERRIDE;

private:
	float	m_flShowReloadHintAt;

	// Since the ammo in the clip can be predicted/networked out of order from when the reload sound happens
	// We need to keep track of this invividually on client and server to modify the pitch
	int		m_nReloadPitchStep;
	bool	ShouldUseVRManualRocketReload() const;
	void	VRRocketManualReloadPostFrame();
	void	ResetVRRocketManualReloadState();
	void	VRStartRocketInsert();
	void	VRCommitRocket();
	void	PlayVRRocketReloadSound();
	float	GetVRRocketInsertDuration() const;
	float	GetVRRocketVisualInsertDuration() const;
	bool	CanStartVRRocketManualReload();

	CNetworkVar( bool, m_bVRRocketHeld );
	CNetworkVar( bool, m_bVRRocketInsertActive );
	CNetworkVar( float, m_flVRRocketInsertStartTime );
	CNetworkVar( float, m_flNextVRRocketStartTime );
	int		m_iVRRocketLastClipForManualReload;

#ifdef GAME_DLL
	int		m_iConsecutiveCrits;
	bool	m_bIsOverloading;
#endif

	CTFRocketLauncher( const CTFRocketLauncher & ) {}
};

// ------------------------------------------------------------------------------------------------------------------------
class CTFRocketLauncher_DirectHit : public CTFRocketLauncher
{
public:
	DECLARE_CLASS( CTFRocketLauncher_DirectHit, CTFRocketLauncher );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

	// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT; }
};

// ------------------------------------------------------------------------------------------------------------------------
class CTFRocketLauncher_AirStrike : public CTFRocketLauncher
{
public:
	DECLARE_CLASS( CTFRocketLauncher_AirStrike, CTFRocketLauncher );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFRocketLauncher_AirStrike();

	virtual int		GetWeaponID( void ) const		{ return TF_WEAPON_ROCKETLAUNCHER; }
	const char*		GetEffectLabelText( void )		{ return "#TF_KILLS"; }
	virtual int		GetCount( void );

#ifdef GAME_DLL
	virtual void	OnPlayerKill( CTFPlayer *pVictim, const CTakeDamageInfo &info );
#endif
};

// ------------------------------------------------------------------------------------------------------------------------
class CTFRocketLauncher_Mortar : public CTFRocketLauncher
{
public:
	DECLARE_CLASS( CTFRocketLauncher_Mortar, CTFRocketLauncher );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	//CTFRocketLauncher_Mortar();

	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_ROCKETLAUNCHER; }

	virtual CBaseEntity *FireProjectile( CTFPlayer *pPlayer );

	virtual void	SecondaryAttack( void );
	virtual void	ItemPostFrame( void );
	virtual void	ItemBusyFrame( void );

private:
	
	void			RedirectRockets();

#ifdef GAME_DLL
	CUtlVector< EHANDLE > m_vecRockets;
#endif // GAME_DLL

};

// ------------------------------------------------------------------------------------------------------------------------
class CTFCrossbow : public CTFRocketLauncher
{
public:
	DECLARE_CLASS( CTFCrossbow, CTFRocketLauncher );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

	// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFCrossbow();

	virtual void	Precache( void ) OVERRIDE;
	virtual bool	Holster( CBaseCombatWeapon *pSwitchingTo ) OVERRIDE;
	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_CROSSBOW; }
	virtual void	SecondaryAttack( void );
	virtual void	PrimaryAttack( void ) OVERRIDE;
	virtual float	GetProjectileSpeed( void );
	virtual float	GetProjectileGravity( void );
	virtual bool	IsViewModelFlipped( void );

	virtual void	ItemPostFrame( void );
	virtual void	ItemBusyFrame( void ) OVERRIDE;
	virtual bool	Reload( void ) OVERRIDE;
	virtual void	HandleFireOnEmpty( void ) OVERRIDE;
	virtual bool	ShouldSuppressAutoAndSinglyReloadForVR() const OVERRIDE;
	virtual void	ModifyProjectile( CBaseEntity* pProj );
	virtual void	WeaponRegenerate( void );

	float				GetProgress( void );
	const char*			GetEffectLabelText( void )					{ return "#TF_BOLT"; }

	bool	ShouldUseVRCrossbowManualReload() const;
	int		GetVRAmmoPhase() const					{ return m_iVRAmmoPhase; }
	float	GetVRAmmoPhaseProgress() const;
	bool	IsVRAmmoOut() const					{ return m_bVRAmmoOut; }
	bool	HasVRAmmoInHand() const				{ return m_bVRAmmoHeld; }
	bool	IsVRAmmoInserting() const				{ return m_iVRAmmoPhase == VR_CROSSBOW_AMMO_PHASE_INSERTING; }
	bool	IsVRCrossbowManualReloadBusy() const		{ return m_iVRAmmoPhase != VR_CROSSBOW_AMMO_PHASE_IDLE; }
	bool	IsVRCrossbowAmmoPoseActive() const		{ return m_bVRAmmoHeld || IsVRAmmoInserting(); }
	bool	CanStartVRAmmoPull() const;
	bool	CanStartVRAmmoEject() const;

	float	GetVRAmmoEjectDuration() const;
	float	GetVRAmmoInsertDuration() const;
	float	GetVRAmmoFinishDuration() const;

	CNetworkVar( float, m_flRegenerateDuration );
	CNetworkVar( float, m_flLastUsedTimestamp );
	CNetworkVar( int, m_iVRAmmoPhase );
	CNetworkVar( float, m_flVRAmmoPhaseStartTime );
	CNetworkVar( bool, m_bVRAmmoOut );
	CNetworkVar( bool, m_bVRAmmoHeld );

private:
	void	VRCrossbowAmmoPostFrame();
	void	ResetVRCrossbowAmmoState();
	void	VRStartAmmoEject();
	void	VRStartAmmoInsert();
	void	VRCommitAmmoInsert();
#ifdef GAME_DLL
	void	VRSpawnEjectedAmmo();
	bool	m_bVRAmmoPhysSpawned;
#endif

	bool m_bMilkNextAttack;
};

#endif // TF_WEAPON_ROCKETLAUNCHER_H
