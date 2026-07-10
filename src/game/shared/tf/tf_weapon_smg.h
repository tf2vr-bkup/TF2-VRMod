//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//=============================================================================
#ifndef TF_WEAPON_SMG_H
#define TF_WEAPON_SMG_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_gun.h"

enum VRSMGAmmoPhase
{
	VR_SMG_AMMO_PHASE_IDLE = 0,
	VR_SMG_AMMO_PHASE_EJECTING,
	VR_SMG_AMMO_PHASE_INSERTING,
	VR_SMG_AMMO_PHASE_FINISHING,
};

#define VR_SMG_RELOAD_ANIM_FPS	30.0f

inline const char *VRSMG_ReloadSequenceName()
{
	return "smg_reload";
}

inline const char *VRSMG_GunModelName()
{
	return "models/weapons/vr_models/vr_smg/vr_smg.mdl";
}

inline const char *VRSMG_GunModelForWorldModel( const char *pszWorldModel )
{
	if ( pszWorldModel && V_stristr( pszWorldModel, "pro_smg" ) )
		return "models/weapons/vr_models/vr_pro_smg/vr_pro_smg.mdl";

	return VRSMG_GunModelName();
}

inline const char *VRSMG_AmmoModelName()
{
	return "models/weapons/vr_models/vr_smg/vr_smg_ammo.mdl";
}

inline const char *VRSMG_AmmoModelForWorldModel( const char *pszWorldModel )
{
	if ( pszWorldModel && V_stristr( pszWorldModel, "pro_smg" ) )
		return "models/weapons/vr_models/vr_pro_smg/vr_pro_smg_ammo.mdl";

	return VRSMG_AmmoModelName();
}

inline const char *VRSMG_AmmoBoneName()
{
	return "weapon_bone_1";
}

inline float VRSMG_FrameEjectStart()
{
	return 8.0f;
}

inline float VRSMG_FrameAmmoFree()
{
	return 10.0f;
}

inline float VRSMG_FramePause()
{
	return 10.0f;
}

inline float VRSMG_FrameInsertStart()
{
	return 24.0f;
}

inline float VRSMG_FrameInsertTarget()
{
	return 24.0f;
}

inline float VRSMG_FrameInsertEnd()
{
	return 26.0f;
}

inline float VRSMG_FrameFinishEnd()
{
	return -1.0f;
}

// Client specific.
#ifdef CLIENT_DLL
#define CTFSMG C_TFSMG
#define CTFChargedSMG C_TFChargedSMG
#endif

//=============================================================================
//
// TF Weapon Sub-machine gun.
//
class CTFSMG : public CTFWeaponBaseGun
{
public:

	DECLARE_CLASS( CTFSMG, CTFWeaponBaseGun );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();

// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFSMG();
	~CTFSMG() {}

	virtual void	Precache() OVERRIDE;
	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_SMG; }

	virtual int		GetDamageType( void ) const;
	virtual bool	CanFireCriticalShot( bool bIsHeadshot, CBaseEntity *pTarget = NULL ) OVERRIDE;
	virtual bool	Holster( CBaseCombatWeapon *pSwitchingTo ) OVERRIDE;
	virtual void	ItemPostFrame() OVERRIDE;
	virtual void	ItemBusyFrame() OVERRIDE;
	virtual bool	Reload() OVERRIDE;
	virtual void	HandleFireOnEmpty() OVERRIDE;
	virtual void	PrimaryAttack() OVERRIDE;
	virtual void	WeaponRegenerate( void ) OVERRIDE;
	virtual bool	ShouldSuppressAutoAndSinglyReloadForVR() const OVERRIDE;

	bool			CanHeadshot( void ) const { int iMode = 0; CALL_ATTRIB_HOOK_INT( iMode, set_weapon_mode ); return (iMode == 1); };

	bool	ShouldUseVRSMGManualReload() const;
	int		GetVRAmmoPhase() const					{ return m_iVRAmmoPhase; }
	float	GetVRAmmoPhaseProgress() const;
	bool	IsVRAmmoOut() const					{ return m_bVRAmmoOut; }
	bool	HasVRAmmoInHand() const				{ return m_bVRAmmoHeld; }
	bool	HasVRAmmoExtractHeld() const			{ return m_bVRAmmoExtractHeld; }
	bool	IsVRAmmoInserting() const				{ return m_iVRAmmoPhase == VR_SMG_AMMO_PHASE_INSERTING; }
	bool	IsVRSMGManualReloadBusy() const		{ return m_iVRAmmoPhase != VR_SMG_AMMO_PHASE_IDLE; }
	bool	IsVRSMGAmmoPoseActive() const			{ return m_bVRAmmoHeld || IsVRAmmoInserting() || m_bVRAmmoExtractHeld; }
	bool	CanStartVRAmmoPull() const;
	bool	CanStartVRAmmoEject() const;

	float	GetVRAmmoEjectDuration() const;
	float	GetVRAmmoInsertDuration() const;
	float	GetVRAmmoFinishDuration() const;

protected:
	void	VRSMGAmmoPostFrame();
	void	ResetVRSMGAmmoState();
	void	VRStartAmmoEject();
	void	VRStartAmmoInsert();
	void	VRCommitAmmoInsert();
	bool	VRRestoreEjectedAmmo();
#ifdef GAME_DLL
	void	VRSpawnEjectedAmmo( bool bFromThrow );
	bool	m_bVRAmmoPhysSpawned;
#endif

	CNetworkVar( int, m_iVRAmmoPhase );
	CNetworkVar( float, m_flVRAmmoPhaseStartTime );
	CNetworkVar( bool, m_bVRAmmoOut );
	CNetworkVar( bool, m_bVRAmmoHeld );
	// Client-driven extract hold: mag is free of the gun but still in the off-hand.
	CNetworkVar( bool, m_bVRAmmoExtractHeld );
	// -1 means a fresh mag pulled from reserve; >=0 means the original ejected clip.
	CNetworkVar( int, m_iVRAmmoHeldCount );
	CNetworkVar( bool, m_bVRAmmoHeldCountRefilled );
	CNetworkVar( bool, m_bVRAmmoInsertLatched );

private:

	CTFSMG( const CTFSMG & ) {}
};

//=============================================================================
//
// TF Weapon Charged Sub-machine gun.
//
class CTFChargedSMG : public CTFSMG
{
public:
	DECLARE_CLASS( CTFChargedSMG, CTFSMG );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	// Server specific.
#ifdef GAME_DLL
	DECLARE_DATADESC();
#endif

	CTFChargedSMG() {}
	~CTFChargedSMG() {}

	virtual int		GetWeaponID( void ) const			{ return TF_WEAPON_CHARGED_SMG; }

	const char*		GetEffectLabelText( void ) { return "#TF_SmgCharge"; }
	float			GetProgress( void );
	bool			ShouldFlashChargeBar();
	void			SecondaryAttack() OVERRIDE;
	bool			CanPerformSecondaryAttack() const OVERRIDE;
	void			WeaponReset() OVERRIDE;

#ifdef GAME_DLL
	void	ApplyOnHitAttributes( CBaseEntity *pVictimBaseEntity, CTFPlayer *pAttacker, const CTakeDamageInfo &info ) OVERRIDE;
#endif

protected:
	CNetworkVar( float, m_flMinicritCharge );

	float m_flMinicritStartTime;

private:
	CTFChargedSMG( const CTFChargedSMG & ) {}
};


#endif // TF_WEAPON_SMG_H
