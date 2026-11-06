#include "PluginEditor.h"
#include "Presets.h"
#include "WebUIData.h"

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

    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (900, 560, 1800, 1200);
    setSize (1120, 700);

    startTimerHz (30);
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

                       // -> [{ name, category }]
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

                           auto* result = new juce::DynamicObject();
                           result->setProperty ("ok", error.isEmpty());
                           result->setProperty ("error", error);
                           complete (juce::var (result));
                       })

                       // -> { file, dryFile, seconds, dropped } (the last take; file is "" if none)
                       .withNativeFunction ("stopRecording", [this] (const auto&, auto complete)
                       {
                           const auto take = processorRef.getRecorder().stop();

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

void PluginEditor::timerCallback()
{
    const auto& pitch = processorRef.getPitchEngine();

    auto* status = new juce::DynamicObject();
    status->setProperty ("input",  toDecibels (processorRef.inputPeak));
    status->setProperty ("output", toDecibels (processorRef.outputPeak));
    status->setProperty ("note",   pitch.detectedNote.load());
    status->setProperty ("target", pitch.tunedNote.load());
    status->setProperty ("preset", processorRef.currentPreset.load());

    const auto& recorder = processorRef.getRecorder();
    status->setProperty ("recording", recorder.isRecording() ? recorder.getSeconds() : -1.0);

    webView->emitEventIfBrowserIsVisible ("meters", juce::var (status));
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
