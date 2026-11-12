#!/usr/bin/env python3
"""Converts the Demucs (HTDemucs) network to Core ML, so MicFX can split stems on the GPU.

Run it once, on the Mac you use MicFX on:

    python3 -m venv ~/micfx-export && source ~/micfx-export/bin/activate
    pip install torch coremltools einops numpy "demucs==4.0.1"
    python3 scripts/export_htdemucs_coreml.py

It downloads Meta's pretrained htdemucs weights (MIT licence) through the demucs package,
converts the network, and writes

    ~/Library/Application Support/MicFX/Models/htdemucs-core.mlpackage

MicFX looks for it there: the next "Split into stems" runs on the GPU, and falls back to
the CPU if the model is missing or doesn't work. It then checks the converted model against
the original on random input and prints how close they are.

What is converted is only the network core, the part of HTDemucs.forward between "normalise"
and "undo the normalising". The spectrogram, the mask, the inverse spectrogram and the
segment crossfades stay in MicFX (src/stems/DemucsPipeline.cpp), because Core ML has no good
way to do them and they're cheap. The network runs at the one size it was trained on, 7.8 s.
"""
import argparse
import contextlib
import sys
from pathlib import Path

DEFAULT_OUT = Path.home() / "Library" / "Application Support" / "MicFX" / "Models" / "htdemucs-core.mlpackage"

# What src/stems/DemucsPipeline.h expects.
SEGMENT_SAMPLES = 343980  # 7.8 s at 44.1 kHz
FREQ_BINS, FRAMES, FREQ_CHANNELS, TIME_CHANNELS = 2048, 336, 4, 2
SOURCES = ["drums", "bass", "other", "vocals"]


def load_model(name):
    from demucs.pretrained import get_model

    loaded = get_model(name)
    model = loaded.models[0] if hasattr(loaded, "models") else loaded  # a "bag" of one model
    model.eval()
    return model


def check_model(model):
    samples = int(model.segment * model.samplerate)
    problems = []
    if list(model.sources) != SOURCES:
        problems.append(f"sources are {list(model.sources)}, expected {SOURCES}")
    if samples != SEGMENT_SAMPLES:
        problems.append(f"the segment is {samples} samples, expected {SEGMENT_SAMPLES}")
    if not getattr(model, "cac", False):
        problems.append("the model doesn't use complex-as-channels (cac)")
    if problems:
        sys.exit("This isn't the model MicFX expects: " + "; ".join(problems))


def make_core(model):
    """The part of HTDemucs.forward (demucs 4.0.1) between normalising and un-normalising."""
    import torch
    from einops import rearrange

    class Core(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, x, xt):
            m = self.m
            saved, saved_t, lengths, lengths_t = [], [], [], []

            for idx, encode in enumerate(m.encoder):
                lengths.append(x.shape[-1])
                inject = None
                if idx < len(m.tencoder):
                    lengths_t.append(xt.shape[-1])
                    tenc = m.tencoder[idx]
                    xt = tenc(xt)
                    if not tenc.empty:
                        saved_t.append(xt)
                    else:
                        inject = xt
                x = encode(x, inject)
                if idx == 0 and m.freq_emb is not None:
                    frs = torch.arange(x.shape[-2], device=x.device)
                    emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                    x = x + m.freq_emb_scale * emb
                saved.append(x)

            if m.crosstransformer:
                if m.bottom_channels:
                    b, c, f, t = x.shape
                    x = rearrange(x, "b c f t-> b c (f t)")
                    x = m.channel_upsampler(x)
                    x = rearrange(x, "b c (f t)-> b c f t", f=f)
                    xt = m.channel_upsampler_t(xt)

                x, xt = m.crosstransformer(x, xt)

                if m.bottom_channels:
                    x = rearrange(x, "b c f t-> b c (f t)")
                    x = m.channel_downsampler(x)
                    x = rearrange(x, "b c (f t)-> b c f t", f=f)
                    xt = m.channel_downsampler_t(xt)

            for idx, decode in enumerate(m.decoder):
                skip = saved.pop(-1)
                x, pre = decode(x, skip, lengths.pop(-1))
                offset = m.depth - len(m.tdecoder)
                if idx >= offset:
                    tdec = m.tdecoder[idx - offset]
                    length_t = lengths_t.pop(-1)
                    if tdec.empty:
                        pre = pre[:, :, 0]
                        xt, _ = tdec(pre, None, length_t)
                    else:
                        skip = saved_t.pop(-1)
                        xt, _ = tdec(xt, skip, length_t)

            return x, xt  # [1, 16, 2048, 336] and [1, 8, 343980]

    return Core(model).eval()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"where to write the model (default: {DEFAULT_OUT})")
    parser.add_argument("--model", default="htdemucs", help="the demucs model name (default: htdemucs)")
    parser.add_argument("--fp32", action="store_true", help="keep 32-bit precision (about twice the size and slower; for checking)")
    parser.add_argument("--no-check", action="store_true", help="skip comparing the converted model with the original")
    args = parser.parse_args()

    import numpy as np
    import torch
    import coremltools as ct

    print("Loading the pretrained model (downloads about 80 MB the first time)…")
    model = load_model(args.model)
    check_model(model)
    core = make_core(model)

    x = torch.randn(1, FREQ_CHANNELS, FREQ_BINS, FRAMES)
    xt = torch.randn(1, TIME_CHANNELS, SEGMENT_SAMPLES)

    # Core ML can't convert PyTorch's fused "fast path" attention, so keep it to the plain ops.
    mha = getattr(torch.backends, "mha", None)
    if mha is not None:
        mha.set_fastpath_enabled(False)
        grad = torch.no_grad()
    else:
        grad = contextlib.nullcontext()  # older torch: training-mode parameters switch the fast path off

    print("Tracing the network…")
    with grad:
        reference = core(x, xt)
        traced = torch.jit.trace(core, (x, xt), check_trace=False)

    print("Converting to Core ML (a few minutes)…")
    precision = ct.precision.FLOAT32 if args.fp32 else ct.precision.FLOAT16
    mlmodel = ct.convert(
        traced,
        inputs=[
            ct.TensorType(name="x", shape=tuple(x.shape), dtype=np.float32),
            ct.TensorType(name="xt", shape=tuple(xt.shape), dtype=np.float32),
        ],
        outputs=[ct.TensorType(name="x_out", dtype=np.float32), ct.TensorType(name="xt_out", dtype=np.float32)],
        convert_to="mlprogram",
        compute_precision=precision,
        minimum_deployment_target=ct.target.macOS13,
    )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.out.exists():
        import shutil

        shutil.rmtree(args.out)
        shutil.rmtree(args.out.with_suffix(".mlmodelc"), ignore_errors=True)  # MicFX's compiled copy
    mlmodel.save(str(args.out))
    print(f"Saved {args.out}")

    if not args.no_check:
        if sys.platform != "darwin":
            print("Skipping the check: Core ML can only run on a Mac.")
        else:
            print("Checking against the original (the first run builds the GPU version, which takes a while)…")
            loaded = ct.models.MLModel(str(args.out), compute_units=ct.ComputeUnit.CPU_AND_GPU)
            out = loaded.predict({"x": x.numpy(), "xt": xt.numpy()})
            for name, expected in (("x_out", reference[0]), ("xt_out", reference[1])):
                got = np.asarray(out[name]).reshape(expected.shape)
                want = expected.numpy()
                error = np.linalg.norm(got - want) / max(np.linalg.norm(want), 1e-12)
                snr = -20 * np.log10(max(error, 1e-12))
                verdict = "ok" if snr > 30 else "LOW: stems may sound worse, try --fp32"
                print(f"  {name}: {snr:.1f} dB match with the original ({verdict})")

    print("Done. The next 'Split into stems' in MicFX runs on the GPU.")


if __name__ == "__main__":
    main()
