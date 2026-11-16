#include "DawActions.h"
#include "DawBridge.h"

namespace
{
    namespace ids = daw::ids;

    juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> properties)
    {
        auto* result = new juce::DynamicObject();
        for (const auto& [name, value] : properties)
            result->setProperty (name, value);
        return juce::var (result);
    }

    juce::var fail (const juce::String& message) { return obj ({ { "ok", false }, { "error", message } }); }

    juce::var done (const juce::String& summary, std::initializer_list<std::pair<const char*, juce::var>> more = {})
    {
        auto result = obj ({ { "ok", true }, { "summary", summary } });
        for (const auto& [name, value] : more)
            result.getDynamicObject()->setProperty (name, value);
        return result;
    }

    double number (const juce::ValueTree& tree, const juce::Identifier& name)
    {
        const auto& value = tree[name];
        return value.isString() ? value.toString().getDoubleValue() : (double) value;
    }

    juce::String lower (const juce::String& s) { return s.trim().toLowerCase(); }

    juce::String orElse (const juce::String& s, const juce::String& fallback) { return s.isEmpty() ? fallback : s; }

    juce::String joinLabels (const juce::StringArray& labels)
    {
        return labels.size() <= 3 ? labels.joinIntoString (", ") : labels.joinIntoString (", ", 0, 3) + " and " + juce::String (labels.size() - 3) + " more";
    }

    const char* const noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    DawActions::Param P (const char* name, const char* type, const char* description, bool required = false)
    {
        DawActions::Param p;
        p.name = name; p.type = type; p.description = description; p.required = required;
        return p;
    }

    DawActions::Param withRange (DawActions::Param p, double min, double max) { p.hasRange = true; p.min = min; p.max = max; return p; }
    DawActions::Param withChoices (DawActions::Param p, juce::StringArray c) { p.choices = std::move (c); return p; }

    const char* const targetHelp = "Which track(s): an id, a number (1 is the first), a name, a role (voice, background, instrumental, "
                                   "guitar, piano, pad, synth, bass, drums), \"selected\", \"all\", or a list of those";
}

DawActions::DawActions (PluginProcessor& p, DawBridge& b) : processor (p), bridge (b)
{
    build();
}

//==============================================================================
std::vector<int> DawActions::allTrackIds() const
{
    std::vector<int> result;
    for (const auto& track : processor.getDawEdit().getTracks())
        result.push_back ((int) track[ids::id]);
    return result;
}

int DawActions::selectedTrack() const
{
    const auto view = juce::JSON::parse (processor.getDawEdit().getState()[ids::viewState].toString());
    return view.hasProperty ("track") && ! view["track"].isVoid() ? (int) view["track"] : -1;
}

juce::String DawActions::trackLabel (int trackId) const
{
    auto& edit = processor.getDawEdit();
    const auto tracks = edit.getTracks();
    for (int i = 0; i < tracks.size(); ++i)
        if ((int) tracks[i][ids::id] == trackId)
            return tracks[i][ids::name].toString() + " (track " + juce::String (i + 1) + ")";
    return "track " + juce::String (trackId);
}

std::vector<int> DawActions::resolveTracks (const juce::var& spec, juce::String& problem) const
{
    auto& edit = processor.getDawEdit();
    const auto tracks = edit.getTracks();
    std::vector<int> result;
    const auto push = [&] (int id) { if (std::find (result.begin(), result.end(), id) == result.end()) result.push_back (id); };

    const auto one = [&] (const juce::var& item)
    {
        if (item.isInt() || item.isInt64() || item.isDouble())
        {
            if (const auto id = (int) item; edit.getTrack (id).isValid()) { push (id); return; }
            problem = "There is no track with id " + juce::String ((int) item) + ".";
            return;
        }

        auto text = lower (item.toString());
        if (text.isEmpty()) { problem = "A track was left empty."; return; }

        if (text == "all" || text == "every track" || text == "everything" || text == "all tracks")
        {
            for (const auto id : allTrackIds()) push (id);
            return;
        }
        if (text == "selected" || text == "selection" || text == "this track")
        {
            if (const auto id = selectedTrack(); id > 0 && edit.getTrack (id).isValid()) { push (id); return; }
            problem = "No track is selected.";
            return;
        }

        // A role tag (also plural: "guitars", "instrumental tracks").
        auto roleKey = text;
        if (roleKey.endsWith (" tracks")) roleKey = roleKey.dropLastCharacters (7);
        if (roleKey.endsWithChar ('s') && roleKey.length() > 3) roleKey = roleKey.dropLastCharacters (1);
        if (roleKey == "backing" || roleKey == "instrumental") roleKey = "instrumental";
        if (roleKey == "backing vocal") roleKey = "background";
        for (const auto role : analysis::allRoles)
            if (roleKey == analysis::getRoleKey (role))
            {
                const auto before = result.size();
                const auto includeInstruments = role == analysis::Role::backing; // "instrumental tracks": every instrument role too
                for (const auto& track : tracks)
                {
                    if (! track.hasProperty (ids::mixRole)) continue;
                    const auto tagged = analysis::getRoleFromKey (track[ids::mixRole].toString());
                    if (tagged == role || (includeInstruments && analysis::isInstrument (tagged)))
                        push ((int) track[ids::id]);
                }
                if (result.size() == before)
                    problem = "No track is tagged \"" + roleKey + "\". Tag tracks first (track.role) or use their names.";
                return;
            }

        // A number ("2", "track 2") or a name.
        const auto numeric = text.startsWith ("track ") ? text.substring (6).trim() : text;
        if (numeric.isNotEmpty() && numeric.containsOnly ("0123456789"))
        {
            const auto n = numeric.getIntValue();
            if (n >= 1 && n <= tracks.size()) { push ((int) tracks[n - 1][ids::id]); return; }
            problem = "There is no track " + juce::String (n) + " (the project has " + juce::String (tracks.size()) + ").";
            return;
        }

        for (const auto& track : tracks)
            if (lower (track[ids::name].toString()) == text) { push ((int) track[ids::id]); return; }

        auto matches = 0;
        for (const auto& track : tracks)
            if (lower (track[ids::name].toString()).contains (text)) { push ((int) track[ids::id]); ++matches; }
        if (matches == 0)
            problem = "No track is called \"" + item.toString() + "\".";
    };

    if (const auto* list = spec.getArray())
        for (const auto& item : *list)
            one (item);
    else
        one (spec);

    if (problem.isEmpty() && result.empty())
        problem = "That didn't match any track.";
    if (problem.isNotEmpty())
        result.clear();
    return result;
}

int DawActions::resolveReturn (const juce::var& spec, juce::String& problem) const
{
    auto& edit = processor.getDawEdit();
    const auto returns = edit.getReturns();
    if (spec.isInt() || spec.isInt64() || spec.isDouble())
        if (edit.getReturn ((int) spec).isValid())
            return (int) spec;

    const auto text = lower (spec.toString());
    for (const auto& r : returns)
        if (lower (r[ids::name].toString()) == text || lower (r[ids::name].toString()) == "return " + text)
            return (int) r[ids::id];
    for (const auto& r : returns)
        if (text.isNotEmpty() && lower (r[ids::name].toString()).contains (text))
            return (int) r[ids::id];

    problem = returns.isEmpty() ? "The project has no return tracks yet (add one with return.add)."
                                : "No return is called \"" + spec.toString() + "\".";
    return -1;
}

std::optional<std::pair<double, double>> DawActions::resolveRange (const juce::var& spec, juce::String& problem) const
{
    auto& edit = processor.getDawEdit();
    const auto text = lower (spec.toString());
    const auto length = (double) processor.getDawEngine().getLength() / processor.getDawEngine().getSampleRate();

    if (text == "loop" || text == "selection")
    {
        const auto& state = edit.getState();
        if ((bool) state.getProperty (ids::loopOn, false))
            return std::make_pair (number (state, ids::loopStart), number (state, ids::loopEnd));
        problem = "There is no loop set.";
        return std::nullopt;
    }
    if (text == "all" || text == "song" || text == "whole song")
        return std::make_pair (0.0, length);

    // A section by name; "chorus 2" is the second one.
    const auto sections = edit.getSongSections();
    auto wanted = text;
    auto nth = 1;
    if (wanted.length() > 2 && wanted.getLastCharacters (1).containsOnly ("0123456789") && wanted[wanted.length() - 2] == ' ')
    {
        nth = wanted.getLastCharacters (1).getIntValue();
        wanted = wanted.dropLastCharacters (2).trim();
    }
    auto seen = 0;
    for (const auto& section : sections)
        if (lower (section[ids::name].toString()).startsWith (wanted) && ++seen == nth)
        {
            const auto start = number (section, ids::time);
            return std::make_pair (start, start + number (section, ids::length));
        }
    for (const auto& marker : edit.getMarkers()) // a plain marker: up to the next one
        if (lower (marker[ids::name].toString()) == wanted)
        {
            const auto start = number (marker, ids::time);
            auto end = length;
            for (const auto& other : edit.getMarkers())
                if (number (other, ids::time) > start) { end = number (other, ids::time); break; }
            return std::make_pair (start, end);
        }

    juce::StringArray names;
    for (const auto& s : sections)
        names.add (s[ids::name].toString());
    problem = "No section or loop called \"" + spec.toString() + "\". Sections: " + (names.isEmpty() ? juce::String ("none (run the song map first)") : names.joinIntoString (", ")) + ".";
    return std::nullopt;
}

std::optional<double> DawActions::resolveTime (const juce::var& spec, juce::String& problem) const
{
    auto& edit = processor.getDawEdit();
    auto& engine = processor.getDawEngine();
    if (spec.isInt() || spec.isInt64() || spec.isDouble())
        return (double) spec;

    const auto text = lower (spec.toString());
    if (text.isEmpty()) { problem = "No time given."; return std::nullopt; }
    if (text == "start") return 0.0;
    if (text == "end") return (double) engine.getLength() / engine.getSampleRate();
    if (text == "playhead" || text == "now") return (double) engine.getPosition() / engine.getSampleRate();

    // "bar 9" / "bars 9": the start of that bar (bar 1 is the song's start).
    auto barText = text;
    if (barText.startsWith ("bars ")) barText = barText.substring (5);
    else if (barText.startsWith ("bar ")) barText = barText.substring (4);
    if (barText != text && barText.trim().containsOnly ("0123456789."))
        return edit.getTempoMap().toSeconds ((barText.getDoubleValue() - 1.0) * edit.getBeatsPerBar());

    if (text.containsOnly ("0123456789.-"))
        return text.getDoubleValue();

    if (const auto range = resolveRange (spec, problem))
        return range->first;
    return std::nullopt;
}

juce::var DawActions::call (const juce::String& command, const juce::var& args)
{
    // Part of the action's own undo step.
    auto* withContinue = new juce::DynamicObject();
    if (const auto* source = args.getDynamicObject())
        for (const auto& [name, value] : source->getProperties())
            withContinue->setProperty (name, value);
    withContinue->setProperty ("continue", true);

    juce::var result ("did not complete");
    bridge.handle (command, juce::var (withContinue), [&] (juce::var r) { result = std::move (r); });
    return result;
}

//==============================================================================
juce::var DawActions::list() const
{
    juce::Array<juce::var> out;
    for (const auto& action : actions)
    {
        juce::Array<juce::var> params;
        for (const auto& p : action.params)
        {
            auto entry = obj ({ { "name", p.name }, { "type", p.type }, { "description", p.description }, { "required", p.required } });
            if (p.hasRange) { entry.getDynamicObject()->setProperty ("min", p.min); entry.getDynamicObject()->setProperty ("max", p.max); }
            if (! p.choices.isEmpty())
            {
                juce::Array<juce::var> choices;
                for (const auto& c : p.choices) choices.add (c);
                entry.getDynamicObject()->setProperty ("choices", choices);
            }
            params.add (entry);
        }
        out.add (obj ({ { "name", action.name }, { "description", action.description }, { "readOnly", action.readOnly },
                        { "undoName", action.undoName }, { "params", params } }));
    }
    return obj ({ { "ok", true }, { "actions", out } });
}

void DawActions::run (const juce::String& name, const juce::var& args, Complete complete)
{
    const auto it = byName.find (name);
    if (it == byName.end())
    {
        juce::StringArray near;
        for (const auto& action : actions)
            if (action.name.contains (name) || (name.contains (".") && action.name.startsWith (name.upToFirstOccurrenceOf (".", true, false))))
                near.add (action.name);
        complete (fail ("There is no action called \"" + name + "\"." + (near.isEmpty() ? juce::String() : " Did you mean: " + near.joinIntoString (", ") + "?")));
        return;
    }

    const auto& action = actions[it->second];

    // Checks before anything changes: required arguments present, numbers in range, choices known.
    for (const auto& p : action.params)
    {
        if (! args.hasProperty (p.name))
        {
            if (p.required) { complete (fail ("Missing \"" + p.name + "\": " + p.description + ".")); return; }
            continue;
        }

        const auto& v = args[juce::Identifier (p.name)];
        if (p.type == "number" || p.type == "integer")
        {
            if (! (v.isInt() || v.isInt64() || v.isDouble() || (v.isString() && v.toString().trim().containsOnly ("0123456789.-+") && v.toString().trim().isNotEmpty())))
            { complete (fail ("\"" + p.name + "\" must be a number.")); return; }
            if (p.hasRange && ((double) v < p.min || (double) v > p.max))
            { complete (fail ("\"" + p.name + "\" must be between " + juce::String (p.min) + " and " + juce::String (p.max) + ".")); return; }
        }
        if (! p.choices.isEmpty() && ! p.choices.contains (lower (v.toString())))
        { complete (fail ("\"" + p.name + "\" must be one of: " + p.choices.joinIntoString (", ") + ".")); return; }
    }

    auto& undo = processor.getDawEdit().getUndoManager();
    if (! action.readOnly)
        undo.beginNewTransaction (action.undoName);

    // An action that fails must not leave half its work behind: undo what its step did.
    const auto writes = ! action.readOnly;
    action.run (args, [complete = std::move (complete), &undo, writes] (juce::var result)
    {
        if (writes && result.isObject() && ! (bool) result.getProperty ("ok", true) && undo.getNumActionsInCurrentTransaction() > 0)
            undo.undo();
        complete (std::move (result));
    });
}

void DawActions::onEvent (const juce::Identifier& event, const juce::var& payload)
{
    const auto state = payload["state"].toString();
    if (event.toString() == "dawAutoMix" && waitingAutoMix)
    {
        if (state == "analysing")
            return;

        auto finish = std::move (waitingAutoMix);
        waitingAutoMix = nullptr;
        if (state != "ready")
        {
            finish (fail (state == "cancelled" ? juce::String ("Auto Mix was cancelled.") : orElse (payload["error"].toString(), "Auto Mix failed.")));
            return;
        }
        if (autoMixApply)
        {
            processor.getDawEdit().getUndoManager().beginNewTransaction ("Auto Mix");
            const auto applied = call ("autoMix", obj ({ { "action", "apply" } }));
            if (! (bool) applied["ok"]) { finish (applied); return; }
        }
        finish (done (autoMixApply ? "Auto Mix listened and applied its plan." : "Auto Mix listened; the plan is ready (not applied).", { { "plan", payload } }));
    }
    else if (event.toString() == "dawMixCheck" && waitingMixCheck)
    {
        if (state == "analysing" || state == "fixed")
            return;

        auto finish = std::move (waitingMixCheck);
        waitingMixCheck = nullptr;
        if (state != "ready")
        {
            finish (fail (state == "cancelled" ? juce::String ("Mix Check was cancelled.") : orElse (payload["error"].toString(), "Mix Check failed.")));
            return;
        }
        finish (done ("Mix Check finished.", { { "report", payload } }));
    }
}

//==============================================================================
void DawActions::add (const char* name, const char* description, const char* undoName, std::vector<Param> params,
                      std::function<void (const juce::var&, Complete)> run)
{
    Action action;
    action.name = name;
    action.description = description;
    action.undoName = undoName;
    action.readOnly = juce::String (undoName).isEmpty();
    action.params = std::move (params);
    action.run = std::move (run);
    byName[action.name] = actions.size();
    actions.push_back (std::move (action));
}

void DawActions::build()
{
    auto& edit = processor.getDawEdit();
    auto& engine = processor.getDawEngine();

    // Resolves args["tracks"] (or "track"), calls `each` per track, and answers with its summary.
    const auto forTracks = [this] (const juce::var& args, const char* key, Complete& complete, const std::function<void (const std::vector<int>&)>& go)
    {
        juce::String problem;
        const auto ids = resolveTracks (args[key], problem);
        if (ids.empty()) { complete (fail (problem)); return; }
        go (ids);
    };
    const auto labels = [this] (const std::vector<int>& trackIds)
    {
        juce::StringArray out;
        for (const auto id : trackIds) out.add (trackLabel (id));
        return joinLabels (out);
    };
    const auto ofTracks = [this] (const std::vector<int>& trackIds)
    {
        juce::Array<juce::var> out;
        for (const auto id : trackIds) out.add (id);
        return out;
    };

    //============================================================================ read
    add ("session.summary", "Overview of the project: tempo, key, sections, loop, tracks (id, number, name, role, level, effects), returns, playhead.", "", {},
        [this, &edit, &engine] (const juce::var&, Complete complete)
        {
            juce::Array<juce::var> tracks, returns, sections;
            const auto list = edit.getTracks();
            for (int i = 0; i < list.size(); ++i)
            {
                const auto& t = list[i];
                juce::Array<juce::var> effects;
                for (const auto& insert : daw::Edit::getInserts (t))
                    effects.add (insert[ids::type].toString());
                tracks.add (obj ({ { "id", (int) t[ids::id] }, { "number", i + 1 }, { "name", t[ids::name].toString() },
                                   { "kind", daw::Edit::isMidiTrack (t) ? "midi" : "audio" },
                                   { "role", t.hasProperty (ids::mixRole) ? t[ids::mixRole].toString() : juce::String() },
                                   { "volumeDb", number (t, ids::volume) }, { "pan", number (t, ids::pan) },
                                   { "mute", (bool) t.getProperty (ids::mute, false) }, { "solo", (bool) t.getProperty (ids::solo, false) },
                                   { "transpose", number (t, ids::transpose) }, { "effects", effects } }));
            }
            for (const auto& r : edit.getReturns())
                returns.add (obj ({ { "id", (int) r[ids::id] }, { "name", r[ids::name].toString() } }));
            for (const auto& s : edit.getSongSections())
                sections.add (obj ({ { "name", s[ids::name].toString() }, { "start", number (s, ids::time) }, { "end", number (s, ids::time) + number (s, ids::length) } }));

            const auto rate = engine.getSampleRate();
            const auto& state = edit.getState();
            juce::var key;
            if (edit.getKey() >= 0)
                key = juce::String (noteNames[edit.getKey() % 12]) + (edit.isKeyMinor() ? " minor" : " major");
            complete (obj ({ { "ok", true }, { "tempo", edit.getTempo() }, { "beatsPerBar", edit.getBeatsPerBar() }, { "key", key },
                             { "lengthSeconds", (double) engine.getLength() / rate }, { "playheadSeconds", (double) engine.getPosition() / rate },
                             { "playing", engine.isPlaying() },
                             { "loop", obj ({ { "on", (bool) state.getProperty (ids::loopOn, false) }, { "start", number (state, ids::loopStart) }, { "end", number (state, ids::loopEnd) } }) },
                             { "tracks", tracks }, { "returns", returns }, { "sections", sections },
                             { "selectedTrack", selectedTrack() }, { "canUndo", edit.getUndoManager().canUndo() },
                             { "undoName", edit.getUndoManager().getUndoDescription() } }));
        });

    add ("track.get", "One track in detail: its effects with every value, sends, and clips.", "",
        { P ("track", "track", targetHelp, true) },
        [this, forTracks] (const juce::var& args, Complete complete)
        {
            forTracks (args, "track", complete, [&] (const std::vector<int>& found)
            {
                const auto trackId = found.front();
                auto& e = processor.getDawEdit();
                const auto track = e.getTrack (trackId);
                juce::Array<juce::var> clips, sends;
                for (const auto& send : track)
                    if (send.hasType (ids::SEND))
                        sends.add (obj ({ { "return", (int) send[ids::target] }, { "levelDb", number (send, ids::level) }, { "pre", (bool) send.getProperty (ids::pre, false) } }));
                for (const auto& clip : track)
                    if (clip.hasType (ids::CLIP))
                        clips.add (obj ({ { "id", (int) clip[ids::id] }, { "name", clip[ids::name].toString() }, { "start", number (clip, ids::start) },
                                          { "length", number (clip, ids::length) }, { "kind", clip[ids::kind].toString().isEmpty() ? juce::String ("audio") : clip[ids::kind].toString() } }));
                complete (obj ({ { "ok", true }, { "id", trackId }, { "name", track[ids::name].toString() },
                                 { "role", track[ids::mixRole].toString() }, { "volumeDb", number (track, ids::volume) }, { "pan", number (track, ids::pan) },
                                 { "mute", (bool) track.getProperty (ids::mute, false) }, { "solo", (bool) track.getProperty (ids::solo, false) },
                                 { "transpose", number (track, ids::transpose) },
                                 { "effects", bridge.describeInserts (track) }, { "sends", sends }, { "clips", clips } }));
            });
        });

    add ("mix.plan", "The last Auto Mix plan: tracks with their roles, and every planned move with its reason.", "", {},
        [this] (const juce::var&, Complete complete) { complete (bridge.describeAutoMix()); });

    add ("mix.report", "The last Mix Check report (problems found and fixes offered).", "", {},
        [this] (const juce::var&, Complete complete) { complete (bridge.describeMixCheck()); });

    //============================================================================ transport
    add ("transport.play", "Start playing from the playhead, or from `from` (seconds, \"bar 9\", a section name).", "",
        { P ("from", "time", "Where to start (default: the playhead)") },
        [this, &engine] (const juce::var& args, Complete complete)
        {
            if (args.hasProperty ("from"))
            {
                juce::String problem;
                const auto t = resolveTime (args["from"], problem);
                if (! t) { complete (fail (problem)); return; }
                engine.seek ((juce::int64) (*t * engine.getSampleRate()));
            }
            engine.play();
            complete (done ("Playing."));
        });

    add ("transport.stop", "Stop playing (and go back to the start with rewind: true).", "",
        { P ("rewind", "boolean", "Go back to the start") },
        [&engine] (const juce::var& args, Complete complete)
        {
            engine.stop();
            if ((bool) args.getProperty ("rewind", false)) engine.seek (0);
            complete (done ("Stopped."));
        });

    add ("transport.locate", "Move the playhead.", "",
        { P ("time", "time", "Seconds, \"bar 9\", a section name, \"start\" or \"end\"", true) },
        [this, &engine] (const juce::var& args, Complete complete)
        {
            juce::String problem;
            const auto t = resolveTime (args["time"], problem);
            if (! t) { complete (fail (problem)); return; }
            engine.seek ((juce::int64) (juce::jmax (0.0, *t) * engine.getSampleRate()));
            complete (done ("Playhead at " + juce::String (*t, 2) + " s.", { { "seconds", *t } }));
        });

    add ("loop.set", "Set the loop range and turn it on (or off with on: false).", "Loop range",
        { P ("from", "time", "Start: seconds, \"bar 9\", or a section name (which also sets the end)"), P ("to", "time", "End"), P ("on", "boolean", "Loop on (default true)") },
        [this, &edit] (const juce::var& args, Complete complete)
        {
            const auto on = (bool) args.getProperty ("on", true);
            juce::String problem;
            double start = number (edit.getState(), ids::loopStart), end = number (edit.getState(), ids::loopEnd);
            if (args.hasProperty ("from"))
            {
                if (! args.hasProperty ("to"))
                {
                    const auto range = resolveRange (args["from"], problem);
                    if (range) { start = range->first; end = range->second; }
                    else if (const auto t = resolveTime (args["from"], problem)) { start = *t; problem = {}; }
                }
                else if (const auto t = resolveTime (args["from"], problem)) start = *t;
            }
            if (problem.isEmpty() && args.hasProperty ("to"))
                if (const auto t = resolveTime (args["to"], problem)) end = *t;
            if (problem.isNotEmpty()) { complete (fail (problem)); return; }
            if (on && end <= start) { complete (fail ("The loop must end after it starts.")); return; }
            call ("loop", obj ({ { "on", on }, { "start", start }, { "end", end } }));
            complete (done (on ? "Looping " + juce::String (start, 2) + " to " + juce::String (end, 2) + " s." : "Loop off."));
        });

    //============================================================================ project
    add ("tempo.set", "Set the project tempo in BPM.", "Tempo", { withRange (P ("bpm", "number", "Beats per minute", true), 20, 400) },
        [this] (const juce::var& args, Complete complete)
        {
            call ("tempo", obj ({ { "bpm", (double) args["bpm"] } }));
            complete (done ("Tempo is " + juce::String ((double) args["bpm"], 1) + " BPM."));
        });

    add ("key.set", "Set the song's key (used by autotune and the instruments).", "Song key",
        { P ("tonic", "string", "C, C#, D, ... or a number 0-11", true), P ("minor", "boolean", "Minor key (default major)") },
        [&edit] (const juce::var& args, Complete complete)
        {
            auto tonic = -1;
            const auto text = args["tonic"].toString().trim();
            if (text.isNotEmpty() && text.containsOnly ("0123456789"))
                tonic = text.getIntValue();
            for (int i = 0; i < 12 && tonic < 0; ++i)
                if (text.equalsIgnoreCase (noteNames[i]))
                    tonic = i;
            // Flats: Bb is A#.
            for (int i = 0; i < 12 && tonic < 0; ++i)
                if (text.length() == 2 && text[1] == 'b' && text.substring (0, 1).equalsIgnoreCase (noteNames[i]))
                    tonic = (i + 11) % 12;
            if (tonic < 0 || tonic > 11) { complete (fail ("Unknown key \"" + text + "\". Use a note name like C, F#, Bb or a number 0-11.")); return; }
            const auto minor = (bool) args.getProperty ("minor", false);
            edit.setKey (tonic, minor);
            complete (done ("Key is " + juce::String (noteNames[tonic]) + (minor ? " minor." : " major.")));
        });

    //============================================================================ tracks
    add ("track.add", "Add a track at the end: an audio track, or a MIDI track with an instrument.", "Add track",
        { P ("name", "string", "Track name"), withChoices (P ("kind", "string", "audio (default) or midi"), { "audio", "midi" }),
          P ("instrument", "string", "For MIDI tracks: an instrument id (see instruments)") },
        [this] (const juce::var& args, Complete complete)
        {
            const auto midi = lower (args["kind"].toString()) == "midi" || args.hasProperty ("instrument");
            auto a = obj ({ { "name", args["name"].toString() } });
            if (args.hasProperty ("instrument")) a.getDynamicObject()->setProperty ("instrument", args["instrument"]);
            const auto result = call (midi ? "addMidiTrack" : "addTrack", a);
            if (! result.isInt()) { complete (result.isObject() ? result : fail ("Couldn't add the track.")); return; }
            complete (done ("Added " + trackLabel ((int) result) + ".", { { "track", (int) result } }));
        });

    add ("track.rename", "Rename tracks.", "Rename track",
        { P ("tracks", "tracks", targetHelp, true), P ("name", "string", "The new name", true) },
        [=] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                const auto before = labels (found);
                for (const auto id : found) call ("setTrack", obj ({ { "id", id }, { "name", args["name"] } }));
                complete (done ("Renamed " + before + " to " + args["name"].toString() + ".", { { "tracks", ofTracks (found) } }));
            });
        });

    add ("track.mute", "Mute or unmute tracks.", "Mute",
        { P ("tracks", "tracks", targetHelp, true), P ("mute", "boolean", "true to mute (default), false to unmute") },
        [=] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                const auto on = (bool) args.getProperty ("mute", true);
                for (const auto id : found) call ("setTrack", obj ({ { "id", id }, { "mute", on } }));
                complete (done ((on ? "Muted " : "Unmuted ") + labels (found) + ".", { { "tracks", ofTracks (found) } }));
            });
        });

    add ("track.solo", "Solo or unsolo tracks.", "Solo",
        { P ("tracks", "tracks", targetHelp, true), P ("solo", "boolean", "true to solo (default), false to unsolo") },
        [=] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                const auto on = (bool) args.getProperty ("solo", true);
                for (const auto id : found) call ("setTrack", obj ({ { "id", id }, { "solo", on } }));
                complete (done ((on ? "Soloed " : "Unsoloed ") + labels (found) + ".", { { "tracks", ofTracks (found) } }));
            });
        });

    add ("track.volume", "Set tracks' level in dB (0 is unity), or move it by `change` dB.", "Volume",
        { P ("tracks", "tracks", targetHelp, true), withRange (P ("db", "number", "Level in dB, e.g. -6"), -100, 12),
          withRange (P ("change", "number", "Or: raise (+) / lower (-) the current level by this many dB"), -60, 60) },
        [=, &edit] (const juce::var& args, Complete complete)
        {
            if (! args.hasProperty ("db") && ! args.hasProperty ("change")) { complete (fail ("Give \"db\" (a level) or \"change\" (dB to add).")); return; }
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                for (const auto id : found)
                {
                    const auto level = args.hasProperty ("db") ? (double) args["db"] : number (edit.getTrack (id), ids::volume) + (double) args["change"];
                    call ("setTrack", obj ({ { "id", id }, { "volume", juce::jlimit (-100.0, 12.0, level) } }));
                }
                complete (done ("Volume of " + labels (found) + (args.hasProperty ("db") ? " set to " + juce::String ((double) args["db"], 1) + " dB."
                                                                                           : " changed by " + juce::String ((double) args["change"], 1) + " dB."),
                                { { "tracks", ofTracks (found) } }));
            });
        });

    add ("track.pan", "Pan tracks: -1 is left, 0 centre, 1 right.", "Pan",
        { P ("tracks", "tracks", targetHelp, true), withRange (P ("pan", "number", "-1 to 1", true), -1, 1) },
        [=] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                for (const auto id : found) call ("setTrack", obj ({ { "id", id }, { "pan", (double) args["pan"] } }));
                complete (done ("Panned " + labels (found) + " to " + juce::String ((double) args["pan"], 2) + ".", { { "tracks", ofTracks (found) } }));
            });
        });

    add ("track.role", "Tag tracks with a role (voice, background, instrumental, guitar, piano, pad, synth, bass, drums, instrument, other) so \"the guitars\" or \"instrumental tracks\" can be addressed; \"auto\" forgets the tag.", "Tag track",
        { P ("tracks", "tracks", targetHelp, true), P ("role", "string", "The role key, or auto", true) },
        [=, &edit] (const juce::var& args, Complete complete)
        {
            const auto key = lower (args["role"].toString());
            auto valid = key == "auto";
            for (const auto role : analysis::allRoles) valid = valid || key == analysis::getRoleKey (role);
            if (! valid) { complete (fail ("Unknown role \"" + key + "\". Roles: voice, background, instrumental, guitar, piano, pad, synth, bass, drums, instrument, other, auto.")); return; }
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                for (const auto id : found)
                    call ("autoMix", obj ({ { "action", "tag" }, { "track", id }, { "role", key } }));
                complete (done ("Tagged " + labels (found) + " as " + key + ".", { { "tracks", ofTracks (found) } }));
            });
        });

    add ("track.transpose", "Change the key of tracks by semitones (e.g. -3), all their audio and MIDI. Set 0 to undo.", "Track key",
        { P ("tracks", "tracks", targetHelp, true), withRange (P ("semitones", "number", "Semitones, up (+) or down (-)", true), -24, 24),
          P ("relative", "boolean", "Add to the current transposition instead of replacing it") },
        [=, &edit] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                const auto relative = (bool) args.getProperty ("relative", false);
                for (const auto id : found)
                {
                    const auto semitones = (double) args["semitones"] + (relative ? number (edit.getTrack (id), ids::transpose) : 0.0);
                    call ("setTrack", obj ({ { "id", id }, { "transpose", semitones } }));
                }
                complete (done ("Transposed " + labels (found) + (relative ? " by " : " to ") + juce::String ((double) args["semitones"], 1) + " semitones.",
                                { { "tracks", ofTracks (found) } }));
            });
        });

    //============================================================================ effects and sends
    add ("effect.add", "Add an effect to tracks (reverb, echo, eq, compressor, deEsser, exciter, saturation, chorus, ...). `values` sets its parameters.", "Add effect",
        { P ("tracks", "tracks", targetHelp, true), P ("type", "string", "The effect's key, e.g. reverb, compressor, eq", true),
          P ("values", "object", "Starting parameters { name: number } (see track.get for names)") },
        [=] (const juce::var& args, Complete complete)
        {
            const auto type = args["type"].toString().trim();
            if (! daw::Edit::moduleTypeFromKey (type).has_value())
            {
                juce::StringArray keys;
                for (int i = 0; i < fx::numModuleTypes; ++i) keys.add (fx::getModuleKey ((fx::ModuleType) i));
                complete (fail ("There is no effect \"" + type + "\". Effects: " + keys.joinIntoString (", ") + "."));
                return;
            }
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                juce::Array<juce::var> added;
                for (const auto id : found)
                {
                    auto a = obj ({ { "track", id }, { "type", type } });
                    if (args.hasProperty ("values")) a.getDynamicObject()->setProperty ("values", args["values"]);
                    const auto insert = call ("addInsert", a);
                    if (! insert.isInt() || (int) insert < 0) { complete (fail ("Couldn't add " + type + " to " + trackLabel (id) + ".")); return; }
                    added.add (insert);
                }
                complete (done ("Added " + type + " to " + labels (found) + ".", { { "tracks", ofTracks (found) }, { "inserts", added } }));
            });
        });

    add ("effect.remove", "Remove an effect (the first of that type) from tracks.", "Remove effect",
        { P ("tracks", "tracks", targetHelp, true), P ("type", "string", "The effect's key, e.g. reverb", true) },
        [=, &edit] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                auto removed = 0;
                for (const auto id : found)
                    for (const auto& insert : daw::Edit::getInserts (edit.getTrack (id)))
                        if (insert[ids::type].toString() == args["type"].toString().trim())
                        {
                            call ("removeInsert", obj ({ { "id", (int) insert[ids::id] } }));
                            ++removed;
                            break;
                        }
                if (removed == 0) { complete (fail ("None of " + labels (found) + " has a " + args["type"].toString() + " effect.")); return; }
                complete (done ("Removed " + args["type"].toString() + " from " + juce::String (removed) + " track(s).", { { "tracks", ofTracks (found) } }));
            });
        });

    add ("effect.set", "Change one parameter of an effect (the first of that type) on tracks. Parameter names come from track.get.", "Effect change",
        { P ("tracks", "tracks", targetHelp, true), P ("type", "string", "The effect's key, e.g. reverb", true),
          P ("param", "string", "The parameter's name", true), P ("value", "number", "The new value", true) },
        [=, &edit] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                auto changed = 0;
                for (const auto id : found)
                    for (const auto& insert : daw::Edit::getInserts (edit.getTrack (id)))
                        if (insert[ids::type].toString() == args["type"].toString().trim())
                        {
                            if (! insert.hasProperty (juce::Identifier (args["param"].toString())))
                            { complete (fail (args["type"].toString() + " has no parameter \"" + args["param"].toString() + "\". See track.get for its names.")); return; }
                            call ("setInsert", obj ({ { "id", (int) insert[ids::id] }, { "param", args["param"] }, { "value", (double) args["value"] } }));
                            ++changed;
                            break;
                        }
                if (changed == 0) { complete (fail ("None of " + labels (found) + " has a " + args["type"].toString() + " effect. Add it with effect.add.")); return; }
                complete (done ("Set " + args["type"].toString() + " " + args["param"].toString() + " to " + juce::String ((double) args["value"], 2) + " on " + juce::String (changed) + " track(s).",
                                { { "tracks", ofTracks (found) } }));
            });
        });

    add ("return.add", "Add a return track (a shared effect for sends), e.g. a reverb or echo return.", "Add return",
        { P ("name", "string", "Name"), P ("effect", "string", "The effect's key, e.g. reverb") },
        [this] (const juce::var& args, Complete complete)
        {
            auto a = obj ({});
            if (args.hasProperty ("name")) a.getDynamicObject()->setProperty ("name", args["name"]);
            if (args.hasProperty ("effect")) a.getDynamicObject()->setProperty ("effect", args["effect"]);
            const auto id = call ("addReturn", a);
            complete (done ("Added return " + processor.getDawEdit().getReturn ((int) id)[ids::name].toString() + ".", { { "return", id } }));
        });

    add ("send.set", "Send tracks to a return (e.g. \"Reverb\") at a level in dB (-100 or less: off).", "Send level",
        { P ("tracks", "tracks", targetHelp, true), P ("return", "string", "The return's id or name", true), withRange (P ("db", "number", "Send level in dB, e.g. -12", true), -100, 6) },
        [=] (const juce::var& args, Complete complete)
        {
            forTracks (args, "tracks", complete, [&] (const std::vector<int>& found)
            {
                juce::String problem;
                const auto target = resolveReturn (args["return"], problem);
                if (target < 0) { complete (fail (problem)); return; }
                for (const auto id : found) call ("setSend", obj ({ { "id", id }, { "target", target }, { "level", (double) args["db"] } }));
                complete (done ("Sent " + labels (found) + " to " + processor.getDawEdit().getReturn (target)[ids::name].toString() + " at " + juce::String ((double) args["db"], 1) + " dB.",
                                { { "tracks", ofTracks (found) } }));
            });
        });

    //============================================================================ AI mix
    add ("mix.auto", "Run Auto Mix: listen to the song, plan a mix, and apply it (apply: false to only plan). Takes a while on a long song.", "Auto Mix",
        { withChoices (P ("style", "string", "pop (default), ballad, rock, coaching, hipHop, rnb, acoustic, electronic, jazz, lofi, speech"),
                       { "pop", "ballad", "rock", "coaching", "hiphop", "rnb", "acoustic", "electronic", "jazz", "lofi", "speech" }),
          withChoices (P ("mode", "string", "mix (whole song, default), fit, clean or master"), { "mix", "fit", "clean", "master" }),
          P ("apply", "boolean", "Apply the plan (default true)") },
        [this] (const juce::var& args, Complete complete)
        {
            if (waitingAutoMix) { complete (fail ("Auto Mix is already listening.")); return; }
            autoMixApply = (bool) args.getProperty ("apply", true);
            auto a = obj ({ { "action", "analyse" } });
            if (args.hasProperty ("style")) a.getDynamicObject()->setProperty ("style", args["style"]);
            if (args.hasProperty ("mode")) a.getDynamicObject()->setProperty ("mode", args["mode"]);
            waitingAutoMix = std::move (complete);
            const auto started = call ("autoMix", a);
            if (! (bool) started["ok"]) { auto finish = std::move (waitingAutoMix); waitingAutoMix = nullptr; finish (started); }
        });

    add ("mix.check", "Run Mix Check: measure the mix and list problems with suggested fixes.", "",
        { P ("style", "string", "The style to judge by (default pop)") },
        [this] (const juce::var& args, Complete complete)
        {
            if (waitingMixCheck) { complete (fail ("Mix Check is already running.")); return; }
            auto a = obj ({ { "action", "run" } });
            if (args.hasProperty ("style")) a.getDynamicObject()->setProperty ("style", args["style"]);
            waitingMixCheck = std::move (complete);
            const auto started = call ("mixCheck", a);
            if (! (bool) started["ok"]) { auto finish = std::move (waitingMixCheck); waitingMixCheck = nullptr; finish (started); }
        });

    add ("mix.fix", "Apply one of Mix Check's suggested fixes, by its index in mix.report.", "Mix Check fix",
        { withRange (P ("index", "integer", "The finding's index", true), 0, 999) },
        [this] (const juce::var& args, Complete complete)
        {
            const auto result = call ("mixCheck", obj ({ { "action", "fix" }, { "index", (int) args["index"] } }));
            complete ((bool) result["ok"] ? done ("Applied fix " + juce::String ((int) args["index"]) + ".") : result);
        });

    //============================================================================ history
    add ("history.undo", "Undo the last change (any action is one step).", "", {},
        [&edit] (const juce::var&, Complete complete)
        {
            auto& undo = edit.getUndoManager();
            if (! undo.canUndo()) { complete (fail ("Nothing to undo.")); return; }
            const auto name = undo.getUndoDescription();
            undo.undo();
            complete (done ("Undid " + name + "."));
        });

    add ("history.redo", "Redo what was just undone.", "", {},
        [&edit] (const juce::var&, Complete complete)
        {
            auto& undo = edit.getUndoManager();
            if (! undo.canRedo()) { complete (fail ("Nothing to redo.")); return; }
            const auto name = undo.getRedoDescription();
            undo.redo();
            complete (done ("Redid " + name + "."));
        });
}
