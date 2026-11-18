// Sampler: one sample over the keys, or sliced onto keys (see Sampler.h).
#include "Sampler.h"

#include "InstrumentEditor.h"
#include "Registry.h"

#include <algorithm>

namespace micfx::inst
{
namespace
{
    constexpr int numVoices = 32;

    std::shared_ptr<SamplerSample> readRegion (const juce::File& file, double regionStart, double regionEnd, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "Can't find " + file.getFileName() + ".";
            return {};
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        {
            error = "Can't read " + file.getFileName() + " as audio.";
            return {};
        }

        const auto rate = reader->sampleRate;
        const auto total = (juce::int64) reader->lengthInSamples;
        auto first = juce::jlimit<juce::int64> (0, total - 1, (juce::int64) std::llround (juce::jmax (0.0, regionStart) * rate));
        auto last = regionEnd > regionStart ? juce::jlimit<juce::int64> (first + 1, total, (juce::int64) std::llround (regionEnd * rate)) : total;
        last = juce::jmin (last, first + (juce::int64) (SamplerInstrument::maxSampleSeconds * rate));
        const auto length = (int) (last - first);
        if (length < 2)
        {
            error = "That part of " + file.getFileName() + " is empty.";
            return {};
        }

        const auto channels = (int) juce::jmin (2u, reader->numChannels);
        juce::AudioBuffer<float> buffer (channels, length);
        if (! reader->read (&buffer, 0, length, first, true, channels > 1))
        {
            error = "Couldn't decode " + file.getFileName() + ".";
            return {};
        }

        auto sample = std::make_shared<SamplerSample>();
        sample->sampleRate = rate;
        sample->name = file.getFileNameWithoutExtension();
        sample->file = file;
        sample->regionStart = (double) first / rate;
        sample->regionEnd = (double) last / rate;
        sample->left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
        if (channels > 1)
            sample->right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + length);

        constexpr int buckets = 600;
        sample->peaks.assign (buckets, 0.0f);
        auto top = 0.0f;
        for (int i = 0; i < length; ++i)
        {
            auto v = std::abs (sample->left[(size_t) i]);
            if (channels > 1)
                v = juce::jmax (v, std::abs (sample->right[(size_t) i]));
            auto& peak = sample->peaks[(size_t) ((int64_t) i * buckets / length)];
            peak = juce::jmax (peak, v);
            top = juce::jmax (top, v);
        }
        if (top > 0.0f)
            for (auto& p : sample->peaks)
                p /= top;
        return sample;
    }

    //==========================================================================
    class SamplerEngine final : public Engine, public SamplerInstrument
    {
    public:
        explicit SamplerEngine (Params p)
        {
            mode = p ("mode"); root = p ("root"); tune = p ("tune");
            attack = p ("attack"); decay = p ("decay"); sustain = p ("sustain"); release = p ("release");
            cutoff = p ("cutoff"); loop = p ("loop"); loopStart = p ("loopStart"); loopEnd = p ("loopEnd");
            oneShot = p ("oneShot"); velocity = p ("velocity");
        }

        void prepare (double rate, int) override { sampleRate = rate; reset(); }

        void reset() override
        {
            for (auto& v : voices)
                v.active = false;
        }

        double getTailSeconds() const override { return juce::jmin (release->load() + 0.5, 10.0); }

        void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) override
        {
            const auto numSamples = buffer.getNumSamples();
            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (buffer.getNumChannels() > 1 ? 1 : 0);

            int position = 0;
            for (const auto metadata : midi)
            {
                const auto at = juce::jlimit (0, numSamples, metadata.samplePosition);
                if (at > position)
                {
                    renderVoices (left + position, right + position, at - position);
                    position = at;
                }
                handle (metadata.getMessage());
            }
            if (position < numSamples)
                renderVoices (left + position, right + position, numSamples - position);

            if (right == left)
                juce::FloatVectorOperations::multiply (left, 0.5f, numSamples);
        }

        //======================================================================
        bool load (const juce::File& file, double regionStart, double regionEnd, juce::String& error) override
        {
            auto sample = readRegion (file, regionStart, regionEnd, error);
            if (sample == nullptr)
                return false;
            install (std::move (sample), file, regionStart, regionEnd);
            setSlices ({});
            return true;
        }

        void clear() override
        {
            install (nullptr, {}, 0.0, 0.0);
            setSlices ({});
        }

        std::shared_ptr<const SamplerSample> getSample() const override
        {
            const juce::SpinLock::ScopedLockType lock (dataLock);
            return sample;
        }

        void setSlices (std::vector<double> starts) override
        {
            std::sort (starts.begin(), starts.end());
            if (starts.size() > (size_t) maxSlices)
                starts.resize ((size_t) maxSlices);
            auto list = std::make_shared<const std::vector<double>> (std::move (starts));
            {
                const juce::SpinLock::ScopedLockType lock (dataLock);
                slices.swap (list);
            }
            retire (std::move (list));
        }

        std::vector<double> getSlices() const override
        {
            const juce::SpinLock::ScopedLockType lock (dataLock);
            return slices != nullptr ? *slices : std::vector<double>();
        }

        //======================================================================
        void saveExtraState (juce::XmlElement& xml) const override
        {
            {
                const juce::ScopedLock lock (infoLock);
                if (file != juce::File())
                {
                    auto* s = xml.createNewChildElement ("Sample");
                    s->setAttribute ("path", file.getFullPathName());
                    s->setAttribute ("start", regionStartWanted);
                    s->setAttribute ("end", regionEndWanted);
                }
            }
            juce::StringArray list;
            for (const auto at : getSlices())
                list.add (juce::String (at, 6));
            xml.setAttribute ("slices", list.joinIntoString (","));
        }

        void loadExtraState (const juce::XmlElement& xml) override
        {
            if (const auto* s = xml.getChildByName ("Sample"))
            {
                const juce::File wanted (s->getStringAttribute ("path"));
                const auto start = s->getDoubleAttribute ("start"), end = s->getDoubleAttribute ("end");
                const auto current = getSample();
                if (wanted != juce::File() && (current == nullptr || current->file != wanted))
                {
                    juce::String error;
                    if (auto loaded = readRegion (wanted, start, end, error))
                        install (std::move (loaded), wanted, start, end);
                    else
                        install (nullptr, wanted, start, end); // keep the path: missing, not forgotten
                }
            }
            else if (getSample() != nullptr || file != juce::File())
                install (nullptr, {}, 0.0, 0.0);

            std::vector<double> list;
            for (const auto& t : juce::StringArray::fromTokens (xml.getStringAttribute ("slices"), ",", {}))
                if (t.isNotEmpty())
                    list.push_back (t.getDoubleValue());
            setSlices (std::move (list));
        }

        juce::String getKeyName (int note) const override
        {
            if (mode->load() < 0.5f)
                return {};
            const auto n = sliceForNote (note);
            return n >= 0 && n < (int) getSlices().size() ? "Slice " + juce::String (n + 1) : juce::String();
        }

    private:
        struct Voice
        {
            std::shared_ptr<const SamplerSample> sample;
            Envelope env;
            Svf filterLeft, filterRight;
            double position = 0.0, step = 1.0, endAt = 0.0, loopFrom = 0.0, loopTo = 0.0;
            float gain = 0.0f;
            bool active = false, looping = false, filtered = false, holdsNote = true;
            int note = -1;
            uint32_t age = 0;
        };

        void install (std::shared_ptr<const SamplerSample> s, const juce::File& f, double start, double end)
        {
            {
                const juce::ScopedLock lock (infoLock);
                file = f;
                regionStartWanted = start;
                regionEndWanted = end;
            }
            std::shared_ptr<const SamplerSample> old;
            {
                const juce::SpinLock::ScopedLockType lock (dataLock);
                old = std::exchange (sample, std::move (s));
            }
            retire (std::move (old));
        }

        template <typename T>
        void retire (std::shared_ptr<T> old)
        {
            // Kept until no voice uses it: the audio thread never frees memory.
            if (old == nullptr)
                return;
            const juce::ScopedLock lock (retireLock);
            retiredSamples.erase (std::remove_if (retiredSamples.begin(), retiredSamples.end(),
                                                  [] (const auto& p) { return p.use_count() == 1; }), retiredSamples.end());
            retiredSamples.push_back (std::move (old));
        }

        void handle (const juce::MidiMessage& m)
        {
            if (m.isNoteOn())
                trigger (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
            {
                for (auto& v : voices)
                    if (v.active && v.note == m.getNoteNumber() && v.holdsNote)
                        v.env.noteOff();
            }
            else if (m.isAllNotesOff())
            {
                for (auto& v : voices)
                    if (v.active && v.holdsNote)
                        v.env.noteOff();
            }
            else if (m.isAllSoundOff())
                reset();
        }

        void trigger (int note, float vel)
        {
            std::shared_ptr<const SamplerSample> s;
            std::shared_ptr<const std::vector<double>> sl;
            {
                const juce::SpinLock::ScopedLockType lock (dataLock);
                s = sample;
                sl = slices;
            }
            if (s == nullptr || s->length() < 2)
                return;

            const auto length = (double) s->length();
            const auto slicing = mode->load (std::memory_order_relaxed) >= 0.5f;
            double begin = 0.0, end = length - 1.0, semitones = 0.0;
            auto looping = false;
            double loopFrom = 0.0, loopTo = 0.0;

            if (slicing)
            {
                const auto index = sliceForNote (note);
                if (sl == nullptr || index < 0 || index >= (int) sl->size())
                    return;
                begin = juce::jlimit (0.0, length - 2.0, (*sl)[(size_t) index] * s->sampleRate);
                end = index + 1 < (int) sl->size() ? juce::jlimit (begin + 1.0, length - 1.0, (*sl)[(size_t) index + 1] * s->sampleRate)
                                                   : length - 1.0;
                semitones = tune->load (std::memory_order_relaxed);
            }
            else
            {
                semitones = (double) (note - juce::roundToInt (root->load (std::memory_order_relaxed))) + tune->load (std::memory_order_relaxed);
                const auto a = (double) loopStart->load (std::memory_order_relaxed) * length;
                const auto b = (double) loopEnd->load (std::memory_order_relaxed) * length;
                if (loop->load (std::memory_order_relaxed) >= 0.5f && b - a > 32.0)
                {
                    looping = true;
                    loopFrom = a;
                    loopTo = juce::jmin (b, length - 1.0);
                }
            }

            Voice* slot = nullptr;
            for (auto& v : voices)
                if (! v.active) { slot = &v; break; }
            if (slot == nullptr)
                for (auto& v : voices)
                    if (slot == nullptr || v.age < slot->age)
                        slot = &v;

            const auto sensitivity = velocity->load (std::memory_order_relaxed);
            slot->sample = s;
            slot->position = begin;
            slot->endAt = end;
            slot->step = s->sampleRate / sampleRate * std::exp2 (semitones / 12.0);
            slot->looping = looping;
            slot->loopFrom = loopFrom;
            slot->loopTo = loopTo;
            slot->gain = ((1.0f - sensitivity) + sensitivity * vel) * 0.9f;
            slot->holdsNote = oneShot->load (std::memory_order_relaxed) < 0.5f;
            slot->env.setSampleRate (sampleRate);
            slot->env.setParameters (attack->load (std::memory_order_relaxed), decay->load (std::memory_order_relaxed),
                                     sustain->load (std::memory_order_relaxed), release->load (std::memory_order_relaxed));
            slot->env.reset();
            slot->env.noteOn();
            const auto cut = cutoff->load (std::memory_order_relaxed);
            slot->filtered = cut < 19000.0f;
            if (slot->filtered)
            {
                slot->filterLeft.reset(); slot->filterRight.reset();
                slot->filterLeft.set (cut, 0.15f, sampleRate);
                slot->filterRight.set (cut, 0.15f, sampleRate);
            }
            slot->note = note;
            slot->age = ++counter;
            slot->active = true;
        }

        void renderVoices (float* left, float* right, int n)
        {
            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                const auto& s = *v.sample;
                const auto length = s.length();
                const auto* l = s.left.data();
                const auto* r = s.right.empty() ? l : s.right.data();

                for (int i = 0; i < n; ++i)
                {
                    if (v.looping && v.position >= v.loopTo)
                        v.position -= (v.loopTo - v.loopFrom); // keeps looping through the release

                    const auto index = (int) v.position;
                    if (v.position >= v.endAt || index >= length - 1)
                    {
                        v.active = false;
                        break;
                    }
                    const auto frac = (float) (v.position - index);
                    auto sl = l[index] + (l[index + 1] - l[index]) * frac;
                    auto sr = r[index] + (r[index + 1] - r[index]) * frac;
                    if (v.filtered)
                    {
                        sl = v.filterLeft.lowpass (sl);
                        sr = v.filterRight.lowpass (sr);
                    }

                    auto g = v.env.next() * v.gain;
                    if (! v.env.isActive())
                    {
                        v.active = false;
                        break;
                    }
                    if (! v.looping)
                        if (const auto remaining = v.endAt - v.position; remaining < 48.0) // a short fade: no click at the end
                            g *= (float) remaining / 48.0f;

                    left[i] += sl * g;
                    right[i] += sr * g;
                    v.position += v.step;
                }

                if (! v.active)
                    v.sample.reset(); // the retired list frees it, on the message thread
            }
        }

        std::atomic<float> *mode, *root, *tune, *attack, *decay, *sustain, *release, *cutoff, *loop, *loopStart, *loopEnd, *oneShot, *velocity;

        mutable juce::SpinLock dataLock;
        std::shared_ptr<const SamplerSample> sample;
        std::shared_ptr<const std::vector<double>> slices;
        mutable juce::CriticalSection infoLock;
        juce::File file;
        double regionStartWanted = 0.0, regionEndWanted = 0.0;
        juce::CriticalSection retireLock;
        std::vector<std::shared_ptr<const void>> retiredSamples;

        std::array<Voice, (size_t) numVoices> voices;
        uint32_t counter = 0;
        double sampleRate = 48000.0;
    };

    //==========================================================================
    class SamplerEditor final : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
    {
    public:
        explicit SamplerEditor (InstrumentProcessor& p)
            : AudioProcessorEditor (p), processor (p), sampler (*asSampler (p.getEngine())), look (p.getSpec().accent)
        {
            setLookAndFeel (&look);
            auto& state = p.getState();

            loadButton.onClick = [this] { chooseFile(); };
            clearButton.onClick = [this] { sampler.clear(); repaint(); };
            addAndMakeVisible (loadButton);
            addAndMakeVisible (clearButton);

            modeBox.addItemList ({ "Keys", "Slices" }, 1);
            addAndMakeVisible (modeBox);
            modeAttachment = std::make_unique<ComboBoxAttachment> (state, "mode", modeBox);

            for (auto* t : { &loopToggle, &oneShotToggle })
                addAndMakeVisible (*t);
            loopAttachment = std::make_unique<ButtonAttachment> (state, "loop", loopToggle);
            oneShotAttachment = std::make_unique<ButtonAttachment> (state, "oneShot", oneShotToggle);

            const struct { const char* id; const char* label; } list[] = {
                { "root", "Root" }, { "tune", "Tune" }, { "attack", "Attack" }, { "decay", "Decay" }, { "sustain", "Sustain" },
                { "release", "Release" }, { "cutoff", "Cutoff" }, { "velocity", "Velocity" }, { "volume", "Volume" } };
            for (const auto& item : list)
            {
                auto knobControl = std::make_unique<Knob>();
                knobControl->slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
                knobControl->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
                knobControl->slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (InstrumentLook::text));
                knobControl->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                knobControl->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
                knobControl->label.setText (item.label, juce::dontSendNotification);
                knobControl->label.setJustificationType (juce::Justification::centred);
                knobControl->label.setColour (juce::Label::textColourId, juce::Colour (InstrumentLook::dimText));
                knobControl->label.setFont (juce::Font (juce::FontOptions (11.5f)));
                knobControl->attachment = std::make_unique<SliderAttachment> (state, item.id, knobControl->slider);
                addAndMakeVisible (knobControl->slider);
                addAndMakeVisible (knobControl->label);
                knobs.push_back (std::move (knobControl));
            }

            setSize (760, 440);
            startTimerHz (10);
        }

        ~SamplerEditor() override
        {
            stopTimer();
            setLookAndFeel (nullptr);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (InstrumentLook::background));
            auto header = getLocalBounds().reduced (16, 12).removeFromTop (44);
            g.setColour (juce::Colour (InstrumentLook::text));
            g.setFont (juce::Font (juce::FontOptions (24.0f, juce::Font::bold)));
            g.drawText ("Sampler", header.removeFromLeft (110), juce::Justification::centredLeft);
            g.setColour (look.accent);
            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.drawText ("Play a sample over the keys, or slice it", header.removeFromLeft (260), juce::Justification::centredLeft);

            g.setColour (juce::Colour (0xff111319));
            g.fillRoundedRectangle (waveArea.toFloat(), 6.0f);
            const auto s = sampler.getSample();
            if (s == nullptr || s->peaks.empty())
            {
                g.setColour (juce::Colour (InstrumentLook::dimText));
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (dragOver ? "Drop to load" : "Drop an audio file here, or press Load",
                            waveArea, juce::Justification::centred);
                return;
            }

            const auto inner = waveArea.toFloat().reduced (6.0f, 6.0f);
            const auto slicing = modeBox.getSelectedId() == 2;
            if (! slicing && loopToggle.getToggleState())
            {
                const auto a = (float) processor.getState().getRawParameterValue ("loopStart")->load();
                const auto b = (float) processor.getState().getRawParameterValue ("loopEnd")->load();
                g.setColour (look.accent.withAlpha (0.18f));
                g.fillRect (inner.getX() + a * inner.getWidth(), inner.getY(), juce::jmax (1.0f, (b - a) * inner.getWidth()), inner.getHeight());
            }

            const auto n = (int) s->peaks.size();
            g.setColour (look.accent);
            for (int x = 0; x < (int) inner.getWidth(); ++x)
            {
                const auto peak = s->peaks[(size_t) juce::jlimit (0, n - 1, x * n / juce::jmax (1, (int) inner.getWidth()))];
                const auto h = juce::jmax (1.0f, peak * inner.getHeight());
                g.fillRect (inner.getX() + (float) x, inner.getCentreY() - h * 0.5f, 1.0f, h);
            }

            if (slicing)
            {
                g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                const auto slices = sampler.getSlices();
                for (size_t i = 0; i < slices.size(); ++i)
                {
                    const auto x = inner.getX() + (float) (slices[i] / s->seconds()) * inner.getWidth();
                    g.setColour (juce::Colours::white.withAlpha (0.7f));
                    g.fillRect (x, inner.getY(), 1.0f, inner.getHeight());
                    g.drawText (juce::String ((int) i + 1), juce::Rectangle<float> (x + 2.0f, inner.getY(), 24.0f, 14.0f),
                                juce::Justification::centredLeft);
                }
            }
            else
            {
                g.setColour (juce::Colour (InstrumentLook::dimText));
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText ("Drag here to set the loop", waveArea.withTrimmedLeft (10).withTrimmedBottom (4), juce::Justification::bottomLeft);
            }

            g.setColour (juce::Colour (InstrumentLook::text));
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (s->name + "  ·  " + juce::String (s->seconds(), 2) + " s", waveArea.reduced (10, 4),
                        juce::Justification::topRight);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 12);
            auto header = area.removeFromTop (44);
            clearButton.setBounds (header.removeFromRight (70).withSizeKeepingCentre (70, 28));
            header.removeFromRight (6);
            loadButton.setBounds (header.removeFromRight (100).withSizeKeepingCentre (100, 28));
            header.removeFromRight (10);
            modeBox.setBounds (header.removeFromRight (100).withSizeKeepingCentre (100, 28));
            area.removeFromTop (6);

            waveArea = area.removeFromTop (180);
            area.removeFromTop (8);
            auto toggles = area.removeFromTop (26);
            loopToggle.setBounds (toggles.removeFromLeft (150));
            oneShotToggle.setBounds (toggles.removeFromLeft (230));
            area.removeFromTop (6);

            const auto cell = area.getWidth() / (int) knobs.size();
            for (auto& k : knobs)
            {
                auto c = area.removeFromLeft (cell);
                k->label.setBounds (c.removeFromTop (16));
                k->slider.setBounds (c);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override { dragLoop (e, true); }
        void mouseDrag (const juce::MouseEvent& e) override { dragLoop (e, false); }

        bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
        void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
        void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
        void filesDropped (const juce::StringArray& files, int, int) override
        {
            dragOver = false;
            loadPath (files[0]);
        }

    private:
        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

        struct Knob
        {
            juce::Slider slider;
            juce::Label label;
            std::unique_ptr<SliderAttachment> attachment;
        };

        void timerCallback() override
        {
            const auto n = sampler.getSlices().size();
            const auto have = sampler.getSample() != nullptr;
            if (n != lastSlices || have != lastHave)
            {
                lastSlices = n;
                lastHave = have;
                repaint();
            }
        }

        void setParam (const char* id, float value)
        {
            if (auto* parameter = processor.getState().getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        }

        // Dragging over the waveform in Keys mode sets the loop points and turns the loop on.
        void dragLoop (const juce::MouseEvent& e, bool)
        {
            if (modeBox.getSelectedId() == 2 || sampler.getSample() == nullptr || ! waveArea.contains (e.getMouseDownPosition()))
                return;
            const auto inner = waveArea.toFloat().reduced (6.0f, 6.0f);
            const auto at = [&] (int x) { return juce::jlimit (0.0f, 1.0f, ((float) x - inner.getX()) / inner.getWidth()); };
            const auto a = at (e.getMouseDownPosition().x), b = at (e.x);
            if (std::abs (b - a) > 0.002f)
            {
                setParam ("loopStart", juce::jmin (a, b));
                setParam ("loopEnd", juce::jmax (a, b));
                setParam ("loop", 1.0f);
                repaint();
            }
        }

        void chooseFile()
        {
            chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& c)
                                  {
                                      if (c.getResult() != juce::File())
                                          loadPath (c.getResult().getFullPathName());
                                  });
        }

        void loadPath (const juce::String& path)
        {
            juce::String error;
            if (! sampler.load (juce::File (path), 0.0, 0.0, error))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Sampler", error);
            setParam ("loopStart", 0.0f);
            setParam ("loopEnd", 1.0f);
            repaint();
        }

        InstrumentProcessor& processor;
        SamplerInstrument& sampler;
        InstrumentLook look;
        juce::TextButton loadButton { "Load…" }, clearButton { "Clear" };
        juce::ComboBox modeBox;
        juce::ToggleButton loopToggle { "Loop" }, oneShotToggle { "One-shot (ignore key release)" };
        std::unique_ptr<ComboBoxAttachment> modeAttachment;
        std::unique_ptr<ButtonAttachment> loopAttachment, oneShotAttachment;
        std::vector<std::unique_ptr<Knob>> knobs;
        juce::Rectangle<int> waveArea;
        std::unique_ptr<juce::FileChooser> chooser;
        bool dragOver = false, lastHave = false;
        size_t lastSlices = 0;
    };
}

SamplerInstrument* asSampler (Engine& engine) { return dynamic_cast<SamplerInstrument*> (&engine); }

const Spec& getSamplerSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "sampler";
        s.name = "Sampler";
        s.tagline = "Play a sample over the keys, or slice it";
        s.category = "Sampler";
        s.accent = juce::Colour (0xffe8a23a);
        s.lowestKey = 36;
        s.highestKey = 96;

        s.params.push_back (choice ("mode", "Mode", { "Keys", "Slices" }, 0));
        s.params.push_back (knob ("root", "Root note", 0.0f, 127.0f, 60.0f, Unit::none, 0.0f, 1.0f));
        s.params.push_back (knob ("tune", "Tune", -24.0f, 24.0f, 0.0f, Unit::semitones, 0.0f, 0.01f));
        s.params.push_back (knob ("attack", "Attack", 0.001f, 5.0f, 0.002f, Unit::seconds, 0.2f));
        s.params.push_back (knob ("decay", "Decay", 0.01f, 10.0f, 0.5f, Unit::seconds, 1.0f));
        s.params.push_back (knob ("sustain", "Sustain", 0.0f, 1.0f, 1.0f));
        s.params.push_back (knob ("release", "Release", 0.005f, 10.0f, 0.15f, Unit::seconds, 0.5f));
        s.params.push_back (knob ("cutoff", "Cutoff", 40.0f, 20000.0f, 20000.0f, Unit::hertz, 1500.0f));
        s.params.push_back (toggle ("loop", "Loop", false));
        s.params.push_back (knob ("loopStart", "Loop start", 0.0f, 1.0f, 0.0f));
        s.params.push_back (knob ("loopEnd", "Loop end", 0.0f, 1.0f, 1.0f));
        s.params.push_back (toggle ("oneShot", "One-shot", false));
        s.params.push_back (knob ("velocity", "Velocity", 0.0f, 1.0f, 0.6f));

        s.presets = { { "Empty Sampler", {} } };
        s.makeEngine = [] (Params p) { return std::make_unique<SamplerEngine> (p); };
        s.makeEditor = [] (InstrumentProcessor& p) -> juce::AudioProcessorEditor* { return new SamplerEditor (p); };
        return s;
    }();
    return spec;
}
}
