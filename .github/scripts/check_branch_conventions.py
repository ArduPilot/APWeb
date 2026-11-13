#!/usr/bin/env python3

'''
Check PR branch commit conventions.

Validates:
  - no fixup! commits

Based on ArduPilot's Tools/scripts/check_branch_conventions.py.
'''

import argparse
import os
import subprocess
import sys

DOCS_URL = "https://ardupilot.org/dev/docs/submitting-patches-back-to-master.html"

# Enable colour when attached to a terminal or running under GitHub Actions
_colour = sys.stdout.isatty() or os.environ.get('GITHUB_ACTIONS') == 'true'
_GREEN = '\033[32m' if _colour else ''
_RED = '\033[31m' if _colour else ''
_RESET = '\033[0m' if _colour else ''

PASS = f"{_GREEN}✓{_RESET}"
FAIL = f"{_RED}✗{_RESET}"


def run_git(args):
    return subprocess.run(["git"] + args, check=True, capture_output=True, text=True).stdout


def check_fixup_commits(commits: str) -> bool:
    bad = [line for line in commits.splitlines() if "fixup!" in line]
    if bad:
        print(f"{FAIL} fixup! commits are not allowed:")
        for line in bad:
            print(f"         {line}")
        print(f"       See: {DOCS_URL}")
        return False
    print(f"{PASS} No fixup! commits.")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Check PR branch commit conventions")
    parser.add_argument("--base-branch", default="origin/master",
                        help="upstream base branch or commit to compare against (default: origin/master)")
    parser.add_argument("--head", default="HEAD",
                        help="branch or commit to check (default: HEAD)")
    args = parser.parse_args()

    commits = run_git(["log", f"{args.base_branch}..{args.head}", "--oneline"]).strip()
    n = len(commits.splitlines()) if commits else 0
    print(f"\nChecking {n} commit(s) since {args.base_branch}...\n")

    results = [
        check_fixup_commits(commits),
    ]

    failures = results.count(False)
    print(f"\n{'All checks passed.' if not failures else f'{failures} check(s) failed.'}")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
