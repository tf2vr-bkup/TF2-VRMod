//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================

#ifndef TF_WEAPON_SYRINGEGUN_H
#define TF_WEAPON_SYRINGEGUN_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_gun.h"

// Client specific.
#ifdef CLIENT_DLL
#define CTFSyringeGun C_TFSyringeGun
#endif

// VR manual syringe reload phases. These mirror the pistol magazine phases so
// the client hand/input code can reuse the same command path.
enum VRSyringeGunAmmoPhase
{
	VR_SYRINGEGUN_AMMO_PHASE_IDLE = 0,
	VR_SYRINGEGUN_AMMO_PHASE_EJECTING,
	VR_SYRINGEGUN_AMMO_PHASE_INSERTING,
	VR_SYRINGEGUN_AMMO_PHASE_FINISHING,
};

#define VR_SYRINGEGUN_RELOAD_ANIM_FPS	30.0f

inline const char *VRSyringeGun_ReloadSequenceName()
{
	return "sg_reload";
}

inline const char *VRSyringeGun_GunModelName()
{
	return "models/weapons/vr_models/vr_syringegun/vr_syringegun.mdl";
}

inline const char *VRSyringeGun_GunModelForWorldModel( const char *pszWorldModel )
{
	if ( pszWorldModel )
	{
		if ( V_stristr( pszWorldModel, "proto_syringegun" ) )
			return "models/weapons/vr_models/vr_proto_syringegun/vr_proto_syringegun.mdl";
		if ( V_stristr( pszWorldModel, "leechgun" ) )
			return "models/weapons/vr_models/vr_leechgun/vr_leechgun.mdl";
		if ( V_stristr( pszWorldModel, "crusaders_crossbow" ) || V_stristr( pszWorldModel, "crossbow" ) )
			return "models/weapons/vr_models/vr_crusaders_crossbow/vr_crusaders_crossbow.mdl";
	}

	return VRSyringeGun_GunModelName();
}

inline const char *VRSyringeGun_AmmoModelName()
{
	return "models/weapons/vr_models/vr_syringegun/vr_syringegun_ammo.mdl";
}

inline const char *VRSyringeGun_AmmoModelForWorldModel( const char *pszWorldModel )
{
	if ( pszWorldModel )
	{
		if ( V_stristr( pszWorldModel, "proto_syringegun" ) )
			return "models/weapons/vr_models/vr_proto_syringegun/vr_proto_syringegun_ammo.mdl";
		if ( V_stristr( pszWorldModel, "leechgun" ) )
			return "models/weapons/vr_models/vr_leechgun/vr_leechgun_ammo.mdl";
		if ( V_stristr( pszWorldModel, "crusaders_crossbow" ) || V_stristr( pszWorldModel, "crossbow" ) )
			return "models/weapons/vr_models/vr_crusaders_crossbow/vr_crusaders_crossbow_ammo.mdl";
	}

	return VRSyringeGun_AmmoModelName();
}

inline const char *VRSyringeGun_AmmoBoneName()
{
	return "vm_weapon_bone_1";
}

inline float VRSyringeGun_FrameAmmoFree()
{
	return 12.0f;
}

inline float VRSyringeGun_FramePause()
{
	return 18.0f;
}

inline float VRSyringeGun_FrameInsertStart()
{
	return 18.0f;
}

inline float VRSyringeGun_FrameInsertTarget()
{
	return 18.0f;
}

inline float VRSyringeGun_FrameInsertEnd()
{
	return 20.0f;
}

inline float VRSyringeGun_FrameFinishEnd()
{
	return -1.0f;
}

enum syringe_weapontypes_t
{
	SYRINGE_DEFAULT = 0,
	SYRINGE_UBER_SCALES_SPEED,
};

//=============================================================================
//
// TF Weapon Syringe gun.
//
class CTFSyringeGun : public CTFWeaponBaseGun
{
public:

	DECLARE_CLASS( CTFSyringeGun, CTFWeaponBaseGun );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFSyringeGun();
	~CTFSyringeGun() {}

	virtual void	Precache();
	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_SYRINGEGUN_MEDIC; }
	int				GetSyringeType( void ) const		{ int iMode = 0; CALL_ATTRIB_HOOK_INT( iMode, set_weapon_mode ); return iMode; };
	virtual bool	Deploy( void );
	virtual bool	Holster( CBaseCombatWeapon *pSwitchingTo );
	virtual void	ItemPostFrame() OVERRIDE;
	virtual void	ItemBusyFrame() OVERRIDE;
	virtual bool	Reload() OVERRIDE;
	virtual void	HandleFireOnEmpty() OVERRIDE;
	virtual void	PrimaryAttack() OVERRIDE;
	virtual bool	ShouldSuppressAutoAndSinglyReloadForVR() const OVERRIDE;

	virtual void RemoveProjectileAmmo( CTFPlayer *pPlayer );
	virtual bool HasPrimaryAmmo( void );

	bool	ShouldUseVRSyringeGunManualReload() const;
	int		GetVRAmmoPhase() const					{ return m_iVRAmmoPhase; }
	float	GetVRAmmoPhaseProgress() const;
	bool	IsVRAmmoOut() const					{ return m_bVRAmmoOut; }
	bool	HasVRAmmoInHand() const				{ return m_bVRAmmoHeld; }
	bool	IsVRAmmoInserting() const				{ return m_iVRAmmoPhase == VR_SYRINGEGUN_AMMO_PHASE_INSERTING; }
	bool	IsVRSyringeGunManualReloadBusy() const	{ return m_iVRAmmoPhase != VR_SYRINGEGUN_AMMO_PHASE_IDLE; }
	bool	IsVRSyringeGunAmmoPoseActive() const		{ return m_bVRAmmoHeld || IsVRAmmoInserting(); }
	bool	CanStartVRAmmoPull() const;
	bool	CanStartVRAmmoEject() const;
	void	ClearVRSyringeGunManualReloadState()		{ ResetVRSyringeGunAmmoState(); }

	float	GetVRAmmoEjectDuration() const;
	float	GetVRAmmoInsertDuration() const;
	float	GetVRAmmoFinishDuration() const;

protected:
	void	VRSyringeGunAmmoPostFrame();
	void	ResetVRSyringeGunAmmoState();
	void	VRStartAmmoEject();
	void	VRStartAmmoInsert();
	void	VRCommitAmmoInsert();
#ifdef GAME_DLL
	void	VRSpawnEjectedAmmo();
#endif

	CNetworkVar( int, m_iVRAmmoPhase );
	CNetworkVar( float, m_flVRAmmoPhaseStartTime );
	CNetworkVar( bool, m_bVRAmmoOut );
	CNetworkVar( bool, m_bVRAmmoHeld );

#ifdef GAME_DLL
	bool	m_bVRAmmoPhysSpawned;
#endif

private:

	CTFSyringeGun( const CTFSyringeGun & ) {}
};

#endif // TF_WEAPON_SYRINGEGUN_H
