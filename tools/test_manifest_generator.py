#!/usr/bin/env python3
"""Regression test suite for capture_xmake_manifest.py.

Verifies:
1. Positive test: generator against the trimmed engine tree reproduces
   xmake/manifest.lua byte-for-byte.
2. Negative control 1: a missing non-trimmed source fails.
3. Negative control 2: a trimmed compiled source fails (both when missing and present).
Controls are executed in an isolated temporary copy of the needed tree files,
never modifying the engine tree.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

# Import generator from the same directory
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import capture_xmake_manifest as cm


def test_positive(source_root: Path, manifest_path: Path) -> None:
    print("Testing positive generation against trimmed engine tree...")
    with tempfile.TemporaryDirectory() as tmp:
        out_path = Path(tmp) / "manifest.lua"
        ret = cm.main([
            "--source-root", str(source_root),
            "--output", str(out_path),
        ])
        assert ret == 0, f"Generator exited with code {ret}"
        gen_text = out_path.read_text(encoding="utf-8")
        ref_text = manifest_path.read_text(encoding="utf-8")
        assert gen_text == ref_text, "Generated manifest differs from checked-in manifest"
    print("  [PASS] Positive test: generated manifest matches xmake/manifest.lua byte-for-byte")


def _collect_tree_inputs(source_root: Path) -> tuple[list[str], list[str]]:
    """Sources and CMake files the real engine tree makes the generator read."""
    all_inputs: list[str] = []
    orig = cm.Interpreter._cmd_target_sources

    def tracked(interp: cm.Interpreter, raw: str) -> None:
        args = interp._arg_list(raw)
        for item in args[1:]:
            if item.upper() in ("PRIVATE", "PUBLIC", "INTERFACE") or item.startswith("$<"):
                continue
            p = Path(item)
            if not p.is_absolute():
                p = (interp._current_source_dir / p).resolve()
            if p.exists():
                all_inputs.append(p.relative_to(source_root).as_posix())
        return orig(interp, raw)

    cm.Interpreter._cmd_target_sources = tracked
    try:
        manifest = cm.build_manifest(source_root, source_root, False)
    finally:
        cm.Interpreter._cmd_target_sources = orig
    return all_inputs, list(manifest["_cmake_files_read"])


def _populate_temp_engine_tree(source_root: Path, temp_root: Path, file_inputs: list[str],
                               cmake_files: list[str]) -> None:
    for rel in cmake_files + ["version.cmake", "TRIM"]:
        src = source_root / rel
        if src.is_file():
            dst = temp_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    for rel in file_inputs:
        dst = temp_root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.touch()

    for entries in cm.EXTRA_SOURCES.values():
        for extra, _ in entries:
            dst = temp_root / extra
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.touch()

    for inc in (
        "src/engraving/thirdparty/dtl",
        "src",
        "muse/framework",
        "muse/framework/global",
        "src/engraving",
        "muse/framework/draw",
        "muse/framework/audio",
    ):
        (temp_root / inc).mkdir(parents=True, exist_ok=True)


def test_negative_controls(source_root: Path, file_inputs: list[str], cmake_files: list[str]) -> None:
    print("Testing negative control 1: missing non-trimmed source fails...")
    with tempfile.TemporaryDirectory() as tmp:
        troot = Path(tmp) / "engine"
        _populate_temp_engine_tree(source_root, troot, file_inputs, cmake_files)

        # Remove a non-trimmed compiled source
        victim = troot / "src/engraving/editing/edittuplet.cpp"
        assert victim.is_file(), "edittuplet.cpp should exist in temp tree"
        victim.unlink()

        try:
            cm.build_manifest(troot, troot, False)
            assert False, "build_manifest should fail when a non-trimmed source is missing"
        except cm.CmakeError as err:
            assert "missing file src/engraving/editing/edittuplet.cpp" in str(err)
            print(f"  [PASS] Missing non-trimmed source correctly rejected: {err}")

    print("Testing negative control 2: trimmed compiled source fails...")
    # Case A: trimmed compiled source is missing
    with tempfile.TemporaryDirectory() as tmp:
        troot = Path(tmp) / "engine"
        _populate_temp_engine_tree(source_root, troot, file_inputs, cmake_files)
        victim = troot / "src/engraving/editing/edittuplet.cpp"
        victim.unlink()
        with open(troot / "TRIM", "a", encoding="utf-8") as f:
            f.write("\nsrc/engraving/editing/edittuplet.cpp\n")

        try:
            cm.build_manifest(troot, troot, False)
            assert False, "build_manifest should fail when a compiled source is trimmed and missing"
        except cm.CmakeError as err:
            assert "compiled source is covered by TRIM" in str(err) and "edittuplet.cpp" in str(err)
            print(f"  [PASS] Missing trimmed compiled source correctly rejected: {err}")

    # Case B: trimmed compiled source exists on disk
    with tempfile.TemporaryDirectory() as tmp:
        troot = Path(tmp) / "engine"
        _populate_temp_engine_tree(source_root, troot, file_inputs, cmake_files)
        with open(troot / "TRIM", "a", encoding="utf-8") as f:
            f.write("\nsrc/engraving/editing/edittuplet.cpp\n")

        try:
            cm.build_manifest(troot, troot, False)
            assert False, "build_manifest should fail when an existing compiled source is covered by TRIM"
        except cm.CmakeError as err:
            assert "compiled source is covered by TRIM" in str(err) and "edittuplet.cpp" in str(err)
            print(f"  [PASS] Present trimmed compiled source correctly rejected: {err}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT,
                        help="path to the trimmed engine tree (default: this repository)")
    parser.add_argument("--manifest", type=Path, default=ROOT / "xmake/manifest.lua",
                        help="path to checked-in manifest.lua")
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    manifest_path = args.manifest.resolve()

    if not source_root.is_dir():
        print(f"error: source root not found: {source_root}", file=sys.stderr)
        return 2

    test_positive(source_root, manifest_path)
    file_inputs, cmake_files = _collect_tree_inputs(source_root)
    test_negative_controls(source_root, file_inputs, cmake_files)
    print("All manifest generator regression checks passed successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
