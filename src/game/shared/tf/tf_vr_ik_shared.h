//========= TF2VR, All rights reserved. ============//
//
// Shared VR arm IK solver -- used by both client and server.
//
//=============================================================================//

#ifndef TF_VR_IK_SHARED_H
#define TF_VR_IK_SHARED_H
#pragma once

#include "cbase.h"

void VRIK_AppendChildren_R( CUtlVector< const mstudiobone_t * > *pChildBones, const studiohdr_t *pHdr, int nBone );

bool VRIK_SolveTwoBoneIK(
	const Vector &shoulderPos,
	const Vector &targetPos,
	float upperLen,
	float foreLen,
	const Vector &poleVector,
	const Vector &outwardDir,
	float minOutward,
	const Vector &upDir,
	float maxUp,
	Vector &outElbowPos );

void VRIK_RotateBoneToDirection(
	const matrix3x4_t &oldBoneMat,
	const Vector &oldDir,
	const Vector &newDir,
	const Vector &newPos,
	matrix3x4_t &outMat );

void VRIK_CascadeBoneDelta(
	CBaseAnimating *pAnim,
	const studiohdr_t *pHdr,
	matrix3x4_t *pBones,
	int parentBoneIdx,
	const matrix3x4_t &delta,
	const int *skipBones,
	int numSkip );

void VRIK_ConstrainPoleOutward( Vector &poleDir, const Vector &outwardDir, float minOutward );
void VRIK_ConstrainPoleMaxUp( Vector &poleDir, const Vector &upDir, const Vector &fallbackDir, float maxUp );

void VRIK_BuildAnatomicalArmPole(
	const Vector &shoulderPos,
	const Vector &targetPos,
	const QAngle &targetAng,
	const Vector &bodyForward,
	const Vector &bodyUp,
	const Vector &outwardDir,
	const Vector &preferredBendDir,
	float upperLen,
	float foreLen,
	float poleBlend,
	float upBias,
	float fwdBias,
	float crossBias,
	float minOutward,
	float maxUp,
	Vector &poleDir );

void VRIK_ApplyArmIK(
	CBaseAnimating *pAnim,
	const studiohdr_t *pHdr,
	matrix3x4_t *pBones,
	int iCollar,
	int iUpperArm,
	int iLowerArm,
	int iHand,
	float collarLen,
	float upperLen,
	float foreLen,
	const Vector &targetPos,
	const QAngle &targetAng,
	const Vector &poleVector,
	const Vector &elbowOutwardDir,
	const Vector &elbowUpDir,
	float minElbowOutward,
	float maxElbowUp,
	const Vector &shoulderForwardDir,
	float shoulderForwardRange,
	float shoulderRange,
	float wristTwistShare,
	float maxWristTwist,
	float maxForearmTwist,
	float maxForearmHardTwist,
	float maxUpperArmTwist,
	float maxUpperArmHardTwist,
	bool bDebug );

#endif // TF_VR_IK_SHARED_H
