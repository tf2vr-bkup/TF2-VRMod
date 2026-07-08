//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: TF2VR Physical Magazine Entity - Client
//
//=============================================================================

#ifndef C_TFVR_WEAPON_MAGAZINE_H
#define C_TFVR_WEAPON_MAGAZINE_H
#ifdef _WIN32
#pragma once
#endif

#include "cbase.h"
#include "c_physicsprop.h"

//=============================================================================
//
// TF2VR Magazine - Client-side physical magazine entity
//
class C_TFVRWeaponMagazine : public C_PhysicsProp
{
	DECLARE_CLASS( C_TFVRWeaponMagazine, C_PhysicsProp );
	DECLARE_CLIENTCLASS();

public:
	C_TFVRWeaponMagazine();
	virtual ~C_TFVRWeaponMagazine();

	virtual int DrawModel( int flags ) OVERRIDE;

	// Accessors
	int GetWeaponType() const { return m_iWeaponType; }
	int GetAmmoCount() const { return m_iAmmoCount; }
	float GetClientSpawnTime() const { return m_flClientSpawnTime; }

private:
	bool ShouldDrawInVRHandLayer() const;

	int m_iWeaponType;
	int m_iAmmoCount;
	float m_flClientSpawnTime;  // when this entity appeared on the client
};

#endif // C_TFVR_WEAPON_MAGAZINE_H
