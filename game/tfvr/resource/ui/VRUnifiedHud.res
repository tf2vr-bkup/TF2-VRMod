"Resource/UI/VRUnifiedHud.res"
{
	"VRUnifiedHud"
	{
		"ControlName"		"EditablePanel"
		"fieldName"			"VRUnifiedHud"
		"xpos"				"0"
		"ypos"				"0"
		"wide"				"400"
		"tall"				"300"
		"visible"			"1"
		"enabled"			"1"
		"bgcolor_override"	"0 0 0 0"		// Transparent background
		"paintborder"		"0"
	}
	
	// Just use the existing HUD elements - much simpler!
	"HudPlayerStatus"
	{
		"ControlName"		"CTFHudPlayerStatus"
		"fieldName"			"HudPlayerStatus"
		"xpos"				"c-100"		// Center the player status
		"ypos"				"20"		// Top section
		"wide"				"200"
		"tall"				"120"
		"visible"			"1"
		"enabled"			"1"
	}
	
	"HudObjectiveStatus"
	{
		"ControlName"		"CTFHudObjectiveStatus"
		"fieldName"			"HudObjectiveStatus"
		"xpos"				"c-150"		// Center the objectives
		"ypos"				"160"		// Below player status
		"wide"				"300"
		"tall"				"120"
		"visible"			"1"
		"enabled"			"1"
	}
}
