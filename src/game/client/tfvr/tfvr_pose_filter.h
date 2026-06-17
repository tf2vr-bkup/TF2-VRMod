#ifndef TFVR_POSE_FILTER_H
#define TFVR_POSE_FILTER_H

#include "../public/openxr/openxr.h"
#include <math.h>

// Lightweight 1Euro filter for OpenXR poses. Positions are filtered per axis;
// orientations are filtered by angular speed and shortest-path slerp.
class CTFVROneEuroFloatFilter
{
public:
    CTFVROneEuroFloatFilter()
    {
        Reset();
    }

    void Reset()
    {
        m_bInitialized = false;
        m_flLastRaw = 0.0f;
        m_flLastFiltered = 0.0f;
        m_flLastDerivative = 0.0f;
        m_flLastTimestamp = 0.0;
    }

    float Filter(float flValue, double flTimestamp, float flMinCutoff, float flBeta, float flDerivativeCutoff)
    {
        if (!m_bInitialized)
        {
            m_bInitialized = true;
            m_flLastRaw = flValue;
            m_flLastFiltered = flValue;
            m_flLastDerivative = 0.0f;
            m_flLastTimestamp = flTimestamp;
            return flValue;
        }

        const double flDeltaTime = flTimestamp - m_flLastTimestamp;
        if (flDeltaTime <= 0.0 || flDeltaTime > 0.25)
        {
            m_flLastRaw = flValue;
            m_flLastFiltered = flValue;
            m_flLastDerivative = 0.0f;
            m_flLastTimestamp = flTimestamp;
            return flValue;
        }

        const float flDerivative = (flValue - m_flLastRaw) / static_cast<float>(flDeltaTime);
        const float flDerivativeAlpha = GetAlpha(static_cast<float>(flDeltaTime), flDerivativeCutoff);
        m_flLastDerivative = Lerp(m_flLastDerivative, flDerivative, flDerivativeAlpha);

        const float flCutoff = MaxFloat(0.0001f, flMinCutoff + flBeta * fabsf(m_flLastDerivative));
        const float flAlpha = GetAlpha(static_cast<float>(flDeltaTime), flCutoff);
        m_flLastFiltered = Lerp(m_flLastFiltered, flValue, flAlpha);
        m_flLastRaw = flValue;
        m_flLastTimestamp = flTimestamp;
        return m_flLastFiltered;
    }

    static float GetAlpha(float flDeltaTime, float flCutoff)
    {
        const float flPi = 3.14159265358979323846f;
        const float flSafeCutoff = MaxFloat(0.0001f, flCutoff);
        return 1.0f / (1.0f + (1.0f / (2.0f * flPi * flSafeCutoff)) / MaxFloat(0.0001f, flDeltaTime));
    }

    static float MaxFloat(float a, float b)
    {
        return a > b ? a : b;
    }

private:
    static float Lerp(float flFrom, float flTo, float flT)
    {
        return flFrom + (flTo - flFrom) * flT;
    }

    bool m_bInitialized;
    float m_flLastRaw;
    float m_flLastFiltered;
    float m_flLastDerivative;
    double m_flLastTimestamp;
};

class CTFVRPoseOneEuroFilter
{
public:
    CTFVRPoseOneEuroFilter()
    {
        Reset();
    }

    void Reset()
    {
        for (int i = 0; i < 3; ++i)
            m_positionFilters[i].Reset();

        m_bHasOrientation = false;
        m_flLastOrientationTimestamp = 0.0;
        m_orientationSpeedFilter.Reset();
        SetIdentity(m_lastRawOrientation);
        SetIdentity(m_filteredOrientation);
    }

    XrPosef Filter(const XrPosef& pose, double flTimestamp, float flMinCutoff, float flBeta, float flDerivativeCutoff)
    {
        XrPosef filteredPose = pose;

        filteredPose.position.x = m_positionFilters[0].Filter(pose.position.x, flTimestamp, flMinCutoff, flBeta, flDerivativeCutoff);
        filteredPose.position.y = m_positionFilters[1].Filter(pose.position.y, flTimestamp, flMinCutoff, flBeta, flDerivativeCutoff);
        filteredPose.position.z = m_positionFilters[2].Filter(pose.position.z, flTimestamp, flMinCutoff, flBeta, flDerivativeCutoff);
        filteredPose.orientation = FilterOrientation(pose.orientation, flTimestamp, flMinCutoff, flBeta, flDerivativeCutoff);

        return filteredPose;
    }

private:
    static void SetIdentity(XrQuaternionf& quat)
    {
        quat.x = 0.0f;
        quat.y = 0.0f;
        quat.z = 0.0f;
        quat.w = 1.0f;
    }

    static float QuaternionDot(const XrQuaternionf& a, const XrQuaternionf& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    static void NormalizeQuaternion(XrQuaternionf& quat, const XrQuaternionf& fallback)
    {
        const float flLengthSqr = QuaternionDot(quat, quat);
        if (flLengthSqr <= 0.000001f)
        {
            quat = fallback;
            return;
        }

        const float flInvLength = 1.0f / sqrtf(flLengthSqr);
        quat.x *= flInvLength;
        quat.y *= flInvLength;
        quat.z *= flInvLength;
        quat.w *= flInvLength;
    }

    static XrQuaternionf NormalizeQuaternionCopy(const XrQuaternionf& quat)
    {
        XrQuaternionf result = quat;
        XrQuaternionf identity;
        SetIdentity(identity);
        NormalizeQuaternion(result, identity);
        return result;
    }

    static XrQuaternionf SlerpShortestPath(const XrQuaternionf& from, const XrQuaternionf& to, float flT)
    {
        XrQuaternionf target = to;
        float flDot = QuaternionDot(from, target);
        if (flDot < 0.0f)
        {
            target.x = -target.x;
            target.y = -target.y;
            target.z = -target.z;
            target.w = -target.w;
            flDot = -flDot;
        }

        flDot = ClampFloat(flDot, 0.0f, 1.0f);
        flT = ClampFloat(flT, 0.0f, 1.0f);

        XrQuaternionf result;
        if (flDot > 0.9995f)
        {
            result.x = from.x + (target.x - from.x) * flT;
            result.y = from.y + (target.y - from.y) * flT;
            result.z = from.z + (target.z - from.z) * flT;
            result.w = from.w + (target.w - from.w) * flT;
            NormalizeQuaternion(result, target);
            return result;
        }

        const float flTheta0 = acosf(flDot);
        const float flTheta = flTheta0 * flT;
        const float flSinTheta0 = sinf(flTheta0);
        const float flSinTheta = sinf(flTheta);
        const float flScaleFrom = cosf(flTheta) - flDot * flSinTheta / flSinTheta0;
        const float flScaleTo = flSinTheta / flSinTheta0;

        result.x = from.x * flScaleFrom + target.x * flScaleTo;
        result.y = from.y * flScaleFrom + target.y * flScaleTo;
        result.z = from.z * flScaleFrom + target.z * flScaleTo;
        result.w = from.w * flScaleFrom + target.w * flScaleTo;
        NormalizeQuaternion(result, target);
        return result;
    }

    static float ClampFloat(float flValue, float flMin, float flMax)
    {
        if (flValue < flMin)
            return flMin;
        if (flValue > flMax)
            return flMax;
        return flValue;
    }

    XrQuaternionf FilterOrientation(const XrQuaternionf& orientation, double flTimestamp, float flMinCutoff, float flBeta, float flDerivativeCutoff)
    {
        XrQuaternionf raw = NormalizeQuaternionCopy(orientation);
        if (!m_bHasOrientation)
        {
            m_bHasOrientation = true;
            m_flLastOrientationTimestamp = flTimestamp;
            m_lastRawOrientation = raw;
            m_filteredOrientation = raw;
            return raw;
        }

        const float flDeltaTime = static_cast<float>(flTimestamp - m_flLastOrientationTimestamp);
        if (flDeltaTime <= 0.0f || flDeltaTime > 0.25f)
        {
            m_orientationSpeedFilter.Reset();
            m_flLastOrientationTimestamp = flTimestamp;
            m_lastRawOrientation = raw;
            m_filteredOrientation = raw;
            return raw;
        }

        const float flDot = ClampFloat(fabsf(QuaternionDot(m_lastRawOrientation, raw)), 0.0f, 1.0f);
        const float flAngularSpeed = (2.0f * acosf(flDot)) / flDeltaTime;
        const float flFilteredAngularSpeed = m_orientationSpeedFilter.Filter(flAngularSpeed, flTimestamp, flDerivativeCutoff, 0.0f, flDerivativeCutoff);
        const float flCutoff = CTFVROneEuroFloatFilter::MaxFloat(0.0001f, flMinCutoff + flBeta * flFilteredAngularSpeed);
        const float flAlpha = CTFVROneEuroFloatFilter::GetAlpha(flDeltaTime, flCutoff);

        m_filteredOrientation = SlerpShortestPath(m_filteredOrientation, raw, flAlpha);
        m_lastRawOrientation = raw;
        m_flLastOrientationTimestamp = flTimestamp;
        return m_filteredOrientation;
    }

    CTFVROneEuroFloatFilter m_positionFilters[3];
    CTFVROneEuroFloatFilter m_orientationSpeedFilter;
    bool m_bHasOrientation;
    double m_flLastOrientationTimestamp;
    XrQuaternionf m_lastRawOrientation;
    XrQuaternionf m_filteredOrientation;
};

#endif // TFVR_POSE_FILTER_H
