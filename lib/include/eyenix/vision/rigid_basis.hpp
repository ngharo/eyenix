// Adapted for Eyenix from tobii-faithful v1.1.8 (https://github.com/SciFi-Bob/tobii-faithful),
// Copyright its authors, licensed under the Apache License 2.0 (licenses/Apache-2.0.txt).
// Changes: namespaces and include paths renamed; see git history for anything further.
#pragma once

#include "eyenix/vision/face_geometry_data.hpp"

#include <array>
#include <cstddef>

namespace eyenix::vision {

inline constexpr std::array<unsigned, 8> kDeformableBasisLandmarks = {
    132, 136, 147, 205, 361, 365, 376, 425,
};

inline constexpr std::size_t kRigidProcrustesLandmarkCount =
    kProcrustesLandmarkCount - kDeformableBasisLandmarks.size();

constexpr bool IsDeformableBasisLandmark(unsigned index) {
    for (const unsigned d : kDeformableBasisLandmarks) {
        if (d == index) return true;
    }
    return false;
}

constexpr std::array<WeightedLandmark, kRigidProcrustesLandmarkCount> MakeRigidBasis() {
    std::array<WeightedLandmark, kRigidProcrustesLandmarkCount> out{};
    float total = 0.0f;
    for (const auto& b : kProcrustesBasis) {
        if (!IsDeformableBasisLandmark(b.index)) total += b.weight;
    }
    std::size_t n = 0;
    for (const auto& b : kProcrustesBasis) {
        if (IsDeformableBasisLandmark(b.index)) continue;
        out[n++] = WeightedLandmark{b.index, b.weight / total};
    }
    return out;
}

inline constexpr std::array<WeightedLandmark, kRigidProcrustesLandmarkCount>
    kRigidProcrustesBasis = MakeRigidBasis();

}
