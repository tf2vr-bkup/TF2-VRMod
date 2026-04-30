//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: VR comfort vignette post overlay
//
//=============================================================================//

#include "BaseVSShader.h"

#include "sdk_screenspaceeffect_vs20.inc"
#include "post_vignette_ps20b.inc"

BEGIN_VS_SHADER_FLAGS( Post_Vignette, "VR comfort vignette", SHADER_NOT_EDITABLE )
	BEGIN_SHADER_PARAMS
		SHADER_PARAM( VOPACITY, SHADER_PARAM_TYPE_FLOAT, "0.5", "Opacity of vignette" )
		SHADER_PARAM( VINNERRADIUS, SHADER_PARAM_TYPE_FLOAT, "0.4", "Inner radius of clear area" )
		SHADER_PARAM( VOUTERRADIUS, SHADER_PARAM_TYPE_FLOAT, "0.7", "Outer radius of occluded area" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
	}

	SHADER_INIT
	{
	}

	SHADER_FALLBACK
	{
		return 0;
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			pShaderShadow->EnableDepthWrites( false );
			pShaderShadow->EnableDepthTest( false );
			pShaderShadow->VertexShaderVertexFormat( VERTEX_POSITION, 1, 0, 0 );

			DECLARE_STATIC_VERTEX_SHADER( sdk_screenspaceeffect_vs20 );
			SET_STATIC_VERTEX_SHADER( sdk_screenspaceeffect_vs20 );

			DECLARE_STATIC_PIXEL_SHADER( post_vignette_ps20b );
			SET_STATIC_PIXEL_SHADER( post_vignette_ps20b );

			pShaderShadow->EnableBlending( true );
			pShaderShadow->BlendFunc( SHADER_BLEND_SRC_ALPHA, SHADER_BLEND_ONE_MINUS_SRC_ALPHA );
			pShaderShadow->BlendOp( SHADER_BLEND_OP_ADD );
		}
		DYNAMIC_STATE
		{
			DECLARE_DYNAMIC_VERTEX_SHADER( sdk_screenspaceeffect_vs20 );
			SET_DYNAMIC_VERTEX_SHADER( sdk_screenspaceeffect_vs20 );

			DECLARE_DYNAMIC_PIXEL_SHADER( post_vignette_ps20b );
			SET_DYNAMIC_PIXEL_SHADER( post_vignette_ps20b );

			float vOpacity[4] = { params[VOPACITY]->GetFloatValue(), 0.0f, 0.0f, 0.0f };
			pShaderAPI->SetPixelShaderConstant( 0, vOpacity );

			float vInnerRadius[4] = { params[VINNERRADIUS]->GetFloatValue(), 0.0f, 0.0f, 0.0f };
			pShaderAPI->SetPixelShaderConstant( 1, vInnerRadius );

			float vOuterRadius[4] = { params[VOUTERRADIUS]->GetFloatValue(), 0.0f, 0.0f, 0.0f };
			pShaderAPI->SetPixelShaderConstant( 2, vOuterRadius );
		}
		Draw();
	}
END_SHADER
