// Kit: the sampler drum kit (see Kit.h). The engine plays the samples; the window is
// sixteen pads you click or drop files on, and the knobs of the selected pad.
#include "Kit.h"

#include "Fx.h"
#include "InstrumentEditor.h"
#include "Registry.h"

#include <deque>

namespace micfx::inst
{
namespace
{
    constexpr int numPads = KitSampler::numPads;
    constexpr int numVoices = 32;

    //==========================================================================
    juce::CriticalSection searchLock;
    juce::Array<juce::File> searchFolders;

    juce::File findMoved (const juce::File& original)
    {
        const auto name = original.getFileName();
        if (name.isEmpty())
            return {};

        juce::Array<juce::File> folders;
        {
            const juce::ScopedLock lock (searchLock);
            folders = searchFolders;
        }
        folders.insert (0, original.getParentDirectory());

        for (const auto& folder : folders)
            if (const auto direct = folder.getChildFile (name); direct.existsAsFile())
                return direct;
        for (const auto& folder : folders)
            if (folder.isDirectory())
            {
                const auto found = folder.findChildFiles (juce::File::findFiles, true, name);
                if (! found.isEmpty())
                    return found.getFirst();
            }
        return {};
    }

    std::shared_ptr<KitSample> readSample (const juce::File& file, juce::String& error)
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

        const auto length = (int) juce::jmin ((juce::int64) (KitSampler::maxSampleSeconds * reader->sampleRate), reader->lengthInSamples);
        const auto channels = (int) juce::jmin (2u, reader->numChannels);
        juce::AudioBuffer<float> buffer (channels, length);
        if (! reader->read (&buffer, 0, length, 0, true, channels > 1))
        {
            error = "Couldn't decode " + file.getFileName() + ".";
            return {};
        }

        auto sample = std::make_shared<KitSample>();
        sample->sampleRate = reader->sampleRate;
        sample->name = file.getFileNameWithoutExtension();
        sample->file = file;
        sample->left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
        if (channels > 1)
            sample->right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + length);

        constexpr int buckets = 200;
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

    // Every pad's parameter ids (ParamSpec keeps a pointer to its id, so they live for good).
    struct PadIds { std::string level, pan, tune, decay, choke; };

    const std::vector<PadIds>& padIds()
    {
        static const std::vector<PadIds> ids = []
        {
            std::vector<PadIds> list;
            for (int i = 1; i <= numPads; ++i)
            {
                const auto n = std::to_string (i);
                list.push_back ({ "p" + n + "Level", "p" + n + "Pan", "p" + n + "Tune", "p" + n + "Decay", "p" + n + "Choke" });
            }
            return list;
        }();
        return ids;
    }

    constexpr float fullDecay = 8.0f; // the decay knob's top: the whole sample

    // The hats choke each other, like the real ones: closed (42), pedal (44) and open (46).
    int defaultChoke (int pad) { return pad == 6 || pad == 8 || pad == 10 ? 1 : 0; }

    //==========================================================================
    class KitEngine final : public Engine, public KitSampler
    {
    public:
        explicit KitEngine (Params p)
        {
            const auto& ids = padIds();
            for (int i = 0; i < numPads; ++i)
            {
                level[i] = p (ids[(size_t) i].level.c_str());
                pan[i] = p (ids[(size_t) i].pan.c_str());
                tune[i] = p (ids[(size_t) i].tune.c_str());
                decay[i] = p (ids[(size_t) i].decay.c_str());
                choke[i] = p (ids[(size_t) i].choke.c_str());
            }
            velocity = p ("velocity");
            room = p ("room");
        }

        ~KitEngine() override = default;

        //======================================================================
        void prepare (double rate, int) override
        {
            sampleRate = rate;
            this->room_.prepare (rate);
            reset();
        }

        void reset() override
        {
            for (auto& v : voices)
                v.active = false;
            room_.reset();
        }

        double getTailSeconds() const override { return tail.load(); }

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

            room_.process (buffer.getWritePointer (0), buffer.getWritePointer (buffer.getNumChannels() > 1 ? 1 : 0),
                           numSamples, room->load (std::memory_order_relaxed), 0.5f, 0.6f);
        }

        //======================================================================
        bool loadSample (int pad, const juce::File& file, juce::String& error) override
        {
            if (! isPad (pad))
            {
                error = "No such pad.";
                return false;
            }
            auto sample = readSample (file, error);
            if (sample == nullptr)
                return false;
            install (pad, std::move (sample), file);
            return true;
        }

        void clearPad (int pad) override
        {
            if (isPad (pad))
                install (pad, nullptr, {});
        }

        std::shared_ptr<const KitSample> getSample (int pad) const override
        {
            if (! isPad (pad))
                return {};
            const juce::SpinLock::ScopedLockType lock (padLock);
            return pads[(size_t) pad];
        }

        juce::File getPadFile (int pad) const override
        {
            if (! isPad (pad))
                return {};
            const juce::ScopedLock lock (infoLock);
            return files[(size_t) pad];
        }

        float takeFlash (int pad) override
        {
            return isPad (pad) ? flash[(size_t) pad].exchange (0.0f) : 0.0f;
        }

        //======================================================================
        void saveExtraState (juce::XmlElement& xml) const override
        {
            const juce::ScopedLock lock (infoLock);
            for (int i = 0; i < numPads; ++i)
                if (files[(size_t) i] != juce::File())
                {
                    auto* pad = xml.createNewChildElement ("Pad");
                    pad->setAttribute ("index", i);
                    pad->setAttribute ("path", files[(size_t) i].getFullPathName());
                    pad->setAttribute ("name", files[(size_t) i].getFileName());
                }
        }

        void loadExtraState (const juce::XmlElement& xml) override
        {
            std::array<juce::File, (size_t) numPads> wanted {};
            for (const auto* pad : xml.getChildWithTagNameIterator ("Pad"))
                if (const auto i = pad->getIntAttribute ("index", -1); isPad (i))
                {
                    wanted[(size_t) i] = juce::File (pad->getStringAttribute ("path"));
                    if (wanted[(size_t) i] == juce::File() || wanted[(size_t) i].getFileName().isEmpty())
                        wanted[(size_t) i] = juce::File();
                }

            for (int i = 0; i < numPads; ++i)
            {
                const auto& file = wanted[(size_t) i];
                if (file == juce::File())
                {
                    if (getSample (i) != nullptr || getPadFile (i) != juce::File())
                        clearPad (i);
                    continue;
                }
                if (const auto current = getSample (i); current != nullptr && current->file == file)
                    continue;

                juce::String error;
                auto sample = readSample (file, error);
                if (sample == nullptr)
                    if (const auto moved = findMoved (file); moved != juce::File())
                        sample = readSample (moved, error);
                if (sample != nullptr)
                    install (i, sample, sample->file);
                else
                    install (i, nullptr, file); // keep the path: the sample is missing, not forgotten
            }
        }

        juce::String getKeyName (int note) const override
        {
            const auto pad = padForNote (note);
            if (! isPad (pad))
                return {};
            if (const auto sample = getSample (pad))
                return sample->name;
            if (const auto file = getPadFile (pad); file != juce::File())
                return file.getFileNameWithoutExtension();
            return hintName (pad);
        }

    private:
        struct Voice
        {
            std::shared_ptr<const KitSample> sample;
            double position = 0.0, step = 1.0;
            float gainLeft = 0.0f, gainRight = 0.0f;
            float envelope = 1.0f, envelopeCoefficient = 1.0f;
            float attack = 0.0f, attackStep = 1.0f;
            float chokeGain = 1.0f;
            bool active = false, choking = false;
            int pad = -1, group = 0;
            uint32_t age = 0;
        };

        // Swaps a pad's sample (message thread or the thread that loads a project). The old
        // one is kept until no voice plays it, so the audio thread never frees memory.
        void install (int pad, std::shared_ptr<const KitSample> sample, const juce::File& file)
        {
            std::shared_ptr<const KitSample> old;
            {
                const juce::SpinLock::ScopedLockType lock (padLock);
                old = std::exchange (pads[(size_t) pad], sample);
            }
            {
                const juce::ScopedLock lock (infoLock);
                files[(size_t) pad] = file;
            }

            const juce::ScopedLock lock (retireLock);
            if (old != nullptr)
                retired.push_back (std::move (old));
            std::erase_if (retired, [] (const auto& s) { return s.use_count() == 1; });

            auto longest = 0.0;
            for (int i = 0; i < numPads; ++i)
                if (const auto s = getSample (i))
                    longest = juce::jmax (longest, s->seconds());
            tail.store (juce::jmin (longest, 8.0) + 1.0);
        }

        void handle (const juce::MidiMessage& m)
        {
            if (m.isNoteOn())
                trigger (padForNote (m.getNoteNumber()), m.getFloatVelocity());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                if (m.isAllSoundOff())
                    for (auto& v : voices)
                        v.active = false;
                else
                    for (auto& v : voices)
                        v.choking = v.active;
            }
        }

        void trigger (int pad, float vel)
        {
            if (! isPad (pad))
                return;
            std::shared_ptr<const KitSample> sample;
            {
                const juce::SpinLock::ScopedLockType lock (padLock);
                sample = pads[(size_t) pad];
            }
            if (sample == nullptr || sample->length() < 2)
                return;

            const auto group = juce::roundToInt (choke[pad]->load (std::memory_order_relaxed));
            for (auto& v : voices)
                if (v.active && (v.pad == pad || (group > 0 && v.group == group)))
                    v.choking = true;

            Voice* slot = nullptr;
            for (auto& v : voices)
                if (! v.active) { slot = &v; break; }
            if (slot == nullptr)
                for (auto& v : voices)
                    if (slot == nullptr || v.age < slot->age)
                        slot = &v;

            const auto sensitivity = velocity->load (std::memory_order_relaxed);
            const auto gain = juce::Decibels::decibelsToGain (level[pad]->load (std::memory_order_relaxed))
                              * ((1.0f - sensitivity) + sensitivity * vel);
            const auto angle = (0.5f + 0.5f * pan[pad]->load (std::memory_order_relaxed)) * juce::MathConstants<float>::halfPi;
            const auto decayTime = decay[pad]->load (std::memory_order_relaxed);

            slot->sample = sample;
            slot->position = 0.0;
            slot->step = sample->sampleRate / sampleRate * std::exp2 ((double) tune[pad]->load (std::memory_order_relaxed) / 12.0);
            slot->gainLeft = gain * std::cos (angle) * 1.41f;
            slot->gainRight = gain * std::sin (angle) * 1.41f;
            slot->envelope = 1.0f;
            slot->envelopeCoefficient = decayTime >= fullDecay - 0.05f ? 1.0f : decayCoefficient (decayTime / 4.6f, sampleRate);
            slot->attack = 0.0f;
            slot->attackStep = (float) (1.0 / (0.0005 * sampleRate));
            slot->chokeGain = 1.0f;
            slot->choking = false;
            slot->pad = pad;
            slot->group = group;
            slot->age = ++counter;
            slot->active = true;
            flash[(size_t) pad].store (juce::jmax (0.35f, vel));
        }

        void renderVoices (float* left, float* right, int n)
        {
            const auto chokeCoefficient = decayCoefficient (0.004f, sampleRate);
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
                    const auto index = (int) v.position;
                    if (index >= length - 1)
                    {
                        v.active = false;
                        break;
                    }
                    const auto frac = (float) (v.position - index);
                    const auto sl = l[index] + (l[index + 1] - l[index]) * frac;
                    const auto sr = r[index] + (r[index + 1] - r[index]) * frac;

                    auto g = v.envelope;
                    if (v.attack < 1.0f)
                    {
                        v.attack = juce::jmin (1.0f, v.attack + v.attackStep);
                        g *= v.attack;
                    }
                    if (const auto left_ = length - 1 - index; left_ < 48) // a short fade: never a click at the end
                        g *= (float) left_ / 48.0f;
                    if (v.choking)
                    {
                        v.chokeGain *= chokeCoefficient;
                        if (v.chokeGain < 1.0e-4f)
                        {
                            v.active = false;
                            break;
                        }
                        g *= v.chokeGain;
                    }

                    left[i] += sl * g * v.gainLeft;
                    right[i] += sr * g * v.gainRight;
                    v.envelope *= v.envelopeCoefficient;
                    v.position += v.step;
                }

                if (v.envelope < 1.0e-5f)
                    v.active = false;
                if (! v.active)
                    v.sample.reset(); // the retired list frees it, on the message thread
            }
        }

        std::atomic<float>* level[numPads] {};
        std::atomic<float>* pan[numPads] {};
        std::atomic<float>* tune[numPads] {};
        std::atomic<float>* decay[numPads] {};
        std::atomic<float>* choke[numPads] {};
        std::atomic<float>* velocity = nullptr;
        std::atomic<float>* room = nullptr;

        mutable juce::SpinLock padLock;
        std::array<std::shared_ptr<const KitSample>, (size_t) numPads> pads;
        mutable juce::CriticalSection infoLock;
        std::array<juce::File, (size_t) numPads> files;
        juce::CriticalSection retireLock;
        std::vector<std::shared_ptr<const KitSample>> retired;

        std::array<std::atomic<float>, (size_t) numPads> flash {};
        std::atomic<double> tail { 1.0 };
        std::array<Voice, (size_t) numVoices> voices;
        uint32_t counter = 0;
        double sampleRate = 48000.0;
        Room room_;
    };

    //==========================================================================
    // The window.
    class KitEditor final : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit KitEditor (InstrumentProcessor& p)
            : AudioProcessorEditor (p), processor (p), kit (*asKit (p.getEngine())), look (p.getSpec().accent)
        {
            setLookAndFeel (&look);

            for (int i = 0; i < numPads; ++i)
            {
                pads[(size_t) i] = std::make_unique<PadView> (*this, i);
                addAndMakeVisible (*pads[(size_t) i]);
            }

            loadButton.onClick = [this] { chooseFiles(); };
            clearButton.onClick = [this] { kit.clearPad (selected); refresh(); };
            clearAllButton.onClick = [this]
            {
                for (int i = 0; i < numPads; ++i)
                    kit.clearPad (i);
                refresh();
            };
            addAndMakeVisible (loadButton);
            addAndMakeVisible (clearButton);
            addAndMakeVisible (clearAllButton);

            for (auto* label : { &levelLabel, &panLabel, &tuneLabel, &decayLabel, &chokeLabel, &velocityLabel, &roomLabel })
            {
                label->setJustificationType (juce::Justification::centred);
                label->setColour (juce::Label::textColourId, juce::Colour (InstrumentLook::dimText));
                label->setFont (juce::Font (juce::FontOptions (11.5f)));
                addAndMakeVisible (*label);
            }

            for (auto* slider : { &levelSlider, &panSlider, &tuneSlider, &decaySlider, &velocitySlider, &roomSlider, &volumeSlider })
            {
                slider->setSliderStyle (juce::Slider::RotaryVerticalDrag);
                slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
                slider->setColour (juce::Slider::textBoxTextColourId, juce::Colour (InstrumentLook::text));
                slider->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                slider->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
                addAndMakeVisible (*slider);
            }
            chokeBox.addItemList ({ "Off", "A", "B", "C", "D" }, 1);
            addAndMakeVisible (chokeBox);

            auto& state = p.getState();
            velocityAttachment = std::make_unique<SliderAttachment> (state, "velocity", velocitySlider);
            roomAttachment = std::make_unique<SliderAttachment> (state, "room", roomSlider);
            volumeAttachment = std::make_unique<SliderAttachment> (state, "volume", volumeSlider);

            select (0);
            setSize (780, 640);
            startTimerHz (30);
        }

        ~KitEditor() override
        {
            stopTimer();
            setLookAndFeel (nullptr);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (InstrumentLook::background));

            auto header = getLocalBounds().reduced (16, 12).removeFromTop (74).withHeight (44);
            g.setColour (juce::Colour (InstrumentLook::text));
            g.setFont (juce::Font (juce::FontOptions (24.0f, juce::Font::bold)));
            g.drawText ("Kit", header.removeFromLeft (70), juce::Justification::centredLeft);
            g.setColour (look.accent);
            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.drawText ("Sampler drum kit", header.removeFromLeft (150), juce::Justification::centredLeft);

            g.setColour (juce::Colour (InstrumentLook::panel));
            g.fillRoundedRectangle (detailArea.toFloat(), 8.0f);
            g.setColour (juce::Colour (InstrumentLook::panelEdge));
            g.drawRoundedRectangle (detailArea.toFloat().reduced (0.5f), 8.0f, 1.0f);

            auto strip = detailArea.reduced (12, 8);
            const auto sample = kit.getSample (selected);
            const auto file = kit.getPadFile (selected);
            g.setColour (juce::Colour (InstrumentLook::text));
            g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            g.drawText ("Pad " + juce::String (selected + 1) + "  ·  "
                            + juce::MidiMessage::getMidiNoteName (KitSampler::noteForPad (selected), true, true, 4)
                            + "  ·  " + (sample != nullptr ? sample->name : file != juce::File() ? file.getFileName() + " (not found)"
                                                                                                    : KitSampler::hintName (selected)),
                        strip.removeFromTop (20), juce::Justification::centredLeft);

            waveArea = strip.removeFromLeft (250).removeFromTop (92).withTrimmedTop (6).withTrimmedBottom (10);
            paintWave (g, waveArea, sample.get(), look.accent, selected);
            if (sample != nullptr)
            {
                g.setColour (juce::Colour (InstrumentLook::dimText));
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (juce::String (sample->seconds(), 2) + " s  ·  " + juce::String (juce::roundToInt (sample->sampleRate)) + " Hz"
                                + (sample->right.empty() ? "  ·  mono" : "  ·  stereo"),
                            clearButton.getBounds().withX (clearButton.getRight() + 10).withWidth (170), juce::Justification::centredLeft);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 12);
            auto header = area.removeFromTop (74);
            volumeSlider.setBounds (header.removeFromRight (70));
            header.removeFromRight (8);
            clearAllButton.setBounds (header.removeFromRight (80).withSizeKeepingCentre (80, 28));
            header.removeFromRight (6);
            loadButton.setBounds (header.removeFromRight (130).withSizeKeepingCentre (130, 28));

            detailArea = area.removeFromBottom (150);
            area.removeFromBottom (10);

            const auto padWidth = (area.getWidth() - 3 * 10) / 4, padHeight = (area.getHeight() - 3 * 10) / 4;
            for (int i = 0; i < numPads; ++i)
            {
                const auto column = i % 4, row = 3 - i / 4; // pad 1 bottom left, like a drum machine
                pads[(size_t) i]->setBounds (area.getX() + column * (padWidth + 10), area.getY() + row * (padHeight + 10), padWidth, padHeight);
            }

            auto strip = detailArea.reduced (12, 8);
            strip.removeFromTop (20);
            auto buttons = strip.removeFromLeft (250).removeFromBottom (34);
            clearButton.setBounds (buttons.removeFromLeft (80).withSizeKeepingCentre (80, 26));

            strip.removeFromLeft (14);
            const auto knobWidth = strip.getWidth() / 7;
            auto place = [&] (juce::Component& c, juce::Label& l, const char* name)
            {
                auto cell = strip.removeFromLeft (knobWidth);
                l.setText (name, juce::dontSendNotification);
                l.setBounds (cell.removeFromTop (16));
                c.setBounds (cell.withTrimmedBottom (2));
            };
            place (levelSlider, levelLabel, "Level");
            place (panSlider, panLabel, "Pan");
            place (tuneSlider, tuneLabel, "Tune");
            place (decaySlider, decayLabel, "Decay");
            {
                auto cell = strip.removeFromLeft (knobWidth);
                chokeLabel.setText ("Choke", juce::dontSendNotification);
                chokeLabel.setBounds (cell.removeFromTop (16));
                chokeBox.setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth() - 6, 70), 26).withY (cell.getY() + 16));
            }
            place (velocitySlider, velocityLabel, "Velocity");
            place (roomSlider, roomLabel, "Room");
        }

        //======================================================================
        // Pads.
        void select (int pad)
        {
            selected = pad;
            const auto& ids = padIds()[(size_t) pad];
            auto& state = processor.getState();
            levelAttachment.reset(); panAttachment.reset(); tuneAttachment.reset(); decayAttachment.reset(); chokeAttachment.reset();
            levelAttachment = std::make_unique<SliderAttachment> (state, ids.level, levelSlider);
            panAttachment = std::make_unique<SliderAttachment> (state, ids.pan, panSlider);
            tuneAttachment = std::make_unique<SliderAttachment> (state, ids.tune, tuneSlider);
            decayAttachment = std::make_unique<SliderAttachment> (state, ids.decay, decaySlider);
            chokeAttachment = std::make_unique<ComboBoxAttachment> (state, ids.choke, chokeBox);
            for (auto& view : pads)
                view->repaint();
            repaint();
        }

        void refresh()
        {
            for (auto& view : pads)
                view->repaint();
            repaint();
        }

        // Files dropped or chosen: the first on this pad, the rest on the empty pads after it.
        void loadFiles (const juce::StringArray& paths, int firstPad)
        {
            juce::String problems;
            auto pad = firstPad;
            auto first = true;
            for (const auto& path : paths)
            {
                if (! first)
                    while (pad < numPads && kit.getSample (pad) != nullptr)
                        ++pad;
                if (pad >= numPads)
                    break;
                juce::String error;
                if (kit.loadSample (pad, juce::File (path), error))
                {
                    if (first)
                        select (pad);
                    ++pad;
                    first = false;
                }
                else
                    problems << error << " ";
            }
            if (problems.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Kit", problems.trim());
            refresh();
        }

        static void paintWave (juce::Graphics& g, juce::Rectangle<int> area, const KitSample* sample, juce::Colour colour, int)
        {
            g.setColour (juce::Colour (0xff111319));
            g.fillRoundedRectangle (area.toFloat(), 5.0f);
            if (sample == nullptr || sample->peaks.empty())
                return;
            const auto inner = area.toFloat().reduced (4.0f, 4.0f);
            const auto n = (int) sample->peaks.size();
            g.setColour (colour);
            for (int x = 0; x < (int) inner.getWidth(); ++x)
            {
                const auto peak = sample->peaks[(size_t) juce::jlimit (0, n - 1, x * n / juce::jmax (1, (int) inner.getWidth()))];
                const auto h = juce::jmax (1.0f, peak * inner.getHeight());
                g.fillRect (inner.getX() + (float) x, inner.getCentreY() - h * 0.5f, 1.0f, h);
            }
        }

    private:
        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

        class PadView final : public juce::Component, public juce::FileDragAndDropTarget
        {
        public:
            PadView (KitEditor& e, int i) : editor (e), index (i) {}

            void paint (juce::Graphics& g) override
            {
                const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
                const auto accent = editor.look.accent;
                const auto sample = editor.kit.getSample (index);
                const auto missing = sample == nullptr && editor.kit.getPadFile (index) != juce::File();

                g.setColour (juce::Colour (InstrumentLook::panel).interpolatedWith (accent, glow * 0.55f));
                g.fillRoundedRectangle (bounds, 9.0f);
                g.setColour (dragOver ? accent : editor.selected == index ? accent.withAlpha (0.95f) : juce::Colour (InstrumentLook::panelEdge));
                g.drawRoundedRectangle (bounds.reduced (0.5f), 9.0f, editor.selected == index || dragOver ? 2.0f : 1.0f);

                g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                g.setColour (juce::Colour (InstrumentLook::dimText));
                g.drawText (juce::String (index + 1) + "  " + juce::MidiMessage::getMidiNoteName (KitSampler::noteForPad (index), true, true, 4),
                            getLocalBounds().reduced (9, 6).removeFromTop (14), juce::Justification::centredLeft);

                if (sample != nullptr)
                {
                    paintWave (g, getLocalBounds().reduced (9, 0).withTrimmedTop (24).withTrimmedBottom (26), sample.get(), accent, index);
                    g.setColour (juce::Colour (InstrumentLook::text));
                    g.setFont (juce::Font (juce::FontOptions (12.0f)));
                    g.drawFittedText (sample->name, getLocalBounds().reduced (9, 6).removeFromBottom (16), juce::Justification::centredLeft, 1);
                }
                else
                {
                    g.setColour (missing ? juce::Colour (0xffe0735a) : juce::Colour (InstrumentLook::dimText).withAlpha (0.7f));
                    g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
                    g.drawText (missing ? "Not found" : KitSampler::hintName (index), getLocalBounds().withTrimmedTop (14), juce::Justification::centred);
                    g.setFont (juce::Font (juce::FontOptions (10.5f)));
                    g.drawText (missing ? editor.kit.getPadFile (index).getFileName() : "drop a sample", getLocalBounds().withTrimmedTop (48), juce::Justification::centred);
                }
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                editor.select (index);
                if (e.mods.isPopupMenu())
                {
                    showMenu();
                    return;
                }
                held = true;
                editor.processor.getKeyboardState().noteOn (1, KitSampler::noteForPad (index), 0.85f);
            }

            void mouseUp (const juce::MouseEvent&) override
            {
                if (held)
                    editor.processor.getKeyboardState().noteOff (1, KitSampler::noteForPad (index), 0.0f);
                held = false;
            }

            void mouseDoubleClick (const juce::MouseEvent&) override { editor.chooseFiles(); }

            bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
            void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
            void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
            void filesDropped (const juce::StringArray& files, int, int) override
            {
                dragOver = false;
                editor.loadFiles (files, index);
            }

            void tick()
            {
                const auto hit = editor.kit.takeFlash (index);
                const auto next = juce::jmax (hit, glow * 0.82f);
                const auto changed = std::abs (next - glow) > 0.01f;
                glow = next < 0.02f ? 0.0f : next;
                if (changed)
                    repaint();
            }

        private:
            void showMenu()
            {
                juce::PopupMenu menu;
                menu.addItem (1, "Load sample...");
                menu.addItem (2, "Clear pad", editor.kit.getPadFile (index) != juce::File());
                menu.showMenuAsync ({}, [this, safe = juce::Component::SafePointer<PadView> (this)] (int result)
                {
                    if (safe == nullptr)
                        return;
                    if (result == 1)
                        editor.chooseFiles();
                    else if (result == 2)
                    {
                        editor.kit.clearPad (index);
                        editor.refresh();
                    }
                });
            }

            KitEditor& editor;
            int index;
            float glow = 0.0f;
            bool held = false, dragOver = false;
        };

        void chooseFiles()
        {
            chooser = std::make_unique<juce::FileChooser> ("Load samples into the kit (the first on pad " + juce::String (selected + 1) + ")",
                                                           juce::File(), []
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                return formats.getWildcardForAllFormats();
            }());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
                                  [this, safe = juce::Component::SafePointer<KitEditor> (this)] (const juce::FileChooser& c)
            {
                if (safe == nullptr)
                    return;
                juce::StringArray paths;
                for (const auto& f : c.getResults())
                    paths.add (f.getFullPathName());
                if (! paths.isEmpty())
                    loadFiles (paths, selected);
            });
        }

        void timerCallback() override
        {
            for (auto& view : pads)
                view->tick();
        }

        InstrumentProcessor& processor;
        KitSampler& kit;
        InstrumentLook look;
        int selected = 0;
        juce::Rectangle<int> detailArea;
        juce::Rectangle<int> waveArea;
        std::array<std::unique_ptr<PadView>, (size_t) numPads> pads;

        juce::TextButton loadButton { "Load samples..." }, clearButton { "Clear pad" }, clearAllButton { "Clear all" };
        juce::Slider levelSlider, panSlider, tuneSlider, decaySlider, velocitySlider, roomSlider, volumeSlider;
        juce::ComboBox chokeBox;
        juce::Label levelLabel, panLabel, tuneLabel, decayLabel, chokeLabel, velocityLabel, roomLabel;
        std::unique_ptr<SliderAttachment> levelAttachment, panAttachment, tuneAttachment, decayAttachment, velocityAttachment,
                                          roomAttachment, volumeAttachment;
        std::unique_ptr<ComboBoxAttachment> chokeAttachment;
        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitEditor)
    };
}

//==============================================================================
juce::String KitSampler::hintName (int pad)
{
    static const char* names[numPads] = { "Kick", "Rim", "Snare", "Clap", "Snare 2", "Low tom", "Hat", "Tom", "Pedal hat",
                                          "Tom", "Open hat", "Tom", "Crash", "Tom", "Ride", "Ride 2" };
    return isPad (pad) ? juce::String (names[pad]) : juce::String();
}

KitSampler* asKit (Engine& engine) { return dynamic_cast<KitSampler*> (&engine); }

void setKitSearchFolders (const juce::Array<juce::File>& folders)
{
    const juce::ScopedLock lock (searchLock);
    searchFolders = folders;
}

const Spec& getKitSpec()
{
    static const Spec spec = []
    {
        // The names ParamSpec points to must live for good.
        static std::deque<std::string> names;
        Spec s;
        s.key = "kit";
        s.name = "Kit";
        s.tagline = "Sampler drum kit";
        s.category = "Drums";
        s.accent = juce::Colour (0xff4fc3b0);
        s.lowestKey = 36;
        s.highestKey = 51;

        const auto& ids = padIds();
        for (int i = 0; i < numPads; ++i)
        {
            const auto& id = ids[(size_t) i];
            names.push_back ("Pad " + std::to_string (i + 1) + " level");
            s.params.push_back (knob (id.level.c_str(), names.back().c_str(), -24.0f, 12.0f, 0.0f, Unit::decibels, -6.0f));
            names.push_back ("Pad " + std::to_string (i + 1) + " pan");
            s.params.push_back (knob (id.pan.c_str(), names.back().c_str(), -1.0f, 1.0f, 0.0f, Unit::bipolar));
            names.push_back ("Pad " + std::to_string (i + 1) + " tune");
            s.params.push_back (knob (id.tune.c_str(), names.back().c_str(), -12.0f, 12.0f, 0.0f, Unit::semitones, 0.0f, 0.1f));
            names.push_back ("Pad " + std::to_string (i + 1) + " decay");
            s.params.push_back (knob (id.decay.c_str(), names.back().c_str(), 0.02f, fullDecay, fullDecay, Unit::seconds, 0.6f));
            names.push_back ("Pad " + std::to_string (i + 1) + " choke");
            s.params.push_back (choice (id.choke.c_str(), names.back().c_str(), { "Off", "A", "B", "C", "D" }, defaultChoke (i)));
        }
        s.params.push_back (knob ("velocity", "Velocity", 0.0f, 1.0f, 0.7f));
        s.params.push_back (knob ("room", "Room", 0.0f, 1.0f, 0.0f));

        s.presets = { { "Empty Kit", {} } };
        s.keyName = [] (int note) { return KitSampler::hintName (KitSampler::padForNote (note)); };
        s.makeEngine = [] (Params p) { return std::make_unique<KitEngine> (p); };
        s.makeEditor = [] (InstrumentProcessor& p) -> juce::AudioProcessorEditor* { return new KitEditor (p); };
        return s;
    }();
    return spec;
}
}
