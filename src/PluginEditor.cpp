#include "PluginEditor.h"
#include "Presets.h"
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

        if (extension == "html")  return "text/html";
        if (extension == "css")   return "text/css";
        if (extension == "js")    return "text/javascript";
        if (extension == "json")  return "application/json";
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

    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (900, 560, 10000, 10000); // no real maximum, so full screen fills any display
    setSize (1120, 780);

    startTimerHz (30);
}

PluginEditor::~PluginEditor()
{
    daw.reset();

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
    status->setProperty ("preset", processorRef.currentPreset.load());

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
    juce::int64 latency = processorRef.getLatencySamples();

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
