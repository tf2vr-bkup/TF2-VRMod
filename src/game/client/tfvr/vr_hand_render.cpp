// Purpose: VR Hand Rendering Layer implementation

#include "cbase.h"
#include "vr_hand_render.h"
#include "iclientrenderable.h"
#include "sourcevr/isourcevirtualreality.h"

static ConVar tfvr_hand_layer("tfvr_hand_layer", "100", FCVAR_ARCHIVE,
	"Render VR hands on a separate layer from the world (enables close inspection and future sniper scope)");

static ConVar tfvr_hand_layer_znear("tfvr_hand_layer_znear", ".5", FCVAR_ARCHIVE,
	"Z-near override for VR hand layer (0 = use world zNear). "
	"Uses DepthRange to linearly remap depth for exact occlusion. "
	"Requires d3d9.useD32forD24 = True in dxvk.conf for close-range precision.");

static ConVar tfvr_hand_layer_zfar("tfvr_hand_layer_zfar", "500", FCVAR_ARCHIVE,
	"Z-far override for VR hand layer (0 = use world zFar). "
	"A tighter zFar (e.g. 500-2000) concentrates depth precision "
	"around arm's reach for cleaner close-up inspection.");

static bool g_bWorldPassActive = false;
static bool g_bHandPassActive = false;
static CUtlVector<IClientRenderable*> g_HandLayerBaseRenderables;
static CUtlVector<IClientRenderable*> g_HandLayerRenderables;
static CUtlVector<IClientRenderable*> g_HandLayerLateRenderables;
static CUtlVector<C_BaseEntity*> g_ParticleOwners;
static CUtlVector<IClientRenderable*> g_DeferredParticles;

void VRHandLayer_BeginWorldPass()
{
	g_bWorldPassActive = true;
	g_bHandPassActive = false;
	g_HandLayerBaseRenderables.RemoveAll();
	g_HandLayerRenderables.RemoveAll();
	g_HandLayerLateRenderables.RemoveAll();
	g_ParticleOwners.RemoveAll();
	g_DeferredParticles.RemoveAll();
}

void VRHandLayer_EndWorldPass()
{
	g_bWorldPassActive = false;
}

bool VRHandLayer_ShouldSkipDraw()
{
	return tfvr_hand_layer.GetBool() && g_bWorldPassActive && !g_bHandPassActive;
}

bool VRHandLayer_IsHandPass()
{
	return g_bHandPassActive;
}

void VRHandLayer_AddRenderable(IClientRenderable *pRenderable)
{
	if (pRenderable && g_HandLayerRenderables.Find(pRenderable) == g_HandLayerRenderables.InvalidIndex())
	{
		g_HandLayerRenderables.AddToTail(pRenderable);
	}
}

void VRHandLayer_AddBaseRenderable(IClientRenderable *pRenderable)
{
	if (pRenderable && g_HandLayerBaseRenderables.Find(pRenderable) == g_HandLayerBaseRenderables.InvalidIndex())
	{
		g_HandLayerBaseRenderables.AddToTail(pRenderable);
	}
}

int VRHandLayer_GetBaseRenderableCount()
{
	return g_HandLayerBaseRenderables.Count();
}

IClientRenderable *VRHandLayer_GetBaseRenderable(int index)
{
	if (index >= 0 && index < g_HandLayerBaseRenderables.Count())
		return g_HandLayerBaseRenderables[index];
	return NULL;
}

void VRHandLayer_AddLateRenderable(IClientRenderable *pRenderable)
{
	if (pRenderable && g_HandLayerLateRenderables.Find(pRenderable) == g_HandLayerLateRenderables.InvalidIndex())
	{
		g_HandLayerLateRenderables.AddToTail(pRenderable);
	}
}

int VRHandLayer_GetLateRenderableCount()
{
	return g_HandLayerLateRenderables.Count();
}

IClientRenderable *VRHandLayer_GetLateRenderable(int index)
{
	if (index >= 0 && index < g_HandLayerLateRenderables.Count())
		return g_HandLayerLateRenderables[index];
	return NULL;
}

int VRHandLayer_GetRenderableCount()
{
	return g_HandLayerRenderables.Count();
}

IClientRenderable *VRHandLayer_GetRenderable(int index)
{
	if (index >= 0 && index < g_HandLayerRenderables.Count())
		return g_HandLayerRenderables[index];
	return NULL;
}

void VRHandLayer_ClearRenderables()
{
	g_HandLayerBaseRenderables.RemoveAll();
	g_HandLayerRenderables.RemoveAll();
	g_HandLayerLateRenderables.RemoveAll();
}

bool VRHandLayer_IsEnabled()
{
	return UseVR() && tfvr_hand_layer.GetBool();
}

float VRHandLayer_GetZNearOverride()
{
	return tfvr_hand_layer_znear.GetFloat();
}

float VRHandLayer_GetZFarOverride()
{
	return tfvr_hand_layer_zfar.GetFloat();
}

void VRHandLayer_SetHandPassActive(bool bActive)
{
	g_bHandPassActive = bActive;
}

void VRHandLayer_AddParticleOwner(C_BaseEntity *pEntity)
{
	if (pEntity && g_ParticleOwners.Find(pEntity) == g_ParticleOwners.InvalidIndex())
	{
		g_ParticleOwners.AddToTail(pEntity);
	}
}

bool VRHandLayer_IsParticleOwner(C_BaseEntity *pEntity)
{
	return pEntity && g_ParticleOwners.Find(pEntity) != g_ParticleOwners.InvalidIndex();
}

void VRHandLayer_AddDeferredParticle(IClientRenderable *pParticle)
{
	if (pParticle && g_DeferredParticles.Find(pParticle) == g_DeferredParticles.InvalidIndex())
	{
		g_DeferredParticles.AddToTail(pParticle);
	}
}

int VRHandLayer_GetDeferredParticleCount()
{
	return g_DeferredParticles.Count();
}

IClientRenderable *VRHandLayer_GetDeferredParticle(int index)
{
	if (index >= 0 && index < g_DeferredParticles.Count())
		return g_DeferredParticles[index];
	return NULL;
}

void VRHandLayer_ClearDeferredParticles()
{
	g_DeferredParticles.RemoveAll();
}
