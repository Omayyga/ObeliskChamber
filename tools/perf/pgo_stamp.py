# SPDX-License-Identifier: GPL-3.0-or-later
"""Records which runtime source a runtime profile (rt.profdata) was trained on, next to it, so make-release.ps1 can warn when the runtime
has changed since (docs/PGO.md, "A release runtime"). The idea is TotK Recomp's profile record (pgo-<ver>.json).

    python tools\\perf\\pgo_stamp.py C:\\pgo\\rt.profdata

Run it right after merging the profile, from the checkout the instrumented build was made from. It writes <profile>.stamp.txt:

    commit=<HEAD commit>
    runtime_tree=<git tree of runtime/ at HEAD>
    dirty=<0 or 1: runtime/ had uncommitted changes>
    written=<UTC time>

A changed function only loses its own counts (clang matches each function by name and control-flow hash), so a stale profile still
helps; the warning says how much of runtime/ changed so the choice to record again is an informed one.
"""
import datetime
import os
import subprocess
import sys


def git(root, *args):
    return subprocess.run(['git', '-C', root, *args], check=True, capture_output=True, text=True).stdout.strip()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    prof = os.path.abspath(sys.argv[1])
    if not os.path.isfile(prof):
        print(f'no such profile: {prof}')
        return 1
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    commit = git(root, 'rev-parse', 'HEAD')
    tree = git(root, 'rev-parse', 'HEAD:runtime')
    dirty = '1' if git(root, 'status', '--porcelain', '--', 'runtime') else '0'
    now = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    stamp = prof + '.stamp.txt'
    with open(stamp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'commit={commit}\nruntime_tree={tree}\ndirty={dirty}\nwritten={now}\n')
    print(f'wrote {stamp}: runtime/ at {tree[:12]} (commit {commit[:12]}){" with uncommitted changes" if dirty == "1" else ""}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
