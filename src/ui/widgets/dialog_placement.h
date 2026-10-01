#pragma once

class QDialog;

// Settings > Windows > "Center dialogs on screen". Call this immediately after
// a dialog restored its remembered geometry: with the setting on, the dialog is
// re-centred on the screen it will actually appear on, so a window last used
// on a monitor that is no longer attached comes back in the middle of the
// screen that is left instead of somewhere off-view. With the setting off
// (the default, and what an untouched install sees) the remembered position is
// left exactly as restored.
void center_dialog_on_screen(QDialog *dialog);
