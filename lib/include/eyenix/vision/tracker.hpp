// Adapted for Eyenix from tobii-faithful v1.1.8 (https://github.com/SciFi-Bob/tobii-faithful),
// Copyright its authors, licensed under the Apache License 2.0 (licenses/Apache-2.0.txt).
// Changes: namespaces and include paths renamed; see git history for anything further.
#pragma once

#include "eyenix/vision/face_detector.hpp"
#include "eyenix/vision/face_landmarks.hpp"
#include "eyenix/vision/pose.hpp"

#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <vector>

namespace eyenix::vision {

struct TrackerConfig {
    DetectorConfig detector;
    LandmarkConfig landmarks;
    bool apply_clahe = true;
    PoseOptions pose;
};

struct TrackedFace {
    HeadPose pose;
    std::vector<cv::Point3f> landmarks;
    float presence = 0.0f;
    bool reacquired = false;
};

class FaceTracker {
public:
    FaceTracker(const std::string& detector_model, const std::string& landmark_model,
                TrackerConfig cfg = {});

    std::optional<TrackedFace> Track(const cv::Mat& gray);

    void Reset();

    void set_pose_options(const PoseOptions& options) { cfg_.pose = options; }
    const PoseOptions& pose_options() const { return cfg_.pose; }

    const std::vector<cv::Point3f>& last_landmarks() const { return previous_; }

private:
    FaceDetector   detector_;
    FaceLandmarker landmarker_;
    TrackerConfig  cfg_;
    std::vector<cv::Point3f> previous_;
};

}
