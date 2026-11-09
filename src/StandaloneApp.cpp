// The standalone app: JUCE's default StandaloneFilterApp, plus "Check for updates..." in the
// window's Options menu. Built with JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP (see CMakeLists.txt).

#include <juce_audio_utils/juce_audio_utils.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <unistd.h>

namespace
{
    juce::File getUpdateScript()
    {
       #ifdef MICFX_SOURCE_DIR
        return juce::File (MICFX_SOURCE_DIR).getChildFile ("scripts/update.sh");
       #else
        return {};
       #endif
    }

    //==============================================================================
    // Runs scripts/update.sh on a background thread and shows its output.
    class UpdateComponent final : public juce::Component,
                                  private juce::Thread
    {
    public:
        UpdateComponent() : juce::Thread ("MicFX updater")
        {
            log.setMultiLine (true);
            log.setReadOnly (true);
            log.setCaretVisible (false);
            log.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
            addAndMakeVisible (status);
            addAndMakeVisible (log);
            addAndMakeVisible (actionButton);
            addAndMakeVisible (closeButton);

            closeButton.setButtonText ("Close");
            closeButton.onClick = [this] { close(); };
            actionButton.onClick = [this] { runAction(); };

            setSize (560, 360);

            if (! script.existsAsFile())
            {
                setState (State::unavailable,
                          "Updates only work when MicFX was built from a git checkout of the source. "
                          "Couldn't find " + script.getFullPathName() + ".");
                return;
            }

            startCommand ("check");
        }

        ~UpdateComponent() override
        {
            signalThreadShouldExit();
            process.kill();
            stopThread (3000);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12);
            auto buttons = area.removeFromBottom (28);
            closeButton.setBounds (buttons.removeFromRight (100));
            buttons.removeFromRight (8);
            actionButton.setBounds (buttons.removeFromRight (170));
            status.setBounds (area.removeFromTop (40));
            area.removeFromTop (6);
            log.setBounds (area.withTrimmedBottom (10));
        }

    private:
        enum class State { checking, upToDate, available, blocked, installing, installed, failed, unavailable };

        void setState (State newState, const juce::String& message)
        {
            state = newState;
            status.setText (message, juce::dontSendNotification);

            const auto showAction = state == State::available || state == State::installed;
            actionButton.setVisible (showAction);
            actionButton.setButtonText (state == State::installed ? "Relaunch MicFX" : "Update and rebuild");
            closeButton.setButtonText (state == State::installing ? "Cancel" : "Close");
        }

        void startCommand (const juce::String& command)
        {
            jassert (! isThreadRunning());
            pendingCommand = command;

            if (command == "check")
                setState (State::checking, "Checking for updates...");
            else
                setState (State::installing, "Updating and rebuilding MicFX. This takes a few minutes...");

            log.clear();
            startThread();
        }

        void runAction()
        {
            if (state == State::available)
                startCommand ("install");
            else if (state == State::installed)
                relaunch();
        }

        void relaunch()
        {
            const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
            juce::ChildProcess relauncher;

            if (relauncher.start (juce::StringArray { "/bin/bash", script.getFullPathName(), "relaunch",
                                                      juce::String (::getpid()), app.getFullPathName() }))
                relauncher.waitForProcessToFinish (5000);

            // Quit after this click has been handled: quitting closes (and deletes) this dialog.
            juce::MessageManager::callAsync ([]
            {
                if (auto* instance = juce::JUCEApplicationBase::getInstance())
                    instance->systemRequestedQuit();
            });
        }

        void close()
        {
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                window->exitModalState (0);
        }

        void run() override
        {
            const auto command = pendingCommand;

            if (! process.start (juce::StringArray { "/bin/bash", script.getFullPathName(), command }))
            {
                finished (command, 1);
                return;
            }

            char buffer[1024];

            while (! threadShouldExit())
            {
                const auto numRead = process.readProcessOutput (buffer, sizeof (buffer));

                if (numRead <= 0)
                    break;

                appendToLog (juce::String::fromUTF8 (buffer, numRead));
            }

            if (threadShouldExit())
                return;

            process.waitForProcessToFinish (-1);
            finished (command, (int) process.getExitCode());
        }

        void appendToLog (const juce::String& text)
        {
            juce::MessageManager::callAsync ([safe = SafePointer<UpdateComponent> (this), text]
            {
                if (safe == nullptr)
                    return;

                safe->log.moveCaretToEnd();
                safe->log.insertTextAtCaret (text);
            });
        }

        void finished (const juce::String& command, int exitCode)
        {
            juce::MessageManager::callAsync ([safe = SafePointer<UpdateComponent> (this), command, exitCode]
            {
                if (safe == nullptr)
                    return;

                if (command == "check")
                {
                    if (exitCode == 0)        safe->setState (State::upToDate, "MicFX is up to date.");
                    else if (exitCode == 10)  safe->setState (State::available, "A new version of MicFX is available.");
                    else if (exitCode == 11)  safe->setState (State::blocked, "A new version is available, but this checkout can't be updated automatically. See below.");
                    else                      safe->setState (State::failed, "Couldn't check for updates. See below.");
                }
                else
                {
                    if (exitCode == 0)
                        safe->setState (State::installed, "MicFX was rebuilt. Relaunch it to use the new version. "
                                                          "macOS will ask for microphone access again.");
                    else
                        safe->setState (State::failed, "The update didn't finish. See the log below.");
                }
            });
        }

        const juce::File script = getUpdateScript();
        juce::ChildProcess process;
        juce::String pendingCommand;
        State state = State::checking;

        juce::Label status;
        juce::TextEditor log;
        juce::TextButton actionButton, closeButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateComponent)
    };

    void showUpdateDialog (juce::Component* parent)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new UpdateComponent());
        options.dialogTitle = "Check for updates";
        options.dialogBackgroundColour = juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
        options.componentToCentreAround = parent;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.launchAsync();
    }

    //==============================================================================
    // JUCE's standalone window with one more item in its Options menu.
    class MicFXWindow final : public juce::StandaloneFilterWindow
    {
    public:
        using StandaloneFilterWindow::StandaloneFilterWindow;

    private:
        static constexpr int checkForUpdatesId = 100;

        void buttonClicked (juce::Button* button) override
        {
            juce::PopupMenu m;
            m.addItem (1, TRANS ("Audio/MIDI Settings..."));
            m.addSeparator();
            m.addItem (2, TRANS ("Save current state..."));
            m.addItem (3, TRANS ("Load a saved state..."));
            m.addSeparator();
            m.addItem (4, TRANS ("Reset to default state"));
            m.addSeparator();
            m.addItem (checkForUpdatesId, TRANS ("Check for updates..."));

            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (button),
                             [safe = SafePointer<MicFXWindow> (this)] (int result)
                             {
                                 if (safe == nullptr || result == 0)
                                     return;

                                 if (result == checkForUpdatesId)
                                     showUpdateDialog (safe.getComponent());
                                 else
                                     safe->handleMenuResult (result);
                             });
        }
    };

    //==============================================================================
    // Same as JUCE's StandaloneFilterApp (juce_audio_plugin_client_Standalone.cpp), so
    // the audio device settings stay where they were.
    class MicFXApp final : public juce::JUCEApplication
    {
    public:
        MicFXApp()
        {
            juce::PropertiesFile::Options options;
            options.applicationName     = juce::CharPointer_UTF8 (JucePlugin_Name);
            options.filenameSuffix      = ".settings";
            options.osxLibrarySubFolder = "Application Support";
           #if JUCE_LINUX || JUCE_BSD
            options.folderName          = "~/.config";
           #else
            options.folderName          = "";
           #endif

            appProperties.setStorageParameters (options);
        }

        const juce::String getApplicationName() override           { return juce::CharPointer_UTF8 (JucePlugin_Name); }
        const juce::String getApplicationVersion() override        { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override                 { return true; }
        void anotherInstanceStarted (const juce::String&) override {}

        void initialise (const juce::String&) override
        {
            if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
            {
                pluginHolder = createPluginHolder();
                return;
            }

            mainWindow = std::make_unique<MicFXWindow> (getApplicationName(),
                                                        juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                                                        createPluginHolder());
            mainWindow->setVisible (true);
        }

        void shutdown() override
        {
            pluginHolder = nullptr;
            mainWindow = nullptr;
            appProperties.saveIfNeeded();
        }

        void systemRequestedQuit() override
        {
            if (pluginHolder != nullptr)
                pluginHolder->savePluginState();

            if (mainWindow != nullptr)
                mainWindow->pluginHolder->savePluginState();

            if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            {
                juce::Timer::callAfterDelay (100, []
                {
                    if (auto* app = juce::JUCEApplicationBase::getInstance())
                        app->systemRequestedQuit();
                });
            }
            else
            {
                quit();
            }
        }

    private:
        std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
        {
            return std::make_unique<juce::StandalonePluginHolder> (appProperties.getUserSettings(),
                                                                   false,
                                                                   juce::String{},
                                                                   nullptr,
                                                                   juce::Array<juce::StandalonePluginHolder::PluginInOuts>{},
                                                                   false);
        }

        juce::ApplicationProperties appProperties;
        std::unique_ptr<MicFXWindow> mainWindow;
        std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new MicFXApp();
}

#endif
