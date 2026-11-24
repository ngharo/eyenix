// Adapted for Eyenix from tobii-faithful v1.1.8 (https://github.com/SciFi-Bob/tobii-faithful),
// Copyright its authors, licensed under the Apache License 2.0 (licenses/Apache-2.0.txt).
// Changes: namespaces and include paths renamed; see git history for anything further.
#pragma once

#include <opencv2/core.hpp>

namespace eyenix::vision {

struct PreprocessConfig {
    double clahe_clip_limit = 2.0;
    int    clahe_tile_grid  = 8;
};

cv::Mat ApplyClahe(const cv::Mat& gray, const PreprocessConfig& cfg = {});

cv::Mat GrayToRgb(const cv::Mat& gray);

}
