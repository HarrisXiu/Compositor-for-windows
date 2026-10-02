# Third-party notices

Compositor Windows is based on [robbietilton/Compositor](https://github.com/robbietilton/Compositor) at commit `11d8d7a50992b24fd9a760a1c13b1c01b70aaf30`. The original C image-processing code is included under the repository's MIT license; the portable package includes `LICENSE-Compositor.txt`.

This build dynamically links the open-source Qt 6 Core, Gui, Widgets, Concurrent and Svg libraries and deploys their platform/image plugins. Qt libraries are distributed under the GNU Lesser General Public License version 3. The package includes the LGPLv3 and GPLv3 texts in `licenses/`; copyright, license and dependency information is also embedded in Qt's deployed `sbom` descriptions when provided by the Qt installation.

- Qt source: https://code.qt.io/cgit/qt/qtbase.git/ and https://code.qt.io/cgit/qt/qtsvg.git/ (tag matching `BUILD-INFO.txt`).
- Qt Image Formats source, if installed: https://code.qt.io/cgit/qt/qtimageformats.git/.
- Qt licensing: https://www.qt.io/licensing/open-source-lgpl-obligations.
- This fork's source and build scripts: https://github.com/HarrisXiu/Compositor-for-windows.

Qt DLLs are replaceable and are kept separate from the executable. No additional restriction on replacing Qt libraries or debugging those replacements is imposed by this application.

The portable package may include Microsoft's Visual C++ runtime components copied by Qt's deployment tool. These are Microsoft components distributed under their accompanying runtime redistribution terms, not under the Compositor MIT license.

RAW import dynamically links the unmodified LibRaw 0.22.2 Windows SDK under its CDDL 1.0 license option. LibRaw is Copyright (C) 2008–2025 LibRaw LLC and includes the additional attributions in its `COPYRIGHT` file. The package includes `licenses/LibRaw-CDDL-1.0.txt`, `licenses/LibRaw-COPYRIGHT.txt`, and the complete original binary/source SDK archive in `third-party-source/LibRaw-0.22.2-Win64.zip`. Extract that archive to obtain the library's corresponding source, build files, and embedded third-party notices. The DLL is separate and replaceable.

- LibRaw release/source archive: https://www.libraw.org/data/LibRaw-0.22.2-Win64.zip
- Pinned archive SHA256: `AC64FA12BB00A7581332D4C6AB918C0533FB3F119D6B668D47A6875410DCA948`.

Compositor's own Windows source continues to use MIT. Third-party library licenses apply to those libraries independently.
