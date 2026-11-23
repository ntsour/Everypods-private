#include "AirPodsIcon.hpp"

#include <algorithm>
#include <mutex>

// gdiplus.h uses min/max, which NOMINMAX removes.
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace Propods {

namespace {

// GDI+ is started once and left running for the life of the process.
bool ensureGdiplus() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    });
    return ok;
}

void drawAirPods(Gdiplus::Graphics& g, Gdiplus::REAL x0, Gdiplus::REAL y0, Gdiplus::REAL W, Gdiplus::REAL H,
                 const Gdiplus::Color& c) {
    using namespace Gdiplus;
    SolidBrush brush(c);
    const REAL d = W * 0.40f;          // bud diameter
    const REAL stemW = W * 0.15f;
    Pen stem(c, stemW);
    stem.SetStartCap(LineCapRound);
    stem.SetEndCap(LineCapRound);
    for (int side = 0; side < 2; ++side) {
        const REAL dir = side == 0 ? 1.0f : -1.0f;        // +1: left bud, ear tip points right
        const REAL cx = x0 + (side == 0 ? W * 0.27f : W * 0.73f);
        const REAL cy = y0 + H * 0.28f;
        g.FillEllipse(&brush, cx - d / 2, cy - d / 2, d, d);
        const REAL tip = d * 0.5f;                        // ear tip, towards the middle
        g.FillEllipse(&brush, cx + dir * d * 0.42f - tip / 2, cy - tip / 2 - d * 0.05f, tip, tip);
        const REAL sx = cx - dir * d * 0.20f;             // stem on the outer side
        g.DrawLine(&stem, sx, cy, sx - dir * W * 0.03f, y0 + H * 0.90f);
    }
}

}  // namespace

HICON createAirPodsIcon(int sizePx, COLORREF color) {
    if (sizePx <= 0 || !ensureGdiplus()) return nullptr;
    using namespace Gdiplus;
    Bitmap bmp(sizePx, sizePx, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Ok) return nullptr;
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));
        const REAL s = static_cast<REAL>(sizePx);
        const REAL m = s * 0.04f;  // small margin so round caps are not clipped
        drawAirPods(g, m, m, s - 2 * m, s - 2 * m,
                    Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    }
    HICON icon = nullptr;
    if (bmp.GetHICON(&icon) != Ok) return nullptr;
    return icon;
}

}
