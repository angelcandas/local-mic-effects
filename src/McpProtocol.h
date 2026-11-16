#pragma once

#include <juce_core/juce_core.h>
#include <functional>

// The Model Context Protocol, tools only, over the DAW action catalogue (src/DawActions.h).
//
// Pure JSON in, JSON out, so it can be tested without a socket. It speaks the stateless
// protocol of 2026-07-28 (no handshake: every request says its own version in params._meta,
// "server/discover" says what the server offers) and the older handshake of 2025-06-18 and
// 2025-11-25 ("initialize"), because clients in the field still use both.
namespace mcp
{
    inline constexpr const char* latestVersion = "2026-07-28";

    struct Backend
    {
        // DawActions::list(): { ok, actions: [{ name, description, readOnly, params }] }
        std::function<juce::var()> listActions;
        // Runs one action and returns its { ok, summary | error, ... }. Blocks until it finishes
        // (the server marshals this onto the app's message thread).
        std::function<juce::var (const juce::String& name, const juce::var& args)> runAction;
        // Actions that must not be offered to an outside client (none in the first version).
        std::function<bool (const juce::String& name)> hidden;
        juce::String serverName = "micfx", serverVersion = "1";
        juce::String instructions;
    };

    // One action as an MCP tool: { name, title, description, inputSchema, annotations }.
    juce::var toTool (const juce::var& action);

    // One JSON-RPC request or notification -> the response object, or a void var when there is
    // nothing to answer (notifications, and anything without an id).
    juce::var handle (const juce::var& request, const Backend&);

    // The JSON-RPC error object for { code, message }.
    juce::var errorResponse (const juce::var& id, int code, const juce::String& message, const juce::var& data = {});
}
