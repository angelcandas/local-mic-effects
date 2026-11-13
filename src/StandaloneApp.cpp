// The standalone app: JUCE's default StandaloneFilterApp, plus "Check for updates..." in the
// window's Options menu, a maximise button, and a File / Edit / Audio / View / Transport menu bar (the
// macOS menu bar at the top of the screen, or a bar under the title on Windows and Linux).
// Built with JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP (see CMakeLists.txt).

#include <juce_audio_utils/juce_audio_utils.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <unistd.h>
#include <memory>
#include <set>

#include "DeviceFavourites.h"
#include "PluginEditor.h"

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
    // The menu bar. Most items are handed to the page (PluginEditor::sendMenuCommand), which
    // runs them exactly like its buttons and keyboard shortcuts, so the two never disagree.
    // The shortcuts are shown as text only: the page keeps handling the keys (on macOS, Ctrl
    // in the page's shortcuts is also Command).
    struct MenuItem
    {
        const char* action;   // "" = separator
        const char* name;
        const char* keys;
        bool dawOnly;
    };

    const std::vector<std::pair<const char*, std::vector<MenuItem>>>& getMenus()
    {
        static const std::vector<std::pair<const char*, std::vector<MenuItem>>> menus
        {
            { "File", {
                { "new",       "New project",            "Ctrl+N",       true },
                { "open",      "Open project...",        "Ctrl+O",       true },
                { "save",      "Save",                   "Ctrl+S",       true },
                { "saveAs",    "Save as...",             "Ctrl+Shift+S", true },
                { "", "", "", false },
                { "import",    "Import audio...",        "Ctrl+I",       true },
                { "export",    "Export...",              "Ctrl+Shift+R", true },
                { "", "", "", false },
                { "app:audio", "Audio settings...",      "",             false },
                { "app:favourites", "Audio device favourites...", "",    false },
               #if ! JUCE_MAC
                { "app:updates", "Check for updates...", "",             false },
                { "", "", "", false },
                { "app:quit",  "Exit",                   "",             false },
               #endif
            } },
            { "Edit", {
                { "undo",      "Undo",                   "Ctrl+Z",       true },
                { "redo",      "Redo",                   "Ctrl+Shift+Z", true },
                { "", "", "", false },
                { "cut",       "Cut clip",               "Ctrl+X",       true },
                { "copy",      "Copy clip",              "Ctrl+C",       true },
                { "paste",     "Paste at the playhead",  "Ctrl+V",       true },
                { "duplicate", "Duplicate clip",         "Ctrl+D",       true },
                { "split",     "Split at the playhead",  "Ctrl+E",       true },
                { "delete",    "Delete clip",            "Delete",       true },
                { "", "", "", false },
                { "addTrack",  "New track",              "Ctrl+T",       true },
                { "duplicateTrack", "Duplicate track",   "Ctrl+Shift+D", true },
                { "renameTrack", "Rename track",         "Ctrl+R",       true },
                { "deleteTrack", "Delete track",         "Ctrl+Delete",  true },
                { "quantize",  "Quantize",               "Q",            true },
                { "", "", "", false },
                { "autoMix",   "Auto Mix...",            "",             true },
                { "songKey",   "Song key...",            "",             true },
            } },
            // Offline processes that change a clip's audio (not live effects or settings).
            { "Audio", {
                { "normaliseClip",      "Normalise clip",                     "",  true },
                { "", "", "", false },
                { "stripSilenceQuiet",  "Strip silence: quiet room (-55 dB)", "",  true },
                { "stripSilenceNormal", "Strip silence: normal (-45 dB)",     "",  true },
                { "stripSilenceNoisy",  "Strip silence: noisy (-35 dB)",      "",  true },
                { "", "", "", false },
                { "splitStems",         "Split into stems...",                "",  true },
            } },
            { "View", {
                { "mode:live", "Live",                   "",             false },
                { "mode:daw",  "Multitrack (DAW)",       "",             false },
                { "", "", "", false },
                { "toggleView", "Tracks / mixer",        "Tab",          true },
                { "toggleDock", "Effects panel",         "E",            true },
                { "toggleLibrary", "Library",            "Shift+B",      true },
                { "follow",    "Follow the playhead",    "F",            true },
                { "", "", "", false },
                { "zoomIn",    "Zoom in",                "+",            true },
                { "zoomOut",   "Zoom out",               "-",            true },
                { "taller",    "Taller tracks",          "Shift+Up",     true },
                { "shorter",   "Shorter tracks",         "Shift+Down",   true },
                { "", "", "", false },
               #if JUCE_MAC
                { "app:fullScreen", "Full screen",       "Ctrl+Cmd+F",   false },
               #else
                { "app:fullScreen", "Full screen",       "F11",          false },
               #endif
                { "shortcuts", "Keyboard shortcuts",     "?",            true },
            } },
            { "Transport", {
                { "play",      "Play / pause",           "Space",        true },
                { "stop",      "Stop",                   "Num 0",        true },
                { "record",    "Record",                 "R",            true },
                { "", "", "", false },
                { "start",     "Go to the start",        "Home",         true },
                { "end",       "Go to the end",          "End",          true },
                { "", "", "", false },
                { "loop",      "Loop range",             "L",            true },
                { "metronome", "Click (metronome)",      "C",            true },
            } },
        };
        return menus;
    }


    //==============================================================================
    // Remembers the audio devices in use (with sample rate, buffer size and bit depth), keeps them in
    // an ordered favourites list, and when a device is plugged in or pulled out moves to the best
    // favourite that is connected, using the settings it had last time.
    class DeviceFavouritesManager final : private juce::ChangeListener
    {
    public:
        DeviceFavouritesManager (juce::AudioDeviceManager& dm, juce::PropertySet* store)
            : manager (dm), settings (store)
        {
            if (settings != nullptr)
                favourites = devfav::Favourites::fromString (settings->getValue ("deviceFavourites"));

            manager.addChangeListener (this);
            juce::MessageManager::callAsync ([this, alive = std::weak_ptr<int> (token)]
                                             { if (! alive.expired()) update(); });
        }

        ~DeviceFavouritesManager() override { manager.removeChangeListener (this); }

        devfav::Favourites& get() { return favourites; }

        void changed()
        {
            if (settings != nullptr)
                settings->setValue ("deviceFavourites", favourites.toString());
        }

        void showDialog();

    private:
        std::shared_ptr<int> token = std::make_shared<int> (0);

        void changeListenerCallback (juce::ChangeBroadcaster*) override { update(); }

        devfav::Entry currentEntry() const
        {
            devfav::Entry e;
            if (auto* device = manager.getCurrentAudioDevice())
            {
                const auto setup = manager.getAudioDeviceSetup();
                e.type = device->getTypeName();
                e.output = setup.outputDeviceName;
                e.input = setup.inputDeviceName;
                e.sampleRate = device->getCurrentSampleRate();
                e.bufferSize = device->getCurrentBufferSizeSamples();
                e.bitDepth = device->getCurrentBitDepth();
            }
            return e;
        }

        bool isAvailable (const devfav::Entry& e) const
        {
            for (auto* type : manager.getAvailableDeviceTypes())
            {
                if (type->getTypeName() != e.type)
                    continue;

                return (e.output.isEmpty() || type->getDeviceNames (false).contains (e.output))
                    && (e.input.isEmpty()  || type->getDeviceNames (true).contains (e.input));
            }
            return false;
        }

        static juce::String key (const devfav::Entry& e) { return e.type + "|" + e.output + "|" + e.input; }

        bool switchTo (const devfav::Entry& e)
        {
            if (manager.getCurrentAudioDeviceType() != e.type)
                manager.setCurrentAudioDeviceType (e.type, true);

            auto setup = manager.getAudioDeviceSetup();
            setup.outputDeviceName = e.output;
            setup.inputDeviceName = e.input;
            setup.sampleRate = e.sampleRate;
            setup.bufferSize = e.bufferSize;
            setup.useDefaultInputChannels = true;
            setup.useDefaultOutputChannels = true;

            if (manager.setAudioDeviceSetup (setup, true).isEmpty())
                return true;

            // The saved rate or buffer is not offered any more: take the device's own.
            setup.sampleRate = 0.0;
            setup.bufferSize = 0;
            return manager.setAudioDeviceSetup (setup, true).isEmpty();
        }

        void update()
        {
            std::set<juce::String> now;
            for (const auto& e : favourites.list)
                if (isAvailable (e))
                    now.insert (key (e));

            const auto listChanged = ! started || now != lastAvailable;
            const auto previous = lastAvailable;
            lastAvailable = now;
            started = true;

            const auto current = currentEntry();
            const auto haveCurrent = manager.getCurrentAudioDevice() != nullptr;

            if (listChanged)
            {
                const auto index = favourites.choose (haveCurrent ? &current : nullptr,
                                                      [this] (const devfav::Entry& e) { return isAvailable (e); },
                                                      [&] (const devfav::Entry& e) { return previous.count (key (e)) == 0; });

                if (index >= 0 && key (favourites.list[(size_t) index]) != failed)
                {
                    const auto target = favourites.list[(size_t) index];
                    if (switchTo (target))
                        failed.clear();
                    else
                        failed = key (target);
                    return; // the switch sends its own change message; it is remembered then
                }
            }

            if (haveCurrent)
            {
                favourites.remember (current);
                changed();
            }
        }

        juce::AudioDeviceManager& manager;
        juce::PropertySet* settings;
        devfav::Favourites favourites;
        std::set<juce::String> lastAvailable;
        juce::String failed;
        bool started = false;
    };

    //==============================================================================
    class FavouritesComponent final : public juce::Component,
                                      private juce::ListBoxModel
    {
    public:
        explicit FavouritesComponent (DeviceFavouritesManager& m) : owner (m)
        {
            info.setText ("Devices are remembered when you use them. When one is unplugged, the next one in "
                          "this list that is connected takes over, and the first one comes back when it is "
                          "plugged in again, with the sample rate and buffer size it last had.",
                          juce::dontSendNotification);
            info.setJustificationType (juce::Justification::topLeft);
            info.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (info);

            list.setModel (this);
            list.setRowHeight (28);
            addAndMakeVisible (list);

            for (auto* b : { &up, &down, &remove })
                addAndMakeVisible (b);

            up.onClick     = [this] { moveSelected (-1); };
            down.onClick   = [this] { moveSelected (1); };
            remove.onClick = [this]
            {
                owner.get().remove (list.getSelectedRow());
                owner.changed();
                list.updateContent();
                list.repaint();
            };

            setSize (520, 360);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            info.setBounds (r.removeFromTop (58));
            auto buttons = r.removeFromBottom (30);
            r.removeFromBottom (8);
            up.setBounds (buttons.removeFromLeft (90));
            buttons.removeFromLeft (8);
            down.setBounds (buttons.removeFromLeft (90));
            buttons.removeFromLeft (8);
            remove.setBounds (buttons.removeFromLeft (90));
            list.setBounds (r);
        }

    private:
        int getNumRows() override { return (int) owner.get().list.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (row < 0 || row >= getNumRows())
                return;

            const auto& e = owner.get().list[(size_t) row];
            if (selected)
                g.fillAll (juce::Colours::white.withAlpha (0.15f));

            g.setColour (findColour (juce::ListBox::textColourId));
            g.setFont (14.0f);
            g.drawText (juce::String (row + 1) + ".  " + e.label(), 8, 0, width * 6 / 10 - 8, height, juce::Justification::centredLeft, true);

            juce::String detail;
            if (e.sampleRate > 0)  detail << juce::String (e.sampleRate / 1000.0, 1) << " kHz";
            if (e.bitDepth > 0)    detail << ", " << e.bitDepth << "-bit";
            if (e.bufferSize > 0)  detail << ", " << e.bufferSize << " samples";
            g.setColour (findColour (juce::ListBox::textColourId).withAlpha (0.65f));
            g.drawText (detail, width * 6 / 10, 0, width * 4 / 10 - 8, height, juce::Justification::centredRight, true);
        }

        void moveSelected (int by)
        {
            const auto row = list.getSelectedRow();
            owner.get().move (row, row + by);
            owner.changed();
            list.selectRow (juce::jlimit (0, juce::jmax (0, getNumRows() - 1), row + by));
            list.updateContent();
            list.repaint();
        }

        DeviceFavouritesManager& owner;
        juce::Label info;
        juce::ListBox list;
        juce::TextButton up { "Move up" }, down { "Move down" }, remove { "Remove" };
    };

    void DeviceFavouritesManager::showDialog()
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new FavouritesComponent (*this));
        options.dialogTitle = "Audio device favourites";
        options.componentToCentreAround = nullptr;
        options.dialogBackgroundColour = juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    //==============================================================================
    // JUCE's standalone window with one more item in its Options menu, a maximise button and
    // the menu bar.
    //==============================================================================
    // Keeps every input of the audio interface open (up to the processor's limit), so DAW
    // tracks can pick any of them: on start, and whenever the device changes.
    class InputOpener final : private juce::ChangeListener
    {
    public:
        explicit InputOpener (juce::AudioDeviceManager& m) : manager (m)
        {
            manager.addChangeListener (this);
            openAll();
        }

        ~InputOpener() override { manager.removeChangeListener (this); }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { openAll(); }

        void openAll()
        {
            auto* device = manager.getCurrentAudioDevice();
            if (device == nullptr)
                return;

            const auto available = juce::jmin (device->getInputChannelNames().size(), (int) PluginProcessor::maxAppInputs);
            if (device->getActiveInputChannels().countNumberOfSetBits() >= available)
                return;

            auto setup = manager.getAudioDeviceSetup();
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputChannels.setRange (0, available, true);
            manager.setAudioDeviceSetup (setup, true);
        }

        juce::AudioDeviceManager& manager;
    };

    class MicFXWindow final : public juce::StandaloneFilterWindow,
                              private juce::MenuBarModel
    {
    public:
        MicFXWindow (const juce::String& title, juce::Colour background, std::unique_ptr<juce::StandalonePluginHolder> holder)
            : StandaloneFilterWindow (title, background, std::move (holder)),
              inputOpener (pluginHolder->deviceManager)
        {
            setTitleBarButtonsRequired (juce::DocumentWindow::allButtons, false);

           #if JUCE_MAC
            appleMenu.addItem (checkForUpdatesId, TRANS ("Check for updates..."));
            juce::MenuBarModel::setMacMainMenu (this, &appleMenu);
           #else
            setMenuBar (this);
           #endif
        }

        ~MicFXWindow() override
        {
           #if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu (nullptr);
           #else
            setMenuBar (nullptr);
           #endif
        }

        DeviceFavouritesManager* favourites = nullptr;

    private:
        InputOpener inputOpener;
        static constexpr int checkForUpdatesId = 100;
        static constexpr int favouritesId = 101;
        static constexpr int firstMenuItemId = 1000;

        PluginEditor* getEditor() const
        {
            if (auto* processor = getAudioProcessor())
                return dynamic_cast<PluginEditor*> (processor->getActiveEditor());

            return nullptr;
        }

        //==============================================================================
        juce::StringArray getMenuBarNames() override
        {
            juce::StringArray names;
            for (const auto& [name, items] : getMenus())
                names.add (name);
            return names;
        }

        juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
        {
            juce::PopupMenu menu;
            const auto& menus = getMenus();
            if (! juce::isPositiveAndBelow (index, (int) menus.size()))
                return menu;

            auto* editor = getEditor();
            const auto isDaw = editor != nullptr && editor->isDawMode();
            const auto action = [] (const MenuItem& item) { return juce::String (item.action); };

            for (size_t i = 0; i < menus[(size_t) index].second.size(); ++i)
            {
                const auto& item = menus[(size_t) index].second[i];
                if (action (item).isEmpty())
                {
                    menu.addSeparator();
                    continue;
                }

                juce::PopupMenu::Item entry (item.name);
                entry.itemID = firstMenuItemId + index * 100 + (int) i;
                entry.shortcutKeyDescription = item.keys; // shown on Windows and Linux; the macOS bar leaves it out
                entry.isEnabled = editor != nullptr && (isDaw || ! item.dawOnly);

                if (action (item) == "mode:live")       entry.isTicked = ! isDaw;
                else if (action (item) == "mode:daw")   entry.isTicked = isDaw;
                else if (action (item) == "app:fullScreen")
                {
                    entry.isTicked = isFullScreen();
                    entry.isEnabled = true;
                }
                else if (action (item).startsWith ("app:"))
                    entry.isEnabled = true;
                else if (isDaw && (action (item) == "undo" || action (item) == "redo"))
                {
                    // Names the step, as the toolbar's buttons do: "Undo Move clip".
                    const auto step = action (item) == "undo" ? editor->getUndoName() : editor->getRedoName();
                    entry.isEnabled = step.isNotEmpty();
                    if (step.isNotEmpty())
                        entry.text = juce::String (item.name) + " " + step;
                }

                menu.addItem (std::move (entry));
            }

            return menu;
        }

        void menuItemSelected (int itemId, int) override
        {
            if (itemId == checkForUpdatesId)
            {
                showUpdateDialog (this);
                return;
            }

            const auto index = (itemId - firstMenuItemId) / 100, position = (itemId - firstMenuItemId) % 100;
            const auto& menus = getMenus();
            if (! juce::isPositiveAndBelow (index, (int) menus.size())
                || ! juce::isPositiveAndBelow (position, (int) menus[(size_t) index].second.size()))
                return;

            const juce::String action (menus[(size_t) index].second[(size_t) position].action);

            if (action == "app:audio")            pluginHolder->showAudioSettingsDialog();
            else if (action == "app:favourites")  { if (favourites != nullptr) favourites->showDialog(); }
            else if (action == "app:updates")     showUpdateDialog (this);
            else if (action == "app:fullScreen")  setFullScreen (! isFullScreen());
            else if (action == "app:quit")        juce::JUCEApplicationBase::getInstance()->systemRequestedQuit();
            else if (auto* editor = getEditor())  editor->sendMenuCommand (action);
        }

        //==============================================================================
        void buttonClicked (juce::Button* button) override
        {
            juce::PopupMenu m;
            m.addItem (1, TRANS ("Audio/MIDI Settings..."));
            m.addItem (favouritesId, TRANS ("Audio device favourites..."));
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
                                 else if (result == favouritesId)
                                 {
                                     if (safe->favourites != nullptr)
                                         safe->favourites->showDialog();
                                 }
                                 else
                                     safe->handleMenuResult (result);
                             });
        }

        juce::PopupMenu appleMenu;
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
                favourites = std::make_unique<DeviceFavouritesManager> (pluginHolder->deviceManager, appProperties.getUserSettings());
                return;
            }

            mainWindow = std::make_unique<MicFXWindow> (getApplicationName(),
                                                        juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                                                        createPluginHolder());
            favourites = std::make_unique<DeviceFavouritesManager> (mainWindow->pluginHolder->deviceManager, appProperties.getUserSettings());
            mainWindow->favourites = favourites.get();
            mainWindow->setVisible (true);
        }

        void shutdown() override
        {
            favourites = nullptr;
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
                                                                   true); // open every MIDI input (MIDI tracks in DAW mode)
        }

        juce::ApplicationProperties appProperties;
        std::unique_ptr<DeviceFavouritesManager> favourites;
        std::unique_ptr<MicFXWindow> mainWindow;
        std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new MicFXApp();
}

#endif
