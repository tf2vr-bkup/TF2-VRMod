"resource/ui/CreateServerDialog.res"
{
	"CreateServerDialog"
	{
		"ControlName"	"Frame"
		"fieldName"		"CreateServerDialog"
		"xpos"			"c-150"
		"ypos"			"c-65"
		"wide"			"300"
		"tall"			"130"
		"autoResize"	"0"
		"pinCorner"		"0"
		"visible"		"1"
		"enabled"		"1"
		"tabPosition"	"0"
		"settitlebarvisible"	"1"
	}

	"MapLabel"
	{
		"ControlName"	"Label"
		"fieldName"		"MapLabel"
		"xpos"			"10"
		"ypos"			"30"
		"wide"			"60"
		"tall"			"20"
		"labelText"		"Map:"
		"textAlignment"	"east"
	}

	"MapComboBox"
	{
		"ControlName"	"ComboBox"
		"fieldName"		"MapComboBox"
		"xpos"			"75"
		"ypos"			"30"
		"wide"			"210"
		"tall"			"20"
		"editable"		"0"
	}

	"BotCountLabel"
	{
		"ControlName"	"Label"
		"fieldName"		"BotCountLabel"
		"xpos"			"10"
		"ypos"			"55"
		"wide"			"60"
		"tall"			"20"
		"labelText"		"Bots:"
		"textAlignment"	"east"
	}

	"BotCountComboBox"
	{
		"ControlName"	"ComboBox"
		"fieldName"		"BotCountComboBox"
		"xpos"			"75"
		"ypos"			"55"
		"wide"			"60"
		"tall"			"20"
		"editable"		"0"
	}

	"BotDifficultyLabel"
	{
		"ControlName"	"Label"
		"fieldName"		"BotDifficultyLabel"
		"xpos"			"140"
		"ypos"			"55"
		"wide"			"60"
		"tall"			"20"
		"labelText"		"Difficulty:"
		"textAlignment"	"east"
	}

	"BotDifficultyComboBox"
	{
		"ControlName"	"ComboBox"
		"fieldName"		"BotDifficultyComboBox"
		"xpos"			"205"
		"ypos"			"55"
		"wide"			"80"
		"tall"			"20"
		"editable"		"0"
	}

	"StartButton"
	{
		"ControlName"	"Button"
		"fieldName"		"StartButton"
		"xpos"			"115"
		"ypos"			"90"
		"zpos"			"1"
		"wide"			"80"
		"tall"			"24"
		"autoResize"	"0"
		"pinCorner"		"3"
		"visible"		"1"
		"enabled"		"1"
		"labelText"		"Start"
		"textAlignment"	"center"
		"command"		"Start"
		"default"		"1"
	}

	"CancelButton"
	{
		"ControlName"	"Button"
		"fieldName"		"CancelButton"
		"xpos"			"200"
		"ypos"			"90"
		"zpos"			"1"
		"wide"			"80"
		"tall"			"24"
		"autoResize"	"0"
		"pinCorner"		"3"
		"visible"		"1"
		"enabled"		"1"
		"labelText"		"Cancel"
		"textAlignment"	"center"
		"command"		"Cancel"
	}
}
