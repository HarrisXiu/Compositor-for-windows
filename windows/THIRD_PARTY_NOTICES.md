# Third-party notices

Compositor Windows is based on [robbietilton/Compositor](https://github.com/robbietilton/Compositor) at commit `11d8d7a50992b24fd9a760a1c13b1c01b70aaf30`. The original C image-processing code is included under the repository's MIT license; the portable package includes `LICENSE-Compositor.txt`.

This build dynamically links the unmodified open-source Qt 6.10.2 Core, Gui, Widgets, Svg and Concurrent libraries. Qt Concurrent supports row-parallel compositing and asynchronous work; its DLL is included. It deploys these Qt plugins: `platforms/qwindows`, `styles/qmodernwindowsstyle`, `iconengines/qsvgicon` and the `imageformats` plugins (GIF, ICNS, ICO, JPEG, SVG, TGA, TIFF, WBMP, WebP). Qt Network and its plugins are not deployed. Qt libraries are distributed under the GNU Lesser General Public License version 3. The package includes:

- `licenses/LGPL-3.0-only.txt` and `licenses/GPL-3.0-only.txt` (LGPLv3 incorporates the GPLv3 text).
- `licenses/Qt/<module>/`: every license text referenced by the deployed Qt modules and the third-party code inside them, taken from Qt's own source archives.
- `licenses/Qt-third-party-components.txt`: a readable list of that third-party code (for example zlib, libpng, libjpeg, HarfBuzz, FreeType, PCRE2, libtiff and libwebp) with versions, licenses and copyright notices, generated from Qt's SBOM.
- `sbom/`: Qt's machine-readable SPDX and CycloneDX descriptions.

Qt's corresponding source is published with every release, beside the portable ZIP, as the exact archives Qt distributes, with their SHA256 sums in `SHA256SUMS.txt`:

- `qtbase-everywhere-src-6.10.2.tar.xz`
- `qtsvg-everywhere-src-6.10.2.tar.xz`
- `qtimageformats-everywhere-src-6.10.2.tar.xz`

The same archives are available from Qt at https://download.qt.io/archive/qt/6.10/6.10.2/submodules/. Qt licensing: https://www.qt.io/licensing/open-source-lgpl-obligations. This fork's source and build scripts: https://github.com/HarrisXiu/Compositor-for-windows.

Qt DLLs are replaceable and are kept separate from the executable. No additional restriction on replacing Qt libraries or debugging those replacements is imposed by this application.

Qt Core uses Windows' own ICU library from the system directory (Windows 10 version 1703 and later); the package does not redistribute it.

The portable package includes Microsoft components distributed under their own redistribution terms, not under the Compositor MIT license: the Visual C++ runtime DLLs, and `D3Dcompiler_47.dll` from the Windows SDK, which Qt's deployment tool adds for Direct3D shader compilation.

RAW import dynamically links the unmodified LibRaw 0.22.2 Windows SDK under its CDDL 1.0 license option. LibRaw is Copyright (C) 2008–2025 LibRaw LLC and includes the additional attributions in its `COPYRIGHT` file. The package includes `licenses/LibRaw-CDDL-1.0.txt`, `licenses/LibRaw-COPYRIGHT.txt`, and the complete original binary/source SDK archive in `third-party-source/LibRaw-0.22.2-Win64.zip`. Extract that archive to obtain the library's corresponding source, build files, and embedded third-party notices. The DLL is separate and replaceable.

- LibRaw release/source archive: https://www.libraw.org/data/LibRaw-0.22.2-Win64.zip
- Pinned archive SHA256: `AC64FA12BB00A7581332D4C6AB918C0533FB3F119D6B668D47A6875410DCA948`.

Compositor's own Windows source continues to use MIT. Third-party library licenses apply to those libraries independently.
