#ifndef TFVR_VR_HUD_VISIBILITY_H
#define TFVR_VR_HUD_VISIBILITY_H

#ifdef _WIN32
#pragma once
#endif

#include "cbase.h"
#include "hud.h"

inline bool TFVR_ShouldHideHudElements()
{
	extern ConVar hidehud;

	int iHideHud = 0;
	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( pPlayer )
	{
		iHideHud = pPlayer->m_Local.m_iHideHUD;
	}

	if ( hidehud.GetInt() )
	{
		iHideHud = hidehud.GetInt();
	}

	return ( iHideHud & HIDEHUD_ALL ) != 0;
}

#endif // TFVR_VR_HUD_VISIBILITY_H
