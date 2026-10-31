# Native engine build

Configure with `xmake f -P . -y -m release --cairo_prefix=PREFIX`, then
`xmake build -P . -y -j8`. The ordinary build contains eleven production libraries.
Native score, MusicXML/MXL, MIDI, Guitar Pro/PowerTab and MNX imports are compiled;
all audio encoders, including AAC, are always enabled. The MNX exporter remains
uncompiled. Native score saves omit thumbnails and serialize excerpts sequentially.

Tests and their GoogleTest dependency are opt-in. Add `--engrave_tests=y` to the
complete configure command, then run `xmake build -P . -y -j8 c0_gate`. This builds
the supported unit-test closure, including `diagnostics_test` and `io_boundary_test`,
the public API embedder/header-leak check and output probes. It does not execute
them. Individual tests and optional development probes remain explicit even in a
test-enabled build. Gate runners must pass that option when configuring this checkout.

`muse_generated` writes only `engrave_version.h`, included by
`muse/framework/global/appinfo.cpp`, which backs the public capabilities version
accessor. There is no generated framework feature header. Native platform guards
use compiler definitions such as `_WIN32`; no `Q_OS_WIN` alias or `kors_profiler`
dependency remains. Diagnostics use the small logger sink and optional
`ENGRAVE_PROFILE` scoped timers.

Install runtime assets from this checkout with `python tools/install_assets.py DIR`;
check the complete installed directory with `python tools/install_assets.py DIR --verify`.
The hash-pinned manifest replaces QRC staging lists. See `engrave/ASSETS.md` for the
directory contract and `python tools/test_install_assets.py` for packaging checks.
Fonts and SoundFonts supplied by consumers remain external; no assets are embedded
in the library. The hard fork accepts selected upstream fixes by cherry-pick;
`TRIM` records historical deletions, and upstream sync tooling is retired.
