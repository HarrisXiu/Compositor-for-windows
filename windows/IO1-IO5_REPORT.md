# IO1–IO5 implementation and verification — 2026-10-05

Branch: `codex/io1-io5`, based on `15837df`. Work is isolated in the managed `io1-io5` worktree because another chat is editing S2 in the original checkout. Application version remains **0.4.0**, project format remains **11**. No saved fields, model binaries, updater or release package are added.

IO1–IO4 are implemented. IO5 now implements Bezier path conversion, the six effects supported by the native document format, and pinned real-file regressions. This does **not** establish complete Photoshop interoperability: unsupported effects and appearance differences are listed below. The development plan's later full pixel-parity acceptance remains open.

## Delivered behavior

| Task | Implementation |
| --- | --- |
| IO1 | Parent-directory watching, package metadata polling, periodic asset SHA256 checks, 350 ms debounce, background loading and two stable observations. Manifest-only, image-only and atomic folder replacements refresh the canvas. Invalid/incomplete writes preserve the current document and retry. Modified tabs ask before replacing edits. Reload clears obsolete history and preserves zoom and a same-size selection. |
| IO2 | Background recovery snapshots every 60 seconds; configurable/disabled interval. Separate per-process locked recovery folders, startup Recover/Discard/Cancel, and modified untitled recovery copies. Clean saves/accepted closes remove owned snapshots. Recent files retain 20 unique successful paths. Staging cleanup removes only unlocked UUID staging folders older than 24 hours and retains backups. |
| IO3 | Default 512 MiB history budget per project, configurable from 16 to 8192 MiB. Unique shared QImage buffers count once; document metadata and selection/mask buffers count. Oldest entries are removed while preserving document pixels and retained undo/redo state. The existing 40-command cap also applies. |
| IO4 | Unicode HEIC/HEIF/HIF import through Windows WIC, bounded allocation, RGBA conversion, ICC-to-sRGB conversion when a profile is available, EXIF orientation and DPI. Missing native decoding support produces a HEIF/HEVC extension diagnostic. |
| IO5 | PSD/PSB Bezier knots and open/closed paths, even-odd/non-zero fill, union/subtract/intersect/XOR, initial fill, inversion and disabled masks. Vector/pixel mask coverage is combined on the layer grid; disabled pixel masks stay ineffective. Fill/stroke paths without raster channels are rasterized. Modern `lfx2`/`lmfx` and legacy `lrFX` convert native shadows, glows, overlay and stroke. Eight-bit grayscale was added for the real PSB fixture. |

Saving checks the destination fingerprint again after encoding and before replacing the package. The monitor accepts the fingerprint of the staged document, so an external write after saving remains detectable. This check detects concurrent changes; it is not an interprocess transaction with unrelated editors. External producers should still follow [the safe project-writing contract](../docs/writing-comp-files.md).

On Windows, monitoring package directories directly prevented atomic folder replacement. The implementation watches the parent and polls package metadata without holding directory handles open. Every three seconds it also checks referenced asset contents, covering equal-size/equal-timestamp replacements. Hashing/loading is asynchronous; large-project watcher and autosave throughput still needs hardware acceptance.

Recovery snapshots do not mark the source saved or alter it. Restarted recovery opens an untitled modified copy; Save asks for a destination. Sessions owned by other running instances are excluded. Source backups are not automatically deleted. The history budget limits retained undo data, not the document, render caches, active gestures or total process memory.

## Validation

Release build: Windows 11 x64, Visual Studio 2026, Qt 6.10.2, existing pinned LibRaw/ONNX Runtime/DirectML SDKs. Final CTest: **14/14 suites pass, 0 failures**, 104.59 seconds. The IO suite logs **17 passes, 0 failures, 1 opt-in skip**; the native Photoshop suite logs **24 passes, 0 failures**. The new IO suite covers external manifest/asset/folder writes, incomplete writes, dirty-tab refusal, saved fingerprints, recovery locks, actual killed-process recovery, startup recovery, autosave without marking clean, undo-to-clean/redo snapshot recreation, stale staging cleanup, recent files, memory eviction/sharing and WIC Unicode pixels/alpha/errors.

Photoshop regressions cover PSD and PSB, holes/inversion/disabled vector masks, vectors without raster channels, disabled pixel masks, all six native effect types, multi-effect descriptors, damaged data fallback and `.comp` save/reload. The Photoshop suite also passed with the native Windows Qt backend. Offscreen Qt does not load the Windows font database, so native real-file checks were used for the evidence below.

The native hidden-window smoke test created/opened a demo `.comp` and exited successfully. An offscreen screenshot confirmed completed canvas rendering; its UI fonts are unavailable in that backend, so it is not a typography acceptance image. Build dependencies are deployed beside the executable; there is no new ZIP or installer. Python delivery-tool syntax and Git whitespace checks also pass.

The opt-in real test loads each file, validates assets, renders it, saves/loads `.comp`, and verifies that the imported rendered result survives the round trip exactly. It records conversion warnings and writes `io-results.json` plus rendered PNGs. This round trip checks our representation; it does not by itself check Photoshop parity.

| Pinned source fixture | Native Windows result | RGBA reference MAE, 0–255 scale |
| --- | --- | --- |
| `vector-layer` PSD | Import/render/round trip pass, 200×200 | 0 |
| `vector-complex` PSD | Import/render/round trip pass, 200×200 | 0.13975 |
| `winding-even-odd` PSD | Import/render/round trip pass, 99×93 | 0.42698 |
| `winding-non-zero` PSD | Import/render/round trip pass, 99×93 | 0.32866 |
| `effects` PSD | Seven layers, four with converted native effects; round trip pass | 8.59961; **full appearance parity fails** |
| `psb-test` PSB | Eleven grayscale layers, one with converted effects; round trip pass | No meaningful composite reference; text font substituted with Tahoma |
| libheif `example.heic` | Native WIC decode pass, 1280×854 | Decode acceptance only; no color/orientation reference comparison |

Vector reference acceptance is mean RGBA error ≤1.0. The effects metric is reported without treating it as a passing Photoshop parity check. The PSB upstream `canvas.png` is a blank merged image and is deliberately not used to certify layered appearance. These are seven upstream files, not a complete camera/Photoshop corpus.

## Reproduce

Build with [the normal Windows build instructions](README.md#build-and-test). The opt-in fixture download uses immutable revisions and SHA256 checks, keeps the ag-psd license, and writes only under ignored artifacts by default:

```powershell
.\windows\fetch-io-fixtures.ps1
$env:COMPOSITOR_IO_FIXTURES = (Resolve-Path artifacts\io1-io5\fixtures).Path
$env:QT_QPA_PLATFORM = 'windows'
.\build\windows\Release\compositor_io_tests.exe realFormatFixtures -o 'artifacts/io1-io5/real-tests.txt,txt'
```

Deploy Qt's `qwindows` platform plugin beside the test executable for native checks. CMake deploys `qoffscreen` for ordinary CI. Real fixtures are skipped by default; enabling them requires downloading the files and installing the necessary Windows HEIC codec. A missing codec is recorded as `codec_missing`, rather than decode acceptance. Both a fresh fixture download and cached SHA256 verification passed locally.

Sources: [ag-psd test corpus](https://github.com/Agamnentzar/ag-psd/tree/387049670cb89b88fb8fe1b7c01aeacf98dd2e3b/test/read), [libheif sample](https://github.com/strukturag/libheif/blob/e981ebdf2b46a761820d41150ae8154d9dcc9e52/example.heic). The fixture script records all URLs/checksums; binaries are not committed. Local logs and renders are in the original checkout's ignored `artifacts/io1-io5/` directory, with a separate `build/io1-io5/` build to avoid the concurrent S2 task.

## Photoshop compatibility boundaries

Arbitrary paths become raster masks/pixels, rather than editable saved Bezier objects. Complex stroke styles and Photoshop-specific shape behavior are not fully preserved. The six native effect types retain enabled state, RGB/grayscale color, opacity, supported size/blur/distance/angle and inside/outside stroke parameters. Multiple instances retain the first. Centered stroke becomes outside stroke; effect blend modes become Normal, and spread/contours use native approximations with conversion warnings.

Gradient/pattern overlay, bevel/emboss and satin are unsupported. Separate Photoshop fill opacity, global-light behavior, complex contours, exact blur semantics and typography can differ. Smart objects retain pixels. Sixteen/32-bit, CMYK/Lab and ZIP-compressed Photoshop channels remain unsupported. **IO5 must not be marked as complete Adobe effect or pixel-parity acceptance while these limits remain.** The visible import conversion report identifies losses for each layer.

References used for the parser: [Adobe Photoshop file format](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/) and [Microsoft WIC HEIF codec](https://learn.microsoft.com/en-us/windows/win32/wic/heif-codec).

## Acceptance follow-up — 2026-10-05

Independent acceptance rebuilt `c49e249` in a separate worktree: 14 suites, 409 passes, 0 failures, 21 opt-in skips. The 13 pinned fixtures matched their SHA256 values, and the real-file run reproduced every reported figure (vector MAE 0 / 0.13975 / 0.42698 / 0.32866, effects 8.59961, PSB and HEIC passes). Fixes made on `io1-io5-fixes`:

| Finding | Change |
| --- | --- |
| Staging cleanup could delete the latest save. Saving unlocks the stage before renaming the project to `.compositor-backup-X` and the stage into place; interrupted between the renames, the stage is the only copy of the latest save, and cleanup removed it after 24 hours. | A stage is kept whenever a backup with the same suffix exists. Regression: `stagingCleanupKeepsInterruptedReplacement` (fails without the fix). |
| The monitor rehashed every asset every three seconds, even when nothing changed. | The content audit interval is 50 times the last hashing time, between 3 seconds and 2 minutes: unchanged for small projects, about 2% of the time for large ones. Size/time changes are still noticed within a second. |
| "Cannot fingerprint project asset" did not say why a save failed. | Oversized assets are named with the 512 MiB limit, which matches the existing load limit. |
| Photoshop path boolean operations were unbounded. | At most 1,000 shape operations per path; beyond that the vector mask is skipped with a conversion note. |
| The real-file check silently skipped a reference of the wrong size. | A present reference must load and match the render size. |

After the fixes: 14 suites, **413 passes, 0 failures, 21 opt-in skips** (IO 20 + 1 skip, Photoshop 25); the real-file run passes with the stricter reference check.

### Known issue, deferred

Rotated HEIC orientation is unverified. iPhone photos usually carry an `irot` rotation in the HEIF container as well as an EXIF orientation. If the Windows HEIF decoder already applies `irot`, the EXIF rotation applied afterwards turns such photos a second time; if it does not, the EXIF path may not be found. The pinned `example.heic` is not rotated. Verify with portrait iPhone photos (EXIF 6 and 8) before relying on HEIC orientation.

## Second acceptance fixes — 2026-10-05

Branch `fix/io-acceptance`, on top of `c49e249`, merged after the follow-up above. A second acceptance found these defects; each now has a regression test.

| Area | Defect | Fix and measurement |
| --- | --- | --- |
| IO3 | Every command push serialized both document snapshots of all 40 commands to estimate memory: about 110 ms per edit on a 400-layer document, selection changes included. | Each command's footprint is measured once with a metadata estimate; the shared-image union runs only when an upper bound exceeds the budget. 60 edits: 6.5 s → 0.54 s. An edit that alone exceeds the budget keeps one undo step. |
| IO1 | Opening hashed the whole project on the UI thread; Save hashed it before starting and again afterwards; idle projects were rehashed every 3 s (about 22% of a core on 157 MiB). | Baseline hashing runs in the background and changes made meanwhile are reported normally; the save job checks for external changes itself and reuses the saved fingerprint; the adaptive audit interval above is kept and metadata now includes creation times. 157 MiB: open 1.9 s → 1.2 s (load only), Save's longest UI stall about 2 s → 44 ms. |
| IO1 | A project damaged on disk (a referenced image deleted) made Ctrl+S fail with "Project asset is missing". | Missing, invalid and unreadable assets contribute markers to the fingerprint, so the damage is reported as an external change and Save can replace it after confirmation. |
| IO1/IO2 | Opening a project by a path differing only in letter case created a second tab watching and saving the same folder; recent files kept both spellings. | Project paths and recent files compare case-insensitively. |
| IO5 | A vector fill/stroke layer without raster data but with a pixel mask: the mask laid out on the canvas was stretched over the new path-sized grid. | The mask keeps its canvas placement. |
| IO5 (real file) | In the user's 5400×7200 poster PSD, the "get" text lost its last letter: the PostScript name `BodoniMT` was not matched to the installed `Bodoni MT` and Tahoma wrapped out of the frame. A line mixing two fonts kept only the first. Paragraph text with explicit leading sat 15 px low on every line (Qt places a fixed-height line's baseline at 0.8× the line height; Photoshop at the font's ascent). | PostScript names resolve to installed families; style runs become native font/color runs (only the size stays single); the layer is placed by the first baseline actually laid out. Against Photoshop's own composite: MAE 2.62 → 0.15, differing pixels 1.96% → 0.12%, every text line 0 px offset. |

For the deferred HEIC issue above, WIC-encoded files carrying a HEIF rotation were checked: WIC returns rotated pixels and orientation 1, so EXIF orientation is not applied twice. A rotated iPhone sample remains untested.

The pinned fixture results are unchanged for the vector and effects files. The delivered `psb-test` render in `artifacts/io1-io5` does not match what `c49e249` itself renders (MAE 11, rebuilt and rerun natively); the fixes change only its text region (0.12% of pixels).
