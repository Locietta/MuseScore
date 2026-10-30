#!/usr/bin/env python3
"""Generate ``xmake/manifest.lua`` from the engine's own CMake source lists.

Why this exists
---------------
``xmake/manifest.lua`` is the checked-in source graph the xmake build compiles;
xmake itself never runs CMake.  The original capture read a
``compile_commands.json`` from our own CMake/Ninja baseline, which only existed
for the MuseScore 4.7.6 engine port.  MuseScore 5.0 (this fork, framework
absorbed at ``muse/framework``) has no such baseline, and the build needs the
*engine's* opinion about which files belong to a module, not a file-by-file
guess.

So this script reads the upstream ``target_sources`` lists -
``muse/framework/<module>/CMakeLists.txt``, ``src/<area>/CMakeLists.txt`` and the
``*.cmake`` source lists they include - through a deliberately small CMake
interpreter, and writes the manifest.  The engine tree is only read, never
written.  ``python tools/capture_xmake_manifest.py`` with no arguments
regenerates ``xmake/manifest.lua`` from the file set described below.

The manifest describes the engine targets only: the module libraries
(``muse_global`` ... ``muse_audio_engine``) plus ``noqt_support``, the Qt-free
adapters' engine-side sources (font faces, MIDI export, MusicXML configuration)
that a host compiles into its own adapter target.  Everything the host adds on
top of that - its own sources, include directories and generated files - is the
host project's business, not the engine's.

What the interpreter supports
-----------------------------
``set``/``list``/``include``/``add_subdirectory``/``add_compile_definitions``,
``target_sources``, ``target_compile_definitions``, ``target_include_directories``,
``muse_create_module``/``muse_create_thirdparty_module``, function/macro
definition skipping, and ``if``/``elseif``/``else``/``endif`` with
``NOT``/``AND``/``OR``/parentheses, ``STREQUAL``, ``MATCHES``, ``EQUAL``,
``VERSION_(GREATER|LESS|EQUAL)``, ``DEFINED``, ``EXISTS``, ``TARGET`` and
``COMMAND``.  ``COMMAND x`` is always false (we do not use the upstream
dependency-build helpers); ``message(FATAL_ERROR ...)`` aborts the generator.
Anything else is ignored and reported with ``--verbose``; a ``file(GLOB)`` or a
generator expression in a source list is a hard error, because silently
dropping sources is exactly the failure mode this script must not have.

The feature selection (``SELECTION`` below) is the project's, not upstream's:
no Qt, no UI/QML, no unit tests, no engraving devtools, FreeType font faces with
the upstream text-drawing path, `GLOBAL_NO_INTERNAL`, `ENGRAVING_NO_INTERNAL`
(which implies `ENGRAVING_NO_INTERACTIVE`/`_API`), `MUSICXML_NO_INTERNAL`.
Changing a switch here is the only supported way to change what the manifest
compiles.

Target routing
--------------
Upstream splits some modules into more targets than we want (``muse_audio`` ->
``muse_audio_common``/``main``/``engine``/``driver``) and keeps Qt-only files in
the same module as Qt-free ones.  ``route()`` below maps each upstream target's
sources onto our targets and records, for every dropped file, why it was
dropped.  The drops are printed by ``--verbose`` and are part of the review
surface.

Placeholders
------------
``${SOURCE_ROOT}`` is this checkout (``--source-root``, default: the directory
containing this script's parent).  No absolute paths are written; every
placeholder path is verified to exist.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Feature selection - the project's build switches, as CMake variables.
# ---------------------------------------------------------------------------

PLATFORM = {
    "OS_IS_WIN": True,
    "OS_IS_MAC": False,
    "OS_IS_LINUX": False,
    "OS_IS_WASM": False,
    "ARCH_IS_X86_64": True,
    "ARCH_IS_AARCH64": False,
    "MSVC": True,
}

SELECTION = {
    **PLATFORM,
    # No Qt anywhere in the build.
    "MUSE_QT_SUPPORT": False,
    # Single-threaded engine, as in the 4.7.6 baseline (deliberately undefined).
    "MUSE_THREADS_SUPPORT": False,
    "MUSE_ENABLE_UNIT_TESTS": False,
    "MUSE_ENABLE_UNIT_TESTS_CODE_COVERAGE": False,
    "MUSE_ENABLE_CUSTOM_ALLOCATOR": False,
    # global
    "GLOBAL_NO_INTERNAL": True,
    "MUSE_MODULE_GLOBAL_LOGGER_DEBUGLEVEL": True,
    "MUSE_MODULE_GLOBAL_MULTI_IOC": False,
    # draw: internal/ (font face + provider) is compiled into noqt_support so the
    # Qt-free implementations stay isolated from the module target.
    "DRAW_NO_INTERNAL": False,
    "DRAW_NO_QSVGRENDER": True,
    "MUSE_MODULE_DRAW_USE_FONTFACE_FT": True,
    "MUSE_MODULE_DRAW_USE_FONTFACE_XT": False,
    "MUSE_MODULE_DRAW_USE_QTTEXTDRAW": True,
    "MUSE_MODULE_DRAW_TRACE": False,
    "MUSE_MODULE_DRAW_TESTS": False,
    "MUSE_MODULE_DRAW_USE_QTFONTMETRICS": False,
    # mpe
    "MUSE_MODULE_MPE_TESTS": False,
    "MUSE_MODULE_MPE_QML": False,
    # engraving
    "ENGRAVING_NO_ACCESSIBILITY": True,
    "ENGRAVING_NO_INTERNAL": False,
    "MUE_BUILD_ENGRAVING_PLAYBACK": True,
    "MUE_BUILD_ENGRAVING_DEVTOOLS": False,
    "MUE_BUILD_ENGRAVING_TESTS": False,
    "MUE_BUILD_ENGRAVING_QML": False,
    "MUE_ENABLE_ENGRAVING_RENDER_DEBUG": False,
    "MUE_ENABLE_ENGRAVING_LD_ACCESS": False,
    "MUE_ENABLE_ENGRAVING_LD_PASSES": False,
    "MSVC_IDE": False,
    # import/export
    "MUSICXML_NO_INTERNAL": True,
    "MUE_BUILD_IMPORTEXPORT_TESTS": False,
    # audio engine (no web facade, no driver, encoders enabled)
    "WEBENGINE_FACADE_MODE": False,
    "MUSE_MODULE_AUDIO": True,
    "MUSE_MODULE_AUDIO_EXPORT": True,
    "MUSE_MODULE_AUDIO_TESTS": False,
}

# Additional variables that upstream's top-level CMakeLists sets project-wide.
GLOBAL_DEFINES = ["KORS_PROFILER_ENABLED"]

# Defines every translation unit of the engine build gets (and that a host
# compiling engine headers must repeat), because they change class
# layouts/interfaces.  They are this build's selection, not upstream's: the
# xmake build applies this list to every engine target, so there is one source
# of truth.  ``MUSE_THREADS_SUPPORT`` is deliberately absent (single-threaded
# engine, as in the 4.7.6 baseline), and so is ``MUE_BUILD_ENGRAVING_PLAYBACK``
# (5.0 gates the playback *sources* on it in CMake but no longer reads the
# macro).
PROJECT_DEFINES = [
    "NO_QT_SUPPORT",
    "ENGRAVING_NO_ACCESSIBILITY",
    "ENGRAVING_NO_INTERACTIVE",
    "ENGRAVING_NO_API",
    "MUSICXML_NO_INTERNAL",
    "MUSICXML_NO_INTERACTIVE",
    "MUSICXML_NO_VALIDATION",
    "MUSE_MODULE_AUDIO_EXPORT",
    "MUSE_MODULE_DRAW_USE_FONTFACE_FT",
    "MUSE_MODULE_DRAW_USE_QTTEXTDRAW",
]

# ---------------------------------------------------------------------------
# What to compile: upstream entry points and their manifest target mapping.
# ---------------------------------------------------------------------------

# upstream CMakeLists -> upstream target names it creates
ENTRY_POINTS = [
    "muse/framework/global/CMakeLists.txt",
    "muse/framework/draw/CMakeLists.txt",
    "muse/framework/mpe/CMakeLists.txt",
    "muse/framework/audio/common/CMakeLists.txt",
    "muse/framework/audio/engine/CMakeLists.txt",
    "muse/framework/audio/thirdparty/fluidsynth/CMakeLists.txt",
    "src/engraving/CMakeLists.txt",
    "src/importexport/musicxml/CMakeLists.txt",
    "src/importexport/midi/CMakeLists.txt",
    "src/importexport/guitarpro/CMakeLists.txt",
]

# add_subdirectory() calls we do not follow: the fluidsynth wrapper is read
# directly (ENTRY_POINTS) because its own CMakeLists probes every audio driver.
SKIP_SUBDIRS = {"muse/framework/audio/thirdparty/fluidsynth"}

SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".s", ".asm"}

# Files dropped from an upstream target, with the reason.  Every entry must be
# matched by an actual upstream source, else the generator fails (so this list
# cannot silently rot).
DROPPED = {
    # Guitar Pro: module/UI glue.  guitarproreader.h needs the `project` module
    # (Qt/QML notation layer), which this build does not have.
    "src/importexport/guitarpro/guitarpromodule.cpp": "module glue (project/UI module)",
    "src/importexport/guitarpro/internal/guitarproconfiguration.cpp": "used only by guitarproreader",
    "src/importexport/guitarpro/internal/guitarproreader.cpp": "needs project/INotationReader (UI module)",
    # MIDI: module + reader/writer glue; notationmidireader needs the `project`
    # module and Qt::Core5Compat (see src/importexport/midi/CMakeLists.txt:47).
    "src/importexport/midi/midimodule.cpp": "module glue (project/UI module)",
    "src/importexport/midi/internal/midiconfiguration.cpp": "settings-backed, only used by module glue",
    "src/importexport/midi/internal/notationmidireader.cpp": "needs project module (+ Qt::Core5Compat)",
    "src/importexport/midi/internal/notationmidiwriter.cpp": "needs project module",
    # MIDI import: 5.0 added a Qt table model for the import panel
    # (importmidi_model.h:28 #include <QAbstractTableModel>).  Not used by the
    # Qt-free import path.
    "src/importexport/midi/internal/midiimport/importmidi_model.cpp": "Qt table model (UI only)",
    # The engine's own audio worker thread.  The headless host drives the audio
    # engine from its own thread and keeps MUSE_THREADS_SUPPORT undefined; the
    # worker pulls in the thread-pool/rpc machinery none of our targets need.
    "muse/framework/audio/engine/platform/general/generalaudioworker.cpp":
        "audio worker thread (host drives the engine; MUSE_THREADS_SUPPORT off)",
    # Engraving: Qt-only mime adapter and plugin API v1 (ENGRAVING_NO_API).
    "src/engraving/internal/qmimedataadapter.cpp": "Qt mime data adapter (UI only)",
    "src/engraving/api/v1/apistructs.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/cursor.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/elements.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/engravingapiv1.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/enums.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/excerpt.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/instrument.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/part.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/playevent.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/qmlpluginapi.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/score.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/scoreelement.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/selection.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/style.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
    "src/engraving/api/v1/util.cpp": "plugin API (QML/UI layer, ENGRAVING_NO_API)",
}

# Upstream gates these behind `if (MUSICXML_NO_INTERNAL) ... else()`, but the
# Qt-free configuration adapter is ours to provide (4.7.6 had the same split).
# (file, upstream target whose include dirs it needs)
EXTRA_SOURCES = {
    "noqt_support": [
        ("src/importexport/musicxml/internal/musicxmlconfiguration.cpp", "iex_musicxml"),
    ],
}

# Stable target order in the committed manifest.  The module targets come first,
# then the engine-side sources a host adapter target compiles (noqt_support).
TARGET_ORDER = [
    "muse_global",
    "muse_draw",
    "muse_mpe",
    "engraving",
    "iex_musicxml",
    "iex_guitarpro",
    "iex_midi_import",
    "native_fluidsynth",
    "muse_audio_engine",
    "noqt_support",
]

GLOBAL_INCLUDES = [
    "${SOURCE_ROOT}/muse",
    "${SOURCE_ROOT}/muse/framework",
    "${SOURCE_ROOT}/muse/framework/global",
    "${SOURCE_ROOT}/src",
]


class CmakeError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Minimal CMake interpreter
# ---------------------------------------------------------------------------


@dataclass
class Target:
    name: str
    sources: list[Path] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)
    public_defines: list[str] = field(default_factory=list)
    includes: list[Path] = field(default_factory=list)
    links: list[str] = field(default_factory=list)


def _strip_comments(text: str) -> str:
    out: list[str] = []
    in_quote = False
    i = 0
    while i < len(text):
        char = text[i]
        if char == '"':
            in_quote = not in_quote
            out.append(char)
        elif char == "#" and not in_quote:
            # bracket comment?
            if text.startswith("#[[", i):
                end = text.find("]]", i + 3)
                if end < 0:
                    raise CmakeError("unterminated bracket comment")
                i = end + 2
                continue
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(char)
        i += 1
    return "".join(out)


def _commands(text: str):
    """Yield (line_number, command_name, argument_string)."""
    text = _strip_comments(text)
    buffer = ""
    depth = 0
    in_quote = False
    start_line = 1
    line = 1
    for char in text:
        if char == "\n":
            line += 1
        if char == '"':
            in_quote = not in_quote
        if not in_quote:
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
        if depth == 0 and char == "\n":
            if buffer.strip():
                raise CmakeError("unterminated CMake command: " + buffer.strip()[:80])
            continue
        if not buffer.strip() and not char.isspace():
            start_line = line
        buffer += char
        if depth == 0 and char == ")" and buffer.strip():
            match = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*$", buffer, re.S)
            if not match:
                raise CmakeError(f"cannot parse CMake command at line {start_line}: {buffer[:80]!r}")
            yield start_line, match.group(1).lower(), match.group(2)
            buffer = ""
    if buffer.strip():
        raise CmakeError("unterminated CMake command at EOF: " + buffer.strip()[:80])


def _split_args(text: str) -> list[str]:
    args: list[str] = []
    current = ""
    in_quote = False
    depth = 0
    for char in text:
        if in_quote:
            current += char
            if char == '"':
                in_quote = False
            continue
        if char == '"':
            in_quote = True
            current += char
            continue
        if char == "(":
            depth += 1
            current += char
            continue
        if char == ")":
            depth -= 1
            current += char
            continue
        if char.isspace() and depth == 0:
            if current:
                args.append(current)
                current = ""
            continue
        current += char
    if current:
        args.append(current)
    return args


class Interpreter:
    def __init__(self, source_root: Path, variables: dict[str, object]):
        self.source_root = source_root
        self.vars: dict[str, object] = {key: value for key, value in variables.items()}
        self.targets: dict[str, Target] = {}
        self.command_defines: list[str] = []
        self.ignored_commands: set[str] = set()
        self.subdir_skips: list[str] = []
        self._current_source_dir = source_root
        self._current_list_dir = source_root
        self._condition_stack: list[tuple[bool, bool]] = []  # (parent_active, branch_taken)
        self._active = True
        self._current_target: str | None = None
        self._skip_block: str | None = None
        self.cmake_files_read: list[Path] = []
        self.trimmed_inputs: list[tuple[str, str]] = []
        self.trim_entries: list[str] | None = None
        trim_file = self.source_root / "TRIM"
        if trim_file.is_file():
            self.trim_entries = [
                line.strip()
                for line in trim_file.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.lstrip().startswith("#")
            ]

    # -- helpers -----------------------------------------------------------
    def _rel(self, path: Path) -> str:
        try:
            return path.resolve().relative_to(self.source_root).as_posix()
        except ValueError:
            return str(path)

    def _expand(self, text: str) -> str:
        if text.startswith("$<"):
            raise CmakeError(f"generator expression in argument: {text}")

        def replace(match: re.Match[str]) -> str:
            name = match.group(1)
            if name == "CMAKE_CURRENT_LIST_DIR":
                return self._current_list_dir.as_posix()
            if name == "CMAKE_CURRENT_SOURCE_DIR":
                return self._current_source_dir.as_posix()
            if name in ("CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR", "CMAKE_CURRENT_LIST_FILE"):
                if name == "CMAKE_CURRENT_LIST_FILE":
                    return (self._current_list_dir / "<current>").as_posix()
                return self.source_root.as_posix()
            if name in ("CMAKE_BINARY_DIR", "PROJECT_BINARY_DIR", "CMAKE_CURRENT_BINARY_DIR"):
                return "${BUILD_ROOT}"
            value = self.vars.get(name)
            if value is None:
                return ""
            if isinstance(value, bool):
                return "ON" if value else "OFF"
            if isinstance(value, list):
                return ";".join(str(item) for item in value)
            return str(value)

        return re.sub(r"\$\{([A-Za-z0-9_]+)\}", replace, text)

    def _arg_list(self, raw: str) -> list[str]:
        result: list[str] = []
        for token in _split_args(raw.strip()):
            quoted = len(token) >= 2 and token.startswith('"') and token.endswith('"')
            if quoted:
                result.append(self._expand(token[1:-1]))
                continue
            if re.fullmatch(r"\$\{[A-Za-z0-9_]+\}", token):
                result.extend(self._arg_list_value(token[2:-1]))
                continue
            expanded = self._expand(token)
            if expanded == "":
                continue
            result.extend(item for item in expanded.split(";") if item != "")
        return result

    def _arg_list_value(self, name: str) -> list[str]:
        value = self.vars.get(name)
        if value is None:
            return []
        if isinstance(value, list):
            return [str(item) for item in value]
        if isinstance(value, bool):
            return ["ON" if value else "OFF"]
        return [item for item in str(value).split(";") if item != ""]

    def _truthy(self, name: str) -> bool:
        value = self.vars.get(name)
        if isinstance(value, bool):
            return value
        if value is None:
            return False
        if isinstance(value, list):
            return any(_is_true_literal(item) for item in value)
        return _is_true_literal(str(value))

    def _target(self, name: str) -> Target:
        if name not in self.targets:
            self.targets[name] = Target(name)
        return self.targets[name]

    # -- conditions --------------------------------------------------------
    def _eval_condition(self, args: list[str]) -> bool:
        tokens: list[str] = []
        for arg in args:
            tokens.extend(_split_args(arg))
        position = 0

        def peek() -> str | None:
            return tokens[position] if position < len(tokens) else None

        def take() -> str:
            nonlocal position
            if position >= len(tokens):
                raise CmakeError("unexpected end of condition: " + " ".join(args))
            position += 1
            return tokens[position - 1]

        def value_of(token: str) -> str:
            if token.startswith("${") and token.endswith("}"):
                items = self._arg_list(token)
                return items[0] if items else ""
            if token.upper() in ("ON", "YES", "TRUE", "Y") and token in ("ON", "YES", "TRUE", "Y"):
                return token
            return token

        def primary() -> bool:
            token = take()
            if token == "(":
                result = or_expr()
                if take() != ")":
                    raise CmakeError("missing ')' in condition")
                return result
            upper = token.upper()
            if upper == "NOT":
                return not primary()
            if upper in ("DEFINED", "EXISTS", "TARGET", "COMMAND", "POLICY", "IS_DIRECTORY", "IS_ABSOLUTE"):
                operand = value_of(take())
                if upper == "DEFINED":
                    return operand in self.vars
                if upper == "TARGET":
                    return operand in self.targets
                if upper == "EXISTS":
                    return Path(self._expand(operand)).exists()
                return False
            # comparison operators
            if peek() is not None and peek().upper() in (
                "STREQUAL",
                "MATCHES",
                "EQUAL",
                "IN_LIST",
                "VERSION_GREATER",
                "VERSION_LESS",
                "VERSION_EQUAL",
                "VERSION_GREATER_EQUAL",
                "VERSION_LESS_EQUAL",
            ):
                operator = take().upper()
                rhs = value_of(take())
                lhs = value_of(token)
                if operator == "STREQUAL":
                    return lhs == rhs
                if operator == "MATCHES":
                    return re.fullmatch(rhs, lhs) is not None
                if operator == "EQUAL":
                    return _to_int(lhs) == _to_int(rhs)
                if operator == "IN_LIST":
                    return lhs in [item for item in str(self.vars.get(rhs, [])) .split(";")]
                return _compare_versions(lhs, rhs, operator)
            upper_token = token.upper()
            if upper_token in ("ON", "YES", "TRUE", "Y", "1"):
                return True
            if upper_token in ("OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", "0", ""):
                return False
            return self._truthy(token)

        def and_expr() -> bool:
            result = primary()
            while peek() is not None and peek().upper() == "AND":
                take()
                rhs = primary()
                result = result and rhs
            return result

        def or_expr() -> bool:
            result = and_expr()
            while peek() is not None and peek().upper() == "OR":
                take()
                rhs = and_expr()
                result = result or rhs
            return result

        result = or_expr()
        if position != len(tokens):
            raise CmakeError("trailing tokens in condition: " + " ".join(args))
        return result

    # -- execution ---------------------------------------------------------
    def run(self, relative: str) -> None:
        path = (self.source_root / relative).resolve()
        if not path.is_file():
            raise CmakeError(f"entry point missing: {path}")
        previous_source = self._current_source_dir
        self._current_source_dir = path.parent
        self._execute_file(path)
        self._current_source_dir = previous_source

    def is_trimmed(self, rel_path: str) -> bool:
        if self.trim_entries is None:
            return False
        for entry in self.trim_entries:
            if entry.endswith("/"):
                if rel_path.startswith(entry):
                    return True
            else:
                if rel_path == entry:
                    return True
        return False

    def _execute_file(self, path: Path) -> None:
        if path not in self.cmake_files_read:
            self.cmake_files_read.append(path)
        text = path.read_text(encoding="utf-8", errors="replace")
        previous_list_dir = self._current_list_dir
        self._current_list_dir = path.parent
        for line, name, raw in _commands(text):
            try:
                self._dispatch(name, raw, line)
            except CmakeError as error:
                raise CmakeError(f"{self._rel(path)}:{line}: {error}") from error
        self._current_list_dir = previous_list_dir

    def _dispatch(self, name: str, raw: str, line: int) -> None:
        if name in ("function", "macro"):
            self._skip_block = "end" + name
            return
        if name in ("endfunction", "endmacro"):
            self._skip_block = None
            return
        if self._skip_block:
            return

        if name in ("if", "elseif", "else", "endif"):
            self._condition(name, raw)
            return
        if not self._active:
            return

        if name in ("set",):
            self._cmd_set(raw)
        elif name == "list":
            self._cmd_list(raw)
        elif name == "include":
            self._cmd_include(raw)
        elif name == "add_subdirectory":
            self._cmd_add_subdirectory(raw)
        elif name in ("muse_create_module", "muse_create_thirdparty_module", "add_library"):
            self._cmd_create_target(name, raw)
        elif name == "target_sources":
            self._cmd_target_sources(raw)
        elif name == "target_compile_definitions":
            self._cmd_target_defines(raw)
        elif name == "target_include_directories":
            self._cmd_target_includes(raw)
        elif name == "target_link_libraries":
            self._cmd_target_links(raw)
        elif name in ("add_compile_definitions", "add_definitions"):
            for arg in self._arg_list(raw):
                define = arg[2:] if arg.startswith("-D") else arg
                if define:
                    self.command_defines.append(define)
        elif name == "message":
            args = self._arg_list(raw)
            if args and args[0].upper() in ("FATAL_ERROR", "SEND_ERROR"):
                raise CmakeError("message(" + " ".join(args) + ")")
        elif name == "file":
            args = self._arg_list(raw)
            if args and args[0].upper() == "GLOB":
                raise CmakeError("file(GLOB) is not supported; source lists must be explicit")
        elif name == "option":
            args = self._arg_list(raw)
            if args:
                self.vars.setdefault(args[0], args[1] if len(args) > 1 else False)
        elif name in ("foreach", "while"):
            raise CmakeError(f"{name}() loops are not supported by this generator")
        else:
            self.ignored_commands.add(name)

    def _condition(self, name: str, raw: str) -> None:
        if name == "if":
            parent_active = self._active
            result = self._eval_condition(self._arg_list(raw)) if parent_active else False
            self._condition_stack.append((parent_active, result))
            self._active = parent_active and result
        elif name == "elseif":
            if not self._condition_stack:
                raise CmakeError("elseif() without if()")
            parent_active, taken = self._condition_stack[-1]
            result = False
            if parent_active and not taken:
                result = self._eval_condition(self._arg_list(raw))
            self._condition_stack[-1] = (parent_active, taken or result)
            self._active = parent_active and not taken and result
        elif name == "else":
            if not self._condition_stack:
                raise CmakeError("else() without if()")
            parent_active, taken = self._condition_stack[-1]
            self._active = parent_active and not taken
            self._condition_stack[-1] = (parent_active, True)
        else:  # endif
            if not self._condition_stack:
                raise CmakeError("endif() without if()")
            parent_active, _ = self._condition_stack.pop()
            self._active = parent_active

    def _cmd_target_links(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        target = self._target(args[0])
        for item in args[1:]:
            if item.upper() in ("PRIVATE", "PUBLIC", "INTERFACE"):
                continue
            if item.startswith("$"):
                continue
            # Imported/external targets (Qt::Core, freetype::freetype, ...) are
            # supplied by xmake packages; only in-tree modules are followed.
            if item not in target.links:
                target.links.append(item)

    def resolve_includes(self) -> None:
        """Propagate includes and PUBLIC defines through in-tree links.

        CMake gives a target the include dirs and the PUBLIC compile definitions
        of what it links (PRIVATE is enough for its own compilation), which is
        how muse_audio_engine sees the FluidSynth headers and NO_GLIB/NO_THREADS
        from the fluidsynth target (target_include_directories /
        target_compile_definitions PUBLIC).
        """
        resolved_includes: dict[str, list[Path]] = {}
        resolved_defines: dict[str, list[str]] = {}

        def includes_of(name: str, seen: set[str]) -> list[Path]:
            if name in resolved_includes:
                return resolved_includes[name]
            if name in seen:
                return []
            seen.add(name)
            target = self.targets.get(name)
            if target is None:
                return []
            result = list(target.includes)
            for dep in target.links:
                for include in includes_of(dep, seen):
                    if include not in result:
                        result.append(include)
            resolved_includes[name] = result
            return result

        def defines_of(name: str, seen: set[str]) -> list[str]:
            if name in resolved_defines:
                return resolved_defines[name]
            if name in seen:
                return []
            seen.add(name)
            target = self.targets.get(name)
            if target is None:
                return []
            result = list(target.public_defines)
            for dep in target.links:
                for define in defines_of(dep, seen):
                    if define not in result:
                        result.append(define)
            resolved_defines[name] = result
            return result

        for name in self.targets:
            includes_of(name, set())
            defines_of(name, set())
        for name, includes in resolved_includes.items():
            self.targets[name].includes = includes
        for name, defines in resolved_defines.items():
            target = self.targets[name]
            for define in defines:
                if define not in target.defines:
                    target.defines.append(define)

    def _cmd_set(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        name = args[0]
        values = [item for item in args[1:] if item not in ("CACHE", "PARENT_SCOPE", "INTERNAL")]
        if "CACHE" in args:
            # set(VAR value CACHE STRING "doc") -> keep the value
            values = [item for item in args[1:] if item not in ("CACHE", "PARENT_SCOPE", "INTERNAL")]
        self.vars[name] = values

    def _cmd_list(self, raw: str) -> None:
        args = self._arg_list(raw)
        if len(args) < 2:
            return
        action, name = args[0].upper(), args[1]
        current = self.vars.get(name)
        current_list = list(current) if isinstance(current, list) else ([] if current is None else [current])
        if action == "APPEND":
            self.vars[name] = current_list + args[2:]
        elif action == "REMOVE_ITEM":
            self.vars[name] = [item for item in current_list if item not in args[2:]]
        elif action == "REMOVE_DUPLICATES":
            seen: set[str] = set()
            result = []
            for item in current_list:
                if item not in seen:
                    seen.add(item)
                    result.append(item)
            self.vars[name] = result
        elif action == "PREPEND":
            self.vars[name] = args[2:] + current_list
        # LENGTH / GET / FIND are read-only and unused here.

    def _cmd_include(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        candidate = Path(self._expand(args[0]))
        if not candidate.is_absolute():
            candidate = self._current_source_dir / candidate
        if candidate.suffix != ".cmake":
            candidate = candidate.with_suffix(".cmake")
        if not candidate.is_file():
            alt = self._current_list_dir / args[0]
            if alt.suffix != ".cmake":
                alt = alt.with_suffix(".cmake")
            if alt.is_file():
                candidate = alt
        if not candidate.is_file():
            # A CMake builtin module (GetPlatformInfo, CheckCompilerVersion, ...).
            self.ignored_commands.add("include:" + args[0])
            return
        self._execute_file(candidate.resolve())

    def _cmd_add_subdirectory(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        directory = (self._current_source_dir / args[0]).resolve()
        try:
            relative = directory.relative_to(self.source_root).as_posix()
        except ValueError:
            relative = str(directory)
        if relative in SKIP_SUBDIRS:
            self.subdir_skips.append(relative)
            return
        cmakelists = directory / "CMakeLists.txt"
        if not cmakelists.is_file():
            raise CmakeError(f"add_subdirectory without CMakeLists.txt: {directory}")
        previous_source = self._current_source_dir
        self._current_source_dir = directory
        self._execute_file(cmakelists)
        self._current_source_dir = previous_source

    def _cmd_create_target(self, command: str, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        name = args[0]
        target = self._target(name)
        self._current_target = name
        if command == "muse_create_module":
            # muse_create_module() include dirs (cmake/MuseCreateModule.cmake).
            for item in (
                self._current_source_dir,
                self.source_root / "src",
                self.source_root / "muse",
                self.source_root / "muse" / "framework",
                self.source_root / "muse" / "framework" / "global",
            ):
                if item.is_dir() and item not in target.includes:
                    target.includes.append(item)

    def _cmd_target_sources(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        target = self._target(args[0])
        for item in args[1:]:
            if item.upper() in ("PRIVATE", "PUBLIC", "INTERFACE"):
                continue
            if item.startswith("$<"):
                raise CmakeError(f"generator expression in target_sources({args[0]}): {item}")
            path = Path(item)
            if not path.is_absolute():
                path = (self._current_source_dir / path).resolve()
            if not path.exists():
                alternative = (self._current_list_dir / item).resolve()
                if alternative.exists():
                    path = alternative
                else:
                    rel = self._rel(path)
                    if self.trim_entries is not None and self.is_trimmed(rel):
                        if path.suffix.lower() in SOURCE_EXTENSIONS and rel not in DROPPED:
                            raise CmakeError(f"target_sources({args[0]}): compiled source is covered by TRIM: {rel}")
                        self.trimmed_inputs.append((target.name, rel))
                        continue
                    raise CmakeError(f"target_sources({args[0]}): missing file {rel}")
            if path.suffix.lower() not in SOURCE_EXTENSIONS:
                # Headers and other non-compiled entries are validated but not
                # written to the manifest.
                continue
            rel = self._rel(path)
            if self.trim_entries is not None and self.is_trimmed(rel) and rel not in DROPPED:
                raise CmakeError(f"target_sources({args[0]}): compiled source is covered by TRIM: {rel}")
            target.sources.append(path)

    def _cmd_target_defines(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        target = self._target(args[0])
        scope = "PRIVATE"
        for item in args[1:]:
            if item.upper() in ("PRIVATE", "PUBLIC", "INTERFACE"):
                scope = item.upper()
                continue
            define = item[2:] if item.startswith("-D") else item
            if not define:
                continue
            if scope != "INTERFACE" and define not in target.defines:
                target.defines.append(define)
            if scope in ("PUBLIC", "INTERFACE") and define not in target.public_defines:
                target.public_defines.append(define)

    def _cmd_target_includes(self, raw: str) -> None:
        args = self._arg_list(raw)
        if not args:
            return
        target = self._target(args[0])
        for item in args[1:]:
            if item.upper() in ("PRIVATE", "PUBLIC", "INTERFACE", "SYSTEM", "BEFORE"):
                continue
            if item.startswith("$<"):
                raise CmakeError(f"generator expression in target_include_directories: {item}")
            path = Path(self._expand(item))
            if not path.is_absolute():
                path = (self._current_source_dir / path).resolve()
            if path.is_dir() and path not in target.includes:
                target.includes.append(path)


def _is_true_literal(value: str) -> bool:
    return value.upper() in ("ON", "YES", "TRUE", "Y", "1") or (
        value != "" and value.upper() not in ("OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", "0")
    )


def _to_int(value: str) -> int:
    try:
        return int(value)
    except ValueError:
        return 0


def _version_tuple(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", value))


def _compare_versions(lhs: str, rhs: str, operator: str) -> bool:
    left, right = _version_tuple(lhs), _version_tuple(rhs)
    if operator == "VERSION_GREATER":
        return left > right
    if operator == "VERSION_LESS":
        return left < right
    if operator == "VERSION_EQUAL":
        return left == right
    if operator == "VERSION_GREATER_EQUAL":
        return left >= right
    return left <= right


# ---------------------------------------------------------------------------
# Manifest assembly
# ---------------------------------------------------------------------------


def _portable(path: Path, source_root: Path, project_root: Path) -> str:
    try:
        return "${SOURCE_ROOT}/" + path.relative_to(source_root).as_posix()
    except ValueError:
        pass
    try:
        return "${PROJECT_ROOT}/" + path.relative_to(project_root).as_posix()
    except ValueError:
        pass
    raise CmakeError(f"path is outside the portable roots: {path}")


def route(interpreter: Interpreter, drops: dict[str, str]) -> dict[str, Target]:
    """Map upstream targets onto our manifest targets."""
    result: dict[str, Target] = {}

    def out(name: str) -> Target:
        if name not in result:
            result[name] = Target(name)
        return result[name]

    def add(manifest_target: str, upstream: Target, path: Path, defines, includes) -> None:
        target = out(manifest_target)
        if path not in target.sources:
            target.sources.append(path)
        for define in defines:
            if define not in target.defines:
                target.defines.append(define)
        for include in includes:
            if include not in target.includes:
                target.includes.append(include)

    for name, upstream in interpreter.targets.items():
        if not upstream.sources:
            continue
        kept = []
        for path in upstream.sources:
            relative = path.relative_to(interpreter.source_root).as_posix()
            if relative in drops:
                continue
            kept.append(path)
        upstream_sources = kept
        if name in ("muse_global", "muse_mpe", "engraving", "iex_musicxml"):
            for path in upstream_sources:
                add(name, upstream, path, upstream.defines, upstream.includes)
        elif name == "fluidsynth":
            for path in upstream_sources:
                add("native_fluidsynth", upstream, path, upstream.defines, upstream.includes)
        elif name == "muse_draw":
            for path in upstream_sources:
                relative = path.relative_to(interpreter.source_root).as_posix()
                if "/internal/" in relative:
                    add("noqt_support", upstream, path, upstream.defines, upstream.includes)
                else:
                    add("muse_draw", upstream, path, upstream.defines, upstream.includes)
        elif name == "muse_audio_engine" or name == "muse_audio_common":
            for path in upstream_sources:
                add("muse_audio_engine", upstream, path, upstream.defines, upstream.includes)
        elif name == "beatroot":
            for path in upstream_sources:
                add("iex_midi_import", upstream, path, upstream.defines, upstream.includes)
        elif name == "iex_midi":
            for path in upstream_sources:
                relative = path.relative_to(interpreter.source_root).as_posix()
                if "/internal/midiimport/" in relative:
                    add("iex_midi_import", upstream, path, upstream.defines, upstream.includes)
                elif "/internal/midishared/" in relative or "/internal/midiexport/" in relative:
                    add("noqt_support", upstream, path, upstream.defines, upstream.includes)
                else:
                    raise CmakeError(
                        "unrouted iex_midi source (add to route()/DROPPED): " + relative)
        elif name == "iex_guitarpro":
            for path in upstream_sources:
                add("iex_guitarpro", upstream, path, upstream.defines, upstream.includes)
        else:
            raise CmakeError(f"unrouted upstream target: {name}")
    return result


def build_manifest(source_root: Path, project_root: Path, verbose: bool) -> dict:
    interpreter = Interpreter(source_root, SELECTION)
    for entry in ENTRY_POINTS:
        interpreter.run(entry)

    # Link an upstream target's include dirs into its consumers, as CMake does.
    interpreter.resolve_includes()

    interpreter.targets.setdefault("beatroot", Target("beatroot"))

    # Verify the route table covers the upstream file set exactly.
    dropped_paths: dict[str, str] = {}
    seen_upstream: set[str] = set()
    for target in interpreter.targets.values():
        for path in target.sources:
            seen_upstream.add(path.relative_to(source_root).as_posix())
    for relative in DROPPED:
        if relative not in seen_upstream:
            if interpreter.trim_entries is not None and interpreter.is_trimmed(relative):
                continue
            raise CmakeError("DROPPED lists a file the engine no longer compiles: " + relative)
    dropped_paths = dict(DROPPED)

    routed = route(interpreter, dropped_paths)

    # Documented extra sources (compile-time selection, not file routing).
    for manifest_target, entries in EXTRA_SOURCES.items():
        for relative, owner in entries:
            path = source_root / relative
            if not path.is_file():
                raise CmakeError("EXTRA_SOURCES entry missing: " + relative)
            target = routed.setdefault(manifest_target, Target(manifest_target))
            target.sources.append(path)
            for include in interpreter.targets[owner].includes:
                if include not in target.includes:
                    target.includes.append(include)

    # The engine's own Qt-free font faces and MIDI export live in the engine
    # tree but are compiled by the host adapter target; route() has already
    # collected them under "noqt_support", including its includes.
    routed.setdefault("noqt_support", Target("noqt_support"))

    if not routed:
        raise CmakeError("no targets were generated")

    if interpreter.trim_entries is not None:
        for target_name, target in routed.items():
            for path in target.sources:
                if path.suffix.lower() in SOURCE_EXTENSIONS:
                    try:
                        rel = path.relative_to(source_root).as_posix()
                    except ValueError:
                        continue
                    if interpreter.is_trimmed(rel):
                        raise CmakeError(f"{target_name}: compiled source is covered by TRIM: {rel}")

    # Serialise: filter to compilable sources, use portable paths.
    manifest: dict = {"targets": {}, "defines": list(PROJECT_DEFINES),
                      "global_defines": list(GLOBAL_DEFINES),
                      "includes": list(GLOBAL_INCLUDES)}
    seen_portable: dict[str, str] = {}
    for name in TARGET_ORDER:
        target = routed.get(name)
        if target is None:
            continue
        sources: list[str] = []
        for path in target.sources:
            if path.suffix.lower() not in SOURCE_EXTENSIONS:
                continue
            portable = _portable(path, source_root, project_root)
            if not (path.is_file()):
                raise CmakeError(f"{name}: source does not exist: {portable}")
            if portable in seen_portable and portable.startswith("${SOURCE_ROOT}/"):
                raise CmakeError(
                    f"{portable} is compiled by both {seen_portable[portable]} and {name}")
            seen_portable[portable] = name
            sources.append(portable)
        includes: list[str] = []
        for path in target.includes:
            portable = _portable(path, source_root, project_root)
            if portable not in includes:
                includes.append(portable)
        entry: dict = {
            "sources": _sort_sources(sources),
            "defines": sorted(_dedupe(GLOBAL_DEFINES + interpreter.command_defines + target.defines)),
            "includes": includes,
        }
        manifest["targets"][name] = entry

    unknown = set(routed) - set(TARGET_ORDER)
    if unknown:
        raise CmakeError(f"generated targets missing from TARGET_ORDER: {sorted(unknown)}")

    if verbose:
        for name in TARGET_ORDER:
            entry = manifest["targets"].get(name)
            if entry:
                print(f"{name}: {len(entry['sources'])} sources, "
                      f"{len(entry['defines'])} defines, {len(entry['includes'])} includes")
        if interpreter.ignored_commands:
            print("ignored CMake commands: " + ", ".join(sorted(interpreter.ignored_commands)))
        for relative, reason in dropped_paths.items():
            print(f"dropped {relative}: {reason}")
        for target_name, rel in interpreter.trimmed_inputs:
            print(f"trimmed {rel} (covered by TRIM, target {target_name})")
        for relative in interpreter.subdir_skips:
            print(f"skipped add_subdirectory {relative}")

    _self_check(manifest, source_root)
    manifest["_trimmed_inputs"] = list(interpreter.trimmed_inputs)
    manifest["_cmake_files_read"] = [p.relative_to(source_root).as_posix() for p in interpreter.cmake_files_read]
    return manifest


def _dedupe(values: list[str]) -> list[str]:
    seen: set[str] = set()
    result = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def _sort_sources(sources: list[str]) -> list[str]:
    """Sort; the manifest is engine-only, so every path is a SOURCE_ROOT one."""
    return sorted(sources)


def _self_check(manifest: dict, source_root: Path) -> None:
    def sources(target: str) -> list[str]:
        return manifest["targets"][target]["sources"]

    # Fonts: the FT backend + upstream text drawing must be compiled, and the
    # SDF render cache is required by FontsEngine (5.0).
    for required in (
        "${SOURCE_ROOT}/muse/framework/draw/internal/fontfaceft.cpp",
        "${SOURCE_ROOT}/muse/framework/draw/internal/fontrendercache.cpp",
        "${SOURCE_ROOT}/muse/framework/draw/internal/fontsengine.cpp",
        "${SOURCE_ROOT}/muse/framework/draw/internal/fontprovider.cpp",
        "${SOURCE_ROOT}/muse/framework/draw/internal/fontsdatabase.cpp",
    ):
        if required not in sources("noqt_support"):
            raise CmakeError(f"noqt_support must compile {required}")
    if any("/internal/fontfacext.cpp" in item for item in sources("noqt_support")):
        raise CmakeError("fontfacext.cpp needs msdfgen/lodepng; the XT backend is off")

    # The msdfgen SDF path must stay behind MUSE_MODULE_DRAW_USE_QTTEXTDRAW.
    noqt_defines = manifest["targets"]["noqt_support"]["defines"]
    for required in ("MUSE_MODULE_DRAW_USE_FONTFACE_FT", "MUSE_MODULE_DRAW_USE_QTTEXTDRAW"):
        if required not in noqt_defines:
            raise CmakeError(f"noqt_support must define {required}")

    # dtl moved into src/engraving/thirdparty.
    engraving_includes = manifest["targets"]["engraving"]["includes"]
    dtl = "${SOURCE_ROOT}/src/engraving/thirdparty/dtl"
    if dtl not in engraving_includes:
        raise CmakeError(f"engraving must include {dtl}")
    if not (source_root / "src/engraving/thirdparty/dtl").is_dir():
        raise CmakeError("the engine has no src/engraving/thirdparty/dtl")

    # No Qt-only module may sneak in.
    for name, entry in manifest["targets"].items():
        for item in entry["sources"]:
            lowered = item.lower()
            if "/qml/" in lowered or lowered.endswith("_qml.cpp"):
                raise CmakeError(f"{name} compiles QML-facing source {item}")
            if name in ("muse_global", "muse_draw", "muse_mpe", "engraving") and \
                    "/tests/" in lowered:
                raise CmakeError(f"{name} compiles a test source {item}")

    # Engraving internal Qt-free subset must be compiled.
    for required in (
        "${SOURCE_ROOT}/src/engraving/internal/engravingconfiguration.cpp",
        "${SOURCE_ROOT}/src/engraving/internal/engravingcontextconfiguration.cpp",
        "${SOURCE_ROOT}/src/engraving/internal/engravingfontsprovider.cpp",
        "${SOURCE_ROOT}/src/engraving/internal/engravingfont.cpp",
        "${SOURCE_ROOT}/src/engraving/internal/palettescoreprovider.cpp",
    ):
        if required not in sources("engraving"):
            raise CmakeError(f"engraving must compile {required}")

    # Engraving must not compile accessibility/, qmimedataadapter, or the api/v1 layer.
    for item in sources("engraving"):
        if "/accessibility/" in item:
            raise CmakeError("engraving is configured with ENGRAVING_NO_ACCESSIBILITY")
        if item.endswith("qmimedataadapter.cpp"):
            raise CmakeError("qmimedataadapter.cpp needs Qt")
        if "/engraving/api/v1/" in item:
            raise CmakeError("engraving is configured with ENGRAVING_NO_API")

    # noqt_support compiles the engine's font faces and MIDI export, so it needs
    # the engine include dirs those units expect.
    noqt_includes = manifest["targets"]["noqt_support"]["includes"]
    for req in (
        "${SOURCE_ROOT}/src/engraving/thirdparty/dtl",
        "${SOURCE_ROOT}/src",
        "${SOURCE_ROOT}/src/engraving",
        "${SOURCE_ROOT}/muse/framework",
        "${SOURCE_ROOT}/muse/framework/global",
        "${SOURCE_ROOT}/muse/framework/draw",
    ):
        if req not in noqt_includes:
            raise CmakeError(f"noqt_support missing required include: {req}")

    # The musicxml Qt-free configuration adapter is ours to place.
    if not any(item.endswith("musicxmlconfiguration.cpp") for item in sources("noqt_support")):
        raise CmakeError("musicxmlconfiguration.cpp must be compiled by noqt_support")

    # MIDI import must not pull the Qt table model in.
    for item in sources("iex_midi_import"):
        if item.endswith("importmidi_model.cpp"):
            raise CmakeError("importmidi_model.cpp is a Qt-only table model")

    # FluidSynth stays in tree.
    if not any("fluidsynth-2.3.3" in item for item in sources("native_fluidsynth")):
        raise CmakeError("FluidSynth sources are missing from native_fluidsynth")

    # The manifest is the engine's own source graph: nothing may point outside
    # this checkout.  A host's sources, include dirs and generated files belong
    # to the host's build file, not here.
    for name, entry in manifest["targets"].items():
        for item in entry["sources"] + entry["includes"]:
            if item.startswith("${PROJECT_ROOT}"):
                raise CmakeError(
                    f"{name}: manifest must not reference non-engine paths: {item}")
    for item in manifest["includes"]:
        if item.startswith("${PROJECT_ROOT}"):
            raise CmakeError(f"global includes must be engine paths: {item}")


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def _lua_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def write_manifest(manifest: dict, output: Path) -> None:
    lines = [
        "-- Generated by tools/capture_xmake_manifest.py from the engine's own",
        "-- CMake source lists (muse/framework/*/CMakeLists.txt, src/*/CMakeLists.txt).",
        "-- Do not edit by hand: change the selection in that script and regenerate.",
        "-- Paths use ${SOURCE_ROOT} (this checkout); host files belong to the host.",
        "MUSESCORE_XMAKE_MANIFEST = {",
        "    targets = {",
    ]
    for name in TARGET_ORDER:
        spec = manifest["targets"].get(name)
        if spec is None:
            continue
        lines.append(f"        [{_lua_string(name)}] = {{")
        lines.append("            sources = {")
        for item in spec["sources"]:
            lines.append(f"                {_lua_string(item)},")
        lines.append("            },")
        if spec.get("defines"):
            lines.append("            defines = {")
            for item in spec["defines"]:
                lines.append(f"                {_lua_string(item)},")
            lines.append("            },")
        if spec.get("includes"):
            lines.append("            includes = {")
            for item in spec["includes"]:
                lines.append(f"                {_lua_string(item)},")
            lines.append("            },")
        lines.append("        },")
    lines.extend(
        [
            "    },",
            "    -- Applied to every engine target: this build's Qt-free feature",
            "    -- selection.",
            "    defines = {",
        ]
    )
    for item in manifest["defines"]:
        lines.append(f"        {_lua_string(item)},")
    lines.extend(
        [
            "    },",
            "    -- Upstream's project-wide defines (add_compile_definitions).",
            "    global_defines = {",
        ]
    )
    for item in manifest["global_defines"]:
        lines.append(f"        {_lua_string(item)},")
    lines.extend(
        [
            "    },",
            "    includes = {",
        ]
    )
    for item in manifest["includes"]:
        lines.append(f"        {_lua_string(item)},")
    lines.extend(["    },", "}", ""])

    text = "\n".join(lines)
    if re.search(r"(?<![A-Za-z])[A-Za-z]:[\\/]", text):
        raise CmakeError("generated manifest contains an absolute Windows path")
    if re.search(r"(?<![A-Za-z])/(home|usr|opt|tmp)/", text):
        raise CmakeError("generated manifest contains an absolute Unix path")
    output.parent.mkdir(parents=True, exist_ok=True)
    # The repository pins eol=lf; write bytes so Windows keeps LF too.
    output.write_bytes(text.encode("utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        help="engine checkout (default: this repository)")
    parser.add_argument("--output", type=Path, help="manifest output path")
    parser.add_argument("--verbose", action="store_true", help="print the routing report")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    source_root = (args.source_root or root).resolve()
    output = (args.output or root / "xmake" / "manifest.lua").resolve()

    if not (source_root / "version.cmake").is_file():
        print(f"error: engine checkout not found: {source_root}", file=sys.stderr)
        return 2
    if not (source_root / "muse/framework").is_dir():
        print(
            f"error: {source_root} has no muse/framework; the manifest generator needs the "
            "MuseScore 5.0 tree layout (framework absorbed at muse/).",
            file=sys.stderr,
        )
        return 2

    try:
        manifest = build_manifest(source_root, source_root, args.verbose)
        write_manifest(manifest, output)
    except (OSError, CmakeError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    total = sum(len(spec["sources"]) for spec in manifest["targets"].values())
    print(f"wrote {output}")
    print(f"{total} sources across {len(manifest['targets'])} targets, "
          f"{len(manifest['includes'])} include directories")
    if "_trimmed_inputs" in manifest and manifest["_trimmed_inputs"]:
        print(f"{len(manifest['_trimmed_inputs'])} CMake-listed inputs intentionally trimmed (covered by TRIM)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
