#!/usr/bin/env python3
"""Reproducible end-to-end test for `tools/sync-upstream.py` (roadmap-engrave A1).

The test never touches the repository it is run from: it creates a throwaway
clone (`git clone --shared --no-checkout`, so the object store is shared and no
network is needed), rebuilds the trimmed base the fork started from, and then
replays a sequence of real syncs against real upstream history.

Base: upstream f81ba926fa (gitlink `muse` = 95da93e431) plus one commit that
adds a test TRIM (`.gitmodules`, `muse_deps`, `src/appshell/`), then `absorb`.
Every later step syncs the pinned upstream commit listed below, in order, and
checks exit codes, commit parents, the discarded/dropped report and the tree:

  step 1  absorb from f81ba926fa             framework 95da93e431
  step 2  sync --target 7b2d005464           gitlink bump to e40d9c9dc6
  step 3  sync --target 840bd348e1           no gitlink bump -> one merge only
  step 4  sync --target 11157e4086           framework conflict, resolved by hand
  step 5  sync --target f019f4079d           interrupted twice, then `continue`
  step 6  sync --target 9625e32ad4           pins framework 353f5f612a (missing)
  step 7  sync --target 0ea613f              final upstream commit
  step 8  sync --target 0ea613f              idempotent rerun

Steps 9-26 build their own history with plumbing (`make_commit`, see below): a
synthetic commit on top of the pinned upstream commit, published as the branch
test/extra/upstream, and synthetic framework commits on the framework mirror. They
cover the hardening findings:

  step 9   a target that adds a root TRIM file is refused
  step 10  an upstream path with spaces and non-ASCII under a trimmed prefix is
           dropped and reported verbatim (NUL-delimited path handling)
  step 11  a colliding regular file named like the `muse~<sha>` link conflict is
           not silently dropped, it stops for a human
  step 12  a framework commit merged in through a side parent does not change the
           recorded framework base, and the wrong merge base is refused
  step 13  a rewritten framework mirror is refused
  step 14  a fatal `git merge` failure (index lock) stops, keeps the state, and
           `continue` retries after the cause is fixed
  step 15  a manual `git merge --abort` before `continue` is not treated as solved
  step 16  `abort` refuses to touch a foreign merge
  step 17  `continue` resumes from every stage boundary, including a completed
           fast-forward that has not been cleaned up
  step 18  a leftover `sync/<sha>` branch without state is reported, not guessed at
  step 19  rerunning `sync` after a completed fast-forward cleans up and reports
           "already contains"

Steps 20-26 are the second hardening round (A-G of the follow-up review):

  step 20  (A) `sync` refuses an untracked or ignored file under a removed path, and
           `continue`/`abort` refuse an edit the sync did not make
  step 21  (B) trimmed additions whose names look like globs (`item[ab].txt` next to
           `itema.txt`) are removed literally and reported individually
  step 22  (C) a sync run with PYTHONIOENCODING=gbk still reports non-ASCII paths and
           exits 0
  step 23  (D) a merge commit made outside the tool (no provenance trailer) is
           refused, for the upstream and the framework merge
  step 24  (E) an upstream gitlink at `muse~important.txt` with the link's SHA is a
           real conflict, not ort's displaced name
  step 25  (F) `abort`/`continue` work at the start save, the temp-branch stop and
           both fast-forward windows, and refuse a merge the sync did not start
  step 26  (G) an interruption between `git rm --cached` and the worktree removal is
           replayed: no lost report entry, no leftover file

Requirements: python 3.10+, git 2.40+ (ort merge strategy, `-Xsubtree`), and a
clone that carries the mirrors `origin/upstream/musescore` and
`origin/upstream/muse-framework` plus the framework commits the pins name. The
source clone is a blobless partial clone, so its object store must already hold
the blobs of the pinned trees; a normal sync run against it fetches them.

Usage:
  python tools/test_sync_upstream.py [--repo PATH] [--keep]

`--repo` is the repository containing `tools/sync-upstream.py` (default: the repo
this script lives in). `--keep` keeps the temporary clone for inspection. Prints
PASS/FAIL per step, aborts on the first failure (later steps depend on it) and
exits 1 if any step failed.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

# --- pinned history -------------------------------------------------------------
# Upstream commits on origin/upstream/musescore and the `muse` gitlink each pins.
BASE = "f81ba926fa7c557f663ee51ec88d6e1a460b905e"  # "removed some ui actions"
TARGET_LINK_BUMP = "7b2d00546419848f3c9ba1f786b22e84163dd2f5"  # step 60
TARGET_NO_LINK_BUMP = "840bd348e1d28a0375cb62be19fa5466c8188fab"  # linked-range fix
TARGET_FRAMEWORK_CONFLICT = "11157e40866fccaff469cbfe516d4469dbc5efef"  # port_convert_file_to_score_2_mss
TARGET_INTERRUPTED = "f019f4079d0ba74c481fa93396b3486a77f22635"  # remove context module
TARGET_MISSING_FRAMEWORK = "9625e32ad4c1a700b8057a35fb40d510571595ea"  # notation tab scope
TARGET_FINAL = "0ea613ff3426b9383c5276996108003be2f0e7f2"  # upstream main

# Framework commits on origin/upstream/muse-framework.
FW_BASE = "95da93e431fa683757bfdba56218878b699b22f0"
FW_LINK_BUMP = "e40d9c9dc6b31733f53f9dc2ae1ca06a63bbf310"
FW_CONFLICT = "3cae2ca17031dced34785f1ae42b2819195391d1"
FW_FINAL = "ecdc6e7ef87b41c3fd2ce4e9df39343640862c4f"
# Pinned by TARGET_MISSING_FRAMEWORK but never pushed to the mirror.
FW_MISSING = "353f5f612a057205ecea635d21216edd86325699"
# The absorbed framework commit on main, recorded without a trailer (fallback path).
ABSORB_COMMIT = "7150f2b05ec4a19dafb3075513bca2ebcd970989"

UPSTREAM_REF = "origin/upstream/musescore"
FRAMEWORK_REF = "origin/upstream/muse-framework"

# (upstream commit, framework gitlink, what the step does with it)
PINNED = [
    (BASE, FW_BASE, "base"),
    (TARGET_LINK_BUMP, FW_LINK_BUMP, "step 2"),
    (TARGET_NO_LINK_BUMP, FW_LINK_BUMP, "step 3"),
    (TARGET_FRAMEWORK_CONFLICT, FW_CONFLICT, "step 4"),
    (TARGET_INTERRUPTED, FW_CONFLICT, "step 5"),
    (TARGET_MISSING_FRAMEWORK, FW_MISSING, "step 6"),
    (TARGET_FINAL, FW_FINAL, "step 7"),
]

TRIM = """# Test-only TRIM for tools/test_sync_upstream.py: the two submodule gitlinks and
# the appshell trim the fork also drops. One path per line, `#` comments; a
# trailing `/` marks a directory prefix, otherwise the entry is an exact file.
.gitmodules
muse_deps
src/appshell/
"""

# Discarded changes and dropped additions each sync must report. Derived from
# `git diff --no-renames --name-status <from> <to> -- .gitmodules muse_deps
# src/appshell` between the base and the targets above; `preflight` re-derives
# them from history so the expectations cannot silently drift.
STEP2_DISCARDED = [
    ".gitmodules",
    "muse_deps",
    "src/appshell/internal/appshellcommandscontroller.cpp",
    "src/appshell/internal/appshellcommandscontroller.h",
    "src/appshell/internal/sessionsmanager.cpp",
    "src/appshell/internal/sessionsmanager.h",
    "src/appshell/internal/startupscenario.cpp",
    "src/appshell/internal/startupscenario.h",
    "src/appshell/qml/MuseScore/AppShell/CMakeLists.txt",
    "src/appshell/qml/MuseScore/AppShell/appmenumodel.cpp",
    "src/appshell/qml/MuseScore/AppShell/appmenumodel.h",
    "src/appshell/qml/MuseScore/AppShell/navigableappmenumodel.cpp",
    "src/appshell/qml/MuseScore/AppShell/notationpagemodel.cpp",
    "src/appshell/qml/MuseScore/AppShell/notationpagemodel.h",
]
STEP4_DISCARDED = [
    "muse_deps",
    "src/appshell/qml/MuseScore/AppShell/NotationReviewPage/ReviewNotationView.qml",
]
STEP5_DISCARDED = [
    "src/appshell/CMakeLists.txt",
    "src/appshell/appshellmodule.cpp",
    "src/appshell/appshellmodule.h",
    "src/appshell/internal/appshellcommandscontroller.cpp",
    "src/appshell/internal/appshellcommandscontroller.h",
]
STEP5_ADDED = [
    "src/appshell/internal/extensioncontextresolver.cpp",
    "src/appshell/internal/extensioncontextresolver.h",
    "src/appshell/internal/shortcutresolver.cpp",
    "src/appshell/internal/shortcutresolver.h",
]
# Step 6 stops before merging anything, so step 7 merges f019f4079d..0ea613f.
# Note `.gitmodules` changes twice in that span and cancels out, so it is not
# reported here although step 2 and step 4 do discard it.
STEP7_DISCARDED = [
    "src/appshell/internal/appshellcommandscontroller.cpp",
    "src/appshell/internal/appshellcommandscontroller.h",
    "src/appshell/internal/extensioncontextresolver.cpp",
    "src/appshell/internal/extensioncontextresolver.h",
    "src/appshell/internal/sessionsmanager.cpp",
    "src/appshell/internal/sessionsmanager.h",
    "src/appshell/internal/shortcutresolver.cpp",
    "src/appshell/internal/shortcutresolver.h",
    "src/appshell/internal/startupscenario.cpp",
    "src/appshell/internal/startupscenario.h",
    "src/appshell/qml/MuseScore/AppShell/appmenumodel.cpp",
    "src/appshell/qml/MuseScore/AppShell/appmenumodel.h",
    "src/appshell/qml/MuseScore/AppShell/maintoolbarmodel.cpp",
    "src/appshell/qml/MuseScore/AppShell/maintoolbarmodel.h",
    "src/appshell/qml/MuseScore/AppShell/mainwindowtitleprovider.cpp",
    "src/appshell/qml/MuseScore/AppShell/mainwindowtitleprovider.h",
    "src/appshell/qml/MuseScore/AppShell/notationpagemodel.cpp",
    "src/appshell/qml/MuseScore/AppShell/notationpagemodel.h",
    "src/appshell/qml/MuseScore/AppShell/notationstatusbarmodel.h",
]

# The file the local framework delta touches, and the marker it inserts.
CONFLICT_FILE = "muse/framework/cloud/CMakeLists.txt"
CONFLICT_FILE_IN_FRAMEWORK = "framework/cloud/CMakeLists.txt"
LOCAL_EDIT = "# LOCAL-EDIT-SYNC-TEST: engraved local delta next to the upstream cloud tests block"
UPSTREAM_ANCHOR_ABSENT = "if (MUSE_MODULE_CLOUD_TESTS)"  # added by FW_CONFLICT
UPSTREAM_ANCHOR_PRESENT = "if (MUSE_MODULE_CLOUD_QML)"  # insertion point before FW_CONFLICT

FINAL_DIFF = ["D\t.gitmodules", "A\tTRIM", "D\tmuse_deps"]

REPORT_HEADERS = {
    "discardedChanges": "upstream changes discarded on trimmed paths: ",
    "droppedAdditions": "new upstream files dropped under trimmed prefixes: ",
    "resolutions": "conflict resolutions that differ from the merge sides: ",
}

SYNC_TIMEOUT = 1800
TEST_IDENTITY = "sync-upstream-test@example.invalid"


class Failure(Exception):
    """A step assertion failed."""


# --- process helpers ------------------------------------------------------------

def build_env(tmp: Path) -> dict:
    """Isolated environment: no user/system git config, no prompts, fixed identity."""
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("GIT_") and key != "SYNC_UPSTREAM_TEST_STOP_AFTER"}
    empty_config = tmp / "empty-gitconfig"
    empty_config.write_bytes(b"")
    env.update({
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": str(empty_config),
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_AUTHOR_NAME": "Sync Upstream Test",
        "GIT_AUTHOR_EMAIL": TEST_IDENTITY,
        "GIT_COMMITTER_NAME": "Sync Upstream Test",
        "GIT_COMMITTER_EMAIL": TEST_IDENTITY,
        "PYTHONIOENCODING": "utf-8",
        "PYTHONUTF8": "1",
    })
    return env


def run(cmd: list[str], cwd: Path | None, env: dict, check: bool = True,
        timeout: int | None = None) -> subprocess.CompletedProcess:
    try:
        result = subprocess.run(cmd, cwd=None if cwd is None else str(cwd), env=env,
                                capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=timeout)
    except FileNotFoundError as error:
        raise Failure(f"cannot run {cmd[0]}: {error}") from error
    except subprocess.TimeoutExpired as error:
        raise Failure(f"timed out after {timeout}s: {' '.join(cmd)}") from error
    if check and result.returncode != 0:
        raise Failure(f"command failed ({result.returncode}): {' '.join(cmd)}\n"
                      f"{result.stderr.strip() or result.stdout.strip()}")
    return result


class Sandbox:
    """The throwaway clone covering the whole test run."""

    def __init__(self, work: Path, env: dict):
        self.work = work
        self.env = env
        self._git_dir: Path | None = None

    def git(self, *args: str, check: bool = True, timeout: int | None = None) -> str:
        return run(["git", *args], self.work, self.env, check=check, timeout=timeout).stdout

    def ok(self, *args: str) -> bool:
        return run(["git", *args], self.work, self.env, check=False).returncode == 0

    def rev(self, ref: str) -> str:
        return self.git("rev-parse", "--verify", f"{ref}^{{commit}}").strip()

    def tree(self, ref: str) -> str:
        return self.git("rev-parse", "--verify", f"{ref}^{{tree}}").strip()

    def tree_at(self, ref: str, path: str) -> str:
        """Object id of `path` inside `ref` (a directory entry, so a tree)."""
        return self.git("rev-parse", "--verify", f"{ref}:{path}").strip()

    def parents(self, rev: str = "HEAD") -> list[str]:
        return self.git("log", "--format=%P", "-1", rev).split()

    def count(self, rev_range: str, first_parent: bool = False) -> int:
        flags = ["--first-parent"] if first_parent else []
        return int(self.git("rev-list", "--count", *flags, rev_range).strip())

    def branch(self) -> str:
        return self.git("rev-parse", "--abbrev-ref", "HEAD").strip()

    def status(self) -> str:
        return self.git("status", "--porcelain", "--untracked-files=no").strip()

    def status_all(self) -> str:
        """Every change, untracked files included (the sync's own cleanliness rule)."""
        return self.git("status", "--porcelain", "--untracked-files=all").strip()

    def ls_files(self, *pathspec: str) -> list[str]:
        out = self.git("-c", "core.quotepath=false", "ls-files", "-z", *pathspec)
        return [entry for entry in out.split("\0") if entry]

    def gitlinks(self) -> list[str]:
        entries = []
        for line in self.git("ls-files", "-s", "-z").split("\0"):
            if not line:
                continue
            meta, path = line.split("\t", 1)
            if meta.startswith("160000"):
                entries.append(path)
        return entries

    def unmerged(self) -> dict[str, set[int]]:
        paths: dict[str, set[int]] = {}
        for line in self.git("ls-files", "-u", "-z").split("\0"):
            if not line:
                continue
            meta, path = line.split("\t", 1)
            paths.setdefault(path, set()).add(int(meta.split()[2]))
        return paths

    def name_status(self, older: str, newer: str, *pathspec: str) -> list[str]:
        out = self.git("-c", "core.quotepath=false", "diff", "--no-renames", "--name-status",
                       "-z", older, newer, "--", *pathspec)
        fields = [field for field in out.split("\0") if field]
        if len(fields) % 2:
            raise Failure(f"unexpected name-status output for {older}..{newer}: {fields}")
        return [f"{fields[i]}\t{fields[i + 1]}" for i in range(0, len(fields), 2)]

    def blob(self, ref: str) -> str:
        return self.git("cat-file", "blob", ref)

    def make_commit(self, parent: str | None, files: dict[str, str], message: str,
                    gitlink: str | None = None, tree_of: str | None = None,
                    gitlinks: dict[str, str] | None = None) -> str:
        """A commit built with plumbing, leaving HEAD, the index and the worktree alone.

        `files` (path -> text) is written on top of `parent` (or of `tree_of` when given,
        which makes an unrelated root commit with that tree). `gitlink` sets the `muse`
        gitlink, i.e. the framework pin of a synthetic upstream commit; `gitlinks` adds
        further gitlinks (path -> commit).
        """
        index = self.work.parent / "test-index"
        index.unlink(missing_ok=True)
        env = dict(self.env, GIT_INDEX_FILE=str(index))

        def plumbing(*args: str, stdin: bytes | None = None) -> str:
            result = subprocess.run(["git", "--literal-pathspecs", *args], cwd=str(self.work),
                                    env=env, input=stdin, capture_output=True)
            if result.returncode != 0:
                raise Failure(f"git {' '.join(args)} failed: "
                              f"{result.stderr.decode('utf-8', 'replace').strip()}")
            return result.stdout.decode("utf-8", "replace")

        plumbing("read-tree", tree_of or parent or "--empty")
        for path, content in files.items():
            blob = plumbing("hash-object", "-w", "--stdin", stdin=content.encode("utf-8")).strip()
            plumbing("update-index", "--add", "--cacheinfo", "100644", blob, path)
        for path, sha in ({"muse": gitlink} if gitlink else {}).items():
            plumbing("update-index", "--add", "--cacheinfo", "160000", sha, path)
        for path, sha in (gitlinks or {}).items():
            plumbing("update-index", "--add", "--cacheinfo", "160000", sha, path)
        tree = plumbing("write-tree").strip()
        args = ["commit-tree", tree, "-m", message]
        if parent:
            args += ["-p", parent]
        return plumbing(*args).strip()

    @property
    def git_dir(self) -> Path:
        if self._git_dir is None:
            self._git_dir = Path(self.git("rev-parse", "--absolute-git-dir").strip())
        return self._git_dir

    def state_file(self) -> Path:
        return self.git_dir / "sync-upstream.json"

    def sync_branches(self) -> list[str]:
        # `--format` prints no leading indent, so strip the whole short ref name.
        return [line.strip() for line in self.git("branch", "--list", "--format=%(refname:short)",
                                                 "sync/*").splitlines() if line.strip()]

    def path(self, relative: str) -> Path:
        return self.work / relative

    def exists(self, relative: str) -> bool:
        return (self.work / relative).exists()

    def read(self, relative: str) -> str:
        return self.git("cat-file", "blob", f"HEAD:{relative}")

    def state(self) -> dict:
        return json.loads(self.state_file().read_text(encoding="utf-8"))


class Context:
    def __init__(self, script: Path, work: Path, env: dict):
        self.script = script
        self.sb = Sandbox(work, env)
        self.base = ""
        self.local_edit_commit = ""
        # The framework commit the branch has incorporated; synthetic pins descend from it.
        self.framework_tip = FW_FINAL

    # -- assertions ----------------------------------------------------------
    def expect(self, condition: bool, what: str, detail: str = "") -> None:
        if not condition:
            raise Failure(f"{what}" + (f"\n  {detail}" if detail else ""))

    def expect_eq(self, actual, expected, what: str) -> None:
        if actual != expected:
            raise Failure(f"{what}\n  expected: {expected}\n  actual:   {actual}")

    # -- synthetic history ---------------------------------------------------
    def upstream_commit(self, files: dict[str, str] | None = None, framework: str | None = None,
                        message: str = "test: synthetic upstream commit",
                        ref: str = "extra/upstream") -> str:
        """A synthetic upstream commit on top of the pinned final upstream commit.

        Only the given files (and optionally the `muse` gitlink) change, so merging it
        into the branch brings exactly those changes. The branch `ref` makes it a
        target the script accepts as `--upstream-ref`.
        """
        sha = self.sb.make_commit(TARGET_FINAL, files or {}, message,
                                  gitlink=framework or self.framework_tip)
        self.sb.git("update-ref", f"refs/heads/{ref}", sha)
        return sha

    def framework_commit(self, files: dict[str, str], message: str = "test: synthetic framework",
                         parent: str | None = None) -> str:
        """A synthetic framework commit `parent` (default: the incorporated framework)."""
        sha = self.sb.make_commit(parent or self.framework_tip, files, message)
        self.sb.git("update-ref", "refs/heads/extra/framework", sha)
        return sha

    # -- driving the script under test ---------------------------------------
    def sync_synthetic(self, target: str, framework_ref: str | None = None, *,
                       expect: int = 0, stop_after: str | None = None,
                       env_overrides: dict[str, str | None] | None = None):
        """Run a sync against a synthetic upstream commit (`extra/upstream`).

        The framework mirror is `extra/framework`, pinned to the framework commit the
        branch has incorporated, so synthetic framework pins are reachable from it.
        """
        if framework_ref is None:
            self.sb.git("update-ref", "refs/heads/extra/framework", self.framework_tip)
            framework_ref = "extra/framework"
        return self.sync("--upstream-ref", "extra/upstream", "--framework-ref", framework_ref,
                         "sync", "--target", target, expect=expect, stop_after=stop_after,
                         env_overrides=env_overrides)

    def sync(self, *args: str, expect: int = 0, stop_after: str | None = None,
             env_overrides: dict[str, str | None] | None = None):
        env = dict(self.sb.env)
        for key, value in (env_overrides or {}).items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        if stop_after:
            env["SYNC_UPSTREAM_TEST_STOP_AFTER"] = stop_after
        cmd = [sys.executable, str(self.script), "-C", str(self.sb.work), *args]
        banner = " ".join(args) + (f"   [SYNC_UPSTREAM_TEST_STOP_AFTER={stop_after}]"
                                   if stop_after else "")
        print(f"  $ sync-upstream.py {banner}")
        started = time.time()
        try:
            result = subprocess.run(cmd, cwd=str(self.sb.work), env=env, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace",
                                    timeout=SYNC_TIMEOUT)
        except subprocess.TimeoutExpired as error:
            raise Failure(f"sync-upstream.py timed out after {SYNC_TIMEOUT}s: {banner}") from error
        for line in result.stdout.splitlines():
            print(f"    | {line}")
        for line in result.stderr.splitlines():
            print(f"    ! {line}")
        print(f"    -> exit {result.returncode} in {time.time() - started:.1f}s "
              f"(expected {expect})")
        if result.returncode != expect:
            hint = ""
            combined = f"{result.stdout}\n{result.stderr}".lower()
            if any(marker in combined for marker in
                   ("unable to read", "not our ref", "could not read from remote",
                    "promisor remote")):
                hint = ("\n  hint: the source clone is a blobless partial clone and lacks "
                        "blobs the sandbox needs; run one sync against it first so its object "
                        "store is complete, or fetch the missing objects manually.")
            raise Failure(f"expected exit {expect}, got {result.returncode} for `{banner}`"
                          f"\n  stdout:\n{indent(result.stdout)}\n  stderr:\n"
                          f"{indent(result.stderr)}{hint}")
        return result.returncode, result.stdout, result.stderr

    # -- shared assertions --------------------------------------------------
    def require_clean_end(self, step: str) -> None:
        sb = self.sb
        self.expect_eq(sb.status_all(), "", f"{step}: working tree is not clean")
        self.expect_eq(sb.branch(), "test/base", f"{step}: did not return to the sync branch")
        self.expect_eq(sb.sync_branches(), [], f"{step}: temporary branches left behind")
        self.expect(not sb.state_file().exists(), f"{step}: .git/sync-upstream.json left behind")
        self.expect_eq(sb.gitlinks(), [], f"{step}: gitlinks left in the index")

    def require_no_trimmed(self, step: str) -> None:
        sb = self.sb
        self.expect_eq(sb.ls_files("--", ".gitmodules", "muse_deps", "src/appshell"), [],
                       f"{step}: trimmed paths present in the index")
        self.expect(not sb.exists(".gitmodules"), f"{step}: .gitmodules still on disk")
        for path in ("muse_deps", "src/appshell"):
            if not sb.exists(path):
                continue
            leftovers = [str(item.relative_to(sb.work)).replace(os.sep, "/")
                         for item in sb.path(path).rglob("*") if item.is_file()]
            self.expect_eq(leftovers, [], f"{step}: files left under trimmed path {path}/")


def indent(text: str, prefix: str = "    ") -> str:
    return "\n".join(prefix + line for line in text.strip().splitlines()) or f"{prefix}<empty>"


def parse_report(out: str) -> dict[str, list[str]]:
    """Parse the report blocks `summary()` prints."""
    lines = out.splitlines()
    report: dict[str, list[str]] = {}
    index = 0
    while index < len(lines):
        for key, header in REPORT_HEADERS.items():
            if lines[index].startswith(header):
                raw = lines[index][len(header):].strip()
                try:
                    announced = int(raw)
                except ValueError as error:
                    raise Failure(f"report header for {key} is not a count: {lines[index]!r}") from error
                items = []
                index += 1
                while index < len(lines) and lines[index].startswith("  ") and lines[index].strip():
                    items.append(lines[index].strip())
                    index += 1
                if len(items) != announced:
                    raise Failure(f"report {key} announced {announced} entries but listed "
                                  f"{len(items)}: {items}")
                if key in report:
                    raise Failure(f"report {key} printed twice")
                report[key] = items
                break
        else:
            index += 1
    for key, header in REPORT_HEADERS.items():
        if key not in report:
            raise Failure(f"no report block '{header.strip()}' in the sync output")
    return report


# --- preflight ------------------------------------------------------------------

def git_read(repo: Path, env: dict, *args: str) -> str:
    return run(["git", "-C", str(repo), *args], None, env).stdout


def describe_script(repo: Path, script: Path, env: dict) -> str:
    """Say which revision of tools/sync-upstream.py the run exercises."""
    worktree_hash = git_read(repo, env, "hash-object", "--", str(script)).strip()
    head = run(["git", "-C", str(repo), "rev-parse", "HEAD:tools/sync-upstream.py"],
               None, env, check=False)
    if head.returncode != 0:
        return f"blob {worktree_hash[:12]}, not in HEAD"
    committed = head.stdout.strip()
    if committed == worktree_hash:
        return f"blob {committed[:12]} (same as HEAD)"
    return f"blob {worktree_hash[:12]}, DIFFERS from HEAD ({committed[:12]})"


def load_script(script: Path):
    """Import tools/sync-upstream.py as a module (it only defines things at import)."""
    spec = importlib.util.spec_from_file_location("sync_upstream_under_test", script)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as error:  # noqa: BLE001 - a broken script is a preflight failure
        raise Failure(f"cannot import {script}: {error}") from error
    return module


def check_framework_metadata(repo: Path, script: Path, env: dict) -> None:
    """The `Framework-Commit` trailer parser, including the pre-trailer absorb commit."""
    module = load_script(script)
    cases = [
        (f"Merge upstream/muse-framework {FW_FINAL} into muse/\n\nFramework-Commit: "
         f"{FW_FINAL}\n", FW_FINAL, "trailer"),
        (f"Absorb muse_framework {FW_FINAL} at muse/ (subtree)", FW_FINAL, "legacy subject"),
        (f"Absorb muse_framework {FW_FINAL} at muse/ (subtree)\n\nFramework-Commit: "
         f"{FW_LINK_BUMP}", FW_LINK_BUMP, "trailer wins over the subject"),
        ("Merge upstream/musescore 7b2d005464\n\nno trailer here", None, "no metadata"),
    ]
    for message, expected, label in cases:
        actual = module.framework_from_message(message)
        if actual != expected:
            raise Failure(f"framework metadata parser ({label}): expected {expected}, got "
                          f"{actual} for {message!r}")
    # main's first absorb commit 7150f2b05e predates the trailer; its subject must parse.
    if run(["git", "-C", str(repo), "cat-file", "-e", f"{ABSORB_COMMIT}^{{commit}}"],
           None, env, check=False).returncode == 0:
        message = git_read(repo, env, "log", "-1", "--format=%B", ABSORB_COMMIT)
        if not message.startswith("Absorb muse_framework "):
            raise Failure(f"{ABSORB_COMMIT[:12]} is not the absorb commit this fallback expects")
        if module.framework_from_message(message) != FW_FINAL:
            raise Failure(f"the legacy absorb commit {ABSORB_COMMIT[:12]} does not parse to "
                          f"{FW_FINAL}")
        legacy = "legacy absorb 7150f2b05e parsed"
    else:
        legacy = "legacy absorb commit absent (fallback covered by the unit cases)"
    print(f"  preflight: framework metadata parser ok ({legacy})")


def preflight(repo: Path, script: Path, env: dict) -> None:
    """Check the pinned history this test relies on, before building anything."""
    if not script.is_file():
        raise Failure(f"{script} does not exist")
    for ref in (UPSTREAM_REF, FRAMEWORK_REF):
        if run(["git", "-C", str(repo), "rev-parse", "--verify", "--quiet", ref],
               None, env, check=False).returncode != 0:
            raise Failure(f"{repo} has no {ref}; the test needs the upstream mirrors "
                          f"(origin/upstream/musescore and origin/upstream/muse-framework)")

    def is_ancestor(older: str, newer: str) -> bool:
        return run(["git", "-C", str(repo), "merge-base", "--is-ancestor", older, newer],
                   None, env, check=False).returncode == 0

    def gitlink(commit: str) -> str:
        out = git_read(repo, env, "ls-tree", commit, "--", "muse").strip()
        if not out:
            return ""
        meta, path = out.split("\t", 1)
        return meta.split()[2] if meta.startswith("160000") else ""

    problems = []
    for commit, framework, label in PINNED:
        if not is_ancestor(commit, UPSTREAM_REF):
            problems.append(f"{label}: {commit} is not on {UPSTREAM_REF}")
            continue
        if gitlink(commit) != framework:
            problems.append(f"{label}: {commit} pins muse={gitlink(commit) or '<none>'}, "
                            f"expected {framework}")
        if framework != FW_MISSING and not is_ancestor(framework, FRAMEWORK_REF):
            problems.append(f"{label}: framework {framework} is not on {FRAMEWORK_REF}")
    # Upstream order and framework order the steps rely on.
    order = [commit for commit, _fw, _label in PINNED if commit != TARGET_MISSING_FRAMEWORK]
    for older, newer in zip(order, order[1:]):
        if not is_ancestor(older, newer):
            problems.append(f"upstream order broken: {older} is not an ancestor of {newer}")
    framework_order = [FW_BASE, FW_LINK_BUMP, FW_CONFLICT, FW_FINAL]
    for older, newer in zip(framework_order, framework_order[1:]):
        if not is_ancestor(older, newer):
            problems.append(f"framework order broken: {older} is not an ancestor of {newer}")
    # The missing pin must really be missing, or step 6 asserts nothing.
    if run(["git", "-C", str(repo), "cat-file", "-e", f"{FW_MISSING}^{{commit}}"],
           None, env, check=False).returncode == 0:
        problems.append(f"{FW_MISSING} exists locally; step 6 (missing framework pin) "
                        f"would not test anything")
    # Preconditions of the conflict scenario in step 4.
    if run(["git", "-C", str(repo), "cat-file", "-e", f"{FW_LINK_BUMP}:{CONFLICT_FILE_IN_FRAMEWORK}"],
           None, env, check=False).returncode != 0:
        problems.append(f"{FW_LINK_BUMP} has no {CONFLICT_FILE_IN_FRAMEWORK}")
    else:
        before = git_read(repo, env, "cat-file", "blob",
                          f"{FW_LINK_BUMP}:{CONFLICT_FILE_IN_FRAMEWORK}")
        after = git_read(repo, env, "cat-file", "blob",
                         f"{FW_CONFLICT}:{CONFLICT_FILE_IN_FRAMEWORK}")
        if before.count(UPSTREAM_ANCHOR_PRESENT) != 1:
            problems.append(f"{FW_LINK_BUMP}:{CONFLICT_FILE_IN_FRAMEWORK} lost the "
                            f"'{UPSTREAM_ANCHOR_PRESENT}' insertion point")
        if before.count(UPSTREAM_ANCHOR_ABSENT) or after.count(UPSTREAM_ANCHOR_ABSENT) != 1:
            problems.append(f"{FW_CONFLICT}:{CONFLICT_FILE_IN_FRAMEWORK} no longer adds exactly "
                            f"one '{UPSTREAM_ANCHOR_ABSENT}' block")
    # `.gitmodules` must be a file (trimmed by exact path), not a gitlink.
    if not git_read(repo, env, "ls-tree", BASE, "--", ".gitmodules").startswith("100644"):
        problems.append(f"{BASE} has no .gitmodules file to trim")
    # The report expectations must match the trimmed diffs the steps merge.
    expected = [
        ("step 2", BASE, TARGET_LINK_BUMP, STEP2_DISCARDED, []),
        ("step 3", TARGET_LINK_BUMP, TARGET_NO_LINK_BUMP, [], []),
        ("step 4", TARGET_NO_LINK_BUMP, TARGET_FRAMEWORK_CONFLICT, STEP4_DISCARDED, []),
        ("step 5", TARGET_FRAMEWORK_CONFLICT, TARGET_INTERRUPTED, STEP5_DISCARDED, STEP5_ADDED),
        ("step 6", TARGET_INTERRUPTED, TARGET_MISSING_FRAMEWORK, None, None),
        # step 6 merges nothing, so the next real merge starts at step 5's target
        ("step 7", TARGET_INTERRUPTED, TARGET_FINAL, STEP7_DISCARDED, []),
    ]
    for label, older, newer, discarded, dropped in expected:
        if discarded is None:
            continue
        out = git_read(repo, env, "-c", "core.quotepath=false", "diff", "--no-renames",
                       "--name-status", "-z", older, newer, "--",
                       ".gitmodules", "muse_deps", "src/appshell")
        fields = [field for field in out.split("\0") if field]
        pairs = [(fields[i], fields[i + 1]) for i in range(0, len(fields) - 1, 2)]
        modified = sorted(path for status, path in pairs if status == "M")
        added = sorted(path for status, path in pairs if status == "A")
        if sorted(discarded) != modified:
            problems.append(f"{label}: trimmed modifications differ from the pinned report "
                            f"expectation\n  history:    {modified}\n  expectation: {sorted(discarded)}")
        if sorted(dropped) != added:
            problems.append(f"{label}: trimmed additions differ from the pinned report "
                            f"expectation\n  history:    {added}\n  expectation: {sorted(dropped)}")
    if problems:
        raise Failure("pinned history does not match this test's expectations:\n  "
                      + "\n  ".join(problems))
    check_framework_metadata(repo, script, env)
    print(f"  preflight: {len(PINNED)} pins verified on {UPSTREAM_REF}/{FRAMEWORK_REF}, "
          f"{FW_MISSING[:12]} absent, report expectations match history")


# --- sandbox setup --------------------------------------------------------------

def create_sandbox(repo: Path, tmp: Path, env: dict) -> Path:
    work = tmp / "work"
    print(f"  cloning {repo} -> {work} (shared, no checkout)")
    run(["git", "clone", "--quiet", "--shared", "--no-checkout", str(repo), str(work)],
        None, env, timeout=600)
    git = ["git", "-C", str(work)]
    for key, value in (("core.eol", "lf"), ("core.autocrlf", "false"),
                       ("core.longpaths", "true"), ("core.symlinks", "true"),
                       ("core.fsmonitor", "false"), ("gc.auto", "0"),
                       ("maintenance.auto", "false"), ("commit.gpgsign", "false"),
                       ("user.name", "Sync Upstream Test"),
                       ("user.email", "sync-upstream-test@example.invalid")):
        run([*git, "config", "--local", key, value], None, env)
    # The clone copies branches but not mirror refs; `sync-upstream.py` expects them.
    for ref in (f"refs/remotes/{UPSTREAM_REF}", f"refs/remotes/{FRAMEWORK_REF}"):
        run([*git, "fetch", "--quiet", "--no-tags", "--no-write-fetch-head", str(repo),
             f"+{ref}:{ref}"], None, env, timeout=1800)
    for ref in (UPSTREAM_REF, FRAMEWORK_REF):
        run([*git, "rev-parse", "--verify", "--quiet", ref], None, env)
    return work


# --- steps ----------------------------------------------------------------------

def step_absorb(ctx: Context) -> None:
    sb = ctx.sb
    sb.git("checkout", "--quiet", "-b", "test/base", BASE)
    ctx.expect_eq(sb.rev("HEAD"), BASE, "sandbox base is not the pinned upstream commit")
    sb.path("TRIM").write_bytes(TRIM.encode("utf-8"))
    sb.git("add", "--", "TRIM")
    sb.git("commit", "--quiet", "-m", "TRIM (test only)")
    ctx.base = sb.rev("HEAD")
    ctx.expect_eq(sorted(sb.parents()), [BASE], "TRIM commit is not based on the pinned commit")
    ctx.expect_eq(sb.name_status("HEAD~1", "HEAD"), ["A\tTRIM"], "TRIM commit content")

    # (A) absorb refuses to start when it could lose work: a file inside the `muse`
    # gitlink directory (git status does not look there) and an ignored file under a
    # trimmed prefix.
    precious = "muse/precious.txt"
    sb.path("muse").mkdir(exist_ok=True)
    sb.path(precious).write_bytes(b"do not delete me\n")
    _rc, _out, err = ctx.sync("absorb", expect=1)
    ctx.expect("muse/" in err and "already holds files" in err,
               "step 1: absorb did not refuse the untracked file inside muse/", err)
    ctx.expect_eq(sb.rev("HEAD"), ctx.base, "step 1: a refused absorb moved the branch")
    ctx.expect(sb.path(precious).exists(), "step 1: a refused absorb deleted the file")
    ctx.expect(not (sb.git_dir / "MERGE_HEAD").exists(),
               "step 1: a refused absorb left a merge in progress")
    sb.path(precious).unlink()
    ignored = "src/appshell/ignored-local.txt"
    sb.path(ignored).write_bytes(b"ignored but present\n")
    exclude = sb.git_dir / "info" / "exclude"
    exclude.parent.mkdir(exist_ok=True)
    exclude.write_bytes(b"src/appshell/ignored-local.txt\n")
    ctx.expect_eq(sb.status_all(), "", "step 1: the ignore rule did not hide the file")
    _rc, _out, err = ctx.sync("absorb", expect=1)
    ctx.expect("ignored files" in err and ignored in err,
               "step 1: absorb did not refuse the ignored file under a trimmed prefix", err)
    ctx.expect(sb.path(ignored).exists(), "step 1: a refused absorb deleted the ignored file")
    ctx.expect_eq(sb.rev("HEAD"), ctx.base, "step 1: the second refusal moved the branch")
    sb.path(ignored).unlink()
    exclude.write_bytes(b"")

    _rc, out, _err = ctx.sync("absorb", expect=0)
    ctx.expect(f"absorbed {FW_BASE} at muse/" in out,
               "absorb did not report the pinned framework commit", out)

    ctx.expect_eq(sorted(sb.parents()), sorted([ctx.base, FW_BASE]),
                  "absorb is not a merge commit with the framework as second parent")
    ctx.expect_eq(sb.tree_at("HEAD", "muse"), sb.tree(FW_BASE),
                  "muse/ is not the absorbed framework tree")
    message = sb.git("log", "-1", "--format=%B", "HEAD")
    ctx.expect(f"Framework-Commit: {FW_BASE}" in message,
               "step 1: the absorb commit has no Framework-Commit trailer", message)
    ctx.expect(sb.exists("muse"), "muse/ directory missing after absorb")
    ctx.expect(not sb.exists(".gitmodules"), ".gitmodules survived absorb")
    ctx.require_no_trimmed("step 1")
    ctx.require_clean_end("step 1")


def step_link_bump(ctx: Context) -> None:
    sb = ctx.sb
    previous = sb.rev("HEAD")
    _rc, out, _err = ctx.sync("sync", "--target", TARGET_LINK_BUMP, expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["discardedChanges"], STEP2_DISCARDED,
                  "step 2: discarded changes are not the trimmed upstream edits")
    ctx.expect_eq(report["droppedAdditions"], [],
                  "step 2: unexpected dropped additions")
    head = sb.rev("HEAD")
    ctx.expect_eq(sb.count(f"{previous}..{head}", first_parent=True), 2,
                  "step 2: expected exactly two new commits (upstream + framework merge)")
    upstream_merge = sb.parents(head)[0]
    ctx.expect_eq(sb.parents(head), [upstream_merge, FW_LINK_BUMP],
                  "step 2: framework merge does not have the pinned framework as second parent")
    ctx.expect_eq(sb.parents(upstream_merge), [previous, TARGET_LINK_BUMP],
                  "step 2: upstream merge does not have the target as second parent")
    ctx.expect_eq(sb.tree_at("HEAD", "muse"), sb.tree(FW_LINK_BUMP),
                  "step 2: muse/ is not the newly pinned framework tree")
    ctx.require_no_trimmed("step 2")
    ctx.require_clean_end("step 2")


def step_no_link_bump(ctx: Context) -> None:
    sb = ctx.sb
    previous = sb.rev("HEAD")
    _rc, out, _err = ctx.sync("sync", "--target", TARGET_NO_LINK_BUMP, expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["discardedChanges"], [],
                  "step 3: reported discarded changes but the range touches no trimmed path")
    ctx.expect_eq(report["droppedAdditions"], [],
                  "step 3: unexpected dropped additions")
    head = sb.rev("HEAD")
    ctx.expect_eq(sb.count(f"{previous}..{head}", first_parent=True), 1,
                  "step 3: an unchanged gitlink must not produce a framework merge")
    ctx.expect_eq(sb.parents(head), [previous, TARGET_NO_LINK_BUMP],
                  "step 3: expected one upstream merge commit")
    ctx.expect_eq(sb.tree_at("HEAD", "muse"), sb.tree(FW_LINK_BUMP),
                  "step 3: muse/ changed although the gitlink did not move")
    ctx.require_clean_end("step 3")


def step_framework_conflict(ctx: Context) -> None:
    sb = ctx.sb
    target_file = sb.path(CONFLICT_FILE)
    text = target_file.read_bytes().decode("utf-8")
    ctx.expect_eq(text.count(UPSTREAM_ANCHOR_PRESENT), 1,
                  f"step 4: {CONFLICT_FILE} has no unique '{UPSTREAM_ANCHOR_PRESENT}' anchor")
    lines = text.splitlines(keepends=True)
    anchor = next(index for index, line in enumerate(lines)
                  if line.strip() == UPSTREAM_ANCHOR_PRESENT)
    lines.insert(anchor, LOCAL_EDIT + "\n")
    target_file.write_bytes("".join(lines).encode("utf-8"))
    sb.git("add", "--", CONFLICT_FILE)
    sb.git("commit", "--quiet", "-m", "test: local framework delta in cloud CMakeLists")
    ctx.local_edit_commit = sb.rev("HEAD")
    previous = ctx.local_edit_commit
    print(f"  local framework delta committed as {ctx.local_edit_commit[:12]}")

    _rc, out, _err = ctx.sync("sync", "--target", TARGET_FRAMEWORK_CONFLICT, expect=2)
    ctx.expect("framework conflicts in muse/" in out,
               "step 4: the script did not report a framework conflict", out)
    unmerged = sb.unmerged()
    ctx.expect_eq(sorted(unmerged), [CONFLICT_FILE],
                  "step 4: conflicts are not exactly the colliding framework file")
    ctx.expect(all(path.startswith("muse/") for path in unmerged),
               "step 4: a conflict appeared outside muse/ (the -Xno-renames pairing bug)",
               f"unmerged: {sorted(unmerged)}")
    ctx.expect(not any(path.startswith(("src/appshell/", "muse_deps")) for path in unmerged),
               "step 4: trimmed upstream files were paired with framework files as renames",
               f"unmerged: {sorted(unmerged)}")
    printed = parse_list_after(out, "framework conflicts in muse/")
    ctx.expect_eq(printed, sorted(unmerged),
                  "step 4: the reported conflict list differs from the index")
    upstream_merge = sb.rev("HEAD")
    ctx.expect_eq(sb.parents(upstream_merge), [previous, TARGET_FRAMEWORK_CONFLICT],
                  "step 4: the upstream merge was not committed before the framework merge")
    ctx.expect_eq(sb.branch(), f"sync/{TARGET_FRAMEWORK_CONFLICT[:12]}",
                  "step 4: the sync is not on its temporary branch")
    ctx.expect(sb.state_file().exists(), "step 4: no sync state while a conflict is open")

    # Resolve keeping both sides, exactly as a human would.
    conflicted = target_file.read_bytes().decode("utf-8")
    ctx.expect("<<<<<<<" in conflicted and ">>>>>>>" in conflicted,
               "step 4: the file has no conflict markers to resolve")
    resolved = resolve_keep_both(conflicted)
    if not resolved.endswith("\n"):
        resolved += "\n"
    ctx.expect(not any(marker in resolved for marker in ("<<<<<<<", ">>>>>>>")),
               "step 4: resolution left conflict markers behind")
    target_file.write_bytes(resolved.encode("utf-8"))
    expected = sb.blob(f"{FW_CONFLICT}:{CONFLICT_FILE_IN_FRAMEWORK}")
    expected = expected.replace(UPSTREAM_ANCHOR_ABSENT, LOCAL_EDIT + "\n" + UPSTREAM_ANCHOR_ABSENT, 1)
    ctx.expect_eq(resolved, expected,
                  "step 4: the resolution is not the upstream file with the local line kept")
    ctx.expect(LOCAL_EDIT in resolved and UPSTREAM_ANCHOR_ABSENT in resolved,
               "step 4: resolution dropped one of the two sides")
    # (D) The human only stages the resolution; the tool makes the merge commit.
    sb.git("add", "--", CONFLICT_FILE)
    _rc, out, _err = ctx.sync("continue", expect=0)
    ctx.expect_eq(sb.parents(), [upstream_merge, FW_CONFLICT],
                  "step 4: the resolution commit is not a merge of the pinned framework")
    message = sb.git("log", "-1", "--format=%B", "HEAD")
    ctx.expect(f"Framework-Commit: {FW_CONFLICT}" in message,
               "step 4: the tool's framework merge commit has no Framework-Commit trailer",
               message)
    report = parse_report(out)
    ctx.expect_eq(report["discardedChanges"], STEP4_DISCARDED,
                  "step 4: discarded changes are not the trimmed upstream edits")
    ctx.expect_eq(report["droppedAdditions"], [],
                  "step 4: unexpected dropped additions")
    ctx.expect_eq(report["resolutions"], [CONFLICT_FILE],
                  "step 4: the report does not list the path the human resolved")
    ctx.expect_eq(sb.read(CONFLICT_FILE), expected,
                  "step 4: the committed file does not keep both lines")
    ctx.require_clean_end("step 4")


def parse_list_after(out: str, header_prefix: str) -> list[str]:
    """Items of the two-space-indented list printed after `header_prefix`."""
    items: list[str] = []
    collecting = False
    for line in out.splitlines():
        if not collecting:
            collecting = line.startswith(header_prefix)
            continue
        if line.startswith("  ") and line.strip():
            items.append(line.strip())
        else:
            break
    return items


def resolve_keep_both(text: str) -> str:
    """Concatenate the `ours` and `theirs` sides of every conflict block."""
    resolved: list[str] = []
    side: str | None = None
    for line in text.splitlines(keepends=True):
        if line.startswith("<<<<<<< "):
            side = "ours"
            continue
        if side == "ours" and line.startswith("======="):
            side = "theirs"
            continue
        if side == "theirs" and line.startswith(">>>>>>> "):
            side = None
            continue
        resolved.append(line)
    return "".join(resolved)


def step_interrupted(ctx: Context) -> None:
    sb = ctx.sb
    previous = sb.rev("HEAD")
    temp_branch = f"sync/{TARGET_INTERRUPTED[:12]}"

    _rc, _out, err = ctx.sync("sync", "--target", TARGET_INTERRUPTED, expect=1,
                              stop_after="upstream-merge")
    ctx.expect("[test] stopped after upstream-merge" in err,
               "step 5: the upstream-merge stop hook did not fire", err)
    ctx.expect(sb.state_file().exists(), "step 5: no sync state after the interruption")
    ctx.expect_eq(sb.branch(), temp_branch, "step 5: interruption left the wrong branch")
    ctx.expect((sb.git_dir / "MERGE_HEAD").exists(),
               "step 5: interruption did not leave the upstream merge unfinished")

    _rc, out, err = ctx.sync("sync", "--target", TARGET_INTERRUPTED, expect=1,
                             stop_after="upstream-commit")
    ctx.expect("found an unfinished sync; aborting it first" in out,
               "step 5: the rerun did not abort the leftover sync first", out)
    ctx.expect("[test] stopped after upstream-commit" in err,
               "step 5: the upstream-commit stop hook did not fire", err)
    ctx.expect(sb.state_file().exists(), "step 5: no sync state after the second stop")
    ctx.expect(not (sb.git_dir / "MERGE_HEAD").exists(),
               "step 5: the upstream merge should be committed at this stage")
    ctx.expect_eq(sb.parents(), [previous, TARGET_INTERRUPTED],
                  "step 5: the interrupted run did not commit the upstream merge")
    ctx.expect_eq(sb.branch(), temp_branch, "step 5: second run is on the wrong branch")

    _rc, out, _err = ctx.sync("continue", expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["discardedChanges"], STEP5_DISCARDED,
                  "step 5: discarded changes are not the trimmed upstream edits")
    ctx.expect_eq(report["droppedAdditions"], STEP5_ADDED,
                  "step 5: new upstream files under src/appshell were not reported as dropped")
    for path in STEP5_ADDED:
        ctx.expect(not sb.exists(path), f"step 5: dropped addition {path} is still in the tree")
    ctx.expect_eq(sb.ls_files("--", "src/appshell"), [],
                  "step 5: dropped additions are still in the index")
    head = sb.rev("HEAD")
    ctx.expect_eq(sb.count(f"{previous}..{head}", first_parent=True), 1,
                  "step 5: an unchanged gitlink must not produce a framework merge")
    ctx.expect_eq(sb.parents(head), [previous, TARGET_INTERRUPTED],
                  "step 5: the continued sync did not fast-forward to the upstream merge")
    ctx.require_clean_end("step 5")


def step_missing_framework(ctx: Context) -> None:
    sb = ctx.sb
    head, tree, branch = sb.rev("HEAD"), sb.tree("HEAD"), sb.branch()
    index = sb.git("ls-files", "-s", "-z")
    _rc, _out, err = ctx.sync("sync", "--target", TARGET_MISSING_FRAMEWORK, expect=1)
    ctx.expect(f"pins framework {FW_MISSING}" in err,
               "step 6: the error does not name the missing framework pin", err)
    ctx.expect(f"which is not on {FRAMEWORK_REF}" in err,
               "step 6: the error does not name the framework ref it checked", err)
    ctx.expect_eq(sb.rev("HEAD"), head, "step 6: a failed sync moved the branch")
    ctx.expect_eq(sb.tree("HEAD"), tree, "step 6: a failed sync changed the tree")
    ctx.expect_eq(sb.branch(), branch, "step 6: a failed sync changed the branch")
    ctx.expect_eq(sb.git("ls-files", "-s", "-z"), index, "step 6: a failed sync touched the index")
    ctx.require_clean_end("step 6")


def step_final(ctx: Context) -> None:
    sb = ctx.sb
    previous = sb.rev("HEAD")
    _rc, out, _err = ctx.sync("sync", "--target", TARGET_FINAL, expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["discardedChanges"], STEP7_DISCARDED,
                  "step 7: discarded changes are not the trimmed upstream edits")
    ctx.expect_eq(report["droppedAdditions"], [],
                  "step 7: unexpected dropped additions")
    head = sb.rev("HEAD")
    ctx.expect_eq(sb.count(f"{previous}..{head}", first_parent=True), 2,
                  "step 7: expected upstream and framework merge commits")
    upstream_merge = sb.parents(head)[0]
    ctx.expect_eq(sb.parents(head), [upstream_merge, FW_FINAL],
                  "step 7: framework merge does not end at the final framework commit")
    ctx.expect_eq(sb.parents(upstream_merge), [previous, TARGET_FINAL],
                  "step 7: upstream merge does not end at upstream main")

    diff = sb.name_status(TARGET_FINAL, "HEAD", ".", ":!muse", ":!src/appshell")
    ctx.expect_eq(diff, FINAL_DIFF,
                  "step 7: the tree relative to upstream main is not just TRIM + two gitlinks")
    framework_delta = sb.name_status(sb.tree(FW_FINAL), sb.tree_at("HEAD", "muse"), ".")
    ctx.expect_eq(framework_delta, [f"M\t{CONFLICT_FILE_IN_FRAMEWORK}"],
                  "step 7: muse/ differs from the final framework by more than the local edit")
    delta = sb.git("diff", "--no-renames", "-U0", sb.tree(FW_FINAL), sb.tree_at("HEAD", "muse"), "--",
                   CONFLICT_FILE_IN_FRAMEWORK)
    added = [line for line in delta.splitlines() if line.startswith("+") and not line.startswith("+++")]
    removed = [line for line in delta.splitlines() if line.startswith("-") and not line.startswith("---")]
    ctx.expect_eq(added, ["+" + LOCAL_EDIT],
                  "step 7: the local framework delta is not the single kept line")
    ctx.expect_eq(removed, [], "step 7: the local framework delta removed a framework line")
    ctx.require_no_trimmed("step 7")
    ctx.require_clean_end("step 7")


def step_idempotent(ctx: Context) -> None:
    sb = ctx.sb
    head, tree, index = sb.rev("HEAD"), sb.tree("HEAD"), sb.git("ls-files", "-s", "-z")
    _rc, out, _err = ctx.sync("sync", "--target", TARGET_FINAL, expect=0)
    ctx.expect(f"already contains {TARGET_FINAL}" in out,
               "step 8: the rerun did not report that the branch already contains the target", out)
    ctx.expect_eq(sb.rev("HEAD"), head, "step 8: the rerun moved the branch")
    ctx.expect_eq(sb.tree("HEAD"), tree, "step 8: the rerun changed the tree")
    ctx.expect_eq(sb.git("ls-files", "-s", "-z"), index, "step 8: the rerun changed the index")
    ctx.require_clean_end("step 8")


# --- steps on synthetic history -------------------------------------------------

UNICODE_ADDED = [
    "src/appshell/odd dir/caf\u00e9 \u2713.txt",
    "src/appshell/odd dir/\u00f1and\u00fa \u03c0.txt",
]
COLLIDING_LINK_NAME = "muse~deadbeef"
BOUNDARIES = ["start", "temp-branch", "upstream-merge", "upstream-commit",
              "framework-merge", "check", "fast-forward"]


def expect_refused(ctx: Context, step: str, before: tuple[str, str, str]) -> None:
    """A refused command must not touch the branch, the tree or the index."""
    sb = ctx.sb
    head, tree, index = before
    ctx.expect_eq(sb.rev("HEAD"), head, f"{step}: a refused command moved the branch")
    ctx.expect_eq(sb.tree("HEAD"), tree, f"{step}: a refused command changed the tree")
    ctx.expect_eq(sb.git("ls-files", "-s", "-z"), index,
                  f"{step}: a refused command touched the index")


def step_trim_policy(ctx: Context) -> None:
    """A target that adds the root TRIM file is refused before anything is created."""
    sb = ctx.sb
    before = (sb.rev("HEAD"), sb.tree("HEAD"), sb.git("ls-files", "-s", "-z"))
    target = ctx.upstream_commit({"TRIM": "# upstream must not own this file\n"},
                                 message="test: upstream adds a TRIM file")
    _rc, _out, err = ctx.sync_synthetic(target, expect=1)
    ctx.expect(f"{target[:12]} adds a root TRIM" in err,
               "step 9: the refusal does not report the upstream TRIM addition", err)
    expect_refused(ctx, "step 9", before)
    ctx.expect_eq(sb.sync_branches(), [], "step 9: a temporary branch was created")
    ctx.expect(not sb.state_file().exists(), "step 9: a state file was written")
    ctx.require_clean_end("step 9")


def step_trimmed_unicode(ctx: Context) -> None:
    """Paths with spaces and non-ASCII under a trimmed prefix are dropped and reported."""
    sb = ctx.sb
    previous = sb.rev("HEAD")
    target = ctx.upstream_commit({path: f"upstream content of {path}\n" for path in UNICODE_ADDED},
                                 message="test: upstream adds trimmed-prefix files")
    _rc, out, _err = ctx.sync_synthetic(target, expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["droppedAdditions"], sorted(UNICODE_ADDED),
                  "step 10: trimmed additions are not reported verbatim (quoting or -z bug)")
    ctx.expect_eq(report["discardedChanges"], [],
                  "step 10: unexpected discarded changes")
    for path in UNICODE_ADDED:
        ctx.expect(not sb.exists(path), f"step 10: dropped addition {path!r} is still on disk")
    ctx.expect_eq(sb.ls_files("--", "src/appshell"), [],
                  "step 10: dropped additions are still in the index")
    ctx.expect_eq(sb.parents(sb.rev("HEAD")), [previous, target],
                  "step 10: the sync did not produce a single upstream merge")
    ctx.require_no_trimmed("step 10")
    ctx.require_clean_end("step 10")


def step_link_name_collision(ctx: Context) -> None:
    """A regular file at a `muse~...` name is a conflict for a human, never dropped."""
    sb = ctx.sb
    before = sb.rev("HEAD")
    sb.path(COLLIDING_LINK_NAME).write_bytes(b"local file that looks like the link conflict\n")
    sb.git("add", "--", COLLIDING_LINK_NAME)
    sb.git("commit", "--quiet", "-m", "test: local file named like the muse~ link conflict")
    local = sb.rev("HEAD")
    target = ctx.upstream_commit({COLLIDING_LINK_NAME: "upstream file with the same name\n"},
                                 message="test: upstream adds a muse~ file")
    _rc, out, _err = ctx.sync_synthetic(target, expect=2)
    ctx.expect_eq(sorted(sb.unmerged()), [COLLIDING_LINK_NAME],
                  "step 11: the colliding file was dropped from the index instead of conflicting")
    printed = parse_list_after(out, "conflicts that need a human")
    ctx.expect_eq(printed, [COLLIDING_LINK_NAME],
                  "step 11: the reported conflict list differs from the index")
    ctx.expect(sb.state_file().exists(), "step 11: no state while the conflict is open")
    ctx.expect_eq(sb.branch(), f"sync/{target[:12]}",
                  "step 11: the sync is not on its temporary branch")
    _rc, _out, _err = ctx.sync("abort", expect=0)
    ctx.expect_eq(sb.rev("HEAD"), local, "step 11: abort did not return to the branch tip")
    ctx.expect(sb.exists(COLLIDING_LINK_NAME),
               "step 11: abort lost the local file the branch had")
    # Roll the test's own commit back so later steps start from the synced tree.
    sb.git("reset", "--quiet", "--hard", before)
    ctx.expect(not sb.exists(COLLIDING_LINK_NAME), "step 11: the rollback kept the file")
    ctx.require_clean_end("step 11")


def step_framework_side_parent(ctx: Context) -> None:
    """A framework commit merged through a side parent must not become the merge base."""
    sb = ctx.sb
    before = sb.rev("HEAD")
    recorded = ctx.framework_tip
    side_framework = ctx.framework_commit(
        {"extra/side-reachable.txt": "merged into the branch from a side parent\n"},
        message="test: framework commit merged elsewhere")
    sb.git("merge", "--quiet", "--no-ff", "-s", "ort", "-Xsubtree=muse", "-m",
           f"test: side merge of {side_framework}", side_framework)
    side = sb.rev("HEAD")
    ctx.expect_eq(sb.parents(), [before, side_framework],
                  "step 12: the side merge is not a merge of the synthetic framework")
    ctx.expect_eq(sb.git("merge-base", "HEAD", side_framework, check=False).strip(),
                  side_framework,
                  "step 12: git no longer sees the side-merged framework as the merge base")
    pinned = ctx.framework_commit({"extra/after-side.txt": "a later framework commit\n"},
                                  message="test: framework commit after the side merge",
                                  parent=side_framework)
    target = ctx.upstream_commit({"src/extra/after-side.txt": "upstream change\n"},
                                 framework=pinned,
                                 message="test: pin the framework commit after the side merge")
    _rc, _out, err = ctx.sync_synthetic(target, "extra/framework", expect=1)
    ctx.expect("Framework-Commit" in err and "merge base" in err,
               "step 12: the refusal does not explain the framework merge base", err)
    ctx.expect(f"{recorded[:12]}" in err and f"{side_framework[:12]}" in err,
               "step 12: the refusal does not name the recorded base and the wrong base", err)
    state = sb.state()
    ctx.expect_eq(state["previousFramework"], recorded,
                  "step 12: side-parent reachability changed the recorded framework base")
    ctx.expect_eq(state["framework"], pinned, "step 12: the state lost the pinned framework")
    ctx.expect(sb.state_file().exists(), "step 12: no state after the refusal")
    _rc, _out, _err = ctx.sync("abort", expect=0)
    ctx.expect_eq(sb.rev("HEAD"), side, "step 12: abort did not return to the branch")
    sb.git("reset", "--quiet", "--hard", before)
    sb.git("update-ref", "-d", "refs/heads/extra/framework")
    ctx.framework_tip = recorded
    ctx.require_clean_end("step 12")


def step_framework_rewritten(ctx: Context) -> None:
    """A mirror that no longer contains the recorded framework commit is refused."""
    sb = ctx.sb
    before = (sb.rev("HEAD"), sb.tree("HEAD"), sb.git("ls-files", "-s", "-z"))
    recorded = ctx.framework_tip
    # Same content, but no ancestry: a rewritten (force-pushed) mirror.
    rewritten = sb.make_commit(None, {}, "test: rewritten framework history",
                               tree_of=recorded)
    sb.git("update-ref", "refs/heads/extra/rewritten-framework", rewritten)
    target = ctx.upstream_commit({"src/extra/rewritten.txt": "pinned a rewritten framework\n"},
                                 framework=rewritten,
                                 message="test: pin a framework commit from a rewritten mirror")
    _rc, _out, err = ctx.sync_synthetic(target, "extra/rewritten-framework", expect=1)
    ctx.expect("rewritten or diverged" in err,
               "step 13: the rewritten mirror is not reported as rewritten", err)
    ctx.expect(f"{recorded[:12]}" in err and f"{rewritten[:12]}" in err,
               "step 13: the refusal does not name both framework commits", err)
    expect_refused(ctx, "step 13", before)
    ctx.expect_eq(sb.sync_branches(), [], "step 13: a temporary branch was created")
    ctx.require_clean_end("step 13")


def step_fatal_merge(ctx: Context) -> None:
    """A fatal merge error stops, keeps the state, and `continue` can retry it."""
    sb = ctx.sb
    before = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/locked-index.txt": "upstream change\n"},
                                 message="test: target for the fatal merge")
    temp = f"sync/{target[:12]}"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="temp-branch")
    ctx.expect_eq(sb.branch(), temp, "step 14: the interruption left the wrong branch")
    # Make the merge command itself fail the way a concurrent git process would.
    lock = sb.git_dir / "index.lock"
    lock.write_bytes(b"")
    _rc, _out, err = ctx.sync("continue", expect=1)
    ctx.expect("git merge failed" in err,
               "step 14: the fatal merge error was not reported", err)
    ctx.expect("continue" in err and "abort" in err,
               "step 14: the error does not say how to recover", err)
    ctx.expect("index.lock" in err or "Unable to write index" in err,
               "step 14: the error does not carry git's own message", err)
    ctx.expect(sb.state_file().exists(), "step 14: the state was dropped after a fatal merge")
    ctx.expect_eq(sb.branch(), temp, "step 14: a fatal merge changed the branch")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 14: a fatal merge moved the temp branch")
    ctx.expect(not (sb.git_dir / "MERGE_HEAD").exists(),
               "step 14: a fatal merge left a merge in progress")
    ctx.expect(sb.state()["stage"] == "temp-branch",
               "step 14: the state advanced past the failed merge")
    ctx.expect_eq(sb.state()["mergeOutcome"], "failed",
                  "step 14: the state does not record the failed merge")
    ctx.expect_eq(sb.status(), "", "step 14: the failed merge dirtied the worktree")
    # The obstruction is gone: `continue` retries the merge and finishes.
    lock.unlink()
    _rc, out, _err = ctx.sync("continue", expect=0)
    ctx.expect(sb.rev("HEAD") != before, "step 14: continue did not merge anything")
    ctx.expect_eq(sb.parents(), [before, target],
                  "step 14: the retried merge is not an upstream merge")
    ctx.expect(sb.read("src/extra/locked-index.txt") == "upstream change\n",
               "step 14: the merged file is not upstream's version")
    ctx.require_clean_end("step 14")


def step_manual_merge_abort(ctx: Context) -> None:
    """A manual `git merge --abort` before `continue` must not be taken as resolved."""
    sb = ctx.sb
    before = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/manual-abort.txt": "upstream change\n"},
                                 message="test: target for the manual abort")
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="upstream-merge")
    ctx.expect(sb.state_file().exists(), "step 15: no state after the interruption")
    ctx.expect((sb.git_dir / "MERGE_HEAD").exists(),
               "step 15: the interruption did not leave the merge in progress")
    sb.git("merge", "--abort")
    ctx.expect(not (sb.git_dir / "MERGE_HEAD").exists(), "step 15: git merge --abort failed")
    _rc, _out, err = ctx.sync("continue", expect=1)
    ctx.expect("no longer in progress" in err,
               "step 15: continue did not notice the aborted merge", err)
    ctx.expect_eq(sb.rev("HEAD"), before,
                  "step 15: continue committed after a manual `git merge --abort`")
    ctx.expect_eq(sb.count(f"{before}..HEAD"), 0,
                  "step 15: continue created a commit after a manual abort")
    ctx.expect(sb.state_file().exists(), "step 15: the state was dropped by the refusal")
    _rc, _out, _err = ctx.sync("abort", expect=0)
    ctx.expect_eq(sb.rev("HEAD"), before, "step 15: abort did not return to the branch tip")
    ctx.require_clean_end("step 15")


def step_abort_refusals(ctx: Context) -> None:
    """`abort` only touches the merge this sync started."""
    sb = ctx.sb
    before = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/abort-guard.txt": "upstream change\n"},
                                 message="test: target for the abort guard")
    temp = f"sync/{target[:12]}"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="temp-branch")
    ctx.expect_eq(sb.branch(), temp, "step 16: the interruption did not leave the temp branch")
    ctx.expect(sb.state_file().exists(), "step 16: no state after the interruption")

    # A merge this sync did not start is in progress on the temporary branch. It happens
    # to merge the same commit the sync would merge, so only the recorded intent can tell
    # them apart (item F).
    sb.git("merge", "--quiet", "--no-commit", "--no-ff", target)
    ctx.expect_eq((sb.git_dir / "MERGE_HEAD").read_text(encoding="utf-8").split()[0], target,
                  "step 16: the foreign merge did not start")
    staged = sb.git("diff", "--cached", "--name-only", "-z")
    _rc, _out, err = ctx.sync("abort", expect=1)
    ctx.expect("refusing to abort your merge" in err,
               "step 16: abort did not refuse the foreign merge", err)
    ctx.expect(f"{target[:12]}" in err, "step 16: the refusal does not name the merge", err)
    ctx.expect((sb.git_dir / "MERGE_HEAD").exists(),
               "step 16: abort removed somebody else's merge")
    ctx.expect_eq((sb.git_dir / "MERGE_HEAD").read_text(encoding="utf-8").split()[0], target,
                  "step 16: abort changed MERGE_HEAD")
    ctx.expect_eq(sb.git("diff", "--cached", "--name-only", "-z"), staged,
                  "step 16: abort changed the staged merge result")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 16: the refused abort moved HEAD")
    # The user aborts their own merge; then `abort` works.
    sb.git("merge", "--abort")
    _rc, out, _err = ctx.sync("abort", expect=0)
    ctx.expect("sync aborted" in out, "step 16: abort did not report success")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 16: abort did not return to the branch tip")
    ctx.require_clean_end("step 16")


def step_resume_boundaries(ctx: Context) -> None:
    """`continue` resumes from every stage boundary of the sync state machine."""
    sb = ctx.sb
    sb.git("update-ref", "refs/heads/extra/framework", ctx.framework_tip)
    for number, boundary in enumerate(BOUNDARIES):
        previous = sb.rev("HEAD")
        if boundary == "framework-merge":
            pinned = ctx.framework_commit(
                {f"extra/boundary-{number}.txt": f"framework change {number}\n"},
                message=f"test: framework move for the {boundary} boundary")
            target = ctx.upstream_commit({f"src/extra/boundary-{number}.txt": "upstream\n"},
                                         framework=pinned,
                                         message=f"test: pin for the {boundary} boundary")
        else:
            pinned = None
            target = ctx.upstream_commit({f"src/extra/boundary-{number}.txt": "upstream\n"},
                                         message=f"test: target for the {boundary} boundary")
        temp = f"sync/{target[:12]}"
        _rc, _out, err = ctx.sync_synthetic(target, "extra/framework", expect=1,
                                            stop_after=boundary)
        ctx.expect(f"[test] stopped after {boundary}" in err,
                   f"step 17: the {boundary} stop hook did not fire", err)
        ctx.expect(sb.state_file().exists(), f"step 17: no state at the {boundary} boundary")
        state = sb.state()
        ctx.expect_eq(state["stage"], boundary, f"step 17: wrong stage at {boundary}")
        ctx.expect_eq(state["target"], target, f"step 17: wrong target at {boundary}")
        if boundary == "start":
            ctx.expect_eq(sb.branch(), "test/base", "step 17: start left the branch")
            ctx.expect_eq(sb.sync_branches(), [], "step 17: start created a temp branch")
            ctx.expect_eq(sb.rev("HEAD"), state["start"], "step 17: start moved HEAD")
        elif boundary == "temp-branch":
            ctx.expect_eq(sb.branch(), temp, "step 17: temp-branch left the branch")
            ctx.expect_eq(sb.rev("HEAD"), state["start"], "step 17: temp-branch moved HEAD")
        elif boundary == "upstream-merge":
            ctx.expect_eq(sb.branch(), temp, "step 17: upstream-merge left the branch")
            ctx.expect_eq((sb.git_dir / "MERGE_HEAD").exists(), True,
                          "step 17: upstream-merge did not leave the merge open")
            ctx.expect_eq(sb.rev("HEAD"), state["start"],
                          "step 17: upstream-merge moved HEAD")
        elif boundary == "upstream-commit":
            ctx.expect_eq(sb.parents(), [state["start"], target],
                          "step 17: upstream-commit is not the expected merge")
            ctx.expect_eq(state["upstreamMerge"], sb.rev("HEAD"),
                          "step 17: the state does not record the upstream merge")
        elif boundary == "framework-merge":
            ctx.expect_eq(sb.parents(), [state["upstreamMerge"], pinned],
                          "step 17: framework-merge is not the expected merge")
            ctx.expect_eq(state["frameworkMerge"], sb.rev("HEAD"),
                          "step 17: the state does not record the framework merge")
            message = sb.git("log", "-1", "--format=%B", sb.rev("HEAD"))
            ctx.expect(f"Framework-Commit: {pinned}" in message,
                       "step 17: the framework merge lost its Framework-Commit trailer", message)
        elif boundary == "check":
            ctx.expect_eq(sb.branch(), temp, "step 17: check left the branch")
            ctx.expect_eq(state["head"], sb.rev("HEAD"),
                          "step 17: the state does not record the verified tip")
        elif boundary == "fast-forward":
            # The completed fast-forward that was not cleaned up yet.
            ctx.expect_eq(sb.branch(), "test/base", "step 17: fast-forward left the branch")
            ctx.expect_eq(sb.rev("HEAD"), sb.rev(f"refs/heads/{temp}"),
                          "step 17: the branch did not reach the sync tip")
            ctx.expect_eq(state["head"], sb.rev("HEAD"),
                          "step 17: the branch is not at the recorded tip")
            ctx.expect_eq(sb.sync_branches(), [temp],
                          "step 17: the temporary branch is already gone")
        _rc, out, _err = ctx.sync("continue", expect=0)
        head = sb.rev("HEAD")
        expected = 2 if pinned else 1
        ctx.expect_eq(sb.count(f"{previous}..{head}", first_parent=True), expected,
                      f"step 17: resuming at {boundary} produced the wrong first-parent "
                      f"history")
        ctx.expect(sb.ok("merge-base", "--is-ancestor", target, head),
                   f"step 17: the target is not contained after resuming at {boundary}")
        if pinned:
            ctx.framework_tip = pinned
        ctx.require_clean_end(f"step 17 ({boundary})")

    # The last boundary is cleanup: everything is done and only the test hook fires.
    previous = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/after-cleanup.txt": "upstream change\n"},
                                 message="test: target for the cleanup boundary")
    _rc, _out, err = ctx.sync_synthetic(target, "extra/framework", expect=1,
                                        stop_after="cleanup")
    ctx.expect("[test] stopped after cleanup" in err,
               "step 17: the cleanup stop hook did not fire", err)
    ctx.expect(not sb.state_file().exists(), "step 17: cleanup left the state file behind")
    ctx.expect_eq(sb.sync_branches(), [], "step 17: cleanup left a temporary branch behind")
    ctx.expect_eq(sb.count(f"{previous}..{sb.rev('HEAD')}", first_parent=True), 1,
                  "step 17: the sync did not complete before the cleanup stop")
    _rc, out, _err = ctx.sync_synthetic(target, "extra/framework", expect=0)
    ctx.expect(f"already contains {target}" in out,
               "step 17: rerunning after the cleanup stop is not a no-op", out)
    ctx.require_clean_end("step 17 (cleanup)")


def step_leftover_temp_branch(ctx: Context) -> None:
    """A `sync/<sha>` branch without a state file is reported, not guessed at."""
    sb = ctx.sb
    before = (sb.rev("HEAD"), sb.tree("HEAD"), sb.git("ls-files", "-s", "-z"))
    leftover = "sync/0123456789ab"
    sb.git("branch", leftover, "HEAD")
    _rc, _out, err = ctx.sync("sync", "--target", TARGET_FINAL, expect=1)
    ctx.expect("no sync state file" in err and leftover in err,
               "step 18: a leftover temp branch without state is not reported", err)
    ctx.expect("git branch -D" in err, "step 18: the guidance does not say how to clean up", err)
    expect_refused(ctx, "step 18", before)
    ctx.expect_eq(sb.sync_branches(), [leftover],
                  "step 18: the leftover branch was deleted or another one appeared")
    _rc, _out, err = ctx.sync("abort", expect=1)
    ctx.expect(leftover in err, "step 18: abort does not mention the leftover branch", err)
    ctx.expect_eq(sb.sync_branches(), [leftover], "step 18: abort deleted the leftover branch")
    sb.git("branch", "--delete", leftover)
    ctx.require_clean_end("step 18")


def step_rerun_after_fast_forward(ctx: Context) -> None:
    """Rerunning `sync` after a completed fast-forward cleans up and does nothing else."""
    sb = ctx.sb
    previous = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/completed-ff.txt": "upstream change\n"},
                                 message="test: target for the completed fast-forward")
    temp = f"sync/{target[:12]}"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="fast-forward")
    ctx.expect_eq(sb.sync_branches(), [temp],
                  "step 19: the fast-forward boundary left no temporary branch")
    synced = sb.rev("HEAD")
    _rc, out, err = ctx.sync_synthetic(target, expect=0)
    ctx.expect("found an unfinished sync; aborting it first" in out,
               "step 19: the rerun did not abort the leftover sync", out)
    ctx.expect(f"already contains {target}" in out,
               "step 19: the rerun did not report that the target is contained", out)
    ctx.expect_eq(sb.rev("HEAD"), synced, "step 19: the rerun moved the branch")
    ctx.expect_eq(sb.count(f"{previous}..{synced}", first_parent=True), 1,
                  "step 19: the completed sync is not a single upstream merge")
    ctx.require_clean_end("step 19")


def step_clean_preconditions(ctx: Context) -> None:
    """(A) sync/continue/abort refuse every change the sync did not leave behind."""
    sb = ctx.sb
    before = (sb.rev("HEAD"), sb.tree("HEAD"), sb.git("ls-files", "-s", "-z"))
    target = ctx.upstream_commit({"src/extra/precondition.txt": "upstream change\n"},
                                 message="test: target for the precondition checks")

    # (a) an untracked file anywhere in the clone
    stray = "scratch-untracked.txt"
    sb.path(stray).write_bytes(b"not the tool's business\n")
    _rc, _out, err = ctx.sync_synthetic(target, expect=1)
    ctx.expect("not clean" in err and stray in err,
               "step 20: sync did not refuse the untracked file", err)
    expect_refused(ctx, "step 20", before)
    ctx.expect_eq(sb.sync_branches(), [], "step 20: a temporary branch was created")
    ctx.expect(not sb.state_file().exists(), "step 20: a state file was written")
    ctx.expect(sb.path(stray).exists(), "step 20: the refused sync deleted the file")
    sb.path(stray).unlink()

    # (b) an ignored file under a path the run removes
    ignored = "src/appshell/ignored-local.txt"
    sb.path(ignored).parent.mkdir(parents=True, exist_ok=True)
    sb.path(ignored).write_bytes(b"ignored but present\n")
    exclude = sb.git_dir / "info" / "exclude"
    exclude.parent.mkdir(exist_ok=True)
    exclude.write_bytes(b"src/appshell/ignored-local.txt\n")
    ctx.expect_eq(sb.status_all(), "", "step 20: the ignore rule did not hide the file")
    _rc, _out, err = ctx.sync_synthetic(target, expect=1)
    ctx.expect("ignored files" in err and ignored in err,
               "step 20: sync did not refuse the ignored file", err)
    ctx.expect(sb.path(ignored).exists(), "step 20: the refused sync deleted the ignored file")
    sb.path(ignored).unlink()
    exclude.write_bytes(b"")

    # (c) continue and abort refuse an edit the sync did not make
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="check")
    ctx.expect_eq(sb.state()["stage"], "check", "step 20: the check stop did not fire")
    edited = sb.path("TRIM")
    original = edited.read_bytes()
    edited.write_bytes(original + b"# an unsaved local edit\n")
    _rc, _out, err = ctx.sync("continue", expect=1)
    ctx.expect("changes this sync did not make" in err and "TRIM" in err,
               "step 20: continue did not refuse the unsaved edit", err)
    ctx.expect_eq(sb.rev("refs/heads/test/base"), before[0],
                  "step 20: the refused continue moved the branch")
    ctx.expect_eq(sb.state()["stage"], "check",
                  "step 20: the refused continue advanced the state")
    ctx.expect_eq(edited.read_bytes(), original + b"# an unsaved local edit\n",
                  "step 20: the refused continue reverted the edit")
    _rc, _out, err = ctx.sync("abort", expect=1)
    ctx.expect("changes this sync did not make" in err,
               "step 20: abort did not refuse the unsaved edit", err)
    ctx.expect(sb.state_file().exists(), "step 20: the refused abort dropped the state")
    edited.write_bytes(original)
    _rc, _out, _err = ctx.sync("continue", expect=0)
    ctx.expect_eq(sb.parents(), [before[0], target],
                  "step 20: the resumed sync is not the expected upstream merge")
    ctx.require_clean_end("step 20")


def step_literal_paths(ctx: Context) -> None:
    """(B) trimmed paths that look like globs are removed literally."""
    sb = ctx.sb
    previous = sb.rev("HEAD")
    globbed = "src/appshell/item[ab].txt"
    plain = "src/appshell/itema.txt"
    target = ctx.upstream_commit({globbed: "bracket name\n", plain: "plain name\n"},
                                 message="test: trimmed additions with bracket names")
    _rc, out, _err = ctx.sync_synthetic(target, expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["droppedAdditions"], sorted([globbed, plain]),
                  "step 21: the bracket-named additions are not reported one by one")
    ctx.expect_eq(report["discardedChanges"], [], "step 21: unexpected discarded changes")
    for path in (globbed, plain):
        ctx.expect(not sb.exists(path), f"step 21: dropped addition {path!r} is still on disk")
    ctx.expect_eq(sb.ls_files("--", "src/appshell"), [],
                  "step 21: a dropped addition is still in the index")
    ctx.expect_eq(sb.parents(), [previous, target],
                  "step 21: the sync did not produce a single upstream merge")
    ctx.require_no_trimmed("step 21")
    ctx.require_clean_end("step 21")


def step_legacy_console(ctx: Context) -> None:
    """(C) a GBK console code page does not turn a successful sync into a failure."""
    sb = ctx.sb
    previous = sb.rev("HEAD")
    path = "src/appshell/caf\u00e9 \u2713-gbk.txt"
    target = ctx.upstream_commit({path: "non-ascii upstream name\n"},
                                 message="test: target for the console code page")
    _rc, out, _err = ctx.sync_synthetic(
        target, expect=0,
        env_overrides={"PYTHONIOENCODING": "gbk", "PYTHONUTF8": None})
    report = parse_report(out)
    ctx.expect_eq(report["droppedAdditions"], [path],
                  "step 22: the non-ASCII path was not reported under a GBK code page")
    ctx.expect(not sb.exists(path), "step 22: the dropped addition is still on disk")
    ctx.expect_eq(sb.parents(), [previous, target],
                  "step 22: the GBK run did not finish the sync")
    ctx.require_clean_end("step 22")


def step_foreign_merge_commits(ctx: Context) -> None:
    """(D) merges the tool did not make are refused, not adopted."""
    sb = ctx.sb
    before = sb.rev("HEAD")

    # (a) the upstream merge committed by hand, without the tool's trailer
    target = ctx.upstream_commit({"src/extra/foreign-upstream.txt": "upstream change\n"},
                                 message="test: target for a hand-made upstream merge")
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="upstream-merge")
    # Do the tool's own resolution by hand (drop ort's displaced `muse~<sha>` entry) and
    # commit the merge the way `git merge --continue` would, i.e. without our trailer.
    for path in list(sb.unmerged()):
        sb.git("rm", "-q", "--cached", "--", path)
        if sb.path(path).is_dir():
            shutil.rmtree(sb.path(path))
        elif sb.path(path).exists():
            sb.path(path).unlink()
    sb.git("commit", "--quiet", "--no-edit")
    ctx.expect_eq(sb.parents(), [before, target],
                  "step 23: the hand-made upstream merge is not a merge of the target")
    ctx.expect("Sync-Upstream" not in sb.git("log", "-1", "--format=%B", "HEAD"),
               "step 23: git's own merge message already carries a Sync-Upstream trailer")
    _rc, _out, err = ctx.sync("continue", expect=1)
    ctx.expect("this sync did not make" in err and "Sync-Upstream" in err,
               "step 23: continue adopted the hand-made upstream merge", err)
    ctx.expect("git reset --hard" in err, "step 23: the refusal has no instructions", err)
    _rc, out, _err = ctx.sync("abort", expect=0)
    ctx.expect("sync aborted" in out, "step 23: abort did not report success")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 23: abort did not return to the branch tip")
    ctx.require_clean_end("step 23 (upstream)")

    # (b) the framework merge committed by hand, without the tool's trailer
    pinned = ctx.framework_commit({"extra/foreign-framework.txt": "framework change\n"},
                                  message="test: framework move for a hand-made merge")
    target = ctx.upstream_commit({"src/extra/foreign-framework.txt": "upstream change\n"},
                                 framework=pinned,
                                 message="test: target pinning that framework")
    _rc, _out, _err = ctx.sync_synthetic(target, "extra/framework", expect=1,
                                        stop_after="upstream-commit")
    sb.git("merge", "--quiet", "--no-ff", "-s", "ort", "-Xsubtree=muse",
           "-m", "hand-made framework merge", pinned)
    ctx.expect_eq(sb.git("log", "-1", "--format=%B").strip(), "hand-made framework merge",
                  "step 23: the hand-made framework merge has an unexpected message")
    _rc, _out, err = ctx.sync("continue", expect=1)
    ctx.expect("this sync did not make" in err and "Framework-Commit" in err,
               "step 23: continue adopted the hand-made framework merge", err)
    _rc, _out, _err = ctx.sync("abort", expect=0)
    ctx.expect_eq(sb.rev("HEAD"), before, "step 23: abort did not return to the branch tip")
    ctx.require_clean_end("step 23 (framework)")


def step_link_name_provenance(ctx: Context) -> None:
    """(E) a gitlink that really is named `muse~...` is a conflict for a human."""
    sb = ctx.sb
    before = sb.rev("HEAD")
    upstream_link = "muse~important.txt"
    sb.path(upstream_link).write_bytes(b"local file with that name\n")
    sb.git("add", "--", upstream_link)
    sb.git("commit", "--quiet", "-m", "test: local file named like an upstream gitlink")
    local = sb.rev("HEAD")
    target = sb.make_commit(TARGET_FINAL, {}, "test: upstream gitlink named muse~important.txt",
                            gitlink=ctx.framework_tip,
                            gitlinks={upstream_link: ctx.framework_tip})
    sb.git("update-ref", "refs/heads/extra/upstream", target)
    ctx.expect_eq(sb.git("ls-tree", "-z", target, "--", upstream_link).split(" ")[0],
                  "160000", "step 24: the synthetic target has no gitlink at that name")
    _rc, out, _err = ctx.sync_synthetic(target, expect=2)
    ctx.expect(upstream_link in sb.unmerged(),
               "step 24: the colliding gitlink was dropped instead of conflicting")
    ctx.expect(any(path.startswith("muse~") for path in sb.unmerged()),
               "step 24: no muse~ path is unmerged at all", f"unmerged: {sorted(sb.unmerged())}")
    printed = parse_list_after(out, "conflicts that need a human")
    ctx.expect_eq(printed, sorted(sb.unmerged()),
                  "step 24: the reported conflict list differs from the index")
    ctx.expect_eq(sb.state()["linkDrops"], [],
                  "step 24: the provenance check accepted the path as ort's displaced name")
    _rc, _out, _err = ctx.sync("abort", expect=0)
    ctx.expect_eq(sb.rev("HEAD"), local, "step 24: abort did not return to the branch tip")
    sb.git("reset", "--quiet", "--hard", before)
    ctx.require_clean_end("step 24")


def step_recovery_windows(ctx: Context) -> None:
    """(F) abort and continue handle every recorded state."""
    sb = ctx.sb

    # (a) abort right after the start save, before the temporary branch exists
    target = ctx.upstream_commit({"src/extra/window-a.txt": "upstream\n"},
                                 message="test: target for the start-save window")
    before = sb.rev("HEAD")
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="start")
    ctx.expect_eq(sb.state()["stage"], "start", "step 25: the start stop did not fire")
    ctx.expect_eq(sb.sync_branches(), [], "step 25: the start stop created a temp branch")
    _rc, out, _err = ctx.sync("abort", expect=0)
    ctx.expect("sync aborted" in out, "step 25: abort refused the start-save state", out)
    ctx.expect(not sb.state_file().exists(), "step 25: abort left the state file behind")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 25: abort moved the branch")
    ctx.require_clean_end("step 25 (start abort)")

    # (b) continue from "state saved, temporary branch not yet created"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="start")
    _rc, _out, _err = ctx.sync("continue", expect=0)
    ctx.expect_eq(sb.parents(), [before, target],
                  "step 25: continue did not create the branch and merge")
    ctx.require_clean_end("step 25 (start continue)")

    # (c) continue after the fast-forward ran inside the stage (checkout done, merge not)
    before = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/window-c.txt": "upstream\n"},
                                 message="test: target for the checkout window")
    temp = f"sync/{target[:12]}"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="check")
    ctx.expect_eq(sb.rev(f"refs/heads/{temp}"), sb.state()["head"],
                  "step 25: the check stop did not leave the verified tip on the temp branch")
    sb.git("checkout", "--quiet", "test/base")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 25: the branch is not at the sync start")
    _rc, _out, _err = ctx.sync("continue", expect=0)
    ctx.expect_eq(sb.count(f"{before}..HEAD", first_parent=True), 1,
                  "step 25: the checkout window produced the wrong history")
    ctx.expect_eq(sb.parents(), [before, target],
                  "step 25: the fast-forward was not completed")
    ctx.require_clean_end("step 25 (checkout window continue)")

    # (d) abort in that same window
    before = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/window-d.txt": "upstream\n"},
                                 message="test: target for the abort window")
    temp = f"sync/{target[:12]}"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="check")
    sb.git("checkout", "--quiet", "test/base")
    _rc, out, _err = ctx.sync("abort", expect=0)
    ctx.expect("sync aborted" in out, "step 25: abort refused the checkout window", out)
    ctx.expect_eq(sb.sync_branches(), [], "step 25: abort left the temp branch behind")
    ctx.expect_eq(sb.rev("HEAD"), before, "step 25: abort moved the branch")
    ctx.require_clean_end("step 25 (checkout window abort)")

    # (e) continue after the fast-forward completed but the state was not updated
    before = sb.rev("HEAD")
    target = ctx.upstream_commit({"src/extra/window-e.txt": "upstream\n"},
                                 message="test: target for the completed fast-forward")
    temp = f"sync/{target[:12]}"
    _rc, _out, _err = ctx.sync_synthetic(target, expect=1, stop_after="check")
    synced = sb.rev(f"refs/heads/{temp}")
    sb.git("checkout", "--quiet", "test/base")
    sb.git("merge", "--quiet", "--ff-only", temp)
    ctx.expect_eq(sb.rev("HEAD"), synced, "step 25: the manual fast-forward did not land")
    _rc, _out, _err = ctx.sync("continue", expect=0)
    ctx.expect_eq(sb.rev("HEAD"), synced,
                  "step 25: continue did not leave the completed fast-forward alone")
    ctx.expect_eq(sb.count(f"{before}..HEAD", first_parent=True), 1,
                  "step 25: continue repeated the fast-forward")
    ctx.require_clean_end("step 25 (completed fast-forward)")


def step_removal_audit(ctx: Context) -> None:
    """(G) an interruption between index and worktree removal is replayed."""
    sb = ctx.sb
    previous = sb.rev("HEAD")
    first = "src/appshell/audit/one.txt"
    second = "src/appshell/audit/two.txt"
    target = ctx.upstream_commit({first: "one\n", second: "two\n"},
                                 message="test: target for the removal audit")
    _rc, _out, err = ctx.sync_synthetic(
        target, expect=1, env_overrides={"SYNC_UPSTREAM_TEST_STOP_AFTER_RM": "2"})
    ctx.expect("stopped after removing index entry 2" in err,
               "step 26: the removal stop hook did not fire", err)
    state = sb.state()
    ctx.expect(state["removals"], "step 26: the removal plan was not persisted")
    planned = [entry["path"] for entry in state["removals"]]
    ctx.expect(planned[0].startswith("muse~"),
               "step 26: the plan does not start with the displaced gitlink entry",
               f"planned: {planned}")
    ctx.expect_eq(planned[1:], [first, second],
                  "step 26: the persisted plan is not the planned removal")
    ctx.expect_eq(state["report"]["droppedAdditions"], [first, second],
                  "step 26: the report entries were not persisted with the plan")
    ctx.expect_eq(sb.ls_files("--", "src/appshell/audit"), [second],
                  "step 26: the interrupted removal left the wrong entries in the index")
    ctx.expect(sb.exists(first),
               "step 26: the interruption did not leave the file in the worktree")
    ctx.expect_eq(sorted(line for line in sb.status_all().splitlines() if line.strip()),
                  sorted([f"A  {second}", f"?? {first}"]),
                  "step 26: the half-removed state is not the expected one")
    _rc, out, _err = ctx.sync("continue", expect=0)
    report = parse_report(out)
    ctx.expect_eq(report["droppedAdditions"], sorted([first, second]),
                  "step 26: the resumed sync lost or duplicated a report entry")
    for path in (first, second):
        ctx.expect(not sb.exists(path), f"step 26: {path!r} is still in the worktree")
    ctx.expect_eq(sb.ls_files("--", "src/appshell"), [],
                  "step 26: a removed path is still in the index")
    ctx.expect_eq(sb.parents(), [previous, target],
                  "step 26: the resumed sync is not the expected upstream merge")
    ctx.require_no_trimmed("step 26")
    ctx.require_clean_end("step 26")


STEPS = [
    ("absorb: test TRIM + framework absorbed at muse/", step_absorb),
    ("link bump 7b2d005464: two merges and the trimmed-edit report", step_link_bump),
    ("no link bump 840bd348e1: a single merge commit", step_no_link_bump),
    ("framework conflict 11157e4086: resolve, commit, continue", step_framework_conflict),
    ("interruption f019f4079d: stop twice, then continue", step_interrupted),
    ("missing framework pin 9625e32ad4: refused, branch untouched", step_missing_framework),
    ("final upstream 0ea613f: tree equals upstream plus TRIM", step_final),
    ("idempotence: second sync of 0ea613f changes nothing", step_idempotent),
    ("TRIM policy: an upstream TRIM file is refused", step_trim_policy),
    ("trimmed paths with spaces and non-ASCII are dropped verbatim", step_trimmed_unicode),
    ("a colliding muse~ file stops for a human", step_link_name_collision),
    ("side-parent framework reachability keeps the recorded base", step_framework_side_parent),
    ("a rewritten framework mirror is refused", step_framework_rewritten),
    ("a fatal merge error keeps the state and `continue` retries", step_fatal_merge),
    ("a manual merge --abort is not treated as resolved", step_manual_merge_abort),
    ("abort refuses a foreign merge or another branch", step_abort_refusals),
    ("continue resumes from every stage boundary", step_resume_boundaries),
    ("a leftover sync branch without state is reported", step_leftover_temp_branch),
    ("rerunning sync after a completed fast-forward cleans up", step_rerun_after_fast_forward),
    ("preconditions: dirty and ignored files are refused (A)", step_clean_preconditions),
    ("trimmed paths that look like globs are literal (B)", step_literal_paths),
    ("a GBK console code page does not break the report (C)", step_legacy_console),
    ("merge commits made outside the tool are refused (D)", step_foreign_merge_commits),
    ("a real muse~... gitlink is a conflict (E)", step_link_name_provenance),
    ("abort and continue work at every recorded state (F)", step_recovery_windows),
    ("an interrupted removal is replayed from the plan (G)", step_removal_audit),
]


# --- driver ---------------------------------------------------------------------

def caller_snapshot(repo: Path, env: dict) -> tuple[str, str, dict[str, str]]:
    """The caller's branch state; the test must leave every one of these alone."""
    head = git_read(repo, env, "rev-parse", "HEAD").strip()
    branch = git_read(repo, env, "rev-parse", "--abbrev-ref", "HEAD").strip()
    refs = {}
    for line in git_read(repo, env, "for-each-ref", "--format=%(refname) %(objectname)",
                         "refs/heads").splitlines():
        name, sha = line.split(" ")
        refs[name] = sha
    return head, branch, refs


def check_caller_untouched(repo: Path, env: dict,
                           before: tuple[str, str, dict[str, str]],
                           after: tuple[str, str, dict[str, str]]) -> None:
    """Branch heads that existed must still point at the same commits.

    The source repository is shared with other worktrees, so a branch pushed forward by
    someone else while the test runs is only reported. A branch that moved to a commit
    the test identity wrote is a real failure: the test never commits to the source.
    """
    before_head, before_branch, before_refs = before
    after_head, after_branch, after_refs = after
    if (after_head, after_branch) != (before_head, before_branch):
        raise Failure(f"source repository HEAD moved: {before_branch}@{before_head[:12]} -> "
                      f"{after_branch}@{after_head[:12]}")
    moved = []
    for name, sha in before_refs.items():
        new = after_refs.get(name)
        if new == sha:
            continue
        if new and git_read(repo, env, "log", "-1", "--format=%ce", new).strip() == TEST_IDENTITY:
            moved.append(f"{name}: {sha[:12]} -> {new[:12]} (written by this test)")
        else:
            print(f"  note: {name} moved outside this test "
                  f"({sha[:12]} -> {new[:12] if new else '<deleted>'}); ignoring")
    if moved:
        raise Failure("the test moved branches in the source repository:\n  " + "\n  ".join(moved))


def remove_tree(path: Path) -> None:
    def on_error(function, failed_path, _excinfo):
        try:
            os.chmod(failed_path, stat.S_IWRITE)
            function(failed_path)
        except OSError:
            pass
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=lambda fn, p, _e: on_error(fn, p, None))
    else:
        shutil.rmtree(path, onerror=on_error)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="End-to-end test for tools/sync-upstream.py (throwaway clone, real history).")
    parser.add_argument("--repo", type=Path, default=None,
                        help="repository containing tools/sync-upstream.py "
                             "(default: the repo this script lives in)")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary clone after the run")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    # Paths in the report are printed verbatim (non-ASCII included) and the Windows
    # console defaults to a legacy code page; pin the streams to UTF-8.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    options = parse_args(argv)
    here = Path(__file__).resolve()
    repo_hint = options.repo or here.parent.parent
    started = time.time()
    tmp: Path | None = None
    exit_code = 1

    print("sync-upstream.py end-to-end test")
    print(f"  test script:       {here}")
    print(f"  source repository: {repo_hint}")

    tmp = Path(tempfile.mkdtemp(prefix="sync-upstream-test-"))
    env = build_env(tmp)
    try:
        try:
            repo = Path(git_read(repo_hint, env, "rev-parse", "--show-toplevel").strip())
        except Failure as error:
            raise Failure(f"--repo {repo_hint} is not a git work tree: {error}") from error
        print(f"  resolved repository: {repo}")
        before = caller_snapshot(repo, env)
        script = repo / "tools" / "sync-upstream.py"   # the script under test
        preflight(repo, script, env)
        print(f"  script under test: {script} ({describe_script(repo, script, env)})")
        work = create_sandbox(repo, tmp, env)
        ctx = Context(script, work, env)
        print(f"  temporary clone: {work}")

        failed_at: int | None = None
        passed = 0
        for number, (title, function) in enumerate(STEPS, 1):
            if failed_at is not None:
                print(f"\nSKIP step {number}: {title} (an earlier step failed)")
                continue
            print(f"\nSTEP {number}: {title}")
            try:
                function(ctx)
            except Failure as error:
                print(f"FAIL step {number}: {title}\n{indent(str(error))}")
                failed_at = number
            except Exception:  # noqa: BLE001 - a crash is a test failure too
                print(f"FAIL step {number}: {title}\n{indent(traceback.format_exc())}")
                failed_at = number
            else:
                passed += 1
                print(f"PASS step {number}: {title}")

        after = caller_snapshot(repo, env)
        try:
            check_caller_untouched(repo, env, before, after)
        except Failure as error:
            print(f"FAIL source repository was modified\n{indent(str(error))}")
            failed_at = failed_at or len(STEPS) + 1

        elapsed = time.time() - started
        if failed_at is not None:
            print(f"\nFAILED at step {failed_at} ({passed}/{len(STEPS)} steps passed, "
                  f"{elapsed:.1f}s)")
        else:
            print(f"\nPASS: all {len(STEPS)} steps ({passed} passed, {elapsed:.1f}s)")
            exit_code = 0
    except Failure as error:
        print(f"FAIL (setup)\n{indent(str(error))}")
    except Exception:  # noqa: BLE001
        print(f"FAIL (crashed)\n{indent(traceback.format_exc())}")
    finally:
        if tmp is not None:
            if options.keep:
                print(f"kept temporary clone: {tmp}")
            else:
                remove_tree(tmp)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
