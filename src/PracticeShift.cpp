#include "PracticeShift.h"

#include "daw/Warp.h"

#include <thread>

struct KeySpeedRender::State
{
    struct Item
    {
        size_t part = 0;
        size_t piece = 0;      // the render's piece (whole: the render isn't in pieces)
        bool whole = false;
        int first = 0, count = 0; // where it lands in the copy
        bool started = false, done = false;
    };

    std::vector<std::unique_ptr<daw::warp::Render>> renders; // one per part
    std::vector<size_t> firstItem;                           // each part's first item
    std::vector<Item> items;
    std::mutex lock;
    std::atomic<int> total { 0 }, done { 0 }, running { 0 };
    std::atomic<bool> ready { false }; // the items are known
};

KeySpeedRender::KeySpeedRender (double rate, double s, double st)
    : sampleRate (rate), speed (s), semitones (st), state (std::make_shared<State>())
{
}

KeySpeedRender::~KeySpeedRender() = default;

int KeySpeedRender::getLength (int numSamples, double speed)
{
    return std::abs (speed - 1.0) < 1.0e-6 ? numSamples : juce::jmax (1, (int) std::round ((double) numSamples / speed));
}

void KeySpeedRender::start (std::vector<Part> newParts, double focusSeconds, int threads)
{
    parts = std::move (newParts);
    setFocus (focusSeconds);
    if (threads <= 0)
        threads = juce::jlimit (1, 8, (int) std::thread::hardware_concurrency() - 1);

    juce::Thread::launch (juce::Thread::Priority::low, [self = shared_from_this(), threads] { self->prepareAndRun (threads); });
}

void KeySpeedRender::setFocus (double copySeconds) noexcept
{
    focus = (juce::int64) std::round (juce::jmax (0.0, copySeconds) * sampleRate);
}

double KeySpeedRender::getProgress() const noexcept
{
    if (finished)
        return 1.0;
    const auto total = state->total.load();
    return total > 0 ? (double) state->done.load() / total : 0.0;
}

bool KeySpeedRender::isReady (double fromSeconds, double toSeconds) const
{
    for (const auto& part : parts)
        if (part.fill != nullptr && ! part.fill->isReady ((juce::int64) std::floor (fromSeconds * sampleRate), (juce::int64) std::ceil (toSeconds * sampleRate)))
            return false;
    return true;
}

void KeySpeedRender::prepareAndRun (int threads)
{
    auto& s = *state;

    // The pieces of every part (finding a stem's seams takes a moment, so it's done here).
    daw::warp::Settings settings[2];
    for (int voice = 0; voice < 2; ++voice)
    {
        auto& set = settings[voice];
        set.semitones = semitones;
        set.algorithm = voice ? daw::warp::Algorithm::solo : daw::warp::Algorithm::mix;
        set.formant = voice != 0;
    }

    for (size_t p = 0; p < parts.size() && ! cancelled; ++p)
    {
        const auto& part = parts[p];
        const std::shared_ptr<const juce::AudioBuffer<float>> audio (part.source, &part.source->audio);
        auto set = settings[part.voice ? 1 : 0];
        const auto seconds = audio->getNumSamples() / sampleRate;
        if (std::abs (speed - 1.0) >= 1.0e-6)
            set.map = daw::warp::Map (std::vector<daw::warp::Marker> { { 0.0, 0.0 }, { seconds, seconds / speed } });

        auto render = std::make_unique<daw::warp::Render> (audio, sampleRate, set);
        s.firstItem.push_back (s.items.size());
        if (render->isSegmented())
        {
            for (size_t k = 0; k < render->getPieces().size(); ++k)
            {
                State::Item item;
                item.part = p;
                item.piece = k;
                std::tie (item.first, item.count) = render->getPieceSpan (k);
                s.items.push_back (item);
            }
        }
        else
        {
            State::Item item;
            item.part = p;
            item.whole = true;
            item.count = render->getLength();
            s.items.push_back (item);
        }
        s.renders.push_back (std::move (render));
    }

    if (cancelled)
    {
        finished = true;
        return;
    }

    s.total = (int) s.items.size();
    s.ready = true;

    s.running = threads;
    for (int t = 1; t < threads; ++t)
        juce::Thread::launch (juce::Thread::Priority::low, [self = shared_from_this()] { self->work(); });
    work();
}

bool KeySpeedRender::takeNext (size_t& next)
{
    auto& s = *state;
    const std::lock_guard<std::mutex> guard (s.lock);

    // The first piece not started that plays at or after the focus; else the earliest left.
    const auto at = focus.load();
    auto best = s.items.size(), fallback = s.items.size();
    for (size_t i = 0; i < s.items.size(); ++i)
    {
        const auto& item = s.items[i];
        if (item.started)
            continue;
        if (item.first + item.count > at)
        {
            if (best == s.items.size() || item.first < s.items[best].first)
                best = i;
        }
        else if (fallback == s.items.size() || item.first < s.items[fallback].first)
        {
            fallback = i;
        }
    }

    next = best != s.items.size() ? best : fallback;
    if (next == s.items.size())
        return false;
    s.items[next].started = true;
    return true;
}

void KeySpeedRender::work()
{
    auto& s = *state;
    const auto keepGoing = [this] { return ! cancelled.load(); };

    size_t item = 0;
    while (! cancelled && takeNext (item))
    {
        const auto& it = s.items[item];
        const auto& render = *s.renders[it.part];
        auto audio = it.whole ? render.renderWhole (keepGoing) : render.renderPiece (it.piece, keepGoing);
        if (cancelled || audio.getNumSamples() == 0)
            break;
        finishItem (item, audio);
    }

    if (--s.running == 0)
        finished = true;
}

void KeySpeedRender::finishItem (size_t index, const juce::AudioBuffer<float>& audio)
{
    auto& s = *state;
    const std::lock_guard<std::mutex> guard (s.lock);

    auto& item = s.items[index];
    const auto& part = parts[item.part];
    auto& fill = *part.fill;
    const auto& render = *s.renders[item.part];
    const auto channels = (int) fill.channels.size();

    if (item.whole)
    {
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::copy (fill.channels[(size_t) ch], audio.getReadPointer (juce::jmin (ch, audio.getNumChannels() - 1)),
                                               juce::jmin (fill.numSamples, audio.getNumSamples()));
        item.done = true;
        ++s.done;
        fill.markReady (0, fill.numSamples);
        return;
    }

    render.addPiece (item.piece, audio, fill.channels.data(), channels);
    item.done = true;
    ++s.done;

    // The blocks this piece touched are ready once every piece over them is done (its
    // neighbours share the crossfades at the seams).
    const auto begin = s.firstItem[item.part];
    const auto end = item.part + 1 < s.firstItem.size() ? s.firstItem[item.part + 1] : s.items.size();
    const auto from = juce::jmax (0, item.first), to = juce::jmin (fill.numSamples, item.first + item.count);
    for (auto b = from / Multitrack::Fill::blockSize; b * Multitrack::Fill::blockSize < to; ++b)
    {
        const auto blockStart = b * Multitrack::Fill::blockSize;
        const auto blockEnd = juce::jmin (fill.numSamples, blockStart + Multitrack::Fill::blockSize);
        auto covered = true;
        for (auto j = (index >= begin + 2 ? index - 2 : begin); j < end && j <= index + 2; ++j)
        {
            const auto& other = s.items[j];
            if (other.first < blockEnd && other.first + other.count > blockStart && ! other.done)
                covered = false;
        }
        if (covered)
            fill.markBlock (b);
    }
}
