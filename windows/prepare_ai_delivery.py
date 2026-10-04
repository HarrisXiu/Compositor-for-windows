"""Prepare offline AI1 self-tests and unpublished model release materials.

Only the Python standard library is needed. This script never contacts a server.
"""
import argparse
import copy
import hashlib
import json
import shutil
import subprocess
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reference_files(value):
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "file":
                if not isinstance(child, str) or Path(child).name != child:
                    raise ValueError("Unsafe tensor/image reference filename")
                yield child
            else:
                yield from reference_files(child)
    elif isinstance(value, list):
        for child in value:
            yield from reference_files(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True, help="Release build directory containing test EXEs")
    parser.add_argument("--qt-root", type=Path, required=True, help="Qt SDK, used only to copy QtTest/offscreen")
    parser.add_argument("--output", type=Path, required=True, help="New output folder; existing files are never overwritten")
    parser.add_argument("--include-full-birefnet", action="store_true")
    parser.add_argument("--verification", type=Path, action="append", default=[])
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Use a new delivery folder; existing results are preserved")
    probe = args.package / "compositor_ai_probe.exe"
    references = json.loads(args.references.read_text(encoding="utf-8"))
    if references.get("version") != 1:
        raise ValueError("Unsupported reference manifest")
    args.output.mkdir(parents=True)
    release = args.output / "model-release"
    release.mkdir()
    subprocess.run([str(probe.resolve()), "--catalog", str((release / "catalog.json").resolve())], check=True)
    subprocess.run([str(probe.resolve()), "--write-model-licenses", str((release / "notices").resolve())], check=True)
    catalog = json.loads((release / "catalog.json").read_text(encoding="utf-8"))
    if any(model["published"] for model in catalog["models"]):
        raise ValueError("This preparation flow is for unpublished model releases")
    records = {model["id"]: model for model in references["models"]}
    definitions = {model["id"]: model for model in catalog["models"]}
    for identifier, model in definitions.items():
        record = records[identifier]
        for asset in model["assets"]:
            role = "decoder" if "decoder" in asset["filename"] else "encoder"
            source = Path(record[role])
            if not source.is_absolute():
                source = args.references.parent / source
            if source.stat().st_size != asset["bytes"] or sha256(source) != asset["sha256"]:
                raise ValueError(f"Model differs from built-in catalog: {identifier}/{asset['filename']}")
            shutil.copy2(source, release / asset["filename"])
            card = source.parent / "MODEL-CARD.md"
            if card.is_file():
                shutil.copy2(card, release / "notices" / identifier / "MODEL-CARD.md")
    reports = release / "validation"
    reports.mkdir()
    covered = set()
    for path in args.verification:
        report = json.loads(path.read_text(encoding="utf-8"))
        if report.get("status") != "passed" or not report.get("checks") or not all(item["passed"] for item in report["checks"]):
            raise ValueError(f"Verification is incomplete or failed: {path}")
        for model in report["models"]:
            identifier = model["id"]
            if identifier not in definitions:
                continue
            record = records[identifier]
            if model.get("encoder_sha256") != record["encoder_sha256"] or model.get("decoder_sha256") != record.get("decoder_sha256"):
                raise ValueError(f"Verification model hashes differ: {identifier}")
            covered.add(identifier)
        shutil.copy2(path, reports / path.name)
    if args.verification and covered != set(definitions):
        raise ValueError("Verification reports do not cover every catalog model")
    (release / "RELEASE-NOTES.md").write_text(
        "# AI1 model release staging (unpublished)\n\n"
        "No remote release or upload was created. All URLs in catalog.json remain inactive "
        "until a separately authorized publication; published=false keeps downloads disabled.\n\n"
        "Assets are FP32 ONNX. BiRefNet Lite uses export-only GridSample/MatMul lowering. "
        "SAM 2 has one incorrect nested branch shape annotation removed; computation and weights are unchanged. "
        "Encoder/decoder pairs support image feature reuse, point prompts and iterative refinement. "
        "Retain each model's complete notices/ folder, upstream model card and validation reports with redistribution.\n\n"
        "Validation measures numerical fidelity on three upstream SAM 2 images, not labeled segmentation "
        "quality, portrait/hair acceptance, Mac parity or universal speed. DirectML is supported only "
        "on Intel integrated GPUs; NVIDIA/AMD use CPU. NVIDIA CUDA is deferred and not implemented. "
        "Machines without a supported Intel integrated GPU require CPU fallback reports. No signing is performed.\n",
        encoding="utf-8")
    sums = {path.relative_to(release).as_posix(): sha256(path) for path in sorted(release.rglob("*")) if path.is_file()}
    (release / "SHA256SUMS.json").write_text(json.dumps(sums, indent=2) + "\n", encoding="utf-8")
    bundle = args.output / "AI1-self-test"
    shutil.copytree(args.package, bundle / "bin")
    for name in ("compositor_tests", "compositor_model_tests", "compositor_ai_tests", "compositor_ui_tests",
                 "compositor_photoshop_tests", "compositor_raw_tests", "compositor_dither_tests", "compositor_layer_tests",
                 "compositor_canvas_tests"):
        shutil.copy2(args.build / (name + ".exe"), bundle / "bin")
    shutil.copy2(args.qt_root / "bin/Qt6Test.dll", bundle / "bin")
    shutil.copy2(args.qt_root / "plugins/platforms/qoffscreen.dll", bundle / "bin/platforms")
    manifest = copy.deepcopy(references)
    manifest["models"] = [model for model in manifest["models"] if model["id"] in definitions or
                          (args.include_full_birefnet and model["id"] == "birefnet")]
    for model in manifest["models"]:
        identifier = model["id"]
        destination = bundle / "models" / identifier
        destination.mkdir(parents=True)
        for role in ("encoder", "decoder"):
            if role not in model:
                continue
            source = Path(model[role])
            if not source.is_absolute():
                source = args.references.parent / source
            if sha256(source) != model[role + "_sha256"]:
                raise ValueError(f"Reference/model mismatch: {source}")
            shutil.copy2(source, destination / source.name)
            model[role] = "../models/" + identifier + "/" + source.name
        for field in ("original_encoder", "original_encoder_sha256"):
            model.pop(field, None)
        if identifier in definitions:
            for path in (release / "notices" / identifier).iterdir():
                shutil.copy2(path, destination)
        else:
            for path in source.parent.iterdir():
                if path.name.startswith("LICENSE") or path.name == "MODEL-CARD.md":
                    shutil.copy2(path, destination)
    tensors = bundle / "references"
    tensors.mkdir()
    for filename in sorted(set(reference_files(manifest["models"]))):
        shutil.copy2(args.references.parent / filename, tensors / filename)
    (tensors / "references.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    scripts = Path(__file__).resolve().parent
    shutil.copy2(scripts / "ai-self-test.ps1", bundle / "self-test.ps1")
    shutil.copy2(scripts / "AI1_SELF_TEST.md", bundle / "README.md")
    (bundle / "开始测试.cmd").write_text(
        '@echo off\r\npowershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0self-test.ps1" %*\r\n'
        'set "AI1_RESULT=%ERRORLEVEL%"\r\npause\r\nexit /b %AI1_RESULT%\r\n', encoding="ascii", newline="")
    integrity = [{"path": path.relative_to(bundle).as_posix(), "sha256": sha256(path)}
                 for path in sorted(bundle.rglob("*")) if path.is_file()]
    (bundle / "SHA256SUMS.json").write_text(json.dumps(integrity, indent=2) + "\n", encoding="utf-8")
    archive = args.output / "AI1-complete-self-test.zip"
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as output:
        for path in sorted(bundle.rglob("*")):
            if path.is_file():
                output.write(path, path.relative_to(args.output).as_posix())
    (args.output / "DELIVERY-INFO.json").write_text(json.dumps({
        "utc": datetime.now(timezone.utc).isoformat(), "model_release_published": False,
        "bundle": archive.name, "bundle_bytes": archive.stat().st_size, "bundle_sha256": sha256(archive),
        "model_ids": [model["id"] for model in manifest["models"]],
    }, indent=2) + "\n", encoding="utf-8")
    print(archive.resolve())


if __name__ == "__main__":
    main()
