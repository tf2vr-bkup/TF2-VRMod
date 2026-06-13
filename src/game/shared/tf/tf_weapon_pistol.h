//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#ifndef TF_WEAPON_PISTOL_H
#define TF_WEAPON_PISTOL_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_gun.h"

#include "tf_weapon_shotgun.h"

// Client specific.
#ifdef CLIENT_DLL
#define CTFPistol C_TFPistol
#define CTFPistol_Scout C_TFPistol_Scout
#define CTFPistol_ScoutPrimary C_TFPistol_ScoutPrimary
#define CTFPistol_ScoutSecondary C_TFPistol_ScoutSecondary
#endif

// We allow the pistol to fire as fast as the player can click.
// This is the minimum time between shots.
#define	PISTOL_FASTEST_REFIRE_TIME		0.1f

// VR manual magazine reload phases (drives p_reload sampling on the client).
enum VRPistolMagPhase
{
	VR_PISTOL_MAG_PHASE_IDLE = 0,
	VR_PISTOL_MAG_PHASE_EJECTING,   // p_reload frames 0-16 (mag clears the gun at frame 6)
	VR_PISTOL_MAG_PHASE_INSERTING,  // p_reload frames 17-19 (off-hand seats the fresh mag)
	VR_PISTOL_MAG_PHASE_FINISHING,  // p_reload frame 19 -> end (weapon hand finishes the motion)
};

// The pistol reload animations were authored at 30fps; used to convert
// per-frame animation deltas into world-space velocity for the ejected mag.
#define VR_PISTOL_RELOAD_ANIM_FPS	30.0f

// Reload animation frame markers, shared by weapon timing and client visuals.
// Scout uses p_reload, Engineer uses pstl_reload; the phases are identical
// but the authored frames differ.
inline bool VRPistol_IsEngineer( int iWeaponID )
{
	return iWeaponID == TF_WEAPON_PISTOL;
}

inline bool VRPistol_IsManualReloadWeaponID( int iWeaponID )
{
	return iWeaponID == TF_WEAPON_PISTOL
		|| iWeaponID == TF_WEAPON_PISTOL_SCOUT
		|| iWeaponID == TF_WEAPON_HANDGUN_SCOUT_SECONDARY;
}

inline const char *VRPistol_ReloadSequenceName( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? "pstl_reload" : "p_reload";
}

inline const char *VRPistol_GunModelForWorldModel( const char *pszWorldModel )
{
	if ( pszWorldModel )
	{
		if ( V_stristr( pszWorldModel, "c_winger_pistol" ) )
			return "models/weapons/vr_models/vr_winger_pistol/vr_winger_pistol.mdl";
		if ( V_stristr( pszWorldModel, "c_pep_pistol" ) )
			return "models/weapons/vr_models/vr_pep_pistol/vr_pep_pistol.mdl";
		if ( V_stristr( pszWorldModel, "c_invasion_pistol" ) )
			return "models/weapons/vr_models/vr_invasion_pistol/vr_invasion_pistol.mdl";
		if ( V_stristr( pszWorldModel, "c_ttg_max_gun" ) )
			return "models/weapons/vr_models/vr_ttg_max_gun/vr_ttg_max_gun.mdl";
	}

	return "models/weapons/vr_models/vr_pistol/vr_pistol.mdl";
}

inline const char *VRPistol_AmmoModelForWorldModel( const char *pszWorldModel )
{
	if ( pszWorldModel )
	{
		if ( V_stristr( pszWorldModel, "c_winger_pistol" ) )
			return "models/weapons/vr_models/vr_winger_pistol/vr_winger_pistol_ammo.mdl";
		if ( V_stristr( pszWorldModel, "c_pep_pistol" ) )
			return "models/weapons/vr_models/vr_pep_pistol/vr_pep_pistol_ammo.mdl";
		if ( V_stristr( pszWorldModel, "c_invasion_pistol" ) )
			return "models/weapons/vr_models/vr_invasion_pistol/vr_invasion_pistol_ammo.mdl";
	}

	// Stock pistol ammo is also the temporary fallback for Max Gun.
	return "models/weapons/vr_models/vr_pistol/vr_pistol_ammo.mdl";
}

inline bool VRPistol_HasVRModelForWorldModel( const char *pszWorldModel )
{
	return pszWorldModel
		&& ( V_stristr( pszWorldModel, "c_pistol" )
			|| V_stristr( pszWorldModel, "w_pistol" )
			|| V_stristr( pszWorldModel, "c_winger_pistol" )
			|| V_stristr( pszWorldModel, "c_pep_pistol" )
			|| V_stristr( pszWorldModel, "c_invasion_pistol" )
			|| V_stristr( pszWorldModel, "c_ttg_max_gun" ) );
}

// Frame where the mag visually clears the gun (physics prop spawns one frame ahead)
inline float VRPistol_FrameMagFree( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 4.0f : 6.0f;
}

// Frame where the weapon hand pauses awaiting a fresh mag (end of the eject motion)
inline float VRPistol_FramePause( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 4.0f : 16.0f;
}

// Frame the off-hand samples while holding the mag / where the load motion starts
inline float VRPistol_FrameInsertStart( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 12.0f : 17.0f;
}

// Frame used only for the magwell/proximity target. Engineer's off-hand pose
// starts at frame 12, but the insert target lines up better at frame 14.
inline float VRPistol_FrameInsertTarget( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 14.0f : VRPistol_FrameInsertStart( iWeaponID );
}

// Frame where the mag seats and the off-hand detaches
inline float VRPistol_FrameInsertEnd( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 16.0f : 19.0f;
}

// Frame where the weapon-hand finish motion stops (-1 = play to sequence end),
// after which the client blends back to the idle pose.
inline float VRPistol_FrameFinishEnd( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 20.0f : -1.0f;
}

// Minimum frame where firing can resume after eject starts. Engineer's authored
// pistol reload is longer than the visual handoff frames we sample.
inline float VRPistol_FrameReady( int iWeaponID )
{
	return VRPistol_IsEngineer( iWeaponID ) ? 37.0f : -1.0f;
}

// The faster the player fires, the more inaccurate he becomes
#define	PISTOL_ACCURACY_SHOT_PENALTY_TIME		0.2f	// Applied amount of time each shot adds to the time we must recover from
#define	PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME	1.5f	// Maximum time penalty we'll allow

//=============================================================================
//
// TF Weapon Pistol.
//
class CTFPistol : public CTFWeaponBaseGun
{
public:

	DECLARE_CLASS( CTFPistol, CTFWeaponBaseGun );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFPistol();
	~CTFPistol() {}

	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_PISTOL; }

	virtual void	Precache() OVERRIDE;
	virtual void	PrimaryAttack() OVERRIDE;
	virtual void	HandleFireOnEmpty() OVERRIDE;
	virtual void	ItemPostFrame() OVERRIDE;
	virtual void	ItemBusyFrame() OVERRIDE;
	virtual bool	Reload() OVERRIDE;
	virtual bool	Holster( CBaseCombatWeapon *pSwitchingTo ) OVERRIDE;
	virtual bool	ShouldSuppressAutoAndSinglyReloadForVR() const OVERRIDE;

	// VR manual magazine reload
	bool	ShouldUseVRPistolManualReload() const;
	int		GetVRMagPhase() const					{ return m_iVRMagPhase; }
	float	GetVRMagPhaseProgress() const;
	bool	IsVRMagOut() const						{ return m_bVRMagOut; }
	bool	HasVRMagazineInHand() const				{ return m_bVRMagazineHeld; }
	bool	IsVRMagInserting() const				{ return m_iVRMagPhase == VR_PISTOL_MAG_PHASE_INSERTING; }
	bool	IsVRPistolManualReloadBusy() const		{ return m_iVRMagPhase != VR_PISTOL_MAG_PHASE_IDLE; }
	// True while the off-hand should pose with a magazine (held or being inserted)
	bool	IsVRPistolMagPoseActive() const			{ return m_bVRMagazineHeld || IsVRMagInserting(); }
	bool	CanStartVRMagPull() const;
	bool	CanStartVRMagEject() const;
	void	ClearVRPistolManualReloadState()			{ ResetVRPistolMagState(); }
	int		GetVRPistolReloadWeaponID() const;

	float	GetVRMagEjectDuration() const;
	float	GetVRMagInsertDuration() const;
	float	GetVRMagFinishDuration() const;

protected:
	void	VRPistolMagPostFrame();
	void	ResetVRPistolMagState();
	void	VRStartMagEject();
	void	VRStartMagInsert();
	void	VRCommitMagInsert();
#ifdef GAME_DLL
	void	VRSpawnEjectedMagazine();
#endif

	CNetworkVar( int, m_iVRMagPhase );
	CNetworkVar( float, m_flVRMagPhaseStartTime );
	CNetworkVar( float, m_flVRMagEarliestReadyTime );
	CNetworkVar( bool, m_bVRMagOut );        // mag is out of the gun (eject frame 6 -> insert commit)
	CNetworkVar( bool, m_bVRMagazineHeld );  // spare mag held in the off hand

#ifdef GAME_DLL
	bool	m_bVRMagPhysSpawned;             // dropped-mag prop spawned for the current eject
#endif

private:
	CTFPistol( const CTFPistol & ) {}
};

// Scout specific version
class CTFPistol_Scout : public CTFPistol
{
public:
	DECLARE_CLASS( CTFPistol_Scout, CTFPistol );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_PISTOL_SCOUT; }
};

class CTFPistol_ScoutPrimary : public CTFPistol_Scout
{
public:
	DECLARE_CLASS( CTFPistol_ScoutPrimary, CTFPistol_Scout );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();
	
	CTFPistol_ScoutPrimary();

	virtual int		GetViewModelWeaponRole() { return TF_WPN_TYPE_SECONDARY; }
	virtual int		GetWeaponID( void ) const	{ return TF_WEAPON_HANDGUN_SCOUT_PRIMARY; }
	virtual void	PlayWeaponShootSound( void );
	virtual void	SecondaryAttack( void );
	virtual void	ItemPostFrame();
	virtual bool	Holster( CBaseCombatWeapon *pSwitchingTo );
	virtual void	Precache( void );

	void			Push( void );

#ifdef CLIENT_DLL
	virtual bool	ShouldPlayClientReloadSound() { return true; }
#endif

private:
	float			m_flPushTime;
};

class CTFPistol_ScoutSecondary : public CTFPistol_Scout
{
public:
	DECLARE_CLASS( CTFPistol_ScoutSecondary, CTFPistol_Scout );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

	virtual int		GetViewModelWeaponRole() { return TF_WPN_TYPE_SECONDARY; }
	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_HANDGUN_SCOUT_SECONDARY; }

	virtual int		GetDamageType( void ) const;
};

#endif // TF_WEAPON_PISTOL_H
