// Head position and rotation from the tracker's camera frames and eye positions.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "eyenix/display.hpp"
#include "eyenix/tracker.hpp"

namespace eyenix {

/// Where the head is and which way it faces, relative to the screen.
/// Axes follow the screen: x along its top edge, y up its side, z out of it towards the user.
struct HeadPose {
    std::uint64_t timestampUs = 0;
    bool positionValid = false;
    Point3 positionMm;                      // midpoint between the eyes, from the screen centre
    double pitch = 0, yaw = 0, roll = 0;    // radians; all zero when facing the screen squarely
    float presence = 0;                     // 0..1, how sure the face model is that it sees a face
};

/// Finds a face in each camera frame and works out the head pose.
/// Each frame costs a few milliseconds of CPU. Not thread-safe.
class HeadTracker {
public:
    /// modelDir holds face_detector.onnx and face_landmarks_detector.onnx
    /// (see tools/convert_models.py). Throws std::runtime_error if they cannot be loaded.
    explicit HeadTracker(const std::string& modelDir);
    ~HeadTracker();
    HeadTracker(const HeadTracker&) = delete;
    HeadTracker& operator=(const HeadTracker&) = delete;

    /// The screen the pose is measured against; the same area given to Tracker::setDisplayArea().
    void setDisplayArea(const DisplayArea& area);

    /// Feed every gaze sample: head position comes from its eye positions.
    void onGaze(const GazeSample& sample);

    /// Pose for a camera frame, or nullopt when no face is found in it.
    std::optional<HeadPose> onImage(const ImageFrame& frame);

    /// Forget the face being followed, e.g. after the tracker reconnects.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eyenix
