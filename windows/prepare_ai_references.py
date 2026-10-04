import argparse
import hashlib
import json
import struct
import time
import zlib
from pathlib import Path

import numpy as np
import onnxruntime as ort
import torch
from PIL import Image
from torchvision.transforms import Compose, Normalize, Resize, ToTensor

from export_models import load_rgb, sha256, square_input


def save_tensor(directory, name, array):
    array = np.ascontiguousarray(array)
    if array.dtype not in (np.dtype("float32"), np.dtype("int32"), np.dtype("int64")):
        raise ValueError("Unsupported reference tensor type")
    raw = array.astype(array.dtype.newbyteorder("<"), copy=False).tobytes()
    filename = name + ".qz"
    (directory / filename).write_bytes(struct.pack(">I", len(raw)) + zlib.compress(raw, 3))
    return {"file": filename, "shape": list(array.shape), "type": str(array.dtype), "sha256": hashlib.sha256(raw).hexdigest()}


def session(path, threads):
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    return ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])


def outputs(session, inputs, directory, prefix):
    started = time.perf_counter()
    tensors = session.run(None, inputs)
    elapsed = (time.perf_counter() - started) * 1000
    result = {}
    for information, array in zip(session.get_outputs(), tensors, strict=True):
        result[information.name] = save_tensor(directory, prefix + "-" + information.name, array)
    return result, tensors, elapsed


def mobile_image(image):
    height, width = image.shape[:2]
    scale = 1024 / max(height, width)
    new_height, new_width = int(height * scale + 0.5), int(width * scale + 0.5)
    resized = np.asarray(Image.fromarray(image).resize((new_width, new_height), Image.Resampling.BILINEAR), np.float32)
    resized = (resized - np.array([123.675, 116.28, 103.53], np.float32)) / np.array([58.395, 57.12, 57.375], np.float32)
    result = np.zeros((1, 3, 1024, 1024), np.float32)
    result[0, :, :new_height, :new_width] = resized.transpose(2, 0, 1)
    return result, (new_height, new_width)


def main():
    parser = argparse.ArgumentParser(description="Generate local Python ONNX goldens for C++/DirectML verification")
    parser.add_argument("--models-root", type=Path, required=True)
    parser.add_argument("--images-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lite", type=Path)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Use a new reference directory")
    args.output.mkdir(parents=True)
    torch.set_num_threads(args.threads)
    sam2_transform = Compose([ToTensor(), Resize((1024, 1024)), Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225])])
    models = {
        "birefnet": {"kind": "birefnet", "encoder": args.models_root / "birefnet-fp32/birefnet.onnx"},
        "sam2": {"kind": "sam2", "encoder": args.models_root / "sam2-hiera-tiny-fp32/sam2_encoder.onnx", "decoder": args.models_root / "sam2-hiera-tiny-fp32/sam2_decoder.onnx"},
        "mobilesam": {"kind": "mobilesam", "encoder": args.models_root / "mobilesam-fp32/mobilesam_encoder.onnx", "decoder": args.models_root / "mobilesam-fp32/mobilesam_decoder.onnx"},
    }
    if args.lite:
        models["birefnet-lite"] = {"kind": "birefnet", "encoder": args.lite}
    report = {"version": 1, "onnxruntime": ort.__version__, "torch": torch.__version__, "provider": "CPUExecutionProvider", "threads": args.threads, "models": []}
    for identifier, model in models.items():
        if not model["encoder"].is_file() or ("decoder" in model and not model["decoder"].is_file()):
            raise FileNotFoundError(f"Missing graphs for {identifier}")
        print("Preparing " + identifier, flush=True)
        encoder = session(model["encoder"], args.threads)
        decoder = session(model["decoder"], args.threads) if "decoder" in model else None
        record = {"id": identifier, "kind": model["kind"], "encoder": str(model["encoder"].resolve()), "encoder_sha256": sha256(model["encoder"]), "images": []}
        if decoder:
            record.update({"decoder": str(model["decoder"].resolve()), "decoder_sha256": sha256(model["decoder"])})
        for name in ("truck", "cars", "groceries"):
            image = load_rgb(args.images_dir / (name + ".jpg"))
            height, width = image.shape[:2]
            image_file = name + ".png"
            if not (args.output / image_file).exists():
                Image.fromarray(image).save(args.output / image_file)
            mobile = model["kind"] == "mobilesam"
            if mobile:
                tensor, resized = mobile_image(image)
            elif model["kind"] == "sam2":
                tensor = sam2_transform(image).unsqueeze(0).numpy()
                resized = (1024, 1024)
            else:
                tensor = square_input(image).numpy()
                resized = (1024, 1024)
            prefix = identifier + "-" + name
            image_record = {"file": image_file, "original_size": [width, height], "resized_size": [resized[1], resized[0]], "input": save_tensor(args.output, prefix + "-image", tensor)}
            reference, features, elapsed = outputs(encoder, {"image": tensor}, args.output, prefix + "-encoder")
            image_record.update({"encoder_outputs": reference, "encoder_ms": elapsed, "cases": []})
            if decoder:
                feature_inputs = dict(zip((item.name for item in encoder.get_outputs()), features, strict=True))
                previous = None
                cases = [("positive", [[0.5, 0.5]], [1]), ("positive_negative", [[0.5, 0.5], [0.1, 0.1]], [1, 0]), ("three_points", [[0.4, 0.4], [0.6, 0.6], [0.1, 0.1]], [1, 1, 0]), ("refine", [[0.5, 0.5]], [1])]
                for label, fractions, labels in cases:
                    original = np.asarray(fractions, np.float32) * np.array([width, height], np.float32)
                    coords = original.astype(np.float64) * np.array([resized[1] / width, resized[0] / height]) if mobile else original * np.array([1024 / width, 1024 / height], np.float32)
                    mask = previous if label == "refine" else np.zeros((1, 1, 256, 256), np.float32)
                    prompt_labels = np.array([labels + [-1] if mobile else labels], np.float32 if mobile else np.int32)
                    if mobile:
                        coords = np.concatenate((coords, np.zeros((1, 2))))
                    inputs = {**feature_inputs, "point_coords": coords[None].astype(np.float32), "point_labels": prompt_labels,
                              "mask_input" if mobile else "input_masks": mask,
                              "has_mask_input" if mobile else "has_input_masks": np.array([float(label == "refine")], np.float32),
                              "orig_im_size" if mobile else "original_image_size": np.array([height, width], np.float32 if mobile else np.int64)}
                    output, arrays, elapsed = outputs(decoder, inputs, args.output, prefix + "-" + label)
                    selected = 1 if mobile else 0
                    previous = arrays[2][:, selected:selected + 1].copy()
                    image_record["cases"].append({"name": label, "points": original.tolist(), "labels": labels, "refine": label == "refine", "feedback_candidate": selected, "outputs": output, "decoder_ms": elapsed})
                    print(f"{identifier}/{name}/{label}: {elapsed:.2f} ms", flush=True)
            record["images"].append(image_record)
            print(f"{identifier}/{name}/encoder: {image_record['encoder_ms']:.2f} ms", flush=True)
        report["models"].append(record)
        del encoder, decoder
    path = args.output / "references.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Reference manifest: " + str(path), flush=True)


if __name__ == "__main__":
    main()
