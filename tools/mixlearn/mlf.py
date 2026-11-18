"""Reads the .mlf measurement files written by MicFXMixBench --learn (see src/analysis/MixLearn.h)."""
import json
import struct
import zlib

import numpy as np

NUM_BANDS = 30
SILENCE_DB = -200.0


class Signal:
    """One file of a song: band power per hop, per channel."""

    def __init__(self, info, left, right, corr, peak, rms):
        self.name = info["name"]
        self.kind = info["kind"]
        self.role = info.get("role") or ""
        self.sample_rate = info["sampleRate"]
        self.channels = info["channels"]
        self.left_db = left            # [hops, bands]
        self.right_db = right          # None for mono
        self.corr = corr               # None for mono
        self.peak_db = peak            # [hops]
        self.rms_db = rms              # [hops]

    @property
    def hops(self):
        return self.left_db.shape[0]

    def power(self, db):
        return np.where(db <= -199.0, 0.0, 10.0 ** (db / 10.0))

    def left(self):
        return self.power(self.left_db)

    def right(self):
        return self.left() if self.right_db is None else self.power(self.right_db)

    def mid(self):
        """Mean square of (L+R)/2 per band."""
        if self.right_db is None:
            return self.left()
        pl, pr = self.left(), self.right()
        c = self.corr * np.sqrt(pl * pr)
        return np.maximum(0.0, (pl + pr + 2.0 * c) / 4.0)

    def side(self):
        if self.right_db is None:
            return np.zeros_like(self.left_db)
        pl, pr = self.left(), self.right()
        c = self.corr * np.sqrt(pl * pr)
        return np.maximum(0.0, (pl + pr - 2.0 * c) / 4.0)

    def mono_power(self):
        """Average of the channels' powers: the level a track brings whatever its pan."""
        return 0.5 * (self.left() + self.right())


class Song:
    def __init__(self, header, signals):
        self.name = header["song"]
        self.style = header.get("style") or ""
        self.tempo = header.get("tempo", 120.0)
        self.hop = header.get("hop", 0.1)
        self.band_centres = np.array(header["bands"], dtype=float)
        self.tracks = [s for s in signals if s.kind == "track"]
        mixes = [s for s in signals if s.kind == "mix"]
        self.mix = mixes[0] if mixes else None


def load(path):
    raw = open(path, "rb").read()
    try:
        data = zlib.decompress(raw)
    except zlib.error:
        data = zlib.decompress(raw, 31)  # gzip
    if data[:4] != b"MLF1":
        raise ValueError(f"{path}: not an MLF1 file")
    (length,) = struct.unpack_from("<i", data, 4)
    header = json.loads(data[8:8 + length].decode("utf-8"))
    pos = 8 + length
    signals = []

    def take(count, scale):
        nonlocal pos
        values = np.frombuffer(data, dtype="<i2", count=count, offset=pos).astype(np.float64) / scale
        pos += 2 * count
        return values

    for info in header["files"]:
        hops = info["hops"]
        left = take(hops * NUM_BANDS, 100.0).reshape(hops, NUM_BANDS)
        right = corr = None
        if info["channels"] >= 2:
            right = take(hops * NUM_BANDS, 100.0).reshape(hops, NUM_BANDS)
            corr = take(hops * NUM_BANDS, 10000.0).reshape(hops, NUM_BANDS)
        peak = take(hops, 100.0)
        rms = take(hops, 100.0)
        signals.append(Signal(info, left, right, corr, peak, rms))
    return Song(header, signals)
