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
    total = np.zeros(max(len(t) for t in tracks_power))
    for t in tracks_power:
        total[: len(t)] += t.sum(axis=1)
    a = db(total)
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


def align_hops(tracks_power, mix_power, window=80, step=20, max_lag=600, min_corr=0.6):
    """Which track hop each mix hop plays, allowing for edits.

    The human mixes of a multitrack library often start later than the raw tracks and are
    sometimes edited (a section cut): one lag for the whole song doesn't hold. Every 2 s,
    a window of 8 s of the mix's band envelopes (8 bands, log energy) is matched against the
    sum of the raw tracks over ±60 s; each mix hop takes the lag of the windows around it
    (median), and hops whose windows don't match well (correlation under `min_corr`) are left out.
    Returns (source index per mix hop, -1 where left out; the median lag in hops)."""
    n_t = max(len(t) for t in tracks_power)
    groups = [g for g in GROUPS if g]
    tot = np.zeros((n_t, len(groups)))
    for t in tracks_power:
        for k, g in enumerate(groups):
            tot[: len(t), k] += t[:, g].sum(axis=1)
    mix = np.stack([mix_power[:, g].sum(axis=1) for g in groups], axis=1)

    def feats(x):
        f = np.log10(np.maximum(x, 1e-12))
        f = np.maximum(f, f.max(axis=0) - 6.0)            # 60 dB floor per band
        return (f - f.mean(axis=0)) / (f.std(axis=0) + 1e-9)

    a, b = feats(tot), feats(mix)
    n_m = len(b)
    lag_values = np.arange(-max_lag, max_lag + 1)
    scores, centres = [], []
    for start in range(0, max(1, n_m - window + 1), step):
        seg = b[start:start + window]
        seg = seg - seg.mean(axis=0)
        row = np.full(len(lag_values), -1.0)
        for li, lag in enumerate(lag_values):
            i0 = start - lag
            if i0 < 0 or i0 + window > n_t:
                continue
            ref = a[i0:i0 + window]
            ref = ref - ref.mean(axis=0)
            den = np.sqrt((seg ** 2).sum() * (ref ** 2).sum())
            if den > 0:
                row[li] = (seg * ref).sum() / den
        scores.append(row); centres.append(start + window // 2)
    if not scores:
        return np.full(n_m, -1, dtype=int), 0
    scores = np.array(scores)
    # The most likely path of lags: staying put is free, a small drift is cheap, a jump (an
    # edit) costs as much as a fair part of a window's match, so repeated sections of a song
    # can't pull single windows to the wrong place.
    total_, back = scores[0].copy(), []
    for w in range(1, len(scores)):
        stay = total_
        drift = np.maximum(np.roll(total_, 1), np.roll(total_, -1)) - 0.02
        jump = np.full_like(total_, total_.max() - 2.0)
        options = np.stack([stay, drift, jump])
        choice = options.argmax(axis=0)
        back.append((choice, np.roll(np.arange(len(total_)), 1), np.roll(np.arange(len(total_)), -1), int(total_.argmax())))
        best_prev = np.where(choice == 0, np.arange(len(total_)),
                             np.where(choice == 2, int(total_.argmax()),
                                      np.where(np.roll(total_, 1) >= np.roll(total_, -1), np.roll(np.arange(len(total_)), 1),
                                               np.roll(np.arange(len(total_)), -1))))
        back[-1] = best_prev
        total_ = options.max(axis=0) + scores[w]
    path = [int(total_.argmax())]
    for w in range(len(scores) - 1, 0, -1):
        path.append(int(back[w - 1][path[-1]]))
    path = path[::-1]
    lags_w = lag_values[path]
    corr_w = scores[np.arange(len(scores)), path]
    centres = np.array(centres)
    src = np.full(n_m, -1, dtype=int)
    for j in range(n_m):
        near = np.abs(centres - j) <= window // 2
        good = near & (corr_w >= min_corr)
        if good.sum() == 0:
            continue
        lag = int(np.median(lags_w[good]))
        i = j - lag
        if 0 <= i < n_t:
            src[j] = i
    ok = corr_w >= min_corr
    return src, int(np.median(lags_w[ok])) if ok.any() else 0


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


def fit_gains(columns, y):
    """Nonnegative gains with the model's error measured in dB (robustly, soft L1 at 3 dB):
    the relative-error least squares (nnls_relative) as a start, which underestimates a mix it
    can't fit exactly, then refined on log gains. Tracks the start leaves out stay out."""
    from scipy.optimize import least_squares
    g0 = nnls_relative(columns, y)
    on = g0 > 0
    if not on.any():
        return g0
    c = columns[:, on]
    ly = 10.0 * np.log10(y)

    def residual(x):
        return 10.0 * np.log10(np.maximum(c @ (10.0 ** (x / 10.0)), 1e-30)) - ly

    def jac(x):
        g = 10.0 ** (x / 10.0)
        model = np.maximum(c @ g, 1e-30)
        return (c * g[None, :]) / model[:, None]

    x0 = 10.0 * np.log10(g0[on])
    sol = least_squares(residual, x0, jac=jac, loss="soft_l1", f_scale=3.0, bounds=(x0 - 40.0, x0 + 40.0), max_nfev=60)
    g = np.zeros_like(g0)
    g[on] = 10.0 ** (sol.x / 10.0)
    return g


def model_error_db(columns, y, g):
    """Median |model - mix| in dB over the (hop, band) cells."""
    model = columns @ g
    ok = (model > 0) & (y > 0)
    if not ok.any():
        return 99.0
    return float(np.median(np.abs(10.0 * np.log10(model[ok] / y[ok]))))


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


# ----------------------------------------------------------------------------------------
# Roles from track names. The bench's guesses (the app's guessRoleFromName, then the sound)
# miss multitrack names like "BassAmpMic1" (heard as a voice for "mic"), "ElecGtr1a_VoxM160"
# (a Vox amp) or "Tom1"; these rules, the same as the app's since this learning pass, read
# camel case and numbers as separate words and let an instrument's name win over "vox"/"mic".
import re

def split_words(name):
    s = re.sub(r"([a-z])([A-Z])", r"\1 \2", name)
    s = re.sub(r"([A-Za-z])([0-9])", r"\1 \2", s)
    s = re.sub(r"([0-9])([A-Za-z])", r"\1 \2", s)
    s = re.sub(r"[_\-\.\(\)\[\]]", " ", s).lower()
    return s.split()


def role_from_name(name):
    w = split_words(name)
    text = " ".join(w)
    def part(*xs):
        return any(x in text for x in xs)
    def word(*xs):
        return any(x in w for x in xs)
    if part("karaoke", "no vocal", "instrumental"):
        return "instrumental"
    explicit_vocal = part("vocal", "voice", "voz", "sing") or word("vox", "vx", "vo", "rap", "mc")
    drums = part("drum", "kick", "snare", "hihat", "hi hat", "overhead", "cymbal", "perc", "tambourine", "shaker",
                 "clap", "bongo", "conga", "djembe", "sidestick", "808") or word("tom", "toms", "ride", "crash", "hat",
                                                                                    "hats", "oh", "kd", "sn", "loop", "loops", "fill")
    bass = part("bass", "bajo") or word("sub")
    guitar = part("guitar", "gtr", "banjo", "mandolin", "ukulele", "uke", "dobro")
    keys = part("piano", "keys", "keyboard", "rhodes", "wurli", "organ", "clav") or word("ep", "pno")
    pad = part("string", "violin", "viola", "cello", "orchestra", "pad")
    synth = part("synth", "arp", "pluck")
    instrument_named = drums or bass or guitar or keys or pad or synth
    # "Vocals-Acoustic Guitar-Proto" is a vocal mic; "ElecGtr1a_VoxM160" is a guitar amp.
    vocal = part("vocal", "voice", "voz", "sing") or (explicit_vocal and not instrument_named) or word("lead") and not instrument_named
    if vocal or word("gang", "gangs", "choir", "bv", "bvs", "bgv", "backs") or part("backing vox", "backingvox", "harmon", "adlib"):
        if part("backing", "background", "harmon", "choir", "bgv", "adlib", "gang", "backs") or word("bv", "bvs", "bvox"):
            return "background"
        return "voice"
    if drums:
        return "drums"
    if bass:
        return "bass"
    if guitar:
        return "guitar"
    if keys:
        return "piano"
    if pad:
        return "pad"
    if synth:
        return "synth"
    if part("room"):
        return "other"
    return None


def relabel(track):
    return role_from_name(track.name) or track.role or "other"


# Roles pooled into one stem each (the busses an engineer would compress, EQ and send).
STEM_ROLES = ["voice", "background", "drums", "bass", "guitar", "piano", "pad", "synth", "instrument", "other", "instrumental"]


def fit_song(song, subsample=3):
    fit = SongFit(song)
    if song.mix is None or not song.tracks:
        return None

    roles = [relabel(t) for t in song.tracks]
    raw = [t.mono_power() for t in song.tracks]
    mix_p = song.mix.mono_power()
    src, lag = align_hops(raw, mix_p)
    aligned = src >= 0

    # Track powers on the mix's time grid (the track hop each mix hop plays).
    def on_grid(x):
        out = np.zeros((len(mix_p), x.shape[1]))
        ok = aligned & (src < len(x))
        out[ok] = x[src[ok]]
        return out

    X = [on_grid(r) for r in raw]
    n = len(X)
    mix_level = loudness(mix_p)
    active = (mix_level > (np.max(mix_level) - 40.0)) & aligned
    fit.mix["aligned_share"] = float(aligned.mean())
    hops = np.where(active)[0][::subsample]

    def rows(arrays, bands=FIT_BANDS, target=mix_p):
        cols = np.stack([a[np.ix_(hops, bands)].ravel() for a in arrays], axis=1)
        y = target[np.ix_(hops, bands)].ravel()
        ok = (y > 1e-14) & (cols.sum(axis=1) > 0)
        return cols[ok], y[ok]

    # 1. Level: one gain per track.
    cols, y = rows(X)
    g = fit_gains(cols, y)
    fit.errors["level"] = model_error_db(cols, y, g)

    # Stems: each role's tracks at their fitted levels.
    stem_names = [r for r in STEM_ROLES if any(roles[t] == r and g[t] > 0 for t in range(n))]
    S = []
    for r in stem_names:
        s_ = np.zeros_like(mix_p)
        for t in range(n):
            if roles[t] == r and g[t] > 0:
                s_ += g[t] * X[t]
        S.append(s_)
    m = len(S)

    # 2. EQ: a gain per band group per stem, over its level (each group fitted on its own bands).
    eq = np.full((m, len(GROUPS)), np.nan)
    g_group = np.ones((m, len(GROUPS)))
    for gi, bands in enumerate(GROUPS):
        c, yy = rows(S, bands)
        if len(yy) < 50:
            continue
        gg = fit_gains(c, yy)
        for k in range(m):
            share = np.mean(c[:, k] * gg[k] / yy)
            if share > 0.03 and gg[k] > 0:   # the stem is a real part of this group
                eq[k, gi] = 10.0 * math.log10(gg[k])
                g_group[k, gi] = gg[k]
    # EQ as a shape: the stem's own energy-weighted mean is its level, not EQ.
    for k in range(m):
        e = np.array([S[k][:, b].sum() for b in range(30)])
        wts = np.array([e[bands].sum() if not np.isnan(eq[k, gi]) else 0.0 for gi, bands in enumerate(GROUPS)])
        if wts.sum() > 0:
            mean = np.nansum(eq[k] * wts) / wts.sum()
            eq[k] -= mean
            g_group[k] /= 10.0 ** (mean / 10.0)
    gb = np.ones((m, 30))
    for gi, bands in enumerate(GROUPS):
        for b in bands:
            gb[:, b] = g_group[:, gi]
    Seq = [S[k] * gb[k][None, :] for k in range(m)]
    c, yy = rows(Seq)
    fit.errors["eq"] = model_error_db(c, yy, fit_gains(c, yy))

    # 3. Compression per stem: gain falls by `slope` dB per dB over the stem's median loudness.
    def level_of(k, smooth):
        lvl = smooth_level(S[k], smooth)
        act = active & (lvl > np.max(lvl) - 40.0)
        return lvl, act

    def compressed(k, slope, smooth):
        if slope <= 0.0:
            return Seq[k]
        lvl, act = level_of(k, smooth)
        if act.sum() < 20:
            return Seq[k]
        thr = np.percentile(lvl[act], 50)
        return Seq[k] * (10.0 ** (-slope * np.maximum(0.0, lvl - thr) / 10.0))[:, None]

    slopes, smooths = np.zeros(m), np.full(m, 4)
    current = list(Seq)
    c, yy = rows(current)
    best_err = model_error_db(c, yy, fit_gains(c, yy))
    for _ in range(2):
        for k in range(m):
            for smooth in (1, 4, 15):
                for slope in (0.0, 0.15, 0.3, 0.45, 0.6, 0.75):
                    trial = list(current)
                    trial[k] = compressed(k, slope, smooth)
                    c, yy = rows(trial)
                    e = model_error_db(c, yy, fit_gains(c, yy))
                    if e < best_err - 0.005:
                        best_err, slopes[k], smooths[k], current = e, slope, smooth, trial
    fit.errors["compression"] = best_err

    def gain_reduction(k):
        if slopes[k] <= 0:
            return 0.0, 0.0
        lvl, act = level_of(k, smooths[k])
        thr = np.percentile(lvl[act], 50)
        gr = slopes[k] * np.maximum(0.0, lvl[act] - thr)
        return float(gr.mean()), float(np.percentile(gr, 95))

    # 4. De-essing on voices: an extra slope in the 5-8 kHz group, on that group's own level.
    deess = np.zeros(m)
    bands = GROUPS[SIBILANCE_GROUP]
    for k, r in enumerate(stem_names):
        if r not in ("voice", "background"):
            continue
        c0, y0 = rows(current, bands)
        e0 = model_error_db(c0, y0, fit_gains(c0, y0))
        band_lvl = db(S[k][:, bands].sum(axis=1))
        act = active & (band_lvl > np.max(band_lvl) - 40.0)
        if act.sum() < 20:
            continue
        thr = np.percentile(band_lvl[act], 70)
        for slope in (0.2, 0.4, 0.6):
            trial = list(current)
            trial[k] = current[k].copy()
            trial[k][:, bands] *= (10.0 ** (-slope * np.maximum(0.0, band_lvl - thr) / 10.0))[:, None]
            c1, y1 = rows(trial, bands)
            e1 = model_error_db(c1, y1, fit_gains(c1, y1))
            if e1 < e0 - 0.01:
                e0, deess[k] = e1, slope

    # 5. Reverb: a decaying copy of every stem (a shared reverb's tail), one decay time for the song.
    best = None
    for tau_s in (0.04, 0.08, 0.15, 0.25, 0.4):  # power decay constants: RT60 = 13.8 tau, 0.55 to 5.5 s
        tails = [decay_tail(current[k], tau_s / HOP) for k in range(m)]
        c, yy = rows(current + tails)
        gg = fit_gains(c, yy)
        e = model_error_db(c, yy, gg)
        if best is None or e < best[0]:
            best = (e, tau_s, gg)
    rev_err, tau, g_rev = best
    fit.errors["reverb"] = rev_err
    tail_sum = sum(w for w in (math.exp(-k / (tau / HOP)) for k in range(2, 42)) if w >= 1e-3)
    wet_db = [(10.0 * math.log10(g_rev[m + k] * tail_sum / g_rev[k]) if g_rev[k] > 0 and g_rev[m + k] > 0 else -40.0)
              for k in range(m)]

    # 6. Pan per track: the level model into the mix's left and right.
    pan_db = np.zeros(n)
    if song.mix.right_db is not None:
        XL = [on_grid(t.left()) for t in song.tracks]
        XR = [on_grid(t.right()) for t in song.tracks]
        cl, yl = rows(XL, target=song.mix.left())
        cr, yr = rows(XR, target=song.mix.right())
        gl, gr_ = fit_gains(cl, yl), fit_gains(cr, yr)
        for t in range(n):
            if gl[t] > 0 or gr_[t] > 0:
                pan_db[t] = float(np.clip(10.0 * math.log10(max(gl[t], 1e-4) / max(gr_[t], 1e-4)), -20.0, 20.0))

    # Levels against the mix while each part plays.
    def rel(a_p, b_p, mask):
        if mask.sum() < 10:
            return None
        a = loudness(a_p)[mask]
        b = loudness(b_p)[mask]
        return float(10 * np.log10(np.mean(10 ** (a / 10))) - 10 * np.log10(np.mean(10 ** (b / 10))))

    def playing(p):
        lvl = loudness(p)
        return active & (lvl > np.max(lvl) - 30.0)

    total = sum(Seq)
    fit.mix["role_lu"] = {}
    for k, r in enumerate(stem_names):
        fit.mix["role_lu"][r] = rel(Seq[k], mix_p, playing(Seq[k]))
    if "voice" in stem_names:
        v = Seq[stem_names.index("voice")]
        sings = playing(v)
        fit.mix["voice_over_backing_lu"] = rel(v, total - v, sings)
        # Over the instruments only, as Auto Mix's own target is set (background vocals left out).
        band = total - v - (Seq[stem_names.index("background")] if "background" in stem_names else 0.0)
        fit.mix["voice_over_band_lu"] = rel(v, band, sings)
        if "background" in stem_names:
            b = Seq[stem_names.index("background")]
            both = sings & playing(b)
            r_ = rel(b, v, both)
            fit.mix["background_under_lead_lu"] = None if r_ is None else -r_

    # Per stem.
    fit.stems = []
    for k, r in enumerate(stem_names):
        act = playing(Seq[k])
        raw_tone = db(S[k][act].mean(axis=0)) if act.sum() > 10 else np.full(30, -200.0)
        gr_mean, gr95 = gain_reduction(k)
        members = [t for t in range(n) if roles[t] == r and g[t] > 0]
        weights = [float(np.sum(g[t] * X[t][active])) for t in members]
        fit.stems.append({
            "role": r, "tracks": len(members),
            "active_s": float(act.sum() * HOP),
            "raw_lra": loudness_range(S[k]),
            "eq_db": [None if np.isnan(v_) else round(float(v_), 2) for v_ in eq[k]],
            "raw_tone": [round(float(v_), 2) for v_ in raw_tone],
            "comp_slope": float(slopes[k]), "comp_ratio": float(1.0 / (1.0 - slopes[k])),
            "comp_smooth_s": float(smooths[k] * HOP),
            "comp_gr_mean_db": gr_mean, "comp_gr95_db": gr95,
            "deess_slope": float(deess[k]),
            "wet_db": float(wet_db[k]),
            "pan_abs_db": weighted_median([abs(pan_db[t]) for t in members], weights) if members else 0.0,
            "panned_share": float(sum(w_ for t, w_ in zip(members, weights) if abs(pan_db[t]) > 3.0) / max(1e-9, sum(weights))),
        })
    fit.tracks = [{"name": t.name, "bench_role": t.role, "role": roles[i], "gain_db": float(10 * math.log10(g[i])) if g[i] > 0 else -200.0,
                   "pan_db": float(pan_db[i])} for i, t in enumerate(song.tracks)]

    # The mix as a whole.
    mx = song.mix
    fit.mix["lufs"] = integrated(mix_p)
    fit.mix["lra"] = loudness_range(mix_p)
    pk = mx.peak_db[mx.peak_db > -150]
    rm = mx.rms_db[mx.rms_db > -150]
    fit.mix["peak_db"] = float(pk.max()) if len(pk) else -200.0
    fit.mix["crest_db"] = float(np.percentile(pk, 99) - 10 * np.log10(np.mean(10 ** (rm / 10)))) if len(rm) else 0.0
    tone = db(mix_p[active].mean(axis=0))
    fit.mix["tone"] = [round(float(v_ - np.mean(tone[TONE_BANDS])), 2) for v_ in tone]
    if mx.right_db is not None:
        mid, side = mx.mid()[active], mx.side()[active]
        fit.mix["width_db"] = float(db(side[:, FIT_BANDS].sum()) - db(mid[:, FIT_BANDS].sum()))
        lo = [b for b in FIT_BANDS if CENTRES[b] < 120]
        fit.mix["low_width_db"] = float(db(side[:, lo].sum()) - db(mid[:, lo].sum()))
    fit.mix["reverb_decay_s"] = float(tau * 13.8)
    fit.mix["lag_s"] = lag * HOP
    return fit


# ----------------------------------------------------------------------------------------
def weighted_median(values, weights):
    v = np.asarray(values, float)
    w = np.asarray(weights, float)
    ok = ~np.isnan(v) & (w > 0)
    v, w = v[ok], w[ok]
    if len(v) == 0:
        return None
    order = np.argsort(v)
    v, w = v[order], w[order]
    c = np.cumsum(w)
    return float(v[np.searchsorted(c, c[-1] / 2.0)])


def median_or_none(values):
    vals = [x for x in values if x is not None and not (isinstance(x, float) and np.isnan(x))]
    return None if not vals else round(float(np.median(vals)), 2)


def pool(fits):
    """Role targets from a set of song fits: medians over the songs that have the role."""
    out = {"roles": {}, "mix": {}, "songs": [f.name for f in fits]}
    for role in STEM_ROLES:
        st = [(f, s) for f in fits for s in f.stems if s["role"] == role and s["active_s"] > 5]
        if not st:
            continue
        eq = []
        for gi in range(len(GROUPS)):
            vals = [s["eq_db"][gi] for _, s in st if s["eq_db"][gi] is not None]
            eq.append(None if len(vals) < 2 else round(float(np.median(vals)), 2))
        tones = []
        for _, s in st:
            tone = np.array(s["raw_tone"])
            if np.all(tone <= -150):
                continue
            adj = tone.copy()
            for gi, bands in enumerate(GROUPS):
                if s["eq_db"][gi] is not None:
                    adj[bands] += s["eq_db"][gi]
            adj -= adj[FIT_BANDS].mean()
            tones.append(adj)
        out["roles"][role] = {
            "songs": len(st),
            "level_lu": median_or_none([f.mix["role_lu"].get(role) for f, _ in st]),
            "eq_db": eq,
            "tone": None if not tones else [round(float(v), 2) for v in np.median(np.array(tones), axis=0)],
            "comp_share": round(float(np.mean([s["comp_slope"] > 0 for _, s in st])), 2),
            "comp_ratio": round(float(np.median([s["comp_ratio"] for _, s in st])), 2),
            "comp_ratio_when_used": median_or_none([s["comp_ratio"] for _, s in st if s["comp_slope"] > 0]),
            "comp_gr_mean_db": median_or_none([s["comp_gr_mean_db"] for _, s in st if s["comp_slope"] > 0]),
            "comp_gr95_db": median_or_none([s["comp_gr95_db"] for _, s in st if s["comp_slope"] > 0]),
            "comp_smooth_s": median_or_none([s["comp_smooth_s"] for _, s in st if s["comp_slope"] > 0]),
            "raw_lra": median_or_none([s["raw_lra"] for _, s in st]),
            "deess_share": round(float(np.mean([s["deess_slope"] > 0 for _, s in st])), 2),
            "deess_slope": median_or_none([s["deess_slope"] for _, s in st]),
            "wet_db": median_or_none([s["wet_db"] for _, s in st]),
            "pan_abs_db": median_or_none([s["pan_abs_db"] for _, s in st]),
            "panned_share": median_or_none([s["panned_share"] for _, s in st]),
        }
    for key in ("voice_over_backing_lu", "voice_over_band_lu", "background_under_lead_lu", "lufs", "lra", "crest_db", "width_db", "low_width_db",
                "reverb_decay_s", "peak_db"):
        out["mix"][key] = median_or_none([f.mix.get(key) for f in fits])
    out["mix"]["tone"] = [round(float(v), 2) for v in np.median(np.array([f.mix["tone"] for f in fits]), axis=0)]
    return out


def held_out(fits):
    """Leave one song out: targets pooled from the others against the song's own numbers."""
    rows_ = []
    for i, f in enumerate(fits):
        p = pool([g for j, g in enumerate(fits) if j != i])
        row = {"song": f.name}
        for key in ("voice_over_backing_lu", "voice_over_band_lu", "background_under_lead_lu", "lufs", "width_db"):
            if f.mix.get(key) is not None and p["mix"].get(key) is not None:
                row[key] = [round(f.mix[key], 2), p["mix"][key]]
        errs = []
        for role, actual in f.mix["role_lu"].items():
            target = p["roles"].get(role, {}).get("level_lu")
            if actual is not None and target is not None:
                errs.append(abs(actual - target))
        row["role_level_error_lu"] = None if not errs else round(float(np.mean(errs)), 2)
        eq_err, flat_err = [], []
        for s in f.stems:
            target = p["roles"].get(s["role"], {}).get("eq_db")
            if not target:
                continue
            for gi, v in enumerate(s["eq_db"]):
                if v is not None and target[gi] is not None:
                    eq_err.append(abs(v - target[gi]))
                    flat_err.append(abs(v))
        row["eq_error_db"] = None if not eq_err else round(float(np.mean(eq_err)), 2)
        row["eq_error_flat_db"] = None if not flat_err else round(float(np.mean(flat_err)), 2)
        rows_.append(row)
    return rows_


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("--out", default="learned.json")
    parser.add_argument("--fits", default=None, help="write every song's fit here (json)")
    parser.add_argument("--subsample", type=int, default=3)
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
        changed = [f"{t['name']}: {t['bench_role']} -> {t['role']}" for t in fit.tracks if t["bench_role"] != t["role"]]
        if changed:
            print("  roles: " + "; ".join(changed), flush=True)
        fits.append(fit)

    learned = pool(fits)
    learned["held_out"] = held_out(fits)
    with open(args.out, "w") as f:
        json.dump(learned, f, indent=1)
    if args.fits:
        with open(args.fits, "w") as f:
            json.dump([{"song": x.name, "style": x.style, "errors": x.errors, "mix": x.mix, "stems": x.stems, "tracks": x.tracks} for x in fits], f, indent=1)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
