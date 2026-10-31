# Native score upgrade, 2026-10-31

Engrave supports native score formats from MuseScore 4.6 onward. These fixtures
were upgraded from their original committed bytes with the retained final A4
engine (`engrave-4`, tree `d8c21df149`), before using the trimmed engine.

The conversion exports a complete MSCZ. MSCZ inputs remain archives; MSCX inputs
embed the engine-produced master Style and excerpt Score/Style blocks. Current
readers support both inline styles and inline excerpts. This preserves the
information that the host's raw MSCX export omits. Values are produced by the
engine; no version-only substitution or music/style edits are used.

The adjacent [conversion ledger](native-format-upgrade.json) records each original
path, original SHA256, final path, final SHA256, excerpt count, writer binary SHA256
and source revision. Every final conversion starts from the original revision;
no intermediate conversion is used as the source. Original files remain in git.

Part tests retain their original assertions. Expected serialization outputs for
upgraded inputs were recaptured with the baseline engine; the preserved
part-visible-tracks excerpt uses MSCZ and its test reads that archive.
