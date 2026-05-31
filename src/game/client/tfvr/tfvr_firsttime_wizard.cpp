#include "cbase.h"
#include "tfvr_firsttime_wizard.h"

#include "tfvr_options_dialog.h"
#include "ienginevgui.h"
#include "vgui/ISurface.h"
#include "vgui_controls/CheckButton.h"
#include "vgui_controls/ComboBox.h"
#include "vgui_controls/Slider.h"
#include "vgui_controls/WizardPanel.h"
#include "vgui_controls/WizardSubPanel.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar tfvr_show_firsttime_wizard( "tfvr_show_firsttime_wizard", "1", FCVAR_ARCHIVE, "If enabled, shows the TF2VR first-time setup wizard on VR startup" );

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

	bool ArePumpReloadsAutomatic()
	{
		return GetCvarInt( "tfvr_scattergun_lever_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_sticky_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_bison_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_pomson_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_mangler_pump_reload", 1 ) == 0
			&& GetCvarInt( "tfvr_shotgun_pump_action", 1 ) == 0;
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
	}
}

class CTFVRFirstTimeWelcome : public vgui::WizardSubPanel
{
	DECLARE_CLASS_SIMPLE( CTFVRFirstTimeWelcome, vgui::WizardSubPanel );

public:
	CTFVRFirstTimeWelcome( vgui::Panel *pParent, vgui::WizardSubPanel *pNext )
		: BaseClass( pParent, "TFVRFirstTimeWelcome" )
		, m_pNext( pNext )
	{
		m_pPrimaryHand = new vgui::ComboBox( this, "PrimaryHand", 2, false );
		m_pPrimaryHand->AddItem( "Left", NULL );
		m_pPrimaryHand->AddItem( "Right", NULL );
		m_pPrimaryHand->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_primary_hand", 1 ), 2 ) );

		LoadControlSettings( "resource/TFVRFirstTimeWelcome.res" );
	}

	bool OnFinishButton() OVERRIDE
	{
		ApplySettings();
		return true;
	}

	bool OnNextButton() OVERRIDE
	{
		ApplySettings();
		return true;
	}

	void OnDisplay() OVERRIDE
	{
		if ( GetWizardPanel() )
		{
			GetWizardPanel()->SetFinishButtonEnabled( false );
		}
	}

	vgui::WizardSubPanel *GetNextSubPanel() OVERRIDE { return m_pNext; }

private:
	void ApplySettings()
	{
		SetCvarInt( "tfvr_primary_hand", m_pPrimaryHand->GetActiveItem() );
	}

	vgui::WizardSubPanel *m_pNext;
	vgui::ComboBox *m_pPrimaryHand;
};

class CTFVRFirstTimeMovement : public vgui::WizardSubPanel
{
	DECLARE_CLASS_SIMPLE( CTFVRFirstTimeMovement, vgui::WizardSubPanel );

public:
	CTFVRFirstTimeMovement( vgui::Panel *pParent, vgui::WizardSubPanel *pNext )
		: BaseClass( pParent, "TFVRFirstTimeMovement" )
		, m_pNext( pNext )
	{
		m_pSmoothTurnRateLabel = NULL;
		m_pSnapTurnAngleLabel = NULL;

		m_pLocomotionSource = new vgui::ComboBox( this, "LocomotionSource", 5, false );
		m_pLocomotionSource->AddItem( "Head", NULL );
		m_pLocomotionSource->AddItem( "Primary hand", NULL );
		m_pLocomotionSource->AddItem( "Offhand", NULL );
		m_pLocomotionSource->AddItem( "Left hand", NULL );
		m_pLocomotionSource->AddItem( "Right hand", NULL );
		m_pLocomotionSource->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_locomotion_source", 0 ), 5 ) );

		m_pTurningMode = new vgui::ComboBox( this, "TurningMode", 3, false );
		m_pTurningMode->AddItem( "Disabled", NULL );
		m_pTurningMode->AddItem( "Smooth", NULL );
		m_pTurningMode->AddItem( "Snap", NULL );
		m_pTurningMode->ActivateItem( ClampComboIndex( GetCvarInt( "tfvr_turning_mode", 1 ), 3 ) );
		m_pTurningMode->AddActionSignalTarget( this );

		m_pSnapTurnAngle = new vgui::Slider( this, "SnapTurnAngle" );
		m_pSnapTurnAngle->SetRange( 15, 90 );
		m_pSnapTurnAngle->SetValue( GetCvarInt( "tfvr_snap_turn_angle", 45 ) );

		m_pSmoothTurnRate = new vgui::Slider( this, "SmoothTurnRate" );
		m_pSmoothTurnRate->SetRange( 30, 240 );
		m_pSmoothTurnRate->SetValue( GetCvarInt( "tfvr_smooth_turn_rate", 120 ) );

		m_pComfortVignette = new vgui::CheckButton( this, "ComfortVignette", "Comfort vignette while moving" );
		m_pComfortVignette->SetSelected( GetCvarInt( "tfvr_comfort_vignette_enabled", 0 ) != 0 );

		m_pSeatedMode = new vgui::CheckButton( this, "SeatedMode", "Seated mode" );
		m_pSeatedMode->SetSelected( GetCvarInt( "tfvr_seated_mode", 0 ) != 0 );

		LoadControlSettings( "resource/TFVRFirstTimeMovement.res" );
		m_pSmoothTurnRateLabel = FindChildByName( "SmoothTurnRateLabel" );
		m_pSnapTurnAngleLabel = FindChildByName( "SnapTurnAngleLabel" );
		UpdateTurnControls();
	}

	bool OnFinishButton() OVERRIDE
	{
		ApplySettings();
		return true;
	}

	bool OnNextButton() OVERRIDE
	{
		ApplySettings();
		return true;
	}

	void OnDisplay() OVERRIDE
	{
		if ( GetWizardPanel() )
		{
			GetWizardPanel()->SetFinishButtonEnabled( false );
		}
		UpdateTurnControls();
	}

	vgui::WizardSubPanel *GetNextSubPanel() OVERRIDE { return m_pNext; }

private:
	void ApplySettings()
	{
		SetCvarInt( "tfvr_locomotion_source", m_pLocomotionSource->GetActiveItem() );
		SetCvarInt( "tfvr_turning_mode", m_pTurningMode->GetActiveItem() );
		SetCvarInt( "tfvr_snap_turn_angle", m_pSnapTurnAngle->GetValue() );
		SetCvarInt( "tfvr_smooth_turn_rate", m_pSmoothTurnRate->GetValue() );
		SetCvarInt( "tfvr_comfort_vignette_enabled", m_pComfortVignette->IsSelected() ? 1 : 0 );
		SetCvarInt( "tfvr_seated_mode", m_pSeatedMode->IsSelected() ? 1 : 0 );
	}

	MESSAGE_FUNC( OnTurnModeChanged, "TextChanged" )
	{
		UpdateTurnControls();
	}

	void UpdateTurnControls()
	{
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

	vgui::WizardSubPanel *m_pNext;
	vgui::ComboBox *m_pLocomotionSource;
	vgui::ComboBox *m_pTurningMode;
	vgui::Slider *m_pSnapTurnAngle;
	vgui::Slider *m_pSmoothTurnRate;
	vgui::Panel *m_pSmoothTurnRateLabel;
	vgui::Panel *m_pSnapTurnAngleLabel;
	vgui::CheckButton *m_pComfortVignette;
	vgui::CheckButton *m_pSeatedMode;
};

class CTFVRFirstTimeGameplay : public vgui::WizardSubPanel
{
	DECLARE_CLASS_SIMPLE( CTFVRFirstTimeGameplay, vgui::WizardSubPanel );

public:
	CTFVRFirstTimeGameplay( vgui::Panel *pParent )
		: BaseClass( pParent, "TFVRFirstTimeGameplay" )
	{
		m_pAutomaticReloads = new vgui::CheckButton( this, "AutomaticReloads", "Auto-reload" );
		m_pAutomaticReloads->SetSelected( ArePumpReloadsAutomatic() );

		LoadControlSettings( "resource/TFVRFirstTimeGameplay.res" );
	}

	bool OnFinishButton() OVERRIDE
	{
		SetPumpReloadsAutomatic( m_pAutomaticReloads->IsSelected() );
		return true;
	}

	void OnDisplay() OVERRIDE
	{
		if ( GetWizardPanel() )
		{
			GetWizardPanel()->SetNextButtonEnabled( false );
			GetWizardPanel()->SetFinishButtonEnabled( true );
		}
	}

	vgui::WizardSubPanel *GetNextSubPanel() OVERRIDE { return NULL; }

private:
	vgui::CheckButton *m_pAutomaticReloads;
};

class CTFVRFirstTimeWizard : public vgui::WizardPanel
{
	DECLARE_CLASS_SIMPLE( CTFVRFirstTimeWizard, vgui::WizardPanel );

public:
	CTFVRFirstTimeWizard( vgui::VPANEL parent )
		: BaseClass( NULL, "TFVRFirstTimeWizard" )
	{
		SetParent( parent );
		SetDeleteSelfOnClose( true );
		SetBounds( 0, 0, 512, 400 );
		SetSizeable( false );
		SetTitle( "TF2VR First-Time Setup", true );
	}

	void OnFinishButton() OVERRIDE
	{
		BaseClass::OnFinishButton();
		tfvr_show_firsttime_wizard.SetValue( false );
		engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
		if ( tfvrOptions )
		{
			tfvrOptions->Reload();
		}
	}
};

void ShowTFVRFirstTimeWizard( vgui::VPANEL parent )
{
	CTFVRFirstTimeWizard *pWizard = new CTFVRFirstTimeWizard( parent );

	int x, y, wideWorkspace, tallWorkspace, wide, tall;
	vgui::surface()->GetWorkspaceBounds( x, y, wideWorkspace, tallWorkspace );
	pWizard->GetSize( wide, tall );
	pWizard->SetPos( x + ( wideWorkspace - wide ) / 2, y + ( tallWorkspace - tall ) / 2 );

	CTFVRFirstTimeGameplay *pGameplay = new CTFVRFirstTimeGameplay( pWizard );
	CTFVRFirstTimeMovement *pMovement = new CTFVRFirstTimeMovement( pWizard, pGameplay );
	CTFVRFirstTimeWelcome *pWelcome = new CTFVRFirstTimeWelcome( pWizard, pMovement );

	pWizard->Run( pWelcome );
}

CON_COMMAND( tfvr_launch_firsttime_wizard, "Runs the TF2VR first-time setup wizard again." )
{
	ShowTFVRFirstTimeWizard( enginevgui->GetPanel( PANEL_GAMEUIDLL ) );
}
