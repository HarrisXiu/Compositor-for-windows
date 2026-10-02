# Compositor for Windows — C/C++ preview

**当前 Windows 版尚未完全完成，后续将持续更新。** This is Windows migration preview 0.4, built with C++20 and Qt 6 Widgets. Eight original C pixel-processing source files are compiled directly from `Compositor/Rendering`; the original Dither kernel is ported to C++ with Windows parallel execution. There is no Rust code or runtime dependency on Python. The original macOS project is retained. Application and port source remain under the root MIT license; third-party libraries retain their own licenses.

## Run

Extract **the entire** `Compositor-Windows-x64.zip` archive and run `Compositor.exe` in that folder. Keep the DLLs and plugin folders beside the executable. Target: Windows 10/11 x64. This portable preview does not install file associations. Automatic updating is deliberately excluded from the Windows port: there is no update checker, download service or updater in the Windows build.

Use **Help > Open Demo** to try an editable layered document immediately, or import your own images with `Ctrl+I`.

Choose **View > Language** (**视图 > 语言** / **表示 > 言語**) for **简体中文**, **English**, or **日本語**. Menus, tools and editing dialogs update immediately; the choice is stored in the user profile for the next launch. The first launch follows the system language when supported. Document filenames, layer names, text content and project parameter keys are preserved. Native Windows file dialogs follow Windows language settings; some decoder diagnostics and Photoshop conversion details remain English.

## Build and test

Install Visual Studio 2022 or 2026 with Desktop development with C++, a Windows SDK, CMake, and the Qt 6.8+ MSVC x64 development package. The verified build uses Qt 6.10.2 and Visual Studio 2026. For TIFF support, include Qt Image Formats; for SVG, include Qt SVG.

From PowerShell at the repository root:

```powershell
.\windows\build.ps1 -QtRoot C:\Qt\6.10.2\msvc2022_64 -Package
```

The script fetches the SHA256-pinned LibRaw 0.22.2 Windows SDK once, configures CMake, compiles C/C++, runs five test suites, deploys the Qt/LibRaw DLLs and plugins, and creates `artifacts/Compositor-Windows-x64.zip`. Packaging also fetches the SHA256-pinned Qt 6.10.2 source archives once (`windows/fetch-qt-source.ps1`, about 55 MB), copies their license texts into the package, and leaves the archives with `SHA256SUMS.txt` in `artifacts/qt-source/`.

When publishing a release, attach everything in `artifacts/qt-source/` beside the ZIP: Qt is LGPLv3, so its source must be available wherever the binaries are. On a tag, CI uploads them as the `Qt-6.10.2-source` artifact. If Qt is already in the local `.cache/Qt` directory, `-QtRoot` is optional. Subsequent builds can use the cached SDK offline. For direct CMake builds, set `COMPOSITOR_LIBRAW_ROOT` to the extracted SDK directory. Use an x64 Visual Studio CMake generator; projects configured with Ninja require a separate developer-shell configuration.

## Implemented in this preview

- Multiple project tabs; new, open, import, background save, and PNG/JPEG export with resolution metadata.
- `.comp` folder structure and versions 1–11; unknown manifest and layer fields are preserved on save. Image paths, layer UUIDs, hierarchy cycles, clipping cycles, masks and file-size limits are validated.
- Layers, folders, drag reordering and nesting, renaming, duplication, visibility, opacity, all 24 blend modes, raster/folder masks and clipping masks.
- Move, exact position/size/rotation, flips and non-destructive raster transforms.
- Brush and eraser with size, hardness, opacity, source-resolution painting, selection limits and mask painting; clone stamp and the original C content-aware spot-healing implementation.
- Rectangle/ellipse/lasso selections, magic wand, selection addition/subtraction, invert and feather.
- Gradient; editable rectangle/ellipse/line shapes; editable text with font, size, alignment, tracking, leading, paragraph bounds, and UTF-16 color/font runs; eyedropper, pan, zoom and fit. Select a layer and use **Layer > Edit Text / Edit Shape**. Existing PNGs remain the saved appearance until edited; Windows font substitution can change edited text when a Mac font is unavailable.
- Undo/redo for document and selection edits, including feather and selection restoration on crop/resize. Selection-only changes do not count as unsaved image content. Session colors and edit target are independent per project; X swaps foreground/background, D resets colors, Escape cancels a gesture and Space temporarily pans. Fill/clear, crop, canvas size and copy merged/paste image are available.
- Invert, Exposure, per-channel Levels/Curves, per-color-range Hue/Saturation and Colorize, Black & White, Gradient Map, Color Balance, Gaussian/Motion Blur, Add Noise, Grain, Lens Correction, basic Camera Raw exposure and the original C Content-Aware Fill.
- Create/edit all 12 adjustment kinds through **Layer > New Adjustment Layer / Edit Adjustment**. Folder masks/opacity and adjustment blend modes are applied; contiguous clipping stacks preserve the base's alpha. Filters and effects have modal previews and commit one undo entry.
- All six layer effects: Stroke, Drop Shadow, Color Overlay, Inner Shadow, Outer Glow and Inner Glow. Effects follow the layer's masked source shape and transforms, with a bounded 64 MiB result cache. Use **Layer > Layer Effects** to edit, disable or remove them.
- Photoshop PSD/PSB 8-bit RGB import with raw/RLE channels, folders, blend modes, opacity, raster masks, clipping, ICC conversion, resolution, Levels/Curves/Hue-Saturation/Invert adjustment conversion, editable horizontal type and `vogk` rectangle/rounded-rectangle/ellipse shapes. Text retains the first style, tracking, leading, alignment and supported paragraph bounds; vertical/sheared/unevenly scaled type stays raster. Windows fonts can change imported editable type. Smart objects stay raster; unsupported effects/adjustments and conversions are listed under **Help > Import Conversion Report**. PSD import opens a new document; Save writes a `.comp` project.
- Camera RAW development through LibRaw with Unicode paths, camera orientation, exposure, temperature/tint, Boost, camera-white-balance Reset, a coalesced/cancellable preview and full-resolution import. Use **File > Develop RAW Photo**, open a RAW file or drop it onto the canvas. The temperature slider is relative to an estimated 5000 K baseline; Reset retains the actual camera multipliers. The portable tone curve differs from Apple's RAW rendering. Camera-format coverage follows the bundled LibRaw decoder; no Windows Store codec is required for RAW.
- All eleven Dither styles: Atkinson, Floyd–Steinberg, Bayer 2/4/8, halftone dots/lines/diamonds, Mac Patterns, ASCII and CRT Scanlines. Controls include palettes, chunky pixels/dots, density/contrast, glyphs, glow, beads and wobble. ASCII uses Windows monospace fonts.
- Blur brush, Smudge and Liquify, with source-resolution commit, document-space stroke movement, selection limits and one undo step. Blur also works on raster/folder masks. The Blur radius is measured in canvas pixels; very small layer scales currently cap source-space blur at 250 px.
- Standalone Vignette, Bloom / Glow and Tonal Contrast. Vignette can paint an empty layer. Camera Raw filter has pages for Light/Color, parametric and per-channel point curves, eight-family Color Mixer, up to eight Point Colors, four grading wheels, Effects, Detail, Optics, Geometry, Calibration and scopes. It includes asynchronous Auto white balance, white-balance/defringe/point-color sampling, guided perspective correction, Histogram/Vectorscope, clipping indicators and sharpening-mask preview. Indicators are preview-only; applying commits one undo step. C pixel kernels and the upstream curve/geometry formulas are retained; Qt performs projective raster sampling.
- PNG/JPEG/BMP/WebP import; SVG/TIFF depend on their deployed Qt image plugins. Imported EXIF orientation is applied and tagged images are converted to sRGB.

## Differences and work remaining

**This preview does not yet satisfy full feature parity with the Mac app.** It uses a CPU renderer; Direct3D acceleration, tiled rendering, Camera Raw targeted slider gestures and RGB hover readouts, full canvas filter previews and color-band editors, mesh distort, advanced text/shape interactions, selection transforms, snapping/rulers/grid editing, complete vector conversion, HEIC, AI subject/object selection and background removal, installer/shell integration remain to be migrated. Automatic updating is excluded from the target. See [the migration checklist](PORTING_STATUS.md).

Unknown future layer effects are retained but omitted from the preview; a visible warning lists them and flattening/export/copy merged is blocked while present. Supported effects and adjustments now render. Hue/Saturation uses the original 33³ color-cube algorithm and C interpolation; Curves uses the original PCHIP algorithm and C lookup. Gaussian/Motion Blur, standalone Bloom and font rasterization use Windows implementations; Mac reference comparisons are still required to establish visual parity. Preview noise/grain coordinates and spatial filter extents still need matching against the original. PSD compression/depth support matches the upstream reader, but real Photoshop interoperability needs broader fixtures beyond the independent test files.

Saving stages a complete sibling package, renames the existing project to a backup, installs the staged folder, then removes the backup. If replacement fails, it attempts to restore the original; if restoration also fails, the error identifies the preserved backup. These two Windows directory renames are **not a single atomic operation**; a power failure between them can leave `.compositor-stage-*` and `.compositor-backup-*` folders beside the project. Keep these folders for recovery. Third-party package files (e.g. Quick Look previews) are not regenerated in this preview.

History currently retains up to 40 copy-on-write document snapshots; a dedicated history memory budget is still needed for very large projects. Full-resolution filtering/export can block the UI; saving uses an immutable worker snapshot. Long-side previews are bounded to 1,600 pixels. No macOS build was run on this Windows machine.

## Shortcuts

`Ctrl+N/O/S`, `Ctrl+I` import, `Ctrl+J` duplicate, `Ctrl+Z` undo, `Ctrl+Y` redo, `Ctrl+A/D` select/deselect, `Shift+F5` fill, `Delete` clear pixels, `Ctrl+0/1` fit/actual pixels. Tools: `V/B/E/M/L/W/G/U/T/I/S/J/H`. Alt-click sets a clone source; Shift adds a marquee/lasso selection; Alt subtracts. Use the **Paint mask** checkbox to target an existing mask.

## Layout

- `windows/src/document.*`: JSON model, validation and safe project IO.
- `windows/src/render.*`: CPU compositing, placement, masks and blend modes.
- `windows/src/filters.*`: filter adapters around original C algorithms.
- `windows/src/canvas.*`: canvas and pointer interactions.
- `windows/src/editor.*`: tabs, undo, panels, menus and asynchronous saving.
- `windows/tests`: format/rendering regression tests and Qt mouse-interaction tests.

Original Compositor and the Windows port are MIT licensed. Qt is dynamically linked under LGPLv3; LibRaw uses its CDDL 1.0 option, with the matching original SDK/source archive included in the package. See `THIRD_PARTY_NOTICES.md` and `licenses/`. The source for this port and its CMake/build scripts remains in the fork.
