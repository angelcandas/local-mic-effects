#include "PluginEditor.h"
#include "Presets.h"
#include "analysis/Key.h"
#include "WebUIData.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace
{
    // Keeps the WebView on our bundled page; links can't navigate it elsewhere.
    struct SinglePageBrowser final : juce::WebBrowserComponent
    {
        using WebBrowserComponent::WebBrowserComponent;

        bool pageAboutToLoad (const juce::String& newURL) override
        {
            return newURL == getResourceProviderRoot();
        }
    };

    juce::String getMimeType (const juce::String& fileName)
    {
        const auto extension = fileName.fromLastOccurrenceOf (".", false, false).toLowerCase();

        if (extension == "html")  return "text/html; charset=utf-8";
        if (extension == "css")   return "text/css; charset=utf-8";
        if (extension == "js")    return "text/javascript; charset=utf-8";
        if (extension == "json")  return "application/json; charset=utf-8";
        if (extension == "svg")   return "image/svg+xml";
        if (extension == "png")   return "image/png";
        if (extension == "woff2") return "font/woff2";

        return "application/octet-stream";
    }

    juce::PropertiesFile::Options settingsOptions()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = "MicFX";
        options.filenameSuffix      = ".settings";
        options.folderName          = "MicFX";
        options.osxLibrarySubFolder = "Application Support";
        return options;
    }

    float toDecibels (std::atomic<float>& peak)
    {
        return juce::Decibels::gainToDecibels (peak.exchange (0.0f), -100.0f);
    }
}

//==============================================================================
// Defined here rather than in PluginProcessor.cpp so the processor builds without
// the WebView (the test runner uses it headless).
juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor (*this);
}

//==============================================================================
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), settings (settingsOptions())
{
    webView = std::make_unique<SinglePageBrowser> (createOptions());

    auto& multitrack = processorRef.getMultitrack();
    multitrack.onTracksChanged = [this] { webView->emitEventIfBrowserIsVisible ("tracks", juce::var()); };
    multitrack.onError = [this] (const juce::String& message) { webView->emitEventIfBrowserIsVisible ("trackError", message); };

    // Attach every relay created in createOptions() to its parameter.
    auto* undoManager = processorRef.getState().undoManager;
    size_t slider = 0, toggle = 0, combo = 0;

    for (auto* parameter : processorRef.getParameters())
    {
        auto& ranged = *dynamic_cast<juce::RangedAudioParameter*> (parameter);

        if (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr)
            toggleAttachments.push_back (std::make_unique<juce::WebToggleButtonParameterAttachment> (ranged, *toggleRelays[toggle++], undoManager));
        else if (dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr)
            comboAttachments.push_back (std::make_unique<juce::WebComboBoxParameterAttachment> (ranged, *comboRelays[combo++], undoManager));
        else
            sliderAttachments.push_back (std::make_unique<juce::WebSliderParameterAttachment> (ranged, *sliderRelays[slider++], undoManager));
    }

    daw = std::make_unique<DawBridge> (processorRef, settings,
                                       [this] (const juce::Identifier& event, const juce::var& payload)
                                       { webView->emitEventIfBrowserIsVisible (event, payload); },
                                       [this] { return getRoundTripLatency(); });

    practice = std::make_unique<Practice> (processorRef, settings,
                                           [this] (const juce::Identifier& event, const juce::var& payload)
                                           { webView->emitEventIfBrowserIsVisible (event, payload); },
                                           [this] { return getDeviceLatency(); });

   #if JucePlugin_Build_Standalone
    // The app's audio interface: its open inputs by name, for the tracks' input pickers.
    if (juce::JUCEApplicationBase::isStandaloneApp())
        daw->inputNames = []
        {
            juce::StringArray names;
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
                if (auto* device = holder->deviceManager.getCurrentAudioDevice())
                {
                    const auto all = device->getInputChannelNames();
                    const auto active = device->getActiveInputChannels();
                    for (int i = 0; i < all.size(); ++i)
                        if (active[i] && names.size() < PluginProcessor::maxAppInputs)
                            names.add (all[i]);
                }
            return names;
        };
   #endif

    // MIDI learn tells the page what it learned, and passes on the actions only the page
    // can do (record, scenes).
    {
        auto& map = processorRef.getMidiMap();
        map.onLearned = [this] (const MidiMap::Mapping& m)
        {
            auto* learned = new juce::DynamicObject();
            learned->setProperty ("target", m.target);
            learned->setProperty ("name", MidiMap::describe (m));
            learned->setProperty ("midi", describeMidi());
            webView->emitEventIfBrowserIsVisible ("midiLearned", juce::var (learned));
        };
        map.onAction = [this] (const juce::String& action) { webView->emitEventIfBrowserIsVisible ("midiAction", action); };
    }

    // The looper's tempo, lock and volume are app preferences, like the recording folder.
    {
        auto& looper = processorRef.getLooper();
        looper.setTempo (settings.getDoubleValue ("looper.bpm", 120.0), settings.getIntValue ("looper.beatsPerBar", 4));
        looper.setTempoLocked (settings.getBoolValue ("looper.locked", false));
        looper.setVolume ((float) settings.getDoubleValue ("looper.volume", 1.0));
    }

    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (900, 560, 10000, 10000); // no real maximum, so full screen fills any display
    setSize (1120, 780);

    startTimerHz (30);
}

PluginEditor::~PluginEditor()
{
    practice.reset();
    daw.reset();

    auto& map = processorRef.getMidiMap();
    map.onLearned = nullptr;
    map.onAction = nullptr;

    // The processor (and its tracks) outlive the editor.
    auto& multitrack = processorRef.getMultitrack();
    multitrack.onTracksChanged = nullptr;
    multitrack.onError = nullptr;
}

juce::WebBrowserComponent::Options PluginEditor::createOptions()
{
    auto options = juce::WebBrowserComponent::Options{}
                       .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                       .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                    .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)))
                       .withNativeIntegrationEnabled()
                       .withResourceProvider ([] (const auto& url) { return getResource (url); })

                       // -> { [parameterId]: defaultValue }
                       .withNativeFunction ("getParameterDefaults", [this] (const auto&, auto complete)
                       {
                           auto* defaults = new juce::DynamicObject();

                           for (auto* parameter : processorRef.getParameters())
                               if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                                   defaults->setProperty (ranged->getParameterID(),
                                                          ranged->convertFrom0to1 (ranged->getDefaultValue()));

                           complete (juce::var (defaults));
                       })

                       // -> { [parameterId]: { min, max, skew, step, label, default, choices?, bool? } }, for controls the
                       // DAW screen draws itself (track inserts aren't host parameters).
                       .withNativeFunction ("getParameterInfo", [this] (const auto&, auto complete)
                       {
                           auto* info = new juce::DynamicObject();

                           for (auto* parameter : processorRef.getParameters())
                           {
                               auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
                               if (ranged == nullptr)
                                   continue;

                               auto* entry = new juce::DynamicObject();
                               const auto& range = ranged->getNormalisableRange();
                               entry->setProperty ("min", range.start);
                               entry->setProperty ("max", range.end);
                               entry->setProperty ("skew", range.skew);
                               entry->setProperty ("step", range.interval);
                               entry->setProperty ("label", ranged->getLabel());
                               entry->setProperty ("default", ranged->convertFrom0to1 (ranged->getDefaultValue()));

                               if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
                               {
                                   juce::Array<juce::var> choices;
                                   for (const auto& c : choice->choices)
                                       choices.add (c);
                                   entry->setProperty ("choices", choices);
                               }

                               if (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr)
                                   entry->setProperty ("bool", true);

                               info->setProperty (ranged->getParameterID(), juce::var (entry));
                           }

                           complete (juce::var (info));
                       })

                       // (command, args) -> depends on the command; see DawBridge::handle
                       .withNativeFunction ("daw", [this] (const auto& args, auto complete)
                       {
                           if (daw == nullptr || args.size() == 0)
                           {
                               complete ({});
                               return;
                           }

                           daw->handle (args[0].toString(), args.size() > 1 ? args[1] : juce::var (new juce::DynamicObject()),
                                        std::move (complete));
                       })

                       // -> [{ name, category }]
                       .withNativeFunction ("window", [this] (const auto& args, auto complete)
                       {
                           if (args.size() > 0 && args[0].toString() == "fullScreen")
                               toggleFullScreen();

                           complete (describeWindow());
                       })
                       .withNativeFunction ("getPresets", [] (const auto&, auto complete)
                       {
                           juce::Array<juce::var> list;

                           for (const auto& preset : getPresets())
                           {
                               auto* entry = new juce::DynamicObject();
                               entry->setProperty ("name", preset.name);
                               entry->setProperty ("category", preset.category);
                               list.add (juce::var (entry));
                           }

                           complete (list);
                       })

                       // (action, options) -> see Practice::handle.
                       .withNativeFunction ("practice", [this] (const auto& args, auto complete)
                       {
                           if (practice == nullptr)
                               return complete ({});
                           complete (practice->handle (args.size() > 0 ? args[0].toString() : juce::String(),
                                                       args.size() > 1 ? args[1] : juce::var()));
                       })

                       // (action, on) -> { muted, dry }. "mute": the panic button; "dry": hear the mic without effects.
                       .withNativeFunction ("liveControl", [this] (const auto& args, auto complete)
                       {
                           const auto action = args.size() > 0 ? args[0].toString() : juce::String();
                           const auto on = args.size() > 1 && (bool) args[1];
                           if (action == "mute") processorRef.outputMuted = on;
                           if (action == "dry")  processorRef.dryCompare = on;

                           auto* state = new juce::DynamicObject();
                           state->setProperty ("muted", processorRef.outputMuted.load());
                           state->setProperty ("dry", processorRef.dryCompare.load());
                           complete (juce::var (state));
                       })

                       // (action, args) -> the looper's state (see describeLooper). Actions: "main" (record /
                       // overdub / end the overdub / play), "stop", "play", "undo", "redo", "clear", "status",
                       // "layer" { layer, gain?, muted? }, "settings" { bpm?, beatsPerBar?, locked?, volume? };
                       // "save" and "sendToDaw" answer with their own result (below).
                       .withNativeFunction ("looper", [this] (const auto& args, auto complete)
                       {
                           const auto action = args.size() > 0 ? args[0].toString() : juce::String();
                           const auto options = args.size() > 1 ? args[1] : juce::var();
                           auto& looper = processorRef.getLooper();
                           using Command = Looper::Command;

                           if (action == "main")  looper.send (Command::main);
                           if (action == "stop")  looper.send (Command::stop);
                           if (action == "play")  looper.send (Command::play);
                           if (action == "undo")  looper.send (Command::undo);
                           if (action == "redo")  looper.send (Command::redo);
                           if (action == "clear") looper.send (Command::clear);

                           if (action == "layer")
                           {
                               const auto layer = (int) options.getProperty ("layer", -1);
                               if (options.hasProperty ("gain"))  looper.setLayerGain (layer, (float) options["gain"]);
                               if (options.hasProperty ("muted")) looper.setLayerMuted (layer, (bool) options["muted"]);
                           }

                           // "save" -> { ok, error, files, folder }: every layer as a WAV in the recordings folder.
                           // "sendToDaw" { repeats } -> { ok, error, tracks }: also puts them on DAW tracks.
                           if (action == "save" || action == "sendToDaw")
                           {
                               const auto toDaw = action == "sendToDaw";
                               auto* result = new juce::DynamicObject();
                               juce::String error;

                               if (toDaw && (daw == nullptr || ! DawBridge::isAvailable()))
                                   error = "The DAW is only in the standalone app.";

                               const auto folder = getRecordingFolder();
                               const auto files = error.isEmpty()
                                                      ? looper.saveLayers (folder, "Loop " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), error)
                                                      : juce::Array<juce::File>();

                               if (error.isEmpty() && toDaw)
                               {
                                   std::vector<DawBridge::LoopLayer> layers;
                                   for (int i = 0; i < files.size(); ++i)
                                       layers.push_back ({ files[i], juce::Decibels::gainToDecibels (looper.getLayerGain (i), -60.0f), looper.isLayerMuted (i) });

                                   const auto added = daw->addLoopTracks (layers, (double) looper.getLength() / looper.getSampleRate(),
                                                                          (int) options.getProperty ("repeats", 8),
                                                                          looper.isTempoLocked() ? looper.getBpm() : 0.0, looper.getBeatsPerBar());
                                   error = added["error"].toString();
                                   result->setProperty ("tracks", added["tracks"]);
                               }

                               juce::Array<juce::var> paths;
                               for (const auto& file : files)
                                   paths.add (file.getFullPathName());

                               result->setProperty ("ok", error.isEmpty());
                               result->setProperty ("error", error);
                               result->setProperty ("files", paths);
                               result->setProperty ("folder", folder.getFullPathName());
                               complete (juce::var (result));
                               return;
                           }

                           if (action == "settings")
                           {
                               looper.setTempo (options.getProperty ("bpm", looper.getBpm()),
                                                options.getProperty ("beatsPerBar", looper.getBeatsPerBar()));
                               if (options.hasProperty ("locked")) looper.setTempoLocked ((bool) options["locked"]);
                               if (options.hasProperty ("volume")) looper.setVolume (juce::jlimit (0.0f, 2.0f, (float) options["volume"]));
                               saveLooperSettings();
                           }

                           complete (describeLooper (true));
                       })

                       // (action, args) -> { mappings: [{ source, channel, number, target, name }], learning, last }.
                       // Actions: "list", "learn" { target } (the next CC, note or program change is
                       // mapped to it; "midiLearned" follows), "cancel", "remove" { target }, "clear".
                       .withNativeFunction ("midi", [this] (const auto& args, auto complete)
                       {
                           const auto action = args.size() > 0 ? args[0].toString() : juce::String();
                           const auto options = args.size() > 1 ? args[1] : juce::var();
                           auto& map = processorRef.getMidiMap();

                           if (action == "learn")  map.learn (options["target"].toString());
                           if (action == "cancel") map.learn ({});
                           if (action == "remove") map.removeTarget (options["target"].toString());
                           if (action == "clear")  map.clear();

                           complete (describeMidi());
                       })

                       // (action, args) -> the scenes ({ scenes: [{ name }], active, modified, glide, gliding, max }).
                       // Actions: "list", "add" { name? }, "update" { index }, "rename" { index, name },
                       // "remove" { index }, "recall" { index }, "step" { delta }, "glide" { seconds }.
                       .withNativeFunction ("scenes", [this] (const auto& args, auto complete)
                       {
                           const auto action = args.size() > 0 ? args[0].toString() : juce::String();
                           const auto options = args.size() > 1 ? args[1] : juce::var();
                           auto& scenes = processorRef.getScenes();
                           const auto index = (int) options.getProperty ("index", -1);

                           if (action == "add")    scenes.add (options["name"].toString());
                           if (action == "update") scenes.update (index);
                           if (action == "rename") scenes.rename (index, options["name"].toString());
                           if (action == "remove") scenes.remove (index);
                           if (action == "recall") scenes.recall (index);
                           if (action == "step")   scenes.step ((int) options.getProperty ("delta", 1));
                           if (action == "glide")  scenes.setGlideSeconds ((double) options.getProperty ("seconds", 0.5));

                           complete (scenes.describe());
                       })

                       // (action, args) -> the setlist (see Setlist::describe) plus { available, recent, warning,
                       // error }. Actions: "list", "add" { name? }, "update" { index }, "remove" { index },
                       // "move" { index, to }, "info" { index, name?, key?, minor?, tempo?, lyrics?, notes? },
                       // "load" { index }, "step" { delta }, "new", "open", "openRecent" { file }, "save",
                       // "saveAs", "chooseBacking" { index }, "chooseLyrics" { index }. Standalone app only.
                       .withNativeFunction ("setlist", [this] (const auto& args, auto complete)
                       {
                           handleSetlist (args.size() > 0 ? args[0].toString() : juce::String(),
                                          args.size() > 1 ? args[1] : juce::var(), std::move (complete));
                       })

                       // (key) -> the stored string, or "" / (key, value) -> value. Small UI preferences
                       // (favourite presets) kept in the app's settings file.
                       .withNativeFunction ("uiPref", [this] (const auto& args, auto complete)
                       {
                           const auto key = "ui." + (args.size() > 0 ? args[0].toString().retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") : juce::String());
                           if (args.size() > 1)
                               settings.setValue (key, args[1].toString().substring (0, 20000));
                           complete (settings.getValue (key));
                       })

                       // (index) -> index
                       .withNativeFunction ("loadPreset", [this] (const auto& args, auto complete)
                       {
                           processorRef.setCurrentProgram ((int) args[0]);
                           complete (processorRef.getCurrentProgram());
                       })

                       // -> { folder, includeDry, recording }
                       .withNativeFunction ("getRecorderSettings", [this] (const auto&, auto complete)
                       {
                           complete (describeRecorderSettings());
                       })

                       // (includeDry) -> { ok, error }
                       .withNativeFunction ("startRecording", [this] (const auto& args, auto complete)
                       {
                           const auto includeDry = args.size() > 0 && (bool) args[0];
                           settings.setValue ("recordDry", includeDry);

                           const auto error = processorRef.getRecorder().start (getRecordingFolder(), includeDry);

                           // Recording rolls the transport so you sing along with the backing track.
                           if (error.isEmpty())
                               processorRef.getMultitrack().play();

                           auto* result = new juce::DynamicObject();
                           result->setProperty ("ok", error.isEmpty());
                           result->setProperty ("error", error);
                           complete (juce::var (result));
                       })

                       // -> { file, dryFile, seconds, dropped } (the last take; file is "" if none)
                       .withNativeFunction ("stopRecording", [this] (const auto&, auto complete)
                       {
                           const auto take = processorRef.getRecorder().stop();
                           auto& multitrack = processorRef.getMultitrack();
                           multitrack.pause();

                           // Each new take gets its own row, moved earlier by the audio round trip
                           // so it lines up with the music you heard while singing.
                           if (take.file.existsAsFile() && take.file != lastTakeOnTimeline && take.seconds > 0.0)
                           {
                               lastTakeOnTimeline = take.file;
                               multitrack.addTake (take.file, take.timelineStart - getRoundTripLatency());
                           }

                           auto* result = new juce::DynamicObject();
                           result->setProperty ("file", take.file.getFullPathName());
                           result->setProperty ("fileName", take.file.getFileName());
                           result->setProperty ("dryFile", take.dryFile.getFullPathName());
                           result->setProperty ("seconds", take.seconds);
                           result->setProperty ("dropped", take.droppedAudio);
                           complete (juce::var (result));
                       })

                       // -> settings as getRecorderSettings, after the user picks a folder (or cancels)
                       .withNativeFunction ("chooseRecordingFolder", [this] (const auto&, auto complete)
                       {
                           folderChooser = std::make_unique<juce::FileChooser> ("Save recordings in…", getRecordingFolder());
                           folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                                       [this, complete] (const juce::FileChooser& chooser)
                                                       {
                                                           if (const auto folder = chooser.getResult(); folder != juce::File{})
                                                               settings.setValue ("recordFolder", folder.getFullPathName());

                                                           complete (describeRecorderSettings());
                                                       });
                       })

                       // (includeDry) -> undefined
                       .withNativeFunction ("setRecordDry", [this] (const auto& args, auto complete)
                       {
                           settings.setValue ("recordDry", args.size() > 0 && (bool) args[0]);
                           complete ({});
                       })

                       // -> undefined; shows the last take in Finder (or the folder if there's none)
                       .withNativeFunction ("revealRecording", [this] (const auto&, auto complete)
                       {
                           const auto file = processorRef.getRecorder().getLastTake().file;
                           (file.existsAsFile() ? file : getRecordingFolder()).revealToUser();
                           complete ({});
                       })

                       // -> [{ id, name, isTake, start, length, gain, muted, peaks, peakSeconds }] (times in seconds)
                       .withNativeFunction ("getTracks", [this] (const auto&, auto complete)
                       {
                           complete (describeTracks());
                       })

                       // -> true if a file was picked; the track arrives with a "tracks" event
                       .withNativeFunction ("loadBackingTrack", [this] (const auto&, auto complete)
                       {
                           folderChooser = std::make_unique<juce::FileChooser> ("Load a backing track",
                                                                                juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                                                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a");
                           folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                       [this, complete] (const juce::FileChooser& chooser)
                                                       {
                                                           const auto file = chooser.getResult();

                                                           if (file.existsAsFile())
                                                               processorRef.getMultitrack().loadBackingTrack (file);

                                                           complete (file.existsAsFile());
                                                       });
                       })

                       // (id, { gain?, muted? }) -> undefined
                       .withNativeFunction ("setTrack", [this] (const auto& args, auto complete)
                       {
                           if (args.size() >= 2)
                           {
                               auto& multitrack = processorRef.getMultitrack();
                               const auto id = (int) args[0];

                               if (args[1].hasProperty ("gain"))  multitrack.setTrackGain (id, (float) args[1]["gain"]);
                               if (args[1].hasProperty ("muted")) multitrack.setTrackMuted (id, (bool) args[1]["muted"]);
                           }

                           complete ({});
                       })

                       // () -> { ok, error, key, minor, name, confidence }: the key of the backing
                       // track (the first one that isn't a take), found on a background thread.
                       .withNativeFunction ("detectBackingKey", [this] (const auto&, auto complete)
                       {
                           std::shared_ptr<const Multitrack::Track> backing;
                           for (const auto& track : processorRef.getMultitrack().getTracks())
                               if (! track->isTake) { backing = track; break; }

                           if (backing == nullptr)
                           {
                               auto* result = new juce::DynamicObject();
                               result->setProperty ("ok", false);
                               result->setProperty ("error", "Load a backing track first.");
                               complete (juce::var (result));
                               return;
                           }

                           const auto rate = processorRef.getMultitrack().getSampleRate();
                           juce::Component::SafePointer<PluginEditor> editor (this);

                           juce::Thread::launch ([backing, rate, editor, complete]
                           {
                               auto* result = new juce::DynamicObject();
                               const auto key = analysis::detectKey (backing->audio, rate,
                                                                     [editor] { return editor != nullptr; });
                               result->setProperty ("ok", key.isValid());
                               result->setProperty ("error", key.isValid() ? juce::String() : juce::String ("No clear key in the backing track."));
                               result->setProperty ("key", key.key);
                               result->setProperty ("minor", key.minor);
                               result->setProperty ("name", key.getName());
                               result->setProperty ("confidence", key.confidence);
                               result->setProperty ("track", backing->name);
                               juce::var payload (result);

                               juce::MessageManager::callAsync ([editor, complete, payload]
                               {
                                   if (editor != nullptr)
                                       complete (payload);
                               });
                           });
                       })

                       // (id) -> undefined
                       .withNativeFunction ("removeTrack", [this] (const auto& args, auto complete)
                       {
                           if (args.size() > 0)
                               processorRef.getMultitrack().removeTrack ((int) args[0]);

                           complete ({});
                       })

                       // ("play" | "pause" | "stop" | "seek", seconds?) -> undefined
                       .withNativeFunction ("transport", [this] (const auto& args, auto complete)
                       {
                           auto& multitrack = processorRef.getMultitrack();
                           const auto action = args.size() > 0 ? args[0].toString() : juce::String();

                           if (action == "play")
                           {
                               if (multitrack.getLength() > 0 && multitrack.getPosition() >= multitrack.getLength())
                                   multitrack.seek (0);
                               multitrack.play();
                           }
                           else if (action == "pause")
                           {
                               multitrack.pause();
                           }
                           else if (action == "stop")
                           {
                               multitrack.pause();
                               multitrack.seek (0);
                           }
                           else if (action == "seek" && args.size() > 1)
                           {
                               multitrack.seek ((juce::int64) ((double) args[1] * multitrack.getSampleRate()));
                           }

                           complete ({});
                       })

                       // (message) -> undefined; the UI reports uncaught JavaScript errors here.
                       .withNativeFunction ("logError", [] (const auto& args, auto complete)
                       {
                           juce::Logger::writeToLog ("[MicFX UI] " + args[0].toString());
                           complete ({});
                       });

    // One relay per parameter, named after its ID. Relays have to be registered
    // with the options before the WebView is created.
    for (auto* parameter : processorRef.getParameters())
    {
        const auto id = dynamic_cast<juce::RangedAudioParameter&> (*parameter).getParameterID();

        if (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr)
            options = options.withOptionsFrom (*toggleRelays.emplace_back (std::make_unique<juce::WebToggleButtonRelay> (id)));
        else if (dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr)
            options = options.withOptionsFrom (*comboRelays.emplace_back (std::make_unique<juce::WebComboBoxRelay> (id)));
        else
            options = options.withOptionsFrom (*sliderRelays.emplace_back (std::make_unique<juce::WebSliderRelay> (id)));
    }

    return options;
}

void PluginEditor::resized()
{
    webView->setBounds (getLocalBounds());
}

void PluginEditor::sendMenuCommand (const juce::String& action)
{
    webView->emitEventIfBrowserIsVisible ("appMenu", action);
}

juce::ResizableWindow* PluginEditor::getAppWindow() const
{
    if (processorRef.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return nullptr;

    return findParentComponentOfClass<juce::ResizableWindow>();
}

void PluginEditor::toggleFullScreen()
{
    if (auto* window = getAppWindow())
        window->setFullScreen (! window->isFullScreen());
}

juce::var PluginEditor::describeWindow() const
{
    auto* window = getAppWindow();
    auto* result = new juce::DynamicObject();
    result->setProperty ("available", window != nullptr);
    result->setProperty ("fullScreen", window != nullptr && window->isFullScreen());
    return juce::var (result);
}

void PluginEditor::timerCallback()
{
    if (daw != nullptr)
        daw->tick();
    if (practice != nullptr && ! processorRef.isDawMode())
        practice->tick();

    // The title bar button or the system can change full screen too; keep the page's button in step.
    if (auto* window = getAppWindow(); window != nullptr && window->isFullScreen() != lastFullScreen)
    {
        lastFullScreen = window->isFullScreen();
        webView->emitEventIfBrowserIsVisible ("window", describeWindow());
    }

    const auto& pitch = processorRef.getPitchEngine();

    auto* status = new juce::DynamicObject();
    status->setProperty ("input",  toDecibels (processorRef.inputPeak));
    status->setProperty ("output", toDecibels (processorRef.outputPeak));
    status->setProperty ("note",   pitch.detectedNote.load());
    status->setProperty ("target", pitch.tunedNote.load());

    // The harmonizer: the chord it follows ("" for none) and each voice's note (-1 = silent).
    status->setProperty ("chord", processorRef.getHarmonyChord().getName());
    juce::Array<juce::var> voices;
    for (const auto& note : pitch.voiceNotes)
        voices.add (note.load());
    status->setProperty ("voices", voices);
    status->setProperty ("preset", processorRef.currentPreset.load());

    // Live readings for the effect cards that are on, by module key (Live mode only).
    if (! processorRef.isDawMode())
    {
        auto* effects = new juce::DynamicObject();
        auto& chain = processorRef.getLiveChain();

        for (int i = 0; i < chain.size(); ++i)
            if (auto meter = fx::describeMeter (chain[i], spectrumAnalyser); ! meter.isVoid())
                effects->setProperty (fx::getModuleKey (chain[i].getType()), meter);

        status->setProperty ("fx", juce::var (effects));
    }

    // The scenes (which is active, and whether the sound has moved away from it), 5 times a second.
    if (! processorRef.isDawMode() && looperTicks % 6 == 3)
        status->setProperty ("scenes", processorRef.getScenes().describe());

    // The looper, with its layers' waveforms 5 times a second while it has a loop.
    if (! processorRef.isDawMode())
        status->setProperty ("looper", describeLooper (++looperTicks % 6 == 0
                                                       && processorRef.getLooper().getState() != Looper::State::empty));

    // MIDI: the last message (for the activity light) and the panic mute, which a pedal can flip.
    auto* midi = new juce::DynamicObject();
    midi->setProperty ("count", processorRef.getMidiMap().getEventCount());
    midi->setProperty ("last", processorRef.getMidiMap().getLastEvent());
    midi->setProperty ("learning", processorRef.getMidiMap().getLearnTarget());
    status->setProperty ("midi", juce::var (midi));
    status->setProperty ("muted", processorRef.outputMuted.load());

    const auto& recorder = processorRef.getRecorder();
    status->setProperty ("recording", recorder.isRecording() ? recorder.getSeconds() : -1.0);

    auto& multitrack = processorRef.getMultitrack();
    const auto rate = multitrack.getSampleRate();

    // Playback stops at the end of the song, unless you're still recording.
    if (multitrack.isPlaying() && ! recorder.isRecording()
        && multitrack.getLength() > 0 && multitrack.getPosition() >= multitrack.getLength())
        multitrack.pause();

    auto* transport = new juce::DynamicObject();
    transport->setProperty ("position", (double) multitrack.getPosition() / rate);
    transport->setProperty ("length", (double) multitrack.getLength() / rate);
    transport->setProperty ("playing", multitrack.isPlaying());
    transport->setProperty ("loading", multitrack.getNumLoading());
    status->setProperty ("transport", juce::var (transport));

    webView->emitEventIfBrowserIsVisible ("meters", juce::var (status));
}

juce::var PluginEditor::describeLooper (bool withOverview)
{
    const auto& looper = processorRef.getLooper();
    const auto rate = looper.getSampleRate();
    const auto layers = looper.getNumLayers();

    auto* result = new juce::DynamicObject();
    result->setProperty ("state", Looper::getStateName (looper.getState()));
    result->setProperty ("layers", layers);
    result->setProperty ("redo", looper.getNumRedoLayers());
    result->setProperty ("maxLayers", Looper::maxLayers);
    result->setProperty ("length", (double) looper.getLength() / rate);
    result->setProperty ("position", (double) looper.getPosition() / rate);
    result->setProperty ("closing", (double) looper.getClosingTarget() / rate);
    result->setProperty ("maxSeconds", Looper::maxSeconds);
    result->setProperty ("bpm", looper.getBpm());
    result->setProperty ("beatsPerBar", looper.getBeatsPerBar());
    result->setProperty ("locked", looper.isTempoLocked());
    result->setProperty ("volume", (double) looper.getVolume());

    juce::Array<juce::var> gains, mutes;
    for (int i = 0; i < Looper::maxLayers; ++i)
    {
        gains.add ((double) looper.getLayerGain (i));
        mutes.add (looper.isLayerMuted (i));
    }
    result->setProperty ("gains", gains);
    result->setProperty ("mutes", mutes);

    if (withOverview)
    {
        juce::Array<juce::var> overviews;
        for (int i = 0; i < layers; ++i)
        {
            juce::Array<juce::var> peaks;
            for (auto peak : looper.getOverview (i, 96))
                peaks.add (std::round (peak * 1000.0f) / 1000.0f);
            overviews.add (peaks);
        }
        result->setProperty ("overviews", overviews);
    }

    return juce::var (result);
}

juce::var PluginEditor::describeSetlist (const juce::String& warning, const juce::String& error)
{
    auto result = processorRef.getSetlist().describe();
    auto* object = result.getDynamicObject();
    object->setProperty ("available", juce::JUCEApplicationBase::isStandaloneApp());
    object->setProperty ("warning", warning);
    object->setProperty ("error", error);

    juce::Array<juce::var> recent;
    for (const auto& path : juce::StringArray::fromLines (settings.getValue ("setlist.recent")))
        if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
            recent.add (path);
    object->setProperty ("recent", recent);
    return result;
}

void PluginEditor::rememberSetlist (const juce::File& file)
{
    auto recent = juce::StringArray::fromLines (settings.getValue ("setlist.recent"));
    recent.removeString (file.getFullPathName());
    recent.insert (0, file.getFullPathName());
    recent.removeEmptyStrings();
    while (recent.size() > 8)
        recent.remove (recent.size() - 1);
    settings.setValue ("setlist.recent", recent.joinIntoString ("\n"));
}

void PluginEditor::handleSetlist (const juce::String& action, const juce::var& options, std::function<void (juce::var)> complete)
{
    auto& setlist = processorRef.getSetlist();
    const auto index = (int) options.getProperty ("index", -1);
    juce::String warning, error;

    if (! juce::JUCEApplicationBase::isStandaloneApp())
    {
        complete (describeSetlist ({}, "Setlists are in the standalone app."));
        return;
    }

    using Flags = juce::FileBrowserComponent::FileChooserFlags;
    const auto choose = [this, complete] (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                                          std::function<juce::String (const juce::File&)> onPicked)
    {
        folderChooser = std::make_unique<juce::FileChooser> (title, start, patterns);
        folderChooser->launchAsync (flags, [this, complete, onPicked] (const juce::FileChooser& chooser)
        {
            const auto picked = chooser.getResult();
            const auto message = picked != juce::File() ? onPicked (picked) : juce::String();
            complete (describeSetlist ({}, message));
        });
    };

    const auto defaultFolder = setlist.getFile() != juce::File() ? setlist.getFile().getParentDirectory()
                                                                 : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    if (action == "add")    setlist.addSong (options["name"].toString());
    if (action == "update") setlist.updateSong (index);
    if (action == "remove") setlist.removeSong (index);
    if (action == "move")   setlist.moveSong (index, (int) options.getProperty ("to", index));
    if (action == "info")   setlist.setSongInfo (index, options);
    // A song change replaces the backing track, so it waits while a take is recording.
    if ((action == "load" || action == "step") && processorRef.getRecorder().isRecording())
    {
        complete (describeSetlist ({}, "Stop recording before changing songs."));
        return;
    }

    if (action == "load")   setlist.loadSong (index, warning);
    if (action == "step")   setlist.step ((int) options.getProperty ("delta", 1), warning);
    if (action == "new")    setlist.clear();

    if (action == "openRecent")
    {
        const juce::File file (options["file"].toString());
        error = setlist.open (file);
        if (error.isEmpty())
            rememberSetlist (file);
    }

    if (action == "save" && setlist.getFile() != juce::File())
    {
        error = setlist.save (setlist.getFile());
    }
    else if (action == "save" || action == "saveAs")
    {
        choose ("Save the setlist", defaultFolder.getChildFile (setlist.getName() + ".micfxset"), "*.micfxset",
                Flags::saveMode | Flags::canSelectFiles | Flags::warnAboutOverwriting,
                [this] (const juce::File& picked)
                {
                    const auto file = picked.withFileExtension ("micfxset");
                    const auto message = processorRef.getSetlist().save (file);
                    if (message.isEmpty())
                        rememberSetlist (file);
                    return message;
                });
        return;
    }

    if (action == "open")
    {
        choose ("Open a setlist", defaultFolder, "*.micfxset", Flags::openMode | Flags::canSelectFiles,
                [this] (const juce::File& picked)
                {
                    const auto message = processorRef.getSetlist().open (picked);
                    if (message.isEmpty())
                        rememberSetlist (picked);
                    return message;
                });
        return;
    }

    if (action == "chooseBacking")
    {
        choose ("The song's backing track", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a", Flags::openMode | Flags::canSelectFiles,
                [this, index] (const juce::File& picked)
                {
                    auto* fields = new juce::DynamicObject();
                    fields->setProperty ("backing", picked.getFullPathName());
                    processorRef.getSetlist().setSongInfo (index, juce::var (fields));
                    return juce::String();
                });
        return;
    }

    if (action == "chooseLyrics")
    {
        choose ("The song's lyrics (plain text, or .lrc with times)", defaultFolder, "*.txt;*.lrc",
                Flags::openMode | Flags::canSelectFiles,
                [this, index] (const juce::File& picked)
                {
                    if (picked.getSize() > 200000)
                        return juce::String ("That file is too big for lyrics.");
                    auto* fields = new juce::DynamicObject();
                    fields->setProperty ("lyrics", picked.loadFileAsString());
                    processorRef.getSetlist().setSongInfo (index, juce::var (fields));
                    return juce::String();
                });
        return;
    }

    complete (describeSetlist (warning, error));
}

juce::var PluginEditor::describeMidi()
{
    const auto& map = processorRef.getMidiMap();
    auto* result = new juce::DynamicObject();
    result->setProperty ("mappings", map.toVar());
    result->setProperty ("learning", map.getLearnTarget());
    result->setProperty ("last", map.getLastEvent());
    return juce::var (result);
}

void PluginEditor::saveLooperSettings()
{
    const auto& looper = processorRef.getLooper();
    settings.setValue ("looper.bpm", looper.getBpm());
    settings.setValue ("looper.beatsPerBar", looper.getBeatsPerBar());
    settings.setValue ("looper.locked", looper.isTempoLocked());
    settings.setValue ("looper.volume", (double) looper.getVolume());
}

juce::var PluginEditor::describeTracks()
{
    const auto& multitrack = processorRef.getMultitrack();
    const auto rate = multitrack.getSampleRate();
    juce::Array<juce::var> list;

    for (const auto& track : multitrack.getTracks())
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("id", track->id);
        entry->setProperty ("name", track->name);
        entry->setProperty ("file", track->file.getFullPathName());
        entry->setProperty ("isTake", track->isTake);
        entry->setProperty ("start", (double) track->start / rate);
        entry->setProperty ("length", (double) track->audio.getNumSamples() / rate);
        entry->setProperty ("gain", track->gain.load());
        entry->setProperty ("muted", track->muted.load());
        entry->setProperty ("peakSeconds", Multitrack::peakSeconds);

        juce::Array<juce::var> peaks;
        peaks.ensureStorageAllocated (track->peaks.size());
        for (auto peak : track->peaks)
            peaks.add (std::round (peak * 1000.0f) / 1000.0f); // keeps the JSON small
        entry->setProperty ("peaks", peaks);

        list.add (juce::var (entry));
    }

    return list;
}

juce::int64 PluginEditor::getRoundTripLatency()
{
    return processorRef.getLatencySamples() + getDeviceLatency();
}

juce::int64 PluginEditor::getDeviceLatency()
{
    juce::int64 latency = 0;

   #if JucePlugin_Build_Standalone
    // In the app, add the audio interface's own input + output latency. In a DAW the
    // host compensates for its side.
    if (juce::JUCEApplicationBase::isStandaloneApp())
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            if (auto* device = holder->deviceManager.getCurrentAudioDevice())
                latency += device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
   #endif

    return latency;
}

juce::File PluginEditor::getRecordingFolder()
{
    const auto saved = settings.getValue ("recordFolder");
    return juce::File::isAbsolutePath (saved) ? juce::File (saved) : Recorder::getDefaultFolder();
}

juce::var PluginEditor::describeRecorderSettings()
{
    auto* result = new juce::DynamicObject();
    result->setProperty ("folder", getRecordingFolder().getFullPathName());
    result->setProperty ("includeDry", settings.getBoolValue ("recordDry", false));
    result->setProperty ("recording", processorRef.getRecorder().isRecording());
    return juce::var (result);
}

//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> PluginEditor::getResource (const juce::String& url)
{
    // Embedded files are looked up by file name only (juce_add_binary_data flattens
    // folders), so "/juce/index.js" resolves to JUCE's bundled frontend library.
    const auto path = url == "/" ? juce::String ("index.html")
                                 : url.fromFirstOccurrenceOf ("/", false, false);
    const auto fileName = path.fromLastOccurrenceOf ("/", false, false);

   #if defined (MICFX_UI_SOURCE_DIR)
    // Debug builds read the UI straight from the source tree: after editing ui/,
    // right-click the UI -> Reload. No rebuild (or new mic permission prompt) needed.
    const auto file = path == "juce/index.js" ? juce::File (MICFX_JUCE_JS_FILE)
                                              : juce::File (MICFX_UI_SOURCE_DIR).getChildFile (path);

    if (juce::MemoryBlock contents; file.existsAsFile() && file.loadFileAsData (contents))
    {
        std::vector<std::byte> bytes (contents.getSize());
        std::memcpy (bytes.data(), contents.getData(), contents.getSize());
        return juce::WebBrowserComponent::Resource { std::move (bytes), getMimeType (fileName) };
    }
   #endif

    for (int i = 0; i < WebUI::namedResourceListSize; ++i)
    {
        if (fileName != WebUI::originalFilenames[i])
            continue;

        int size = 0;
        const auto* data = WebUI::getNamedResource (WebUI::namedResourceList[i], size);

        std::vector<std::byte> bytes ((size_t) size);
        std::memcpy (bytes.data(), data, (size_t) size);

        return juce::WebBrowserComponent::Resource { std::move (bytes), getMimeType (fileName) };
    }

    return std::nullopt;
}
