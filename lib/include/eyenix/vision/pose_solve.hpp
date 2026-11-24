// Adapted for Eyenix from tobii-faithful v1.1.8 (https://github.com/SciFi-Bob/tobii-faithful),
// Copyright its authors, licensed under the Apache License 2.0 (licenses/Apache-2.0.txt).
// Changes: namespaces and include paths renamed; see git history for anything further.
#pragma once

#include <string>

namespace eyenix {

enum class PoseSolve {
    kMediaPipe,
    kRigid,
};

inline const char* PoseSolveName(PoseSolve s) {
    return s == PoseSolve::kRigid ? "rigid" : "mediapipe";
}

inline bool ParsePoseSolve(const std::string& name, PoseSolve& out) {
    if (name == "rigid") { out = PoseSolve::kRigid; return true; }
    if (name == "mediapipe") { out = PoseSolve::kMediaPipe; return true; }
    return false;
}

}
