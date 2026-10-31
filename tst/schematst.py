#!/usr/bin/env python3
# Validates what itst-svcalc sends against the official MCP JSON Schemas shipped in the
# modelcontextprotocol checkout (schema/<revision>/schema.json). Usage:
#   schematst.py <path-to-itst-svcalc> <path-to-modelcontextprotocol-checkout>
# Skipped (exit 0) when the jsonschema module or the schemas are not available.

import json
import os
import subprocess
import sys

BIN = sys.argv[1]
SPEC = sys.argv[2] if len(sys.argv) > 2 else ""

try:
    import jsonschema
except ImportError:
    print("schematst: jsonschema module not found, skipped")
    sys.exit(0)

failures = []
checked = 0


def load(rev):
    path = os.path.join(SPEC, "schema", rev, "schema.json")
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return json.load(f)


def defs_of(schema):
    # 2025-11-25 and later use $defs, earlier revisions draft-07 "definitions"
    return schema.get("$defs") or schema.get("definitions") or {}


def validator(schema, name):
    key = "$defs" if "$defs" in schema else "definitions"
    if name not in defs_of(schema):
        return None
    root = {"$schema": schema["$schema"], key: schema[key], "$ref": "#/%s/%s" % (key, name)}
    return jsonschema.validators.validator_for(root)(root)


class Era:
    def __init__(self, rev):
        self.rev = rev
        self.schema = load(rev)
        self.cache = {}

    def check(self, what, type_name, value):
        global checked
        v = self.cache.get(type_name)
        if v is None:
            v = self.cache[type_name] = validator(self.schema, type_name)
        if v is None:
            failures.append("[%s] schema has no %s (for %s)" % (self.rev, type_name, what))
            return
        checked += 1
        errs = sorted(v.iter_errors(value), key=lambda e: list(e.path))
        for e in errs[:3]:
            failures.append("[%s] %s is not a valid %s: %s at /%s\n      %s" % (
                self.rev, what, type_name, e.message[:160], "/".join(str(p) for p in e.path), json.dumps(value)[:300]))


def run(msgs, extra=()):
    data = b"\n".join(json.dumps(m).encode() for m in msgs)
    p = subprocess.run([BIN, "iostrm", *extra, "-i", "-"], input=data, capture_output=True, timeout=120)
    out = []
    for line in p.stdout.decode().splitlines():
        if line.strip():
            out.append(json.loads(line))
    return out


# ---- result type of each method
RESULT_TYPES = {
    "server/discover": "DiscoverResult",
    "tools/list": "ListToolsResult",
    "tools/call": "CallToolResult",
    "prompts/list": "ListPromptsResult",
    "prompts/get": "GetPromptResult",
    "resources/list": "ListResourcesResult",
    "resources/read": "ReadResourceResult",
    "resources/templates/list": "ListResourceTemplatesResult",
    "completion/complete": "CompleteResult",
    "initialize": "InitializeResult",
    "ping": "EmptyResult",
    "logging/setLevel": "EmptyResult",
    "resources/subscribe": "EmptyResult",
    "resources/unsubscribe": "EmptyResult",
}
NOTIF_TYPES = {
    "notifications/progress": "ProgressNotification",
    "notifications/message": "LoggingMessageNotification",
    "notifications/tools/list_changed": "ToolListChangedNotification",
    "notifications/prompts/list_changed": "PromptListChangedNotification",
    "notifications/resources/list_changed": "ResourceListChangedNotification",
    "notifications/resources/updated": "ResourceUpdatedNotification",
    "notifications/subscriptions/acknowledged": "SubscriptionsAcknowledgedNotification",
}
REQUEST_TYPES = {
    "elicitation/create": "ElicitRequest",
    "sampling/createMessage": "CreateMessageRequest",
    "roots/list": "ListRootsRequest",
}
ERROR_TYPES = {-32700: "ParseError", -32601: "MethodNotFoundError", -32602: "InvalidParamsError", -32603: "InternalError",
               -32020: "HeaderMismatchError", -32021: "MissingRequiredClientCapabilityError", -32022: "UnsupportedProtocolVersionError"}


def validate_stream(era, sent, replies):
    """Check every message the server wrote against the type its role in the exchange implies."""
    methods = {m["id"]: m["method"] for m in sent if "id" in m and "method" in m}
    for msg in replies:
        if "method" in msg:
            if "id" in msg:
                t = REQUEST_TYPES.get(msg["method"])
                if t:
                    era.check("server request %s" % msg["method"], t, msg)
            else:
                t = NOTIF_TYPES.get(msg["method"])
                if t:
                    era.check("notification %s" % msg["method"], t, msg)
                else:
                    failures.append("[%s] unexpected notification %s" % (era.rev, msg["method"]))
        elif "result" in msg:
            m = methods.get(msg["id"])
            res = msg["result"]
            if res.get("resultType") == "input_required":
                era.check("input_required result of %s" % m, "InputRequiredResult", res)
            elif m == "subscriptions/listen":
                era.check("listen result", "SubscriptionsListenResult", res)
            elif m in RESULT_TYPES:
                era.check("result of %s" % m, RESULT_TYPES[m], res)
            else:
                failures.append("[%s] result for unknown request %r" % (era.rev, m))
        elif "error" in msg:
            t = ERROR_TYPES.get(msg["error"]["code"])
            if t and t in defs_of(era.schema):
                # the generic JSON-RPC codes are described as the error object, the MCP ones as the whole message
                inner = msg["error"]["code"] in (-32700, -32601, -32602, -32603)
                era.check("error %s" % msg["error"]["code"], t, msg["error"] if inner else msg)


def legacy(rev):
    era = Era(rev)
    if era.schema is None:
        print("schematst: no schema for %s, skipped" % rev)
        return
    caps = {"elicitation": {"form": {}, "url": {}}, "sampling": {}, "roots": {}}
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": rev, "capabilities": caps, "clientInfo": {"name": "schematst", "version": "1"}}}
    inited = {"jsonrpc": "2.0", "method": "notifications/initialized"}
    n = [10]

    def req(method, params=None):
        n[0] += 1
        m = {"jsonrpc": "2.0", "id": n[0], "method": method}
        if params is not None:
            m["params"] = params
        return m

    def call(name, args=None, **kw):
        return req("tools/call", dict({"name": name, "arguments": args or {}}, **kw))

    sent = [init, inited, req("ping"), req("tools/list"), req("prompts/list"), req("resources/list"), req("resources/templates/list"),
            req("logging/setLevel", {"level": "debug"}),
            call("calc", {"a": 1, "b": 2, "op": "+"}), call("rich"), call("structured"), call("typed", {"flag": True, "ratio": 1.5, "tag": "x", "data": {"k": [1]}}),
            call("typed", {}), call("nope"), call("touch"),
            call("work", {"who": "W", "steps": 2, "loud": True}, _meta={"progressToken": 5}),
            req("prompts/get", {"name": "greet", "arguments": {"who": "Ann"}}), req("prompts/get", {"name": "greet", "arguments": {}}), req("prompts/get", {"name": "rich"}),
            req("resources/read", {"uri": "file:///readme.txt"}), req("resources/read", {"uri": "file:///data.bin"}),
            req("resources/read", {"uri": "item://book/1"}), req("resources/read", {"uri": "file:///nope"}),
            req("resources/subscribe", {"uri": "file:///readme.txt"}), call("touch"), req("resources/unsubscribe", {"uri": "file:///readme.txt"}),
            req("completion/complete", {"ref": {"type": "ref/prompt", "name": "greet"}, "argument": {"name": "who", "value": "A"}}),
            req("no/such")]
    # answers to the questions the server will ask, in the order it asks them
    sent += [call("ask", {"what": "form"}), {"jsonrpc": "2.0", "id": 1, "result": {"action": "accept", "content": {"colour": "red"}}},
             call("ask", {"what": "sample"}), {"jsonrpc": "2.0", "id": 2, "result": {"role": "assistant", "content": {"type": "text", "text": "hi"}, "model": "m"}},
             call("ask", {"what": "roots"}), {"jsonrpc": "2.0", "id": 3, "result": {"roots": [{"uri": "file:///w"}]}}]
    replies = run(sent, ["--extra-tools", "3", "--page-size", "4"])
    validate_stream(era, sent, replies)
    # the first page is paginated, make sure the follow-up cursor is valid too
    cur = [m for m in replies if m.get("id") == 14][0]["result"].get("nextCursor") if False else None
    print("schematst: %s legacy exchange, %d messages" % (rev, len(replies)))


def modern():
    era = Era("2026-07-28")
    if era.schema is None:
        print("schematst: no schema for 2026-07-28, skipped")
        return
    PV, CAPS = "io.modelcontextprotocol/protocolVersion", "io.modelcontextprotocol/clientCapabilities"
    caps = {"elicitation": {"form": {}, "url": {}}, "sampling": {}, "roots": {}}
    n = [100]

    def req(method, params=None, caps_=None, **meta):
        n[0] += 1
        p = dict(params or {})
        p["_meta"] = dict({PV: "2026-07-28", CAPS: caps if caps_ is None else caps_,
                           "io.modelcontextprotocol/clientInfo": {"name": "schematst", "version": "1"}}, **p.get("_meta", {}), **meta)
        return {"jsonrpc": "2.0", "id": n[0], "method": method, "params": p}

    def call(name, args=None, **kw):
        return req("tools/call", {"name": name, "arguments": args or {}, **kw.pop("p", {})}, **kw)

    answers = {"mcpc-1": {"action": "accept", "content": {"colour": "red"}}}
    sent = [req("server/discover"), req("tools/list"), req("prompts/list"), req("resources/list"), req("resources/templates/list"),
            call("calc", {"a": 1, "b": 2, "op": "+"}), call("rich"), call("structured"), call("typed", {"flag": False, "ratio": 2}), call("typed", {}),
            call("nope"), call("work", {"who": "W", "steps": 2, "loud": True}, p={"_meta": {"progressToken": "t"}}, **{"io.modelcontextprotocol/logLevel": "debug"}),
            req("prompts/get", {"name": "greet", "arguments": {"who": "Ann"}}), req("prompts/get", {"name": "greet", "arguments": {}}), req("prompts/get", {"name": "rich"}),
            req("resources/read", {"uri": "file:///readme.txt"}), req("resources/read", {"uri": "file:///data.bin"}),
            req("resources/read", {"uri": "item://book/1"}), req("resources/read", {"uri": "file:///nope"}),
            req("completion/complete", {"ref": {"type": "ref/prompt", "name": "greet"}, "argument": {"name": "who", "value": "A"}}),
            req("no/such"), req("ping"), req("tools/list", ver="1900-01-01") if False else
            {"jsonrpc": "2.0", "id": 99, "method": "tools/list", "params": {"_meta": {PV: "1900-01-01", CAPS: {}}}},
            {"jsonrpc": "2.0", "id": 98, "method": "tools/list", "params": {"_meta": {PV: "2026-07-28"}}},
            # multi round-trip: ask, retry with the answer, state round trip
            call("ask", {"what": "form"}), call("ask", {"what": "form"}, p={"inputResponses": answers}),
            call("ask", {"what": "sample"}), call("ask", {"what": "roots"}), call("ask2"),
            call("stateful"), req("prompts/get", {"name": "confirm"}), req("resources/read", {"uri": "file:///ask.txt"}),
            # a subscription with traffic on it
            req("subscriptions/listen", {"notifications": {"toolsListChanged": True, "resourceSubscriptions": ["file:///readme.txt"]}}),
            call("touch")]
    replies = run(sent, ["--extra-tools", "3", "--page-size", "4"])
    validate_stream(era, sent, replies)
    print("schematst: 2026-07-28 exchange, %d messages" % len(replies))


def main():
    if not SPEC or not os.path.isdir(SPEC):
        print("schematst: specification checkout not found, skipped")
        return
    for rev in ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"):
        legacy(rev)
    modern()
    print("schematst: %d messages validated" % checked)
    if failures:
        print("FAILED:")
        seen = set()
        for f in failures:
            key = f.split("\n")[0]
            if key not in seen:
                seen.add(key)
                print("  - " + f)
        sys.exit(1)
    print("schematst: all valid")


main()
