#!/usr/bin/env python3
"""Scores Auto Mix's levels against the human mixes with measures that can be trusted.

    python3 tools/mixlearn/score.py <bench/results.csv> <fits.json> [--before old/results.csv] [--out score.md]

Ours: the bench's exact numbers (each track as it plays in our mix: vob_ours_exact and
lu_<role>_ours_exact). Human: learn.py's fit of each human mix (`learn.py --fits fits.json`),
which lines the mix up with the tracks hop by hop and explains it to about 2-3 dB, where the
bench's own fit (its vob_human and lu_<role>_human) can be off by many dB on big or edited
multitracks. Both measure the same way: K-weighted power while the role plays.
"""
import argparse
import csv
import json
import statistics

ROLES = ["voice", "background", "drums", "bass", "guitar", "piano", "pad", "synth", "instrument", "instrumental"]
ROLE_NAMES = {"voice": "Voice", "background": "Background vocals", "drums": "Drums", "bass": "Bass", "guitar": "Guitar",
              "piano": "Piano / keys", "pad": "Pad / strings", "synth": "Synth", "instrument": "Other instrument",
              "instrumental": "Backing track"}


def number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def load_bench(path):
    return {row["song"]: row for row in csv.DictReader(open(path))}


def song_scores(row, fit):
    """(voice-over-backing ours, human), and {role: (ours, human)} for the roles in both."""
    if row is None or fit is None:
        return None, {}
    human = fit["mix"]
    vob = (number(row.get("vob_ours_exact")), human.get("voice_over_backing_lu"))
    roles = {}
    for role in ROLES:
        ours = number(row.get(f"lu_{role}_ours_exact"))
        theirs = human.get("role_lu", {}).get(role)
        if ours is not None and theirs is not None:
            roles[role] = (ours, theirs)
    return (vob if None not in vob else None), roles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("fits")
    ap.add_argument("--before", help="an earlier bench/results.csv to compare with")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    fits = {f["song"]: f for f in json.load(open(args.fits))}
    after = load_bench(args.results)
    before = load_bench(args.before) if args.before else None

    md = ["Auto Mix against the human mixes. Ours measured exactly, the human mixes by the learning fit.", ""]
    head = "| Song | Voice over backing, human (LU) | Ours (LU) | Off by (LU) | Role levels off by, average (LU) |"
    rule = "|---|---|---|---|---|"
    if before:
        head = ("| Song | Voice over backing, human (LU) | Ours before / after (LU) | Off by before / after (LU) "
                "| Role levels off by, before / after (LU) |")
    md += [head, rule]

    vob_err = {"before": [], "after": []}
    role_err = {"before": [], "after": []}
    bias = {}
    closer = 0
    for song, row in after.items():
        fit = fits.get(song)
        if fit is None:
            continue
        runs = {"after": song_scores(row, fit)}
        if before:
            runs["before"] = song_scores(before.get(song), fit)
        cells = {}
        for key, (vob, roles) in runs.items():
            err = abs(vob[0] - vob[1]) if vob else None
            rerr = statistics.mean(abs(o - h) for o, h in roles.values()) if roles else None
            if err is not None:
                vob_err[key].append(err)
            if rerr is not None:
                role_err[key].append(rerr)
            cells[key] = (vob, err, rerr)
            if key == "after":
                for role, (o, h) in roles.items():
                    bias.setdefault(role, []).append(o - h)
        vob, err, rerr = cells["after"]
        f = lambda x: "-" if x is None else f"{x:+.1f}"  # noqa: E731
        g = lambda x: "-" if x is None else f"{x:.1f}"  # noqa: E731
        human = f(vob[1]) if vob else "no voice"
        if before:
            bv, be, br = cells["before"]
            if be is not None and err is not None and err < be:
                closer += 1
            md.append(f"| {song} | {human} | {f(bv[0]) if bv else '-'} / {f(vob[0]) if vob else '-'} | {g(be)} / {g(err)} | {g(br)} / {g(rerr)} |")
        else:
            md.append(f"| {song} | {human} | {f(vob[0]) if vob else '-'} | {g(err)} | {g(rerr)} |")

    md.append("")
    med = lambda v: "-" if not v else f"{statistics.median(v):.1f}"  # noqa: E731
    if before:
        md.append(f"Voice over backing, median off by: {med(vob_err['before'])} LU before, {med(vob_err['after'])} LU after "
                  f"(closer on {closer} of {len(vob_err['after'])} songs).")
        md.append(f"Role levels, median off by: {med(role_err['before'])} LU before, {med(role_err['after'])} LU after.")
    else:
        md.append(f"Voice over backing, median off by {med(vob_err['after'])} LU; role levels {med(role_err['after'])} LU.")
    md += ["", "| Role | Songs | Ours minus human, median (LU) |", "|---|---|---|"]
    for role in ROLES:
        if role in bias:
            md.append(f"| {ROLE_NAMES[role]} | {len(bias[role])} | {statistics.median(bias[role]):+.1f} |")

    text = "\n".join(md) + "\n"
    print(text)
    if args.out:
        with open(args.out, "w") as out:
            out.write(text)


if __name__ == "__main__":
    main()
