//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: TF2VR Melee Base - VR-specific melee functionality
//
//=============================================================================

#ifndef TFVR_WEAPON_MELEE_H
#define TFVR_WEAPON_MELEE_H
#ifdef _WIN32
#pragma once
#endif

#include "tfvr_weapon_base.h"
#include "tf_weaponbase_melee.h"

#if defined( CLIENT_DLL )
	#define CTFVRWeaponMelee C_TFVRWeaponMelee
#endif

//=============================================================================
//
// TF2VR Melee Base Class - adds VR-specific melee functionality
//
class CTFVRWeaponMelee : public CTFVRWeaponBase
{
	DECLARE_CLASS( CTFVRWeaponMelee, CTFVRWeaponBase );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

#if !defined( CLIENT_DLL )
	DECLARE_DATADESC();
#endif

public:
	CTFVRWeaponMelee();
	virtual ~CTFVRWeaponMelee();

	// Overrides for VR melee
	virtual void ItemPostFrame() override;
	virtual void PrimaryAttack() override;
	
	// VR-specific swing detection
	virtual bool DetectSwing();
	virtual float CalculateSwingVelocity();
	virtual float CalculateSwingDamageMultiplier();
	
	// Hit detection from weapon model
	virtual bool DoSwingTrace( trace_t &trace );
	virtual void Swing( CTFPlayer *pPlayer );

protected:
	// Velocity tracking for swing detection
	Vector m_vecLastPosition;
	float m_flLastUpdateTime;
	float m_flSwingVelocity;
	float m_flLastSwingTime;
	
	// Minimum velocity to register as a swing (units per second)
	static constexpr float MIN_SWING_VELOCITY = 100.0f;
	
	// Cooldown between swings (seconds)
	static constexpr float SWING_COOLDOWN = 0.5f;
};

#endif // TFVR_WEAPON_MELEE_H
