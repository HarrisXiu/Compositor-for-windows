# AI1 implementation and acceptance — 2026-10-03

AI1's local implementation is integrated and regression-tested. Full acceptance remains open: returned AMD/NVIDIA evidence contains GPU failures, and physical machines without a DX12 GPU remain untested. No remote Release, model upload, push or signing is performed.

## Implementation

- C++20 ONNX Runtime DirectML 1.24.4 / DirectML 1.15.4; compatible adapter selection, CPU fallback with reason, background work, in-flight cancellation, bounded tensors and SAM encoding reuse.
- C++ sRGB RGB preprocessing, Pillow-compatible resizing/rounding, SAM 2 antialiased tensor resize, normalization/padding and point conversion; compared with Python on truck/cars/groceries.
- Help > AI Models and Chinese/Japanese translations; local cache inspection, atomic verified local import and license repair.
- HTTPS downloader: optional, bounded and SHA256 checked; Range/strong If-Range resumption, restart when Range is ignored, cancellation with partial-file retention, invalid size/encoding/range rejection and installation locks.
- Lite reduces FP32 size from about 930 MB to 181.7 MB. Weights are separate from the application; catalog URLs deliberately remain unpublished.

## Fixes

Intel MobileSAM failed seven tensor checks with vendor metacommands. Portable DirectML kernels pass unchanged tolerances with GPU execution; Intel defaults to this path. --vendor-metacommands is a diagnostic override. The original failing report is preserved.

SAM 2's warning came from [5] in a nested, unused Tile branch output. Recursive normalization removes only that annotation. Strict shape inference passes; nine encoder tensors are bitwise identical. Computation/weights are unchanged. The corrected encoder is six bytes smaller, with updated catalog integrity metadata.

CPU Lite, i9-13900HX, three images and two unprofiled runs/image:

| Threads | Mean inference time |
| --- | --- |
| 4 | 9.92 s |
| 8 | 5.26 s |
| 12 | 3.80 s |

Load/preprocessing are separate. Default threads use half the logical processors, bounded to 1–12. No FP16/quantization or lowered fidelity thresholds were introduced.

## Verification

Local evidence stays under artifacts/ai1/; models/logs are Git-ignored. Reports record provider/adapter, model hashes, tensor errors, per-candidate IoU/probability MAE, chosen SAM mask identity, encoder reuse, load/run/preprocessing time and actual provider node execution. Failed/interrupted/skipped hardware checks are never counted as passed. These checks establish fidelity, not ground-truth quality.

Release passes eight CTest suites, including model/download regressions. Ten Python tooling tests pass without fetching weights. The opt-in real HTTPS test downloads a pinned 1073-byte ONNX Runtime MIT license over Schannel and verifies its SHA256; all 20 Qt model-test cases pass in that run. Final default-thread real-model reports pass: CPU 132/132, Intel UHD 138/138, and NVIDIA RTX 4080 Laptop 138/138, across full BiRefNet, Lite, SAM 2 and MobileSAM. Packaging includes runtime DLLs and their complete original licenses/notices, Qt Network and Schannel. Self-tests exercise packaged dependencies with SDK directories removed from PATH.

windows/ai-self-test.ps1 runs offline on another Windows x64 computer, verifies integrity, records hardware/drivers, runs eight suites and actual models, then creates a results ZIP without weights/reference tensors. GPU default-path failures remain failed even if separate portable-kernel diagnostics pass. See AI1_SELF_TEST.md.

## Returned second-machine evidence

The user's results-20261003-180958 was preserved unchanged under artifacts/ai1/returned-7800x3d-4070super/. Hardware: Ryzen 7 7800X3D (8 cores / 16 logical processors), Radeon integrated graphics (driver 32.0.21030.2001), RTX 4070 SUPER (32.0.16.1692), Windows 11 25H2 and a MuMu virtual display driver exposing a duplicate NVIDIA adapter. That driver is inventory evidence; causation is unproven.

Integrity, TLS, eight suites, application smoke and CPU 132/132 pass. Lite CPU nine unprofiled runs average 3881.05 ms (3858.85–3934.07 ms), excluding load/preprocessing.

NVIDIA adapter 0 and duplicate adapter 2 pass 137/138 checks on the default path: full BiRefNet 7/7, SAM 2 65/65, MobileSAM 59/59, Lite 6/7. Lite groceries has mask IoU 0.42480 and probability MAE 0.42515, far outside 0.995/0.001 limits, and takes 5.55–6.74 seconds instead of about 0.11 seconds. Portable diagnostics also fail 137/138, this time full BiRefNet groceries (IoU 0.37127, probability MAE 0.52768); Lite passes there.

AMD full BiRefNet truck passes, cars fails (mask IoU 0.42645, probability MAE 0.27056), then groceries raises 0x887A0006 (device hung) in DmlFusedNode_0_3. Both kernel modes fail. The old all-model process aborts, so no AMD Lite/SAM acceptance can be inferred.

Root cause remains unconfirmed. No tolerance was relaxed. A runtime guard now checks both DirectML and parent D3D12 device removal before/after Run; PreferDirectML releases the failed GPU session and retries CPU, while DirectMLOnly fails explicitly. This detects device loss, not every numerical defect. ONNX Runtime 1.24.4 returns a borrowed GetDMLDevice pointer, so the guard retains its own COM reference. See [Microsoft device-removal guidance](https://learn.microsoft.com/en-us/windows/ai/directml/dml-errors) and the [pinned factory implementation](https://github.com/microsoft/onnxruntime/blob/v1.24.4/onnxruntime/core/providers/dml/dml_provider_factory.cc).

The probe now preserves per-model runtime failures, verifies every repeated BiRefNet output, and supports image filtering/reversed order. Normal self-tests use separate GPU processes per model. A small offline overlay, windows/ai-gpu-retest.ps1 / prepare_ai_gpu_retest.py, diagnoses isolated models, repeated/reversed/single-image runs, portable kernels and automatic fallback. It stages verified original DLLs beside the new EXE because Windows' System32 contains an older ONNX Runtime on our development machine. Diagnostic successes never replace a failed default test. The overlay does not replace the application, original models, base manifest or returned evidence. See AI1_GPU_RETEST.md.

Updated Release passes all eight CTest suites. A separate opt-in directMLDeviceLossFallsBack test creates/destroys five GPU sessions, invalidates the process's logical D3D12 device, checks strict-mode failure and verifies PreferDirectML returns correct CPU output with a device diagnostic; 3/3 Qt cases pass without skips. A probe fixture retains a missing-model failure while successfully validating the next model and every repeated output. The final small overlay runs in Windows PowerShell 5.1 with packaged DLLs: RTX 4080 Laptop and Intel UHD each pass 150/150 checks across all four models, including three runs per BiRefNet image. These local results do not supersede the returned machine's failed acceptance.

## Remaining acceptance

- Retest the returned AMD integrated / RTX 4070 SUPER GPU failures with the diagnostic overlay; root cause and acceptance remain open.
- Physical no-DX12 machine: confirm automatic CPU fallback and usable performance.
- Hardware-wide speed/memory, segmentation ground-truth quality and Mac parity remain unproven.
- Subject/object selection, matte refinement and background-removal UI are later application tools.
- Controlled responses cover download regression because the model release remains unpublished; real model-release endpoint acceptance is deferred.

Local release materials contain sizes/hashes, provenance, model cards, separate licenses/notices and validation reports. published=false prevents requests to nonexistent release URLs. There is no updater or update checker.

The diagnostic launcher now discovers bundles placed alongside it or under repeated AI1-self-test extraction directories, and offers a folder picker when automatic discovery fails. Windows PowerShell 5.1 layout checks pass 8/8 (inside/sibling/double extraction, explicit outer directory, Chinese/spaced paths and missing files); the actual existing bundle also resolves. This changes launcher paths only; the previously validated native probe is unchanged.
