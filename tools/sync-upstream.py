#!/usr/bin/env python3
"""Sync the engrave fork's trimmed `main` with upstream MuseScore (roadmap-engrave A1).

Layout: upstream MuseScore keeps the framework as a gitlink `muse` (submodule
muse_framework) and prebuilt-dependency recipes as a gitlink `muse_deps`. engrave
absorbs the framework as a subtree directory `muse/` (full history, mirrored on the
branch `upstream/muse-framework`) and deletes every path listed in `TRIM`.

Subcommands:
  absorb [--framework SHA]   one-time: replace the `muse` gitlink with the framework
                             tree (merge commit with the framework as second parent)
  sync [--target SHA]        merge an upstream commit (default: the upstream mirror
                             branch), then the framework commit its gitlink names
  continue                   resume after resolving a conflict the script stopped on
  abort                      drop an unfinished sync and return to the branch

A sync runs on a temporary branch `sync/<sha>` and fast-forwards the branch only after
both merges and the tree checks pass, so the branch only ever moves forward. Nothing
is pushed.

State lives in .git/sync-upstream.json. It is written atomically (temp file +
os.replace) *before* the mutation of the next stage, records the commits the sync
expects, and every stage verifies branch and commit identity before it acts, so a
`continue` after an interruption at any stage boundary is safe:

  start -> temp-branch -> upstream-merge -> upstream-commit
        -> framework-merge -> check -> fast-forward -> cleanup

Each stage is idempotent; a stage that finds its own work already done adopts it
(a merged commit is verified to have the expected parents, not to be new). A fatal
`git merge` failure (exit > 1) stops with git's message and keeps the state, so the
cause can be fixed and `continue` retries; `abort` returns to the branch. A merge that
is no longer in progress although it was started (a manual `git merge --abort`) is
not mistaken for a resolved conflict: `continue` refuses.

Every absorb and framework merge commit carries a `Framework-Commit: <sha>` trailer.
The next sync reads the previously incorporated framework commit from the latest such
commit on the first-parent chain (falling back to the subject of the pre-trailer
absorb commit 7150f2b05e) instead of inferring it with `merge-base`, which also finds
framework commits that reached the branch through some other parent. That SHA is the
explicit merge-base expectation: a mirror that no longer contains it, or a pin that
does not descend from it, is refused.

TRIM: one path per line, `#` comments; a trailing `/` marks a directory prefix,
otherwise the entry is an exact file path. No globs. A sync never changes TRIM (only
normal commits on `main` do) and refuses a target that adds or changes it.

Paths are always read from NUL-delimited git output (`-z`), so names with spaces or
non-ASCII characters are reported and removed verbatim.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

UPSTREAM_REF = "origin/upstream/musescore"
FRAMEWORK_REF = "origin/upstream/muse-framework"
FRAMEWORK_PATH = "muse"
TRIM_FILE = "TRIM"
GITLINK = "160000"
FRAMEWORK_TRAILER = "Framework-Commit"
STATE_VERSION = 2
# Stage names double as the values SYNC_UPSTREAM_TEST_STOP_AFTER accepts.
STAGES = ("start", "temp-branch", "upstream-merge", "upstream-commit",
          "framework-merge", "check", "fast-forward", "cleanup")
ABSORB_SUBJECT = re.compile(r"^Absorb muse_framework ([0-9a-f]{40}) at muse/")
TEMP_BRANCH = re.compile(r"^sync/[0-9a-f]{12}$")


class SyncError(Exception):
    pass


def split_z(output: str) -> list[str]:
    """Entries of a `-z` (NUL-delimited) git output; paths are never quoted."""
    return [entry for entry in output.split("\0") if entry]


def indent(text: str, prefix: str = "  ") -> str:
    return textwrap.indent(text, prefix) if text.strip() else f"{prefix}<no output>"


class Repo:
    def __init__(self, root: Path):
        self.root = root

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", *args], cwd=self.root, capture_output=True, text=True,
                              encoding="utf-8")

    def git(self, *args: str, check: bool = True, quiet: bool = False) -> str:
        result = self.run(*args)
        if check and result.returncode != 0:
            raise SyncError(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
        if not quiet and result.stderr.strip() and os.environ.get("SYNC_UPSTREAM_VERBOSE"):
            print(result.stderr.strip(), file=sys.stderr)
        return result.stdout

    def ok(self, *args: str) -> bool:
        return self.run(*args).returncode == 0

    def obj(self, ref: str) -> str | None:
        """Object id of `ref`, or None when it does not resolve."""
        result = self.run("rev-parse", "--verify", "--quiet", ref)
        return result.stdout.strip() if result.returncode == 0 else None

    def rev(self, ref: str) -> str:
        return self.git("rev-parse", "--verify", f"{ref}^{{commit}}").strip()

    def gitlink(self, commit: str, path: str) -> str | None:
        entries = split_z(self.git("ls-tree", "-z", commit, "--", path))
        if not entries:
            return None
        meta, _path = entries[0].split("\t", 1)
        mode, _type, sha = meta.split()
        return sha if mode == GITLINK else None

    def diff_paths(self, older: str, newer: str) -> list[str]:
        """Paths that differ between two tree-ish, NUL-delimited and unquoted."""
        return split_z(self.git("diff", "--no-renames", "--name-only", "-z", older, newer))

    @property
    def git_dir(self) -> Path:
        return Path(self.git("rev-parse", "--absolute-git-dir").strip())

    def merging(self) -> bool:
        return (self.git_dir / "MERGE_HEAD").exists()

    def merge_head(self) -> str | None:
        path = self.git_dir / "MERGE_HEAD"
        if not path.exists():
            return None
        lines = path.read_text(encoding="utf-8").split()
        return lines[0] if lines else None

    def current_branch(self) -> str:
        return self.git("rev-parse", "--abbrev-ref", "HEAD").strip()

    def branch_exists(self, name: str) -> bool:
        return self.ok("rev-parse", "--verify", "--quiet", f"refs/heads/{name}")

    def clean(self) -> bool:
        return not self.git("status", "--porcelain", "--untracked-files=no").strip()


# --- git object helpers ---------------------------------------------------------

def commit_parents(repo: Repo, commit: str) -> list[str]:
    return repo.git("rev-list", "--parents", "-n", "1", commit).split()[1:]


def is_ancestor(repo: Repo, older: str, newer: str) -> bool:
    return repo.ok("merge-base", "--is-ancestor", older, newer)


def index_entries(repo: Repo) -> list[tuple[str, str]]:
    """(mode, path) for every index entry (a conflicted path appears once per stage)."""
    entries = []
    for record in split_z(repo.git("ls-files", "-s", "-z")):
        meta, path = record.split("\t", 1)
        entries.append((meta.split()[0], path))
    return entries


def unmerged_entries(repo: Repo) -> dict[str, dict[int, tuple[str, str]]]:
    """path -> {merge stage: (mode, object id)} from `git ls-files -u -z`."""
    entries: dict[str, dict[int, tuple[str, str]]] = {}
    for record in split_z(repo.git("ls-files", "-u", "-z")):
        meta, path = record.split("\t", 1)
        mode, sha, stage = meta.split()
        entries.setdefault(path, {})[int(stage)] = (mode, sha)
    return entries


def classify_merge(repo: Repo, result: subprocess.CompletedProcess) -> str:
    """Classify a `git merge` run: 'clean', 'pending' or 'conflicts'.

    `pending` is a `--no-commit` merge that is ready to be committed, `conflicts` one
    that left unmerged paths. Anything else (exit > 1, or conflicts without MERGE_HEAD)
    is a fatal failure and raises with git's own message.
    """
    if result.returncode == 0:
        return "pending" if repo.merging() else "clean"
    if result.returncode == 1 and repo.merging() and unmerged_entries(repo):
        return "conflicts"
    detail = (result.stderr.strip() or result.stdout.strip())
    raise SyncError(f"git merge failed (exit {result.returncode}):\n{indent(detail)}\n"
                    f"Nothing was committed. The sync state is kept: fix the cause and run "
                    f"`continue` to retry, or `abort` to return to the branch.")


# --- state ----------------------------------------------------------------------

def state_path(repo: Repo) -> Path:
    return repo.git_dir / "sync-upstream.json"


def save_state(repo: Repo, state: dict) -> None:
    """Write the state atomically: a crash leaves the old or the new file, never a hole."""
    path = state_path(repo)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
    os.replace(tmp, path)


def load_state(repo: Repo) -> dict | None:
    path = state_path(repo)
    if not path.exists():
        return None
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SyncError(f"{path} is not valid JSON ({error}); inspect it and delete it") from error
    if not isinstance(state, dict) or state.get("version") != STATE_VERSION:
        raise SyncError(f"{path} was written by another version of this script; inspect it "
                        f"and delete it, then start the sync again")
    missing = [key for key in ("branch", "temp", "start", "target", "stage") if key not in state]
    if missing:
        raise SyncError(f"{path} is incomplete (missing {', '.join(missing)}); inspect it and "
                        f"delete it, then start the sync again")
    if state["stage"] not in STAGES:
        raise SyncError(f"{path} records an unknown stage {state['stage']!r}; inspect it and "
                        f"delete it, then start the sync again")
    return state


def advance(repo: Repo, state: dict, stage: str) -> None:
    """Record that `stage` finished (its postconditions were verified)."""
    state["stage"] = stage
    save_state(repo, state)
    stop_for_test(stage)


def stop_for_test(stage: str) -> None:
    # Test hook: simulate an interruption right after a stage.
    if os.environ.get("SYNC_UPSTREAM_TEST_STOP_AFTER") == stage:
        raise SystemExit(f"[test] stopped after {stage}")


def temp_branches(repo: Repo) -> list[str]:
    """Our ephemeral `sync/<sha>` branches (there is no state file for them)."""
    names = repo.git("for-each-ref", "--format=%(refname:short)", "refs/heads/sync/").splitlines()
    return [name for name in names if TEMP_BRANCH.match(name)]


def leftover_hint(repo: Repo) -> str:
    leftovers = temp_branches(repo)
    if not leftovers:
        return ""
    return ("\nThere is no state file for the temporary branch(es) "
            + ", ".join(leftovers)
            + ".\nThe script cannot tell which branch such a branch came from, so it neither "
              "checks it out nor deletes it. Inspect it, then remove it with "
              f"`git branch -D {leftovers[0]}` (check out your branch first) and rerun.")


def owned_by_sync(repo: Repo, state: dict, commit: str) -> bool:
    """True when `commit` is one of the commits this sync created or expects."""
    known = {state.get(key) for key in ("start", "upstreamMerge", "frameworkMerge", "head")}
    if commit in known:
        return True
    upstream = state.get("upstreamMerge")
    return bool(upstream) and commit_parents(repo, commit) == [upstream, state["framework"]]


def abort_sync(repo: Repo, state: dict) -> None:
    """Drop the recorded sync. Refuses unless it is provably the sync in progress."""
    temp, branch = state["temp"], state["branch"]
    current = repo.current_branch()
    head = repo.rev("HEAD")
    merge = repo.merge_head()
    if not repo.branch_exists(temp):
        # Cleanup already deleted the temporary branch; only a stale state file is left.
        if current != branch or head != state.get("head"):
            raise SyncError(f"the state records a finished sync, but HEAD is {current} at "
                            f"{head[:12]}; refusing to change anything. Remove "
                            f"{state_path(repo)} yourself if it is stale.")
        state_path(repo).unlink(missing_ok=True)
        return
    if current == branch and state.get("head"):
        # The fast-forward already happened; only the cleanup is left.
        expected = state["head"]
        if merge or head != expected or repo.rev(f"refs/heads/{temp}") != expected:
            raise SyncError(f"the state expects {branch} at {str(expected)[:12]} and "
                            f"{temp} at the same commit, but HEAD is {head[:12]}"
                            f"{' with a merge in progress' if merge else ''}; refusing to "
                            f"clean up. Nothing was changed.")
        repo.git("branch", "-q", "-D", temp)
        state_path(repo).unlink(missing_ok=True)
        return
    if current != temp:
        raise SyncError(f"HEAD is on {current}, but this sync runs on {temp}; refusing to "
                        f"abort. Nothing was changed: finish or abort the other operation "
                        f"first, then check out {temp} and run `abort` again.")
    if not owned_by_sync(repo, state, head):
        raise SyncError(f"{temp} points at {head[:12]}, which this sync did not create; "
                        f"refusing to abort. Nothing was changed.")
    if merge and merge not in (state["target"], state["framework"]):
        raise SyncError(f"a merge of {merge[:12]} is in progress on {temp}, but this sync "
                        f"merges {state['target'][:12]} (upstream) and "
                        f"{state['framework'][:12]} (framework); refusing to abort your "
                        f"merge. It is untouched: finish or abort it yourself, then run "
                        f"`abort` again.")
    if merge:
        repo.git("merge", "--abort")
        if repo.current_branch() != temp:
            raise SyncError(f"`git merge --abort` left HEAD on {repo.current_branch()}, not "
                            f"{temp}; inspect the repository, the state file is kept")
    repo.git("checkout", "-q", "-f", branch)
    if repo.branch_exists(temp):
        repo.git("branch", "-q", "-D", temp)
    state_path(repo).unlink(missing_ok=True)


# --- TRIM -----------------------------------------------------------------------

def load_trim(repo: Repo, ref: str = "HEAD") -> list[str]:
    if not repo.ok("cat-file", "-e", f"{ref}:{TRIM_FILE}"):
        return []
    entries = []
    for line in repo.git("show", f"{ref}:{TRIM_FILE}").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            if any(ch in line for ch in "*?["):
                raise SyncError(f"TRIM entry uses a glob: {line}")
            entries.append(line)
    return entries


def trimmed(path: str, entries: list[str]) -> bool:
    return any(path.startswith(e) if e.endswith("/") else path == e for e in entries)


def check_trim_policy(repo: Repo, base: str, target: str, branch: str) -> None:
    """A sync never changes TRIM: upsteam may neither add nor modify it."""
    before = repo.obj(f"{base}:{TRIM_FILE}")
    after = repo.obj(f"{target}:{TRIM_FILE}")
    if before == after:
        return
    action = "adds a root" if before is None else "changes"
    raise SyncError(f"{target[:12]} {action} {TRIM_FILE} "
                    f"({before or '<absent>'} -> {after or '<absent>'}); a sync never changes "
                    f"{TRIM_FILE} (only normal commits on {branch} do). Nothing was changed.")


# --- framework metadata ---------------------------------------------------------

def framework_from_message(message: str) -> str | None:
    """The `Framework-Commit` trailer of a commit message, if it records one.

    The first absorb commit (7150f2b05e) predates the trailer and names the framework
    commit in its subject; keep reading that so the existing main stays usable.
    """
    for line in reversed(message.splitlines()):
        key, sep, value = line.partition(":")
        if sep and key.strip() == FRAMEWORK_TRAILER:
            value = value.strip()
            if re.fullmatch(r"[0-9a-f]{40}", value):
                return value
            raise SyncError(f"commit message has a malformed {FRAMEWORK_TRAILER} line: {line!r}")
    match = ABSORB_SUBJECT.match(message)
    return match.group(1) if match else None


def validate_framework_record(repo: Repo, commit: str, framework: str) -> None:
    """`commit` must really record `framework` as the framework it merged."""
    parents = commit_parents(repo, commit)
    if len(parents) != 2 or parents[1] != framework:
        raise SyncError(f"{commit[:12]} records framework {framework[:12]}, but its parents are "
                        f"[{' '.join(p[:12] for p in parents) or '<none>'}]; fix that commit's "
                        f"message before syncing")
    if not repo.ok("cat-file", "-e", f"{framework}^{{commit}}"):
        raise SyncError(f"{commit[:12]} records framework {framework[:12]}, which is not in "
                        f"this repository")
    merged = repo.obj(f"{commit}:{FRAMEWORK_PATH}")
    tree = repo.obj(f"{framework}^{{tree}}")
    if merged == tree:
        return
    # The recorded tree is the base the subtree merge started from, so every path where
    # it differs from muse/ at `commit` must already differ from the first parent's muse/.
    from_base = set(repo.diff_paths(tree, merged))
    from_ours = set(repo.diff_paths(tree, f"{commit}^1:{FRAMEWORK_PATH}"))
    if not from_base <= from_ours:
        unexpected = sorted(from_base - from_ours)[:3]
        raise SyncError(f"{commit[:12]} records framework {framework[:12]}, but its "
                        f"{FRAMEWORK_PATH}/ is not that framework tree plus the local delta "
                        f"(unexpected: {unexpected}); the framework metadata is inconsistent")


def recorded_framework(repo: Repo, start: str) -> str:
    """The framework commit the latest absorb/framework merge on `start` recorded.

    Read from the trailer of the newest such commit on the first-parent chain. Deriving
    it with `merge-base <start> <framework ref>` instead is wrong: that also finds
    framework commits which reached the branch through some other parent.
    """
    fields = repo.git("log", "--first-parent", "--format=%H%x1f%B%x1f", start).split("\x1f")
    for index in range(0, len(fields) - 1, 2):
        commit = fields[index].strip()
        if not commit:
            continue
        framework = framework_from_message(fields[index + 1])
        if framework:
            validate_framework_record(repo, commit, framework)
            return framework
    raise SyncError(f"no absorb/framework merge commit (no {FRAMEWORK_TRAILER} trailer) on the "
                    f"first-parent history of {start[:12]}; run `absorb` first")


# --- tree helpers ---------------------------------------------------------------

def remove_worktree_path(repo: Repo, path: str) -> None:
    target = repo.root / path
    if target.is_dir() and not target.is_symlink():
        shutil.rmtree(target, ignore_errors=True)
    elif target.exists() or target.is_symlink():
        target.unlink()
    # Drop directories the removal left empty (git does not track them).
    parent = target.parent
    while parent != repo.root and repo.root in parent.parents:
        try:
            parent.rmdir()
        except OSError:
            break
        parent = parent.parent


def note(report: dict, key: str, path: str) -> None:
    """Record a reported path once (resolving twice must not duplicate entries)."""
    if path not in report[key]:
        report[key].append(path)


def drop_path(repo: Repo, path: str) -> None:
    repo.git("rm", "-q", "--cached", "--", path)
    remove_worktree_path(repo, path)


def resolve_upstream_merge(repo: Repo, trim: list[str], target: str,
                           report: dict) -> list[str]:
    """Resolve the conflicts a sync expects; return the paths that need a human."""
    entries = unmerged_entries(repo)
    unresolved: list[str] = []
    # The upstream gitlink bump: upstream changes `muse`, we deleted it when the
    # framework was absorbed (ort renames the entry to `muse~<sha>`). Only a gitlink from
    # upstream's side may be dropped, and only one such path; anything else at such a
    # name is a real conflict for a human.
    candidates = sorted(path for path in entries
                        if path == FRAMEWORK_PATH or path.startswith(FRAMEWORK_PATH + "~"))
    accepted: set[str] = set()
    expected_link = repo.gitlink(target, FRAMEWORK_PATH)
    if len(candidates) == 1 and expected_link and \
            entries[candidates[0]].get(3) == (GITLINK, expected_link):
        accepted.add(candidates[0])
        drop_path(repo, candidates[0])
    for path in candidates:
        if path not in accepted:
            unresolved.append(path)
    for path, stages in sorted(entries.items()):
        if path in accepted or path in candidates:
            continue
        if trimmed(path, trim) and 2 not in stages:
            # Deleted by us (trimmed), changed upstream: keep it deleted, but report it.
            drop_path(repo, path)
            note(report, "discardedChanges", path)
        else:
            unresolved.append(path)
    # New upstream files under trimmed prefixes merge cleanly; delete and report them.
    for path in split_z(repo.git("diff", "--cached", "--no-renames", "--name-only", "-z",
                                 "--diff-filter=A", "HEAD")):
        if trimmed(path, trim):
            drop_path(repo, path)
            note(report, "droppedAdditions", path)
    return unresolved


def check_tree(repo: Repo, trim: list[str], state: dict) -> list[str]:
    problems = []
    for mode, path in index_entries(repo):
        if mode == GITLINK:
            problems.append(f"gitlink left in the index: {path}")
        if trimmed(path, trim):
            problems.append(f"trimmed path present: {path}")
    if not split_z(repo.git("ls-files", "-z", "--", f"{FRAMEWORK_PATH}/")):
        problems.append(f"{FRAMEWORK_PATH}/ is empty")
    before_trim = repo.obj(f"{state['start']}:{TRIM_FILE}")
    after_trim = repo.obj(f"HEAD:{TRIM_FILE}")
    if before_trim != after_trim:
        problems.append(f"{TRIM_FILE} changed during the sync "
                        f"({before_trim or '<absent>'} -> {after_trim or '<absent>'}); a sync "
                        f"never changes {TRIM_FILE}")
    return problems


# --- stages ---------------------------------------------------------------------

def verify_branch(repo: Repo, expected: str) -> None:
    current = repo.current_branch()
    if current != expected:
        raise SyncError(f"HEAD is on {current}, but this sync runs on {expected}; the sync "
                        f"state no longer matches the repository. Nothing was changed")


def verify_head(repo: Repo, expected: str, what: str = "HEAD") -> None:
    actual = repo.rev("HEAD")
    if actual != expected:
        raise SyncError(f"{what} is {actual[:12]}, expected {expected[:12]}; the sync state no "
                        f"longer matches the repository. Nothing was changed")


def stage_temp_branch(repo: Repo, state: dict) -> bool:
    branch, start, temp = state["branch"], state["start"], state["temp"]
    current = repo.current_branch()
    if current not in (branch, temp):
        raise SyncError(f"HEAD is on {current}, but this sync starts on {branch}; nothing was "
                        f"changed. Check out {branch} (or run `abort`) first")
    verify_head(repo, start, f"{current} at the sync start")
    if repo.branch_exists(temp):
        if repo.rev(f"refs/heads/{temp}") != start:
            raise SyncError(f"the temporary branch {temp} exists but does not point at the "
                            f"sync start {start[:12]}; delete it and rerun if it is stale")
        repo.git("checkout", "-q", "-f", temp)
    else:
        repo.git("checkout", "-q", "-b", temp, start)
    verify_branch(repo, temp)
    verify_head(repo, start)
    advance(repo, state, "temp-branch")
    return True


def stage_upstream_merge(repo: Repo, state: dict) -> bool:
    branch, start, target = state["branch"], state["start"], state["target"]
    verify_branch(repo, state["temp"])
    merge = repo.merge_head()
    if merge:
        if merge != target:
            raise SyncError(f"a merge of {merge[:12]} is in progress, but this sync merges "
                            f"{target[:12]}; nothing was changed. Finish or abort that merge "
                            f"first")
        verify_head(repo, start, "the merge base of the in-progress upstream merge")
        state["mergeStarted"] = True
        save_state(repo, state)
    else:
        if commit_parents(repo, "HEAD") == [start, target]:
            # The merge command ran and was committed before the crash that lost the stage.
            state["mergeStarted"] = True
            state["upstreamMerge"] = repo.rev("HEAD")
            advance(repo, state, "upstream-merge")
            return True
        verify_head(repo, start)
        if state.get("mergeStarted"):
            raise SyncError(
                f"the upstream merge of {target[:12]} is no longer in progress: it was aborted "
                f"(`git merge --abort`) or reset, so this sync cannot be resumed. Nothing was "
                f"committed; run `abort` to return to {branch}, then sync again")
        # No rename detection: relative to the upstream merge base our side "deletes" every
        # trimmed path and "adds" all of muse/, and ort would pair them as bogus renames
        # (observed: a trimmed appshell QML file matched to a framework header). Upstream
        # renames of files we changed then surface as modify/delete conflicts instead.
        result = repo.run("merge", "-q", "--no-commit", "--no-ff", "-s", "ort", "-Xno-renames",
                          target)
        classify_merge(repo, result)
        state["mergeStarted"] = True
        save_state(repo, state)
        if repo.merge_head() != target:
            raise SyncError(f"`git merge {target[:12]}` left no merge to commit; the state is "
                            f"kept, run `abort` and sync again")
    advance(repo, state, "upstream-merge")
    return True


def stage_upstream_commit(repo: Repo, state: dict) -> bool:
    branch, start, target = state["branch"], state["start"], state["target"]
    verify_branch(repo, state["temp"])
    if commit_parents(repo, "HEAD") == [start, target]:
        state["upstreamMerge"] = repo.rev("HEAD")
        advance(repo, state, "upstream-commit")
        return True
    verify_head(repo, start)
    if repo.merge_head() != target:
        raise SyncError(f"the upstream merge of {target[:12]} is no longer in progress (was it "
                        f"aborted?); the sync cannot be resumed. Run `abort` to return to "
                        f"{branch}, then sync again")
    trim = load_trim(repo)
    unresolved = resolve_upstream_merge(repo, trim, target, state["report"])
    save_state(repo, state)
    if unresolved:
        print("conflicts that need a human (resolve, `git add`, then `continue`):")
        for path in unresolved:
            print(f"  {path}")
        return False
    problems = check_tree(repo, trim, state)
    if problems:
        raise SyncError("the upstream merge does not produce the expected tree:\n  "
                        + "\n  ".join(problems))
    repo.git("commit", "-q", "-m", f"Merge upstream/musescore {target}\n\n" + summary(state))
    if commit_parents(repo, "HEAD") != [start, target]:
        raise SyncError(f"the upstream merge commit does not have {start[:12]} and "
                        f"{target[:12]} as parents")
    state["upstreamMerge"] = repo.rev("HEAD")
    advance(repo, state, "upstream-commit")
    return True


def stage_framework_merge(repo: Repo, state: dict) -> bool:
    branch = state["branch"]
    upstream_merge = state.get("upstreamMerge")
    framework, previous = state["framework"], state["previousFramework"]
    if not upstream_merge:
        raise SyncError(f"the sync state records no upstream merge commit; run `abort` and "
                        f"sync again")
    verify_branch(repo, state["temp"])
    if commit_parents(repo, "HEAD") == [upstream_merge, framework]:
        state["frameworkMerge"] = repo.rev("HEAD")
        advance(repo, state, "framework-merge")
        return True
    verify_head(repo, upstream_merge)
    merge = repo.merge_head()
    if merge and merge != framework:
        raise SyncError(f"a merge of {merge[:12]} is in progress, but this sync merges the "
                        f"framework {framework[:12]}; nothing was changed. Finish or abort "
                        f"that merge first")
    if not merge and framework == previous:
        state["frameworkMerge"] = None
        advance(repo, state, "framework-merge")
        return True
    if not merge and state.get("frameworkStarted"):
        raise SyncError(
            f"the framework merge of {framework[:12]} is no longer in progress: it was "
            f"aborted (`git merge --abort`) or reset. Nothing was committed; run `abort` to "
            f"return to {branch}, then sync again")
    if not merge:
        base = repo.git("merge-base", upstream_merge, framework).strip()
        if base != previous:
            raise SyncError(
                f"git would merge the framework from {base[:12]}, but {previous[:12]} is the "
                f"framework commit the latest absorb/framework merge on {branch} recorded "
                f"({FRAMEWORK_TRAILER} trailer), and that is the merge base this sync "
                f"expects.\nFramework history {previous[:12]}..{framework[:12]} reached the "
                f"branch through another parent (or the mirror was rewritten); merging on "
                f"git's base could drop framework changes. Nothing was committed: land the "
                f"framework with a normal framework merge first, or pick another target, "
                f"then sync again (or run `abort`)")
        result = repo.run("merge", "-q", "--no-ff", "-s", "ort", f"-Xsubtree={FRAMEWORK_PATH}",
                          "-m", f"Merge upstream/muse-framework {framework} into "
                                f"{FRAMEWORK_PATH}/\n\n{FRAMEWORK_TRAILER}: {framework}",
                          framework)
        if classify_merge(repo, result) == "conflicts":
            state["frameworkStarted"] = True
            save_state(repo, state)
            print(f"framework conflicts in {FRAMEWORK_PATH}/ (resolve, `git add`, "
                  f"`git commit`, then `continue`):")
            for path in sorted(unmerged_entries(repo)):
                print(f"  {path}")
            return False
        if commit_parents(repo, "HEAD") != [upstream_merge, framework]:
            raise SyncError(f"the framework merge commit does not have {upstream_merge[:12]} "
                            f"and {framework[:12]} as parents")
        state["frameworkMerge"] = repo.rev("HEAD")
        state["frameworkStarted"] = True
        advance(repo, state, "framework-merge")
        return True
    # A merge of the framework is in progress (the stopped conflict a human resumes).
    if unmerged_entries(repo):
        print(f"framework conflicts in {FRAMEWORK_PATH}/ (resolve, `git add`, `git commit`, "
              f"then `continue`):")
        for path in sorted(unmerged_entries(repo)):
            print(f"  {path}")
        return False
    # Everything is staged, the human only has not committed yet.
    repo.git("commit", "-q", "--no-edit")
    if commit_parents(repo, "HEAD") != [upstream_merge, framework]:
        raise SyncError(f"the committed framework merge does not have {upstream_merge[:12]} "
                        f"and {framework[:12]} as parents")
    state["frameworkMerge"] = repo.rev("HEAD")
    advance(repo, state, "framework-merge")
    return True


def verify_history(repo: Repo, state: dict, head: str) -> list[str]:
    """Check the chain of merges the sync made, before the branch is fast-forwarded."""
    problems = []
    start, target, framework = state["start"], state["target"], state["framework"]
    upstream_merge = state.get("upstreamMerge")
    if not upstream_merge:
        return ["the state records no upstream merge commit"]
    if commit_parents(repo, upstream_merge) != [start, target]:
        problems.append(f"upstream merge {upstream_merge[:12]} does not have the parents "
                        f"{start[:12]} and {target[:12]}")
    framework_merge = state.get("frameworkMerge")
    if framework_merge:
        if commit_parents(repo, framework_merge) != [upstream_merge, framework]:
            problems.append(f"framework merge {framework_merge[:12]} does not have the parents "
                            f"{upstream_merge[:12]} and {framework[:12]}")
        # muse/ must be the recorded base plus the branch's own delta plus the framework
        # range; that is what a `-Xsubtree` merge produces.
        base_tree = repo.obj(f"{state['previousFramework']}^{{tree}}")
        local = set(repo.diff_paths(base_tree, f"{start}:{FRAMEWORK_PATH}"))
        merged = set(repo.diff_paths(base_tree, f"{framework_merge}:{FRAMEWORK_PATH}"))
        framework_changes = set(repo.diff_paths(base_tree, f"{framework}^{{tree}}"))
        unexpected = sorted(merged - local - framework_changes)
        if unexpected:
            problems.append(f"framework merge {framework_merge[:12]} produced {FRAMEWORK_PATH}/ "
                            f"changes the framework does not have: {unexpected[:3]}")
        outside = [path for path in repo.diff_paths(f"{framework_merge}^1", framework_merge)
                   if not path.startswith(FRAMEWORK_PATH + "/")]
        if outside:
            problems.append(f"framework merge {framework_merge[:12]} changed paths outside "
                            f"{FRAMEWORK_PATH}/: {outside[:3]}")
    expected_tip = framework_merge or upstream_merge
    if head != expected_tip:
        problems.append(f"the sync tip {head[:12]} is not the merge it just made "
                        f"({expected_tip[:12]})")
    for commit, label in ((target, "target"), (framework, "framework"),
                          (upstream_merge, "upstream merge"), (state["previousFramework"],
                                "previous framework")):
        if not is_ancestor(repo, commit, head):
            problems.append(f"the {label} {commit[:12]} is not an ancestor of {head[:12]}")
    current = repo.obj(f"refs/heads/{state['branch']}")
    if current != start:
        problems.append(f"{state['branch']} points at {str(current)[:12]}, not at the sync "
                        f"start {start[:12]}; it moved during the sync")
    return problems


def stage_check(repo: Repo, state: dict) -> bool:
    verify_branch(repo, state["temp"])
    head = repo.rev("HEAD")
    state["head"] = head
    problems = verify_history(repo, state, head)
    trim = load_trim(repo)  # the TRIM of the final tree; check_tree asserts it did not change
    problems += check_tree(repo, trim, state)
    local = repo.diff_paths(f"{state['previousFramework']}^{{tree}}",
                            f"{state['start']}:{FRAMEWORK_PATH}")
    # Without a local framework delta, muse/ must equal the framework tree exactly.
    if not local and repo.obj(f"HEAD:{FRAMEWORK_PATH}") != \
            repo.obj(f"{state['framework']}^{{tree}}"):
        problems.append(f"{FRAMEWORK_PATH}/ differs from framework {state['framework'][:12]}")
    if problems:
        raise SyncError("the sync does not produce the expected result:\n  "
                        + "\n  ".join(problems)
                        + f"\nNothing was fast-forwarded; run `abort` to return to "
                          f"{state['branch']}")
    advance(repo, state, "check")
    return True


def stage_fast_forward(repo: Repo, state: dict) -> bool:
    branch, temp, head = state["branch"], state["temp"], state["head"]
    verify_branch(repo, temp)
    verify_head(repo, head)
    current = repo.rev(f"refs/heads/{branch}")
    if current not in (state["start"], head):
        raise SyncError(f"{branch} points at {current[:12]}, but the sync started at "
                        f"{state['start'][:12]}; refusing to fast-forward. Run `abort` to "
                        f"return to {branch} and sync again")
    if current != head:
        repo.git("checkout", "-q", branch)
        repo.git("merge", "-q", "--ff-only", head)
    verify_branch(repo, branch)
    verify_head(repo, head)
    advance(repo, state, "fast-forward")
    return True


def stage_cleanup(repo: Repo, state: dict) -> bool:
    branch, temp, head = state["branch"], state["temp"], state["head"]
    verify_branch(repo, branch)
    verify_head(repo, head)
    if repo.branch_exists(temp):
        repo.git("branch", "-q", "-D", temp)
    state_path(repo).unlink(missing_ok=True)
    state["stage"] = "cleanup"  # not saved: the state file is gone
    print(f"{branch} -> {head}")
    print(summary(state))
    stop_for_test("cleanup")
    return True


STAGE_HANDLERS = {
    "temp-branch": stage_temp_branch,
    "upstream-merge": stage_upstream_merge,
    "upstream-commit": stage_upstream_commit,
    "framework-merge": stage_framework_merge,
    "check": stage_check,
    "fast-forward": stage_fast_forward,
    "cleanup": stage_cleanup,
}


def finish(repo: Repo, state: dict) -> int:
    """Run every stage after the recorded one; returns the process exit code."""
    while state["stage"] != "cleanup":
        following = STAGES[STAGES.index(state["stage"]) + 1]
        if not STAGE_HANDLERS[following](repo, state):
            return 2  # a conflict needs a human; the stage was not recorded as done
    return 0


def summary(state: dict) -> str:
    report = state["report"]
    lines = [f"upstream {state['base'][:12]}..{state['target'][:12]}",
             f"framework {state['previousFramework'][:12]}..{state['framework'][:12]}",
             f"upstream changes discarded on trimmed paths: {len(report['discardedChanges'])}"]
    lines += [f"  {p}" for p in report["discardedChanges"]]
    lines.append(f"new upstream files dropped under trimmed prefixes: "
                 f"{len(report['droppedAdditions'])}")
    lines += [f"  {p}" for p in report["droppedAdditions"]]
    return "\n".join(lines)


# ---------------------------------------------------------------------------

def cmd_absorb(repo: Repo, args) -> int:
    if not repo.clean() or repo.merging():
        raise SyncError("working tree is not clean")
    framework = args.framework or repo.gitlink("HEAD", FRAMEWORK_PATH)
    if not framework:
        raise SyncError(f"HEAD has no `{FRAMEWORK_PATH}` gitlink; already absorbed?")
    framework = repo.rev(framework)
    if not repo.ok("merge-base", "--is-ancestor", framework, args.framework_ref):
        raise SyncError(f"{framework} is not on {args.framework_ref}")
    trim = load_trim(repo)
    repo.git("merge", "-q", "-s", "ours", "--no-commit", "--allow-unrelated-histories", framework)
    drop = [FRAMEWORK_PATH]
    drop += [path for mode, path in index_entries(repo)
             if mode == GITLINK and trimmed(path, trim) and path != FRAMEWORK_PATH]
    for path in drop:
        repo.git("rm", "-q", "--cached", "--", path)
        remove_worktree_path(repo, path)
    for _mode, path in index_entries(repo):
        if trimmed(path, trim):
            repo.git("rm", "-q", "--cached", "--", path)
            remove_worktree_path(repo, path)
    repo.git("read-tree", f"--prefix={FRAMEWORK_PATH}/", "-u", framework)
    repo.git("commit", "-q", "-m",
             f"Absorb muse_framework {framework} at {FRAMEWORK_PATH}/ (subtree)\n\n"
             f"{FRAMEWORK_TRAILER}: {framework}")
    print(f"absorbed {framework} at {FRAMEWORK_PATH}/ -> {repo.rev('HEAD')}")
    return 0


def cmd_sync(repo: Repo, args) -> int:
    state = load_state(repo)
    if state:
        print("found an unfinished sync; aborting it first")
        abort_sync(repo, state)
    elif temp_branches(repo):
        raise SyncError("no sync state file" + leftover_hint(repo))
    if not repo.clean() or repo.merging():
        raise SyncError("working tree is not clean")
    branch = repo.current_branch()
    if branch.startswith("sync/"):
        raise SyncError(f"HEAD is on the temporary branch {branch}, which has no sync state; "
                        f"check out your branch and delete it, then run `sync` again")
    start = repo.rev("HEAD")
    target = repo.rev(args.target or args.upstream_ref)
    if not repo.ok("merge-base", "--is-ancestor", target, args.upstream_ref):
        raise SyncError(f"target {target} is not on {args.upstream_ref}")
    if is_ancestor(repo, target, "HEAD"):
        print(f"{branch} already contains {target}")
        return 0
    base = repo.git("merge-base", "HEAD", target).strip()
    check_trim_policy(repo, base, target, branch)
    framework = repo.gitlink(target, FRAMEWORK_PATH)
    if not framework:
        raise SyncError(f"target {target} has no `{FRAMEWORK_PATH}` gitlink")
    if not repo.ok("cat-file", "-e", f"{framework}^{{commit}}") or \
            not is_ancestor(repo, framework, args.framework_ref):
        raise SyncError(f"target {target} pins framework {framework}, which is not on "
                        f"{args.framework_ref}; pick another upstream commit")
    previous = recorded_framework(repo, start)
    if not is_ancestor(repo, previous, framework):
        raise SyncError(f"the framework mirror was rewritten or diverged: {previous[:12]} (the "
                        f"framework commit the last framework merge on {branch} recorded) is "
                        f"not an ancestor of the pinned {framework[:12]} (target "
                        f"{target[:12]}). Refusing to merge across a rewritten framework "
                        f"history; nothing was changed")
    if not is_ancestor(repo, previous, args.framework_ref):
        raise SyncError(f"the framework mirror was rewritten: {previous[:12]} (recorded by the "
                        f"last framework merge on {branch}) is not on {args.framework_ref}; "
                        f"nothing was changed")
    state = {"version": STATE_VERSION, "branch": branch, "temp": f"sync/{target[:12]}",
             "start": start, "target": target, "base": base, "framework": framework,
             "previousFramework": previous, "stage": "start", "mergeStarted": False,
             "frameworkStarted": False, "upstreamMerge": None, "frameworkMerge": None,
             "head": None,
             "report": {"discardedChanges": [], "droppedAdditions": []}}
    save_state(repo, state)  # before the temporary branch exists
    stop_for_test("start")
    return finish(repo, state)


def cmd_continue(repo: Repo, args) -> int:
    state = load_state(repo)
    if not state:
        raise SyncError("no sync in progress" + leftover_hint(repo))
    return finish(repo, state)


def cmd_abort(repo: Repo, args) -> int:
    state = load_state(repo)
    if not state:
        raise SyncError("no sync in progress" + leftover_hint(repo))
    abort_sync(repo, state)
    print("sync aborted")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-C", dest="repo", type=Path, default=Path.cwd())
    parser.add_argument("--upstream-ref", default=UPSTREAM_REF)
    parser.add_argument("--framework-ref", default=FRAMEWORK_REF)
    sub = parser.add_subparsers(dest="command", required=True)
    absorb = sub.add_parser("absorb")
    absorb.add_argument("--framework")
    sync = sub.add_parser("sync")
    sync.add_argument("--target")
    sub.add_parser("continue")
    sub.add_parser("abort")
    args = parser.parse_args()
    repo = Repo(Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=args.repo,
                                    capture_output=True, text=True, check=True).stdout.strip()))
    try:
        return {"absorb": cmd_absorb, "sync": cmd_sync, "continue": cmd_continue,
                "abort": cmd_abort}[args.command](repo, args)
    except SyncError as error:
        print(f"sync-upstream: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
