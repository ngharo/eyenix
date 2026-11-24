// Where the screen is, in the tracker's coordinate system.
#pragma once

namespace eyenix {

struct Point2 { double x = 0, y = 0; };
struct Point3 { double x = 0, y = 0, z = 0; };

/// Three corners of the active display area, millimetres, tracker coordinates
/// (x right, y up, z towards the user, origin at the centre of the tracker).
struct DisplayArea {
    Point3 topLeft, topRight, bottomLeft;
};

/// Distance between the two alignment marks on the Eye Tracker 5 housing.
/// Taken from the tobii-linux project; not yet measured on a device here.
inline constexpr double kMarkerSpacingMm = 184.0;

/// Result of the on-screen alignment step: the user moves two vertical lines
/// at the bottom of the screen until they sit above the tracker's marks.
struct Alignment {
    double screenWidthPx = 0, screenHeightPx = 0;
    double leftMarkPx = 0, rightMarkPx = 0; // x of each line, screen pixels

    // Mounting geometry. Defaults are what Tobii's Windows software sent for a
    // tracker attached to the bottom bezel (captures/04, 05).
    double tiltDeg = 20.0;       // screen plane relative to the tracker's y axis
    double bottomGapMm = 8.59;   // tracker origin to bottom of the picture, along the screen
    double screenDepthMm = -6.42; // screen plane in front (+) or behind (-) the tracker origin
};

/// Physical size of one pixel implied by the alignment.
double mmPerPixel(const Alignment& a);

DisplayArea displayAreaFromAlignment(const Alignment& a);

} // namespace eyenix
