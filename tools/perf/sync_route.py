# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes a recorded route (record_to_route.py) start its gameplay when the save has finished loading, not at a fixed time from launch.

    python -I tools/perf/sync_route.py tools/perf/routes/my_route.tt -o tools/perf/routes/my_route_sync.tt --from 34.3 [--menu menu.tt]
        [--quiet 2000] [--scene map_move]

A recorded route plays every press at the same time from the launch (`@at`). On another PC, or after a change that makes boot or the save
load slower or faster, the first steps of the walk then happen while the loading screen is still up and are lost: the character starts
somewhere in the middle of the route. This keeps the menu part, waits for the load (`@loaded`: a load began, ended, and nothing loaded for
--quiet ms), starts the clock again (`@origin`) and plays the rest at the recorded times counted from --from (seconds of the recording).

--from: the recording's time just before the first gameplay step (default: 0.5 s before the first stick step after the last menu press).
--menu: a file of steps to use instead of the recording's menu part (for a PC whose title screen needs other presses, e.g. with
        [fefw] title_continue on the cursor already sits on Continue). Without it the recording's steps before --from are kept as they are.
--scene: the SCENE that starts at --from (default: any `@log SCENE start` the recording has before --from moves there).
--shots: a screenshot every N seconds of the gameplay part (walk_<seconds>.png), to check that the character went where the recording did.
"""
import argparse
import re
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("route")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--from", dest="start", type=float, default=None, help="seconds of the recording where gameplay begins")
    ap.add_argument("--menu", default=None)
    ap.add_argument("--quiet", type=int, default=2000, help="ms with no load before the gameplay starts")
    ap.add_argument("--scene", default=None)
    ap.add_argument("--shots", type=float, default=0, help="a screenshot every this many seconds of gameplay (walk_000 ...), to see where the route went")
    a = ap.parse_args()

    lines = open(a.route, encoding="utf-8").read().splitlines()
    at = 0.0
    steps = []                         # (time ms, line)
    for ln in lines:
        m = re.match(r"\s*@at\s+(\d+(?:\.\d+)?)", ln)
        if m:
            at = float(m.group(1))
            continue
        steps.append((at, ln))

    if a.start is None:
        sticks = [t for t, ln in steps if ln.strip().startswith("stick") and t > 0.0]
        if not sticks:
            sys.exit("no stick step in the route: give --from")
        start_ms = max(0.0, sticks[0] - 500.0)
    else:
        start_ms = a.start * 1000.0

    head = [ln for t, ln in steps if t == 0.0 and (ln.strip().startswith("#") or not ln.strip())]
    early = [ln for t, ln in steps if t < start_ms and not (t == 0.0 and (ln.strip().startswith("#") or not ln.strip()))]
    scene = a.scene
    if scene is None:
        for ln in early:
            m = re.match(r"\s*@log\s+SCENE\s+start\s+(\S+)", ln)
            if m:
                scene = m.group(1)

    out = []
    out += [f"# Synced from {a.route} by tools/perf/sync_route.py: gameplay starts {a.quiet} ms after the save's load, not at a fixed time."]
    out += head
    out += ["@timeout 0", ""]
    if a.menu:
        menu = [ln for ln in open(a.menu, encoding="utf-8").read().splitlines() if not re.match(r"\s*@(pass|end|timeout)\b", ln)]
        out += menu
    else:
        t_prev = None
        for t, ln in steps:
            if t >= start_ms or (t == 0.0 and (ln.strip().startswith("#") or not ln.strip())):
                continue
            if re.match(r"\s*@log\s+SCENE\s+start\b", ln) or re.match(r"\s*@timeout\b", ln):
                continue
            if t != t_prev:
                out.append(f"@at {int(t)}")
                t_prev = t
            out.append(ln)
    out += ["", f"@loaded {a.quiet}", "@origin"]
    if scene:
        out.append(f"@log SCENE start {scene}")
    play = [(t - start_ms, ln) for t, ln in steps if t >= start_ms
            and not (scene and re.match(rf"\s*@log\s+SCENE\s+start\s+{re.escape(scene)}\b", ln))]
    if a.shots > 0 and play:
        end = max(t for t, _ in play)
        k = 0.0
        while k <= end:
            play.append((k, f"@shot walk_{int(k / 1000):03d}"))
            k += a.shots * 1000.0
        play.sort(key=lambda e: e[0])          # stable: a shot at the same time as a step comes after it
    t_prev = None
    for t, ln in play:
        if t != t_prev:
            out.append(f"@at {int(round(t))}")
            t_prev = t
        out.append(ln)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"{a.out}: gameplay from {start_ms / 1000:.1f} s of the recording, after @loaded {a.quiet}")


if __name__ == "__main__":
    main()
