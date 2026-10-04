import argparse
import hashlib
import importlib.metadata
import json
import os
import shutil
import subprocess
import sys
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "BiRefNet": ("https://github.com/ZhengPeng7/BiRefNet.git", "ebcc0bc8ec7fe919cec829f2dea656b3078acddc", []),
    "MobileSAM": ("https://github.com/ChaoningZhang/MobileSAM.git", "f706ad9c4eb7f219c00d9050e46328518ffb65d2", ["mobile_sam", "weights"]),
    "sam2": ("https://github.com/facebookresearch/sam2.git", "2b90b9f5ceec907a1c18123530e92e794ad901a4", ["sam2", "notebooks/images"]),
    "onnxruntime": ("https://github.com/microsoft/onnxruntime.git", "5630b081cd25e4eccc7516a652ff956e51676794", ["onnxruntime/python/tools/transformers/models/sam2"]),
}
HF_MODELS = {
    "birefnet": ("ZhengPeng7/BiRefNet", "e2bf8e4460fc8fa32bba5ea4d94b3233d367b0e4", "model.safetensors", "MIT"),
    "birefnet-lite": ("ZhengPeng7/BiRefNet_lite", "aa62cd87eafb9cc43056d08ef3615a14628b831d", "model.safetensors", "MIT"),
    "sam2": ("facebook/sam2-hiera-tiny", "7c218beaf0bb87874785f32b582f640134fc1c09", "sam2_hiera_tiny.pt", "Apache-2.0"),
}
TENSOR_LIMITS = {"atol": 0.005, "rtol": 0.0001, "max_mismatch_fraction": 0.01}
MASK_LIMITS = {"max_mean_abs_error": 0.001, "min_iou": 0.995}


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compare_tensor(expected, actual, atol=0.005, rtol=0.0001, max_mismatch_fraction=0.01):
    expected, actual = np.asarray(expected), np.asarray(actual)
    if expected.shape != actual.shape or expected.size == 0:
        return {"passed": False, "reason": "Shape mismatch or empty tensor"}
    if not np.isfinite(expected).all() or not np.isfinite(actual).all():
        return {"passed": False, "reason": "Nonfinite tensor"}
    delta = np.abs(expected.astype(np.float64) - actual.astype(np.float64))
    tolerance = atol + rtol * np.maximum(np.abs(expected), np.abs(actual))
    mismatch = float(np.mean(delta > tolerance))
    return {"passed": mismatch <= max_mismatch_fraction, "shape": list(expected.shape), "max_abs_error": float(delta.max()), "mean_abs_error": float(delta.mean()), "mismatch_fraction": mismatch}


def compare_mask(expected, actual):
    result = compare_tensor(expected, actual)
    if "reason" in result:
        return result
    expected, actual = np.asarray(expected), np.asarray(actual)
    if np.any((expected < 0) | (expected > 1)) or np.any((actual < 0) | (actual > 1)):
        return {"passed": False, "reason": "Mask probabilities outside [0, 1]"}
    left, right = expected > 0.5, actual > 0.5
    pixels = int(np.prod(expected.shape[-2:]))
    union = np.count_nonzero((left | right).reshape(-1, pixels), axis=1)
    intersection = np.count_nonzero((left & right).reshape(-1, pixels), axis=1)
    ious = np.divide(intersection, union, out=np.ones(union.shape, np.float64), where=union != 0)
    mean_errors = np.abs(expected.astype(np.float64) - actual).reshape(-1, pixels).mean(axis=1)
    result["iou"] = float(ious.min())
    result["max_mask_mean_abs_error"] = float(mean_errors.max())
    result["passed"] = result["max_mask_mean_abs_error"] <= MASK_LIMITS["max_mean_abs_error"] and result["iou"] >= MASK_LIMITS["min_iou"]
    return result


def deform_conv_export(input, offset, weight, bias=None, stride=(1, 1), padding=(0, 0), dilation=(1, 1), mask=None):
    import torch
    from torch.nn import functional as F
    from torch.nn.modules.utils import _pair

    stride, padding, dilation = _pair(stride), _pair(padding), _pair(dilation)
    n, channels, height, width = input.shape
    out_channels, channels_per_group, kh, kw = weight.shape
    oh, ow = offset.shape[-2:]
    kernels = kh * kw
    offset_groups = offset.shape[1] // (2 * kernels)
    groups = channels // channels_per_group
    yy, xx = torch.meshgrid(torch.arange(oh, device=input.device, dtype=input.dtype), torch.arange(ow, device=input.device, dtype=input.dtype), indexing="ij")
    ky, kx = torch.meshgrid(torch.arange(kh, device=input.device, dtype=input.dtype), torch.arange(kw, device=input.device, dtype=input.dtype), indexing="ij")
    offsets = offset.reshape(n, offset_groups, kernels, 2, oh, ow).permute(0, 1, 4, 5, 2, 3)
    y = yy[..., None] * stride[0] - padding[0] + ky.flatten() * dilation[0] + offsets[..., 0]
    x = xx[..., None] * stride[1] - padding[1] + kx.flatten() * dilation[1] + offsets[..., 1]
    grid = torch.stack(((2 * x + 1) / width - 1, (2 * y + 1) / height - 1), dim=-1).reshape(n * offset_groups, oh, ow * kernels, 2)
    sampled = F.grid_sample(input.reshape(n * offset_groups, channels // offset_groups, height, width), grid, mode="bilinear", padding_mode="zeros", align_corners=False)
    sampled = sampled.reshape(n, offset_groups, channels // offset_groups, oh, ow, kernels)
    if mask is not None:
        coverage = mask.reshape(n, offset_groups, kernels, oh, ow).permute(0, 1, 3, 4, 2).unsqueeze(2)
        sampled = sampled * coverage
    columns = sampled.reshape(n, channels, oh, ow, kernels).permute(0, 1, 4, 2, 3).reshape(n, groups, channels_per_group * kernels, oh * ow)
    result = torch.matmul(weight.reshape(groups, out_channels // groups, channels_per_group * kernels), columns).reshape(n, out_channels, oh, ow)
    return result if bias is None else result + bias.reshape(1, -1, 1, 1)


@contextmanager
def lower_birefnet_deform(model):
    module = sys.modules[type(model).__module__]
    original = module.deform_conv2d
    module.deform_conv2d = deform_conv_export
    try:
        yield
    finally:
        module.deform_conv2d = original


def git(*args):
    return subprocess.check_output(["git", *map(str, args)], text=True).strip()


def source(name, cache):
    url, revision, directories = SOURCES[name]
    path = cache / name
    if not path.exists():
        git("clone", "--filter=blob:none", "--no-checkout", "--depth", "1", url, path)
        git("-C", path, "fetch", "--depth", "1", "origin", revision)
        git("-C", path, "sparse-checkout", "set", *directories)
        git("-C", path, "checkout", "--detach", revision)
    if git("-C", path, "rev-parse", "HEAD") != revision or git("-C", path, "status", "--porcelain", "--untracked-files=no"):
        raise RuntimeError(f"Source must be clean and pinned to {revision}: {path}")
    return path


def retain_license(path, output, filename, report):
    destination = output / filename
    if destination.exists():
        if sha256(destination) != sha256(path):
            raise RuntimeError(f"Refusing to replace different license content: {destination}")
    else:
        shutil.copyfile(path, destination)
    report.setdefault("licenses", []).append({"file": filename, "sha256": sha256(destination)})


def load_rgb(path):
    from PIL import Image, ImageOps

    with Image.open(path) as image:
        return np.array(ImageOps.exif_transpose(image).convert("RGB"))


def square_input(image):
    import torch
    from PIL import Image

    pixels = np.array(Image.fromarray(image).resize((1024, 1024), Image.Resampling.BILINEAR), dtype=np.float32) / 255
    pixels = (pixels - np.array([0.485, 0.456, 0.406], np.float32)) / np.array([0.229, 0.224, 0.225], np.float32)
    return torch.from_numpy(pixels.transpose(2, 0, 1).copy()).unsqueeze(0)


def numpy_outputs(model, inputs):
    import torch

    with torch.inference_mode():
        outputs = model(*inputs)
    if isinstance(outputs, torch.Tensor):
        outputs = (outputs,)
    return [output.detach().cpu().numpy() for output in outputs]


def export_graph(model, inputs, path, names, outputs, dynamic_axes=None, verify_only=False, normalize_sam2=False):
    import onnx
    import onnxruntime as ort
    import torch

    if not verify_only:
        if path.exists():
            raise FileExistsError(f"Refusing to overwrite {path}; use a new output directory or --verify-only")
        print(f"Exporting {path.name}", flush=True)
        with torch.inference_mode():
            torch.onnx.export(model, inputs, str(path), opset_version=17, input_names=names, output_names=outputs, dynamic_axes=dynamic_axes, dynamo=False)
        if normalize_sam2:
            from normalize_ai_encoder import normalize_sam2_shapes
            graph = onnx.load(str(path))
            normalize_sam2_shapes(graph)
            onnx.save(graph, str(path))
    onnx.checker.check_model(str(path), full_check=True)
    graph = onnx.load(str(path), load_external_data=False)
    if any(node.domain not in ("", "ai.onnx") for node in graph.graph.node):
        raise RuntimeError("Export requires nonstandard operators")
    options = ort.SessionOptions()
    options.intra_op_num_threads = torch.get_num_threads()
    session = ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])
    session.disable_fallback()
    return session


def run(session, inputs):
    feed = dict(zip((item.name for item in session.get_inputs()), (value.detach().cpu().numpy() for value in inputs), strict=True))
    return session.run(None, feed)


def add_check(report, label, result):
    report["checks"].append({"name": label, **result})
    print(f"{label}: {json.dumps(result)}", flush=True)


def sigmoid(logits):
    return 1 / (1 + np.exp(-np.clip(logits, -80, 80)))


def graph_info(path, session):
    io = lambda values: [{"name": value.name, "shape": value.shape, "type": value.type} for value in values]
    return {"file": path.name, "bytes": path.stat().st_size, "sha256": sha256(path), "inputs": io(session.get_inputs()), "outputs": io(session.get_outputs())}


def export_birefnet(args, images, report):
    import torch
    from huggingface_hub import hf_hub_download
    from transformers import AutoModelForImageSegmentation

    repo, revision, filename, license_name = HF_MODELS[args.model]
    checkpoint = hf_hub_download(repo, filename, revision=revision, cache_dir=str(args.cache / "huggingface"))
    report["source"] = {"repo": repo, "revision": revision, "license": license_name, "checkpoint_sha256": sha256(checkpoint), "license_code_revision": SOURCES["BiRefNet"][1]}
    retain_license(source("BiRefNet", args.cache) / "LICENSE", args.output, "LICENSE-BiRefNet.txt", report)
    retain_license(hf_hub_download(repo, "README.md", revision=revision, cache_dir=str(args.cache / "huggingface")), args.output, "MODEL-CARD.md", report)
    model = AutoModelForImageSegmentation.from_pretrained(repo, revision=revision, code_revision=revision, trust_remote_code=True, use_safetensors=True, cache_dir=str(args.cache / "huggingface"), bb_pretrained=False).float().cpu().eval()

    class LastMask(torch.nn.Module):
        def __init__(self, model):
            super().__init__()
            self.model = model

        def forward(self, image):
            return self.model(image)[-1]

    wrapper = LastMask(model).eval()
    sample = (square_input(load_rgb(images[0])),)
    path = args.output / (args.model + ".onnx")
    with lower_birefnet_deform(model):
        session = export_graph(wrapper, sample, path, ["image"], ["logits"], verify_only=args.verify_only)
    for image_path in images:
        inputs = (square_input(load_rgb(image_path)),)
        expected = numpy_outputs(wrapper, inputs)[0]
        actual = run(session, inputs)[0]
        add_check(report, image_path.name + "/logits", compare_tensor(expected, actual))
        add_check(report, image_path.name + "/matte", compare_mask(sigmoid(expected), sigmoid(actual)))
    report["graphs"] = [graph_info(path, session)]
    report["preprocess"] = {"resize": "bilinear square 1024x1024", "layout": "NCHW", "color": "RGB sRGB", "range": "0..1", "mean": [0.485, 0.456, 0.406], "std": [0.229, 0.224, 0.225], "postprocess": "sigmoid logits, then bilinear resize to original dimensions", "deform_conv": "export-only GridSample/MatMul lowering; reference retains torchvision implementation"}


def export_sam(args, images, report):
    import torch
    from huggingface_hub import hf_hub_download

    sam2 = args.model == "sam2"
    if sam2:
        upstream = source("sam2", args.cache)
        exporter_source = source("onnxruntime", args.cache)
        exporter = exporter_source / "onnxruntime/python/tools/transformers/models/sam2"
        retain_license(upstream / "LICENSE", args.output, "LICENSE-SAM2.txt", report)
        retain_license(exporter_source / "LICENSE", args.output, "LICENSE-ONNX-Runtime-exporter.txt", report)
        sys.path[:0] = [str(upstream), str(exporter)]
        from sam2.build_sam import build_sam2
        from sam2.sam2_image_predictor import SAM2ImagePredictor
        from image_encoder import SAM2ImageEncoder
        from image_decoder import SAM2ImageDecoder

        repo, revision, filename, license_name = HF_MODELS["sam2"]
        checkpoint = hf_hub_download(repo, filename, revision=revision, cache_dir=str(args.cache / "huggingface"))
        retain_license(hf_hub_download(repo, "README.md", revision=revision, cache_dir=str(args.cache / "huggingface")), args.output, "MODEL-CARD.md", report)
        model = build_sam2("configs/sam2/sam2_hiera_t.yaml", checkpoint, device="cpu")
        encoder = SAM2ImageEncoder(model).eval()
        decoder = SAM2ImageDecoder(model, multimask_output=True, dynamic_multimask_via_stability=False, return_logits=True).eval()
        predictor = SAM2ImagePredictor(model)
        encoder_names = ["image_features_0", "image_features_1", "image_embeddings"]
        decoder_names = encoder_names + ["point_coords", "point_labels", "input_masks", "has_input_masks", "original_image_size"]
        report["source"] = {"repo": repo, "revision": revision, "license": license_name, "checkpoint_sha256": sha256(checkpoint), "code_revision": SOURCES["sam2"][1], "exporter_revision": SOURCES["onnxruntime"][1]}
    else:
        upstream = source("MobileSAM", args.cache)
        retain_license(upstream / "LICENSE", args.output, "LICENSE-MobileSAM.txt", report)
        retain_license(upstream / "README.md", args.output, "MODEL-CARD.md", report)
        sys.path.insert(0, str(upstream))
        from mobile_sam import sam_model_registry, SamPredictor
        from mobile_sam.utils.onnx import SamOnnxModel
        from mobile_sam.utils.transforms import ResizeLongestSide

        checkpoint = upstream / "weights/mobile_sam.pt"
        model = sam_model_registry["vit_t"]().cpu().eval()
        model.load_state_dict(torch.load(checkpoint, map_location="cpu", weights_only=True))
        encoder = model.image_encoder.eval()
        decoder = SamOnnxModel(model, return_single_mask=False).eval()
        predictor = SamPredictor(model)
        encoder_names = ["image_embeddings"]
        decoder_names = encoder_names + ["point_coords", "point_labels", "mask_input", "has_mask_input", "orig_im_size"]
        report["source"] = {"repo": SOURCES["MobileSAM"][0], "revision": SOURCES["MobileSAM"][1], "license": "Apache-2.0", "checkpoint_sha256": sha256(checkpoint)}

    def image_tensor(image):
        if sam2:
            return predictor._transforms(image).unsqueeze(0)
        resized = ResizeLongestSide(1024).apply_image(image)
        tensor = torch.from_numpy(resized.copy()).permute(2, 0, 1).unsqueeze(0)
        return model.preprocess(tensor)

    def prompts(image, points, labels, previous=None):
        height, width = image.shape[:2]
        original = np.asarray(points, np.float32) * np.array([width, height], np.float32)
        if sam2:
            coords = original * np.array([1024 / width, 1024 / height], np.float32)
            label_tensor = torch.tensor([labels], dtype=torch.int32)
        else:
            coords = ResizeLongestSide(1024).apply_coords(original, (height, width))
            coords = np.concatenate((coords, np.zeros((1, 2), np.float32)))
            label_tensor = torch.tensor([labels + [-1]], dtype=torch.float32)
        return (torch.tensor(coords[None], dtype=torch.float32), label_tensor, torch.zeros(1, 1, 256, 256) if previous is None else torch.from_numpy(previous.copy()), torch.tensor([float(previous is not None)]), torch.tensor([height, width], dtype=torch.int64 if sam2 else torch.float32))

    sample_image = load_rgb(images[0])
    sample = (image_tensor(sample_image),)
    encoder_path = args.output / (args.model + "_encoder.onnx")
    decoder_path = args.output / (args.model + "_decoder.onnx")
    encoder_session = export_graph(encoder, sample, encoder_path, ["image"], encoder_names, verify_only=args.verify_only, normalize_sam2=sam2)
    with torch.inference_mode():
        features = encoder(*sample)
    if isinstance(features, torch.Tensor):
        features = (features,)
    decoder_sample = (*features, *prompts(sample_image, [[0.5, 0.5], [0.1, 0.1]], [1, 0]))
    dynamic_axes = {"point_coords": {1: "num_points"}, "point_labels": {1: "num_points"}, "masks": {2: "original_height", 3: "original_width"}}
    decoder_session = export_graph(decoder, decoder_sample, decoder_path, decoder_names, ["masks", "iou_predictions", "low_res_masks"], dynamic_axes, args.verify_only)
    cases = [("positive", [[0.5, 0.5]], [1]), ("positive_negative", [[0.5, 0.5], [0.1, 0.1]], [1, 0]), ("three_points", [[0.4, 0.4], [0.6, 0.6], [0.1, 0.1]], [1, 1, 0]), ("refine", [[0.5, 0.5]], [1])]
    for image_path in images:
        image = load_rgb(image_path)
        inputs = (image_tensor(image),)
        reference_features = numpy_outputs(encoder, inputs)
        actual_features = run(encoder_session, inputs)
        for name, expected, actual in zip(encoder_names, reference_features, actual_features):
            add_check(report, image_path.name + "/" + name, compare_tensor(expected, actual))
        predictor.set_image(image)
        previous_reference = previous_actual = None
        for label, points, labels in cases:
            refine = label == "refine"
            prompt_reference = prompts(image, points, labels, previous_reference if refine else None)
            prompt_actual = prompts(image, points, labels, previous_actual if refine else None)
            expected = numpy_outputs(decoder, (*[torch.from_numpy(f.copy()) for f in reference_features], *prompt_reference))
            actual = run(decoder_session, (*[torch.from_numpy(f.copy()) for f in actual_features], *prompt_actual))
            height, width = image.shape[:2]
            native = predictor.predict(point_coords=np.asarray(points, np.float32) * np.array([width, height]), point_labels=np.asarray(labels), mask_input=previous_reference[0] if refine else None, multimask_output=True, return_logits=True)
            offset = 0 if sam2 else 1
            native_expected = [expected[0][0, offset:], expected[1][0, offset:], expected[2][0, offset:]]
            for name, left, right in zip(("masks", "iou_predictions", "low_res_masks"), native_expected, native):
                add_check(report, image_path.name + "/" + label + "/native_" + name, compare_tensor(left, right, max_mismatch_fraction=0.001))
            for name, left, right in zip(("masks", "iou_predictions", "low_res_masks"), expected, actual):
                add_check(report, image_path.name + "/" + label + "/" + name, compare_tensor(left, right, max_mismatch_fraction=0.001))
            add_check(report, image_path.name + "/" + label + "/mask_probability", compare_mask(sigmoid(expected[0]), sigmoid(actual[0])))
            best = int(np.argmax(expected[1][0, offset:])) + offset
            actual_best = int(np.argmax(actual[1][0, offset:])) + offset
            add_check(report, image_path.name + "/" + label + "/chosen_candidate", {"passed": best == actual_best, "pytorch": best, "onnx": actual_best})
            previous_reference = expected[2][:, best:best + 1]
            previous_actual = actual[2][:, actual_best:actual_best + 1]
    report["graphs"] = [graph_info(encoder_path, encoder_session), graph_info(decoder_path, decoder_session)]
    report["preprocess"] = {"image_size": 1024, "layout": "NCHW", "color": "RGB sRGB", "resize": "antialiased bilinear tensor square, align_corners=False" if sam2 else "bilinear PIL longest side; normalize, then bottom/right zero-pad", "mean": [0.485, 0.456, 0.406] if sam2 else [123.675, 116.28, 103.53], "std": [0.229, 0.224, 0.225] if sam2 else [58.395, 57.12, 57.375], "pixel_range_before_normalize": "0..1" if sam2 else "0..255", "batch": 1, "point_coordinates": "resized image pixels (x,y)", "point_labels": "1 foreground, 0 background; MobileSAM callers append one -1 padding point", "outputs": "FP32 logits, predicted IoU, low-resolution logits; select mask by predicted IoU; foreground threshold = 0 logits", "mask_count": 3 if sam2 else 4, "multimask_candidates": "all three" if sam2 else "indices 1..3; index 0 is the separate single-mask token", "prompt_scope": "point prompts and iterative mask refinement; boxes and image batching are not validated"}


def main():
    parser = argparse.ArgumentParser(description="Export pinned segmentation weights and verify FP32 ONNX against PyTorch on CPU")
    parser.add_argument("--model", choices=["birefnet", "birefnet-lite", "sam2", "mobilesam"], required=True)
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--images", nargs="+", type=Path)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if args.threads < 1:
        parser.error("--threads must be positive")
    if args.verify_only and args.output is None:
        parser.error("--verify-only requires --output")
    args.cache = args.cache.resolve()
    os.environ.setdefault("HF_MODULES_CACHE", str(args.cache / "transformers-modules"))
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    args.output = (args.output or ROOT / "artifacts/ai1" / args.model / stamp).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    report = {"model": args.model, "status": "incomplete", "utc": stamp, "provider": "CPUExecutionProvider", "precision": "FP32", "opset": 17, "tensor_limits": TENSOR_LIMITS, "decoder_max_mismatch_fraction": 0.001, "mask_limits": MASK_LIMITS, "checks": [], "versions": {name: importlib.metadata.version(name) for name in ("torch", "torchvision", "onnx", "onnxruntime", "transformers", "numpy", "pillow")}, "acceptance_scope": "Numerical export fidelity, not ground-truth segmentation quality, DirectML compatibility or Mac parity"}
    try:
        import torch

        torch.manual_seed(20261002)
        torch.set_num_threads(args.threads)
        if args.images is None:
            fixtures = source("sam2", args.cache) / "notebooks/images"
            args.images = [fixtures / name for name in ("truck.jpg", "cars.jpg", "groceries.jpg")]
        report["images"] = [{"file": str(path.resolve()), "sha256": sha256(path)} for path in args.images]
        print(f"Output: {args.output}", flush=True)
        with torch.inference_mode():
            if args.model.startswith("birefnet"):
                export_birefnet(args, args.images, report)
            else:
                export_sam(args, args.images, report)
        report["status"] = "passed" if report["checks"] and all(check["passed"] for check in report["checks"]) else "failed"
        print("Graphs: " + json.dumps(report["graphs"]), flush=True)
        masks = [check for check in report["checks"] if "iou" in check]
        print("Summary: " + json.dumps({"checks": len(report["checks"]), "failed": sum(not check["passed"] for check in report["checks"]), "min_mask_iou": min(check["iou"] for check in masks), "max_mask_mean_abs_error": max(check["max_mask_mean_abs_error"] for check in masks)}), flush=True)
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        report_path = args.output / ("verification-" + stamp + ".json")
        report_path.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
        print(f"Status: {report['status']}; report: {report_path}", flush=True)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
