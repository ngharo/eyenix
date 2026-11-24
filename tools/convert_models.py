#!/usr/bin/env python3
"""Build the face models Eyenix uses for head tracking from MediaPipe's own bundle.

Downloads Google's face_landmarker.task (Apache-2.0), checks its hash, converts the
face detector and the face landmark model from TFLite to ONNX, and checks that each
converted model gives the same answers as the original.

    uv run --python 3.11 --with 'tensorflow-cpu==2.15.*' --with 'tf2onnx==1.16.1' \
        --with 'numpy<2' --with 'onnx<1.17' --with 'protobuf<5' --with onnxruntime \
        tools/convert_models.py [--out models]

The weights are unchanged apart from float16 -> float32 storage.
"""

import argparse
import hashlib
import pathlib
import subprocess
import sys
import urllib.request
import zipfile

BUNDLE_URL = (
    "https://storage.googleapis.com/mediapipe-models/face_landmarker/"
    "face_landmarker/float16/1/face_landmarker.task"
)
BUNDLE_SHA256 = "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff"
MODELS = ["face_detector", "face_landmarks_detector"]
TOLERANCE = 1e-3  # largest allowed difference between TFLite and ONNX outputs


def fetch_bundle(cache: pathlib.Path) -> pathlib.Path:
    path = cache / "face_landmarker.task"
    if not path.exists():
        print(f"downloading {BUNDLE_URL}")
        with urllib.request.urlopen(BUNDLE_URL, timeout=60) as response:
            path.write_bytes(response.read())
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != BUNDLE_SHA256:
        sys.exit(f"{path}: sha256 {digest} is not the expected {BUNDLE_SHA256}")
    return path


def compare(tflite_path: pathlib.Path, onnx_path: pathlib.Path) -> float:
    """Largest output difference between the two models on the same random input."""
    import numpy as np
    import onnx
    import onnxruntime
    import tensorflow as tf

    model = onnx.load(str(onnx_path))
    kinds = {init.data_type for init in model.graph.initializer}
    if onnx.TensorProto.FLOAT16 in kinds:
        sys.exit(f"{onnx_path}: still has float16 weights")

    interpreter = tf.lite.Interpreter(model_path=str(tflite_path))
    interpreter.allocate_tensors()
    (inp,) = interpreter.get_input_details()
    x = np.random.default_rng(1).uniform(-1, 1, size=inp["shape"]).astype(np.float32)
    interpreter.set_tensor(inp["index"], x)
    interpreter.invoke()
    expected = {d["name"]: interpreter.get_tensor(d["index"]) for d in interpreter.get_output_details()}

    session = onnxruntime.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    names = [o.name for o in session.get_outputs()]
    got = dict(zip(names, session.run(names, {session.get_inputs()[0].name: x})))

    worst = 0.0
    for name, want in expected.items():
        if name not in got:
            sys.exit(f"{onnx_path}: output {name!r} is missing (has {names})")
        worst = max(worst, float(np.abs(got[name].reshape(want.shape) - want).max()))
    print(f"  input {list(x.shape)}, outputs {[(n, list(got[n].shape)) for n in names]}")
    return worst


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default="models", type=pathlib.Path)
    parser.add_argument("--cache", default=pathlib.Path.home() / ".cache/eyenix-models", type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)

    bundle = fetch_bundle(args.cache)
    with zipfile.ZipFile(bundle) as archive:
        for name in MODELS:
            (args.cache / f"{name}.tflite").write_bytes(archive.read(f"{name}.tflite"))

    for name in MODELS:
        tflite, onnx_path = args.cache / f"{name}.tflite", args.out / f"{name}.onnx"
        print(f"converting {name}")
        result = subprocess.run(
            [sys.executable, "-m", "tf2onnx.convert", "--tflite", str(tflite), "--output", str(onnx_path), "--opset", "13"],
            capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(result.stderr)
        worst = compare(tflite, onnx_path)
        print(f"  largest difference from the TFLite model: {worst:.2e}")
        if worst > TOLERANCE:
            sys.exit(f"{onnx_path}: differs from the original by more than {TOLERANCE}")
        print(f"  wrote {onnx_path} ({onnx_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
