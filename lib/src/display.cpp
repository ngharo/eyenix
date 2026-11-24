#include "eyenix/display.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace eyenix {

double mmPerPixel(const Alignment& a) {
    double spacing = a.rightMarkPx - a.leftMarkPx;
    if (!(spacing > 0)) throw std::invalid_argument("alignment marks must be left < right");
    return kMarkerSpacingMm / spacing;
}

DisplayArea displayAreaFromAlignment(const Alignment& a) {
    if (!(a.screenWidthPx > 0 && a.screenHeightPx > 0)) throw std::invalid_argument("screen size must be positive");
    double mm = mmPerPixel(a);
    double trackerPx = (a.leftMarkPx + a.rightMarkPx) / 2; // tracker centre on screen
    double left = -trackerPx * mm;
    double right = (a.screenWidthPx - trackerPx) * mm;
    double height = a.screenHeightPx * mm;

    double t = a.tiltDeg * std::numbers::pi / 180.0, c = std::cos(t), s = std::sin(t);
    // Bottom edge: bottomGap along the screen's up direction (c, s), depth along its normal (-s, c).
    double by = a.bottomGapMm * c - a.screenDepthMm * s;
    double bz = a.bottomGapMm * s + a.screenDepthMm * c;
    double ty = by + height * c, tz = bz + height * s;

    return {{left, ty, tz}, {right, ty, tz}, {left, by, bz}};
}

} // namespace eyenix
