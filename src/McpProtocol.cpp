#include "McpProtocol.h"

namespace mcp
{
    namespace
    {
        const char* const metaVersion = "io.modelcontextprotocol/protocolVersion";
        const char* const metaServerInfo = "io.modelcontextprotocol/serverInfo";

        juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> properties)
        {
            auto* result = new juce::DynamicObject();
            for (const auto& [name, value] : properties)
                result->setProperty (name, value);
            return juce::var (result);
        }

        juce::StringArray supportedVersions() { return { latestVersion, "2025-11-25", "2025-06-18" }; }

        juce::var typeOf (std::initializer_list<const char*> types)
        {
            juce::Array<juce::var> list;
            for (const auto* t : types) list.add (t);
            return list.size() == 1 ? list[0] : juce::var (list);
        }

        // One argument as a JSON Schema property.
        juce::var toSchema (const juce::var& param)
        {
            const auto type = param["type"].toString();
            auto schema = obj ({});
            auto* o = schema.getDynamicObject();

            if (type == "track" || type == "tracks")
            {
                juce::Array<juce::var> any;
                any.add (obj ({ { "type", "string" } }));
                any.add (obj ({ { "type", "integer" } }));
                any.add (obj ({ { "type", "array" }, { "items", obj ({ { "type", typeOf ({ "string", "integer" }) } }) } }));
                o->setProperty ("oneOf", any);
            }
            else if (type == "time")
                o->setProperty ("type", typeOf ({ "string", "number" }));
            else if (type == "number" || type == "integer" || type == "boolean" || type == "string" || type == "object" || type == "array")
                o->setProperty ("type", type);
            else
                o->setProperty ("type", "string");

            if (param.hasProperty ("min")) o->setProperty ("minimum", param["min"]);
            if (param.hasProperty ("max")) o->setProperty ("maximum", param["max"]);
            if (param.hasProperty ("choices")) o->setProperty ("enum", param["choices"]);
            o->setProperty ("description", param["description"]);
            return schema;
        }

        juce::var textContent (const juce::String& text)
        {
            juce::Array<juce::var> content;
            content.add (obj ({ { "type", "text" }, { "text", text } }));
            return content;
        }

        // What the model reads: the one-line summary, then whatever else the action returned.
        juce::String describeResult (const juce::var& result)
        {
            if (! (bool) result.getProperty ("ok", true))
                return result["error"].toString().isEmpty() ? "The action failed." : result["error"].toString();

            auto text = result["summary"].toString();
            juce::DynamicObject::Ptr rest (new juce::DynamicObject());
            if (const auto* source = result.getDynamicObject())
                for (const auto& [name, value] : source->getProperties())
                    if (name.toString() != "ok" && name.toString() != "summary")
                        rest->setProperty (name, value);
            if (rest->getProperties().size() > 0)
                text += (text.isEmpty() ? "" : "\n") + juce::JSON::toString (juce::var (rest.get()), false);
            return text.isEmpty() ? "Done." : text;
        }

        juce::var resultWithMeta (juce::DynamicObject::Ptr result, const Backend& backend)
        {
            result->setProperty ("resultType", "complete");
            result->setProperty ("_meta", obj ({ { metaServerInfo, obj ({ { "name", backend.serverName }, { "version", backend.serverVersion } }) } }));
            return juce::var (result.get());
        }
    }

    juce::var errorResponse (const juce::var& id, int code, const juce::String& message, const juce::var& data)
    {
        auto error = obj ({ { "code", code }, { "message", message } });
        if (! data.isVoid())
            error.getDynamicObject()->setProperty ("data", data);
        return obj ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", error } });
    }

    juce::var toTool (const juce::var& action)
    {
        auto properties = obj ({});
        juce::Array<juce::var> required;
        if (const auto* params = action["params"].getArray())
            for (const auto& p : *params)
            {
                properties.getDynamicObject()->setProperty (p["name"].toString(), toSchema (p));
                if ((bool) p["required"])
                    required.add (p["name"]);
            }

        auto schema = obj ({ { "type", "object" }, { "properties", properties }, { "additionalProperties", false } });
        if (required.size() > 0)
            schema.getDynamicObject()->setProperty ("required", required);

        const auto readOnly = (bool) action["readOnly"];
        return obj ({ { "name", action["name"] }, { "title", action["name"] }, { "description", action["description"] },
                      { "inputSchema", schema },
                      { "annotations", obj ({ { "readOnlyHint", readOnly }, { "destructiveHint", false }, { "idempotentHint", readOnly },
                                              { "openWorldHint", false } }) } });
    }

    juce::var handle (const juce::var& request, const Backend& backend)
    {
        if (! request.isObject())
            return errorResponse (juce::var(), -32600, "A request is a JSON object.");

        const auto hasId = request.hasProperty ("id");
        const auto id = request["id"];
        const auto method = request["method"].toString();
        const auto params = request["params"];

        if (method.isEmpty() || request["jsonrpc"].toString() != "2.0")
            return hasId ? errorResponse (id, -32600, "Not a JSON-RPC 2.0 request.") : juce::var();

        // Notifications ("notifications/initialized", "notifications/cancelled"...) get no answer.
        if (! hasId)
            return {};

        const auto reply = [&] (juce::var result) { return obj ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }); };

        // The stateless protocol names its version in every request; refuse one we don't speak.
        if (const auto requested = params["_meta"][metaVersion].toString(); requested.isNotEmpty() && ! supportedVersions().contains (requested))
        {
            juce::Array<juce::var> supported;
            for (const auto& v : supportedVersions()) supported.add (v);
            return errorResponse (id, -32022, "Unsupported protocol version", obj ({ { "supported", supported }, { "requested", requested } }));
        }

        if (method == "server/discover")
        {
            juce::DynamicObject::Ptr result (new juce::DynamicObject());
            juce::Array<juce::var> versions;
            for (const auto& v : supportedVersions()) versions.add (v);
            result->setProperty ("supportedVersions", versions);
            result->setProperty ("capabilities", obj ({ { "tools", obj ({}) } }));
            if (backend.instructions.isNotEmpty()) result->setProperty ("instructions", backend.instructions);
            result->setProperty ("ttlMs", 300000);
            result->setProperty ("cacheScope", "private");
            return reply (resultWithMeta (result, backend));
        }

        // The older handshake: answer with the version the client asked for when we know it.
        if (method == "initialize")
        {
            const auto asked = params["protocolVersion"].toString();
            juce::DynamicObject::Ptr result (new juce::DynamicObject());
            result->setProperty ("protocolVersion", supportedVersions().contains (asked) && asked != latestVersion ? asked : juce::String ("2025-11-25"));
            result->setProperty ("capabilities", obj ({ { "tools", obj ({ { "listChanged", false } }) } }));
            result->setProperty ("serverInfo", obj ({ { "name", backend.serverName }, { "version", backend.serverVersion } }));
            if (backend.instructions.isNotEmpty()) result->setProperty ("instructions", backend.instructions);
            return reply (juce::var (result.get()));
        }

        if (method == "ping")
            return reply (obj ({}));

        if (method == "tools/list")
        {
            juce::Array<juce::var> tools;
            const auto listed = backend.listActions();
            if (const auto* actions = listed["actions"].getArray())
                for (const auto& action : *actions)
                    if (! (backend.hidden && backend.hidden (action["name"].toString())))
                        tools.add (toTool (action));

            juce::DynamicObject::Ptr result (new juce::DynamicObject());
            result->setProperty ("tools", tools);
            result->setProperty ("ttlMs", 300000);
            result->setProperty ("cacheScope", "private");
            return reply (resultWithMeta (result, backend));
        }

        if (method == "tools/call")
        {
            const auto name = params["name"].toString();
            if (name.isEmpty())
                return errorResponse (id, -32602, "tools/call needs a tool name.");

            auto known = false;
            const auto listed = backend.listActions();
            if (const auto* actions = listed["actions"].getArray())
                for (const auto& action : *actions)
                    known = known || (action["name"].toString() == name && ! (backend.hidden && backend.hidden (name)));
            if (! known)
                return errorResponse (id, -32602, "Unknown tool: " + name);

            const auto arguments = params.hasProperty ("arguments") && params["arguments"].isObject() ? params["arguments"] : obj ({});
            const auto outcome = backend.runAction (name, arguments);
            const auto failed = outcome.isObject() && ! (bool) outcome.getProperty ("ok", true);

            juce::DynamicObject::Ptr result (new juce::DynamicObject());
            result->setProperty ("content", textContent (describeResult (outcome)));
            if (outcome.isObject())
                result->setProperty ("structuredContent", outcome);
            result->setProperty ("isError", failed);
            return reply (resultWithMeta (result, backend));
        }

        return errorResponse (id, -32601, "Method not found: " + method);
    }
}
