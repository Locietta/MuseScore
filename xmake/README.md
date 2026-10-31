# Native engine build

Configure with `xmake f -m release --cairo_prefix=PREFIX`, then `xmake build`.
The ordinary build contains only production libraries. Audio, all current importers,
and every encoder including AAC are always enabled. MNX sources are retained but
not yet compiled.

Tests and their GoogleTest dependency are opt-in. Add `--engrave_tests=y` to the
complete configure command, then run `xmake build c0_gate`. Individual test targets
remain explicit even in a test-enabled build. Gate runners must pass that option
when configuring this checkout.

`framework_config.h` supplies stable offline audio features to the generated
`muse_framework_config.h`. Version and Git metadata are generated separately into
`engrave_version.h`, included only by `muse/framework/global/appinfo.cpp`, which
also backs the version accessor used by the public capabilities API.

The remaining Q_OS_WIN definition serves audio headers owned by C4. Non-audio
sources use compiler platform definitions directly.
