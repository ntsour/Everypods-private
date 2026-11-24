#pragma once

// The tray icon: a pair of AirPods Pro, drawn at run time in one colour.
//
// The Mac app's menu-bar icon is Apple's SF Symbol "airpodspro", which may
// not be used outside Apple platforms, so this is our own drawing of the same
// subject (two buds with ear tips and stems), antialiased with GDI+.

#include <windows.h>

namespace Propods {

// A sizePx x sizePx icon, transparent background, glyph in `color`.
// The caller owns it (DestroyIcon). nullptr on failure.
HICON createAirPodsIcon(int sizePx, COLORREF color);

}
