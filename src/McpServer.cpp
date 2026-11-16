#include "McpServer.h"

#if JUCE_MAC || JUCE_LINUX
 #include <sys/stat.h>
#endif

namespace
{
    constexpr int maxHeaderBytes = 64 * 1024;
    constexpr int maxBodyBytes = 4 * 1024 * 1024;

    bool sameText (const juce::String& a, const juce::String& b)
    {
        // Without stopping at the first difference.
        auto diff = a.length() ^ b.length();
        for (int i = 0; i < juce::jmin (a.length(), b.length()); ++i)
            diff |= (int) a[i] ^ (int) b[i];
        return diff == 0;
    }

    bool isThisMachine (juce::String host)
    {
        host = host.trim().toLowerCase();
        if (host.startsWith ("["))
            host = host.upToFirstOccurrenceOf ("]", true, false);
        else
            host = host.upToFirstOccurrenceOf (":", false, false);
        return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    }

    void reply (juce::StreamingSocket& socket, int status, const juce::String& reason, const juce::String& body = {})
    {
        const auto text = "HTTP/1.1 " + juce::String (status) + " " + reason + "\r\n"
                        + "Content-Type: application/json\r\n"
                        + "Content-Length: " + juce::String ((int) body.getNumBytesAsUTF8()) + "\r\n"
                        + "Cache-Control: no-store\r\n"
                        + "Connection: close\r\n\r\n" + body;
        socket.write (text.toRawUTF8(), (int) text.getNumBytesAsUTF8());
    }
}

McpServer::McpServer (mcp::Backend b) : juce::Thread ("MicFX MCP"), backend (std::move (b)) {}

McpServer::~McpServer() { stop(); }

juce::String McpServer::makeToken()
{
    juce::Random random;
    random.setSeedRandomly();
    juce::MemoryBlock bytes (24);
    for (size_t i = 0; i < bytes.getSize(); ++i)
        bytes[i] = (char) random.nextInt (256);
    return juce::Base64::toBase64 (bytes.getData(), bytes.getSize()).replaceCharacters ("+/=", "-_x");
}

juce::File McpServer::getDiscoveryFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
               .getChildFile ("Application Support")
#endif
               .getChildFile ("MicFX").getChildFile ("mcp.json");
}

bool McpServer::writeDiscoveryFile (int port, const juce::String& token)
{
    const auto file = getDiscoveryFile();
    file.getParentDirectory().createDirectory();

    auto* o = new juce::DynamicObject();
    o->setProperty ("port", port);
    o->setProperty ("token", token);
    const auto text = juce::JSON::toString (juce::var (o), true);

    // Only you can read it: the token is the key to the app.
    file.deleteFile();
    if (! file.create().wasOk())
        return false;
    file.setReadOnly (false);
   #if JUCE_MAC || JUCE_LINUX
    ::chmod (file.getFullPathName().toRawUTF8(), 0600);
   #endif
    return file.replaceWithText (text);
}

void McpServer::removeDiscoveryFile() { getDiscoveryFile().deleteFile(); }

bool McpServer::start (int portNumber, const juce::String& key)
{
    stop();
    token = key;

    auto listening = false;
    if (portNumber > 0)
        listening = listener.createListener (portNumber, "127.0.0.1");
    else
    {
        juce::Random random;
        random.setSeedRandomly();
        for (int attempt = 0; attempt < 30 && ! listening; ++attempt)
            listening = listener.createListener (49152 + random.nextInt (16000), "127.0.0.1");
    }
    if (! listening)
        return false;

    port = listener.getBoundPort();
    pool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions().withNumberOfThreads (4).withThreadName ("MicFX MCP connection"));
    running = true;
    startThread();
    return true;
}

void McpServer::stop()
{
    if (! running)
        return;

    running = false;
    signalThreadShouldExit();
    stopThread (2000);
    listener.close();
    pool.reset(); // waits for the connections in flight
    port = 0;
}

void McpServer::run()
{
    while (! threadShouldExit())
    {
        if (listener.waitUntilReady (true, 250) != 1)
            continue;

        std::shared_ptr<juce::StreamingSocket> connection (listener.waitForNextConnection());
        if (connection != nullptr)
            pool->addJob ([this, connection] { serve (*connection); });
    }
}

// One request: POST /mcp with one JSON-RPC message; answers once and closes.
void McpServer::serve (juce::StreamingSocket& socket)
{
    // Headers.
    std::string data;
    char chunk[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos && (int) data.size() < maxHeaderBytes)
    {
        if (socket.waitUntilReady (true, 5000) != 1) return;
        const auto n = socket.read (chunk, sizeof (chunk), false);
        if (n <= 0) return;
        data.append (chunk, (size_t) n);
        headerEnd = data.find ("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) { reply (socket, 431, "Request Header Fields Too Large"); return; }

    const auto head = juce::String::fromUTF8 (data.data(), (int) headerEnd);
    auto lines = juce::StringArray::fromLines (head);
    const auto request = juce::StringArray::fromTokens (lines[0], " ", "");
    std::map<juce::String, juce::String> headers;
    for (int i = 1; i < lines.size(); ++i)
        if (const auto colon = lines[i].indexOfChar (':'); colon > 0)
            headers[lines[i].substring (0, colon).trim().toLowerCase()] = lines[i].substring (colon + 1).trim();

    const auto header = [&] (const char* name) { const auto it = headers.find (name); return it == headers.end() ? juce::String() : it->second; };

    // Who may talk to the app: this machine's own programs that hold the token.
    if (headers.count ("origin") > 0 && ! isThisMachine (header ("origin").fromFirstOccurrenceOf ("://", false, false)))
    { reply (socket, 403, "Forbidden", "{\"error\":\"Not allowed from a web page.\"}"); return; }
    if (header ("host").isNotEmpty() && ! isThisMachine (header ("host")))
    { reply (socket, 403, "Forbidden", "{\"error\":\"Wrong host.\"}"); return; }
    if (! sameText (header ("authorization"), "Bearer " + token))
    { reply (socket, 401, "Unauthorized", "{\"error\":\"Missing or wrong token.\"}"); return; }

    if (request.size() < 2 || request[0] != "POST" || request[1].upToFirstOccurrenceOf ("?", false, false) != "/mcp")
    { reply (socket, request.size() > 0 && request[0] != "POST" ? 405 : 404, request.size() > 0 && request[0] != "POST" ? "Method Not Allowed" : "Not Found"); return; }

    // Body.
    const auto length = header ("content-length").getIntValue();
    if (length <= 0 || length > maxBodyBytes) { reply (socket, 400, "Bad Request", "{\"error\":\"Send one JSON-RPC message.\"}"); return; }
    auto body = data.substr (headerEnd + 4);
    while ((int) body.size() < length)
    {
        if (socket.waitUntilReady (true, 5000) != 1) return;
        const auto n = socket.read (chunk, sizeof (chunk), false);
        if (n <= 0) return;
        body.append (chunk, (size_t) n);
    }

    const auto message = juce::JSON::parse (juce::String::fromUTF8 (body.data(), length));
    if (message.isVoid())
    {
        reply (socket, 400, "Bad Request", juce::JSON::toString (mcp::errorResponse (juce::var(), -32700, "That isn't JSON."), true));
        return;
    }

    const auto response = mcp::handle (message, backend);
    if (response.isVoid())
        reply (socket, 202, "Accepted");
    else
        reply (socket, 200, "OK", juce::JSON::toString (response, true));
}
