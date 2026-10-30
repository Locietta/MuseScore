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

Threat model: this tool runs only in a dedicated fork clone, operated by the
orchestrator. It is not defended against a concurrent editor of the same clone. What
it does guarantee: `absorb` and `sync` refuse to start unless
`git status --porcelain --ignored=matching --untracked-files=all` is empty - staged,
unstaged, untracked *and ignored* files all count, because a merge overwrites an
untracked or ignored file at the same path without a word. They also refuse a gitlink
directory that already holds files (`git status` does not look inside a gitlink, so a
stray file in `muse/` or `muse_deps/` would be deleted without notice). It never uses
`git checkout -f`, and `continue`/`abort` refuse to mutate anything when the worktree
holds changes the sync did not leave behind (a stopped conflict's own paths are the
only exception).

Abort semantics: `abort` discards *everything* the sync and the human did since the
sync started - the temporary branch, the in-progress merge and any resolution work the
human already staged or committed - and restores the branch the sync started from. It
says so in its output. `continue` is the command that keeps a resolution.

A sync runs on a temporary branch `sync/<sha>` and fast-forwards the branch only after
both merges and the tree checks pass, so the branch only ever moves forward (by that
single `git merge --ff-only`). Nothing is pushed.

State lives in .git/sync-upstream.json. It is written atomically (temp file +
os.replace) *before* the mutation of the next stage and records what the sync expects:

  start -> temp-branch -> upstream-merge -> upstream-commit
        -> framework-merge -> check -> fast-forward -> cleanup

  stage            the last stage that finished
  merging          the commit the next `git merge` will merge (persisted before the
                   command runs, so `abort` can tell our merge from someone else's)
  mergeOutcome     null (command not run or crashed) / ready / conflicts / failed;
                   saved together with the worktree snapshot and unmerged paths the
                   merge left behind, so a crash right after it is recognised
  removals         planned trimmed-path removals (index entries and report entries);
                   `abort` deletes the planned paths the restored branch does not track
  conflicts        paths left unmerged for a human by the current stop
  status           the worktree state the tool leaves behind at that stop
  preResolutionTree  index snapshot taken before the first human resolution
  upstreamMerge / frameworkMerge / head  the commits the sync made

Every stage is idempotent and verifies branch and commit identity before it acts.
`continue` resumes at any boundary, including "state saved, temporary branch not yet
created", "merge command finished, outcome just saved" and "fast-forward done, state
not yet updated". `abort` follows one rule: when the branch still points at the recorded
start it undoes our own merge (only if `MERGE_HEAD` is the commit `merging` recorded),
returns, and deletes the untracked leftovers of the removal plan; when the branch
already points at the recorded final head it just cleans up; anything else is refused
with the exact commands to repair the repository by hand.

The test hook `SYNC_UPSTREAM_TEST_STOP_AFTER` accepts the stage names above plus
`upstream-merge-outcome` and `framework-merge-outcome` (right after each merge outcome
is persisted) and `upstream-commit-pending` (right before the tool makes the upstream
merge commit, once the resolutions are recorded); `SYNC_UPSTREAM_TEST_STOP_AFTER_RM=<n>`
stops between the index and worktree half of the n-th planned removal.

Merge commits are made by the tool, with a provenance trailer: `Sync-Upstream: <target>`
on the upstream merge and `Framework-Commit: <framework>` on the framework merge. A
merge commit with the expected parents but without (or with a wrong) trailer was made
by someone else and is refused, with instructions, instead of adopted. The framework
trailer also names the previously incorporated framework commit for the next sync; it
is read from the newest such commit on the first-parent chain (falling back to the
subject of the pre-trailer absorb commit 7150f2b05e) rather than inferred with
`merge-base`, and is used as the explicit merge-base expectation.

Conflict handling:
  * The `muse` gitlink bump arrives as ort's displaced `muse~<sha>` entry. It is
    dropped only when it is a 160000 gitlink whose stage-3 SHA is the target's muse
    gitlink, and the name exists neither in the target tree nor in ours or the merge
    base (otherwise it is a real conflict for a human).
  * Modify/delete conflicts on TRIM paths keep the deletion and are reported; new
    upstream files under trimmed prefixes are deleted and reported. Both are planned
    and written to the state file before anything is removed, and the plan is replayed
    when a run is resumed (so an interruption can neither lose a report entry nor leave
    a half-removed file behind).
  * Everything else stops for a human.

TRIM belongs to `main`: a sync never changes it, refuses a target that adds or changes
it, and asserts the final tree kept it.

Every git command runs with `--literal-pathspecs`, so a trimmed path such as
`item[ab].txt` is removed as that name and never as a glob. Path listings are read from
NUL-delimited output (`-z`). stdout/stderr are reconfigured to UTF-8 with
`errors="backslashreplace"` at startup, so a legacy console code page can neither
mangle a report nor turn a success into a traceback.

Deliberate limits (not defects):
  * The tool does not prove that a human conflict resolution kept every other hunk.
    It prints, in the final report, the paths whose resolution differs from the merge
    sides (the diff between the pre-resolution index and the final tree, minus the
    paths the tool itself dropped), so a reviewer can inspect them.
  * A fast-forwardable upstream target is not supported: upstream never contains our
    commits, so every sync produces a merge commit.
  * Framework commits that reached the branch through some other parent are refused
    (the tool cannot tell whether their content is really in `muse/`), not merged.
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
UPSTREAM_TRAILER = "Sync-Upstream"
STATE_VERSION = 3
# Stage names double as the values SYNC_UPSTREAM_TEST_STOP_AFTER accepts.
STAGES = ("start", "temp-branch", "upstream-merge", "upstream-commit",
          "framework-merge", "check", "fast-forward", "cleanup")
# Stages whose advance leaves staged worktree changes (a `--no-commit` merge in
# progress); the worktree state after every other stage is clean.
DIRTY_STAGES = ("upstream-merge",)
ABSORB_SUBJECT = re.compile(r"^Absorb muse_framework ([0-9a-f]{40}) at muse/")
TEMP_BRANCH = re.compile(r"^sync/[0-9a-f]{12}$")
SHA1 = re.compile(r"[0-9a-f]{40}")
ISSUE_ENCODING = (AttributeError, ValueError)


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
        # Literal pathspecs everywhere: a trimmed path is a name, never a glob.
        return subprocess.run(["git", "--literal-pathspecs", *args], cwd=self.root,
                              capture_output=True, text=True, encoding="utf-8")

    def git(self, *args: str, check: bool = True) -> str:
        result = self.run(*args)
        if check and result.returncode != 0:
            raise SyncError(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
        if result.stderr.strip() and os.environ.get("SYNC_UPSTREAM_VERBOSE"):
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


def run_git_env(repo: Repo, env: dict[str, str], *args: str, stdin: bytes | None = None) -> str:
    """Run git with a custom environment (a scratch index), returning stdout."""
    result = subprocess.run(["git", "--literal-pathspecs", *args], cwd=repo.root, env=env,
                            input=stdin, capture_output=True)
    if result.returncode != 0:
        raise SyncError(f"git {' '.join(args)} failed:\n"
                        f"{result.stderr.decode('utf-8', 'replace').strip()}")
    return result.stdout.decode("utf-8", "replace")


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


def status_entries(repo: Repo, include_ignored: bool = False) -> list[tuple[str, str]]:
    """(code, path) for staged, unstaged, untracked (and ignored) changes.

    `include_ignored` adds `--ignored=matching`, so a dedicated clone can also refuse
    ignored files, which a merge would overwrite without notice.
    """
    extra = ["--ignored=matching"] if include_ignored else []
    raw = split_z(repo.git("status", "--porcelain", "-z", "--untracked-files=all", *extra))
    entries = []
    renamed = False
    for record in raw:
        if renamed:  # `-z` puts the source path of a rename in its own record
            renamed = False
            continue
        code, path = record[:2], record[3:]
        entries.append((code, path))
        renamed = code[:1] in ("R", "C")
    return entries


def merge_outcome(repo: Repo, result: subprocess.CompletedProcess) -> str:
    """Classify a `git merge` run: clean, ready (--no-commit), conflicts or failed."""
    if result.returncode == 0:
        return "ready" if repo.merging() else "clean"
    if result.returncode == 1 and repo.merging() and unmerged_entries(repo):
        return "conflicts"
    return "failed"


def merge_failure(result: subprocess.CompletedProcess) -> str:
    detail = result.stderr.strip() or result.stdout.strip()
    return (f"git merge failed (exit {result.returncode}):\n{indent(detail)}\n"
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
    state["status"] = ([[code, path] for code, path in status_entries(repo)]
                       if stage in DIRTY_STAGES else [])
    save_state(repo, state)
    stop_for_test(stage)


def stop_for_test(stage: str) -> None:
    # Test hook: simulate an interruption right after a stage.
    if os.environ.get("SYNC_UPSTREAM_TEST_STOP_AFTER") == stage:
        raise SystemExit(f"[test] stopped after {stage}")


def stop_for_test_after_rm(count: int) -> None:
    # Test hook: simulate a crash between `git rm --cached` and the worktree removal.
    if os.environ.get("SYNC_UPSTREAM_TEST_STOP_AFTER_RM") == str(count):
        raise SystemExit(f"[test] stopped after removing index entry {count}")


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


# --- preconditions --------------------------------------------------------------

def filled_gitlink_dirs(repo: Repo) -> list[str]:
    """Gitlink directories that already hold files, which a run would delete.

    `git status` does not descend into a gitlink, so a file inside `muse/` or
    `muse_deps/` (the directories a run removes) is invisible even with `--ignored`.
    """
    found = []
    for mode, path in index_entries(repo):
        if mode != GITLINK:
            continue
        target = repo.root / path
        if target.is_dir() and any(target.iterdir()):
            found.append(path)
    return found


def require_clean(repo: Repo) -> None:
    """Refuse to start unless nothing in the clone can be lost.

    The clone is dedicated to this tool and never builds, so *any* staged, unstaged,
    untracked or ignored file is refused: a merge overwrites untracked and ignored files
    at the same path without a word. `git status` does not look inside a gitlink either,
    hence the explicit check for non-empty `muse`/`muse_deps` directories.
    """
    dirty = status_entries(repo, include_ignored=True)
    if dirty:
        shown = ", ".join(f"{code} {path}" for code, path in dirty[:6])
        more = f", ... ({len(dirty)} paths)" if len(dirty) > 6 else ""
        ignored = any(code == "!!" for code, _path in dirty)
        hint = (" (ignored files count too: a merge overwrites them without notice)"
                if ignored else "")
        raise SyncError(f"the worktree is not clean: {shown}{more}{hint}. Commit, discard or "
                        f"move those files first; this tool only runs in a dedicated clone "
                        f"and never overwrites work it did not make")
    filled = filled_gitlink_dirs(repo)
    if filled:
        raise SyncError(f"the gitlink director{'ies' if len(filled) > 1 else 'y'} "
                        f"{', '.join(f'{p}/' for p in filled)} already holds files; this run "
                        f"would delete them (a dedicated clone keeps its submodules "
                        f"unchecked-out). Move them away first")


def require_only_expected_changes(repo: Repo, state: dict, action: str) -> None:
    """`continue`/`abort` refuse to touch changes the sync did not leave behind.

    Allowed: the paths a stopped conflict left unresolved (the user's resolution), and
    whatever the tool itself left behind at the recorded stop. When our own merge is in
    progress but its outcome was never recorded (the process died inside the merge
    command), its staged changes are accepted as well - only untracked files are not.
    """
    allowed = set(state.get("conflicts") or [])
    allowed |= {entry["path"] for entry in state.get("removals") or []}
    baseline = {(code, path) for code, path in state.get("status") or []}
    merge = repo.merge_head()
    crashed_merge = bool(merge) and state.get("merging") == merge and \
        state.get("mergeOutcome") is None
    unexpected = []
    for code, path in status_entries(repo):
        if path in allowed or (code, path) in baseline:
            continue
        if crashed_merge and code != "??":
            continue
        unexpected.append(f"{code} {path}")
    if unexpected:
        shown = ", ".join(unexpected[:6])
        more = f", ... ({len(unexpected)} paths)" if len(unexpected) > 6 else ""
        raise SyncError(f"`{action}` refuses: the worktree holds changes this sync did not "
                        f"make: {shown}{more}. Nothing was changed. Commit, discard or stash "
                        f"them and run `{action}` again")


def abort_by_hand(repo: Repo, state: dict) -> str:
    return (f"\nNothing was changed. To repair by hand:\n"
            f"  git checkout {state['branch']}\n"
            f"  git branch -D {state['temp']}     # if it still exists\n"
            f"  rm {state_path(repo)}")


# --- abort ----------------------------------------------------------------------

def drop_planned_removals(repo: Repo, state: dict) -> list[str]:
    """Finish the recorded removal plan in the worktree; returns what is still dirty.

    `abort` restores the branch, but the sync may have removed part of a trimmed path
    from the index only - which leaves it as an untracked file that the branch does not
    have. Deleting every planned path that the restored branch does not track makes the
    worktree clean again. A planned path that *is* tracked is never touched.
    """
    plan = [entry["path"] for entry in state.get("removals") or []]
    if not plan:
        return []
    tracked = {path for _mode, path in index_entries(repo)}
    for path in plan:
        if path not in tracked:
            remove_worktree_path(repo, path)
    return [f"{code} {path}" for code, path in status_entries(repo, include_ignored=True)]


def abort_sync(repo: Repo, state: dict) -> None:
    """Drop the recorded sync. One rule: the branch must be at the start or at the head."""
    branch, temp, start = state["branch"], state["temp"], state["start"]
    head = state.get("head")
    branch_tip = repo.obj(f"refs/heads/{branch}")
    if branch_tip is None:
        raise SyncError(f"{branch} does not exist" + abort_by_hand(repo, state))
    if head and branch_tip == head:
        # The fast-forward already happened; only the cleanup is left.
        require_only_expected_changes(repo, state, "abort")
        if repo.current_branch() != branch:
            raise SyncError(f"the sync already fast-forwarded {branch} to {head[:12]}, but HEAD "
                            f"is on {repo.current_branch()}" + abort_by_hand(repo, state))
        if repo.branch_exists(temp) and repo.rev(f"refs/heads/{temp}") != head:
            raise SyncError(f"{temp} is not at the recorded head {head[:12]}"
                            + abort_by_hand(repo, state))
        leftover = drop_planned_removals(repo, state)
        if leftover:
            raise SyncError(f"the sync left the worktree dirty: {', '.join(leftover[:6])}. "
                            f"The state is kept; clean that up and run `abort` again")
        if repo.branch_exists(temp):
            repo.git("branch", "-q", "-D", temp)
        state_path(repo).unlink(missing_ok=True)
        return
    if branch_tip != start:
        raise SyncError(f"{branch} is at {branch_tip[:12]}, but this sync started at "
                        f"{start[:12]} and recorded no fast-forward; it cannot tell what "
                        f"moved the branch" + abort_by_hand(repo, state))
    merge = repo.merge_head()
    if merge:
        if not state.get("merging") or merge != state["merging"]:
            raise SyncError(f"a merge of {merge[:12]} is in progress on {repo.current_branch()}, "
                            f"but this sync did not start it (the merge it started is "
                            f"{state.get('merging') or 'none'}); refusing to abort your merge. "
                            f"It is untouched: finish or abort it yourself, then run `abort` "
                            f"again")
        require_only_expected_changes(repo, state, "abort")
        repo.git("merge", "--abort")
    require_only_expected_changes(repo, state, "abort")
    current = repo.current_branch()
    if current != branch:
        if repo.branch_exists(temp) and current != temp:
            raise SyncError(f"HEAD is on {current}, which is neither {branch} nor {temp}"
                            + abort_by_hand(repo, state))
        repo.git("checkout", "-q", branch)  # never forced
    leftover = drop_planned_removals(repo, state)
    if leftover:
        raise SyncError(f"the sync removed part of a trimmed path and the worktree is still "
                        f"dirty afterwards: {', '.join(leftover[:6])}. The state is kept; "
                        f"clean that up and run `abort` again")
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
    """A sync never changes TRIM: upstream may neither add nor modify it."""
    before = repo.obj(f"{base}:{TRIM_FILE}")
    after = repo.obj(f"{target}:{TRIM_FILE}")
    if before == after:
        return
    action = "adds a root" if before is None else "changes"
    raise SyncError(f"{target[:12]} {action} {TRIM_FILE} "
                    f"({before or '<absent>'} -> {after or '<absent>'}); a sync never changes "
                    f"{TRIM_FILE} (only normal commits on {branch} do). Nothing was changed.")


# --- merge commit metadata ------------------------------------------------------

def trailer_value(message: str, key: str) -> str | None:
    """The SHA a `<key>: <sha>` trailer line of a commit message records, if any."""
    for line in reversed(message.splitlines()):
        name, sep, value = line.partition(":")
        if sep and name.strip() == key:
            value = value.strip()
            if SHA1.fullmatch(value):
                return value
            raise SyncError(f"commit message has a malformed {key} line: {line!r}")
    return None


def framework_from_message(message: str) -> str | None:
    """The `Framework-Commit` trailer, or the subject of the pre-trailer absorb commit.

    The first absorb commit (7150f2b05e) predates the trailer and names the framework
    commit in its subject; keep reading that so the existing main stays usable.
    """
    recorded = trailer_value(message, FRAMEWORK_TRAILER)
    if recorded:
        return recorded
    match = ABSORB_SUBJECT.match(message)
    return match.group(1) if match else None


def commit_message(repo: Repo, commit: str) -> str:
    return repo.git("log", "-1", "--format=%B", commit)


def resolutions_section(resolutions: list[str]) -> str:
    """The report block for conflict resolutions, as printed and recorded in commits."""
    lines = [f"conflict resolutions that differ from the merge sides: {len(resolutions)}"]
    lines += [f"  {path}" for path in resolutions]
    return "\n".join(lines)


def upstream_merge_message(state: dict) -> str:
    return (f"Merge upstream/musescore {state['target']}\n\n{summary(state)}\n\n"
            f"{UPSTREAM_TRAILER}: {state['target']}")


def framework_merge_message(state: dict) -> str:
    """The framework merge repeats the resolutions; the trimmed paths stay in the first."""
    resolutions = state["report"].get("resolutions") or []
    body = f"{resolutions_section(resolutions)}\n\n" if resolutions else ""
    return (f"Merge upstream/muse-framework {state['framework']} into {FRAMEWORK_PATH}/\n\n"
            f"{body}{FRAMEWORK_TRAILER}: {state['framework']}")


def refuse_foreign_merge(repo: Repo, state: dict, commit: str, kind: str, key: str,
                         expected: str, parent: str,
                         parents: list[str] | None = None) -> None:
    recorded = trailer_value(commit_message(repo, commit), key)
    if recorded != expected:
        why = f"its {key} records {recorded or '<nothing>'}, expected {expected[:12]}"
    else:
        why = (f"its parents are [{', '.join(p[:12] for p in parents or [])}], expected "
               f"{parent[:12]} and {expected[:12]}")
    raise SyncError(f"{commit[:12]} is a {kind} merge commit this sync did not make: {why}. "
                    f"The tool commits these merges itself so their provenance is recorded.\n"
                    f"Nothing else was changed. Either redo it:\n"
                    f"  git reset --hard {parent[:12]}\n"
                    f"  git merge ...    # let the tool do it: run `continue`\n"
                    f"or run `abort` to return to {state['branch']}")


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
    ours = repo.obj(f"{commit}^1:{FRAMEWORK_PATH}")
    if ours is None:
        raise SyncError(f"{commit[:12]} records framework {framework[:12]}, but has no "
                        f"{FRAMEWORK_PATH}/ on its first parent, so its "
                        f"{FRAMEWORK_PATH}/ cannot come from that framework tree")
    from_base = set(repo.diff_paths(tree, merged))
    from_ours = set(repo.diff_paths(tree, ours))
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


# --- worktree and index maintenance ---------------------------------------------

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


def apply_removals(repo: Repo, state: dict) -> None:
    """Replay the planned removals, then drop the plan.

    The plan (paths plus the report entry each removal produces) is written to the state
    file before the first removal, so resuming after an interruption between
    `git rm --cached` and the worktree deletion finishes the job and keeps the report.
    """
    plan = state.get("removals") or []
    if not plan:
        return
    present = {path for _mode, path in index_entries(repo)}
    for number, entry in enumerate(plan, 1):
        path = entry["path"]
        if path in present:
            repo.git("rm", "-q", "--cached", "--", path)
            stop_for_test_after_rm(number)
        # `muse` itself is our subtree; only ort's displaced conflict directory may go.
        if not (entry.get("keepDir") and (repo.root / path).is_dir()):
            remove_worktree_path(repo, path)
        if entry.get("report"):
            note(state["report"], entry["report"], path)
    state["removals"] = []
    save_state(repo, state)


def plan_removals(state: dict, plan: list[dict]) -> None:
    """Persist the removals and their report entries before anything is removed."""
    for entry in plan:
        if entry.get("report"):
            note(state["report"], entry["report"], entry["path"])
    state["removals"] = plan


def capture_index_tree(repo: Repo, entries: dict[str, dict[int, tuple[str, str]]]) -> str:
    """A tree object for the current index with conflicted paths taken from our side.

    The final report diffs this against the sync tip, so a reviewer sees which paths a
    human resolution changed relative to the merge sides (the tool does not prove that a
    resolution kept everything else).
    """
    scratch = repo.git_dir / "sync-upstream-index.tmp"
    shutil.copyfile(repo.git_dir / "index", scratch)
    env = dict(os.environ, GIT_INDEX_FILE=str(scratch))
    try:
        records = []
        for path in sorted(entries):
            side = entries[path].get(2) or entries[path].get(3)
            if side:
                records.append(f"{side[0]} {side[1]}\t{path}".encode("utf-8"))
        if records:
            run_git_env(repo, env, "update-index", "-z", "--index-info",
                        stdin=b"\0".join(records) + b"\0")
        return run_git_env(repo, env, "write-tree").strip()
    finally:
        scratch.unlink(missing_ok=True)


def index_tree(repo: Repo) -> str:
    """The tree the next commit records; the index is fully resolved when this runs."""
    return repo.git("write-tree").strip()


def resolution_paths(repo: Repo, state: dict, tip: str,
                     merge_changes: set[str] | None = None) -> list[str]:
    """Paths whose human resolution differs from the merge sides.

    `tip` is the tree-ish to compare the pre-resolution snapshot against: the index the
    tool is about to commit (so the merge commit message records the resolutions) or the
    sync tip (the final report). `merge_changes` are the paths the merge that produced
    `tip` changed by itself; they are not resolutions unless a human resolved that path.
    """
    tree = state.get("preResolutionTree")
    if not tree:
        return []
    changed = set(repo.diff_paths(tree, tip))
    ignored = set(state["report"]["discardedChanges"])
    ignored |= set(state["report"]["droppedAdditions"])
    ignored |= set(state.get("linkDrops") or [])
    if merge_changes:
        ignored |= merge_changes - set(state.get("conflicts") or [])
    return sorted(changed - ignored)


# --- conflict handling ----------------------------------------------------------

def classify_link_conflict(repo: Repo, state: dict,
                           entries: dict[str, dict[int, tuple[str, str]]]
                           ) -> tuple[str | None, list[str]]:
    """The ort-displaced `muse` gitlink entry, if it provably is that, and the rest.

    Only a single `muse`/`muse~*` entry qualifies, and only when its stage 3 is a
    160000 gitlink carrying the target's `muse` SHA, and the name exists neither in the
    target tree nor in ours or the merge base - i.e. it is exactly the name ort made up
    for the displaced gitlink. Anything else is a conflict for a human.
    """
    candidates = sorted(path for path in entries
                        if path == FRAMEWORK_PATH or path.startswith(FRAMEWORK_PATH + "~"))
    if not candidates:
        return None, []
    accepted = None
    expected = repo.gitlink(state["target"], FRAMEWORK_PATH)
    if len(candidates) == 1:
        path = candidates[0]
        if path != FRAMEWORK_PATH and expected and entries[path].get(3) == (GITLINK, expected):
            absent = all(repo.obj(f"{ref}:{path}") is None
                         for ref in (state["target"], state["start"], state["base"]))
            if absent:
                accepted = path
    return accepted, [path for path in candidates if path != accepted]


def resolve_upstream_merge(repo: Repo, state: dict) -> list[str]:
    """Resolve the conflicts a sync expects; return the paths that need a human."""
    trim = load_trim(repo)
    apply_removals(repo, state)  # finish a plan an earlier run did not complete
    entries = unmerged_entries(repo)
    unresolved: list[str] = []
    plan: list[dict] = []
    accepted, refused = classify_link_conflict(repo, state, entries)
    if accepted:
        state["linkDrops"] = sorted(set(state.get("linkDrops") or []) | {accepted})
        plan.append({"path": accepted, "report": None, "keepDir": accepted == FRAMEWORK_PATH})
    unresolved += refused
    for path, stages in sorted(entries.items()):
        if path == accepted or path in refused:
            continue
        if trimmed(path, trim) and 2 not in stages:
            # Deleted by us (trimmed), changed upstream: keep it deleted, but report it.
            plan.append({"path": path, "report": "discardedChanges", "keepDir": False})
        else:
            unresolved.append(path)
    planned = {entry["path"] for entry in plan}
    for path in split_z(repo.git("diff", "--cached", "--no-renames", "--name-only", "-z",
                                 "--diff-filter=A", "HEAD")):
        if trimmed(path, trim) and path not in planned:
            plan.append({"path": path, "report": "droppedAdditions", "keepDir": False})
    if plan:
        plan_removals(state, plan)
        save_state(repo, state)  # plan and report entries are durable before any removal
        apply_removals(repo, state)
    if unresolved:
        state["conflicts"] = sorted(set(state.get("conflicts") or []) | set(unresolved))
        state["status"] = [[code, path] for code, path in status_entries(repo)]
        if not state.get("preResolutionTree"):
            state["preResolutionTree"] = capture_index_tree(repo, entries)
        save_state(repo, state)
    return unresolved


def check_tree(repo: Repo, state: dict, trim: list[str]) -> list[str]:
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
        if current != temp:
            repo.git("checkout", "-q", temp)  # never forced: the worktree is verified clean
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
        verify_head(repo, start, "the base of the in-progress upstream merge")
        state["merging"] = target
        state["mergeOutcome"] = "ready"
        save_state(repo, state)
    else:
        parents = commit_parents(repo, "HEAD")
        if parents == [start, target]:
            recorded = trailer_value(commit_message(repo, "HEAD"), UPSTREAM_TRAILER)
            if recorded != target:
                refuse_foreign_merge(repo, state, repo.rev("HEAD"), "upstream",
                                     UPSTREAM_TRAILER, target, start)
            state["upstreamMerge"] = repo.rev("HEAD")
            state["merging"] = None
            state["mergeOutcome"] = None
            advance(repo, state, "upstream-merge")
            return True
        if len(parents) == 2 and parents[1] == target:
            refuse_foreign_merge(repo, state, repo.rev("HEAD"), "upstream", UPSTREAM_TRAILER,
                                 target, start, parents)
        verify_head(repo, start)
        if state.get("mergeOutcome") in ("ready", "conflicts"):
            raise SyncError(
                f"the upstream merge of {target[:12]} is no longer in progress: it was aborted "
                f"(`git merge --abort`) or reset, so this sync cannot be resumed. Nothing was "
                f"committed; run `abort` to return to {branch}, then sync again")
        # No rename detection: relative to the upstream merge base our side "deletes" every
        # trimmed path and "adds" all of muse/, and ort would pair them as bogus renames
        # (observed: a trimmed appshell QML file matched to a framework header). Upstream
        # renames of files we changed then surface as modify/delete conflicts instead.
        state["merging"] = target
        state["mergeOutcome"] = None
        save_state(repo, state)  # intent is durable before the command runs
        result = repo.run("merge", "-q", "--no-commit", "--no-ff", "-s", "ort", "-Xno-renames",
                          target)
        outcome = merge_outcome(repo, result)
        # The outcome, the worktree it left behind and its unmerged paths are written in
        # one save: a crash right after it must not look like foreign changes.
        state["mergeOutcome"] = outcome
        state["status"] = [[code, path] for code, path in status_entries(repo)]
        state["conflicts"] = sorted(set(state.get("conflicts") or [])
                                    | set(unmerged_entries(repo)))
        save_state(repo, state)
        stop_for_test("upstream-merge-outcome")
        if outcome == "failed":
            raise SyncError(merge_failure(result))
        if repo.merge_head() != target:
            raise SyncError(f"`git merge {target[:12]}` left no merge to commit; the state is "
                            f"kept, run `abort` and sync again")
    advance(repo, state, "upstream-merge")
    return True


def stage_upstream_commit(repo: Repo, state: dict) -> bool:
    branch, start, target = state["branch"], state["start"], state["target"]
    verify_branch(repo, state["temp"])
    parents = commit_parents(repo, "HEAD")
    if parents == [start, target]:
        recorded = trailer_value(commit_message(repo, "HEAD"), UPSTREAM_TRAILER)
        if recorded != target:
            refuse_foreign_merge(repo, state, repo.rev("HEAD"), "upstream", UPSTREAM_TRAILER,
                                 target, start)
        state["upstreamMerge"] = repo.rev("HEAD")
        state["merging"] = None
        state["mergeOutcome"] = None
        advance(repo, state, "upstream-commit")
        return True
    if len(parents) == 2 and parents[1] == target:
        refuse_foreign_merge(repo, state, repo.rev("HEAD"), "upstream", UPSTREAM_TRAILER,
                             target, start, parents)
    verify_head(repo, start)
    if repo.merge_head() != target:
        raise SyncError(f"the upstream merge of {target[:12]} is no longer in progress (was it "
                        f"aborted?); the sync cannot be resumed. Run `abort` to return to "
                        f"{branch}, then sync again")
    unresolved = resolve_upstream_merge(repo, state)
    if not unresolved and state.get("preResolutionTree"):
        # The index now holds the merge the tool is about to commit: record the human's
        # resolutions in the state, so this merge commit message and a `continue` after an
        # interruption record the same paths as the final report.
        state["report"]["resolutions"] = resolution_paths(repo, state, index_tree(repo))
    save_state(repo, state)
    if unresolved:
        print("conflicts that need a human (resolve, `git add`, then `continue`):")
        for path in unresolved:
            print(f"  {path}")
        return False
    problems = check_tree(repo, state, load_trim(repo))
    if problems:
        raise SyncError("the upstream merge does not produce the expected tree:\n  "
                        + "\n  ".join(problems))
    stop_for_test("upstream-commit-pending")
    repo.git("commit", "-q", "-m", upstream_merge_message(state))
    if commit_parents(repo, "HEAD") != [start, target]:
        raise SyncError(f"the upstream merge commit does not have {start[:12]} and "
                        f"{target[:12]} as parents")
    state["upstreamMerge"] = repo.rev("HEAD")
    state["merging"] = None
    state["mergeOutcome"] = None
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
    if framework == previous:
        state["frameworkMerge"] = None
        state["merging"] = None
        state["mergeOutcome"] = None
        advance(repo, state, "framework-merge")
        return True
    merge = repo.merge_head()
    if merge:
        if merge != framework:
            raise SyncError(f"a merge of {merge[:12]} is in progress, but this sync merges the "
                            f"framework {framework[:12]}; nothing was changed. Finish or abort "
                            f"that merge first")
        if unmerged_entries(repo):
            state["conflicts"] = sorted(set(state.get("conflicts") or [])
                                        | set(unmerged_entries(repo)))
            state["status"] = [[code, path] for code, path in status_entries(repo)]
            if not state.get("preResolutionTree"):
                state["preResolutionTree"] = capture_index_tree(repo, unmerged_entries(repo))
            save_state(repo, state)
            print(f"framework conflicts in {FRAMEWORK_PATH}/ (resolve, `git add`, then "
                  f"`continue`; the tool makes the merge commit):")
            for path in sorted(unmerged_entries(repo)):
                print(f"  {path}")
            return False
        # The human resolved and staged everything; the tool makes the merge commit.
        require_only_expected_changes(repo, state, "continue")
        tree = index_tree(repo)
        # What the human resolved is a resolution; what the framework merge brought by
        # itself is not. Recorded before the commit, so the message matches the report.
        state["report"]["resolutions"] = resolution_paths(
            repo, state, tree, set(repo.diff_paths(upstream_merge, tree)))
        save_state(repo, state)
        repo.git("commit", "-q", "-m", framework_merge_message(state))
    else:
        parents = commit_parents(repo, "HEAD")
        if parents == [upstream_merge, framework]:
            recorded = trailer_value(commit_message(repo, "HEAD"), FRAMEWORK_TRAILER)
            if recorded != framework:
                refuse_foreign_merge(repo, state, repo.rev("HEAD"), "framework",
                                     FRAMEWORK_TRAILER, framework, upstream_merge)
            state["frameworkMerge"] = repo.rev("HEAD")
            state["merging"] = None
            state["mergeOutcome"] = None
            advance(repo, state, "framework-merge")
            return True
        if len(parents) == 2 and parents[1] == framework:
            refuse_foreign_merge(repo, state, repo.rev("HEAD"), "framework",
                                 FRAMEWORK_TRAILER, framework, upstream_merge, parents)
        verify_head(repo, upstream_merge)
        if state.get("mergeOutcome") in ("ready", "conflicts"):
            raise SyncError(
                f"the framework merge of {framework[:12]} is no longer in progress: it was "
                f"aborted (`git merge --abort`) or reset. Nothing was committed; run `abort` "
                f"to return to {branch}, then sync again")
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
        state["merging"] = framework
        state["mergeOutcome"] = None
        save_state(repo, state)  # intent is durable before the command runs
        result = repo.run("merge", "-q", "--no-ff", "-s", "ort", f"-Xsubtree={FRAMEWORK_PATH}",
                          "-m", framework_merge_message(state), framework)
        outcome = merge_outcome(repo, result)
        # As for the upstream merge: outcome, worktree state and unmerged paths in one save.
        state["mergeOutcome"] = outcome
        state["status"] = [[code, path] for code, path in status_entries(repo)]
        state["conflicts"] = sorted(set(state.get("conflicts") or [])
                                    | set(unmerged_entries(repo)))
        if outcome == "conflicts" and not state.get("preResolutionTree"):
            state["preResolutionTree"] = capture_index_tree(repo, unmerged_entries(repo))
        save_state(repo, state)
        stop_for_test("framework-merge-outcome")
        if outcome == "failed":
            raise SyncError(merge_failure(result))
        if outcome == "conflicts":
            print(f"framework conflicts in {FRAMEWORK_PATH}/ (resolve, `git add`, then "
                  f"`continue`; the tool makes the merge commit):")
            for path in sorted(unmerged_entries(repo)):
                print(f"  {path}")
            return False
    if commit_parents(repo, "HEAD") != [upstream_merge, framework]:
        raise SyncError(f"the framework merge commit does not have {upstream_merge[:12]} and "
                        f"{framework[:12]} as parents")
    if trailer_value(commit_message(repo, "HEAD"), FRAMEWORK_TRAILER) != framework:
        raise SyncError(f"the framework merge commit lost its {FRAMEWORK_TRAILER} trailer")
    state["frameworkMerge"] = repo.rev("HEAD")
    state["merging"] = None
    state["mergeOutcome"] = None
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
    if trailer_value(commit_message(repo, upstream_merge), UPSTREAM_TRAILER) != target:
        problems.append(f"upstream merge {upstream_merge[:12]} lost its {UPSTREAM_TRAILER} "
                        f"trailer")
    framework_merge = state.get("frameworkMerge")
    if framework_merge:
        if commit_parents(repo, framework_merge) != [upstream_merge, framework]:
            problems.append(f"framework merge {framework_merge[:12]} does not have the parents "
                            f"{upstream_merge[:12]} and {framework[:12]}")
        if trailer_value(commit_message(repo, framework_merge), FRAMEWORK_TRAILER) != framework:
            problems.append(f"framework merge {framework_merge[:12]} lost its "
                            f"{FRAMEWORK_TRAILER} trailer")
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
    framework_merge = state.get("frameworkMerge")
    own_changes = (set(repo.diff_paths(f"{framework_merge}^1", framework_merge))
                   if framework_merge else None)
    state["report"]["resolutions"] = resolution_paths(repo, state, head, own_changes)
    problems = verify_history(repo, state, head)
    trim = load_trim(repo)  # the TRIM of the final tree; check_tree asserts it did not change
    problems += check_tree(repo, state, trim)
    for code, path in status_entries(repo):
        problems.append(f"the worktree is not clean at the sync tip: {code} {path}")
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
    branch_tip = repo.obj(f"refs/heads/{branch}")
    temp_tip = repo.rev(f"refs/heads/{temp}") if repo.branch_exists(temp) else None
    if branch_tip == head:
        # The branch already moved: the fast-forward ran, the state save did not.
        verify_branch(repo, branch)
        verify_head(repo, head)
        if temp_tip not in (None, head):
            raise SyncError(f"{temp} is at {str(temp_tip)[:12]}, expected {head[:12]}")
    elif branch_tip == state["start"] and temp_tip == head:
        current = repo.current_branch()
        if current == temp:
            verify_head(repo, head)
        elif current == branch:
            verify_head(repo, state["start"])
        else:
            raise SyncError(f"HEAD is on {current}, which is neither {branch} nor {temp}; "
                            f"nothing was changed")
        if current != branch:
            repo.git("checkout", "-q", branch)  # never forced: the worktree is verified clean
        repo.git("merge", "-q", "--ff-only", head)
        verify_branch(repo, branch)
        verify_head(repo, head)
    else:
        raise SyncError(f"{branch} is at {str(branch_tip)[:12]} and {temp} at "
                        f"{str(temp_tip)[:12]}, but this sync expects {branch} at "
                        f"{state['start'][:12]} and {temp} at {head[:12]}; refusing to "
                        f"fast-forward. Run `abort` to return to {branch}")
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
    lines.append(resolutions_section(report.get("resolutions") or []))
    return "\n".join(lines)


# ---------------------------------------------------------------------------

def cmd_absorb(repo: Repo, args) -> int:
    trim = load_trim(repo)
    require_clean(repo)
    if repo.merging():
        raise SyncError("a merge is in progress; finish or abort it first")
    framework = args.framework or repo.gitlink("HEAD", FRAMEWORK_PATH)
    if not framework:
        raise SyncError(f"HEAD has no `{FRAMEWORK_PATH}` gitlink; already absorbed?")
    framework = repo.rev(framework)
    if not repo.ok("merge-base", "--is-ancestor", framework, args.framework_ref):
        raise SyncError(f"{framework} is not on {args.framework_ref}")
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
    if repo.merging():
        raise SyncError("a merge is in progress; finish or abort it first")
    branch = repo.current_branch()
    if branch == "HEAD":
        raise SyncError("HEAD is detached; check out the branch to sync (this tool moves the "
                        "branch with a fast-forward) and rerun")
    if branch.startswith("sync/"):
        raise SyncError(f"HEAD is on the temporary branch {branch}, which has no sync state; "
                        f"check out your branch and delete it, then run `sync` again")
    require_clean(repo)
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
             "previousFramework": previous, "stage": "start", "merging": None,
             "mergeOutcome": None, "upstreamMerge": None, "frameworkMerge": None,
             "head": None, "removals": [], "conflicts": [], "status": [],
             "preResolutionTree": None, "linkDrops": [],
             "report": {"discardedChanges": [], "droppedAdditions": [], "resolutions": []}}
    save_state(repo, state)  # before the temporary branch exists
    stop_for_test("start")
    return finish(repo, state)


def cmd_continue(repo: Repo, args) -> int:
    state = load_state(repo)
    if not state:
        raise SyncError("no sync in progress" + leftover_hint(repo))
    require_only_expected_changes(repo, state, "continue")
    return finish(repo, state)


def cmd_abort(repo: Repo, args) -> int:
    state = load_state(repo)
    if not state:
        raise SyncError("no sync in progress" + leftover_hint(repo))
    abort_sync(repo, state)
    print("sync aborted (discarded the in-progress merge, including any resolution work)")
    return 0


def configure_output() -> None:
    """UTF-8 with backslashreplace, whatever code page the console claims (item C)."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except ISSUE_ENCODING:
            pass


def main() -> int:
    configure_output()
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
