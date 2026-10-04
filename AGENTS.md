# Notes for AI agents

Compositor is a macOS image editor for compositing and photo work, written in Swift (SwiftUI and AppKit, with some C for pixel work).

## Designing or editing a Compositor project

If you've been asked to make or change an image in a `.comp` project, you don't need the app's source code. Read [docs/writing-comp-files.md](docs/writing-comp-files.md): it covers the file format, the rules that make a project load, and how to write it safely while it's open, so the person can watch the canvas update as you work.

## Working on the app itself

- Build: open `Compositor.xcodeproj` and run the **Compositor** scheme, or `xcodebuild -project Compositor.xcodeproj -scheme Compositor -destination 'platform=macOS' build`.
- Tests: the `CompositorTests` target (`xcodebuild ... test -only-testing:CompositorTests`). CI runs these on every push.
- Match the surrounding code: its naming, its comment style and density.
- American spelling in code, comments and UI ("color", not "colour").
- The project file format is described in [docs/project-format.md](docs/project-format.md). A change to what's saved means a format version bump there and in `ProjectManifest.current`.

## Windows offline AI model tooling

- Python 3.12 export dependencies are pinned in `windows/model-export-requirements.txt`; use an isolated environment when setting them up. This tooling does not add Python to the application runtime.
- Tooling tests: `python -B windows/tests/model_export_tests.py`. They do not require downloading model weights.
- Export and compare: `python -B windows/export_models.py --model birefnet`, `--model sam2`, or `--model mobilesam`. Source/weight revisions are pinned; defaults cache under `.cache/` and write separate timestamped folders under `artifacts/ai1/`.
- Recheck existing graphs with `--verify-only --output <folder>`. Reports contain graph signatures, SHA256 hashes and per-case checks; preserve the generated model license files. Keep models/cache artifacts out of Git.
- Passing CPU FP32 export checks is not DirectML, ground-truth segmentation-quality or Mac parity acceptance. AI1 C++ inference/model management is integrated; application subject/object selection, refinement and background removal remain separate tasks. See `windows/README.md` and `windows/AI1_INTEGRATION_REPORT.md` for the detailed contract and scope.
