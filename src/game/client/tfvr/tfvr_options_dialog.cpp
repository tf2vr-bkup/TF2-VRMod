#include "cbase.h"
#undef PropertySheet
#include "tfvr_options_dialog.h"

#include "ienginevgui.h"
#include "vgui/IScheme.h"
#include "vgui/ISurface.h"
#include "vgui_controls/Button.h"
#include "vgui_controls/CheckButton.h"
#include "vgui_controls/ComboBox.h"
#include "vgui_controls/Label.h"
#include "vgui_controls/PropertyDialog.h"
#include "vgui_controls/PropertyPage.h"
#include "vgui_controls/PropertySheet.h"
#include "vgui_controls/Slider.h"

using namespace vgui;

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{
	int GetCvarInt( const char *pszName, int nDefault )
	{
		ConVarRef cvarRef( pszName );
		return cvarRef.IsValid() ? cvarRef.GetInt() : nDefault;
	}

	float GetCvarFloat( const char *pszName, float flDefault )
	{
		ConVarRef cvarRef( pszName );
		return cvarRef.IsValid() ? cvarRef.GetFloat() : flDefault;
	}

	void SetCvarInt( const char *pszName, int nValue )
	{
		ConVarRef cvarRef( pszName );
		if ( cvarRef.IsValid() )
		{
			cvarRef.SetValue( nValue );
		}
	}

	void SetCvarFloat( const char *pszName, float flValue )
	{
		ConVarRef cvarRef( pszName );
		if ( cvarRef.IsValid() )
		{
			cvarRef.SetValue( flValue );
		}
	}

	int ClampComboIndex( int nValue, int nMaxExclusive )
	{
		return clamp( nValue, 0, nMaxExclusive - 1 );
	}

	int MirrorResolutionToOption( int nHeight )
	{
		switch ( nHeight )
		{
		case 1080: return 1;
		case 1440: return 2;
		case 2160: return 3;
		default: return 0;
		}
	}

	int OptionToMirrorResolution( int nOption )
	{
		switch ( nOption )
		{
		case 1: return 1080;
		case 2: return 1440;
		case 3: return 2160;
		default: return 720;
		}
	}

	int MSAAToOption( int nMSAA )
	{
		switch ( nMSAA )
		{
		case 2: return 1;
		case 4: return 2;
		case 8: return 3;
		default: return 0;
		}
	}

	int OptionToMSAA( int nOption )
	{
		switch ( nOption )
		{
		case 1: return 2;
		case 2: return 4;
		case 3: return 8;
		default: return 0;
		}
	}

	bool ArePumpReloadsAutomatic()
	{
		return GetCvarInt( "tfvr_scattergun_lever_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_sticky_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_bison_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_pomson_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_mangler_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_shotgun_pump_action", 1 ) == 0
			&& GetCvarInt( "tfvr_pistol_manual_reload", 1 ) == 0;
	}

	void SetPumpReloadsAutomatic( bool bAutomatic )
	{
		const int nManualPumpReload = bAutomatic ? 0 : 1;
		SetCvarInt( "tfvr_scattergun_lever_reload", nManualPumpReload );
		SetCvarInt( "tfvr_sticky_pump_reload", nManualPumpReload );
		SetCvarInt( "tfvr_bison_pump_reload", nManualPumpReload );
		SetCvarInt( "tfvr_pomson_pump_reload", nManualPumpReload );
		SetCvarInt( "tfvr_mangler_pump_reload", nManualPumpReload );
		SetCvarInt( "tfvr_shotgun_pump_action", nManualPumpReload );
		SetCvarInt( "tfvr_pistol_manual_reload", nManualPumpReload );
	}
}

class CTFVROptionsSubPage : public PropertyPage
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsSubPage, PropertyPage );

public:
	CTFVROptionsSubPage( Panel *pParent, const char * /*pszPanelName*/ )
		: BaseClass( pParent, nullptr )
	{
		m_iLeftX = 24;
		m_iRightX = 260;
		m_iCurrentX = m_iLeftX;
		m_iCurrentY = 20;
	}

	MESSAGE_FUNC( OnControlModified, "CheckButtonChecked" )
	{
		PostActionSignal( new KeyValues( "ApplyButtonEnable" ) );
	}

	MESSAGE_FUNC( OnComboModified, "TextChanged" )
	{
		PostActionSignal( new KeyValues( "ApplyButtonEnable" ) );
	}

	MESSAGE_FUNC( OnSliderMoved, "SliderMoved" )
	{
		PostActionSignal( new KeyValues( "ApplyButtonEnable" ) );
	}

protected:
	void StartRightColumn()
	{
		m_iCurrentX = m_iRightX;
		m_iCurrentY = 20;
	}

	CheckButton *AddCheck( const char *pszName, const char *pszText )
	{
		CheckButton *pCheck = new CheckButton( this, pszName, pszText );
		pCheck->AddActionSignalTarget( this );
		pCheck->SetBounds( m_iCurrentX, m_iCurrentY, 220, 24 );
		m_iCurrentY += 30;
		return pCheck;
	}

	ComboBox *AddCombo( const char *pszName, const char *pszLabel, int nItems )
	{
		char szLabelName[128];
		Q_snprintf( szLabelName, sizeof( szLabelName ), "%sLabel", pszName );
		Label *pLabel = new Label( this, szLabelName, pszLabel );
		pLabel->SetBounds( m_iCurrentX, m_iCurrentY, 220, 20 );
		m_iCurrentY += 20;

		ComboBox *pCombo = new ComboBox( this, pszName, nItems, false );
		pCombo->AddActionSignalTarget( this );
		pCombo->SetBounds( m_iCurrentX, m_iCurrentY, 180, 24 );
		m_iCurrentY += 34;
		return pCombo;
	}

	Slider *AddSlider( const char *pszName, const char *pszLabel, int nMin, int nMax )
	{
		char szLabelName[128];
		Q_snprintf( szLabelName, sizeof( szLabelName ), "%sLabel", pszName );
		Label *pLabel = new Label( this, szLabelName, pszLabel );
		pLabel->SetBounds( m_iCurrentX, m_iCurrentY, 220, 20 );
		m_iCurrentY += 20;

		Slider *pSlider = new Slider( this, pszName );
		pSlider->AddActionSignalTarget( this );
		pSlider->SetBounds( m_iCurrentX, m_iCurrentY, 180, 32 );
		pSlider->SetRange( nMin, nMax );
		m_iCurrentY += 42;
		return pSlider;
	}

private:
	int m_iLeftX;
	int m_iRightX;
	int m_iCurrentX;
	int m_iCurrentY;
};

class CTFVROptionsSubControls : public CTFVROptionsSubPage
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsSubControls, CTFVROptionsSubPage );

public:
	CTFVROptionsSubControls( Panel *pParent )
		: BaseClass( pParent, "TFVROptionsSubControls" )
	{
		m_pSmoothTurnRate = NULL;
		m_pSnapTurnAngle = NULL;
		m_pSmoothTurnRateLabel = NULL;
		m_pSnapTurnAngleLabel = NULL;

		m_pPrimaryHand = AddCombo( "PrimaryHand", "Primary hand", 2 );
		m_pPrimaryHand->AddItem( "Left", NULL );
		m_pPrimaryHand->AddItem( "Right", NULL );

		m_pLocomotionSource = AddCombo( "LocomotionSource", "Move direction", 5 );
		m_pLocomotionSource->AddItem( "Head", NULL );
		m_pLocomotionSource->AddItem( "Primary hand", NULL );
		m_pLocomotionSource->AddItem( "Offhand", NULL );
		m_pLocomotionSource->AddItem( "Left hand", NULL );
		m_pLocomotionSource->AddItem( "Right hand", NULL );

		m_pTurningMode = AddCombo( "TurningMode", "Turn mode", 3 );
		m_pTurningMode->AddItem( "Disabled", NULL );
		m_pTurningMode->AddItem( "Smooth", NULL );
		m_pTurningMode->AddItem( "Snap", NULL );

		m_pSmoothTurnRate = AddSlider( "SmoothTurnRate", "Smooth turn speed", 30, 240 );
		m_pSnapTurnAngle = AddSlider( "SnapTurnAngle", "Snap turn angle", 15, 90 );

		StartRightColumn();
		m_pMoveSensitivity = AddSlider( "MoveSensitivity", "Movement sensitivity", 25, 200 );
		m_pThumbstickDeadzone = AddSlider( "ThumbstickDeadzone", "Thumbstick deadzone", 0, 50 );
		m_pTurnDeadzone = AddSlider( "TurnDeadzone", "Turn deadzone", 0, 80 );

		LoadControlSettings( "resource/TFVROptionsSubControls.res" );
		m_pSmoothTurnRateLabel = FindChildByName( "SmoothTurnRateLabel" );
		m_pSnapTurnAngleLabel = FindChildByName( "SnapTurnAngleLabel" );
	}

	void OnResetData() OVERRIDE
	{
		m_pPrimaryHand->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_primary_hand", 1 ), 2 ) );
		m_pLocomotionSource->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_locomotion_source", 0 ), 5 ) );
		m_pTurningMode->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_turning_mode", 1 ), 3 ) );
		m_pSmoothTurnRate->SetValue( GetCvarInt( "tfvr_smooth_turn_rate", 120 ) );
		m_pSnapTurnAngle->SetValue( GetCvarInt( "tfvr_snap_turn_angle", 45 ) );
		m_pMoveSensitivity->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_move_sensitivity", 1.0f ) * 100.0f ) );
		m_pThumbstickDeadzone->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_thumbstick_deadzone", 0.1f ) * 100.0f ) );
		m_pTurnDeadzone->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_turn_deadzone", 0.3f ) * 100.0f ) );
		UpdateTurnControls();
	}

	void OnApplyChanges() OVERRIDE
	{
		SetCvarInt( "tfvr_primary_hand", m_pPrimaryHand->GetActiveItem() );
		SetCvarInt( "tfvr_locomotion_source", m_pLocomotionSource->GetActiveItem() );
		SetCvarInt( "tfvr_turning_mode", m_pTurningMode->GetActiveItem() );
		SetCvarInt( "tfvr_smooth_turn_rate", m_pSmoothTurnRate->GetValue() );
		SetCvarInt( "tfvr_snap_turn_angle", m_pSnapTurnAngle->GetValue() );
		SetCvarFloat( "tfvr_move_sensitivity", 0.01f * m_pMoveSensitivity->GetValue() );
		SetCvarFloat( "tfvr_thumbstick_deadzone", 0.01f * m_pThumbstickDeadzone->GetValue() );
		SetCvarFloat( "tfvr_turn_deadzone", 0.01f * m_pTurnDeadzone->GetValue() );
		engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
	}

	MESSAGE_FUNC( OnTurnModeChanged, "TextChanged" )
	{
		UpdateTurnControls();
		PostActionSignal( new KeyValues( "ApplyButtonEnable" ) );
	}

private:
	void UpdateTurnControls()
	{
		if ( !m_pSmoothTurnRate || !m_pSnapTurnAngle )
			return;

		const int nTurningMode = m_pTurningMode->GetActiveItem();
		const bool bShowSmooth = ( nTurningMode == 1 );
		const bool bShowSnap = ( nTurningMode == 2 );

		m_pSmoothTurnRate->SetVisible( bShowSmooth );
		m_pSmoothTurnRate->SetEnabled( bShowSmooth );
		m_pSnapTurnAngle->SetVisible( bShowSnap );
		m_pSnapTurnAngle->SetEnabled( bShowSnap );

		if ( m_pSmoothTurnRateLabel )
		{
			m_pSmoothTurnRateLabel->SetVisible( bShowSmooth );
			m_pSmoothTurnRateLabel->SetEnabled( bShowSmooth );
		}

		if ( m_pSnapTurnAngleLabel )
		{
			m_pSnapTurnAngleLabel->SetVisible( bShowSnap );
			m_pSnapTurnAngleLabel->SetEnabled( bShowSnap );
		}
	}

	ComboBox *m_pPrimaryHand;
	ComboBox *m_pLocomotionSource;
	ComboBox *m_pTurningMode;
	Slider *m_pSmoothTurnRate;
	Slider *m_pSnapTurnAngle;
	Panel *m_pSmoothTurnRateLabel;
	Panel *m_pSnapTurnAngleLabel;
	Slider *m_pMoveSensitivity;
	Slider *m_pThumbstickDeadzone;
	Slider *m_pTurnDeadzone;
};

class CTFVROptionsSubComfort : public CTFVROptionsSubPage
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsSubComfort, CTFVROptionsSubPage );

public:
	CTFVROptionsSubComfort( Panel *pParent )
		: BaseClass( pParent, "TFVROptionsSubComfort" )
	{
		m_pComfortVignette = AddCheck( "ComfortVignette", "Comfort vignette while moving" );
		m_pVignetteStrength = AddSlider( "VignetteStrength", "Vignette strength", 0, 200 );
		m_pPhysicalCrouch = AddCheck( "PhysicalCrouch", "Physical crouch" );
		m_pSeatedMode = AddCheck( "SeatedMode", "Seated mode" );

		StartRightColumn();
		m_pWorldScale = AddSlider( "WorldScale", "World scale", 36, 60 );

		LoadControlSettings( "resource/TFVROptionsSubComfort.res" );
	}

	void OnResetData() OVERRIDE
	{
		m_pComfortVignette->SetSelected( GetCvarInt( "tfvr_comfort_vignette_enabled", 0 ) != 0 );
		m_pVignetteStrength->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_comfort_vignette_strength", 0.5f ) * 100.0f ) );
		m_pPhysicalCrouch->SetSelected( GetCvarInt( "tfvr_physical_crouch", 1 ) != 0 );
		m_pSeatedMode->SetSelected( GetCvarInt( "tfvr_seated_mode", 0 ) != 0 );
		m_pWorldScale->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_worldscale", 48.0f ) ) );
	}

	void OnApplyChanges() OVERRIDE
	{
		SetCvarInt( "tfvr_comfort_vignette_enabled", m_pComfortVignette->IsSelected() ? 1 : 0 );
		SetCvarFloat( "tfvr_comfort_vignette_strength", 0.01f * m_pVignetteStrength->GetValue() );
		SetCvarInt( "tfvr_physical_crouch", m_pPhysicalCrouch->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_seated_mode", m_pSeatedMode->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_worldscale", m_pWorldScale->GetValue() );
		engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
	}

private:
	CheckButton *m_pComfortVignette;
	Slider *m_pVignetteStrength;
	CheckButton *m_pPhysicalCrouch;
	CheckButton *m_pSeatedMode;
	Slider *m_pWorldScale;
};

class CTFVROptionsSubGameplay : public CTFVROptionsSubPage
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsSubGameplay, CTFVROptionsSubPage );

public:
	CTFVROptionsSubGameplay( Panel *pParent )
		: BaseClass( pParent, "TFVROptionsSubGameplay" )
	{
		m_pTwoHandGrip = AddCheck( "TwoHandGrip", "Two-handed weapon grip" );
		m_pOffhandGrip = AddCheck( "OffhandGrip", "Offhand grip aiming" );
		m_pPhysicalThrow = AddCheck( "PhysicalThrow", "Physical throwable weapons" );
		m_pPhysicalBall = AddCheck( "PhysicalBall", "Physical bat/ball interaction" );
		m_pAutomaticReloads = AddCheck( "AutomaticReloads", "Auto-reload" );

		StartRightColumn();
		m_pMouthActivate = AddCheck( "MouthActivate", "Mouth activation for lunchbox items" );
		m_pVoiceGesture = AddCheck( "VoiceGesture", "Walkie-talkie voice gesture" );

		LoadControlSettings( "resource/TFVROptionsSubGameplay.res" );
	}

	void OnResetData() OVERRIDE
	{
		m_pTwoHandGrip->SetSelected( GetCvarInt( "tfvr_twohand_enabled", 1 ) != 0 );
		m_pOffhandGrip->SetSelected( GetCvarInt( "tfvr_offhand_grip_enabled", 1 ) != 0 );
		m_pPhysicalThrow->SetSelected( GetCvarInt( "tfvr_physical_throw", 1 ) != 0 );
		m_pPhysicalBall->SetSelected( GetCvarInt( "tfvr_physical_ball", 1 ) != 0 );
		m_pAutomaticReloads->SetSelected( ArePumpReloadsAutomatic() );
		m_pMouthActivate->SetSelected( GetCvarInt( "tfvr_mouth_activate_enabled", 1 ) != 0 );
		m_pVoiceGesture->SetSelected( GetCvarInt( "tfvr_voice_gesture_enabled", 1 ) != 0 );
	}

	void OnApplyChanges() OVERRIDE
	{
		SetCvarInt( "tfvr_twohand_enabled", m_pTwoHandGrip->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_offhand_grip_enabled", m_pOffhandGrip->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_physical_throw", m_pPhysicalThrow->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_physical_ball", m_pPhysicalBall->IsSelected() ? 1 : 0 );
		SetPumpReloadsAutomatic( m_pAutomaticReloads->IsSelected() );
		SetCvarInt( "tfvr_mouth_activate_enabled", m_pMouthActivate->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_voice_gesture_enabled", m_pVoiceGesture->IsSelected() ? 1 : 0 );
		engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
	}

private:
	CheckButton *m_pTwoHandGrip;
	CheckButton *m_pOffhandGrip;
	CheckButton *m_pPhysicalThrow;
	CheckButton *m_pPhysicalBall;
	CheckButton *m_pAutomaticReloads;
	CheckButton *m_pMouthActivate;
	CheckButton *m_pVoiceGesture;
};

class CTFVROptionsSubVideo : public CTFVROptionsSubPage
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsSubVideo, CTFVROptionsSubPage );

public:
	CTFVROptionsSubVideo( Panel *pParent )
		: BaseClass( pParent, "TFVROptionsSubVideo" )
	{
		m_pMirrorResolution = AddCombo( "MirrorResolution", "Mirror resolution", 4 );
		m_pMirrorResolution->AddItem( "720p", NULL );
		m_pMirrorResolution->AddItem( "1080p", NULL );
		m_pMirrorResolution->AddItem( "1440p", NULL );
		m_pMirrorResolution->AddItem( "2160p", NULL );

		m_pMSAA = AddCombo( "MSAA", "MSAA", 4 );
		m_pMSAA->AddItem( "Off", NULL );
		m_pMSAA->AddItem( "2x", NULL );
		m_pMSAA->AddItem( "4x", NULL );
		m_pMSAA->AddItem( "8x", NULL );

		m_pForceMaxLOD = AddCheck( "ForceMaxLOD", "Force max LOD" );

		StartRightColumn();
		m_pHUDOnMirror = AddCheck( "HUDOnMirror", "Show HUD on mirror" );
		m_pMenuOnMirror = AddCheck( "MenuOnMirror", "Show menus on mirror" );

		LoadControlSettings( "resource/TFVROptionsSubVideo.res" );
	}

	void OnResetData() OVERRIDE
	{
		m_pMirrorResolution->ActivateItem( MirrorResolutionToOption( GetCvarInt( "tfvr_mirror_resolution", 720 ) ) );
		m_pMSAA->ActivateItem( MSAAToOption( GetCvarInt( "tfvr_msaa", 4 ) ) );
		m_pForceMaxLOD->SetSelected( GetCvarInt( "tfvr_forcemaxlod", 1 ) != 0 );
		m_pHUDOnMirror->SetSelected( GetCvarInt( "tfvr_hud_on_mirror", 1 ) != 0 );
		m_pMenuOnMirror->SetSelected( GetCvarInt( "tfvr_menu_on_mirror", 1 ) != 0 );
	}

	void OnApplyChanges() OVERRIDE
	{
		SetCvarInt( "tfvr_mirror_resolution", OptionToMirrorResolution( m_pMirrorResolution->GetActiveItem() ) );
		SetCvarInt( "tfvr_msaa", OptionToMSAA( m_pMSAA->GetActiveItem() ) );
		SetCvarInt( "tfvr_forcemaxlod", m_pForceMaxLOD->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_hud_on_mirror", m_pHUDOnMirror->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_menu_on_mirror", m_pMenuOnMirror->IsSelected() ? 1 : 0 );
		engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
	}

private:
	ComboBox *m_pMirrorResolution;
	ComboBox *m_pMSAA;
	CheckButton *m_pForceMaxLOD;
	CheckButton *m_pHUDOnMirror;
	CheckButton *m_pMenuOnMirror;
};

class CTFVROptionsSubSpectator : public CTFVROptionsSubPage
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsSubSpectator, CTFVROptionsSubPage );

public:
	CTFVROptionsSubSpectator( Panel *pParent )
		: BaseClass( pParent, "TFVROptionsSubSpectator" )
	{
		m_pSpectatorMode = AddCombo( "SpectatorMode", "Spectator camera", 3 );
		m_pSpectatorMode->AddItem( "Off", NULL );
		m_pSpectatorMode->AddItem( "Mirror smoothing", NULL );
		m_pSpectatorMode->AddItem( "Full smoothing", NULL );

		m_pSpectatorEye = AddCombo( "SpectatorEye", "Spectator view eye", 2 );
		m_pSpectatorEye->AddItem( "Left eye", NULL );
		m_pSpectatorEye->AddItem( "Right eye", NULL );

		m_pSpectatorZoom = AddSlider( "SpectatorZoom", "Mirror zoom", 100, 200 );
		m_pRollSmoothing = AddSlider( "RollSmoothing", "Roll smoothing", 10, 2000 );

		StartRightColumn();
		m_pYawPitchSmoothing = AddSlider( "YawPitchSmoothing", "Yaw/pitch smoothing", 10, 1000 );
		m_pSpectatorExtras = AddCheck( "SpectatorExtras", "Spectator status overlays" );

		LoadControlSettings( "resource/TFVROptionsSubSpectator.res" );
	}

	void OnResetData() OVERRIDE
	{
		m_pSpectatorMode->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_spectator_mode", 0 ), 3 ) );
		m_pSpectatorEye->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_spectator_eye", 0 ), 2 ) );
		m_pSpectatorZoom->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_spectator_zoom", 1.1f ) * 100.0f ) );
		m_pRollSmoothing->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_spectator_roll_halflife", 0.5f ) * 1000.0f ) );
		m_pYawPitchSmoothing->SetValue( RoundFloatToInt( GetCvarFloat( "tfvr_spectator_yawpitch_halflife", 0.09f ) * 1000.0f ) );
		m_pSpectatorExtras->SetSelected( GetCvarInt( "tfvr_spectator_extras_enabled", 1 ) != 0 );
	}

	void OnApplyChanges() OVERRIDE
	{
		SetCvarInt( "tfvr_spectator_mode", m_pSpectatorMode->GetActiveItem() );
		SetCvarInt( "tfvr_spectator_eye", m_pSpectatorEye->GetActiveItem() );
		SetCvarFloat( "tfvr_spectator_zoom", 0.01f * m_pSpectatorZoom->GetValue() );
		SetCvarFloat( "tfvr_spectator_roll_halflife", 0.001f * m_pRollSmoothing->GetValue() );
		SetCvarFloat( "tfvr_spectator_yawpitch_halflife", 0.001f * m_pYawPitchSmoothing->GetValue() );
		SetCvarInt( "tfvr_spectator_extras_enabled", m_pSpectatorExtras->IsSelected() ? 1 : 0 );
		engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
	}

private:
	ComboBox *m_pSpectatorMode;
	ComboBox *m_pSpectatorEye;
	Slider *m_pSpectatorZoom;
	Slider *m_pRollSmoothing;
	Slider *m_pYawPitchSmoothing;
	CheckButton *m_pSpectatorExtras;
};

class CTFVROptionsDialog : public PropertyDialog
{
	DECLARE_CLASS_SIMPLE( CTFVROptionsDialog, PropertyDialog );

public:
	CTFVROptionsDialog( VPANEL parent )
		: BaseClass( NULL, "TFVROptionsDialog" )
	{
		SetParent( parent );
		SetDeleteSelfOnClose( false );
		SetBounds( 0, 0, 512, 460 );
		SetSizeable( false );
		SetTitle( "TF2VR Options", true );

		AddPage( new CTFVROptionsSubControls( this ), "Controls" );
		AddPage( new CTFVROptionsSubComfort( this ), "Comfort" );
		AddPage( new CTFVROptionsSubGameplay( this ), "Gameplay" );
		AddPage( new CTFVROptionsSubVideo( this ), "Video / Mirror" );
		AddPage( new CTFVROptionsSubSpectator( this ), "Spectator" );

		SetApplyButtonVisible( true );
		GetPropertySheet()->SetTabWidth( 84 );
	}

	void Activate() OVERRIDE
	{
		BaseClass::Activate();
		EnableApplyButton( false );
	}
};

class CTFVROptionsDialogInterface : public ITFVROptionsDialog
{
public:
	CTFVROptionsDialogInterface()
	{
		m_pOptions = NULL;
	}

	void Create( VPANEL parent ) OVERRIDE
	{
		if ( !m_pOptions )
		{
			m_pOptions = new CTFVROptionsDialog( parent );
		}
	}

	void Destroy() OVERRIDE
	{
		if ( m_pOptions )
		{
			m_pOptions->SetParent( (VPANEL)NULL );
			delete m_pOptions;
			m_pOptions = NULL;
		}
	}

	void Activate() OVERRIDE
	{
		if ( m_pOptions )
		{
			m_pOptions->Activate();

			int x, y, ww, wt, wide, tall;
			vgui::surface()->GetWorkspaceBounds( x, y, ww, wt );
			m_pOptions->GetSize( wide, tall );
			m_pOptions->SetPos( x + ( ww - wide ) / 2, y + ( wt - tall ) / 2 );
		}
	}

	void Reload() OVERRIDE
	{
		if ( m_pOptions )
		{
			m_pOptions->ResetAllData();
		}
	}

private:
	CTFVROptionsDialog *m_pOptions;
};

static CTFVROptionsDialogInterface g_TFVROptionsDialog;
ITFVROptionsDialog *tfvrOptions = &g_TFVROptionsDialog;

CON_COMMAND( opentfvroptions, "Displays the TF2VR Options dialog." )
{
	tfvrOptions->Activate();
}
