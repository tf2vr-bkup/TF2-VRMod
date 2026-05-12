#ifndef TFVR_OPTIONS_DIALOG_H
#define TFVR_OPTIONS_DIALOG_H

#include "vgui/VGUI.h"

class ITFVROptionsDialog
{
public:
	virtual void Create( vgui::VPANEL parent ) = 0;
	virtual void Destroy() = 0;
	virtual void Activate() = 0;
	virtual void Reload() = 0;
};

extern ITFVROptionsDialog *tfvrOptions;

#endif // TFVR_OPTIONS_DIALOG_H
