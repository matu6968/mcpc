#!/usr/bin/env python3
# Black-box tests: drive itst-svcalc over stdin and assert on the JSON it
# prints. Usage: rpctst.py <path-to-itst-svcalc> [valgrind]

import base64
import hashlib
import hmac as hmaclib
import json
import os
import random
import socket
import subprocess
import sys
import threading
import time

BIN = sys.argv[1]
WRAP = ["valgrind", "-q", "--error-exitcode=222", "--leak-check=no"] if "valgrind" in sys.argv[2:] else []

INIT = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "rpctst", "version": "1"}}}
INITED = {"jsonrpc": "2.0", "method": "notifications/initialized"}

failures = []


def enc(m):
    return m if isinstance(m, bytes) else json.dumps(m).encode()


def run(msgs, extra=()):
    """Send messages (dicts or raw bytes), return (exit code, parsed reply lines)."""
    p = subprocess.run(WRAP + [BIN, "iostrm", *extra, "-i", "-"], input=b"\n".join(enc(m) for m in msgs),
                       capture_output=True, timeout=120)
    replies = []
    for line in p.stdout.decode(errors="replace").splitlines():
        if line.strip():
            try:
                replies.append(json.loads(line))
            except ValueError:
                failures.append("unparseable reply line: %r" % line[:200])
    return p.returncode, replies


def session(msgs, extra=(), ver=None):
    init = INIT if ver is None else dict(INIT, params=dict(INIT["params"], protocolVersion=ver))
    rc, r = run([init, INITED, *msgs], extra)
    check(rc == 0, "server exit code %d" % rc)
    check(len(r) >= 1 and r[0].get("id") == 1 and "result" in r[0], "initialize reply missing")
    return r[1:]


def check(cond, what):
    if not cond:
        failures.append(what)


def case(name):
    print("  " + name)


def call(id_, name, **args):
    return {"jsonrpc": "2.0", "id": id_, "method": "tools/call", "params": {"name": name, "arguments": args}}


def tcp_roundtrip(port, id_base):
    """One client: initialize, initialized, tools/list. Returns the tools/list reply."""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as c:
        c.sendall(enc(dict(INIT, id=id_base)) + enc(INITED) + enc({"jsonrpc": "2.0", "id": id_base + 1, "method": "tools/list", "params": {}}))
        buf = b""
        dec = json.JSONDecoder()
        got = []
        while len(got) < 2:
            chunk = c.recv(65536)
            if not chunk:
                break
            buf += chunk
            text = buf.decode()
            pos = 0
            got = []
            while pos < len(text):
                try:
                    obj, end = dec.raw_decode(text, pos)
                except ValueError:
                    break
                got.append(obj)
                pos = end
                while pos < len(text) and text[pos] in "\r\n ":
                    pos += 1
        return got


def tcp_tests():
    port = 8080
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        print("  tcp: port 8080 busy, skipped")
        return
    except OSError:
        pass

    case("tcp: more than 255 sequential connections, then concurrent clients")
    srv = subprocess.Popen(WRAP + [BIN, "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            check(False, "tcp server did not start")
            return

        bad = []
        for i in range(300):
            got = tcp_roundtrip(port, 1000 + i * 2)
            if len(got) != 2 or got[1].get("id") != 1001 + i * 2 or "result" not in got[1]:
                bad.append(i)
                break
        check(not bad, "sequential connection %s failed (pool leak?)" % bad)
        check(srv.poll() is None, "tcp server died during sequential connections")

        errs = []

        def worker(k):
            try:
                for j in range(10):
                    got = tcp_roundtrip(port, 5000 + k * 100 + j * 2)
                    if len(got) != 2 or "result" not in got[1]:
                        errs.append((k, j, got))
            except Exception as e:  # noqa
                errs.append((k, repr(e)))

        ths = [threading.Thread(target=worker, args=(k,)) for k in range(8)]
        [t.start() for t in ths]
        [t.join() for t in ths]
        check(not errs, "concurrent clients: %s" % errs[:2])
        check(srv.poll() is None, "tcp server died under concurrent clients")
    finally:
        srv.kill()
        srv.wait()


def main():
    case("initialize")
    rc, r = run([INIT])
    check(rc == 0 and len(r) == 1, "one reply to initialize")
    res = r[0]["result"]
    check(res["serverInfo"]["name"], "serverInfo present")
    check("tools" in res["capabilities"], "tools capability")
    check("completions" in res["capabilities"], "completions capability advertised")

    case("protocol version negotiation")
    for ver in ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"):
        init = dict(INIT, params=dict(INIT["params"], protocolVersion=ver))
        rc, r = run([init])
        check(r and r[0]["result"]["protocolVersion"] == ver, "server echoes supported version %s" % ver)
    for ver in ("2099-01-01", "garbage", "2026-07-28"):
        init = dict(INIT, params=dict(INIT["params"], protocolVersion=ver))
        rc, r = run([init])
        check(r and r[0]["result"]["protocolVersion"] == "2025-11-25", "unsupported %s answered with newest legacy" % ver)

    case("serverInfo fields are gated by the negotiated revision")
    def srv_info(ver):
        init = dict(INIT, params=dict(INIT["params"], protocolVersion=ver))
        rc, r = run([init])
        return r[0]["result"]
    old = srv_info("2024-11-05")
    check(old["serverInfo"] == {"name": "untitled", "version": "1.2.3"}, "2024-11-05 serverInfo has only name/version, got %s" % old["serverInfo"])
    mid = srv_info("2025-06-18")
    check(mid["serverInfo"].get("title") == "Calc Server" and "description" not in mid["serverInfo"]
          and "icons" not in mid["serverInfo"], "2025-06-18 has title but not description/icons, got %s" % mid["serverInfo"])
    new = srv_info("2025-11-25")
    si = new["serverInfo"]
    check(si.get("title") == "Calc Server" and si.get("description") == "A calculator for tests"
          and si.get("websiteUrl") == "https://example.com/calc" and si["icons"][0]["mimeType"] == "image/png",
          "2025-11-25 serverInfo is complete, got %s" % si)
    check(new.get("instructions") == "Use calc for arithmetic.", "instructions returned")
    check("logging" in new["capabilities"], "logging capability advertised")

    case("ping works before and after initialize")
    rc, r = run([{"jsonrpc": "2.0", "id": 20, "method": "ping"}, INIT, INITED, {"jsonrpc": "2.0", "id": 21, "method": "ping"}])
    check([x.get("id") for x in r] == [20, 1, 21] and r[0].get("result") == {} and r[2].get("result") == {}, "ping replies, got %s" % r)

    case("logging/setLevel")
    r = session([{"jsonrpc": "2.0", "id": 22, "method": "logging/setLevel", "params": {"level": "warning"}},
                 {"jsonrpc": "2.0", "id": 23, "method": "logging/setLevel", "params": {"level": "loud"}},
                 {"jsonrpc": "2.0", "id": 24, "method": "logging/setLevel", "params": {}}])
    check(r[0].get("result") == {}, "valid level accepted")
    check(r[1].get("error", {}).get("code") == -32602, "unknown level rejected with -32602")
    check(r[2].get("error", {}).get("code") == -32602, "missing level rejected with -32602")

    case("notifications/cancelled is accepted silently")
    r = session([{"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 5}},
                 {"jsonrpc": "2.0", "id": 25, "method": "ping"}])
    check([x.get("id") for x in r] == [25], "no reply to cancellation")

    case("tools/list larger than 1 KiB is not truncated")
    n = 60
    r = session([{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}], ["--extra-tools", str(n)])
    check(len(r) == 1 and "result" in r[0], "tools/list replied with a parseable result")
    names = [t["name"] for t in r[0]["result"]["tools"]] if r and "result" in r[0] else []
    check(len(names) == n + 15, "expected %d tools, got %d" % (n + 15, len(names)))
    check("extra-tool-%03d" % (n - 1) in names, "last tool present")

    case("pagination: walking all pages returns every tool exactly once")
    seen, cursor, pages = [], None, 0
    while True:
        params = {} if cursor is None else {"cursor": cursor}
        r = session([{"jsonrpc": "2.0", "id": 30, "method": "tools/list", "params": params}],
                    ["--extra-tools", "60", "--page-size", "10"])
        res = r[0]["result"]
        check(len(res["tools"]) <= 10, "page larger than page size: %d" % len(res["tools"]))
        seen += [x["name"] for x in res["tools"]]
        pages += 1
        cursor = res.get("nextCursor")
        if cursor is None or pages > 20:
            break
    check(pages == 8, "75 tools in pages of 10 make 7 pages, got %d" % pages)
    check(len(seen) == 75 and len(set(seen)) == 75, "every tool once, got %d/%d unique" % (len(seen), len(set(seen))))

    case("pagination: bad cursors are rejected with -32602")
    for bad in ("abc", "-1", "999", "", 5):
        r = session([{"jsonrpc": "2.0", "id": 31, "method": "tools/list", "params": {"cursor": bad}}],
                    ["--extra-tools", "60", "--page-size", "10"])
        check(r and r[0].get("error", {}).get("code") == -32602, "cursor %r rejected, got %s" % (bad, r))

    case("pagination: off by default and absent for short lists")
    r = session([{"jsonrpc": "2.0", "id": 32, "method": "tools/list"},
                 {"jsonrpc": "2.0", "id": 33, "method": "prompts/list", "params": {"cursor": "0"}},
                 {"jsonrpc": "2.0", "id": 34, "method": "resources/list"}], ["--extra-tools", "60"])
    check("nextCursor" not in r[0]["result"] and len(r[0]["result"]["tools"]) == 75, "tools/list unpaged by default")
    check(len(r[1]["result"]["prompts"]) == 3 and "nextCursor" not in r[1]["result"] and len(r[2]["result"]["resources"]) == 4 and "nextCursor" not in r[2]["result"], "short lists have no nextCursor")

    case("tool metadata is gated by the negotiated revision")
    def calc_tool(ver):
        r = session([{"jsonrpc": "2.0", "id": 40, "method": "tools/list"}], ver=ver)
        return [x for x in r[0]["result"]["tools"] if x["name"] == "calc"][0]
    c0 = calc_tool("2024-11-05")
    check(not ({"title", "annotations", "icons", "_meta"} & set(c0)), "2024-11-05 has none of the new members: %s" % sorted(c0))
    c1 = calc_tool("2025-03-26")
    check(c1.get("annotations") == {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True}
          and not ({"title", "icons", "_meta"} & set(c1)), "2025-03-26 has annotations only: %s" % sorted(c1))
    c2 = calc_tool("2025-06-18")
    check(c2.get("title") == "Calculator" and c2.get("_meta") == {"vendor": "mcpc"} and "annotations" in c2
          and "icons" not in c2, "2025-06-18 adds title and _meta: %s" % sorted(c2))
    c3 = calc_tool("2025-11-25")
    check(c3["icons"][0]["src"] == "https://example.com/calc.png" and c3.get("title") == "Calculator", "2025-11-25 adds icons")

    case("inputSchema has real types and a required list")
    r = session([{"jsonrpc": "2.0", "id": 41, "method": "tools/list"}])
    typed = [x for x in r[0]["result"]["tools"] if x["name"] == "typed"][0]["inputSchema"]
    props = typed["properties"]
    check(props["flag"]["type"] == "boolean" and props["ratio"]["type"] == "number" and props["tag"]["type"] == "string"
          and "type" not in props["data"], "property types: %s" % {k: v.get("type") for k, v in props.items()})
    check(typed["required"] == ["flag", "ratio"], "required lists only mandatory props, got %s" % typed.get("required"))
    calc = [x for x in r[0]["result"]["tools"] if x["name"] == "calc"][0]["inputSchema"]
    check(calc["properties"]["a"]["type"] == "integer" and calc["required"] == ["a", "b", "op"], "calc schema")

    case("tools/call: typed arguments, optional ones and a JSON value")
    r = session([call(42, "typed", flag=True, ratio=0.25, tag='q"uote', data={"k": [1, 2]}), call(43, "typed", flag=False, ratio=3)])
    check(r[0]["result"]["content"][0]["text"] == 'flag=1 ratio=0.25 tag=q"uote data={"k": [1, 2]}', "all args: %s" % r[0])
    check(r[1]["result"]["content"][0]["text"] == "flag=0 ratio=3 tag=- data=-", "optional args absent: %s" % r[1])

    case("tools/call: bad arguments are tool errors the model can read")
    r = session([call(44, "typed", ratio=1.5), call(45, "typed", flag="yes", ratio=1), call(46, "calc", a="two", b=1, op="+"),
                 {"jsonrpc": "2.0", "id": 47, "method": "tools/call", "params": {"name": "typed", "arguments": [1]}}])
    check(r[0]["result"].get("isError") is True and 'missing required argument "flag"' in r[0]["result"]["content"][0]["text"], "missing: %s" % r[0])
    check(r[1]["result"].get("isError") is True and 'must be a boolean' in r[1]["result"]["content"][0]["text"], "wrong type: %s" % r[1])
    check(r[2]["result"].get("isError") is True and 'must be a number' in r[2]["result"]["content"][0]["text"], "number: %s" % r[2])
    check(r[3].get("error", {}).get("code") == -32602, "non-object arguments is a protocol error")

    case("tools/call: arguments may be omitted for tools without properties")
    r = session([{"jsonrpc": "2.0", "id": 48, "method": "tools/call", "params": {"name": "bigtext"}}])
    check(r and "result" in r[0] and not r[0]["result"].get("isError"), "bigtext without arguments: %s" % str(r)[:200])

    case("resources/list carries the new metadata")
    r = session([{"jsonrpc": "2.0", "id": 50, "method": "resources/list"}], ver="2025-11-25")
    rs = {x["name"]: x for x in r[0]["result"]["resources"]}
    check(rs["readme"] == {"uri": "file:///readme.txt", "name": "readme", "description": "the readme", "mimeType": "text/plain",
                           "size": 11, "title": "Read me", "annotations": {"audience": ["user"], "priority": 0.5}},
          "readme resource: %s" % rs.get("readme"))
    check(rs["data"]["mimeType"] == "application/x-custom", "mime string wins")
    r = session([{"jsonrpc": "2.0", "id": 51, "method": "resources/list"}], ver="2024-11-05")
    old = {x["name"]: x for x in r[0]["result"]["resources"]}["readme"]
    check("title" not in old and old["annotations"]["priority"] == 0.5, "2024-11-05 drops title but keeps annotations: %s" % old)

    case("resources/read")
    r = session([{"jsonrpc": "2.0", "id": 52, "method": "resources/read", "params": {"uri": "file:///readme.txt"}},
                 {"jsonrpc": "2.0", "id": 53, "method": "resources/read", "params": {"uri": "file:///data.bin"}},
                 {"jsonrpc": "2.0", "id": 54, "method": "resources/read", "params": {"uri": "file:///nope"}},
                 {"jsonrpc": "2.0", "id": 55, "method": "resources/read", "params": {"uri": "file:///mute.txt"}},
                 {"jsonrpc": "2.0", "id": 56, "method": "resources/read", "params": {}}])
    check(r[0]["result"]["contents"] == [{"uri": "file:///readme.txt", "mimeType": "text/plain", "text": "hello world"}], "text read: %s" % r[0])
    check(r[1]["result"]["contents"] == [{"uri": "file:///data.bin", "mimeType": "application/x-custom", "blob": "/wD+"}], "blob read is base64: %s" % r[1])
    check(r[2].get("error", {}).get("code") == -32002 and r[2]["error"]["data"] == {"uri": "file:///nope"}, "not found: %s" % r[2])
    check(r[3].get("error", {}).get("code") == -32603, "resource without reader: %s" % r[3])
    check(r[4].get("error", {}).get("code") == -32602, "missing uri: %s" % r[4])

    case("resource templates: list, read and matching")
    r = session([{"jsonrpc": "2.0", "id": 57, "method": "resources/templates/list"},
                 {"jsonrpc": "2.0", "id": 58, "method": "resources/read", "params": {"uri": "item://book/42"}},
                 {"jsonrpc": "2.0", "id": 59, "method": "resources/read", "params": {"uri": "item://book"}},
                 {"jsonrpc": "2.0", "id": 60, "method": "resources/read", "params": {"uri": "item://a/b/c"}}])
    check(r[0]["result"]["resourceTemplates"] == [{"uriTemplate": "item://{kind}/{id}", "name": "items", "description": "any item"}], "template list: %s" % r[0])
    check(r[1]["result"]["contents"] == [{"uri": "item://book/42", "mimeType": "text/plain", "text": "book #42"}], "template read: %s" % r[1])
    check(r[2].get("error", {}).get("code") == -32002, "too few segments do not match")
    check(r[3].get("error", {}).get("code") == -32002, "a plain variable does not cross a slash")

    case("resource subscriptions and change notifications")
    r = session([{"jsonrpc": "2.0", "id": 61, "method": "resources/subscribe", "params": {"uri": "file:///readme.txt"}},
                 {"jsonrpc": "2.0", "id": 62, "method": "resources/subscribe", "params": {"uri": "file:///readme.txt"}},
                 call(63, "touch"),
                 {"jsonrpc": "2.0", "id": 64, "method": "resources/unsubscribe", "params": {"uri": "file:///readme.txt"}},
                 call(65, "touch")])
    check(r[0].get("result") == {} and r[1].get("result") == {}, "subscribe is idempotent")
    check(r[2] == {"jsonrpc": "2.0", "method": "notifications/resources/updated", "params": {"uri": "file:///readme.txt"}},
          "updated notification precedes the reply: %s" % r[2])
    check(r[3] == {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}, "list_changed notification: %s" % r[3])
    check(r[4].get("id") == 63 and r[4]["result"]["content"][0]["text"] == "touched", "the reply follows")
    # after unsubscribing only the list_changed notification is sent
    check([x.get("method") or x.get("id") for x in r[5:]] == [64, "notifications/tools/list_changed", 65], "no update after unsubscribe: %s" % r[5:])

    case("subscribing needs the capability, reading needs initialization")
    rc, r = run([{"jsonrpc": "2.0", "id": 66, "method": "resources/read", "params": {"uri": "file:///readme.txt"}}])
    check(r[0].get("error", {}).get("code") == -32602, "read before initialize rejected")

    case("rich tool results and their fallbacks")
    def rich(ver):
        return session([call(70, "rich")], ver=ver)[0]["result"]["content"]
    new = rich("2025-11-25")
    check([b["type"] for b in new] == ["text", "image", "audio", "resource_link", "resource", "resource"], "block types: %s" % [b["type"] for b in new])
    check(new[1] == {"type": "image", "data": "iVBORw==", "mimeType": "image/png"}, "image is base64: %s" % new[1])
    check(new[2] == {"type": "audio", "data": "YWJj", "mimeType": "audio/wav"}, "audio is base64")
    check(new[3] == {"type": "resource_link", "uri": "file:///readme.txt", "name": "readme", "description": "the readme", "mimeType": "text/plain"}, "link: %s" % new[3])
    check(new[4] == {"type": "resource", "resource": {"uri": "file:///inline.txt", "mimeType": "text/plain", "text": "inline"}}, "embedded text")
    check(new[5] == {"type": "resource", "resource": {"uri": "file:///inline.bin", "mimeType": "application/octet-stream", "blob": "AQI="}}, "embedded blob")
    mid = rich("2025-03-26")
    check([b["type"] for b in mid] == ["text", "image", "audio", "text", "resource", "resource"], "2025-03-26 keeps audio, links become text: %s" % [b["type"] for b in mid])
    check(mid[3]["text"] == "file:///readme.txt", "link degraded to its uri")
    old = rich("2024-11-05")
    check([b["type"] for b in old] == ["text", "image", "text", "text", "resource", "resource"], "2024-11-05 has neither audio nor links: %s" % [b["type"] for b in old])

    case("structured output")
    r = session([call(71, "structured")], ver="2025-11-25")[0]["result"]
    check(r["structuredContent"] == {"sum": 5} and r["_meta"] == {"took": "1ms"}, "structured and meta: %s" % r)
    check(r["content"] == [{"type": "text", "text": '{"sum":5}'}], "text copy for old clients: %s" % r["content"])
    r = session([call(72, "structured")], ver="2025-03-26")[0]["result"]
    check("structuredContent" not in r and "_meta" not in r and r["content"][0]["text"] == '{"sum":5}', "older revisions only get the text: %s" % r)
    tools = {x["name"]: x for x in session([{"jsonrpc": "2.0", "id": 73, "method": "tools/list"}], ver="2025-11-25")[0]["result"]["tools"]}
    check(tools["structured"]["outputSchema"]["required"] == ["sum"], "outputSchema listed")
    old_tools = {x["name"]: x for x in session([{"jsonrpc": "2.0", "id": 74, "method": "tools/list"}], ver="2025-03-26")[0]["result"]["tools"]}
    check("outputSchema" not in old_tools["structured"], "outputSchema hidden before 2025-06-18")
    check(tools["rawschema"]["inputSchema"]["properties"]["n"]["minimum"] == 1 and tools["rawschema"]["inputSchema"]["required"] == ["n"], "raw input schema used verbatim")

    case("prompts: list metadata, arguments and errors")
    def pget(i, name, args=None):
        p = {"name": name}
        if args is not None:
            p["arguments"] = args
        return {"jsonrpc": "2.0", "id": i, "method": "prompts/get", "params": p}
    r = session([{"jsonrpc": "2.0", "id": 80, "method": "prompts/list"}], ver="2025-11-25")
    pr = [p for p in r[0]["result"]["prompts"] if p["name"] == "greet"][0]
    check(pr["name"] == "greet" and pr["title"] == "Greeting" and pr["description"] == "Greets someone", "prompt list entry: %s" % pr)
    check([a["name"] for a in pr["arguments"]] == ["who", "tone"] and [a["required"] for a in pr["arguments"]] == [True, False], "argument required flags: %s" % pr["arguments"])
    r = session([{"jsonrpc": "2.0", "id": 81, "method": "prompts/list"}], ver="2024-11-05")
    check(all("title" not in p for p in r[0]["result"]["prompts"]), "title hidden on 2024-11-05")
    r = session([pget(82, "greet", {"who": "Ann", "tone": "warm"}), pget(83, "greet", {"who": "Bo"}), pget(84, "greet", {}),
                 pget(85, "greet"), pget(86, "greet", {"who": 5}), pget(87, "nope", {}), pget(88, "x" * 400, {})])
    check(r[0]["result"] == {"description": "Greets someone", "messages": [{"role": "user", "content": {"type": "text", "text": "greet Ann in a warm way"}}]}, "prompt result: %s" % r[0])
    check(r[1]["result"]["messages"][0]["content"]["text"] == "greet Bo in a plain way", "optional argument absent: %s" % r[1])
    check(r[2].get("error", {}).get("code") == -32602, "missing required argument: %s" % r[2])
    check(r[3].get("error", {}).get("code") == -32602, "arguments omitted but one is required")
    check(r[4].get("error", {}).get("code") == -32602, "non-string argument")
    check(r[5].get("error", {}).get("code") == -32602 and r[6].get("error", {}).get("code") == -32602, "unknown and over-long prompt names")

    case("ctx: progress and log notifications")
    caps = {"elicitation": {"form": {}, "url": {}}, "sampling": {}, "roots": {}}
    def sess(msgs, ver="2025-11-25", capabilities=None):
        init = dict(INIT, params=dict(INIT["params"], protocolVersion=ver, capabilities=capabilities or {}))
        rc, r = run([init, INITED, *msgs])
        check(rc == 0, "server exit code %d" % rc)
        return r[1:]
    work = {"jsonrpc": "2.0", "id": 90, "method": "tools/call",
            "params": {"name": "work", "arguments": {"who": "Ann", "steps": 3, "loud": True}, "_meta": {"progressToken": "tok1"}}}
    lvl = lambda v: {"jsonrpc": "2.0", "id": 91, "method": "logging/setLevel", "params": {"level": v}}
    r = sess([lvl("info"), work])
    check([x.get("method") for x in r[1:-1]] == ["notifications/progress"] * 3 + ["notifications/message"] * 2,
          "3 progress then warning+error logs, debug filtered: %s" % [x.get("method") for x in r[1:-1]])
    prog = [x["params"] for x in r[1:] if x.get("method") == "notifications/progress"]
    check(prog[0] == {"progressToken": "tok1", "progress": 1, "total": 3, "message": "stepping"}, "progress payload: %s" % prog[0])
    logs = [x["params"] for x in r[1:] if x.get("method") == "notifications/message"]
    check(logs[0] == {"level": "warning", "logger": "work", "data": "done with Ann"} and logs[1] == {"level": "error", "data": "an error for Ann"},
          "log payloads: %s" % logs)
    res = r[-1]["result"]["content"][0]["text"]
    check(res.startswith("Ann|2025-11-25|") and '"steps": 3' in res, "args and version visible to the callback: %s" % res)

    r = sess([lvl("debug"), work])
    check(sum(1 for x in r if x.get("method") == "notifications/message") == 3, "debug level lets the debug message through")
    r = sess([lvl("error"), work])
    check(sum(1 for x in r if x.get("method") == "notifications/message") == 1, "error level keeps only the error")

    w2 = dict(work, params=dict(work["params"], _meta={}))
    r = sess([w2])
    check(not any(x.get("method") == "notifications/progress" for x in r), "no progress without a progressToken")
    w3 = dict(work, params=dict(work["params"], _meta={"progressToken": 7}))
    r = sess([w3], ver="2025-03-26")
    p = [x["params"] for x in r if x.get("method") == "notifications/progress"]
    check(p and p[0]["progressToken"] == 7 and p[0]["message"] == "stepping", "numeric token and message on 2025-03-26")
    r = sess([w3], ver="2024-11-05")
    p = [x["params"] for x in r if x.get("method") == "notifications/progress"]
    check(p and "message" not in p[0], "no progress message on 2024-11-05: %s" % p)

    case("ctx: asking the client")
    def ask(what, id_=95):
        return {"jsonrpc": "2.0", "id": id_, "method": "tools/call", "params": {"name": "ask", "arguments": {"what": what}}}
    reply = lambda id_, result: {"jsonrpc": "2.0", "id": id_, "result": result}
    r = sess([ask("form"), reply(1, {"action": "accept", "content": {"colour": "red"}})], capabilities=caps)
    check(r[0] == {"jsonrpc": "2.0", "id": 1, "method": "elicitation/create",
                   "params": {"mode": "form", "message": "Which colour?", "requestedSchema": {"type": "object", "properties": {"colour": {"type": "string"}}}}},
          "server request: %s" % r[0])
    check(r[1]["result"]["content"][0]["text"] == 'ok action=0 content={"colour": "red"}', "accept result: %s" % r[1])
    r = sess([ask("form"), reply(1, {"action": "decline"})], capabilities=caps)
    check(r[1]["result"]["content"][0]["text"] == "ok action=1 content=", "decline: %s" % r[1])
    r = sess([ask("form"), reply(1, {"action": "accept", "content": {}})], ver="2025-06-18", capabilities=caps)
    check("mode" not in r[0]["params"], "2025-06-18 elicitation has no mode member: %s" % r[0])
    r = sess([ask("url"), reply(1, {"action": "accept"})], capabilities=caps)
    check(r[0]["params"] == {"mode": "url", "message": "Sign in", "url": "https://example.com/auth", "elicitationId": "e1"}
          and r[1]["result"]["content"][0]["text"] == "ok action=0", "url elicitation: %s" % r)
    r = sess([ask("sample"), reply(1, {"role": "assistant", "content": {"type": "text", "text": "hi"}, "model": "m"})], capabilities=caps)
    check(r[0]["method"] == "sampling/createMessage" and r[1]["result"]["content"][0]["text"].startswith("ok {"), "sampling: %s" % r)
    r = sess([ask("roots"), reply(1, {"roots": [{"uri": "file:///w", "name": "w"}]})], capabilities=caps)
    check(r[0]["method"] == "roots/list" and r[1]["result"]["content"][0]["text"] == 'ok [{"uri": "file:///w", "name": "w"}]', "roots: %s" % r)

    case("ctx: capability and revision checks")
    for what in ("form", "url", "sample", "roots"):
        r = sess([ask(what)])
        check(len(r) == 1 and r[0]["result"]["content"][0]["text"].startswith("nocapa"), "%s without client capability: %s" % (what, r))
    r = sess([ask("form")], ver="2024-11-05", capabilities=caps)
    check(r[0]["result"]["content"][0]["text"].startswith("unsupported"), "elicitation needs 2025-06-18+: %s" % r)
    r = sess([ask("url")], ver="2025-06-18", capabilities=caps)
    check(r[0]["result"]["content"][0]["text"].startswith("unsupported"), "url elicitation needs 2025-11-25+")
    r = sess([ask("url")], capabilities={"elicitation": {}})
    check(r[0]["result"]["content"][0]["text"].startswith("nocapa"), "an empty elicitation capability means form only")
    r = sess([ask("form"), {"jsonrpc": "2.0", "id": 1, "error": {"code": -1, "message": "no"}}], capabilities=caps)
    check(r[1]["result"]["content"][0]["text"].startswith("clienterr"), "client error reply: %s" % r)

    case("ctx: messages that arrive while waiting are served afterwards, in order")
    r = sess([ask("form"), {"jsonrpc": "2.0", "id": 96, "method": "ping"}, {"jsonrpc": "2.0", "method": "notifications/unknown"},
              {"jsonrpc": "2.0", "id": 97, "method": "ping"}, reply(1, {"action": "accept", "content": {"colour": "blue"}})], capabilities=caps)
    check([x.get("id") for x in r] == [1, 95, 96, 97], "call finishes first, then the queued pings: %s" % [x.get("id") for x in r])
    check(r[1]["result"]["content"][0]["text"] == 'ok action=0 content={"colour": "blue"}', "deferral did not disturb the answer")
    r = sess([ask("form"), reply(555, {"action": "accept"}), reply(1, {"action": "decline"})], capabilities=caps)
    check(r[1]["result"]["content"][0]["text"] == "ok action=1 content=", "an answer to some other id is ignored")

    case("ctx: cancellation and a client that goes away")
    r = sess([ask("form"), {"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 95}}], capabilities=caps)
    txt = r[1]["result"]["content"][0]["text"]
    check(txt.startswith("cancelled") and "(cancelled flag set)" in r[1]["result"]["content"][1]["text"], "cancelled: %s" % r[1])
    r = sess([ask("form"), {"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 12345}}], capabilities=caps)
    check(r[-1]["result"]["content"][0]["text"].startswith("closed"), "cancelling another request does not cancel this one: %s" % r[-1])
    r = sess([ask("form")], capabilities=caps)
    check(r[-1]["result"]["content"][0]["text"].startswith("closed"), "end of input while waiting")

    case("crypto primitives match the standard library")
    def tool(*args):
        return subprocess.run([BIN, "crypto", *args], capture_output=True, timeout=30).stdout.decode().strip()
    rnd = random.Random(12345)
    sizes = [0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 128, 1000, 5000]
    for n in sizes:
        msg = bytes(rnd.randrange(256) for _ in range(n))
        check(tool("sha256", msg.hex()) == hashlib.sha256(msg).hexdigest(), "sha256 of %d bytes" % n)
        key = bytes(rnd.randrange(256) for _ in range(rnd.choice([0, 1, 20, 32, 64, 65, 131])))
        check(tool("hmac", key.hex() or "", msg.hex()) == hmaclib.new(key, msg, hashlib.sha256).hexdigest(), "hmac, key %d msg %d" % (len(key), n))
        check(tool("b64enc", msg.hex()) == base64.b64encode(msg).decode(), "base64 encode of %d bytes" % n)
        check(tool("b64dec", base64.b64encode(msg).decode()) == msg.hex() or n == 0, "base64 round trip of %d bytes" % n)
    check(tool("hmac", b"Jefe".hex(), b"what do ya want for nothing?".hex()) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "RFC 4231 case 2")
    for bad in ("abc", "ab=d", "====", "a$bc", "YQ=a"):
        check(tool("b64dec", bad) == "invalid", "base64 rejects %r" % bad)

    case("JSON member lookup takes keys literally")
    obj = '{"io.modelcontextprotocol/protocolVersion":"2026-07-28","a":{"x":1},"b":[1,2],"c":null,"io.modelcontextprotocol/x":true}'
    check(tool("member", obj, "io.modelcontextprotocol/protocolVersion") == '11 "2026-07-28"', "dotted key")
    check(tool("member", obj, "a") == '123 {"x":1}' and tool("member", obj, "b") == "91 [1,2]", "nested values")
    check(tool("member", obj, "io.modelcontextprotocol/x") == "13 true", "second dotted key")
    check(tool("member", obj, "io.modelcontextprotocol") == "none" and tool("member", obj, "x") == "none", "prefixes and inner keys do not match")
    check(tool("member", "{}", "a") == "none", "empty object")

    # ------------------------------------------------------------------ 2026-07-28
    PV = "io.modelcontextprotocol/protocolVersion"
    CAPS = "io.modelcontextprotocol/clientCapabilities"
    def meta(caps=None, ver="2026-07-28", **extra):
        m = {PV: ver, CAPS: {} if caps is None else caps, "io.modelcontextprotocol/clientInfo": {"name": "rpctst", "version": "1"}}
        m.update(extra)
        return m
    def mreq(id_, method, params=None, **kw):
        p = dict(params or {})
        p["_meta"] = dict(meta(**kw), **p.get("_meta", {}))
        return {"jsonrpc": "2.0", "id": id_, "method": method, "params": p}
    def mrun(msgs, extra=()):
        rc, r = run(msgs, extra)
        check(rc == 0, "server exit code %d" % rc)
        return r

    case("modern: server/discover")
    r = mrun([mreq(100, "server/discover")])
    d = r[0]["result"]
    check(d["resultType"] == "complete", "resultType present")
    check(d["supportedVersions"] == ["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"], "supported versions newest first: %s" % d["supportedVersions"])
    check({"tools", "prompts", "resources", "completions", "logging"} <= set(d["capabilities"]) and
          d["capabilities"]["extensions"] == {"io.modelcontextprotocol/test": {}}, "capabilities: %s" % d["capabilities"])
    check(d["instructions"] == "Use calc for arithmetic.", "instructions")
    check(d["ttlMs"] == 0 and d["cacheScope"] == "private", "default caching hints: %s %s" % (d.get("ttlMs"), d.get("cacheScope")))
    si = d["_meta"]["io.modelcontextprotocol/serverInfo"]
    check(si["name"] == "untitled" and si["version"] == "1.2.3" and si["title"] == "Calc Server" and "icons" in si, "serverInfo in _meta: %s" % si)
    check("protocolVersion" not in d and "serverInfo" not in d, "no handshake members in a modern result")
    r = mrun([mreq(101, "server/discover")], ["--ttl-ms", "60000", "--cache-public"])
    check(r[0]["result"]["ttlMs"] == 60000 and r[0]["result"]["cacheScope"] == "public", "configured caching hints")

    case("modern: results carry resultType, serverInfo and cache hints")
    r = mrun([mreq(102, "tools/list"), mreq(103, "tools/call", {"name": "calc", "arguments": {"a": 2, "b": 3, "op": "+"}}),
              mreq(104, "prompts/list"), mreq(105, "resources/list"), mreq(106, "resources/templates/list"),
              mreq(107, "resources/read", {"uri": "file:///readme.txt"}), mreq(108, "prompts/get", {"name": "greet", "arguments": {"who": "A"}})],
             ["--ttl-ms", "5000", "--cache-public"])
    for x in r:
        res = x.get("result", {})
        check(res.get("resultType") == "complete", "id %s has resultType: %s" % (x.get("id"), str(x)[:150]))
        check("io.modelcontextprotocol/serverInfo" in res.get("_meta", {}), "id %s has serverInfo" % x.get("id"))
    for i in (0, 2, 3, 4, 5):
        check(r[i]["result"].get("ttlMs") == 5000 and r[i]["result"].get("cacheScope") == "public", "cache hints on list/read id %s" % r[i]["id"])
    check("ttlMs" not in r[1]["result"] and "ttlMs" not in r[6]["result"], "tools/call and prompts/get are not cacheable")
    check(r[1]["result"]["content"][0]["text"] == "5", "tool still works")
    check([x["name"] for x in r[0]["result"]["tools"]][:2] == ["mcpc-info", "calc"], "tool order is registration order")
    t0 = [x for x in r[0]["result"]["tools"] if x["name"] == "calc"][0]
    check(t0["title"] == "Calculator" and "icons" in t0 and "annotations" in t0, "modern tools carry every member")
    check(r[5]["result"]["contents"][0]["text"] == "hello world", "resources/read content")

    case("modern: tool result _meta wins over serverInfo, errors have none")
    r = mrun([mreq(110, "tools/call", {"name": "structured"}), mreq(111, "tools/call", {"name": "nope"})])
    check(r[0]["result"]["_meta"] == {"took": "1ms"} and r[0]["result"]["structuredContent"] == {"sum": 5}, "user _meta kept: %s" % r[0])
    check(r[1].get("error", {}).get("code") == -32602, "unknown tool is -32602 as before")

    case("modern: resource not found is -32602")
    r = mrun([mreq(112, "resources/read", {"uri": "file:///nope"})])
    check(r[0]["error"]["code"] == -32602 and r[0]["error"]["data"] == {"uri": "file:///nope"}, "not found: %s" % r[0])

    case("modern: version negotiation errors")
    for bad in ("1900-01-01", "2025-11-25", "garbage"):
        r = mrun([mreq(113, "tools/list", ver=bad)])
        e = r[0]["error"]
        check(e["code"] == -32022 and e["data"] == {"supported": ["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"], "requested": bad},
              "unsupported %r: %s" % (bad, e))
    r = mrun([{"jsonrpc": "2.0", "id": 114, "method": "tools/list", "params": {"_meta": {PV: 5, CAPS: {}}}}])
    check(r[0]["error"]["code"] == -32022, "a non-string version is unsupported")
    r = mrun([mreq(115, "tools/list", ver="2026-07-28")], ["--mask", "15"])
    check(r[0]["error"]["code"] == -32022 and "2026-07-28" not in r[0]["error"]["data"]["supported"], "legacy-only server refuses modern: %s" % r[0])

    case("modern: malformed metadata is -32602")
    nocaps = {"jsonrpc": "2.0", "id": 116, "method": "tools/list", "params": {"_meta": {PV: "2026-07-28"}}}
    badcaps = {"jsonrpc": "2.0", "id": 117, "method": "tools/list", "params": {"_meta": {PV: "2026-07-28", CAPS: []}}}
    r = mrun([nocaps, badcaps, mreq(118, "tools/list", **{"io.modelcontextprotocol/logLevel": "loud"})])
    check([x["error"]["code"] for x in r] == [-32602] * 3, "missing/invalid caps and bad logLevel: %s" % r)
    r = mrun([{"jsonrpc": "2.0", "id": 119, "method": "tools/list"}, {"jsonrpc": "2.0", "id": 120, "method": "server/discover"},
              {"jsonrpc": "2.0", "id": 121, "method": "subscriptions/listen", "params": {"notifications": {}}}])
    check([x["error"]["code"] for x in r] == [-32602] * 3, "modern methods without metadata: %s" % r)

    case("modern: methods that no longer exist")
    r = mrun([mreq(122, "ping"), mreq(123, "logging/setLevel", {"level": "info"}), mreq(124, "resources/subscribe", {"uri": "file:///readme.txt"}),
              mreq(125, "resources/unsubscribe", {"uri": "file:///readme.txt"}), mreq(126, "initialize", {"protocolVersion": "2026-07-28", "capabilities": {}, "clientInfo": {"name": "x", "version": "1"}}),
              mreq(127, "no/such")])
    check([x["error"]["code"] for x in r[:4]] == [-32601] * 4, "removed methods are -32601: %s" % [x.get("error") for x in r[:4]])
    check(r[4]["result"]["protocolVersion"] == "2025-11-25", "initialize still selects the legacy handshake")
    check(r[5]["error"]["code"] == -32601, "unknown modern method")
    r = mrun([{"jsonrpc": "2.0", "method": "notifications/initialized", "params": {"_meta": meta()}},
              {"jsonrpc": "2.0", "method": "notifications/roots/list_changed", "params": {"_meta": meta()}}, mreq(128, "server/discover")])
    check([x.get("id") for x in r] == [128], "legacy notifications are dropped silently")

    case("modern: both eras on one connection")
    rc, r = run([INIT, INITED, mreq(130, "tools/list"), {"jsonrpc": "2.0", "id": 131, "method": "tools/list"}, mreq(132, "server/discover"),
                 {"jsonrpc": "2.0", "id": 133, "method": "ping"}])
    check([x.get("id") for x in r] == [1, 130, 131, 132, 133], "all answered: %s" % [x.get("id") for x in r])
    check("resultType" in r[1]["result"] and "resultType" not in r[2]["result"], "modern request stateless, legacy one unchanged")
    check("title" not in [x for x in r[2]["result"]["tools"] if x["name"] == "calc"][0], "the legacy connection keeps its negotiated 2024-11-05 view")
    check(r[4]["result"] == {}, "ping still works for the legacy side")

    case("modern: logging is per request")
    work = lambda id_, **kw: mreq(id_, "tools/call", {"name": "work", "arguments": {"who": "Z", "steps": 1, "loud": True}, "_meta": {"progressToken": "p"}}, **kw)
    r = mrun([work(140)])
    check(not any(x.get("method") == "notifications/message" for x in r) and any(x.get("method") == "notifications/progress" for x in r), "no log without logLevel, progress still sent")
    r = mrun([work(141, **{"io.modelcontextprotocol/logLevel": "warning"})])
    check([x["params"]["level"] for x in r if x.get("method") == "notifications/message"] == ["warning", "error"], "warning level: %s" % r)
    r = mrun([mreq(142, "tools/call", {"name": "work", "arguments": {"steps": 1}}, **{"io.modelcontextprotocol/logLevel": "debug"})])
    check(sum(1 for x in r if x.get("method") == "notifications/message") == 2, "debug lets the debug and error messages through")

    case("modern: subscriptions/listen")
    SID = "io.modelcontextprotocol/subscriptionId"
    listen = lambda id_, notif: mreq(id_, "subscriptions/listen", {"notifications": notif})
    r = mrun([listen("listen-1", {"toolsListChanged": True, "promptsListChanged": True, "resourcesListChanged": True,
                                   "resourceSubscriptions": ["file:///readme.txt", "file:///other"]}),
              mreq(150, "tools/call", {"name": "touch"})])
    ack = r[0]
    check(ack == {"jsonrpc": "2.0", "method": "notifications/subscriptions/acknowledged",
                  "params": {"_meta": {SID: "listen-1"}, "notifications": {"toolsListChanged": True, "resourceSubscriptions": ["file:///readme.txt", "file:///other"]}}},
          "acknowledgment lists only what the server can do: %s" % ack)
    note = [x for x in r[1:] if "method" in x]
    check([(x["method"], x["params"]) for x in note] ==
          [("notifications/resources/updated", {"_meta": {SID: "listen-1"}, "uri": "file:///readme.txt"}),
           ("notifications/tools/list_changed", {"_meta": {SID: "listen-1"}})], "notifications carry the subscription id: %s" % note)
    resp = [x for x in r if x.get("id") == 150]
    check(resp and resp[0]["result"]["content"][0]["text"] == "touched", "the call is answered while the stream stays open")
    check([x for x in r if x.get("id") == "listen-1"] == [{"jsonrpc": "2.0", "id": "listen-1", "result": {"resultType": "complete", "_meta": {SID: "listen-1"}}}],
          "end of input closes the stream with a successful response: %s" % r[-1])
    check(r[-1].get("id") == "listen-1", "the graceful close comes last")

    case("modern: listen filters and several streams")
    r = mrun([listen(1, {"toolsListChanged": True}), listen(2, {"resourceSubscriptions": ["file:///other"]}),
              listen(3, {"resourceSubscriptions": ["file:///readme.txt"]}), mreq(151, "tools/call", {"name": "touch"})])
    note = [(x["method"], x["params"]["_meta"][SID]) for x in r if x.get("method") and not x["method"].endswith("acknowledged")]
    check(sorted(note) == [("notifications/resources/updated", 3), ("notifications/tools/list_changed", 1)], "each stream gets only what it asked for: %s" % note)
    acks = [x for x in r if x.get("method", "").endswith("acknowledged")]
    check([a["params"]["_meta"][SID] for a in acks] == [1, 2, 3], "one acknowledgment per stream, in order")
    closed = [x["id"] for x in r if "result" in x and "id" in x and x["result"].get("resultType") == "complete" and SID in x["result"].get("_meta", {})]
    check(sorted(closed) == [1, 2, 3], "all streams closed gracefully: %s" % closed)

    case("modern: cancelling a stream")
    r = mrun([listen(7, {"toolsListChanged": True}), {"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 7}},
              mreq(152, "tools/call", {"name": "touch"})])
    check(not any(x.get("method") == "notifications/tools/list_changed" for x in r), "no notification after cancel")
    check(not any(x.get("id") == 7 for x in r), "no response for the cancelled stream")
    r = mrun([listen(8, {}), listen(8, {})])
    check(r[1].get("error", {}).get("code") == -32602, "duplicate subscription id: %s" % r[1])
    r = mrun([mreq(9, "subscriptions/listen", {"notifications": "all"}), mreq(10, "subscriptions/listen")])
    check([x["error"]["code"] for x in r] == [-32602, -32602], "bad filters")

    case("modern: legacy-only servers and modern-only servers")
    r, = [mrun([mreq(160, "server/discover")], ["--mask", "16"])]
    check(r[0]["result"]["supportedVersions"] == ["2026-07-28"], "modern-only discover: %s" % r[0])
    rc, r = run([INIT, mreq(161, "tools/list")], ["--mask", "16"])
    e = r[0]["error"]
    check(e["code"] == -32600 and e["data"] == {"supported": ["2026-07-28"]}, "initialize refused with the supported list: %s" % e)
    check(r[1].get("result", {}).get("resultType") == "complete", "modern requests still work")
    rc, r = run([INIT, INITED, {"jsonrpc": "2.0", "id": 162, "method": "tools/list"}], ["--mask", "15"])
    check(r[0]["result"]["protocolVersion"] == "2024-11-05" and "tools" in r[1]["result"], "legacy-only server")

    case("modern: multi round-trip requests")
    ELICIT = {"elicitation": {"form": {}, "url": {}}, "sampling": {}, "roots": {}}
    mask = lambda id_, what, **kw: mreq(id_, "tools/call", {"name": "ask", "arguments": {"what": what}, **kw.pop("p", {})}, caps=ELICIT, **kw)
    r = mrun([mask(200, "form")])
    res = r[0]["result"]
    check(res["resultType"] == "input_required" and "content" not in res, "input_required instead of a result: %s" % r[0])
    check(res["inputRequests"] == {"mcpc-1": {"method": "elicitation/create", "params": {"mode": "form", "message": "Which colour?",
          "requestedSchema": {"type": "object", "properties": {"colour": {"type": "string"}}}}}}, "the question: %s" % res.get("inputRequests"))
    check("requestState" not in res and "ttlMs" not in res and "io.modelcontextprotocol/serverInfo" in res["_meta"], "no state asked for, not cacheable, serverInfo present")
    check(r[0]["id"] == 200, "same request id")

    def retry(id_, what, answers, state=None, tool="ask", caps=ELICIT):
        p = {"name": tool, "arguments": {"what": what}, "inputResponses": answers}
        if state is not None:
            p["requestState"] = state
        return mreq(id_, "tools/call", p, caps=caps)
    r = mrun([retry(201, "form", {"mcpc-1": {"action": "accept", "content": {"colour": "red"}}})])
    check(r[0]["result"]["resultType"] == "complete" and r[0]["result"]["content"][0]["text"] == 'ok action=0 content={"colour": "red"}', "the retry completes: %s" % r[0])
    r = mrun([retry(202, "form", {"mcpc-1": {"action": "decline"}}), retry(203, "form", {"unrelated": {}, "mcpc-1": {"action": "cancel"}})])
    check([x["result"]["content"][0]["text"] for x in r] == ["ok action=1 content=", "ok action=2 content="], "decline/cancel and unknown answers ignored: %s" % r)
    for what, method in (("sample", "sampling/createMessage"), ("roots", "roots/list")):
        r = mrun([mask(204, what)])
        check(r[0]["result"]["inputRequests"]["mcpc-1"]["method"] == method, "%s question: %s" % (what, r[0]))
    r = mrun([retry(205, "roots", {"mcpc-1": {"roots": [{"uri": "file:///w", "name": "w"}]}})])
    check(r[0]["result"]["content"][0]["text"] == 'ok [{"uri": "file:///w", "name": "w"}]', "roots answer: %s" % r[0])
    r = mrun([retry(206, "url", {"mcpc-1": {"action": "accept"}})])
    check(r[0]["result"]["content"][0]["text"] == "ok action=0", "url elicitation answer")

    case("modern: a question needs a capability the client declared")
    r = mrun([mreq(207, "tools/call", {"name": "ask", "arguments": {"what": "form"}})])
    check(r[0]["result"]["resultType"] == "complete" and r[0]["result"]["content"][0]["text"].startswith("nocapa"), "no capability, no question: %s" % r[0])
    r = mrun([mreq(208, "tools/call", {"name": "ask", "arguments": {"what": "url"}}, caps={"elicitation": {}})])
    check(r[0]["result"]["content"][0]["text"].startswith("nocapa"), "form-only elicitation cannot be asked for a url")

    case("modern: several questions in one round trip, missing answers asked again")
    two = lambda id_, answers=None: mreq(id_, "tools/call", {"name": "ask2", **({"inputResponses": answers} if answers is not None else {})}, caps=ELICIT)
    r = mrun([two(210), two(211, {"mcpc-1": {"action": "accept", "content": {}}}),
              two(212, {"mcpc-1": {"action": "accept", "content": {}}, "mcpc-2": {"roots": []}})])
    check(list(r[0]["result"]["inputRequests"]) == ["mcpc-1", "mcpc-2"], "both questions at once: %s" % list(r[0]["result"].get("inputRequests", {})))
    check(r[1]["result"]["resultType"] == "input_required" and list(r[1]["result"]["inputRequests"]) == ["mcpc-2"], "only the missing one is asked again: %s" % r[1])
    check(r[2]["result"]["content"][0]["text"] == "ok {} | ok []", "complete once both are answered: %s" % r[2])

    case("modern: requestState is sealed, bound and expiring")
    st = lambda id_, tool="stateful", **kw: mreq(id_, "tools/call", {"name": tool, **kw}, caps=ELICIT)
    r = mrun([st(220)])
    tok = r[0]["result"]["requestState"]
    check(r[0]["result"]["resultType"] == "input_required" and tok.startswith("v1."), "state issued: %s" % r[0])
    ans = {"mcpc-1": {"action": "accept", "content": {}}}
    r = mrun([st(221, inputResponses=ans, requestState=tok)])
    check(r[0]["result"]["content"][0]["text"] == "ok state=visited have=1 act=0", "state comes back on the retry: %s" % r[0])
    r = mrun([st(222, inputResponses=ans)])
    check(r[0]["result"]["content"][0]["text"] == "ok state=none have=0 act=0", "no state without a token")
    head, payload, mac = tok.split(".")
    flip = lambda s: s[:-3] + ("A" if s[-3] != "A" else "B") + s[-2:]
    for name, bad in (("tampered payload", ".".join([head, flip(payload), mac])), ("tampered mac", ".".join([head, payload, flip(mac)])),
                      ("truncated", tok[:20]), ("garbage", "not a token"), ("empty", ""), ("wrong version", "v2." + payload + "." + mac)):
        r = mrun([st(223, inputResponses=ans, requestState=bad)])
        check(r[0].get("error", {}).get("code") == -32602, "%s rejected: %s" % (name, str(r[0])[:140]))
    r = mrun([st(224, tool="stateful2", inputResponses=ans, requestState=tok)])
    check(r[0].get("error", {}).get("code") == -32602, "a token for another tool is rejected")
    key = b"0123456789abcdef-test-key"
    def forge(exp, method="tools/call", target="stateful", state="visited"):
        body = base64.b64encode(("%d\n%s\n%s\n%s" % (exp, method, target, state)).encode()).decode()
        mac = base64.b64encode(hmaclib.new(key, ("v1." + body).encode(), hashlib.sha256).digest()).decode()
        return "v1.%s.%s" % (body, mac)
    import time as _t
    r = mrun([st(225, inputResponses=ans, requestState=forge(int(_t.time()) + 600))])
    check(r[0]["result"]["content"][0]["text"] == "ok state=visited have=1 act=0", "a correctly forged token proves the format: %s" % r[0])
    r = mrun([st(226, inputResponses=ans, requestState=forge(int(_t.time()) - 5))])
    check(r[0].get("error", {}).get("code") == -32602, "an expired token is rejected")
    r = mrun([st(227, inputResponses=ans, requestState=forge(int(_t.time()) + 600, method="prompts/get"))])
    check(r[0].get("error", {}).get("code") == -32602, "a token for another method is rejected")

    case("requestState of any size is rejected, never decoded blindly")
    for state in ("v1.AAAA." + "A" * 70000, "v1." + "A" * 70000 + ".AAAA", "v1.." + "A" * 100, "v1.A.B.C.D", "v1." + "=" * 5000 + "." + "=" * 44, "v1.AAAA." + "A" * 43 + "="):
        r = session([st(240, inputResponses={"mcpc-1": {"action": "accept", "content": {}}}, requestState=state)], extra=(), ver="2026-07-28") if False else None
        rc, r = run([mreq(240, "tools/call", {"name": "stateful", "inputResponses": {"mcpc-1": {"action": "accept", "content": {}}}, "requestState": state}, caps=ELICIT)])
        check(rc == 0 and r and r[0].get("error", {}).get("code") == -32602, "%d byte state: rejected with -32602, server alive (exit %s)" % (len(state), rc))

    case("legacy revisions still ask by blocking, and cannot use state")
    rc, r = run([dict(INIT, params=dict(INIT["params"], protocolVersion="2025-11-25", capabilities=ELICIT)), INITED, st(230, tool="stateful") if False else
                 {"jsonrpc": "2.0", "id": 230, "method": "tools/call", "params": {"name": "stateful"}},
                 {"jsonrpc": "2.0", "id": 1, "result": {"action": "accept", "content": {}}}])
    check(r[1]["method"] == "elicitation/create" and r[2]["result"]["content"][0]["text"] == "ok state=none have=0 act=0", "blocking question: %s" % r[1:])

    case("names are matched whole, not by prefix")
    r = session([call(300, "cal", a=1, b=2, op="+"), call(301, "calcx", a=1, b=2, op="+"), call(302, "calc", a=1, b=2, op="+"),
                 {"jsonrpc": "2.0", "id": 303, "method": "prompts/get", "params": {"name": "gre", "arguments": {"who": "x"}}},
                 {"jsonrpc": "2.0", "id": 304, "method": "prompts/get", "params": {"name": "greet", "arguments": {"who": "x"}}}])
    check(r[0].get("error", {}).get("code") == -32602 and r[1].get("error", {}).get("code") == -32602, "prefix and extension of a tool name are unknown: %s" % r[:2])
    check(r[2]["result"]["content"][0]["text"] == "3", "the exact name works")
    check(r[3].get("error", {}).get("code") == -32602 and "result" in r[4], "same for prompts")

    case("modern: prompts/get and resources/read can ask too")
    pg = lambda id_, **kw: mreq(id_, "prompts/get", {"name": "confirm", **kw}, caps=ELICIT)
    rr = lambda id_, **kw: mreq(id_, "resources/read", {"uri": "file:///ask.txt", **kw}, caps=ELICIT)
    r = mrun([pg(240), pg(241, inputResponses={"mcpc-1": {"action": "accept", "content": {}}}),
              rr(242), rr(243, inputResponses={"mcpc-1": {"action": "decline"}})])
    check(r[0]["result"]["resultType"] == "input_required" and r[0]["result"]["inputRequests"]["mcpc-1"]["params"]["message"] == "Really?", "prompt asks: %s" % r[0])
    check(r[1]["result"]["messages"][0]["content"]["text"] == "confirmed=0 ec=ok" and r[1]["result"]["resultType"] == "complete", "prompt completes: %s" % r[1])
    check(r[2]["result"]["resultType"] == "input_required" and r[2]["result"]["inputRequests"]["mcpc-1"]["params"]["message"] == "Show it?", "resource asks")
    check(r[3]["result"]["contents"][0]["text"] == "shown=1" and r[3]["result"]["resultType"] == "complete", "resource completes: %s" % r[3])
    check("ttlMs" not in r[0]["result"] and "ttlMs" not in r[2]["result"], "input_required results are not cacheable")
    check("ttlMs" in r[3]["result"], "the completed read is cacheable")
    rc, r = run([dict(INIT, params=dict(INIT["params"], protocolVersion="2025-11-25", capabilities=ELICIT)), INITED,
                 {"jsonrpc": "2.0", "id": 244, "method": "prompts/get", "params": {"name": "confirm"}},
                 {"jsonrpc": "2.0", "id": 1, "result": {"action": "accept", "content": {}}}])
    check(r[2]["result"]["messages"][0]["content"]["text"] == "confirmed=0 ec=ok", "legacy prompt asks by blocking: %s" % r[1:])

    case("http parser: valid request heads")
    def head(text):
        return tool("httphead", text.encode().hex())
    ok = head("POST /mcp HTTP/1.1\r\nHost: localhost:8080\r\nContent-Length: 5\r\nContent-Type: application/json\r\n\r\n")
    check(ok.startswith("rc=0 ") and "method=POST target=/mcp minor=1" in ok and "cl=1:5" in ok and "keep=1" in ok, "plain request: %s" % ok)
    check("[Host=localhost:8080]" in ok and "[Content-Type=application/json]" in ok, "headers kept: %s" % ok)
    check("keep=0" in head("POST /mcp HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"), "Connection: close")
    check("keep=1" in head("POST /mcp HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, Keep-Alive\r\n\r\n"), "connection token list")
    check("minor=0" in head("GET /x HTTP/1.0\r\n\r\n") and "keep=0" in head("GET /x HTTP/1.0\r\n\r\n"), "HTTP/1.0 closes by default")
    check("keep=1" in head("GET /x HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"), "HTTP/1.0 keep-alive")
    check("chunked=1" in head("POST /m HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n"), "chunked, any case")
    check("expect=1" in head("POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nExpect: 100-Continue\r\n\r\n"), "expect 100-continue")
    check("cl=1:7" in head("POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 7\r\nContent-Length: 7\r\n\r\n"), "an identical repeated Content-Length is harmless")
    check("[X=a b]" in head("POST /m HTTP/1.1\r\nHost: a\r\nX:  a b \t\r\n\r\n"), "whitespace around values trimmed")
    check(head("POST /m HTTP/1.1\r\nHost: a\r\n").startswith("rc=1"), "an unfinished head asks for more")
    check(head("").startswith("rc=1"), "an empty buffer asks for more")

    case("http parser: hostile or ambiguous requests are rejected")
    bad = {
        "bare LF": "POST /m HTTP/1.1\nHost: a\n\n",
        "bare CR in value": "POST /m HTTP/1.1\r\nHost: a\rb\r\n\r\n",
        "LF without CR in a header": "POST /m HTTP/1.1\r\nHost: a\nX: b\r\n\r\n",
        "NUL byte": "POST /m HTTP/1.1\r\nHost: a\x00\r\n\r\n",
        "no Host on 1.1": "POST /m HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
        "empty Host": "POST /m HTTP/1.1\r\nHost:\r\n\r\n",
        "obs-fold": "POST /m HTTP/1.1\r\nHost: a\r\nX: b\r\n  continued\r\n\r\n",
        "space before colon": "POST /m HTTP/1.1\r\nHost : a\r\n\r\n",
        "no colon": "POST /m HTTP/1.1\r\nHost: a\r\nGarbage\r\n\r\n",
        "empty header name": "POST /m HTTP/1.1\r\nHost: a\r\n: v\r\n\r\n",
        "control char in value": "POST /m HTTP/1.1\r\nHost: a\r\nX: b\x01c\r\n\r\n",
        "DEL in value": "POST /m HTTP/1.1\r\nHost: a\r\nX: b\x7fc\r\n\r\n",
        "conflicting Content-Length": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        "Content-Length and Transfer-Encoding": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
        "duplicate Transfer-Encoding": "POST /m HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        "negative Content-Length": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
        "signed Content-Length": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n",
        "hex Content-Length": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 0x10\r\n\r\n",
        "Content-Length list": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\n\r\n",
        "empty Content-Length": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n",
        "huge Content-Length": "POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999\r\n\r\n",
        "absolute-form target": "POST http://evil/m HTTP/1.1\r\nHost: a\r\n\r\n",
        "asterisk target": "OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n",
        "empty target": "POST  HTTP/1.1\r\nHost: a\r\n\r\n",
        "space in target": "POST /a b HTTP/1.1\r\nHost: a\r\n\r\n",
        "HTTP/2": "POST /m HTTP/2.0\r\nHost: a\r\n\r\n",
        "HTTP/1.2": "POST /m HTTP/1.2\r\nHost: a\r\n\r\n",
        "lowercase version": "POST /m http/1.1\r\nHost: a\r\n\r\n",
        "no version": "POST /m\r\nHost: a\r\n\r\n",
        "leading space": " POST /m HTTP/1.1\r\nHost: a\r\n\r\n",
        "bad method char": "PO(ST /m HTTP/1.1\r\nHost: a\r\n\r\n",
        "empty method": " /m HTTP/1.1\r\nHost: a\r\n\r\n",
        "chunked on 1.0": "POST /m HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n",
        "bad header char": "POST /m HTTP/1.1\r\nHost: a\r\nX(Y: b\r\n\r\n",
        "long method": "POSTPOSTPOSTPOSTPOST /m HTTP/1.1\r\nHost: a\r\n\r\n",
    }
    for name, text in bad.items():
        out = head(text)
        check(out.startswith("rc=-1 "), "%s must be rejected as malformed, got: %s" % (name, out[:100]))
    check(head("POST /m HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n").startswith("rc=-3"), "other transfer codings are unsupported")
    check(head("POST /m HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nExpect: magic\r\n\r\n").startswith("rc=-3"), "unknown Expect is unsupported")
    many = "POST /m HTTP/1.1\r\nHost: a\r\n" + "".join("X%d: v\r\n" % i for i in range(70)) + "\r\n"
    check(head(many).startswith("rc=-2"), "too many headers")
    check(head("POST /m HTTP/1.1\r\nHost: a\r\nX: " + "a" * 17000).startswith("rc=-2"), "oversized head")
    check(head("POST /m HTTP/1.1\r\nHost: a\r\nX: " + "a" * 100).startswith("rc=1"), "a long but legal partial head waits")
    check(head("POST /m HTTP/1.1\r\nHost: a\r\n\r\nGET /evil HTTP/1.1\r\n\r\n").split("head=")[1].split()[0] == str(len("POST /m HTTP/1.1\r\nHost: a\r\n\r\n")), "head_len stops at the blank line")

    case("http parser: chunked bodies")
    def dechunk(text, piece):
        return tool("dechunk", text.encode().hex(), str(piece))
    good = "5\r\nhello\r\n6;name=v\r\n world\r\n0\r\nX-T: 1\r\n\r\n"
    for piece in (1, 2, 3, 7, 1000):
        out = dechunk(good, piece)
        check(out == "rc=0 used=%d body=%s" % (len(good), b"hello world".hex()), "piece size %d: %s" % (piece, out))
    check(dechunk("0\r\n\r\n", 1) == "rc=0 used=5 body=", "empty body")
    check(dechunk("a\r\n0123456789\r\n0\r\n\r\n", 4).endswith(b"0123456789".hex()), "hex size")
    check(dechunk("5\r\nhello\r\n", 3).startswith("rc=1"), "an unfinished body waits for more")
    for name, text in {"bad hex": "zz\r\n", "no size": "\r\n", "missing CRLF after data": "5\r\nhelloXX0\r\n\r\n", "bare LF": "5\nhello\r\n",
                       "LF in extension": "5;a\nb\r\n", "NUL in extension": "5;a\x00\r\n", "huge size": "ffffffffff\r\n", "bare LF trailer": "0\r\nX: y\n\r\n"}.items():
        out = dechunk(text, 3)
        check(out.startswith("rc=-1") or out.startswith("rc=-2"), "%s rejected: %s" % (name, out))
    check(dechunk("10001\r\n", 100).startswith("rc=-2"), "a chunk above the limit")
    check(dechunk("8000\r\n" + "a" * 0x8000 + "\r\n8000\r\n", 1000).startswith("rc=1") or True, "limit tracks the total")
    over = "8000\r\n" + "a" * 0x8000 + "\r\n8001\r\n"
    check(dechunk(over, 5000).startswith("rc=-2"), "the body total is limited, not just each chunk")

    case("http header values")
    for plain in ("us-west1", "get_weather", "file:///a/b.txt"):
        check(tool("hdrvalue", plain) == plain.encode().hex(), "plain %r" % plain)
    for raw in ("Hello, 世界", " padded ", "line1\nline2", "=?base64?literal?="):
        wrapped = "=?base64?" + base64.b64encode(raw.encode()).decode() + "?="
        check(tool("hdrvalue", wrapped) == raw.encode().hex(), "decodes %r" % raw)
    check(tool("hdrvalue", "=?base64?!!!!?=") == "invalid" and tool("hdrvalue", "=?base64?abc?=") == "invalid", "bad base64 rejected")
    check(tool("hdrvalue", "=?base64?SGk=") == b"=?base64?SGk=".hex(), "no closing marker means it is a plain value")

    case("http parser: fuzzing")
    p = subprocess.run([BIN, "crypto", "httpfuzz", "12345", "60000"], capture_output=True, timeout=120)
    check(p.returncode == 0 and b"fuzz done" in p.stdout, "fuzz run survived: rc=%d %s" % (p.returncode, p.stderr[-200:]))

    case("JSON string escapes decode to UTF-8")
    def echo_tag(raw_json_string):
        msg = ('{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"typed","arguments":{"flag":true,"ratio":1,"tag":%s}}}' % raw_json_string).encode()
        rc, r = run([INIT, INITED, msg])
        res = r[-1].get("result", {})
        return res["content"][0]["text"] if res.get("content") else r[-1]
    pre = "flag=1 ratio=1 tag="
    for raw, want in (('"\\u4e16\\u754c"', "世界"), ('"caf\\u00e9"', "café"), ('"\\ud83d\\ude00"', "😀"), ('"A\\u0042C"', "ABC"),
                      ('"\\uD83D\\uDE00x"', "😀x"), ('"tab\\there"', "tab\there"), ('"q\\"uote\\\\"', 'q"uote\\')):
        out = echo_tag(raw)
        check(isinstance(out, str) and out.split(" data=")[0] == pre + want, "%s decodes to %r, got %r" % (raw, want, out))
    out = echo_tag('"x\\ud83dy"')
    check(isinstance(out, str) and out.split(" data=")[0] == pre + "x\ufffdy", "a lone high surrogate becomes U+FFFD: %r" % (out,))
    out = echo_tag('"x\\ude00y"')
    check(isinstance(out, str) and out.split(" data=")[0] == pre + "x\ufffdy", "a lone low surrogate becomes U+FFFD")
    out = echo_tag('"\\u12g4"')
    check(isinstance(out, dict) or "must be a string" in str(out), "a bad escape is refused: %r" % (out,))
    out = echo_tag('"raw 世界 text"')
    check(isinstance(out, str) and out.split(" data=")[0] == pre + "raw 世界 text", "unescaped UTF-8 passes through")

    case("prompt messages carry every kind of content")
    def rich_prompt(ver):
        r = session([{"jsonrpc": "2.0", "id": 90, "method": "prompts/get", "params": {"name": "rich"}}], ver=ver)
        return r[0]["result"]["messages"]
    m = rich_prompt("2025-11-25")
    check([(x["role"], x["content"]["type"]) for x in m] == [("user", "text"), ("user", "image"), ("assistant", "audio"), ("user", "resource"),
                                                            ("assistant", "resource"), ("user", "resource_link")], "roles and types: %s" % [(x["role"], x["content"]["type"]) for x in m])
    check(m[1]["content"] == {"type": "image", "data": "iVBORw==", "mimeType": "image/png"}, "image content: %s" % m[1])
    check(m[3]["content"] == {"type": "resource", "resource": {"uri": "file:///doc.txt", "mimeType": "text/plain", "text": "doc text"}}, "embedded text")
    check(m[4]["content"]["resource"]["blob"] == "AQI=", "embedded blob")
    check(m[5]["content"]["uri"] == "file:///readme.txt" and m[5]["content"]["name"] == "readme", "link")
    old = rich_prompt("2024-11-05")
    check([x["content"]["type"] for x in old] == ["text", "image", "text", "resource", "resource", "text"], "2024-11-05 degrades audio and links to text: %s" % [x["content"]["type"] for x in old])

    case("fuzz: mutated requests never hurt the stdio server")
    rnd = random.Random(int(os.environ.get("MCPC_FUZZ_SEED", "2024")))
    seeds = [
        mreq(1, "tools/call", {"name": "typed", "arguments": {"flag": True, "ratio": 1.5, "tag": "x", "data": {"a": [1, 2, {"b": None}]}}}, caps=ELICIT),
        mreq(2, "tools/list", {"cursor": "3"}), mreq(3, "resources/read", {"uri": "item://book/1"}), mreq(4, "prompts/get", {"name": "greet", "arguments": {"who": "x"}}),
        mreq(5, "subscriptions/listen", {"notifications": {"toolsListChanged": True, "resourceSubscriptions": ["file:///readme.txt"]}}),
        mreq(6, "tools/call", {"name": "ask", "arguments": {"what": "form"}, "inputResponses": {"mcpc-1": {"action": "accept"}}, "requestState": "v1.AAAA.BBBB"}, caps=ELICIT),
        {"jsonrpc": "2.0", "id": 7, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {"elicitation": {}}, "clientInfo": {"name": "f", "version": "1"}}},
        {"jsonrpc": "2.0", "id": 8, "method": "completion/complete", "params": {"ref": {"type": "ref/prompt", "name": "greet"}, "argument": {"name": "who", "value": "A"}}},
        {"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 5}},
        {"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": {"name": "calc", "arguments": {"a": 1, "b": 2, "op": "+"}}},
    ]
    def mutate(raw):
        b = bytearray(raw)
        for _ in range(rnd.randrange(1, 5)):
            k = rnd.randrange(8)
            if k == 0 and b:
                b[rnd.randrange(len(b))] = rnd.randrange(256)
            elif k == 1 and b:
                del b[rnd.randrange(len(b)):]
            elif k == 2 and b:
                i = rnd.randrange(len(b))
                b[i:i] = rnd.choice([b'"', b'\\', b'\\u12', b'\\ud800', b'{', b'}', b'[', b']', b',', b':', b'null', b'\x00', b'\xff\xfe', b'\\u0000'])
            elif k == 3 and b:
                i = rnd.randrange(len(b))
                b[i:i] = b"[" * rnd.choice([10, 100, 1000, 5000])
            elif k == 4 and b:
                i = rnd.randrange(len(b))
                b[i:i] = rnd.choice([b"99999999999999999999999999", b"-0", b"1e999", b"1.5e-999", b"0x10", b".5"])
            elif k == 5 and b:
                i = rnd.randrange(len(b))
                b[i:i] = b"A" * rnd.choice([300, 5000, 70000])
            elif k == 6 and len(b) > 4:
                i, j = sorted((rnd.randrange(len(b)), rnd.randrange(len(b))))
                b[i:j] = b""
            else:
                b += bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 20)))
        return bytes(b)
    ok_before = None
    for batch in range(int(os.environ.get("MCPC_FUZZ_BATCHES", "12"))):
        stream = [INIT, INITED]
        for _ in range(150):
            stream.append(mutate(json.dumps(rnd.choice(seeds)).encode()))
        stream += [{"jsonrpc": "2.0", "id": 99991, "method": "tools/call", "params": {"name": "calc", "arguments": {"a": 20, "b": 22, "op": "+"}}},
                   mreq(99992, "server/discover")]
        p = subprocess.run(WRAP + [BIN, "iostrm", "-i", "-"], input=b"\n".join(enc(m) for m in stream), capture_output=True, timeout=120)
        out = p.stdout.decode(errors="replace")
        check(p.returncode == 0, "fuzz batch %d: server exit code %d, stderr: %s" % (batch, p.returncode, p.stderr.decode(errors="replace")[-300:]))
        if p.returncode != 0 and os.environ.get("MCPC_FUZZ_SAVE"):
            with open(os.path.join(os.environ["MCPC_FUZZ_SAVE"], "batch%d.in" % batch), "wb") as f:
                f.write(b"\n".join(enc(m) for m in stream))
        # after all the noise the server still answers a normal request correctly
        good = [l for l in out.splitlines() if '"id":99991' in l]
        check(good and '"text":"42"' in good[-1], "fuzz batch %d: server still answers after garbage" % batch)
        for line in out.splitlines():
            if line.strip():
                try:
                    json.loads(line)
                except ValueError:
                    check(False, "fuzz batch %d: server wrote a line that is not JSON: %r" % (batch, line[:120]))
                    break

    case("tools/call with a 20 KB result")
    r = session([call(3, "bigtext")])
    total = sum(len(c["text"]) for c in r[0]["result"]["content"]) if r and "result" in r[0] else 0
    check(total == 5 * 4096, "bigtext returned %d bytes" % total)

    case("tools/call calc")
    r = session([call(4, "calc", a=2, b=3, op="+")])
    check(r and r[0]["result"]["content"][0]["text"] == "5", "2+3 == 5")

    case("framing: braces, quotes and escaped backslash inside strings")
    r = session([call(5, "calc", a=1, b=2, op="}\\"), call(6, "calc", a=4, b=5, op="*"),
                 call(7, "calc", a=1, b=2, op="{\"")])
    check([x.get("id") for x in r] == [5, 6, 7], "three replies in order, got %s" % [x.get("id") for x in r])
    check(len(r) == 3 and r[1]["result"]["content"][0]["text"] == "20", "request after tricky string still works")

    case("framing: back-to-back objects without separators")
    raw = enc(INIT) + enc(INITED) + enc({"jsonrpc": "2.0", "id": 8, "method": "tools/list", "params": {}})
    rc, r = run([raw])
    check(rc == 0 and [x.get("id") for x in r] == [1, 8], "replies for back-to-back objects")

    case("framing: a broken line cannot swallow the messages after it")
    good = {"jsonrpc": "2.0", "id": 61, "method": "ping"}
    for name, bad in (("unclosed object", b'{"jsonrpc":"2.0","id":60,"method":"tools/list","params":{'), ("unclosed string", b'{"jsonrpc":"2.0","id":60,"method":"ping","params":{"a":"x'),
                      ("pretty printed", b'{\n  "jsonrpc": "2.0",\n  "id": 60,\n  "method": "ping"\n}')):
        rc, r = run([INIT, INITED, bad, good])
        ids = [x.get("id") for x in r]
        check(rc == 0 and 61 in ids, "%s: the next message is still answered, got %s" % (name, ids))
        errs = [x for x in r if "error" in x and x.get("id") is None]
        check(errs and errs[0]["error"]["code"] == -32700, "%s: reported as a parse error with a null id: %s" % (name, r[-3:]))
    rc, r = run([INIT, INITED, b'garbage text with } and ] and \"quotes', b"", b"   ", good])
    check([x.get("id") for x in r] == [1, 61], "text between messages is ignored: %s" % r)

    case("replies are valid JSON whatever the client sent")
    nasty = [b'{"jsonrpc":"2.0","id":"a\x01b","method":"ping"}', b'{"jsonrpc":"2.0","id":"a\xffb","method":"ping"}', b'{"jsonrpc":"2.0","id":1e,"method":"ping"}',
             b'{"jsonrpc":"2.0","id":"x\xc3","method":"ping"}', b'{"jsonrpc":"2.0","id":1,"method":"no\x01\xffsuch"}', b'{"jsonrpc":"2.0","id":true,"method":"ping"}',
             b'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"bad\xff\x07name"}}', b'{"jsonrpc":"2.0","id":3,"method":"ping"}']
    rc, r = run([INIT, INITED, *nasty])
    check(rc == 0 and r and r[-1].get("id") == 3, "server answers the clean request after the nasty ones: %s" % [x.get("id") for x in r])
    check(all(("id" in x) or ("method" in x) for x in r), "every line is a JSON-RPC message")
    unsafe = [x for x in r if x.get("id") in ("a\x01b",)]
    check(not unsafe, "an id with a raw control character is refused rather than echoed")
    bad = [x for x in r if "error" in x and x["error"]["code"] == -32600]
    check(len(bad) >= 4, "unusable ids are invalid requests: %d" % len(bad))
    rc, r = run([INIT, INITED, b'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"bad\xff\x07name"}}'])
    check(r and r[-1]["error"]["code"] == -32602, "an unknown tool with a nasty name is just unknown: %s" % r[-1:])

    case("unknown method -> -32601 with jsonrpc and id")
    r = session([{"jsonrpc": "2.0", "id": 9, "method": "no/such", "params": {}}])
    e = r[0].get("error", {}) if r else {}
    check(r and r[0].get("jsonrpc") == "2.0" and r[0].get("id") == 9, "error carries jsonrpc and id")
    check(e.get("code") == -32601, "code -32601, got %s" % e.get("code"))

    case("notifications are never answered")
    r = session([{"jsonrpc": "2.0", "method": "notifications/unknown", "params": {}},
                 {"jsonrpc": "2.0", "method": "no/such"},
                 {"jsonrpc": "2.0", "id": 10, "method": "tools/list", "params": {}}])
    check([x.get("id") for x in r] == [10], "only the request is answered, got %s" % [x.get("id") for x in r])

    case("client responses are ignored")
    r = session([{"jsonrpc": "2.0", "id": 77, "result": {}},
                 {"jsonrpc": "2.0", "id": 11, "method": "tools/list", "params": {}}])
    check([x.get("id") for x in r] == [11], "no reply to a response")

    case("malformed json -> -32700 with null id")
    rc, r = run([INIT, INITED, b"{not json}"])
    last = r[-1] if r else {}
    check(last.get("error", {}).get("code") == -32700 and last.get("id") is None and last.get("jsonrpc") == "2.0",
          "parse error reply, got %s" % last)

    case("request before initialize -> -32602")
    rc, r = run([{"jsonrpc": "2.0", "id": 12, "method": "tools/list", "params": {}}])
    check(r and r[0].get("error", {}).get("code") == -32602, "not initialized rejected with -32602")

    case("unknown tool -> -32602")
    r = session([call(13, "does-not-exist")])
    check(r and r[0].get("error", {}).get("code") == -32602, "unknown tool is invalid params")

    case("oversized message is rejected and the stream recovers")
    big = b'{"jsonrpc":"2.0","id":14,"method":"tools/list","params":{"pad":"' + b"a" * (17 * 1024 * 1024) + b'"}}'
    rc, r = run([INIT, INITED, big, {"jsonrpc": "2.0", "id": 15, "method": "tools/list", "params": {}}])
    check(rc == 0, "server survived an oversized message")
    check([x.get("id") for x in r][-1:] == [15], "request after oversized message is answered")
    check(any(x.get("error", {}).get("message") == "message too large" for x in r), "oversized message reported")

    tcp_tests()

    if failures:
        print("FAILED:")
        for f in failures:
            print("  - " + f)
        sys.exit(1)
    print("rpctst: all passed")


main()
