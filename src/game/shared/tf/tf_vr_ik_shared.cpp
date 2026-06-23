//========= TF2VR, All rights reserved. ============//
//
// Shared VR arm IK solver -- used by both client and server
// for VR player arm positioning.
//
//=============================================================================//

#include "cbase.h"
#include "tf_vr_ik_shared.h"

#ifdef CLIENT_DLL
#include "engine/ivdebugoverlay.h"
#endif

#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Recursively collect all child bones of the given bone index.
//-----------------------------------------------------------------------------
void VRIK_AppendChildren_R( CUtlVector< const mstudiobone_t * > *pChildBones, const studiohdr_t *pHdr, int nBone )
{
	if ( !pChildBones || !pHdr )
		return;

	for ( int i = nBone + 1; i < pHdr->numbones; ++i )
	{
		const mstudiobone_t *pBone = pHdr->pBone( i );
		if ( pBone->parent == nBone )
		{
			pChildBones->AddToTail( pBone );
			VRIK_AppendChildren_R( pChildBones, pHdr, i );
		}
	}
}

static void VRIK_ConstrainBendDirectionInPlane(
	Vector &bendDir,
	const Vector &armDir,
	const Vector &elbowCenterOffset,
	float elbowHeight,
	float constraintScale,
	const Vector &outwardDir,
	float minOutward,
	const Vector &upDir,
	float maxUp )
{
	Vector armAxis = armDir;
	if ( bendDir.NormalizeInPlace() < 0.001f || armAxis.NormalizeInPlace() < 0.001f )
		return;

	Vector planeOutward = outwardDir;
	bool bHavePlaneOutward = false;
	if ( planeOutward.NormalizeInPlace() >= 0.001f )
	{
		planeOutward = planeOutward - armAxis * DotProduct( planeOutward, armAxis );
		bHavePlaneOutward = ( planeOutward.NormalizeInPlace() >= 0.001f );
	}

	Vector planeUp = upDir;
	bool bHavePlaneUp = false;
	if ( planeUp.NormalizeInPlace() >= 0.001f )
	{
		planeUp = planeUp - armAxis * DotProduct( planeUp, armAxis );
		bHavePlaneUp = ( planeUp.NormalizeInPlace() >= 0.001f );
	}

	for ( int comfortPass = 0; comfortPass < 2; ++comfortPass )
	{
		Vector comfortDir = bendDir;

		if ( minOutward > -1.0f && bHavePlaneOutward )
		{
			Vector outward = outwardDir;
			outward.NormalizeInPlace();

			float outwardPerpLen = ( outward - armAxis * DotProduct( outward, armAxis ) ).Length();
			if ( outwardPerpLen > 0.001f )
			{
				float requiredOutward = ( minOutward * constraintScale - DotProduct( elbowCenterOffset, outward ) ) / MAX( elbowHeight, 0.001f );
				float targetOutward = clamp( requiredOutward / outwardPerpLen, -0.95f, 0.95f );
				float currentOutward = DotProduct( bendDir, planeOutward );
				float outwardDeficit = targetOutward - currentOutward;
				if ( outwardDeficit > -0.15f )
					comfortDir += planeOutward * clamp( outwardDeficit + 0.15f, 0.0f, 0.45f ) * 0.5f;
			}
		}

		if ( maxUp < 1.0f && bHavePlaneUp )
		{
			Vector up = upDir;
			up.NormalizeInPlace();

			float upPerpLen = ( up - armAxis * DotProduct( up, armAxis ) ).Length();
			if ( upPerpLen > 0.001f )
			{
				float allowedUp = ( maxUp * constraintScale - DotProduct( elbowCenterOffset, up ) ) / MAX( elbowHeight, 0.001f );
				float targetUp = clamp( allowedUp / upPerpLen, -0.95f, 0.95f );
				float currentUp = DotProduct( bendDir, planeUp );
				float upExcess = currentUp - targetUp;
				if ( upExcess > -0.15f )
				{
					Vector downComfort = -planeUp;
					if ( bHavePlaneOutward )
						downComfort += planeOutward * 0.25f;
					if ( downComfort.NormalizeInPlace() >= 0.001f )
						comfortDir += downComfort * clamp( upExcess + 0.15f, 0.0f, 0.45f ) * 0.5f;
				}
			}
		}

		comfortDir = comfortDir - armAxis * DotProduct( comfortDir, armAxis );
		if ( comfortDir.NormalizeInPlace() >= 0.001f )
			bendDir = comfortDir;
	}

	for ( int constraintPass = 0; constraintPass < 2; ++constraintPass )
	{
		if ( minOutward > -1.0f && bHavePlaneOutward )
		{
			Vector outward = outwardDir;
			outward.NormalizeInPlace();

			float outwardPerpLen = ( outward - armAxis * DotProduct( outward, armAxis ) ).Length();
			if ( outwardPerpLen > 0.001f )
			{
				float requiredOutward = ( minOutward * constraintScale - DotProduct( elbowCenterOffset, outward ) ) / MAX( elbowHeight, 0.001f );
				VRIK_ConstrainPoleOutward( bendDir, planeOutward, requiredOutward / outwardPerpLen );
			}
		}

		if ( maxUp < 1.0f && bHavePlaneUp )
		{
			Vector up = upDir;
			up.NormalizeInPlace();

			float upPerpLen = ( up - armAxis * DotProduct( up, armAxis ) ).Length();
			if ( upPerpLen > 0.001f )
			{
				Vector fallback = bHavePlaneOutward ? planeOutward : bendDir;
				float allowedUp = ( maxUp * constraintScale - DotProduct( elbowCenterOffset, up ) ) / MAX( elbowHeight, 0.001f );
				VRIK_ConstrainPoleMaxUp( bendDir, planeUp, fallback, allowedUp / upPerpLen );
			}
		}

		bendDir = bendDir - armAxis * DotProduct( bendDir, armAxis );
		if ( bendDir.NormalizeInPlace() < 0.001f )
		{
			if ( bHavePlaneOutward )
				bendDir = planeOutward;
			else if ( bHavePlaneUp )
				bendDir = planeUp;
			else
				return;
		}
	}
}

//-----------------------------------------------------------------------------
// Keep the elbow pole from folding inward toward the chest.
//-----------------------------------------------------------------------------
void VRIK_ConstrainPoleOutward( Vector &poleDir, const Vector &outwardDir, float minOutward )
{
	if ( minOutward <= -1.0f )
		return;

	float poleLen = poleDir.Length();
	float outwardLen = outwardDir.Length();
	if ( poleLen < 0.001f || outwardLen < 0.001f )
		return;

	poleDir /= poleLen;

	Vector outward = outwardDir / outwardLen;
	float outwardDot = DotProduct( poleDir, outward );
	float targetOutward = clamp( minOutward, -0.95f, 0.95f );
	if ( outwardDot >= targetOutward )
		return;

	Vector polePerp = poleDir - outward * outwardDot;
	float polePerpLen = polePerp.Length();
	if ( polePerpLen < 0.001f )
	{
		poleDir += outward * ( targetOutward - outwardDot );
		poleDir.NormalizeInPlace();
		return;
	}

	polePerp /= polePerpLen;
	poleDir = outward * targetOutward + polePerp * sqrtf( 1.0f - targetOutward * targetOutward );
	poleDir.NormalizeInPlace();
}

//-----------------------------------------------------------------------------
// Keep the elbow pole from bending too far upward. Elbows can flare outward or
// down, but a high pole produces neck/chest clipping and candy-wrapper poses.
//-----------------------------------------------------------------------------
void VRIK_ConstrainPoleMaxUp( Vector &poleDir, const Vector &upDir, const Vector &fallbackDir, float maxUp )
{
	if ( maxUp >= 1.0f )
		return;

	float poleLen = poleDir.Length();
	float upLen = upDir.Length();
	if ( poleLen < 0.001f || upLen < 0.001f )
		return;

	poleDir /= poleLen;
	Vector up = upDir / upLen;

	float upDot = DotProduct( poleDir, up );
	float targetUp = clamp( maxUp, -0.95f, 0.95f );
	if ( upDot <= targetUp )
		return;

	Vector polePerp = poleDir - up * upDot;
	float polePerpLen = polePerp.Length();
	if ( polePerpLen < 0.001f )
	{
		polePerp = fallbackDir - up * DotProduct( fallbackDir, up );
		polePerpLen = polePerp.Length();
	}

	if ( polePerpLen < 0.001f )
	{
		poleDir = -up;
		return;
	}

	polePerp /= polePerpLen;
	poleDir = up * targetUp + polePerp * sqrtf( 1.0f - targetUp * targetUp );
	poleDir.NormalizeInPlace();
}

//-----------------------------------------------------------------------------
// 2-bone IK solver: finds elbow position given shoulder, hand target,
// and arm segment lengths. Returns false if inputs are degenerate.
//-----------------------------------------------------------------------------
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
	Vector &outElbowPos )
{
	Vector toTarget = targetPos - shoulderPos;
	float dist = toTarget.Length();

	if ( dist < 0.001f )
		return false;

	float minDist = fabsf( upperLen - foreLen ) + 0.1f;
	float maxDist = upperLen + foreLen - 0.1f;
	dist = clamp( dist, minDist, maxDist );

	float cosA = ( upperLen * upperLen + dist * dist - foreLen * foreLen ) / ( 2.0f * upperLen * dist );
	cosA = clamp( cosA, -1.0f, 1.0f );
	float sinA = sqrtf( 1.0f - cosA * cosA );

	Vector dir = toTarget;
	dir.NormalizeInPlace();

	float projLen = upperLen * cosA;
	float elbowHeight = upperLen * sinA;

	Vector elbowCenter = shoulderPos + dir * projLen;

	Vector toPole = poleVector - shoulderPos;
	Vector perpDir = toPole - dir * DotProduct( toPole, dir );
	float perpLen = perpDir.Length();
	if ( perpLen < 0.001f )
	{
		Vector arbitrary = ( fabsf( dir.z ) < 0.9f ) ? Vector( 0, 0, 1 ) : Vector( 1, 0, 0 );
		perpDir = CrossProduct( dir, arbitrary );
		perpLen = perpDir.Length();
	}
	perpDir /= perpLen;
	VRIK_ConstrainBendDirectionInPlane( perpDir, dir, elbowCenter - shoulderPos, elbowHeight, upperLen + foreLen,
		outwardDir, minOutward, upDir, maxUp );

	outElbowPos = elbowCenter + perpDir * elbowHeight;
	return true;
}

//-----------------------------------------------------------------------------
// Signed angle from one vector to another around an axis.
//-----------------------------------------------------------------------------
static float VRIK_SignedAngleAroundAxis(
	const Vector &from,
	const Vector &to,
	const Vector &axis )
{
	Vector twistAxis = axis;
	float axisLen = twistAxis.Length();
	if ( axisLen < 0.001f )
		return 0.0f;
	twistAxis /= axisLen;

	Vector fromPerp = from - twistAxis * DotProduct( from, twistAxis );
	Vector toPerp = to - twistAxis * DotProduct( to, twistAxis );

	float fromLen = fromPerp.Length();
	float toLen = toPerp.Length();
	if ( fromLen < 0.001f || toLen < 0.001f )
		return 0.0f;

	fromPerp /= fromLen;
	toPerp /= toLen;

	float sinAngle = DotProduct( CrossProduct( fromPerp, toPerp ), twistAxis );
	float cosAngle = clamp( DotProduct( fromPerp, toPerp ), -1.0f, 1.0f );
	return RAD2DEG( atan2f( sinAngle, cosAngle ) );
}

//-----------------------------------------------------------------------------
// Signed roll delta between two bone frames around an axis. Chooses the bone axis
// least aligned to the twist axis, so it works across differing model bone bases.
//-----------------------------------------------------------------------------
static float VRIK_SignedMatrixTwistAroundAxis(
	const matrix3x4_t &fromMat,
	const matrix3x4_t &toMat,
	const Vector &axis )
{
	int bestColumn = -1;
	float bestPerpLenSqr = 0.0f;

	Vector twistAxis = axis;
	float axisLen = twistAxis.Length();
	if ( axisLen < 0.001f )
		return 0.0f;
	twistAxis /= axisLen;

	for ( int i = 0; i < 3; ++i )
	{
		Vector fromColumn, toColumn;
		MatrixGetColumn( fromMat, i, fromColumn );
		MatrixGetColumn( toMat, i, toColumn );

		Vector fromPerp = fromColumn - twistAxis * DotProduct( fromColumn, twistAxis );
		Vector toPerp = toColumn - twistAxis * DotProduct( toColumn, twistAxis );
		float fromLenSqr = DotProduct( fromPerp, fromPerp );
		float toLenSqr = DotProduct( toPerp, toPerp );
		float perpLenSqr = MIN( fromLenSqr, toLenSqr );

		if ( perpLenSqr > bestPerpLenSqr )
		{
			bestColumn = i;
			bestPerpLenSqr = perpLenSqr;
		}
	}

	if ( bestColumn == -1 || bestPerpLenSqr < 0.000001f )
		return 0.0f;

	Vector fromColumn, toColumn;
	MatrixGetColumn( fromMat, bestColumn, fromColumn );
	MatrixGetColumn( toMat, bestColumn, toColumn );
	return VRIK_SignedAngleAroundAxis( fromColumn, toColumn, twistAxis );
}

//-----------------------------------------------------------------------------
// Clamp bone-frame roll around a segment axis while preserving the segment aim.
//-----------------------------------------------------------------------------
static void VRIK_ClampBoneTwistFromBase(
	const matrix3x4_t &baseMat,
	const Vector &twistAxis,
	float maxTwist,
	matrix3x4_t &boneMat )
{
	if ( maxTwist < 0.0f || maxTwist >= 180.0f )
		return;

	Vector axis = twistAxis;
	float axisLen = axis.Length();
	if ( axisLen < 0.001f )
		return;
	axis /= axisLen;

	float twistAngle = VRIK_SignedMatrixTwistAroundAxis( baseMat, boneMat, axis );
	float clampedTwistAngle = clamp( twistAngle, -maxTwist, maxTwist );
	float correctionAngle = clampedTwistAngle - twistAngle;

	if ( fabsf( correctionAngle ) < 0.5f )
		return;

	Vector bonePos;
	MatrixPosition( boneMat, bonePos );

	Quaternion correctionQ;
	AxisAngleQuaternion( axis, correctionAngle, correctionQ );

	matrix3x4_t correctionMat;
	QuaternionMatrix( correctionQ, correctionMat );

	matrix3x4_t noTranslate;
	MatrixCopy( boneMat, noTranslate );
	MatrixSetTranslation( vec3_origin, noTranslate );

	matrix3x4_t constrained;
	ConcatTransforms( correctionMat, noTranslate, constrained );
	MatrixSetTranslation( bonePos, constrained );
	MatrixCopy( constrained, boneMat );
}

static float VRIK_SoftLimitTwistAngle( float twistAngle, float softLimit, float hardLimit )
{
	if ( softLimit < 0.0f || softLimit >= 180.0f )
	{
		if ( hardLimit >= 0.0f && hardLimit < 180.0f )
			return clamp( twistAngle, -hardLimit, hardLimit );
		return twistAngle;
	}

	float absTwist = fabsf( twistAngle );
	if ( absTwist <= softLimit )
		return twistAngle;

	float effectiveHardLimit = hardLimit;
	if ( effectiveHardLimit < softLimit || effectiveHardLimit >= 180.0f )
		effectiveHardLimit = 180.0f;

	float sign = ( twistAngle >= 0.0f ) ? 1.0f : -1.0f;
	float softenedTwist = softLimit + ( absTwist - softLimit ) * 0.5f;
	return sign * MIN( softenedTwist, effectiveHardLimit );
}

static void VRIK_SoftClampBoneTwistFromBase(
	const matrix3x4_t &baseMat,
	const Vector &twistAxis,
	float softLimit,
	float hardLimit,
	matrix3x4_t &boneMat )
{
	if ( ( softLimit < 0.0f || softLimit >= 180.0f ) && ( hardLimit < 0.0f || hardLimit >= 180.0f ) )
		return;

	Vector axis = twistAxis;
	float axisLen = axis.Length();
	if ( axisLen < 0.001f )
		return;
	axis /= axisLen;

	float twistAngle = VRIK_SignedMatrixTwistAroundAxis( baseMat, boneMat, axis );
	float limitedTwistAngle = VRIK_SoftLimitTwistAngle( twistAngle, softLimit, hardLimit );
	float correctionAngle = limitedTwistAngle - twistAngle;

	if ( fabsf( correctionAngle ) < 0.5f )
		return;

	Vector bonePos;
	MatrixPosition( boneMat, bonePos );

	Quaternion correctionQ;
	AxisAngleQuaternion( axis, correctionAngle, correctionQ );

	matrix3x4_t correctionMat;
	QuaternionMatrix( correctionQ, correctionMat );

	matrix3x4_t noTranslate;
	MatrixCopy( boneMat, noTranslate );
	MatrixSetTranslation( vec3_origin, noTranslate );

	matrix3x4_t constrained;
	ConcatTransforms( correctionMat, noTranslate, constrained );
	MatrixSetTranslation( bonePos, constrained );
	MatrixCopy( constrained, boneMat );
}

static void VRIK_ApplyBoneTwist(
	const Vector &twistAxis,
	float twistAngle,
	const Vector &bonePos,
	matrix3x4_t &boneMat )
{
	if ( fabsf( twistAngle ) < 0.5f )
		return;

	Vector axis = twistAxis;
	if ( axis.NormalizeInPlace() < 0.001f )
		return;

	Quaternion twistQ;
	AxisAngleQuaternion( axis, twistAngle, twistQ );

	matrix3x4_t twistMat;
	QuaternionMatrix( twistQ, twistMat );

	matrix3x4_t noTranslate;
	MatrixCopy( boneMat, noTranslate );
	MatrixSetTranslation( vec3_origin, noTranslate );

	matrix3x4_t rotated;
	ConcatTransforms( twistMat, noTranslate, rotated );
	MatrixSetTranslation( bonePos, rotated );
	MatrixCopy( rotated, boneMat );
}

//-----------------------------------------------------------------------------
// Rotate a bone matrix so its primary axis points in a new direction,
// preserving the original twist/roll as much as possible.
//-----------------------------------------------------------------------------
void VRIK_RotateBoneToDirection(
	const matrix3x4_t &oldBoneMat,
	const Vector &oldDir,
	const Vector &newDir,
	const Vector &newPos,
	matrix3x4_t &outMat )
{
	Vector axis = CrossProduct( oldDir, newDir );
	float axisLen = axis.Length();
	float dot = DotProduct( oldDir, newDir );

	Quaternion rotQ;
	if ( axisLen < 0.0001f )
	{
		if ( dot > 0.0f )
		{
			MatrixCopy( oldBoneMat, outMat );
			MatrixSetTranslation( newPos, outMat );
			return;
		}
		Vector perp = ( fabsf( oldDir.z ) < 0.9f ) ? Vector( 0, 0, 1 ) : Vector( 1, 0, 0 );
		axis = CrossProduct( oldDir, perp );
		axis.NormalizeInPlace();
		AxisAngleQuaternion( axis, 180.0f, rotQ );
	}
	else
	{
		axis /= axisLen;
		float angle = RAD2DEG( atan2f( axisLen, dot ) );
		AxisAngleQuaternion( axis, angle, rotQ );
	}

	matrix3x4_t rotMat;
	QuaternionMatrix( rotQ, rotMat );

	matrix3x4_t oldNoTranslate;
	MatrixCopy( oldBoneMat, oldNoTranslate );
	MatrixSetTranslation( vec3_origin, oldNoTranslate );

	matrix3x4_t rotated;
	ConcatTransforms( rotMat, oldNoTranslate, rotated );
	MatrixSetTranslation( newPos, rotated );

	MatrixCopy( rotated, outMat );
}

//-----------------------------------------------------------------------------
// Apply a delta transform to all descendant bones of a given bone,
// skipping bones in the skipSet.
// Operates on the raw pBones buffer, using pAnim only for LookupBone.
//-----------------------------------------------------------------------------
void VRIK_CascadeBoneDelta(
	CBaseAnimating *pAnim,
	const studiohdr_t *pHdr,
	matrix3x4_t *pBones,
	int parentBoneIdx,
	const matrix3x4_t &delta,
	const int *skipBones,
	int numSkip )
{
	CUtlVector< const mstudiobone_t * > children;
	VRIK_AppendChildren_R( &children, pHdr, parentBoneIdx );

	for ( int i = 0; i < children.Count(); i++ )
	{
		int childIdx = pAnim->LookupBone( children[i]->pszName() );
		if ( childIdx == -1 )
			continue;

		bool bSkip = false;
		for ( int s = 0; s < numSkip; s++ )
		{
			if ( childIdx == skipBones[s] )
			{
				bSkip = true;
				break;
			}
		}
		if ( bSkip )
			continue;

		matrix3x4_t newChild;
		ConcatTransforms( delta, pBones[childIdx], newChild );
		MatrixCopy( newChild, pBones[childIdx] );
	}
}

static float VRIK_SmoothStep( float edge0, float edge1, float x )
{
	if ( edge0 == edge1 )
		return x >= edge1 ? 1.0f : 0.0f;

	float t = clamp( ( x - edge0 ) / ( edge1 - edge0 ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

//-----------------------------------------------------------------------------
// Choose a stable default elbow bend direction before solving the chain.
// Controller rotation should not move the elbow by default; it only owns the
// final hand orientation. The bend is driven by body-space reach plus the
// animation/rest elbow direction when that direction is usable.
//-----------------------------------------------------------------------------
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
	Vector &poleDir )
{
	Vector forward = bodyForward;
	Vector up = bodyUp;
	Vector outward = outwardDir;
	if ( forward.NormalizeInPlace() < 0.001f || up.NormalizeInPlace() < 0.001f || outward.NormalizeInPlace() < 0.001f )
	{
		poleDir = Vector( 0, 0, -1 );
		return;
	}

	Vector toTarget = targetPos - shoulderPos;
	float reachDist = toTarget.Length();
	if ( reachDist < 0.001f )
	{
		poleDir = -up + outward;
		poleDir.NormalizeInPlace();
		return;
	}

	Vector objectiveDir = toTarget / reachDist;
	const float armLen = MAX( upperLen + foreLen, 1.0f );
	Vector objectivePosInLookupSpace(
		DotProduct( toTarget, outward ) / armLen,
		DotProduct( toTarget, up ) / armLen,
		DotProduct( toTarget, forward ) / armLen );

	float outwardReach = objectivePosInLookupSpace.x;
	float upReach = objectivePosInLookupSpace.y;
	float forwardReach = objectivePosInLookupSpace.z;
	float reachFrac = clamp( reachDist / armLen, 0.0f, 1.0f );

	float isInside = 0.0f;
	if ( objectivePosInLookupSpace.LengthSqr() > 0.000001f )
		isInside = VRIK_SmoothStep( 0.0f, 0.65f, -outwardReach );

	float highReach = VRIK_SmoothStep( 0.1f, 0.65f, upReach );
	float forwardReachAbs = fabsf( forwardReach );

	Vector regular = outward * ( 0.55f + crossBias * isInside );
	regular += -up * ( 0.85f + upBias * ( 0.25f + highReach ) );
	regular += -forward * ( fwdBias * 0.25f * VRIK_SmoothStep( 0.25f, 0.85f, forwardReachAbs ) );

	Vector preferred = preferredBendDir;
	preferred = preferred - objectiveDir * DotProduct( preferred, objectiveDir );
	if ( preferred.NormalizeInPlace() >= 0.001f )
	{
		float preferredOutward = DotProduct( preferred, outward );
		float preferredUp = DotProduct( preferred, up );
		bool bPreferredComfortable = ( preferredOutward > -0.05f && preferredUp < 0.65f );
		if ( bPreferredComfortable )
		{
			float preserveFrac = ( 1.0f - isInside ) * ( 1.0f - highReach ) * ( 1.0f - VRIK_SmoothStep( 0.82f, 0.98f, reachFrac ) );
			regular = regular * ( 1.0f - preserveFrac * 0.45f ) + preferred * ( preserveFrac * 0.45f );
		}
	}

	regular += outward * MAX( -outwardReach, 0.0f ) * 0.4f;

	if ( regular.NormalizeInPlace() < 0.001f )
		regular = -up + outward;

	Vector hUp;
	AngleVectors( targetAng, NULL, NULL, &hUp );
	Vector handPole = -hUp;
	if ( handPole.NormalizeInPlace() >= 0.001f && poleBlend > 0.001f )
	{
		const float t = clamp( poleBlend, 0.0f, 1.0f );
		regular = regular * ( 1.0f - t ) + handPole * t;
		if ( regular.NormalizeInPlace() < 0.001f )
			regular = -up + outward;
	}

	poleDir = regular;
	VRIK_ConstrainPoleOutward( poleDir, outward, minOutward );
	VRIK_ConstrainPoleMaxUp( poleDir, up, outward, maxUp );
}

//-----------------------------------------------------------------------------
// Full arm IK: collar shoulder movement, two-bone solve, wrist twist
// distribution, and hand orientation. Works on a raw bone buffer.
//-----------------------------------------------------------------------------
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
	bool bDebug )
{
	if ( iUpperArm == -1 || iLowerArm == -1 || iHand == -1 )
		return;

	// --- Collar/clavicle shoulder movement ---
	if ( iCollar != -1 && collarLen > 0.1f && shoulderRange > 0.0f )
	{
		Vector collarPos;
		MatrixPosition( pBones[iCollar], collarPos );
		Vector animShoulderPos;
		MatrixPosition( pBones[iUpperArm], animShoulderPos );

		Vector animCollarDir = ( animShoulderPos - collarPos );
		animCollarDir.NormalizeInPlace();

		Vector reachDir = ( targetPos - collarPos );
		reachDir.NormalizeInPlace();

		Vector shoulderForward = shoulderForwardDir;
		if ( shoulderForwardRange > 0.0f && shoulderForward.NormalizeInPlace() >= 0.001f )
		{
			Vector shoulderToTarget = targetPos - animShoulderPos;
			float reachForward = DotProduct( shoulderToTarget, shoulderForward ) / MAX( upperLen + foreLen, 1.0f );
			float reachForwardAbs = fabsf( reachForward );
			float forwardFrac = VRIK_SmoothStep( 0.25f, 0.85f, reachForwardAbs );
			if ( forwardFrac > 0.001f )
			{
				float forwardSign = ( reachForward >= 0.0f ) ? 1.0f : -1.0f;
				float forwardAngle = clamp( shoulderForwardRange, 0.0f, 60.0f );
				float forwardBias = tanf( DEG2RAD( forwardAngle ) ) * forwardSign * forwardFrac;
				Vector forwardReachDir = reachDir + shoulderForward * forwardBias;
				if ( forwardReachDir.NormalizeInPlace() >= 0.001f )
					reachDir = forwardReachDir;
			}
		}

		float cosAngle = clamp( DotProduct( animCollarDir, reachDir ), -1.0f, 1.0f );
		float angleDeg = RAD2DEG( acosf( cosAngle ) );

		if ( angleDeg > 1.0f )
		{
			float clampedAngle = MIN( angleDeg, shoulderRange );
			float frac = clampedAngle / angleDeg;

			Vector blendedDir = animCollarDir * ( 1.0f - frac ) + reachDir * frac;
			blendedDir.NormalizeInPlace();

			matrix3x4_t oldCollar;
			MatrixCopy( pBones[iCollar], oldCollar );

			VRIK_RotateBoneToDirection( oldCollar, animCollarDir, blendedDir, collarPos, pBones[iCollar] );

			matrix3x4_t collarDelta, collarOldInv;
			MatrixInvert( oldCollar, collarOldInv );
			ConcatTransforms( pBones[iCollar], collarOldInv, collarDelta );

			int skipCollar[] = { iCollar };
			VRIK_CascadeBoneDelta( pAnim, pHdr, pBones, iCollar, collarDelta, skipCollar, 1 );

#ifdef CLIENT_DLL
			if ( bDebug )
			{
				Vector newShoulderPos;
				MatrixPosition( pBones[iUpperArm], newShoulderPos );
				debugoverlay->AddLineOverlay( collarPos, newShoulderPos, 255, 255, 0, true, 0.0f );
			}
#endif
		}
	}

	// --- Two-bone IK ---
	Vector animShoulderPos, animElbowPos, animHandPos;
	MatrixPosition( pBones[iUpperArm], animShoulderPos );
	MatrixPosition( pBones[iLowerArm], animElbowPos );
	MatrixPosition( pBones[iHand], animHandPos );

	matrix3x4_t oldUpperArm;
	MatrixCopy( pBones[iUpperArm], oldUpperArm );

	Vector elbowPos;
	if ( !VRIK_SolveTwoBoneIK( animShoulderPos, targetPos, upperLen, foreLen, poleVector,
		elbowOutwardDir, minElbowOutward, elbowUpDir, maxElbowUp, elbowPos ) )
		return;

	// Upper arm
	Vector animUpperDir = ( animElbowPos - animShoulderPos );
	animUpperDir.NormalizeInPlace();
	Vector animForeDir = ( animHandPos - animElbowPos );
	animForeDir.NormalizeInPlace();

	Vector ikUpperDir = ( elbowPos - animShoulderPos );
	ikUpperDir.NormalizeInPlace();
	Vector ikForeDir = ( targetPos - elbowPos );
	ikForeDir.NormalizeInPlace();

	VRIK_RotateBoneToDirection( oldUpperArm, animUpperDir, ikUpperDir, animShoulderPos, pBones[iUpperArm] );
	VRIK_SoftClampBoneTwistFromBase( oldUpperArm, ikUpperDir, maxUpperArmTwist, maxUpperArmHardTwist, pBones[iUpperArm] );

	matrix3x4_t upperDelta, upperOldInv;
	MatrixInvert( oldUpperArm, upperOldInv );
	ConcatTransforms( pBones[iUpperArm], upperOldInv, upperDelta );

	int skipUpper[] = { iUpperArm };
	VRIK_CascadeBoneDelta( pAnim, pHdr, pBones, iUpperArm, upperDelta, skipUpper, 1 );

	// Forearm
	matrix3x4_t currentLowerArm;
	MatrixCopy( pBones[iLowerArm], currentLowerArm );

	Vector rotatedForeDir;
	VectorRotate( animForeDir, upperDelta, rotatedForeDir );
	rotatedForeDir.NormalizeInPlace();

	VRIK_RotateBoneToDirection( currentLowerArm, rotatedForeDir, ikForeDir, elbowPos, pBones[iLowerArm] );

	// --- Forearm twist sharing ---
	// The hand remains authoritative. Roll the lower arm with the controller so
	// the wrist seam does not absorb all twist, then cap the parent-chain roll.
	bool bLimitWristResidual = ( maxWristTwist >= 0.0f && maxWristTwist < 180.0f );
	bool bCanTwistForearm = ( maxForearmTwist < 0.0f || maxForearmTwist > 0.5f );
	if ( wristTwistShare > 0.001f || ( bLimitWristResidual && bCanTwistForearm ) )
	{
		matrix3x4_t lowerNoTwistDelta, lowerOldInvForBase;
		MatrixInvert( currentLowerArm, lowerOldInvForBase );
		ConcatTransforms( pBones[iLowerArm], lowerOldInvForBase, lowerNoTwistDelta );

		matrix3x4_t baseHandAfterLower;
		ConcatTransforms( lowerNoTwistDelta, pBones[iHand], baseHandAfterLower );

		matrix3x4_t targetHandMat;
		AngleMatrix( targetAng, vec3_origin, targetHandMat );

		float twistAngle = VRIK_SignedMatrixTwistAroundAxis( baseHandAfterLower, targetHandMat, ikForeDir );
		float twistShare = clamp( wristTwistShare, 0.0f, 1.0f );
		float forearmTwistAngle = twistAngle * twistShare;

		if ( bLimitWristResidual )
		{
			float residualWristTwist = twistAngle - forearmTwistAngle;
			float clampedResidualTwist = clamp( residualWristTwist, -maxWristTwist, maxWristTwist );
			forearmTwistAngle = twistAngle - clampedResidualTwist;
		}

		forearmTwistAngle = VRIK_SoftLimitTwistAngle( forearmTwistAngle, maxForearmTwist, maxForearmHardTwist );

		VRIK_ApplyBoneTwist( ikForeDir, forearmTwistAngle, elbowPos, pBones[iLowerArm] );
	}

	VRIK_ClampBoneTwistFromBase( currentLowerArm, ikForeDir, maxForearmHardTwist, pBones[iLowerArm] );

	// Cascade lower arm delta to descendants
	matrix3x4_t lowerDelta, lowerOldInv;
	MatrixInvert( currentLowerArm, lowerOldInv );
	ConcatTransforms( pBones[iLowerArm], lowerOldInv, lowerDelta );

	int skipLower[] = { iLowerArm };
	VRIK_CascadeBoneDelta( pAnim, pHdr, pBones, iLowerArm, lowerDelta, skipLower, 1 );

	// Hand: set rotation from VR angles, position from IK chain
	Vector handWorldPos;
	MatrixPosition( pBones[iHand], handWorldPos );

	matrix3x4_t oldHandAfterCascade;
	MatrixCopy( pBones[iHand], oldHandAfterCascade );

	AngleMatrix( targetAng, handWorldPos, pBones[iHand] );

	matrix3x4_t handDelta, handOldInv;
	MatrixInvert( oldHandAfterCascade, handOldInv );
	ConcatTransforms( pBones[iHand], handOldInv, handDelta );

	int skipHand[] = { iHand };
	VRIK_CascadeBoneDelta( pAnim, pHdr, pBones, iHand, handDelta, skipHand, 1 );

#ifdef CLIENT_DLL
	if ( bDebug )
	{
		debugoverlay->AddLineOverlay( animShoulderPos, elbowPos, 0, 255, 0, true, 0.0f );
		debugoverlay->AddLineOverlay( elbowPos, targetPos, 0, 128, 255, true, 0.0f );
		debugoverlay->AddBoxOverlay( animShoulderPos, Vector(-1,-1,-1), Vector(1,1,1), vec3_angle, 255, 0, 0, 128, 0.0f );
		debugoverlay->AddBoxOverlay( elbowPos, Vector(-1,-1,-1), Vector(1,1,1), vec3_angle, 0, 255, 0, 128, 0.0f );
		debugoverlay->AddBoxOverlay( targetPos, Vector(-1,-1,-1), Vector(1,1,1), vec3_angle, 0, 0, 255, 128, 0.0f );
	}
#endif
}
