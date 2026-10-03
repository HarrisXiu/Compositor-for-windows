# AI1 implementation and acceptance — 2026-10-03

AI1's local implementation is integrated and regression-tested. Full acceptance remains open for AMD and physical machines without a DX12 GPU; the user will run the offline self-test on another AMD computer and bring back results. No remote Release, model upload, push or signing is performed.

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

## Remaining acceptance

- AMD: inspect the user's returned precision/profile reports.
- Physical no-DX12 machine: confirm automatic CPU fallback and usable performance.
- Hardware-wide speed/memory, segmentation ground-truth quality and Mac parity remain unproven.
- Subject/object selection, matte refinement and background-removal UI are later application tools.
- Controlled responses cover download regression because the model release remains unpublished; real model-release endpoint acceptance is deferred.

Local release materials contain sizes/hashes, provenance, model cards, separate licenses/notices and validation reports. published=false prevents requests to nonexistent release URLs. There is no updater or update checker.
