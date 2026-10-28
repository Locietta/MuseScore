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
is pushed. State lives in .git/sync-upstream.json; a rerun of `sync` after an
interruption aborts the leftover state first.

TRIM: one path per line, `#` comments; a trailing `/` marks a directory prefix,
otherwise the entry is an exact file path. No globs.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

UPSTREAM_REF = "origin/upstream/musescore"
FRAMEWORK_REF = "origin/upstream/muse-framework"
FRAMEWORK_PATH = "muse"
TRIM_FILE = "TRIM"
GITLINK = "160000"


class SyncError(Exception):
    pass


class Repo:
    def __init__(self, root: Path):
        self.root = root

    def git(self, *args: str, check: bool = True, quiet: bool = False) -> str:
        result = subprocess.run(["git", *args], cwd=self.root, capture_output=True, text=True,
                                encoding="utf-8")
        if check and result.returncode != 0:
            raise SyncError(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
        if not quiet and result.stderr.strip() and os.environ.get("SYNC_UPSTREAM_VERBOSE"):
            print(result.stderr.strip(), file=sys.stderr)
        return result.stdout

    def ok(self, *args: str) -> bool:
        return subprocess.run(["git", *args], cwd=self.root, capture_output=True).returncode == 0

    def rev(self, ref: str) -> str:
        return self.git("rev-parse", "--verify", f"{ref}^{{commit}}").strip()

    def gitlink(self, commit: str, path: str) -> str | None:
        line = self.git("ls-tree", commit, "--", path).strip()
        if not line:
            return None
        mode, _type, sha = line.split("\t")[0].split()
        return sha if mode == GITLINK else None

    @property
    def git_dir(self) -> Path:
        return Path(self.git("rev-parse", "--absolute-git-dir").strip())

    def merging(self) -> bool:
        return (self.git_dir / "MERGE_HEAD").exists()

    def current_branch(self) -> str:
        return self.git("rev-parse", "--abbrev-ref", "HEAD").strip()

    def clean(self) -> bool:
        return not self.git("status", "--porcelain", "--untracked-files=no").strip()


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


def remove_worktree_path(repo: Repo, path: str) -> None:
    target = repo.root / path
    if target.is_dir() and not target.is_symlink():
        shutil.rmtree(target, ignore_errors=True)
    elif target.exists() or target.is_symlink():
        target.unlink()


def unmerged_paths(repo: Repo) -> dict[str, set[int]]:
    paths: dict[str, set[int]] = {}
    for line in repo.git("ls-files", "-u", "-z").split("\0"):
        if line:
            meta, path = line.split("\t", 1)
            paths.setdefault(path, set()).add(int(meta.split()[2]))
    return paths


def state_path(repo: Repo) -> Path:
    return repo.git_dir / "sync-upstream.json"


def save_state(repo: Repo, state: dict) -> None:
    state_path(repo).write_text(json.dumps(state, indent=1), encoding="utf-8")


def load_state(repo: Repo) -> dict | None:
    path = state_path(repo)
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def drop_state(repo: Repo) -> None:
    state = load_state(repo)
    if repo.merging():
        repo.git("merge", "--abort")
    if state:
        if repo.current_branch() == state["temp"]:
            repo.git("checkout", "-q", "-f", state["branch"])
        if repo.ok("rev-parse", "--verify", f"refs/heads/{state['temp']}"):
            repo.git("branch", "-q", "-D", state["temp"])
        state_path(repo).unlink()


def stop_for_test(stage: str) -> None:
    # Test hook: simulate an interruption right after a stage.
    if os.environ.get("SYNC_UPSTREAM_TEST_STOP_AFTER") == stage:
        raise SystemExit(f"[test] stopped after {stage}")


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
    drop = [FRAMEWORK_PATH] + [p for p in repo.git("ls-files", "-s").splitlines()
                               if p.startswith(GITLINK) and trimmed(p.split("\t", 1)[1], trim)]
    for entry in drop:
        path = entry.split("\t", 1)[1] if "\t" in entry else entry
        repo.git("rm", "-q", "--cached", "--", path)
        remove_worktree_path(repo, path)
    for path in repo.git("ls-files").splitlines():
        if trimmed(path, trim):
            repo.git("rm", "-q", "--cached", "--", path)
            remove_worktree_path(repo, path)
    repo.git("read-tree", f"--prefix={FRAMEWORK_PATH}/", "-u", framework)
    repo.git("commit", "-q", "-m", f"Absorb muse_framework {framework} at {FRAMEWORK_PATH}/ (subtree)")
    print(f"absorbed {framework} at {FRAMEWORK_PATH}/ -> {repo.rev('HEAD')}")
    return 0


def resolve_upstream_merge(repo: Repo, trim: list[str], report: dict) -> list[str]:
    """Resolve the conflicts a sync expects; return the paths that need a human."""
    unresolved = []
    for path, stages in sorted(unmerged_paths(repo).items()):
        if path == FRAMEWORK_PATH or path.startswith(FRAMEWORK_PATH + "~"):
            # The upstream gitlink bump; the framework is merged into muse/ separately.
            repo.git("rm", "-q", "--cached", "--", path)
            remove_worktree_path(repo, path)
        elif trimmed(path, trim) and 2 not in stages:
            # Deleted by us (trimmed), changed upstream: keep it deleted, but report it.
            repo.git("rm", "-q", "--cached", "--", path)
            remove_worktree_path(repo, path)
            report["discardedChanges"].append(path)
        else:
            unresolved.append(path)
    # New upstream files under trimmed prefixes merge cleanly; delete and report them.
    for path in repo.git("diff", "--cached", "--no-renames", "--name-only", "--diff-filter=A", "HEAD").splitlines():
        if trimmed(path, trim):
            repo.git("rm", "-q", "--cached", "--", path)
            remove_worktree_path(repo, path)
            report["droppedAdditions"].append(path)
    return unresolved


def check_tree(repo: Repo, trim: list[str]) -> list[str]:
    problems = []
    for line in repo.git("ls-files", "-s").splitlines():
        meta, path = line.split("\t", 1)
        if meta.startswith(GITLINK):
            problems.append(f"gitlink left in the index: {path}")
        if trimmed(path, trim):
            problems.append(f"trimmed path present: {path}")
    if not repo.git("ls-files", "--", f"{FRAMEWORK_PATH}/").strip():
        problems.append(f"{FRAMEWORK_PATH}/ is empty")
    return problems


def cmd_sync(repo: Repo, args) -> int:
    if load_state(repo) or repo.current_branch().startswith("sync/"):
        print("found an unfinished sync; aborting it first")
        drop_state(repo)
    if not repo.clean() or repo.merging():
        raise SyncError("working tree is not clean")
    branch = repo.current_branch()
    target = repo.rev(args.target or args.upstream_ref)
    if not repo.ok("merge-base", "--is-ancestor", target, args.upstream_ref):
        raise SyncError(f"target {target} is not on {args.upstream_ref}")
    if repo.ok("merge-base", "--is-ancestor", target, "HEAD"):
        print(f"{branch} already contains {target}")
        return 0
    base = repo.git("merge-base", "HEAD", target).strip()
    framework = repo.gitlink(target, FRAMEWORK_PATH)
    if not framework:
        raise SyncError(f"target {target} has no `{FRAMEWORK_PATH}` gitlink")
    if not repo.ok("cat-file", "-e", f"{framework}^{{commit}}") or \
            not repo.ok("merge-base", "--is-ancestor", framework, args.framework_ref):
        raise SyncError(f"target {target} pins framework {framework}, which is not on "
                        f"{args.framework_ref}; pick another upstream commit")
    previous = repo.git("merge-base", "HEAD", args.framework_ref).strip()
    if not repo.ok("merge-base", "--is-ancestor", previous, framework):
        raise SyncError(f"framework would move backwards: {previous} -> {framework}")
    state = {"branch": branch, "temp": f"sync/{target[:12]}", "start": repo.rev("HEAD"),
             "target": target, "base": base, "framework": framework, "previousFramework": previous,
             "stage": "upstream", "report": {"discardedChanges": [], "droppedAdditions": []}}
    repo.git("checkout", "-q", "-b", state["temp"])
    save_state(repo, state)
    # No rename detection: relative to the upstream merge base our side "deletes" every
    # trimmed path and "adds" all of muse/, and ort would pair them as bogus renames
    # (observed: a trimmed appshell QML file matched to a framework header). Upstream
    # renames of files we changed then surface as modify/delete conflicts instead.
    repo.git("merge", "-q", "--no-commit", "--no-ff", "-s", "ort", "-Xno-renames", target, check=False)
    stop_for_test("upstream-merge")
    return finish(repo, state)


def finish(repo: Repo, state: dict) -> int:
    trim = load_trim(repo)
    report = state["report"]
    if state["stage"] == "upstream":
        if repo.merging():
            unresolved = resolve_upstream_merge(repo, trim, report)
            save_state(repo, state)
            if unresolved:
                print("conflicts that need a human (resolve, `git add`, then `continue`):")
                for path in unresolved:
                    print(f"  {path}")
                return 2
            problems = check_tree(repo, trim)
            if problems:
                raise SyncError("tree check failed:\n  " + "\n  ".join(problems))
            repo.git("commit", "-q", "-m", f"Merge upstream/musescore {state['target']}\n\n" + summary(state))
        state["stage"] = "framework"
        save_state(repo, state)
        stop_for_test("upstream-commit")
    if state["stage"] == "framework":
        if not repo.merging() and state["framework"] != state["previousFramework"] and \
                not repo.ok("merge-base", "--is-ancestor", state["framework"], "HEAD"):
            repo.git("merge", "-q", "--no-ff", "-s", "ort", f"-Xsubtree={FRAMEWORK_PATH}", "-m",
                     f"Merge upstream/muse-framework {state['framework']} into {FRAMEWORK_PATH}/",
                     state["framework"], check=False)
        if repo.merging():
            print("framework conflicts in muse/ (resolve, `git add`, `git commit`, then `continue`):")
            for path in sorted(unmerged_paths(repo)):
                print(f"  {path}")
            return 2
        state["stage"] = "check"
        save_state(repo, state)
    problems = check_tree(repo, trim)
    # Without a local framework delta, muse/ must equal the framework tree exactly.
    had_delta = repo.git("rev-parse", f"{state['start']}:{FRAMEWORK_PATH}").strip() != \
        repo.git("rev-parse", f"{state['previousFramework']}^{{tree}}").strip()
    if not had_delta and repo.git("rev-parse", f"HEAD:{FRAMEWORK_PATH}").strip() != \
            repo.git("rev-parse", f"{state['framework']}^{{tree}}").strip():
        problems.append(f"{FRAMEWORK_PATH}/ differs from framework {state['framework']}")
    if problems:
        raise SyncError("tree check failed:\n  " + "\n  ".join(problems))
    head = repo.rev("HEAD")
    repo.git("checkout", "-q", state["branch"])
    repo.git("merge", "-q", "--ff-only", head)
    repo.git("branch", "-q", "-D", state["temp"])
    state_path(repo).unlink()
    print(f"{state['branch']} -> {head}")
    print(summary(state))
    return 0


def summary(state: dict) -> str:
    report = state["report"]
    lines = [f"upstream {state['base'][:12]}..{state['target'][:12]}",
             f"framework {state['previousFramework'][:12]}..{state['framework'][:12]}",
             f"upstream changes discarded on trimmed paths: {len(report['discardedChanges'])}"]
    lines += [f"  {p}" for p in report["discardedChanges"]]
    lines.append(f"new upstream files dropped under trimmed prefixes: {len(report['droppedAdditions'])}")
    lines += [f"  {p}" for p in report["droppedAdditions"]]
    return "\n".join(lines)


def cmd_continue(repo: Repo, args) -> int:
    state = load_state(repo)
    if not state:
        raise SyncError("no sync in progress")
    if state["stage"] == "framework" and repo.merging() and not unmerged_paths(repo):
        repo.git("commit", "-q", "--no-edit")
    return finish(repo, state)


def cmd_abort(repo: Repo, args) -> int:
    drop_state(repo)
    print("sync aborted")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
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
