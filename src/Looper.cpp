#include "Looper.h"

juce::String Looper::getStateName (State s)
{
    switch (s)
    {
        case State::empty:       return "empty";
        case State::recording:   return "recording";
        case State::closing:     return "closing";
        case State::playing:     return "playing";
        case State::overdubbing: return "overdubbing";
        case State::stopped:     return "stopped";
    }

    return "empty";
}

void Looper::prepare (double newRate)
{
    const auto rateChanged = std::abs (newRate - sampleRate) > 0.5;
    sampleRate = newRate;
    capacity = (juce::int64) (maxSeconds * sampleRate);
    fadeLength = juce::jmax (1, (int) (fadeSeconds * sampleRate));
    rampStep = 1.0f / (float) fadeLength;

    if (rateChanged)
    {
        // A loop recorded at another rate would play at the wrong pitch: start over.
        clearLoop();
        if (allocated.load())
        {
            allocated = false;
            for (auto& layer : layers)
                layer.audio.setSize (0, 0);
        }
    }

    publish();
}

bool Looper::send (Command command)
{
    // The first recording needs the layer memory. Until a loop has been recorded once,
    // the audio thread never touches the layers, so they can be allocated here.
    if (command == Command::main && ! allocated.load() && state.load() == State::empty)
    {
        for (auto& layer : layers)
        {
            layer.audio.setSize (2, (int) capacity);
            layer.audio.clear();
        }
        allocated = true;
    }

    const juce::SpinLock::ScopedLockType lock (sendLock);
    const auto scope = fifo.write (1);
    if (scope.blockSize1 + scope.blockSize2 == 0)
        return false;

    commands[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = command;
    return true;
}

void Looper::setLayerGain (int layer, float gain) noexcept
{
    if (juce::isPositiveAndBelow (layer, maxLayers))
        gains[(size_t) layer] = juce::jlimit (0.0f, 4.0f, gain);
}

void Looper::setLayerMuted (int layer, bool muted) noexcept
{
    if (juce::isPositiveAndBelow (layer, maxLayers))
        mutes[(size_t) layer] = muted;
}

void Looper::setTempo (double newBpm, int newBeatsPerBar) noexcept
{
    bpm = juce::jlimit (30.0, 300.0, newBpm);
    beatsPerBar = juce::jlimit (1, 16, newBeatsPerBar);
}

float Looper::getLayerGain (int layer) const noexcept
{
    return juce::isPositiveAndBelow (layer, maxLayers) ? gains[(size_t) layer].load() : 0.0f;
}

bool Looper::isLayerMuted (int layer) const noexcept
{
    return juce::isPositiveAndBelow (layer, maxLayers) && mutes[(size_t) layer].load();
}

juce::int64 Looper::getBarLength() const noexcept
{
    return juce::jmax ((juce::int64) 1, (juce::int64) std::llround (sampleRate * 60.0 / bpm.load() * beatsPerBar.load()));
}

//==============================================================================
void Looper::process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output,
                      int numChannels, int numSamples) noexcept
{
    const auto scope = fifo.read (fifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) handle (commands[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i) handle (commands[(size_t) (scope.startIndex2 + i)]);

    if (! allocated.load() || current == State::empty || current == State::stopped || numChannels < 1)
    {
        publish();
        return;
    }

    const auto* inLeft  = input.getReadPointer (0);
    const auto* inRight = numChannels > 1 && input.getNumChannels() > 1 ? input.getReadPointer (1) : nullptr;
    auto* left  = output.getWritePointer (0);
    auto* right = numChannels > 1 ? output.getWritePointer (1) : nullptr;
    const auto gain = volume.load();
    const auto smoothing = 1.0f - std::exp (-1.0f / (0.01f * (float) sampleRate)); // ~10 ms

    std::array<float, maxLayers> targets {};
    for (size_t k = 0; k < (size_t) maxLayers; ++k)
        targets[k] = mutes[k].load() ? 0.0f : gains[k].load();

    for (int i = 0; i < numSamples; ++i)
    {
        const auto inL = inLeft[i];
        const auto inR = inRight != nullptr ? inRight[i] : inL;

        if (current == State::recording || current == State::closing)
        {
            auto& first = layers[0];
            first.audio.setSample (0, (int) recorded, inL);
            first.audio.setSample (1, (int) recorded, inR);
            ++recorded;

            if (current == State::closing && recorded >= target)
                finishFirstLayer (target, 0);
            else if (recorded >= capacity)
                requestClose (true); // the longest loop there is: close it here

            continue;
        }

        if (current != State::playing && current != State::overdubbing)
            break; // stopped or cleared mid-block

        float outL = 0.0f, outR = 0.0f;

        for (int k = 0; k < layerCount; ++k)
        {
            auto& layer = layers[(size_t) k];
            layer.smoothedGain += (targets[(size_t) k] - layer.smoothedGain) * smoothing;

            if (isCovered (layer, playhead))
            {
                outL += layer.audio.getSample (0, (int) playhead) * layer.smoothedGain;
                outR += layer.audio.getSample (1, (int) playhead) * layer.smoothedGain;
            }
        }

        if (current == State::overdubbing)
        {
            auto& top = layers[(size_t) (layerCount - 1)];

            if (recordGain < recordGainTarget)      recordGain = juce::jmin (recordGainTarget, recordGain + rampStep);
            else if (recordGain > recordGainTarget) recordGain = juce::jmax (recordGainTarget, recordGain - rampStep);

            auto* topL = top.audio.getWritePointer (0);
            auto* topR = top.audio.getWritePointer (1);

            if (top.coverage < loopLength)
            {
                // The first pass writes over whatever was left in the memory.
                topL[playhead] = inL * recordGain;
                topR[playhead] = inR * recordGain;
                top.coverage = top.coverage + 1;
            }
            else
            {
                topL[playhead] += inL * recordGain;
                topR[playhead] += inR * recordGain;
            }

            if (recordGainTarget <= 0.0f && recordGain <= 0.0f)
                current = State::playing; // faded out: the layer is done
        }

        left[i] += outL * gain;
        if (right != nullptr)
            right[i] += outR * gain;

        if (++playhead >= loopLength)
            playhead = 0;
    }

    publish();
}

bool Looper::isCovered (const Layer& layer, juce::int64 pos) const noexcept
{
    const auto coverage = layer.coverage.load();
    if (coverage >= loopLength)
        return true;

    auto offset = pos - layer.start.load();
    if (offset < 0)
        offset += loopLength;
    return offset < coverage;
}

void Looper::handle (Command command) noexcept
{
    switch (command)
    {
        case Command::main:
            switch (current)
            {
                case State::empty:       startFirstLayer(); break;
                case State::recording:   requestClose (false); break;
                case State::closing:     break; // already closing at the bar line
                case State::playing:     startOverdub(); break;
                case State::overdubbing: recordGainTarget = 0.0f; break; // fades out, then plays
                case State::stopped:     playhead = 0; current = State::playing; break;
            }
            break;

        case Command::stop:
            if (current == State::recording || current == State::closing)
                requestClose (true);
            else if (current == State::overdubbing)
                endOverdubNow();

            if (current != State::empty)
            {
                current = State::stopped;
                playhead = 0;
            }
            break;

        case Command::play:
            if (current == State::stopped || current == State::playing)
            {
                playhead = 0;
                current = State::playing;
            }
            break;

        case Command::undo:
            if (current == State::recording || current == State::closing)
                clearLoop();
            else if (current == State::overdubbing)
            {
                --layerCount; // the layer being recorded is dropped, not kept for redo
                current = State::playing;
            }
            else if ((current == State::playing || current == State::stopped) && layerCount > 1)
            {
                --layerCount;
                ++redoCount;
            }
            break;

        case Command::redo:
            if ((current == State::playing || current == State::stopped) && redoCount > 0)
            {
                ++layerCount;
                --redoCount;
            }
            break;

        case Command::clear:
            clearLoop();
            break;
    }
}

void Looper::startFirstLayer() noexcept
{
    if (! allocated.load() || capacity <= 0)
        return;

    recorded = 0;
    loopLength = 0;
    layerCount = 1;
    redoCount = 0;
    layers[0].start = 0;
    layers[0].coverage = 0;
    current = State::recording;
}

void Looper::requestClose (bool padInsteadOfWaiting) noexcept
{
    if (recorded < (juce::int64) (minSeconds * sampleRate))
    {
        clearLoop(); // a slip of the foot, not a loop
        return;
    }

    if (! tempoLocked.load())
    {
        finishFirstLayer (recorded, 0);
        return;
    }

    // Whole bars, the nearest to what was recorded.
    const auto bar = getBarLength();
    auto bars = juce::jmax ((juce::int64) 1, (juce::int64) std::llround ((double) recorded / (double) bar));
    while (bars > 1 && bars * bar > capacity)
        --bars;
    const auto wanted = juce::jmin (capacity, bars * bar);

    if (wanted <= recorded)
    {
        // Pressed late: the loop already started over, so play on from where it is now.
        finishFirstLayer (wanted, (recorded - wanted) % wanted);
    }
    else if (padInsteadOfWaiting)
    {
        for (int ch = 0; ch < 2; ++ch)
            layers[0].audio.clear (ch, (int) recorded, (int) (wanted - recorded));
        finishFirstLayer (wanted, 0);
    }
    else
    {
        // Pressed early: keep recording to the bar line.
        target = wanted;
        current = State::closing;
    }
}

void Looper::finishFirstLayer (juce::int64 newLength, juce::int64 playFrom) noexcept
{
    loopLength = newLength;

    // Short fades where the loop wraps, so the seam doesn't click.
    auto& first = layers[0];
    const auto fade = (int) juce::jmin ((juce::int64) fadeLength, newLength / 4);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* data = first.audio.getWritePointer (ch);
        for (int i = 0; i < fade; ++i)
        {
            const auto ramp = (float) i / (float) fade;
            data[i] *= ramp;
            data[newLength - 1 - i] *= ramp;
        }
    }

    first.start = 0;
    first.coverage = newLength;
    first.smoothedGain = mutes[0].load() ? 0.0f : gains[0].load();
    layerCount = 1;
    redoCount = 0;
    playhead = playFrom;
    current = State::playing;
}

void Looper::startOverdub() noexcept
{
    if (layerCount >= maxLayers)
        return; // all four layers are used: undo one, or clear

    auto& top = layers[(size_t) layerCount];
    top.start = playhead;
    top.coverage = 0;
    top.smoothedGain = mutes[(size_t) layerCount].load() ? 0.0f : gains[(size_t) layerCount].load();
    ++layerCount;
    redoCount = 0;
    recordGain = 0.0f;
    recordGainTarget = 1.0f;
    current = State::overdubbing;
}

void Looper::endOverdubNow() noexcept
{
    // Stopped mid-pass: fade out the end of what was written so it doesn't click.
    auto& top = layers[(size_t) (layerCount - 1)];
    const auto coverage = top.coverage.load();

    if (coverage < loopLength)
    {
        const auto fade = (int) juce::jmin ((juce::int64) fadeLength, coverage);
        for (int i = 0; i < fade; ++i)
        {
            auto pos = top.start.load() + coverage - 1 - i;
            if (pos >= loopLength)
                pos -= loopLength;
            const auto ramp = (float) i / (float) fade;
            for (int ch = 0; ch < 2; ++ch)
                top.audio.setSample (ch, (int) pos, top.audio.getSample (ch, (int) pos) * ramp);
        }
    }

    recordGain = recordGainTarget = 0.0f;
    current = State::playing;
}

void Looper::clearLoop() noexcept
{
    current = State::empty;
    layerCount = redoCount = 0;
    loopLength = playhead = recorded = target = 0;
    recordGain = recordGainTarget = 0.0f;
}

void Looper::publish() noexcept
{
    state = current;
    numLayers = layerCount;
    numRedo = redoCount;
    length = loopLength;
    position = (current == State::recording || current == State::closing) ? recorded : playhead;
    closingTarget = current == State::closing ? target : 0;
}

//==============================================================================
juce::Array<float> Looper::getOverview (int layer, int bins) const
{
    juce::Array<float> result;
    const auto s = state.load();
    const auto recordingFirst = s == State::recording || s == State::closing;
    const auto total = recordingFirst ? position.load() : length.load();

    if (! allocated.load() || bins <= 0 || total <= 0 || ! juce::isPositiveAndBelow (layer, numLayers.load()))
        return result;

    const auto& source = layers[(size_t) layer];
    const auto coverage = recordingFirst ? total : source.coverage.load();
    const auto start = source.start.load();
    result.resize (bins);

    for (int b = 0; b < bins; ++b)
    {
        const auto from = total * b / bins;
        const auto to = juce::jmax (from + 1, total * (b + 1) / bins);
        const auto step = juce::jmax ((juce::int64) 1, (to - from) / 128);
        float peak = 0.0f;

        for (auto pos = from; pos < to; pos += step)
        {
            auto offset = pos - start;
            if (offset < 0)
                offset += total;
            if (offset >= coverage)
                continue;

            peak = juce::jmax (peak, std::abs (source.audio.getSample (0, (int) pos)), std::abs (source.audio.getSample (1, (int) pos)));
        }

        result.set (b, juce::jmin (1.0f, peak));
    }

    return result;
}

bool Looper::copyLayer (int layer, juce::AudioBuffer<float>& destination) const
{
    const auto s = state.load();
    const auto total = length.load();

    if (! allocated.load() || total <= 0 || s == State::recording || s == State::closing
        || ! juce::isPositiveAndBelow (layer, numLayers.load()))
        return false;

    const auto& source = layers[(size_t) layer];
    const auto coverage = source.coverage.load();
    const auto start = source.start.load();
    destination.setSize (2, (int) total);

    for (juce::int64 pos = 0; pos < total; ++pos)
    {
        auto offset = pos - start;
        if (offset < 0)
            offset += total;
        const auto covered = offset < coverage;

        for (int ch = 0; ch < 2; ++ch)
            destination.setSample (ch, (int) pos, covered ? source.audio.getSample (ch, (int) pos) : 0.0f);
    }

    return true;
}
