#!/usr/bin/env python3

'''
Check PR branch commit conventions and markdown linting.

Validates:
  - no merge commits
  - no fixup! commits
  - commit messages have a well-formed subsystem prefix before ':'
    (and not DEBUG:, WIP: etc.)
  - commit subject lines are <= 160 characters
  - no example.com author emails
  - submodule updates are isolated in their own commits
  - submodule references are reachable from master in their canonical
    repos (PRs to master only)
  - changed source files do not lose their trailing newline
  - changed markdown files pass markdownlint-cli2 and contain no
    RST-style markup

Taken from ArduPilot's Tools/scripts/check_branch_conventions.py, less its
ArduPilot-only checks (board IDs, the subsystem-to-path mapping).
'''

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

DOCS_URL = "https://ardupilot.org/dev/docs/submitting-patches-back-to-master.html"
MAX_SUBJECT_LEN = 160
BLACKLISTED_PREFIXES = {
    "DEBUG",
    "DRAFT",
    "TEMP",
    "TMP",
    "WIP",
}
# spaces and quotes allowed to support Revert commits e.g. 'Revert "mavlink.js: ...'
PREFIX_RE = re.compile(r'^[-A-Za-z0-9._/" ]+$')

# extensions checked for a lost trailing newline
SOURCE_EXTENSIONS = {
    ".c", ".cc", ".cpp", ".cxx",
    ".h", ".hh", ".hpp",
    ".py",
    ".lua",
    ".js", ".html", ".css",
}

# Enable colour when attached to a terminal or running under GitHub Actions
_colour = sys.stdout.isatty() or os.environ.get('GITHUB_ACTIONS') == 'true'
_GREEN = '\033[32m' if _colour else ''
_RED = '\033[31m' if _colour else ''
_YELLOW = '\033[33m' if _colour else ''
_RESET = '\033[0m' if _colour else ''

PASS = f"{_GREEN}✓{_RESET}"
FAIL = f"{_RED}✗{_RESET}"
SKIP = f"{_YELLOW}~{_RESET}"


class CheckBranchConventions:

    DEFAULT_UPSTREAM = "origin/master"

    def __init__(self, base_branch: str | None = None, head: str = "HEAD") -> None:
        self.base_branch = base_branch
        # the branch it names, kept when base_branch becomes a merge-base SHA
        self.base_branch_ref = base_branch or self.DEFAULT_UPSTREAM
        self.head = head

    def run_git(self, args) -> str:
        # core.quotePath=false: paths with non-ASCII characters come back
        # as they are rather than C-quoted
        return subprocess.run(["git", "-c", "core.quotePath=false"] + list(args), check=True,
                              capture_output=True, encoding="utf-8", errors="replace").stdout

    def run_git_z(self, args) -> list:
        '''run git with -z output and split it on NULs: paths come back
           exactly, whatever characters they contain'''
        args = list(args)
        # -z goes straight after the subcommand: after a "--" it would be a path
        return [f for f in self.run_git(args[:1] + ["-z"] + args[1:]).split("\0") if f]

    def get_changed_paths_for_commit(self, commit: str) -> list:
        '''return the list of paths changed in a single commit'''
        return self.run_git_z(['diff-tree', '--no-commit-id', '-r', '--name-only', commit])

    def subsystem_for_commit(self, sha: str) -> str | None:
        '''suggest a prefix in this repository's style: the file name
           if the commit changes one file, else its common directory'''
        paths = self.get_changed_paths_for_commit(sha)
        if len(paths) == 1:
            return os.path.basename(paths[0])
        dirs = {os.path.dirname(p) for p in paths}
        if len(dirs) == 1 and next(iter(dirs)):
            return next(iter(dirs))
        return None

    def check_merge_commits(self) -> bool:
        merge_commits = self.run_git(
            ["log", f"{self.base_branch}..{self.head}", "--merges", "--oneline"],
        ).strip()
        if merge_commits:
            print(f"{FAIL} Merge commits are not allowed:")
            for line in merge_commits.splitlines():
                print(f"         {line}")
            print(f"       See: {DOCS_URL}")
            return False
        print(f"{PASS} No merge commits.")
        return True

    def check_fixup_commits(self, commits: str) -> bool:
        # git's fixup! commits start with it; ArduPilot looks anywhere in
        # the line, which also catches commits merely mentioning fixup!
        bad = [line for line in commits.splitlines()
               if line.partition(" ")[2].startswith("fixup!")]
        if bad:
            print(f"{FAIL} fixup! commits are not allowed:")
            for line in bad:
                print(f"         {line}")
            print(f"       See: {DOCS_URL}")
            return False
        print(f"{PASS} No fixup! commits.")
        return True

    def check_commit_messages(self, commits: str) -> bool:
        ok = True
        for line in commits.splitlines():
            if not line.strip():
                continue
            # strip leading hash from --oneline format
            sha, separator, subject = line.partition(" ")
            if not separator:
                sha, subject = "", line
            if ":" not in subject:
                suggestion = self.subsystem_for_commit(sha) if sha else None
                print(f"{FAIL} Commit message subject is missing its subsystem prefix: {line}")
                print("       (this is about the git commit message, not the pull request title)")
                if suggestion is not None:
                    print(f"       Reword the commit message to: '{suggestion}: {subject}'")
                else:
                    print(f"       Reword the commit message to e.g. 'mavlink.js: {subject}'")
                print("       Use 'git commit --amend' for the most recent commit, "
                      "'git rebase -i' for an older one.")
                print(f"       See: {DOCS_URL}")
                ok = False
                continue
            prefix = subject.split(":")[0]
            if prefix.strip().upper() in BLACKLISTED_PREFIXES:
                print(f"{FAIL} Commit has bad subsystem prefix '{prefix}': {line}")
                print(f"       See: {DOCS_URL}")
                ok = False
            if not PREFIX_RE.match(prefix):
                print(f"{FAIL} Commit has malformed subsystem prefix '{prefix}': {line}")
                print("       Prefix must contain only letters, digits, '.', '_', '/', '-', spaces, quotes.")
                print(f"       See: {DOCS_URL}")
                ok = False
        if ok:
            print(f"{PASS} All commit messages have well-formed subsystem tags.")
        return ok

    def check_commit_lengths(self, commits: str) -> bool:
        ok = True
        for line in commits.splitlines():
            if not line.strip():
                continue
            # measure the subject, not the hash --oneline puts in front of it
            subject = line.partition(" ")[2] or line
            if len(subject) > MAX_SUBJECT_LEN:
                print(f"{FAIL} Subject too long ({len(subject)} chars, limit {MAX_SUBJECT_LEN}): {line}")
                ok = False
        if ok:
            print(f"{PASS} All commit subject lines within {MAX_SUBJECT_LEN} characters.")
        return ok

    def check_author_emails(self) -> bool:
        emails = self.run_git(["log", f"{self.base_branch}..{self.head}", "--format=%ae"]).strip()
        bad = [email for email in emails.splitlines() if "example.com" in email]
        if bad:
            print(f"{FAIL} Author email(s) with example.com are not allowed:")
            for email in bad:
                print(f"         {email}")
            return False
        print(f"{PASS} No unacceptable author emails.")
        return True

    def _gitmodules(self, key: str, rev: str | None = None) -> dict:
        '''map submodule name to its .gitmodules value for key (path, url),
           as of rev (default: the commit being checked)'''
        try:
            output = self.run_git(['config', '--blob', f'{rev or self.head}:.gitmodules',
                                   '--get-regexp', rf'\.{key}$'])
        except subprocess.CalledProcessError:
            return {}
        result = {}
        for line in output.splitlines():
            # submodule.modules/mavlink.path modules/mavlink
            parts = line.split()
            if len(parts) == 2:
                result[parts[0][len('submodule.'):-len('.' + key)]] = parts[1]
        return result

    def get_submodule_paths(self) -> set:
        return set(self._gitmodules('path').values())

    def get_submodule_urls(self, rev: str | None = None) -> dict:
        '''mapping of submodule path to canonical url, as of rev'''
        paths = self._gitmodules('path', rev)
        urls = self._gitmodules('url', rev)
        return {paths[name]: url for name, url in urls.items() if name in paths}

    def commits_on_branch(self) -> list:
        commits_raw = self.run_git(['rev-list', '--reverse', f'{self.base_branch}..{self.head}']).strip()
        return [c.strip() for c in commits_raw.splitlines() if c.strip()]

    def get_gitlink_changes_for_commit(self, commit: str) -> list:
        '''(path, old sha, new sha) of each submodule (gitlink) a commit
           adds, updates or removes, from the commit itself rather than
           .gitmodules at the head, which a later commit may have changed.
           A sha is None on the side the gitlink doesn't exist.'''
        # with -z each entry is ":oldmode newmode oldsha newsha status" then the path
        fields = self.run_git_z(['diff-tree', '--no-commit-id', '-r', '--raw', commit])
        changes = []
        for meta, path in zip(fields[0::2], fields[1::2]):
            old_mode, new_mode, old_sha, new_sha = meta.lstrip(":").split()[:4]
            if "160000" in (old_mode, new_mode):
                changes.append((path,
                                old_sha if old_mode == "160000" else None,
                                new_sha if new_mode == "160000" else None))
        return changes

    def get_submodule_changes_for_commit(self, commit: str) -> list:
        '''paths of the submodules a commit adds, updates or removes'''
        return [path for path, _, _ in self.get_gitlink_changes_for_commit(commit)]

    def check_submodule_isolation(self) -> bool:
        '''check that each submodule update is isolated in its own commit'''
        ok = True
        for commit in self.commits_on_branch():
            changed = self.get_changed_paths_for_commit(commit)
            gitlinks = self.get_gitlink_changes_for_commit(commit)
            submodule_paths = [path for path, _, _ in gitlinks]
            if not submodule_paths:
                continue
            # a moved submodule is removed at one path and added at
            # another at the same commit: count it once
            removed = {old: path for path, old, new in gitlinks if new is None}
            moves = []
            for path, old, new in gitlinks:
                if old is None and new in removed:
                    moves.append((removed.pop(new), path))
            moved = {p for move in moves for p in move}
            submodule_changes = ([f"{a} -> {b}" for a, b in moves] +
                                 [path for path in submodule_paths if path not in moved])
            # adding, moving or removing a submodule edits .gitmodules too
            other_changes = [p for p in changed
                             if p not in submodule_paths and p != ".gitmodules"]
            commit_ok = True
            if len(submodule_changes) > 1:
                print(f"{FAIL} {commit[:12]} updates multiple submodules in one commit: "
                      f"{submodule_changes}")
                commit_ok = False
            if other_changes:
                print(f"{FAIL} {commit[:12]} updates submodule(s) {submodule_changes} "
                      f"but also modifies: {other_changes}")
                commit_ok = False
            if commit_ok:
                print(f"{PASS} {commit[:12]} is a clean submodule update of {submodule_changes[0]}")
            else:
                ok = False
        if ok:
            print(f"{PASS} All submodule updates are isolated in their own commits.")
        return ok

    def _base_branch_name(self) -> str | None:
        '''extract bare branch name from the base branch; returns None for bare SHAs'''
        if re.match(r'^[0-9a-f]{40}$', self.base_branch_ref):
            return None
        # "upstream/master" -> "master"
        return self.base_branch_ref.split('/')[-1]

    def _github_repo_from_url(self, url: str) -> str | None:
        '''extract "owner/repo" from a GitHub HTTPS URL, or None if not a GitHub URL'''
        m = re.match(r'https://github\.com/([^/]+/[^/]+?)(?:\.git)?$', url.strip())
        return m.group(1) if m else None

    def _github_api_compare(self, owner_repo: str, sha: str) -> tuple:
        '''
        Call the GitHub compare API to check whether sha is an ancestor of master
        in the given canonical repo.  The REST API uses the repository's own git
        database, so a commit that exists only in a fork returns 404/422.

        Returns (ok, message):
          True,  ""      - sha is reachable from master ("behind" or "identical")
          False, reason  - sha not in master's history or not in the canonical repo
          None,  reason  - unexpected API error; caller should treat as SKIP
        '''
        api_url = f'https://api.github.com/repos/{owner_repo}/compare/master...{sha}'
        headers = {'Accept': 'application/vnd.github.v3+json'}
        token = os.environ.get('GITHUB_TOKEN', '')
        if token:
            headers['Authorization'] = f'Bearer {token}'
        req = urllib.request.Request(api_url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = json.loads(resp.read())
        except urllib.error.HTTPError as e:
            if e.code in (404, 422):
                return False, f"not found in canonical repo {owner_repo}"
            return None, f"GitHub API returned HTTP {e.code} for {owner_repo}"
        except urllib.error.URLError as e:
            return None, f"GitHub API unreachable: {e.reason}"
        status = data.get('status', '')
        if status in ('behind', 'identical'):
            return True, ""
        if status == 'ahead':
            return False, (f"exists in {owner_repo} but is not yet merged to master "
                           f"(it is ahead of master)")
        if status == 'diverged':
            return False, f"not reachable from master in {owner_repo} (diverged)"
        return None, f"unexpected GitHub compare status {status!r}"

    def check_submodule_references_exist(self) -> bool:
        '''For PRs targeting master, verify that each submodule reference
           introduced on this branch is reachable from master in the
           canonical submodule repo.  Skips for non-master PRs.'''
        if self._base_branch_name() != 'master':
            print(f"{SKIP} Submodule reference check only applies to master-targeting PRs.")
            return True

        ok = True
        any_checked = False
        for commit in self.commits_on_branch():
            # each commit's own gitlinks, and its own .gitmodules for
            # their URLs, so a submodule a later commit moves or
            # removes is still checked
            submodule_urls = self.get_submodule_urls(commit)
            for submodule_path in self.get_submodule_changes_for_commit(commit):
                # format: "160000 commit <sha> <path>"
                parts = self.run_git(['ls-tree', commit, submodule_path]).split()
                if len(parts) < 3 or parts[1] != 'commit':
                    continue
                sha = parts[2]
                url = submodule_urls.get(submodule_path)
                owner_repo = self._github_repo_from_url(url) if url else None
                label = f"{commit[:12]} references {submodule_path}@{sha[:12]}"
                if not owner_repo:
                    print(f"{SKIP} {label}: not a GitHub HTTPS URL, skipping")
                    continue
                any_checked = True
                ok_sha, msg = self._github_api_compare(owner_repo, sha)
                if ok_sha is None:
                    print(f"{SKIP} {label}: {msg}")
                elif ok_sha:
                    print(f"{PASS} {label}: reachable from master in {owner_repo}")
                else:
                    print(f"{FAIL} {label}: {msg}")
                    ok = False
        if ok:
            if any_checked:
                print(f"{PASS} All submodule references are reachable from master "
                      f"in their canonical repos.")
            else:
                print(f"{PASS} No submodule reference changes to check.")
        return ok

    def changed_markdown(self) -> list:
        return self.run_git_z(["diff", "--name-only", "--diff-filter=AMR",
                               f"{self.base_branch}...{self.head}", "--", "*.md"])

    @staticmethod
    def _markdown_lines(text: str):
        '''yield (line number, line, whether it is part of a fenced code
           block) for each line of a markdown file'''
        # CommonMark allows a fence up to three spaces in
        fence_re = re.compile(r'^ {0,3}(`{3,}|~{3,})')
        fence = None        # the fence that opened the current code block
        for lineno, line in enumerate(text.splitlines(), 1):
            m = fence_re.match(line)
            if fence is None:
                if m:
                    fence = m.group(1)
                yield lineno, line, fence is not None
            elif (m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence)
                    and line.strip() == m.group(1)):
                # only a bare fence of the same kind, at least as long, closes it
                fence = None
                yield lineno, line, True
            else:
                yield lineno, line, True

    def check_markdown_rst_hyperlinks(self) -> bool:
        '''flag RST-style markup in changed markdown files, outside
           fenced code (where a document may be showing it):
           - external hyperlinks: `text <url>`_ or `__
           - cross-references:    :ref:`...`, :doc:`...`, etc.
        '''
        files = self.changed_markdown()
        if not files:
            print(f"{PASS} No markdown files changed (RST markup check).")
            return True
        patterns = ((re.compile(r"`[^`]*<[^>]*>`__?"), "RST-style hyperlinks"),
                    (re.compile(r":[a-z]+:`"), "RST cross-references"))
        found = {what: [] for _, what in patterns}
        for path in files:
            text = self.run_git(["show", f"{self.head}:{path}"])
            for lineno, line, in_code in self._markdown_lines(text):
                if in_code:
                    continue
                for pattern, what in patterns:
                    if pattern.search(line):
                        found[what].append(f"{path}:{lineno}:{line}")
        ok = True
        for what, hits in found.items():
            if hits:
                print(f"{FAIL} {what} found in markdown file(s):")
                for hit in hits:
                    print(f"         {hit}")
                print("       Use markdown link syntax [text](url) instead.")
                ok = False
        if ok:
            print(f"{PASS} No RST-style markup in changed markdown files.")
        return ok

    def check_markdown_rst_underlines(self) -> bool:
        '''flag setext/RST underline-style headings in changed markdown files'''
        files = self.changed_markdown()
        if not files:
            print(f"{PASS} No markdown files changed (underline heading check).")
            return True
        underline_re = re.compile(r'^([=\-~^#+*])\1{2,}$')
        errors = []
        for path in files:
            text = self.run_git(["show", f"{self.head}:{path}"])
            prev_line = ""
            for lineno, line, in_code in self._markdown_lines(text):
                if (not in_code
                        and underline_re.match(line)
                        and len(line.strip()) == len(prev_line.strip())):
                    errors.append((path, lineno, line.strip()))
                prev_line = line
        if errors:
            print(f"{FAIL} Setext/RST underline-style headings found in markdown file(s):")
            for path, lineno, text in errors:
                print(f"         {path}:{lineno}: {text!r}")
            print("       Use ATX-style headings (# prefix) instead.")
            return False
        print(f"{PASS} No setext/RST underline headings in changed markdown files.")
        return True

    def _blob_ends_with_newline(self, rev: str, path: str) -> bool | None:
        '''return whether the blob rev:path ends with a newline byte;
           None if the blob is missing or empty'''
        result = subprocess.run(["git", "cat-file", "blob", f"{rev}:{path}"], capture_output=True)
        if result.returncode != 0 or not result.stdout:
            return None
        return result.stdout[-1:] in (b"\n", b"\r")

    def check_trailing_newlines(self) -> bool:
        '''check that no changed source file loses its trailing newline.
           Only files the PR adds or modifies are checked, and a modified
           file is only flagged if the base version did end with a newline,
           so pre-existing violations do not block unrelated edits.
        '''
        merge_base = self.run_git(["merge-base", self.base_branch, self.head]).strip()
        # with -z: status, then one path, or two for a rename or copy
        fields = self.run_git_z(["diff", "--name-status", "-M", f"{merge_base}..{self.head}"])
        entries = []
        i = 0
        while i < len(fields):
            status = fields[i]
            npaths = 2 if status[:1] in ("R", "C") else 1
            entries.append((status, fields[i + 1:i + 1 + npaths]))
            i += 1 + npaths
        ok = True
        for status, paths in entries:
            if status.startswith("R") and len(paths) == 2:
                old_path, new_path = paths
            elif status in ("A", "M") and len(paths) == 1:
                new_path = paths[0]
                old_path = None if status == "A" else new_path
            else:
                # deletions, mode changes etc.
                continue
            if pathlib.Path(new_path).suffix.lower() not in SOURCE_EXTENSIONS:
                continue
            new_ends = self._blob_ends_with_newline(self.head, new_path)
            if new_ends is None or new_ends:
                continue
            if old_path is not None:
                if not self._blob_ends_with_newline(merge_base, old_path):
                    # base version already lacked a trailing newline
                    continue
                print(f"{FAIL} {new_path} loses its trailing newline in this PR.")
            else:
                print(f"{FAIL} {new_path} is added without a trailing newline.")
            ok = False
        if ok:
            print(f"{PASS} No changed source files lose their trailing newline.")
        else:
            print("       Configure your editor to end files with a newline "
                  "(e.g. VSCode \"files.insertFinalNewline\": true).")
        return ok

    def check_markdown(self) -> bool:
        files = self.changed_markdown()
        if not files:
            print(f"{PASS} No markdown files changed.")
            return True
        # lint the files as they are in self.head, with its config, which
        # needn't be what is checked out
        with tempfile.TemporaryDirectory() as tmp:
            for path in files + [".markdownlint-cli2.jsonc"]:
                blob = subprocess.run(["git", "cat-file", "blob", f"{self.head}:{path}"],
                                      capture_output=True)
                if blob.returncode != 0:
                    continue
                dest = pathlib.Path(tmp, path)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(blob.stdout)
            try:
                # a leading ':' makes each a literal path, not a glob
                # (so names starting '#' or '!' are still linted)
                result = subprocess.run(["markdownlint-cli2"] + [":" + f for f in files], cwd=tmp)
            except FileNotFoundError:
                print(f"{SKIP} markdownlint-cli2 not installed.")
                return True
        if result.returncode != 0:
            print(f"{FAIL} Markdown linting errors found (see above).")
            return False
        print(f"{PASS} Markdown files pass linting.")
        return True

    def run(self) -> None:
        if self.base_branch is None:
            self.base_branch = self.run_git(["merge-base", self.head, self.DEFAULT_UPSTREAM]).strip()
            print(f"Using merge base with {self.DEFAULT_UPSTREAM}: {self.base_branch}", file=sys.stderr)

        commits = self.run_git(["log", f"{self.base_branch}..{self.head}", "--oneline"]).strip()
        n = len(commits.splitlines()) if commits else 0
        print(f"\nChecking {n} commit(s) since {self.base_branch}...\n")

        results = [
            self.check_merge_commits(),
            self.check_fixup_commits(commits),
            self.check_commit_messages(commits),
            self.check_commit_lengths(commits),
            self.check_author_emails(),
            self.check_submodule_isolation(),
            self.check_submodule_references_exist(),
            self.check_trailing_newlines(),
            self.check_markdown(),
            self.check_markdown_rst_hyperlinks(),
            self.check_markdown_rst_underlines(),
        ]

        failures = results.count(False)
        print(f"\n{'All checks passed.' if not failures else f'{failures} check(s) failed.'}")
        sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Check PR branch commit conventions and markdown linting",
    )
    parser.add_argument(
        "--base-branch",
        default=None,
        help="Upstream base branch or commit to compare against "
             "(default: merge base of the head with origin/master)",
    )
    parser.add_argument(
        "--head",
        default="HEAD",
        help="branch or commit to check (default: HEAD)",
    )
    args = parser.parse_args()
    CheckBranchConventions(args.base_branch, args.head).run()
