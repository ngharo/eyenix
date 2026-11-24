// The pose maths here is adapted from tobii-faithful v1.1.8
// (https://github.com/SciFi-Bob/tobii-faithful: src/pose/head_pose.cpp, src/pipeline/pipeline.cpp,
// include/tobii/pose/{screen_frame,euler}.hpp), Copyright its authors, Apache License 2.0
// (licenses/Apache-2.0.txt). Rewritten around Eyenix's own types.
#include "eyenix/headpose.hpp"

#include <cmath>

#include <Eigen/Dense>
#include <opencv2/core.hpp>

#include "eyenix/vision/tracker.hpp"

namespace eyenix {
namespace {

constexpr double kMinDistanceMm = 250.0, kMaxDistanceMm = 1200.0; // outside this the position is not trusted
constexpr double kSeparationSmoothing = 0.15;

Eigen::Vector3d vec(const Point3& p) { return {p.x, p.y, p.z}; }

// Eye positions arrive with x mirrored relative to the display area's axes.
Eigen::Vector3d eye(const Point3& p) { return {-p.x, p.y, p.z}; }

// The face model's axes to the tracker's: x and z flipped.
const Eigen::Matrix3d kMeshToDevice = Eigen::Vector3d(-1, 1, -1).asDiagonal();

} // namespace

struct HeadTracker::Impl {
    vision::FaceTracker faces;
    Eigen::Matrix3d screenFromDevice = Eigen::Matrix3d::Identity(); // rows: screen x, y, z axes
    Eigen::Vector3d screenCentre = Eigen::Vector3d::Zero();
    GazeSample gaze;
    Eigen::Vector3d halfSeparation = Eigen::Vector3d::Zero(); // half the left-to-right eye vector
    bool haveSeparation = false;

    Impl(const std::string& dir)
        : faces(dir + "/face_detector.onnx", dir + "/face_landmarks_detector.onnx") {}
};

HeadTracker::HeadTracker(const std::string& modelDir) try : impl_(std::make_unique<Impl>(modelDir)) {
} catch (const std::runtime_error&) {
    throw;
} catch (const std::exception& e) { // ONNX Runtime's own exception type
    throw std::runtime_error(std::string("cannot load the face models from ") + modelDir + ": " + e.what());
}

HeadTracker::~HeadTracker() = default;

void HeadTracker::setDisplayArea(const DisplayArea& a) {
    const Eigen::Vector3d tl = vec(a.topLeft), tr = vec(a.topRight), bl = vec(a.bottomLeft);
    const Eigen::Vector3d ex = (tr - tl).normalized();
    const Eigen::Vector3d up = tl - bl;
    const Eigen::Vector3d ey = (up - up.dot(ex) * ex).normalized();
    impl_->screenFromDevice.row(0) = ex;
    impl_->screenFromDevice.row(1) = ey;
    impl_->screenFromDevice.row(2) = ex.cross(ey);
    impl_->screenCentre = 0.5 * (tr + bl);
}

void HeadTracker::onGaze(const GazeSample& sample) { impl_->gaze = sample; }

void HeadTracker::reset() {
    impl_->faces.Reset();
    impl_->gaze = {};
    impl_->haveSeparation = false;
}

std::optional<HeadPose> HeadTracker::onImage(const ImageFrame& frame) {
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0) return std::nullopt;
    if (frame.pixels.size() < static_cast<std::size_t>(frame.stride) * frame.height) return std::nullopt;

    const cv::Mat gray(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC1,
                       const_cast<std::uint8_t*>(frame.pixels.data()), frame.stride);
    const auto face = impl_->faces.Track(gray);
    if (!face) return std::nullopt;

    HeadPose out;
    out.timestampUs = impl_->gaze.timestampUs ? impl_->gaze.timestampUs : frame.timestampUs;
    out.presence = face->presence;

    // Position: midpoint between the eyes. With one eye lost, use the last known separation.
    const bool left = impl_->gaze.left.originValid, right = impl_->gaze.right.originValid;
    const Eigen::Vector3d l = eye(impl_->gaze.left.originMm), r = eye(impl_->gaze.right.originMm);
    std::optional<Eigen::Vector3d> mid;
    if (left && right) {
        const Eigen::Vector3d sep = 0.5 * (l - r);
        impl_->halfSeparation = impl_->haveSeparation
            ? (1.0 - kSeparationSmoothing) * impl_->halfSeparation + kSeparationSmoothing * sep
            : sep;
        impl_->haveSeparation = true;
        mid = 0.5 * (l + r);
    } else if (left && impl_->haveSeparation) {
        mid = l - impl_->halfSeparation;
    } else if (right && impl_->haveSeparation) {
        mid = r + impl_->halfSeparation;
    }
    if (mid) {
        const Eigen::Vector3d p = impl_->screenFromDevice * (*mid - impl_->screenCentre);
        if (p.z() >= kMinDistanceMm && p.z() <= kMaxDistanceMm) {
            out.positionValid = true;
            out.positionMm = {p.x(), p.y(), p.z()};
        }
    }

    // Rotation: the face model's rotation, re-expressed in screen axes, as yaw-pitch-roll.
    Eigen::Matrix3d mesh;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) mesh(row, col) = face->pose.rotation(row, col);
    const Eigen::Matrix3d m = impl_->screenFromDevice * kMeshToDevice * mesh * kMeshToDevice;
    out.yaw = std::atan2(m(0, 2), m(2, 2));
    out.pitch = std::atan2(-m(1, 2), std::hypot(m(0, 2), m(2, 2)));
    out.roll = std::atan2(m(1, 0), m(1, 1));
    return out;
}

} // namespace eyenix
