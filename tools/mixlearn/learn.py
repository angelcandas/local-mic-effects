#!/usr/bin/env python3
"""Learns Auto Mix targets from human mixes of real multitracks.

    python3 tools/mixlearn/learn.py <folder of .mlf files> [--out learned.json] [--report report.md]

Each .mlf (written by MicFXMixBench --learn, see src/analysis/MixLearn.h) holds every raw
track of a song and the human mix as band power per 100 ms. For each song the human mix
is explained as a sum of the raw tracks, each with:

  level        one gain per track (the fader)
  eq           a gain per band group (10 groups, roughly an octave each) over its level
  compression  a gain that falls by `slope` dB per dB the track's loudness goes over its
               median (ratio = 1 / (1 - slope)); the best of a few smoothing times says
               how fast it acts (rides and compression both show up here)
  reverb       a decaying copy of the track (the tail of a shared reverb), its level
               against the dry track (wet/dry dB) and the reverb's decay time
  pan          separate gains into the mix's left and right

and the mix itself gives the loudness, loudness range, crest, width and tonal balance.
The per-track numbers are then pooled by role (voice, background vocals, drums...),
and checked song by song, each song left out of the pooling it is checked against
(leave-one-song-out), so the report says how well targets learned from other songs
predict a new one, next to what Auto Mix aims for today.
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from scipy.optimize import nnls

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mlf  # noqa: E402

HOP = 0.1
CENTRES = 1000.0 * 2.0 ** (np.arange(-16, 14) / 3.0)
FIT_BANDS = np.where((CENTRES >= 50.0) & (CENTRES <= 12500.0))[0]
TONE_BANDS = np.where((CENTRES >= 40.0) & (CENTRES <= 16000.0))[0]

# Band groups for EQ (about an octave each, over the bands that are fitted), with names.
GROUP_EDGES = [50, 100, 200, 400, 800, 1600, 3150, 5000, 8000, 12600]
GROUP_NAMES = ["50", "100", "200", "400", "800", "1k6", "3k15", "5k", "8k"]


def band_groups():
    groups = []
    for lo, hi in zip(GROUP_EDGES[:-1], GROUP_EDGES[1:]):
        groups.append([b for b in FIT_BANDS if lo * 0.95 <= CENTRES[b] < hi * 0.95])
    return groups


GROUPS = band_groups()
SIBILANCE_GROUP = GROUP_NAMES.index("5k")


def k_weights():
    """BS.1770 K-weighting (shelf + high pass) at the band centres, as power gains."""
    f = CENTRES
    shelf_db = 4.0 / (1.0 + (1500.0 / f) ** 2)          # ~+4 dB above 2 kHz
    hp = (f / 38.0) ** 4 / (1.0 + (f / 38.0) ** 4)       # 2nd-order high pass at 38 Hz (power)
    return 10.0 ** (shelf_db / 10.0) * hp


KW = k_weights()


def db(x, floor=-200.0):
    x = np.asarray(x, dtype=float)
    with np.errstate(divide="ignore"):
        return np.where(x > 1e-20, 10.0 * np.log10(np.maximum(x, 1e-30)), floor)


def loudness(power_bands):
    """Approximate K-weighted loudness per hop from band powers (LUFS-like, -0.691 offset)."""
    return db(power_bands @ KW) - 0.691


def momentary(power_bands):
    """Loudness over the last 400 ms (4 hops)."""
    p = power_bands @ KW
    kernel = np.ones(4) / 4.0
    return db(np.convolve(p, kernel)[: len(p)]) - 0.691


def integrated(power_bands):
    """BS.1770 gated integrated loudness over 400 ms blocks (hops of 100 ms)."""
    m = momentary(power_bands)
    p = 10.0 ** ((m + 0.691) / 10.0)
    gated = m > -70.0
    if not gated.any():
        return -200.0
    rel = 10.0 * np.log10(p[gated].mean()) - 0.691 - 10.0
    gated &= m > rel
    return float(10.0 * np.log10(p[gated].mean()) - 0.691) if gated.any() else -200.0


def loudness_range(power_bands):
    p = power_bands @ KW
    st = np.convolve(p, np.ones(30) / 30.0)[: len(p)]
    st_db = db(st) - 0.691
    st_db = st_db[st_db > -70.0]
    if len(st_db) < 10:
        return 0.0
    rel = 10.0 * np.log10((10.0 ** ((st_db + 0.691) / 10.0)).mean()) - 0.691 - 20.0
    st_db = st_db[st_db > rel]
    return float(np.percentile(st_db, 95) - np.percentile(st_db, 10)) if len(st_db) else 0.0


# ----------------------------------------------------------------------------------------
class SongFit:
    """Everything learned from one song."""

    def __init__(self, song):
        self.song = song
        self.name = song.name
        self.style = song.style
        self.tracks = []        # per track dicts
        self.mix = {}
        self.errors = {}


def find_lag(tracks_power, mix_power, max_lag=100):
    a = db(sum(t.sum(axis=1) for t in tracks_power))
    m = db(mix_power.sum(axis=1))
    a = np.maximum(a, -70.0)
    m = np.maximum(m, -70.0)
    best, best_lag = -2.0, 0
    for lag in range(-max_lag, max_lag + 1):
        i0 = max(0, -lag)
        i1 = min(len(a), len(m) - lag)
        if i1 - i0 < 50:
            continue
        x, y = a[i0:i1], m[i0 + lag:i1 + lag]
        if x.std() == 0 or y.std() == 0:
            continue
        c = np.corrcoef(x, y)[0, 1]
        if c > best + 1e-9:
            best, best_lag = c, lag
    return best_lag


def nnls_relative(columns, y, weights=None):
    """Nonnegative gains g minimising Σ w (Σ g_t x_t / y - 1)² over the rows. columns: [rows, n]."""
    a = columns / y[:, None]
    b = np.ones(len(y))
    if weights is not None:
        sw = np.sqrt(weights)
        a = a * sw[:, None]
        b = b * sw
    scale = np.sqrt((a ** 2).sum(axis=0))
    scale[scale == 0] = 1.0
    g, _ = nnls(a / scale, b, maxiter=2000)
    return g / scale


def model_error_db(columns, y, g):
    model = columns @ g
    ok = (model > 0) & (y > 0)
    if not ok.any():
        return 99.0
    return float(np.sqrt(np.mean((10.0 * np.log10(model[ok] / y[ok])) ** 2)))


def smooth_level(power_hops, hops):
    """Loudness of a track per hop, power-averaged over the last `hops` hops."""
    p = power_hops @ KW
    if hops > 1:
        p = np.convolve(p, np.ones(hops) / hops)[: len(p)]
    return db(p)


def decay_tail(x, tau_hops, start=2, length=40):
    """Σ_{k>=start} x[h-k] e^{-k/tau}: a reverb tail of each band (power), per hop."""
    out = np.zeros_like(x)
    for k in range(start, start + length):
        w = math.exp(-k / tau_hops)
        if w < 1e-3:
            break
        out[k:] += w * x[:-k]
    return out


def fit_song(song, subsample=2):
    fit = SongFit(song)
    if song.mix is None or not song.tracks:
        return None

    raw = [t.mono_power() for t in song.tracks]
    mix_p = song.mix.mono_power()
    lag = find_lag(raw, mix_p)
    n_hops = min(max(len(r) for r in raw), len(mix_p) - max(0, lag))
    if lag < 0:
        n_hops = min(n_hops, len(mix_p))

    # Track powers on the mix's time grid (track hop i lands on mix hop i + lag).
    def on_grid(x):
        out = np.zeros((len(mix_p), x.shape[1]))
        for i in range(len(x)):
            j = i + lag
            if 0 <= j < len(out):
                out[j] = x[i]
        return out

    X = [on_grid(r) for r in raw]
    XL = [on_grid(t.left()) for t in song.tracks]
    XR = [on_grid(t.right()) for t in song.tracks]
    n = len(X)

    mix_level = loudness(mix_p)
    active = mix_level > (np.max(mix_level) - 40.0)
    hops = np.where(active)[0][::subsample]
    fb = FIT_BANDS

    def rows(arrays, bands=fb, target=mix_p, hop_idx=hops):
        cols = np.stack([a[np.ix_(hop_idx, bands)].ravel() for a in arrays], axis=1)
        y = target[np.ix_(hop_idx, bands)].ravel()
        ok = (y > 1e-14) & (cols.sum(axis=1) > 0)
        return cols[ok], y[ok]

    # 1. Level: one gain per track.
    cols, y = rows(X)
    g = nnls_relative(cols, y)
    fit.errors["level"] = model_error_db(cols, y, g)

    # 2. EQ: a gain per band group per track (each group fitted on its own bands).
    eq = np.full((n, len(GROUPS)), np.nan)
    g_group = np.zeros((n, len(GROUPS)))
    for gi, bands in enumerate(GROUPS):
        if not bands:
            continue
        c, yy = rows(X, bands)
        if len(yy) < 50:
            continue
        gg = nnls_relative(c, yy)
        g_group[:, gi] = gg
        for t in range(n):
            # Only where the track has real energy in that group (else the gain is noise).
            share = c[:, t] * gg[t] / yy
            if g[t] > 0 and gg[t] > 0 and np.mean(share) > 0.02:
                eq[t, gi] = 10.0 * math.log10(gg[t] / g[t])
    # The static per-band model (for the error, and as the base of steps 3-4).
    gains_bands = np.zeros((n, mix_p.shape[1]))
    for gi, bands in enumerate(GROUPS):
        for b in bands:
            gains_bands[:, b] = g_group[:, gi]
    Xeq = [X[t] * gains_bands[t][None, :] for t in range(n)]
    cols_eq, y_eq = rows(Xeq)
    fit.errors["eq"] = model_error_db(cols_eq, y_eq, np.ones(n))

    # 3. Compression: gain falls by `slope` dB per dB over the track's median loudness.
    def compressed(t, slope, smooth, base=None):
        base = Xeq[t] if base is None else base
        if slope <= 0.0:
            return base
        lvl = smooth_level(X[t], smooth)
        act = lvl > (np.max(lvl) - 45.0)
        if act.sum() < 20:
            return base
        thr = np.percentile(lvl[act], 50)
        gr_db = -slope * np.maximum(0.0, lvl - thr)
        return base * (10.0 ** (gr_db / 10.0))[:, None]

    slopes = np.zeros(n)
    smooths = np.ones(n, dtype=int) * 4
    current = list(Xeq)
    cols_c, y_c = rows(current)
    best_err = model_error_db(cols_c, y_c, nnls_relative(cols_c, y_c))
    for _ in range(2):
        for t in range(n):
            if g[t] <= 0:
                continue
            for smooth in (1, 4, 15):
                for slope in (0.0, 0.15, 0.3, 0.45, 0.6, 0.75):
                    trial = list(current)
                    trial[t] = compressed(t, slope, smooth)
                    c, yy = rows(trial)
                    e = model_error_db(c, yy, nnls_relative(c, yy))
                    if e < best_err - 0.005:
                        best_err, slopes[t], smooths[t] = e, slope, smooth
                        current = trial
    fit.errors["compression"] = best_err

    # Gain reduction the fitted slope makes on each track (mean while active, and the 95th percentile).
    def gain_reduction(t):
        if slopes[t] <= 0:
            return 0.0, 0.0
        lvl = smooth_level(X[t], smooths[t])
        act = lvl > (np.max(lvl) - 45.0)
        thr = np.percentile(lvl[act], 50)
        gr = slopes[t] * np.maximum(0.0, lvl[act] - thr)
        return float(gr.mean()), float(np.percentile(gr, 95))

    # 4. De-essing on voices: an extra slope in the 5-8 kHz group, on that group's own level.
    deess = np.zeros(n)
    for t in range(n):
        if song.tracks[t].role not in ("voice", "background") or not GROUPS[SIBILANCE_GROUP]:
            continue
        bands = GROUPS[SIBILANCE_GROUP]
        c0, y0 = rows(current, bands)
        if len(y0) < 50:
            continue
        e0 = model_error_db(c0, y0, nnls_relative(c0, y0))
        band_lvl = db(X[t][:, bands].sum(axis=1))
        act = band_lvl > (np.max(band_lvl) - 40.0)
        if act.sum() < 20:
            continue
        thr = np.percentile(band_lvl[act], 70)
        for slope in (0.2, 0.4, 0.6):
            trial = list(current)
            trial[t] = current[t].copy()
            trial[t][:, bands] *= (10.0 ** (-slope * np.maximum(0.0, band_lvl - thr) / 10.0))[:, None]
            c1, y1 = rows(trial, bands)
            e1 = model_error_db(c1, y1, nnls_relative(c1, y1))
            if e1 < e0 - 0.01:
                e0, deess[t] = e1, slope

    # 5. Reverb: a decaying copy of every track, one decay time for the song.
    best_rev = None
    for tau_s in (0.04, 0.08, 0.15, 0.25, 0.4):  # power decay time constants: RT60 = 13.8 tau, 0.55 to 5.5 s
        tails = [decay_tail(current[t], tau_s / HOP) for t in range(n)]
        c, yy = rows(current + tails)
        gg = nnls_relative(c, yy)
        e = model_error_db(c, yy, gg)
        if best_rev is None or e < best_rev[0]:
            best_rev = (e, tau_s, gg)
    rev_err, tau, g_rev = best_rev
    fit.errors["reverb"] = rev_err
    wet_db = np.full(n, np.nan)
    for t in range(n):
        dry, wet = g_rev[t], g_rev[n + t]
        if dry > 0 and wet > 0:
            # Tail energy over dry energy for this track (as a send level would make it).
            tail_sum = sum(math.exp(-k / (tau / HOP)) for k in range(2, 42) if math.exp(-k / (tau / HOP)) >= 1e-3)
            wet_db[t] = 10.0 * math.log10(wet * tail_sum / dry)
        elif dry > 0:
            wet_db[t] = -40.0

    # 6. Pan: the same model into the mix's left and right channels.
    stereo_mix = song.mix.right_db is not None
    pan_db = np.zeros(n)
    if stereo_mix:
        cl, yl = rows([XL[t] * gains_bands[t][None, :] for t in range(n)], target=song.mix.left())
        cr, yr = rows([XR[t] * gains_bands[t][None, :] for t in range(n)], target=song.mix.right())
        gl, gr = nnls_relative(cl, yl), nnls_relative(cr, yr)
        for t in range(n):
            if gl[t] > 0 or gr[t] > 0:
                pan_db[t] = 10.0 * math.log10(max(gl[t], 1e-6) / max(gr[t], 1e-6))
            pan_db[t] = float(np.clip(pan_db[t], -30.0, 30.0))

    # Final per-track contribution (level + eq) and role levels against the mix.
    mix_loud = loudness(mix_p)
    for t, track in enumerate(song.tracks):
        contrib = X[t] * gains_bands[t][None, :]
        lvl = loudness(contrib)
        raw_lvl = loudness(X[t])
        act = raw_lvl > (np.max(raw_lvl) - 35.0) if np.max(raw_lvl) > -150 else np.zeros(len(raw_lvl), bool)
        act &= active
        rel_lu = float(10.0 * np.log10(np.mean(10.0 ** (lvl[act] / 10.0))) - 10.0 * np.log10(np.mean(10.0 ** (mix_loud[act] / 10.0)))) if act.sum() > 10 else float("nan")
        raw_spec = db(X[t][act].mean(axis=0)) if act.sum() > 10 else np.full(30, -200.0)
        gr_mean, gr95 = gain_reduction(t)
        fit.tracks.append({
            "name": track.name, "role": track.role, "stereo": track.channels > 1,
            "gain_db": float(10.0 * math.log10(g[t])) if g[t] > 0 else -200.0,
            "active_s": float(act.sum() * HOP),
            "raw_lufs": integrated(X[t]),
            "raw_lra": loudness_range(X[t]),
            "raw_crest_db": float(np.percentile(track.peak_db[track.peak_db > -150], 99) - np.percentile(track.rms_db[track.rms_db > -150], 50)) if (track.rms_db > -150).sum() > 10 else 0.0,
            "level_lu": rel_lu,
            "eq_db": [None if np.isnan(v) else round(float(v), 2) for v in eq[t]],
            "raw_tone": [round(float(v), 2) for v in raw_spec],
            "comp_slope": float(slopes[t]), "comp_ratio": float(1.0 / (1.0 - slopes[t])),
            "comp_smooth_s": float(smooths[t] * HOP),
            "comp_gr_mean_db": gr_mean, "comp_gr95_db": gr95,
            "deess_slope": float(deess[t]),
            "wet_db": None if np.isnan(wet_db[t]) else float(wet_db[t]),
            "pan_db": float(pan_db[t]),
        })

    # Role groups' combined level against the mix while they play, voice over the rest.
    def role_power(predicate):
        p = np.zeros_like(mix_p)
        for t, track in enumerate(song.tracks):
            if predicate(track.role):
                p += X[t] * gains_bands[t][None, :]
        return p

    def rel(p_role, p_rest_or_mix, mask):
        a = loudness(p_role)[mask]
        b = loudness(p_rest_or_mix)[mask]
        if mask.sum() < 10:
            return None
        return float(10 * np.log10(np.mean(10 ** (a / 10))) - 10 * np.log10(np.mean(10 ** (b / 10))))

    voice = role_power(lambda r: r == "voice")
    others = role_power(lambda r: r != "voice")
    v_lvl = loudness(voice)
    sings = active & (v_lvl > (np.max(v_lvl) - 30.0)) if voice.any() else np.zeros(len(mix_p), bool)
    fit.mix["voice_over_backing_lu"] = rel(voice, others, sings) if voice.any() else None
    bv = role_power(lambda r: r == "background")
    bv_lvl = loudness(bv)
    both = sings & (bv_lvl > (np.max(bv_lvl) - 30.0)) if bv.any() else np.zeros(len(mix_p), bool)
    fit.mix["background_under_lead_lu"] = (-rel(bv, voice, both)) if bv.any() and both.sum() > 10 else None
    fit.mix["role_lu"] = {}
    for role in sorted({t.role for t in song.tracks}):
        p = role_power(lambda r, role=role: r == role)
        lvl = loudness(p)
        mask = active & (lvl > (np.max(lvl) - 30.0))
        fit.mix["role_lu"][role] = rel(p, mix_p, mask)

    # The mix as a whole.
    m = song.mix
    fit.mix["lufs"] = integrated(mix_p)
    fit.mix["lra"] = loudness_range(mix_p)
    pk = m.peak_db[m.peak_db > -150]
    rm = m.rms_db[m.rms_db > -150]
    fit.mix["peak_db"] = float(pk.max()) if len(pk) else -200.0
    fit.mix["crest_db"] = float(pk.max() - 10 * np.log10(np.mean(10 ** (rm / 10)))) if len(rm) else 0.0
    tone = db(mix_p[active].mean(axis=0))
    fit.mix["tone"] = [round(float(v - np.mean(tone[TONE_BANDS])), 2) for v in tone]
    if stereo_mix:
        mid, side = m.mid()[active], m.side()[active]
        fit.mix["width_db"] = float(db(side[:, FIT_BANDS].sum()) - db(mid[:, FIT_BANDS].sum()))
        lo = [b for b in FIT_BANDS if CENTRES[b] < 120]
        fit.mix["low_width_db"] = float(db(side[:, lo].sum()) - db(mid[:, lo].sum()))
    fit.mix["reverb_decay_s"] = float(tau * 13.8)
    fit.mix["lag_s"] = lag * HOP
    return fit


# ----------------------------------------------------------------------------------------
ROLES = ["voice", "background", "drums", "bass", "guitar", "piano", "pad", "synth", "instrument", "instrumental", "other"]


def weighted_median(values, weights):
    v = np.asarray(values, float)
    w = np.asarray(weights, float)
    ok = ~np.isnan(v)
    v, w = v[ok], w[ok]
    if len(v) == 0:
        return None
    order = np.argsort(v)
    v, w = v[order], w[order]
    c = np.cumsum(w)
    return float(v[np.searchsorted(c, c[-1] / 2.0)])


def pool(fits):
    """Role targets from a set of song fits (medians, weighted by how long each track plays)."""
    out = {"roles": {}, "mix": {}, "songs": [f.name for f in fits]}
    for role in ROLES:
        tr = [t for f in fits for t in f.tracks if t["role"] == role and t["gain_db"] > -150 and t["active_s"] > 5]
        if not tr:
            continue
        w = [t["active_s"] for t in tr]
        role_levels = [f.mix["role_lu"].get(role) for f in fits if f.mix["role_lu"].get(role) is not None]
        eq = []
        for gi in range(len(GROUPS)):
            vals = [t["eq_db"][gi] for t in tr if t["eq_db"][gi] is not None]
            ws = [t["active_s"] for t in tr if t["eq_db"][gi] is not None]
            eq.append(None if len(vals) < 2 else round(weighted_median(vals, ws), 2))
        # Tone of the role as it sits in the mix: raw spectrum + its EQ, mean 0 over the fitted bands.
        tones = []
        for t in tr:
            tone = np.array(t["raw_tone"])
            if np.all(tone <= -150):
                continue
            adj = tone.copy()
            for gi, bands in enumerate(GROUPS):
                if t["eq_db"][gi] is not None:
                    adj[bands] += t["eq_db"][gi]
            adj -= adj[FIT_BANDS].mean()
            tones.append(adj)
        comp = [t for t in tr if t["comp_slope"] > 0]
        out["roles"][role] = {
            "tracks": len(tr),
            "songs": len({f.name for f in fits for t in f.tracks if t["role"] == role}),
            "level_lu": None if not role_levels else round(float(np.median(role_levels)), 2),
            "eq_db": eq,
            "tone": None if not tones else [round(float(v), 2) for v in np.median(np.array(tones), axis=0)],
            "comp_share": round(len(comp) / len(tr), 2),
            "comp_ratio": round(weighted_median([t["comp_ratio"] for t in tr], w), 2),
            "comp_gr_mean_db": round(weighted_median([t["comp_gr_mean_db"] for t in tr], w), 2),
            "comp_gr95_db": round(weighted_median([t["comp_gr95_db"] for t in tr], w), 2),
            "comp_smooth_s": None if not comp else round(weighted_median([t["comp_smooth_s"] for t in comp], [t["active_s"] for t in comp]), 2),
            "raw_lra": round(weighted_median([t["raw_lra"] for t in tr], w), 2),
            "deess_slope": round(weighted_median([t["deess_slope"] for t in tr], w), 2),
            "wet_db": None if all(t["wet_db"] is None for t in tr) else round(weighted_median([np.nan if t["wet_db"] is None else t["wet_db"] for t in tr], w), 2),
            "pan_abs_db": round(weighted_median([abs(t["pan_db"]) for t in tr], w), 2),
        }
    for key in ("voice_over_backing_lu", "background_under_lead_lu", "lufs", "lra", "crest_db", "width_db", "low_width_db", "reverb_decay_s", "peak_db"):
        vals = [f.mix.get(key) for f in fits if f.mix.get(key) is not None]
        out["mix"][key] = None if not vals else round(float(np.median(vals)), 2)
    tones = np.array([f.mix["tone"] for f in fits])
    out["mix"]["tone"] = [round(float(v), 2) for v in np.median(tones, axis=0)]
    return out


# Auto Mix's targets today (MixAssistant.cpp getTargets, pop), for the before/after table.
TODAY = {
    "voice_over_backing_lu": 1.5,
    "background_under_lead_lu": 6.0,
    "lufs": -14.0,
}


def held_out(fits):
    """Leave one song out: targets pooled from the others against the song's own numbers."""
    rows = []
    for i, f in enumerate(fits):
        others = [g for j, g in enumerate(fits) if j != i]
        if not others:
            continue
        p = pool(others)
        row = {"song": f.name}
        for key in ("voice_over_backing_lu", "background_under_lead_lu"):
            actual = f.mix.get(key)
            if actual is None:
                continue
            row[key] = (actual, p["mix"].get(key), TODAY.get(key))
        role_err_learned, role_err_today = [], []
        for role, actual in f.mix["role_lu"].items():
            target = p["roles"].get(role, {}).get("level_lu")
            if actual is not None and target is not None:
                role_err_learned.append(abs(actual - target))
        row["role_level_error_lu"] = float(np.mean(role_err_learned)) if role_err_learned else None
        eq_err, eq_err_flat = [], []
        for t in f.tracks:
            target = p["roles"].get(t["role"], {}).get("eq_db")
            if not target:
                continue
            for gi, v in enumerate(t["eq_db"]):
                if v is not None and target[gi] is not None:
                    eq_err.append(abs(v - target[gi]))
                    eq_err_flat.append(abs(v))
        row["eq_error_db"] = (float(np.mean(eq_err)) if eq_err else None, float(np.mean(eq_err_flat)) if eq_err_flat else None)
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("--out", default="learned.json")
    parser.add_argument("--fits", default=None, help="write every song's fit here (json)")
    parser.add_argument("--subsample", type=int, default=2)
    args = parser.parse_args()

    files = sorted(os.path.join(args.folder, f) for f in os.listdir(args.folder) if f.endswith(".mlf"))
    fits = []
    for path in files:
        song = mlf.load(path)
        print(f"{song.name}: {len(song.tracks)} tracks", flush=True)
        fit = fit_song(song, args.subsample)
        if fit is None:
            print("  no mix, skipped")
            continue
        print("  model error dB: " + ", ".join(f"{k} {v:.2f}" for k, v in fit.errors.items()), flush=True)
        fits.append(fit)

    learned = pool(fits)
    learned["held_out"] = held_out(fits)
    with open(args.out, "w") as f:
        json.dump(learned, f, indent=1)
    if args.fits:
        with open(args.fits, "w") as f:
            json.dump([{"song": x.name, "style": x.style, "errors": x.errors, "mix": x.mix, "tracks": x.tracks} for x in fits], f, indent=1)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
