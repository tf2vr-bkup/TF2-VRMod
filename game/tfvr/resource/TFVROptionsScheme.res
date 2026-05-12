// TFVR Options Dialog scheme.
//
// We deliberately do NOT use the TF2 ClientScheme/SourceScheme overrides
// (which paint everything with tan/orange Mannco styling). Instead we base
// off the stock SourceSchemeBase shipped with the engine and only tweak a
// few colors, so the dialog matches HL2-style stock GameUI Options panels.
#base "SourceSchemeBase.res"

Scheme
{
	Colors
	{
		"DialogBG"			"58 60 64 245"
		"ControlPlainBG"	"82 84 88 255"
		"PlainText"			"235 235 235 255"
		"PlainTextDim"		"180 180 180 255"
		"PlainAccent"		"110 145 195 255"
		"PlainSelectionBG"	"60 80 110 255"
	}

	BaseSettings
	{
		Frame.BgColor					"DialogBG"
		Frame.OutOfFocusBgColor			"DialogBG"
		FrameTitleBar.TextColor			"PlainText"
		FrameTitleBar.DisabledTextColor	"PlainTextDim"
		FrameTitleButton.FgColor		"PlainText"
		FrameTitleButton.DisabledFgColor "PlainTextDim"
		FrameSystemButton.FgColor		"Blank"
		FrameSystemButton.BgColor		"Blank"
		FrameSystemButton.Icon			""
		FrameSystemButton.DisabledIcon	""

		Border.Bright					"100 100 100 196"
		Border.Dark						"30 30 30 196"
		Border.Selection				"PlainAccent"

		Button.TextColor				"PlainText"
		Button.BgColor					"ControlPlainBG"
		Button.ArmedTextColor			"PlainText"
		Button.ArmedBgColor				"112 116 122 255"
		Button.DepressedTextColor		"PlainText"
		Button.DepressedBgColor			"66 70 76 255"
		Button.FocusBorderColor			"PlainAccent"

		CheckButton.TextColor			"PlainText"
		CheckButton.SelectedTextColor	"PlainText"
		CheckButton.BgColor				"38 40 44 255"
		CheckButton.Border1				"Border.Dark"
		CheckButton.Border2				"Border.Bright"
		CheckButton.Check				"PlainText"
		CheckButton.HighlightFgColor	"PlainTextDim"
		CheckButton.ArmedBgColor		"Blank"
		CheckButton.DepressedBgColor	"Blank"
		CheckButton.DisabledBgColor		"38 40 44 255"

		ToggleButton.SelectedTextColor	"PlainText"

		ComboBoxButton.ArrowColor		"PlainText"
		ComboBoxButton.ArmedArrowColor	"PlainText"
		ComboBoxButton.BgColor			"Blank"
		ComboBoxButton.DisabledBgColor	"Blank"

		Label.TextDullColor				"PlainTextDim"
		Label.TextColor					"PlainText"
		Label.TextBrightColor			"PlainText"
		Label.SelectedTextColor			"PlainText"
		Label.BgColor					"Blank"
		Label.DisabledFgColor1			"PlainTextDim"
		Label.DisabledFgColor2			"Blank"

		ListPanel.TextColor					"PlainText"
		ListPanel.BgColor					"38 40 44 255"
		ListPanel.SelectedBgColor			"PlainSelectionBG"
		ListPanel.SelectedOutOfFocusBgColor	"60 64 70 255"

		Menu.TextInset				"6"
		Menu.FgColor				"PlainText"
		Menu.BgColor				"38 40 44 255"
		Menu.ArmedFgColor			"PlainText"
		Menu.ArmedBgColor			"PlainSelectionBG"
		Menu.DividerColor			"Border.Dark"

		PropertySheet.TextColor			"PlainText"
		PropertySheet.SelectedTextColor	"PlainText"
		PropertySheet.TransitionEffectTime "0"
		PropertySheet.SelectionTextShiftX "0"
		PropertySheet.SelectionTextShiftY "0"

		ScrollBarButton.FgColor				"PlainText"
		ScrollBarButton.BgColor				"ControlPlainBG"
		ScrollBarButton.ArmedFgColor		"PlainText"
		ScrollBarButton.ArmedBgColor		"112 116 122 255"
		ScrollBarButton.DepressedFgColor	"PlainText"
		ScrollBarButton.DepressedBgColor	"66 70 76 255"

		ScrollBarSlider.BgColor				"38 40 44 255"
		ScrollBarSlider.FgColor				"ControlPlainBG"

		Slider.NobColor				"ControlPlainBG"
		Slider.TextColor			"PlainText"
		Slider.TrackColor			"38 40 44 255"
		Slider.DisabledTextColor1	"PlainTextDim"
		Slider.DisabledTextColor2	"Blank"

		TextEntry.TextColor			"PlainText"
		TextEntry.BgColor			"38 40 44 255"
		TextEntry.DisabledTextColor	"PlainTextDim"
		TextEntry.SelectedBgColor	"PlainSelectionBG"

		MainMenu.TextColor			"PlainText"
		MainMenu.ArmedTextColor		"PlainTextDim"
		MainMenu.Inset				"32"
	}

	Fonts
	{
		"DefaultLarge"
		{
			"1"
			{
				"name"		"Tahoma"
				"tall"		"18"
				"weight"	"500"
				"antialias"	"1"
			}
		}
	}
}
