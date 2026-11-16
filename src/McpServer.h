#pragma once

#include "McpProtocol.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <map>
#include <memory>

// The local MCP server (see src/McpProtocol.h): Streamable HTTP, POST /mcp, on 127.0.0.1 only.
//
// Every request needs "Authorization: Bearer <token>"; a request from a web page (an Origin
// that isn't this machine) or to a host name that isn't this machine's is refused, so a page
// in the browser can't reach it. A tool call blocks its connection's thread until the action
// finishes (the backend hands the work to the app's message thread), so there are a few
// connection threads. Never started unless the person turns "AI control" on.
class McpServer final : private juce::Thread
{
public:
    explicit McpServer (mcp::Backend);
    ~McpServer() override;

    // port 0: any free one. False when it couldn't listen.
    bool start (int port, const juce::String& token);
    void stop();
    bool isRunning() const noexcept { return running; }
    int getPort() const noexcept { return port; }

    // Where the helper `micfx-mcp` finds the running app: { port, token }, readable only by you.
    static juce::File getDiscoveryFile();
    static bool writeDiscoveryFile (int port, const juce::String& token);
    static void removeDiscoveryFile();

    static juce::String makeToken();

private:
    void run() override;
    void serve (juce::StreamingSocket&);

    mcp::Backend backend;
    juce::StreamingSocket listener;
    juce::String token;
    std::atomic<bool> running { false };
    int port = 0;
    std::unique_ptr<juce::ThreadPool> pool;
};
