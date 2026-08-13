#pragma once

// Umbrella header for the right-panel tab widgets. The original tab_panels.{h,cpp}
// god file was split into one file pair per class (downloads_tab, plugins_tab,
// archives_tab, data_tab, saves_tab, conflicts_tab); this header keeps the
// existing include sites working. Prefer including the specific panel header
// you need.

#include "ui/panels/archives_tab.h"
#include "ui/panels/conflicts_tab.h"
#include "ui/panels/data_tab.h"
#include "ui/panels/downloads_tab.h"
#include "ui/panels/plugins_tab.h"
#include "ui/panels/saves_tab.h"
