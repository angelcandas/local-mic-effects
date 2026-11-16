// micfx-mcp: the stdio side of MicFX's AI control.
//
// Desktop AI programs start MCP servers as commands and talk to them over stdin and stdout, one
// JSON message per line. The server itself lives inside the running MicFX app (src/McpServer.h);
// this helper only passes each line on to it over a local connection, and passes the answer back.
// It finds the app through the file the app writes while "AI control" is on (McpServer::getDiscoveryFile).
//
// Nothing but protocol messages goes to stdout; messages for a person go to stderr.

#include <juce_core/juce_core.h>
#include <iostream>
#include <string>

namespace
{
    juce::File discoveryFile()
    {
        if (const auto override = juce::SystemStats::getEnvironmentVariable ("MICFX_MCP_FILE", {}); override.isNotEmpty())
            return juce::File (override);

        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               #if JUCE_MAC
                   .getChildFile ("Application Support")
               #endif
                   .getChildFile ("MicFX").getChildFile ("mcp.json");
    }

    juce::var errorLine (const juce::var& id, const juce::String& message)
    {
        auto* error = new juce::DynamicObject();
        error->setProperty ("code", -32603);
        error->setProperty ("message", message);
        auto* response = new juce::DynamicObject();
        response->setProperty ("jsonrpc", "2.0");
        response->setProperty ("id", id);
        response->setProperty ("error", juce::var (error));
        return juce::var (response);
    }

    void write (const juce::var& message)
    {
        std::cout << juce::JSON::toString (message, true).toStdString() << "\n" << std::flush;
    }

    // POSTs `body` to the app. Returns the HTTP status (0: couldn't connect) and fills `answer`.
    int post (int port, const juce::String& token, const juce::String& body, juce::String& answer)
    {
        juce::StreamingSocket socket;
        if (! socket.connect ("127.0.0.1", port, 3000))
            return 0;

        const auto request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + juce::String (port) + "\r\n"
                           + "Authorization: Bearer " + token + "\r\n"
                           + "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
                           + "Content-Length: " + juce::String ((int) body.getNumBytesAsUTF8()) + "\r\nConnection: close\r\n\r\n" + body;
        if (socket.write (request.toRawUTF8(), (int) request.getNumBytesAsUTF8()) < 0)
            return 0;

        std::string data;
        char chunk[8192];
        for (;;)
        {
            if (socket.waitUntilReady (true, 30 * 60 * 1000) != 1) break; // Auto Mix can take minutes
            const auto n = socket.read (chunk, sizeof (chunk), false);
            if (n <= 0) break;
            data.append (chunk, (size_t) n);
        }

        const auto headerEnd = data.find ("\r\n\r\n");
        if (headerEnd == std::string::npos)
            return 0;

        const auto status = juce::String::fromUTF8 (data.data(), (int) headerEnd).fromFirstOccurrenceOf (" ", false, false).getIntValue();
        answer = juce::String::fromUTF8 (data.data() + headerEnd + 4, (int) (data.size() - headerEnd - 4));
        return status;
    }
}

int main()
{
    std::string line;
    while (std::getline (std::cin, line))
    {
        const auto text = juce::String::fromUTF8 (line.c_str()).trim();
        if (text.isEmpty())
            continue;

        const auto message = juce::JSON::parse (text);
        const auto isRequest = message.isObject() && message.hasProperty ("id");
        const auto id = message["id"];
        const auto fail = [&] (const juce::String& why)
        {
            std::cerr << why << std::endl;
            if (isRequest) write (errorLine (id, why));
        };

        if (! message.isObject())
        {
            write (errorLine (juce::var(), "That isn't JSON."));
            continue;
        }

        const auto discovery = juce::JSON::parse (discoveryFile());
        const auto port = (int) discovery["port"];
        if (port <= 0)
        {
            fail ("MicFX isn't ready for AI control. Open MicFX, then Settings, and turn on AI control.");
            continue;
        }

        juce::String answer;
        const auto status = post (port, discovery["token"].toString(), text, answer);
        if (status == 0)
            fail ("Couldn't reach MicFX. Is the app open with AI control turned on?");
        else if (status == 401)
            fail ("MicFX refused the token. Turn AI control off and on again, then restart your AI program.");
        else if (status == 202 || answer.trim().isEmpty())
            continue; // a notification: nothing to say back
        else if (const auto response = juce::JSON::parse (answer); response.isObject())
            write (response);
        else
            fail ("MicFX answered something that isn't JSON (HTTP " + juce::String (status) + ").");
    }
    return 0;
}
