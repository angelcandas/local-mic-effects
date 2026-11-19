#!/usr/bin/env python3
"""Turns learn.py's song fits into the numbers Auto Mix ships with (src/analysis/LearnedTargets.h).

    python3 tools/mixlearn/export.py fits.json [--header src/analysis/LearnedTargets.h]

Two things are learned, each checked on songs left out of the learning first:
  * how far the lead voice sits over the band while it sings, as one shift on every music
    style's own target (the styles keep their differences; speech and "voice up front" stay);
  * a gentle EQ per instrument role, the median move the engineers made on that role's
    tracks (mix against raw), halved and capped, as a low shelf, one bell and a high shelf.
Prints the held-out check, then writes the header.
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import learn  # noqa: E402

# Today's voice-over-backing target per style (MixAssistant.cpp getTargets), before learning.
STYLE_VOB = {"pop": 1.5, "ballad": 2.5, "rock": 0.0, "hiphop": 2.5, "rnb": 2.0, "acoustic": 2.0,
             "electronic": 1.0, "jazz": 1.0, "lofi": 0.5}

# Instrument roles that get a learned EQ, with the C++ Role each is.
EQ_ROLES = {"drums": "drums", "bass": "bass", "guitar": "guitar", "piano": "piano", "synth": "synth"}
EQ_SHRINK = 0.5    # half of the median move: a nudge toward finished mixes, not a copy
EQ_CAP_DB = 3.0
EQ_MIN_SONGS = 3
QUIET_GROUP_DB = 24.0  # a band this far under the track's loudest isn't judged (its "move" is mostly bleed)


def usable_eq(stem):
    tone = np.array(stem["raw_tone"])
    energy = np.array([10 * np.log10(np.mean(10 ** (tone[b] / 10)) + 1e-30) for b in learn.GROUPS])
    return [v if (v is not None and energy[gi] > energy.max() - QUIET_GROUP_DB) else None
            for gi, v in enumerate(stem["eq_db"])]


def eq_table(fits):
    table = {}
    for role in EQ_ROLES:
        stems = [usable_eq(s) for f in fits for s in f["stems"] if s["role"] == role and s["active_s"] > 5]
        if len(stems) < EQ_MIN_SONGS:
            continue
        row = []
        for gi in range(len(learn.GROUPS)):
            vals = [x[gi] for x in stems if x[gi] is not None]
            row.append(0.0 if len(vals) < EQ_MIN_SONGS else float(np.clip(np.median(vals) * EQ_SHRINK, -EQ_CAP_DB, EQ_CAP_DB)))
        table[role] = (row, len(stems))
    return table


def moves_to_groups(m):
    """What the shipped shelves and bell do to each group, roughly (the bell spreads half an octave each side)."""
    centres = [math.sqrt(learn.GROUP_EDGES[i] * learn.GROUP_EDGES[i + 1]) for i in range(len(learn.GROUPS))]
    out = []
    for c in centres:
        v = m["low"] / (1 + (c / 120.0) ** 2) + m["high"] / (1 + (6000.0 / c) ** 2)
        v += m["bell_db"] * math.exp(-0.5 * (math.log2(c / m["bell_hz"]) / 0.6) ** 2)
        out.append(v)
    return out


def eq_error(fit, table, shipped=False):
    """Mean |engineer's move - ours| over the song's judged bands, and the same for no EQ."""
    ours, flat = [], []
    for s in fit["stems"]:
        if s["role"] not in table or s["active_s"] <= 5:
            continue
        row = moves_to_groups(to_moves(table[s["role"]][0])) if shipped else table[s["role"]][0]
        for gi, v in enumerate(usable_eq(s)):
            if v is not None:
                ours.append(abs(v - row[gi]))
                flat.append(abs(v))
    return (np.mean(ours), np.mean(flat)) if ours else None


def vob_shift(fits):
    d = [f["mix"]["voice_over_band_lu"] - STYLE_VOB[f["style"]] for f in fits
         if f["mix"].get("voice_over_band_lu") is not None and f["style"] in STYLE_VOB]
    return float(np.median(d)) if d else 0.0


BALANCE_ROLES = ["drums", "bass", "guitar", "piano", "synth"]
BALANCE_MIN_SONGS = 5


def balance_table(fits):
    """Each instrument role's level against all the instruments while it plays, median over the songs."""
    out = {}
    for role in BALANCE_ROLES:
        vals = [f["mix"]["band_lu"][role] for f in fits if (f["mix"].get("band_lu") or {}).get(role) is not None]
        if len(vals) >= BALANCE_MIN_SONGS:
            out[role] = (round(float(np.median(vals)) * 2) / 2, len(vals))
    return out


def balance_error(fit, table):
    """Mean |balance off| over the song's roles, as shapes (each minus its mean): the learned
    balance, and the tracks at their recorded levels (what Auto Mix kept before)."""
    human, raw = fit["mix"].get("band_lu") or {}, fit["mix"].get("raw_band_lu") or {}
    ks = [k for k in table if human.get(k) is not None and raw.get(k) is not None]
    if len(ks) < 2:
        return None
    h = np.array([human[k] for k in ks])
    r = np.array([raw[k] for k in ks])
    p = np.array([table[k][0] for k in ks])
    c = lambda x: x - x.mean()  # noqa: E731
    return float(np.mean(np.abs(c(p) - c(h)))), float(np.mean(np.abs(c(r) - c(h))))


def to_moves(row):
    """Low shelf (50-200 Hz), one bell (200 Hz-5 kHz) and high shelf (5-12.6 kHz) from the group moves."""
    centres = [math.sqrt(learn.GROUP_EDGES[i] * learn.GROUP_EDGES[i + 1]) for i in range(len(learn.GROUPS))]
    low = float(np.mean(row[0:2]))
    high = float(np.mean(row[7:9]))
    mids = row[2:7]
    k = int(np.argmax(np.abs(mids)))
    bell_db = float(mids[k])
    bell_hz = centres[2 + k]
    r = lambda x: round(x * 2) / 2  # noqa: E731  (0.5 dB steps, as the plan shows them)
    return {"low": r(low), "bell_hz": round(bell_hz), "bell_db": r(bell_db) if abs(bell_db) >= 0.5 else 0.0, "high": r(high)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fits")
    ap.add_argument("--header", default=None)
    args = ap.parse_args()
    fits = json.load(open(args.fits))

    print("Held out (each song against what the other songs teach):")
    vob_today, vob_learned, eq_rows, balance_rows = [], [], [], []
    for i, f in enumerate(fits):
        rest = [g for j, g in enumerate(fits) if j != i]
        actual = f["mix"].get("voice_over_band_lu")
        if actual is not None and f["style"] in STYLE_VOB:
            today = STYLE_VOB[f["style"]]
            learned = today + vob_shift(rest)
            vob_today.append(abs(actual - today))
            vob_learned.append(abs(actual - learned))
        e = eq_error(f, eq_table(rest), shipped=True)
        if e:
            eq_rows.append(e)
        b = balance_error(f, balance_table(rest))
        if b:
            balance_rows.append(b)
    print(f"  voice over the band: off by {np.median(vob_today):.1f} LU today, {np.median(vob_learned):.1f} LU learned "
          f"(median of {len(vob_today)} songs; learned closer on {sum(b < a for a, b in zip(vob_today, vob_learned))})")
    print(f"  instrument EQ, as shipped (shelves + one bell): {np.median([b for _, b in eq_rows]):.2f} dB from the engineers' moves with none, "
          f"{np.median([a for a, _ in eq_rows]):.2f} dB learned (median of {len(eq_rows)} songs; "
          f"learned closer on {sum(a < b for a, b in eq_rows)})")

    if balance_rows:
        print(f"  band balance: {np.median([b for _, b in balance_rows]):.2f} dB off with the tracks as recorded, "
              f"{np.median([a for a, _ in balance_rows]):.2f} dB learned (median of {len(balance_rows)} songs; "
              f"learned closer on {sum(a < b for a, b in balance_rows)})")
    balance = balance_table(fits)
    print("Band balance (each role against all the instruments while it plays): "
          + ", ".join(f"{k} {v[0]:+.1f} LU ({v[1]} songs)" for k, v in balance.items()))

    shift = round(vob_shift(fits) * 2) / 2
    table = eq_table(fits)
    print(f"Voice over the band: {shift:+.1f} LU on every music style")
    moves = {}
    for role, (row, n) in table.items():
        moves[role] = to_moves(row)
        print(f"  {role} ({n} songs): {moves[role]}   groups {[round(x, 1) for x in row]}")

    if args.header:
        songs = len(fits)
        lines = [
            "#pragma once",
            "",
            "// Generated by tools/mixlearn/export.py from the learning songs (tools/README-mixbench.md,",
            f"// \"Learn\"): {songs} multitracks with their engineers' mixes. Don't edit by hand; run it again.",
            "namespace analysis::learned",
            "{",
            "// The lead voice over the band while it sings, in the engineers' mixes, against Auto Mix's",
            "// own style targets: added to every music style's target (not speech or voice up front).",
            f"inline constexpr float voiceOverBandShiftLu = {shift:.1f}f;",
            "",
            "// A gentle EQ per instrument role toward finished mixes: half of the engineers' median move",
            "// on that role (mix against raw track), capped at 3 dB. Low shelf at 120 Hz, a bell, a high",
            "// shelf at 6 kHz.",
            "struct RoleEq",
            "{",
            "    float lowShelfDb, bellHz, bellDb, highShelfDb;",
            "    int songs;",
            "};",
            "",
        ]
        for role, cpp in EQ_ROLES.items():
            if role in moves:
                m = moves[role]
                lines.append(f"inline constexpr RoleEq {cpp}Eq {{ {m['low']:.1f}f, {m['bell_hz']:.0f}.0f, {m['bell_db']:.1f}f, {m['high']:.1f}f, {table[role][1]} }};")
            else:
                lines.append(f"inline constexpr RoleEq {cpp}Eq {{ 0.0f, 1000.0f, 0.0f, 0.0f, 0 }};")
        lines += [
            "",
            "// The band's balance: each instrument role against all the instruments together while",
            "// it plays, in LU (the median over the songs; 0 songs: not learned). Auto Mix brings",
            "// instrument tracks whose faders are untouched toward it.",
            "struct RoleBalance",
            "{",
            "    float lu;",
            "    int songs;",
            "};",
            "",
        ]
        for role in BALANCE_ROLES:
            lu, n = balance.get(role, (0.0, 0))
            lines.append(f"inline constexpr RoleBalance {role}Balance {{ {lu:.1f}f, {n} }};")
        lines += ["}", ""]
        with open(args.header, "w") as f:
            f.write("\n".join(lines))
        print("Wrote " + args.header)


if __name__ == "__main__":
    main()
