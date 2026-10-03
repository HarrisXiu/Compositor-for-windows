"""Build a small offline diagnostic overlay for an existing AI1 self-test bundle."""
import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = args.output.resolve() / "AI1-gpu-retest"
    if root.exists():
        raise SystemExit("Choose a fresh output directory; existing diagnostics are preserved.")
    (root / "probe").mkdir(parents=True)
    shutil.copy2(args.probe, root / "probe/compositor_ai_probe.exe")
    shutil.copy2(source / "ai-gpu-retest.ps1", root / "gpu-retest.ps1")
    shutil.copy2(source / "AI1_GPU_RETEST.md", root / "README.md")
    (root / "run-gpu-retest.cmd").write_text(
        '@echo off\npowershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0gpu-retest.ps1" %*\n'
        'set "taskExit=%ERRORLEVEL%"\npause\nexit /b %taskExit%\n', encoding="ascii")
    records = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            records.append({"path": path.relative_to(root).as_posix(),
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    (root / "SHA256SUMS.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    archive = args.output.resolve() / "AI1-gpu-retest.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                output.write(path, path.relative_to(root.parent))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.output / "DELIVERY-INFO.txt").write_text(
        f"File: {archive.name}\nBytes: {archive.stat().st_size}\nSHA256: {digest}\n",
        encoding="utf-8")
    print(f"{archive}\nBytes: {archive.stat().st_size}\nSHA256: {digest}")


if __name__ == "__main__":
    main()
