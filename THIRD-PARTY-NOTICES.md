# Third-party notices

Eyenix is licensed under the MIT licence (`LICENSE`). It includes, or is built
from, the material below, each under its own terms. The Apache License 2.0
text is `licenses/Apache-2.0.txt`.

## tobii-faithful

Copyright the tobii-faithful authors (SciFi-Bob). Apache License 2.0.
Source: <https://github.com/SciFi-Bob/tobii-faithful>, release v1.1.8.

Game integration in Eyenix is ported from it. Each file says so at the top,
with what was changed.

- **Copied and adapted**: `lib/include/eyenix/vision/*`, `lib/src/vision/*`
  (the face pipeline; namespaces and include paths renamed, one OpenCV call
  replaced), and `src/gamereplies.h` (answers a game is given about the
  tracker; the serial number in one of them replaced with a placeholder).
- **Rewritten from it**: `lib/src/headpose.cpp` (head pose maths),
  `src/gameserver.cpp` (the service games connect to), `src/wineprefix.cpp`
  (finding running Wine prefixes), and `bridge/*.go` (the helper that runs
  inside a Wine prefix; a Go rewrite of its C program).

## MediaPipe

Copyright The MediaPipe Authors. Apache License 2.0.
Source: <https://github.com/google-ai-edge/mediapipe>.

- **Face models**: `models/face_detector.onnx` and
  `models/face_landmarks_detector.onnx`, installed under
  `/usr/share/eyenix/models/`. Converted from MediaPipe's
  `face_landmarker.task` bundle by `tools/convert_models.py`: TFLite to ONNX,
  weights stored as float32 instead of float16, otherwise unchanged. The
  script records the bundle's address and hash and checks the converted
  models against the originals.
- **Face geometry data**: `lib/include/eyenix/vision/face_geometry_data.hpp`,
  the canonical face mesh and its weights from the same bundle, by way of
  tobii-faithful.
- **Anchor generation**: `lib/src/vision/anchors.cpp` and its header, based on
  MediaPipe's SSD anchors calculator, by way of tobii-faithful.

## Linked at run time, not included

ONNX Runtime (MIT), OpenCV (Apache-2.0) and Eigen (MPL-2.0, headers only,
compiled in) come from the distribution's packages.

## Omarchy app templates

Theme and language following (`src/theme.*`, `src/translator.*`) come from the
Omarchy app templates, MIT, David Heinemeier Hansson.
