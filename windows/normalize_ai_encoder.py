import argparse
import copy
import json
import shutil
import zlib
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort

from export_models import compare_tensor, sha256


def normalize_sam2_shapes(model):
    """Remove the legacy exporter's incorrect shape on an unused Tile branch."""
    changed = []

    def visit(graph):
        for node in graph.node:
            for attribute in node.attribute:
                if attribute.type != onnx.AttributeProto.GRAPH:
                    continue
                branch = attribute.g
                for value in branch.output:
                    if value.name == "/image_encoder/trunk/Concat_3_output_0":
                        tensor = value.type.tensor_type
                        if tensor.HasField("shape"):
                            if [dim.dim_value for dim in tensor.shape.dim] != [5]:
                                raise ValueError("Unexpected SAM 2 Tile branch annotation; do not patch this graph")
                            tensor.ClearField("shape")
                            changed.append(value.name)
                visit(branch)

    visit(model.graph)
    return changed


def main():
    parser = argparse.ArgumentParser(description="Check whether stale intermediate ONNX shapes cause SAM 2's MergeShapeInfo warning")
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--output-model", type=Path, required=True)
    parser.add_argument("--output-manifest", type=Path, required=True)
    args = parser.parse_args()
    if args.output_model.exists() or args.output_manifest.exists():
        raise FileExistsError("Use new output paths; original models and references are preserved")
    original = json.loads(args.references.read_text(encoding="utf-8"))
    record = next(item for item in original["models"] if item["id"] == "sam2")
    source = Path(record["encoder"])
    if sha256(source) != record["encoder_sha256"]:
        raise ValueError("Source encoder differs from reference manifest")
    graph = onnx.load(str(source))
    changed = normalize_sam2_shapes(graph)
    if not changed:
        raise ValueError("The source encoder does not contain the known incorrect Tile branch annotation")
    args.output_model.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(graph, str(args.output_model))
    onnx.checker.check_model(str(args.output_model), full_check=True)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    print("Original encoder session", flush=True)
    before = ort.InferenceSession(str(source), sess_options=options, providers=["CPUExecutionProvider"])
    options.add_session_config_entry("session.strict_shape_type_inference", "1")
    print("Normalized encoder session (strict shape inference)", flush=True)
    after = ort.InferenceSession(str(args.output_model), sess_options=options, providers=["CPUExecutionProvider"])
    checks = []
    for image in record["images"]:
        metadata = image["input"]
        data = (args.references.parent / metadata["file"]).read_bytes()
        array = np.frombuffer(zlib.decompress(data[4:]), dtype=np.dtype(metadata["type"])).reshape(metadata["shape"])
        old = before.run(None, {"image": array})
        new = after.run(None, {"image": array})
        for output, left, right in zip(before.get_outputs(), old, new, strict=True):
            result = compare_tensor(left, right, atol=0, rtol=0, max_mismatch_fraction=0)
            checks.append(result)
            print(image["file"] + "/" + output.name + ": " + json.dumps(result), flush=True)
    if not all(item["passed"] for item in checks):
        raise RuntimeError("Metadata normalization changed CPU inference output")
    updated = copy.deepcopy(original)
    item = next(model for model in updated["models"] if model["id"] == "sam2")
    item["original_encoder"] = item["encoder"]
    item["original_encoder_sha256"] = item["encoder_sha256"]
    item["encoder"] = str(args.output_model.resolve())
    item["encoder_sha256"] = sha256(args.output_model)
    item["normalization"] = {"nested_branch_shapes_cleared": changed, "cpu_bitwise_checks": len(checks)}
    if args.output_manifest.parent.resolve() != args.references.parent.resolve():
        raise ValueError("Keep the new manifest beside its unchanged tensor reference files")
    args.output_manifest.write_text(json.dumps(updated, indent=2), encoding="utf-8")
    for filename in ("LICENSE-SAM2.txt", "LICENSE-ONNX-Runtime-exporter.txt", "MODEL-CARD.md"):
        if (source.parent / filename).is_file():
            shutil.copyfile(source.parent / filename, args.output_model.parent / filename)
    print(json.dumps({"shapes_cleared": changed, "bytes": args.output_model.stat().st_size, "sha256": item["encoder_sha256"], "manifest": str(args.output_manifest)}), flush=True)


if __name__ == "__main__":
    main()
