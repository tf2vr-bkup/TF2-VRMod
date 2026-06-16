//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"
#include "usercmd.h"
#include "bitbuf.h"
#include "checksum_md5.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// TF2 specific, need enough space for OBJ_LAST items from tf_shareddefs.h
#define WEAPON_SUBTYPE_BITS	6

template<typename Vec3>
static void WriteVec3Diff( bf_write *buf, const Vec3 &to, const Vec3 &from )
{
	if (to[0] != from[0])
	{
		buf->WriteOneBit(1);
		buf->WriteFloat(to[0]);
	}
	else
	{
		buf->WriteOneBit(0);
	}

	if (to[1] != from[1])
	{
		buf->WriteOneBit(1);
		buf->WriteFloat(to[1]);
	}
	else
	{
		buf->WriteOneBit(0);
	}

	if (to[2] != from[2])
	{
		buf->WriteOneBit(1);
		buf->WriteFloat(to[2]);
	}
	else
	{
		buf->WriteOneBit(0);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Write a delta compressed user command.
// Input  : *buf - 
//			*to - 
//			*from - 
// Output : static
//-----------------------------------------------------------------------------
void WriteUsercmd( bf_write *buf, const CUserCmd *to, const CUserCmd *from )
{
	if ( to->command_number != ( from->command_number + 1 ) )
	{
		buf->WriteOneBit( 1 );
		buf->WriteUBitLong( to->command_number, 32 );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->tick_count != ( from->tick_count + 1 ) )
	{
		buf->WriteOneBit( 1 );
		buf->WriteUBitLong( to->tick_count, 32 );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}


	if ( to->viewangles[ 0 ] != from->viewangles[ 0 ] )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->viewangles[ 0 ] );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->viewangles[ 1 ] != from->viewangles[ 1 ] )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->viewangles[ 1 ] );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->viewangles[ 2 ] != from->viewangles[ 2 ] )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->viewangles[ 2 ] );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->forwardmove != from->forwardmove )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->forwardmove );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->sidemove != from->sidemove )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->sidemove );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->upmove != from->upmove )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->upmove );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->buttons != from->buttons )
	{
		buf->WriteOneBit( 1 );
	  	buf->WriteUBitLong( to->buttons, 32 );
 	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->impulse != from->impulse )
	{
		buf->WriteOneBit( 1 );
	    buf->WriteUBitLong( to->impulse, 8 );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}


	if ( to->weaponselect != from->weaponselect )
	{
		buf->WriteOneBit( 1 );
		buf->WriteUBitLong( to->weaponselect, MAX_EDICT_BITS );

		if ( to->weaponsubtype != from->weaponsubtype )
		{
			buf->WriteOneBit( 1 );
			buf->WriteUBitLong( to->weaponsubtype, WEAPON_SUBTYPE_BITS );
		}
		else
		{
			buf->WriteOneBit( 0 );
		}
	}
	else
	{
		buf->WriteOneBit( 0 );
	}


	// TODO: Can probably get away with fewer bits.
	if ( to->mousedx != from->mousedx )
	{
		buf->WriteOneBit( 1 );
		buf->WriteShort( to->mousedx );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->mousedy != from->mousedy )
	{
		buf->WriteOneBit( 1 );
		buf->WriteShort( to->mousedy );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	WriteVec3Diff(buf, to->playerToHmdOrigin, from->playerToHmdOrigin);
	WriteVec3Diff(buf, to->playerToHmdAngles, from->playerToHmdAngles);
	WriteVec3Diff(buf, to->postFullBodyIKDeltaOrigin, from->postFullBodyIKDeltaOrigin);
	WriteVec3Diff(buf, to->clientEyePosition, from->clientEyePosition);
	WriteVec3Diff(buf, to->leftControllerOrigin, from->leftControllerOrigin);
	WriteVec3Diff(buf, to->leftControllerAngles, from->leftControllerAngles);
	WriteVec3Diff(buf, to->rightControllerOrigin, from->rightControllerOrigin);
	WriteVec3Diff(buf, to->rightControllerAngles, from->rightControllerAngles);
	WriteVec3Diff(buf, to->vrIKHandPosL, from->vrIKHandPosL);
	WriteVec3Diff(buf, to->vrIKHandAngL, from->vrIKHandAngL);
	WriteVec3Diff(buf, to->vrIKHandPosR, from->vrIKHandPosR);
	WriteVec3Diff(buf, to->vrIKHandAngR, from->vrIKHandAngR);
	WriteVec3Diff(buf, to->vrRawControllerPosL, from->vrRawControllerPosL);
	WriteVec3Diff(buf, to->vrRawControllerPosR, from->vrRawControllerPosR);
	WriteVec3Diff(buf, to->vrRawControllerAngL, from->vrRawControllerAngL);
	WriteVec3Diff(buf, to->vrRawControllerAngR, from->vrRawControllerAngR);
	WriteVec3Diff(buf, to->vrThrowVelocity, from->vrThrowVelocity);
	WriteVec3Diff(buf, to->vrThrowOrigin, from->vrThrowOrigin);
	WriteVec3Diff(buf, to->vrThrowAngles, from->vrThrowAngles);
	WriteVec3Diff(buf, to->vrThrowAngVel, from->vrThrowAngVel);

	if ( to->vrMeleeGripSpeed != from->vrMeleeGripSpeed )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->vrMeleeGripSpeed );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->vrMeleeGripSpeedLeft != from->vrMeleeGripSpeedLeft )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->vrMeleeGripSpeedLeft );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	if ( to->vrBowArrowPull01 != from->vrBowArrowPull01 )
	{
		buf->WriteOneBit( 1 );
		buf->WriteFloat( to->vrBowArrowPull01 );
	}
	else
	{
		buf->WriteOneBit( 0 );
	}

	buf->WriteOneBit( to->vrBallAimActive ? 1 : 0 );
	buf->WriteOneBit( to->vrPhysicalCrouch ? 1 : 0 );
	buf->WriteOneBit( to->vrManualPumpReload ? 1 : 0 );
	buf->WriteOneBit( to->vrShotgunShellPull ? 1 : 0 );
	buf->WriteOneBit( to->vrShotgunShellInsert ? 1 : 0 );
	buf->WriteOneBit( to->vrShotgunShellHold ? 1 : 0 );
	buf->WriteOneBit( to->vrRocketPull ? 1 : 0 );
	buf->WriteOneBit( to->vrRocketInsert ? 1 : 0 );
	buf->WriteOneBit( to->vrRocketHold ? 1 : 0 );
	buf->WriteOneBit( to->vrBowArrowPull ? 1 : 0 );
	buf->WriteOneBit( to->vrBowArrowNock ? 1 : 0 );
	buf->WriteOneBit( to->vrBowArrowGripHold ? 1 : 0 );
	buf->WriteOneBit( to->vrBowArrowTriggerHold ? 1 : 0 );
	buf->WriteOneBit( to->vrBowArrowNockIsTrigger ? 1 : 0 );
	buf->WriteOneBit( to->vrMagazineEject ? 1 : 0 );
	buf->WriteOneBit( to->vrMagazinePull ? 1 : 0 );
	buf->WriteOneBit( to->vrMagazineInsert ? 1 : 0 );
	buf->WriteOneBit( to->vrMagazineHold ? 1 : 0 );
	WriteVec3Diff( buf, to->vrMagSpawnOrigin, from->vrMagSpawnOrigin );
	WriteVec3Diff( buf, to->vrMagSpawnAngles, from->vrMagSpawnAngles );
	WriteVec3Diff( buf, to->vrMagEjectVel, from->vrMagEjectVel );
	buf->WriteOneBit( to->vrWeaponArmed ? 1 : 0 );
	buf->WriteOneBit( to->vrWeaponHandIsRight ? 1 : 0 );

#if defined( HL2_CLIENT_DLL )
	if ( to->entitygroundcontact.Count() != 0 )
	{
		buf->WriteOneBit( 1 );
		buf->WriteShort( to->entitygroundcontact.Count() );
		int i;
		for (i = 0; i < to->entitygroundcontact.Count(); i++)
		{
			buf->WriteUBitLong( to->entitygroundcontact[i].entindex, MAX_EDICT_BITS );
			buf->WriteBitCoord( to->entitygroundcontact[i].minheight );
			buf->WriteBitCoord( to->entitygroundcontact[i].maxheight );
		}
	}
	else
	{
		buf->WriteOneBit( 0 );
	}
#endif
}

template<typename Vec3>
static void ReadVec3Diff( bf_read *buf, Vec3 &to )
{
	if (buf->ReadOneBit())
	{
		to[0] = buf->ReadFloat();
	}
	if (buf->ReadOneBit())
	{
		to[1] = buf->ReadFloat();
	}
	if (buf->ReadOneBit())
	{
		to[2] = buf->ReadFloat();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Read in a delta compressed usercommand.
// Input  : *buf - 
//			*move - 
//			*from - 
// Output : static void ReadUsercmd
//-----------------------------------------------------------------------------
void ReadUsercmd( bf_read *buf, CUserCmd *move, CUserCmd *from )
{
	// Assume no change
	*move = *from;

	if ( buf->ReadOneBit() )
	{
		move->command_number = buf->ReadUBitLong( 32 );
	}
	else
	{
		// Assume steady increment
		move->command_number = from->command_number + 1;
	}

	if ( buf->ReadOneBit() )
	{
		move->tick_count = buf->ReadUBitLong( 32 );
	}
	else
	{
		// Assume steady increment
		move->tick_count = from->tick_count + 1;
	}

	// Read direction
	if ( buf->ReadOneBit() )
	{
		move->viewangles[0] = buf->ReadFloat();
	}

	if ( buf->ReadOneBit() )
	{
		move->viewangles[1] = buf->ReadFloat();
	}

	if ( buf->ReadOneBit() )
	{
		move->viewangles[2] = buf->ReadFloat();
	}

	// Moved value validation and clamping to CBasePlayer::ProcessUsercmds()

	// Read movement
	if ( buf->ReadOneBit() )
	{
		move->forwardmove = buf->ReadFloat();
	}

	if ( buf->ReadOneBit() )
	{
		move->sidemove = buf->ReadFloat();
	}

	if ( buf->ReadOneBit() )
	{
		move->upmove = buf->ReadFloat();
	}

	// read buttons
	if ( buf->ReadOneBit() )
	{
		move->buttons = buf->ReadUBitLong( 32 );
	}

	if ( buf->ReadOneBit() )
	{
		move->impulse = buf->ReadUBitLong( 8 );
	}


	if ( buf->ReadOneBit() )
	{
		move->weaponselect = buf->ReadUBitLong( MAX_EDICT_BITS );
		if ( buf->ReadOneBit() )
		{
			move->weaponsubtype = buf->ReadUBitLong( WEAPON_SUBTYPE_BITS );
		}
	}

	move->random_seed = MD5_PseudoRandom( move->command_number ) & 0x7fffffff;

	if ( buf->ReadOneBit() )
	{
		move->mousedx = buf->ReadShort();
	}

	if ( buf->ReadOneBit() )
	{
		move->mousedy = buf->ReadShort();
	}

	ReadVec3Diff(buf, move->playerToHmdOrigin);
	ReadVec3Diff(buf, move->playerToHmdAngles);
	ReadVec3Diff(buf, move->postFullBodyIKDeltaOrigin);
	ReadVec3Diff(buf, move->clientEyePosition);
	ReadVec3Diff(buf, move->leftControllerOrigin);
	ReadVec3Diff(buf, move->leftControllerAngles);
	ReadVec3Diff(buf, move->rightControllerOrigin);
	ReadVec3Diff(buf, move->rightControllerAngles);
	ReadVec3Diff(buf, move->vrIKHandPosL);
	ReadVec3Diff(buf, move->vrIKHandAngL);
	ReadVec3Diff(buf, move->vrIKHandPosR);
	ReadVec3Diff(buf, move->vrIKHandAngR);
	ReadVec3Diff(buf, move->vrRawControllerPosL);
	ReadVec3Diff(buf, move->vrRawControllerPosR);
	ReadVec3Diff(buf, move->vrRawControllerAngL);
	ReadVec3Diff(buf, move->vrRawControllerAngR);
	ReadVec3Diff(buf, move->vrThrowVelocity);
	ReadVec3Diff(buf, move->vrThrowOrigin);
	ReadVec3Diff(buf, move->vrThrowAngles);
	ReadVec3Diff(buf, move->vrThrowAngVel);

	if ( buf->ReadOneBit() )
	{
		move->vrMeleeGripSpeed = buf->ReadFloat();
	}

	if ( buf->ReadOneBit() )
	{
		move->vrMeleeGripSpeedLeft = buf->ReadFloat();
	}

	if ( buf->ReadOneBit() )
	{
		move->vrBowArrowPull01 = buf->ReadFloat();
	}

	move->vrBallAimActive = buf->ReadOneBit() ? true : false;
	move->vrPhysicalCrouch = buf->ReadOneBit() ? true : false;
	move->vrManualPumpReload = buf->ReadOneBit() ? true : false;
	move->vrShotgunShellPull = buf->ReadOneBit() ? true : false;
	move->vrShotgunShellInsert = buf->ReadOneBit() ? true : false;
	move->vrShotgunShellHold = buf->ReadOneBit() ? true : false;
	move->vrRocketPull = buf->ReadOneBit() ? true : false;
	move->vrRocketInsert = buf->ReadOneBit() ? true : false;
	move->vrRocketHold = buf->ReadOneBit() ? true : false;
	move->vrBowArrowPull = buf->ReadOneBit() ? true : false;
	move->vrBowArrowNock = buf->ReadOneBit() ? true : false;
	move->vrBowArrowGripHold = buf->ReadOneBit() ? true : false;
	move->vrBowArrowTriggerHold = buf->ReadOneBit() ? true : false;
	move->vrBowArrowNockIsTrigger = buf->ReadOneBit() ? true : false;
	move->vrMagazineEject = buf->ReadOneBit() ? true : false;
	move->vrMagazinePull = buf->ReadOneBit() ? true : false;
	move->vrMagazineInsert = buf->ReadOneBit() ? true : false;
	move->vrMagazineHold = buf->ReadOneBit() ? true : false;
	ReadVec3Diff( buf, move->vrMagSpawnOrigin );
	ReadVec3Diff( buf, move->vrMagSpawnAngles );
	ReadVec3Diff( buf, move->vrMagEjectVel );
	move->vrWeaponArmed = buf->ReadOneBit() ? true : false;
	move->vrWeaponHandIsRight = buf->ReadOneBit() ? true : false;

#if defined( HL2_DLL )
	if ( buf->ReadOneBit() )
	{
		move->entitygroundcontact.SetCount( buf->ReadShort() );

		int i;
		for (i = 0; i < move->entitygroundcontact.Count(); i++)
		{
			move->entitygroundcontact[i].entindex = buf->ReadUBitLong( MAX_EDICT_BITS );
			move->entitygroundcontact[i].minheight = buf->ReadBitCoord( );
			move->entitygroundcontact[i].maxheight = buf->ReadBitCoord( );
		}
	}
#endif
}
