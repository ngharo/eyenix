// Adapted for Eyenix from tobii-faithful v1.1.8 (https://github.com/SciFi-Bob/tobii-faithful),
// Copyright its authors, licensed under the Apache License 2.0 (licenses/Apache-2.0.txt).
// Changes: namespaces and include paths renamed; see git history for anything further.
#include "eyenix/vision/preprocess.hpp"

#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace eyenix::vision {

cv::Mat ApplyClahe(const cv::Mat& gray, const PreprocessConfig& cfg) {
    if (gray.empty()) {
        throw std::invalid_argument("ApplyClahe: empty input");
    }
    if (gray.type() != CV_8UC1) {
        throw std::invalid_argument("ApplyClahe: expected CV_8UC1");
    }
    auto clahe = cv::createCLAHE(cfg.clahe_clip_limit,
                                 cv::Size(cfg.clahe_tile_grid, cfg.clahe_tile_grid));
    cv::Mat out;
    clahe->apply(gray, out);
    return out;
}

cv::Mat GrayToRgb(const cv::Mat& gray) {
    if (gray.empty()) {
        throw std::invalid_argument("GrayToRgb: empty input");
    }
    cv::Mat rgb;
    cv::cvtColor(gray, rgb, cv::COLOR_GRAY2RGB);
    return rgb;
}

}
