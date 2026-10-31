# Installed runtime assets

From a Git checkout, run `python tools/install_assets.py /absolute/install/share/engrave`.
The destination must be new. Pass it as `EngineOptions::resource_dir` (or the gate's
`--assets`). Run the same command with `--verify` to check installed bytes against
this engine's manifest. Python and Git are packaging tools, not runtime dependencies.

`engrave/assets.json` is versioned with the engine. It pins each installed path's
size and SHA256, including templates, instruments, profiles, fonts and licenses.
Installation reads committed blobs so checkout newline conversion cannot change
resource bytes. Every input is verified before the staged directory is published.
The manifest is installed inside that directory; no second provenance file or QRC
regeneration is needed by this installer. Existing `:/` names remain relative
resource identifiers for internal readers. External font and SoundFont overrides
remain separate and retain their existing lifetime/cache behavior.

When changing an asset, update its manifest digest and size deliberately, validate
installed bytes and run both public engine gates. Never regenerate hashes from an
unverified installed directory. Runtime loading currently preserves the historical
resource-directory contract; `--verify` is the packaging integrity check.
