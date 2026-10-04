# AI1 integration — 2026-10-04

Merged `ai1-runtime` at `42f4103` into `the-one-for-windows`, based on `7fcba58`. The existing L1–L4/R1–R4/C1–C5 editor behavior is retained. Application version remains **0.4.0**, `.comp` remains **11**, project source remains MIT licensed, and automatic updating remains excluded.

## Integrated behavior

- C++ ONNX Runtime inference and model management, background tasks/cancellation, SAM encoder reuse and iterative mask input.
- Verified local import, model/license cache, unpublished download controls, finite-output/device-loss protection and CPU fallback.
- DirectML remains restricted to supported Intel integrated GPUs. Other adapters use CPU; NVIDIA CUDA is deferred.
- Both C1–C5 and AI1 Chinese/Japanese catalogs are retained. The AI Models action joins the existing remappable menu-command registry.
- Nine CTest suites: the existing seven editor/rendering suites plus AI inference and model/download tests. The canvas executable now receives the AI runtime dependencies; offline delivery tooling includes all nine suites.

Subject/object selection, matte refinement and background-removal interactions are still later application work. This merge adds their foundation and model manager, not those editing tools.

## Verification

| Check | Result |
| --- | --- |
| MSVC Release configure/build | Passed; all application, probe and test targets built |
| Combined CTest | **9/9 suites passed**, 0 failed |
| Python model tooling | **10 tests passed**, no weight download |
| Real models, CPU | **144/144 checks passed**; full BiRefNet, Lite, SAM 2 and MobileSAM on three reference images, repeated BiRefNet runs |
| Real models, Intel UHD DirectML | **150/150 checks passed**; all four models, actual GPU nodes recorded in encoder/decoder profiles |
| Intel invalid-output regression | **3/3 Qt cases passed**, no skips |
| Intel device-loss regression | **3/3 Qt cases passed**, no skips |
| Dependencies without SDK directories in PATH | Application smoke and AI/shortcut menu integration **passed**; menu Qt checks 3/3 |
| Chinese/Japanese UI integration | Both AI model and shortcut commands remain registered and translated; model dialog opens successfully |
| Deployed runtime hashes | ONNX Runtime and DirectML DLL hashes match the pinned SDK copies |
| Windows PowerShell 5.1 | Build, SDK-fetch and nine-suite self-test scripts parse successfully |

Real-model verification reports are `artifacts/ai1-integration/cpu.json` and `dml.json`, produced by the merged probe against unchanged reference data with three repeated BiRefNet runs per image. DirectML selected Intel UHD; SAM decoders also use CPU for some operators. Profiles confirm actual GPU execution, and strict DirectML did not silently retry the whole session on CPU. The default CTest run retains its opt-in benchmark, network and hardware skips; a passing suite is not a claim that every optional case ran. The two Intel opt-in regressions above ran separately without skips.

Local configuration succeeded through the build script. MSBuild initially inherited conflicting `Path`/`PATH` entries from reusable host processes; the verified build used a normalized child environment and disabled node reuse. The existing isolated Python environment required execution outside the sandbox to start its base Python. These were local execution constraints; no dependencies or weights were reinstalled.

## Evidence and delivery boundaries

Current logs and model reports are in this worktree's ignored `artifacts/ai1-integration/`; the build log is `artifacts/ai1-integration-build.log`. Model/graph files, sources and reference data were reused from existing caches and the sibling `Compositor-ai1` worktree. No weights were re-exported or modified.

The 2026-10-03 complete AI1 ZIP, its hash/263-file manifest and eight-suite acceptance remain historical evidence in the sibling worktree. They were not regenerated or copied here, and do not contain the latest C1–C5 integration. The merged delivery script now includes the canvas test for any future complete bundle.

No portable ZIP, model release, remote Release or asset upload was produced for this integration. No Mac build or clean-machine/full segmentation-quality acceptance was performed. Historical NVIDIA/AMD DirectML failures remain preserved; these adapters are outside the current GPU support scope.

Remaining work: AI editing interactions, broader Intel/no-DX12 hardware acceptance, segmentation-quality/Mac comparisons, NVIDIA CUDA, and production model-download acceptance after model publication. See [migration status](PORTING_STATUS.md) and [AI1 usage](AI1_README.md).
