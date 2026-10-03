# AI1 current implementation and acceptance — 2026-10-03

The C++ inference foundation, model manager and complete offline test delivery are integrated and regression-tested. DirectML is limited to supported Intel integrated GPUs; NVIDIA/AMD and discrete Intel use CPU. NVIDIA CUDA is deferred and not implemented. Application subject/object selection, matte refinement and background-removal interactions remain later work.

User-facing instructions and model sizes are in [AI1 README](AI1_README.md). Earlier multi-vendor results, compatibility experiments and failed GPU reports are preserved in [historical validation](AI1_HISTORY.md), not used as current GPU acceptance.

## Supported runtime behavior

Automatic selection prefers a supported Intel integrated GPU even on an Intel/NVIDIA hybrid computer. Eligibility requires Intel vendor 0x8086, hardware DX12 support, a non-software adapter and D3D12 UMA reported by the architecture feature query. Discrete Intel GPUs and adapters with unavailable architecture information are excluded. See [Microsoft architecture documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture1).

Intel uses portable kernels by default because vendor metacommands exceeded MobileSAM fidelity tolerances. PreferDirectML records an explanation and retries CPU when initialization, device status or finite-output checks fail. Strict DirectML rejects unsupported adapters before provider registration, and diagnostic flags do not override the hardware gate. Background execution, cancellation and SAM encoder reuse are implemented.

C++ image resizing, normalization, padding and point conversion match the reference preprocessing on truck/cars/groceries. SAM 2's erroneous [5] annotation in an unused nested Tile branch was removed; computation and weights are unchanged, and nine CPU encoder tensors remained bitwise identical. ONNX Runtime DirectML 1.24.4 / DirectML 1.15.4 and model hashes are pinned.

## Current verification

| Check | Evidence |
| --- | --- |
| Release build / regression | Build succeeds; eight CTest suites pass |
| Automatic Intel UHD selection | All four real models pass 150/150 checks with actual GPU nodes |
| Explicit NVIDIA CPU fallback | All 132 checks pass; all encoder/decoder backends are CPU, with reasons and zero DirectML profiler nodes |
| Intel nonfinite-output regression | 3/3 Qt cases pass, no skips |
| Intel device-loss regression | 3/3 Qt cases pass, no skips; strict failure and correct CPU retry |
| Hardware-scope policy | Reject AMD/NVIDIA, discrete Intel, software and non-DX12 fixtures; actual unsupported adapters use CPU |
| Windows PowerShell 5.1 scope branch | 4/4: AMD/NVIDIA-only inventory, hybrid automatic, explicit NVIDIA and explicit Intel |
| Complete ZIP | All 263 files match SHA256; native binaries and self-test script match the build/source |
| Extracted bundle launcher | Integrity, runtime/TLS, eight suites, application smoke and 132 CPU fallback checks pass |

The Intel opt-in regressions were run separately. The complete-bundle launcher used -SkipCpu -AdapterIndex 0 to exercise an explicitly selected NVIDIA adapter; it produced results-20261003-082658 with all 15 stages passed or appropriately skipped. CPU fallback was numerically verified against all four reference models. The no-Intel inventory test is a fixture, not a physical AMD or no-DX12 hardware report.

Earlier export tooling verification recorded ten passing Python tests without downloading weights. Model/download regressions cover resume, cancellation, corrupt files and controlled offline responses. The real model release endpoint is unpublished, so production endpoint acceptance remains deferred.

These checks establish ONNX numerical fidelity and runtime/packaging regression on the recorded hardware. They do not establish labeled segmentation quality, portrait/hair quality, Mac parity or a universal speed guarantee. Failed and skipped GPU checks are never counted as passed.

## Performance

CPU inference uses half the logical processors, bounded to 1–12 threads. Lite inference excludes loading/preprocessing:

| Hardware / threads | Measurement |
| --- | --- |
| i9-13900HX, 4 threads | 9.92 s average over six runs |
| i9-13900HX, 8 threads | 5.26 s average over six runs |
| i9-13900HX, 12 threads | 3.80 s average over six runs |
| Ryzen 7 7800X3D, 8 threads | 3.839 s average over nine returned runs |

No FP16/quantization or reduced fidelity threshold is introduced.

## Delivery and evidence locations

All paths below are relative to the repository worktree; models/cache/logs remain Git-ignored.

| Location | Contents |
| --- | --- |
| artifacts/ai1/intel-only-self-test/AI1-complete-self-test.zip | Current complete offline test bundle |
| artifacts/ai1/intel-only-self-test/DELIVERY-INFO.json | Bundle size, SHA256, model IDs and unpublished status |
| artifacts/ai1/intel-only-auto-fidelity.json | Current Intel 150/150 report |
| artifacts/ai1/intel-only-gpu-regression.txt | Intel nonfinite-output regression |
| artifacts/ai1/intel-only-device-loss.txt | Intel device-loss regression |
| artifacts/ai1/intel-only-script-regression.log | Four scope-branch fixture checks |
| artifacts/ai1/intel-only-self-test-extracted/AI1-self-test/results-20261003-082658/ | Actual packaged CPU fallback and runtime evidence |
| artifacts/ai1/returned-7800x3d-4070super/ | Unchanged copies of returned reports and analysis |

The complete bundle is 1,657,956,111 bytes; SHA256 is `310bb22650d2e22118ecb6dd3d7f9239a02e32e13f02414987a8215ee896f3bf`. It contains full BiRefNet, Lite, SAM 2, MobileSAM, reference tensors/images, application/probe/test binaries, runtime DLLs, Qt plugins, model licenses and third-party notices. Its single launcher is 开始测试.cmd.

The application model manager offers Lite, SAM 2 and MobileSAM. Local import checks SHA256/size and installs atomically with licenses. The editor package does not contain weights or Python. Model release materials remain local and published=false disables requests to unreleased addresses.

## Remaining work and scope

- Application subject/object selection, matte refinement and background-removal interactions.
- NVIDIA CUDA as a separate implementation and acceptance task.
- Physical no-DX12 automatic CPU fallback, broader Intel hardware and performance acceptance.
- Labeled segmentation quality and Mac reference comparisons.
- Real model-release endpoint testing after separately authorized publication.

NVIDIA/AMD DirectML adaptation and acceptance are discontinued by user decision. Historical device hangs/nonfinite outputs remain failed evidence, not fixed or passing results. No remote Release, upload, push or signing is performed.
