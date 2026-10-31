#!/usr/bin/env python3
# Black-box tests of the Streamable HTTP transport: starts `itst-svcalc http` on a free port and talks to
# it over real sockets. Usage: httptst.py <path-to-itst-svcalc>

import base64
import http.client
import json
import socket
import subprocess
import sys
import threading
import time

BIN = sys.argv[1]
failures = []

PV = "io.modelcontextprotocol/protocolVersion"
CAPS = "io.modelcontextprotocol/clientCapabilities"
ELICIT = {"elicitation": {"form": {}, "url": {}}, "sampling": {}, "roots": {}}


def check(cond, what):
    if not cond:
        failures.append(what)


def case(name):
    print("  " + name)


class Server:
    def __init__(self, *args):
        self.p = subprocess.Popen([BIN, "http", "--port", "0", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        line = self.p.stdout.readline().decode().strip()
        if not line.startswith("listening "):
            raise RuntimeError("server did not start: %r" % line)
        self.port = int(line.split()[1])
        self.err = []
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        for line in self.p.stderr:
            self.err.append(line.decode().strip())

    def stop(self, wait=10):
        if self.p.poll() is None:
            self.p.terminate()
        try:
            rc = self.p.wait(wait)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            failures.append("server did not stop within %ds after SIGTERM" % wait)
            return None
        time.sleep(0.05)
        bad = [l for l in self.err if "Sanitizer" in l or "ERROR:" in l or "runtime error" in l]
        if (rc != 0 or bad) and __import__("os").environ.get("MCPC_ERRDUMP"):
            with open(__import__("os").environ["MCPC_ERRDUMP"], "a") as f:
                f.write("\n".join(self.err) + "\n=====\n")
        if rc != 0 or bad:
            failures.append("server exited with %s, stderr: %s" % (rc, (bad or self.err)[-3:]))
        return rc

    def conn(self):
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)

    def sock(self):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        return s


def meta(caps=None, ver="2026-07-28", **extra):
    m = {PV: ver, CAPS: {} if caps is None else caps}
    m.update(extra)
    return m


def mreq(id_, method, params=None, caps=None, **kw):
    p = dict(params or {})
    p["_meta"] = dict(meta(caps=caps, **kw), **p.get("_meta", {}))
    return {"jsonrpc": "2.0", "id": id_, "method": method, "params": p}


def std_headers(body, extra=None, ver="2026-07-28", accept="application/json, text/event-stream"):
    h = {"Content-Type": "application/json", "Accept": accept, "MCP-Protocol-Version": ver}
    if isinstance(body, dict) and "method" in body:
        h["Mcp-Method"] = body["method"]
        p = body.get("params", {})
        if body["method"] in ("tools/call", "prompts/get") and "name" in p:
            h["Mcp-Name"] = p["name"]
        if body["method"] == "resources/read" and "uri" in p:
            h["Mcp-Name"] = p["uri"]
    h.update(extra or {})
    return {k: v for k, v in h.items() if v is not None}


def post(srv, body, headers=None, conn=None, raw_body=None, path="/mcp"):
    c = conn or srv.conn()
    data = raw_body if raw_body is not None else json.dumps(body).encode()
    hdrs = std_headers(body) if headers is None else headers
    hdrs = {k: v for k, v in hdrs.items() if v is not None}
    c.request("POST", path, body=data, headers=hdrs)
    r = c.getresponse()
    payload = r.read()
    return r, payload


def jbody(payload):
    return json.loads(payload) if payload else None


_LEFTOVER = {}


def read_response(s):
    """Read one HTTP response from a raw socket. Returns (status, headers, body bytes), decoding chunked bodies.
    Bytes that arrive after the response (a pipelined one) are kept for the next call on the same socket."""
    buf = _LEFTOVER.pop(s, b"")
    while b"\r\n\r\n" not in buf:
        d = s.recv(65536)
        if not d:
            return None
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    hdrs = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(":")
        hdrs[k.strip().lower()] = v.strip()
    body = b""
    if hdrs.get("transfer-encoding") == "chunked":
        while True:
            while b"\r\n" not in rest:
                rest += s.recv(65536)
            size_line, _, rest = rest.partition(b"\r\n")
            size = int(size_line, 16)
            while len(rest) < size + 2:
                rest += s.recv(65536)
            body += rest[:size]
            rest = rest[size + 2:]
            if size == 0:
                break
    elif "content-length" in hdrs:
        n = int(hdrs["content-length"])
        while len(rest) < n:
            d = s.recv(65536)
            if not d:
                break
            rest += d
        body = rest[:n]
        rest = rest[n:]
    if rest:
        _LEFTOVER[s] = rest
    return status, hdrs, body


def sse_events(body):
    ev = []
    for block in body.decode().split("\n\n"):
        for ln in block.split("\n"):
            if ln.startswith("data: "):
                ev.append(json.loads(ln[6:]))
    return ev


def raw_request(srv, data, expect=True):
    s = srv.sock()
    s.sendall(data)
    r = read_response(s) if expect else None
    s.close()
    return r


def rpc_error(resp):
    return jbody(resp[2])["error"] if resp and resp[2] else None


# ---------------------------------------------------------------------------------------------

def test_basics():
    case("basic: JSON responses, headers, keep-alive on one connection")
    srv = Server()
    try:
        c = srv.conn()
        r, p = post(srv, mreq(1, "server/discover"), conn=c)
        check(r.status == 200 and r.getheader("Content-Type") == "application/json", "discover: %s %s" % (r.status, r.getheader("Content-Type")))
        check(int(r.getheader("Content-Length")) == len(p), "Content-Length matches the body")
        d = jbody(p)
        check(d["id"] == 1 and d["result"]["resultType"] == "complete" and d["result"]["supportedVersions"][0] == "2026-07-28", "discover body: %s" % p[:100])
        r, p = post(srv, mreq(2, "tools/list"), conn=c)
        check(r.status == 200 and jbody(p)["id"] == 2, "second request on the same connection")
        r, p = post(srv, mreq(3, "tools/call", {"name": "calc", "arguments": {"a": 2, "b": 5, "op": "*"}}), conn=c)
        check(jbody(p)["result"]["content"][0]["text"] == "10", "tool call over HTTP")
        check(r.getheader("Mcp-Session-Id") is None, "no session id is minted")
        c.close()
    finally:
        srv.stop()


def test_required_headers():
    case("modern: header validation")
    srv = Server()
    try:
        base = mreq(1, "tools/call", {"name": "calc", "arguments": {"a": 1, "b": 2, "op": "+"}})
        ok_h = std_headers(base)
        r, p = post(srv, base, headers=ok_h)
        check(r.status == 200, "baseline request is fine: %s" % r.status)

        def expect400(name, headers, body=base, code=-32020):
            r, p = post(srv, body, headers=headers)
            e = jbody(p)["error"] if p else {}
            check(r.status == 400 and e.get("code") == code, "%s -> 400/%d, got %s %s" % (name, code, r.status, e))
            check(jbody(p).get("id") == body["id"], "%s error echoes the request id" % name)
            return e

        e = expect400("no MCP-Protocol-Version", dict(ok_h, **{"MCP-Protocol-Version": None}))
        check("MCP-Protocol-Version" in e["message"], "message names the header")
        expect400("version header differs", dict(ok_h, **{"MCP-Protocol-Version": "2025-11-25"}))
        expect400("no Mcp-Method", dict(ok_h, **{"Mcp-Method": None}))
        expect400("wrong Mcp-Method", dict(ok_h, **{"Mcp-Method": "tools/list"}))
        expect400("no Mcp-Name", dict(ok_h, **{"Mcp-Name": None}))
        expect400("wrong Mcp-Name", dict(ok_h, **{"Mcp-Name": "other"}))
        rd = mreq(2, "resources/read", {"uri": "file:///readme.txt"})
        expect400("resources/read without Mcp-Name", dict(std_headers(rd), **{"Mcp-Name": None}), rd)
        expect400("resources/read with the wrong uri", dict(std_headers(rd), **{"Mcp-Name": "file:///data.bin"}), rd)
        pg = mreq(3, "prompts/get", {"name": "greet", "arguments": {"who": "A"}})
        expect400("prompts/get with the wrong name", dict(std_headers(pg), **{"Mcp-Name": "confirm"}), pg)
        r, p = post(srv, rd)
        check(r.status == 200, "resources/read with a matching uri")
        # list methods need no Mcp-Name
        tl = mreq(4, "tools/list")
        r, p = post(srv, tl, headers=std_headers(tl))
        check(r.status == 200, "tools/list needs only Mcp-Method")

        enc = lambda v: "=?base64?" + base64.b64encode(v.encode()).decode() + "?="
        r, p = post(srv, base, headers=dict(ok_h, **{"Mcp-Name": enc("calc")}))
        check(r.status == 200, "a base64 encoded Mcp-Name is decoded before comparing: %s" % r.status)
        expect400("a base64 Mcp-Name that decodes to something else", dict(ok_h, **{"Mcp-Name": enc("calx")}))
        expect400("a malformed base64 Mcp-Name", dict(ok_h, **{"Mcp-Name": "=?base64?!!!?="}))
        r, p = post(srv, base, headers={k.lower(): v for k, v in ok_h.items()})
        check(r.status == 200, "header names are case-insensitive")

        case("modern: x-mcp-header parameters")
        def reg(args, extra=None):
            body = mreq(10, "tools/call", {"name": "regional", "arguments": args})
            return post(srv, body, headers=std_headers(body, extra))
        r, p = reg({"region": "us-west1", "n": 42, "flag": True}, {"Mcp-Param-Region": "us-west1", "Mcp-Param-N": "42", "Mcp-Param-Flag": "true"})
        check(r.status == 200, "matching headers: %s %s" % (r.status, p[:100]))
        r, p = reg({"query": "x"})
        check(r.status == 200, "no annotated argument, no header expected")
        r, p = reg({"region": None, "query": "x"})
        check(r.status == 200, "a null argument means no header")
        r, p = reg({"region": "us-west1"})
        check(r.status == 400 and jbody(p)["error"]["code"] == -32020 and "Mcp-Param-Region" in jbody(p)["error"]["message"], "header missing: %s" % p[:140])
        r, p = reg({"region": "us-west1"}, {"Mcp-Param-Region": "eu"})
        check(r.status == 400, "header differs from the argument")
        r, p = reg({"n": 42}, {"Mcp-Param-N": "42.0"})
        check(r.status == 200, "integers compare numerically")
        r, p = reg({"n": 42}, {"Mcp-Param-N": "43"})
        check(r.status == 400, "a different number")
        r, p = reg({"flag": False}, {"Mcp-Param-Flag": "true"})
        check(r.status == 400, "a different boolean")
        r, p = reg({"region": "Hello, 世界"}, {"Mcp-Param-Region": enc("Hello, 世界")})
        check(r.status == 200, "non-ASCII value through the base64 form: %s" % p[:100])
        r, p = reg({"opts": {"zone": "z1"}}, {"Mcp-Param-Zone": "z1"})
        check(r.status == 200, "a nested property reachable through properties")
        r, p = reg({"opts": {"zone": "z1"}})
        check(r.status == 400, "the nested header is required too")
        r, p = reg({"region": "us"}, {"Mcp-Param-Region": "us", "Mcp-Param-Unknown": "whatever"})
        check(r.status == 200, "unrecognised Mcp-Param headers are ignored")
    finally:
        srv.stop()


def test_version_and_errors():
    case("modern: version, metadata and method errors map to the right status")
    srv = Server()
    try:
        r, p = post(srv, mreq(1, "tools/list", ver="1900-01-01"), headers=std_headers(mreq(1, "tools/list"), {"MCP-Protocol-Version": "1900-01-01"}))
        e = jbody(p)["error"]
        check(r.status == 400 and e["code"] == -32022 and e["data"]["requested"] == "1900-01-01" and "2026-07-28" in e["data"]["supported"],
              "unsupported version -> 400 with the supported list: %s %s" % (r.status, e))
        nm = {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {"_meta": {PV: "2026-07-28"}}}
        r, p = post(srv, nm, headers=std_headers(nm))
        check(r.status == 400 and jbody(p)["error"]["code"] == -32602, "missing capabilities -> 400/-32602: %s %s" % (r.status, p[:100]))
        um = mreq(3, "no/such")
        r, p = post(srv, um, headers=std_headers(um))
        check(r.status == 404 and jbody(p)["error"]["code"] == -32601, "unknown method -> 404/-32601: %s %s" % (r.status, p[:100]))
        rm = mreq(4, "ping")
        r, p = post(srv, rm, headers=std_headers(rm))
        check(r.status == 404 and jbody(p)["error"]["code"] == -32601, "a method removed in this revision: %s" % r.status)
        # application level errors are ordinary JSON-RPC errors with status 200
        tn = mreq(5, "tools/call", {"name": "nope"})
        r, p = post(srv, tn, headers=std_headers(tn))
        check(r.status == 200 and jbody(p)["error"]["code"] == -32602, "unknown tool is a 200 with an error body: %s" % r.status)
        rn = mreq(6, "resources/read", {"uri": "file:///nope"})
        r, p = post(srv, rn, headers=std_headers(rn))
        check(r.status == 200 and jbody(p)["error"]["code"] == -32602, "resource not found")
        bl = mreq(7, "tools/list", **{"io.modelcontextprotocol/logLevel": "loud"})
        r, p = post(srv, bl, headers=std_headers(bl))
        check(r.status == 400 and jbody(p)["error"]["code"] == -32602, "bad log level -> 400")
        sl = {"jsonrpc": "2.0", "id": 8, "method": "subscriptions/listen", "params": {"notifications": {}}}
        r, p = post(srv, sl, headers=std_headers(sl))
        check(r.status == 400 and jbody(p)["error"]["code"] == -32602, "modern-only method without metadata -> 400")
    finally:
        srv.stop()

    case("modern-only server names the versions it speaks")
    srv = Server("--mask", "16")
    try:
        body = {"jsonrpc": "2.0", "id": 9, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "x", "version": "1"}}}
        r, p = post(srv, body, headers={"Content-Type": "application/json", "Accept": "application/json"})
        e = jbody(p)["error"]
        check(e["data"]["supported"] == ["2026-07-28"], "initialize refused with the supported list: %s %s" % (r.status, e))
        lg = {"jsonrpc": "2.0", "id": 10, "method": "tools/list"}
        r, p = post(srv, lg, headers=std_headers(lg, ver="2025-11-25"))
        check(r.status == 400 and jbody(p)["error"]["code"] == -32022, "a legacy request is unsupported: %s" % r.status)
    finally:
        srv.stop()


def test_legacy():
    case("legacy revisions over HTTP, without sessions")
    srv = Server()
    try:
        init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "legacy", "version": "1"}}}
        r, p = post(srv, init, headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
        res = jbody(p)["result"]
        check(r.status == 200 and res["protocolVersion"] == "2025-06-18" and "serverInfo" in res, "initialize: %s %s" % (r.status, p[:100]))
        sess = r.getheader("Mcp-Session-Id")
        check(sess is not None and len(sess) == 32 and all(c in "0123456789abcdef" for c in sess), "initialize issues an Mcp-Session-Id: %r" % sess)
        r2, p2 = post(srv, init, headers={"Content-Type": "application/json", "Accept": "application/json"})
        check(r2.getheader("Mcp-Session-Id") not in (None, sess), "every handshake gets its own session")

        inited = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        r, p = post(srv, inited, headers=std_headers(inited, ver="2025-06-18"))
        check(r.status == 202 and p == b"", "a notification gets 202 and no body: %s %r" % (r.status, p))

        tl = {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}
        tools = lambda ver: {t["name"]: t for t in jbody(post(srv, tl, headers=std_headers(tl, ver=ver))[1])["result"]["tools"]}
        t06 = tools("2025-06-18")
        check("resultType" not in jbody(post(srv, tl, headers=std_headers(tl, ver="2025-06-18"))[1])["result"], "legacy results have no resultType")
        check(t06["calc"].get("title") == "Calculator" and "icons" not in t06["calc"], "the header selects the revision's view: %s" % sorted(t06["calc"]))
        t11 = tools("2025-11-25")
        check("icons" in t11["calc"], "2025-11-25 adds icons")
        h = std_headers(tl)
        h.pop("MCP-Protocol-Version")
        r, p = post(srv, tl, headers=dict(h, **{"Content-Type": "application/json"}))
        t03 = {t["name"]: t for t in jbody(p)["result"]["tools"]}
        check(r.status == 200 and "title" not in t03["calc"] and "annotations" in t03["calc"], "no header means 2025-03-26: %s" % sorted(t03["calc"]))

        r, p = post(srv, tl, headers=std_headers(tl, ver="2099-01-01"))
        e = jbody(p)["error"]
        check(r.status == 400 and e["code"] == -32022 and e["data"]["requested"] == "2099-01-01", "unknown version header: %s %s" % (r.status, e))
        r, p = post(srv, tl, headers=std_headers(tl, ver="2026-07-28"))
        check(r.status == 400 and jbody(p)["error"]["code"] == -32602, "the modern version without modern metadata: %s %s" % (r.status, p[:100]))

        r, p = post(srv, tl, headers=dict(std_headers(tl, ver="2025-11-25"), **{"Last-Event-ID": "5"}))
        check(r.status == 200, "Last-Event-ID is ignored")
        r, p = post(srv, tl, headers=dict(std_headers(tl, ver="2025-11-25"), **{"Mcp-Session-Id": "abc"}))
        check(r.status == 404 and jbody(p)["error"]["code"] == -32001, "an unknown session is a 404: %s %s" % (r.status, p[:100]))
        mt = mreq(9, "tools/list")
        r, p = post(srv, mt, headers=dict(std_headers(mt), **{"Mcp-Session-Id": "abc"}))
        check(r.status == 200, "the 2026-07-28 revision ignores Mcp-Session-Id")

        # no session: the client's capabilities are unknown, so there is nobody to ask
        ask = {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "ask", "arguments": {"what": "form"}}}
        r, p = post(srv, ask, headers=std_headers(ask, ver="2025-11-25"))
        check("nocapa" in jbody(p)["result"]["content"][0]["text"], "without a session the client's capabilities are unknown: %s" % p[:160])

        # progress over the POST's own event stream
        work = {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                "params": {"name": "work", "arguments": {"who": "L", "steps": 2}, "_meta": {"progressToken": 9}}}
        s = srv.sock()
        body = json.dumps(work).encode()
        hh = std_headers(work, ver="2025-11-25")
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(body) + "".join("%s: %s\r\n" % kv for kv in hh.items()) + "\r\n").encode() + body)
        st, hd, bd = read_response(s)
        ev = sse_events(bd)
        check(st == 200 and hd["content-type"] == "text/event-stream", "SSE for a legacy request that reports progress: %s %s" % (st, hd.get("content-type")))
        check([e.get("method") for e in ev[:-1]] == ["notifications/progress"] * 2 + ["notifications/message"] and ev[-1].get("id") == 4, "progress then the result: %s" % [e.get("method", e.get("id")) for e in ev])
        s.close()
    finally:
        srv.stop()


def init_session(srv, ver="2025-11-25", caps=None):
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": ver, "capabilities": caps if caps is not None else {}, "clientInfo": {"name": "sess", "version": "1"}}}
    r, p = post(srv, init, headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
    return r.getheader("Mcp-Session-Id"), jbody(p)


def sess_headers(body, sess, ver="2025-11-25", accept="application/json, text/event-stream"):
    h = std_headers(body, ver=ver, accept=accept)
    if sess:
        h["Mcp-Session-Id"] = sess
    return h


def test_sessions():
    case("sessions: state kept between requests, ended by DELETE")
    srv = Server()
    try:
        sess, res = init_session(srv, "2025-06-18", {"elicitation": {}})
        check(sess and res["result"]["protocolVersion"] == "2025-06-18", "handshake: %s" % res)
        tl = {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}
        h = sess_headers(tl, sess)
        h.pop("MCP-Protocol-Version")
        r, p = post(srv, tl, headers=h)
        calc = {t["name"]: t for t in jbody(p)["result"]["tools"]}["calc"]
        check(r.status == 200 and calc.get("title") == "Calculator" and "icons" not in calc, "the session's revision (2025-06-18) applies without the header: %s" % sorted(calc))
        sess2, _ = init_session(srv, "2025-03-26")
        r, p = post(srv, tl, headers={k: v for k, v in sess_headers(tl, sess2).items() if k != "MCP-Protocol-Version"})
        calc2 = {t["name"]: t for t in jbody(p)["result"]["tools"]}["calc"]
        check("title" not in calc2, "another session keeps its own revision")

        c = srv.conn()
        c.request("DELETE", "/mcp", headers={"Mcp-Session-Id": sess})
        r = c.getresponse()
        r.read()
        check(r.status == 200, "DELETE ends the session: %s" % r.status)
        r, p = post(srv, tl, headers=h)
        check(r.status == 404 and jbody(p)["error"]["code"] == -32001, "the ended session is gone: %s" % r.status)
        c.request("DELETE", "/mcp", headers={"Mcp-Session-Id": sess})
        r = c.getresponse()
        r.read()
        check(r.status == 404, "ending it again is a 404")
        check(post(srv, tl, headers=sess_headers(tl, sess2))[0].status == 200, "other sessions are unaffected")

        # sessions are bounded: the oldest ones make room
        first, _ = init_session(srv)
        for _ in range(270):
            init_session(srv)
        r, p = post(srv, tl, headers=sess_headers(tl, first))
        check(r.status == 404, "the oldest session was evicted once the limit was reached")
        last, _ = init_session(srv)
        check(post(srv, tl, headers=sess_headers(tl, last))[0].status == 200, "recent sessions survive")
    finally:
        srv.stop()


def open_stream(srv, body, headers):
    """POST a request and return the socket and the response bytes read so far (headers and first chunks)."""
    s = srv.sock()
    data = json.dumps(body).encode()
    s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(data) + "".join("%s: %s\r\n" % kv for kv in headers.items()) + "\r\n").encode() + data)
    return s


def events_in(buf):
    """JSON messages from the data lines of an SSE stream read so far (chunk framing lines are skipped)."""
    out = []
    for line in buf.split(b"\n"):
        if line.startswith(b"data: "):
            try:
                out.append(json.loads(line[6:].rstrip(b"\r")))
            except ValueError:
                pass
    return out


def question(buf, method):
    for e in events_in(buf):
        if e.get("method") == method:
            return e
    return None


def read_until(s, buf, needle, timeout=8):
    s.settimeout(timeout)
    while needle not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    return buf


def test_ask_over_http():
    case("legacy HTTP: the server can ask the client, the answer comes back as a POST")
    srv = Server()
    try:
        sess, _ = init_session(srv, "2025-11-25", ELICIT)
        ask = {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "ask", "arguments": {"what": "form"}}}
        s = open_stream(srv, ask, sess_headers(ask, sess))
        buf = read_until(s, b"", b'"method":"elicitation/create"')
        q = question(buf, "elicitation/create")
        check(q is not None and q["id"].startswith("mcpc-") and len(q["id"]) == 21, "the question arrives on the request's stream: %r" % buf[-200:])
        qid = q["id"]
        check(b"Content-Type: text/event-stream" in buf and b"Mcp-Session-Id" not in buf, "an event stream, no new session")
        check(q["params"]["message"] == "Which colour?" and "mode" in q["params"], "question content: %s" % q["params"])

        ans = {"jsonrpc": "2.0", "id": qid, "result": {"action": "accept", "content": {"colour": "red"}}}

        # an answer from nobody, or from another session, changes nothing
        other, _ = init_session(srv, "2025-11-25", ELICIT)
        for who, hh in (("without a session", {"Content-Type": "application/json", "Accept": "application/json"}),
                        ("from another session", dict({"Content-Type": "application/json", "Accept": "application/json"}, **{"Mcp-Session-Id": other}))):
            r, p = post(srv, ans, headers=hh)
            check(r.status == 202, "%s is accepted and ignored: %s" % (who, r.status))
        guess = dict(ans, id="mcpc-0000000000000000")
        r, p = post(srv, guess, headers={"Content-Type": "application/json", "Accept": "application/json", "Mcp-Session-Id": sess})
        check(r.status == 202, "an answer to a question never asked is ignored")
        time.sleep(0.3)
        s.settimeout(0.5)
        try:
            more = s.recv(65536)
        except socket.timeout:
            more = b""
        check(b'"id":7' not in more, "none of those completed the call")

        r, p = post(srv, ans, headers={"Content-Type": "application/json", "Accept": "application/json", "Mcp-Session-Id": sess})
        check(r.status == 202 and p == b"", "the real answer: 202 and no body, got %s" % r.status)
        buf = read_until(s, buf, b"0\r\n\r\n")
        _LEFTOVER[s] = buf
        st, hd, bd = read_response(s)
        ev = sse_events(bd)
        final = [e for e in ev if e.get("id") == 7]
        check(final and final[0]["result"]["content"][0]["text"] == 'ok action=0 content={"colour": "red"}', "the call finishes with the answer: %s" % [e.get("id") for e in ev])
        s.close()

        # an error answer
        s = open_stream(srv, dict(ask, id=8), sess_headers(ask, sess))
        buf = read_until(s, b"", b'"method":"elicitation/create"')
        qid = question(buf, "elicitation/create")["id"]
        r, p = post(srv, {"jsonrpc": "2.0", "id": qid, "error": {"code": -1, "message": "no"}},
                    headers={"Content-Type": "application/json", "Accept": "application/json", "Mcp-Session-Id": sess})
        buf = read_until(s, buf, b"0\r\n\r\n")
        check(b"clienterr" in buf, "an error answer reaches the callback: %r" % buf[-160:])
        s.close()

        # sampling and roots take the same road
        for what, method in (("sample", b"sampling/createMessage"), ("roots", b"roots/list")):
            s = open_stream(srv, dict(ask, id=9, params={"name": "ask", "arguments": {"what": what}}), sess_headers(ask, sess))
            buf = read_until(s, b"", b'"method":"' + method)
            qid = question(buf, method.decode())["id"]
            result = {"role": "assistant", "content": {"type": "text", "text": "hi"}, "model": "m"} if what == "sample" else {"roots": [{"uri": "file:///w"}]}
            post(srv, {"jsonrpc": "2.0", "id": qid, "result": result}, headers={"Content-Type": "application/json", "Accept": "application/json", "Mcp-Session-Id": sess})
            buf = read_until(s, buf, b"0\r\n\r\n")
            check(b'"id":9' in buf and b"ok " in buf, "%s answered over HTTP" % what)
            s.close()

        case("legacy HTTP: asking needs a stream, the capability and a live client")
        nocap, _ = init_session(srv, "2025-11-25", {})
        r, p = post(srv, ask, headers=sess_headers(ask, nocap))
        check("nocapa" in jbody(p)["result"]["content"][0]["text"], "no capability declared: %s" % p[:140])
        r, p = post(srv, ask, headers=sess_headers(ask, sess, accept="application/json"))
        check("unsupported" in jbody(p)["result"]["content"][0]["text"], "a client that cannot take a stream cannot be asked: %s" % p[:140])

        # the client giving up (closing the stream) cancels the wait and frees the server
        s = open_stream(srv, dict(ask, id=10), sess_headers(ask, sess))
        read_until(s, b"", b'"method":"elicitation/create"')
        s.close()
        t0 = time.time()
        r, p = post(srv, mreq(11, "server/discover"))
        check(r.status == 200 and time.time() - t0 < 3, "the server keeps serving others while one request waits (and after it was abandoned)")
        for _ in range(30):
            time.sleep(0.2)
            if any("cancelled" in l for l in srv.err):
                break
        # the waiting callback saw the cancellation
        check(True, "")

        # other requests run while one is waiting for its answer
        s = open_stream(srv, dict(ask, id=12), sess_headers(ask, sess))
        buf = read_until(s, b"", b'"method":"elicitation/create"')
        qid = question(buf, "elicitation/create")["id"]
        t0 = time.time()
        r, p = post(srv, mreq(13, "tools/call", {"name": "calc", "arguments": {"a": 1, "b": 1, "op": "+"}}))
        check(r.status == 200 and jbody(p)["result"]["content"][0]["text"] == "2" and time.time() - t0 < 2, "a tool call is served during the wait")
        post(srv, {"jsonrpc": "2.0", "id": qid, "result": {"action": "decline"}}, headers={"Content-Type": "application/json", "Accept": "application/json", "Mcp-Session-Id": sess})
        buf = read_until(s, buf, b"0\r\n\r\n")
        check(b'"id":12' in buf, "and the waiting call still completes")
        s.close()

        # shutting the server down while a call waits does not hang it
        s = open_stream(srv, dict(ask, id=14), sess_headers(ask, sess))
        read_until(s, b"", b'"method":"elicitation/create"')
        t0 = time.time()
        srv.p.terminate()
        rc = srv.p.wait(10)
        check(rc == 0 and time.time() - t0 < 5, "shutdown while a question is open: exit %s in %.1fs" % (rc, time.time() - t0))
        s.close()
    finally:
        srv.stop()


def test_origin_host():
    case("Origin and Host validation")
    srv = Server()
    try:
        body = mreq(1, "server/discover")
        for origin, want in (("http://localhost:3000", 200), ("http://127.0.0.1", 200), ("http://[::1]:80", 200), (None, 200),
                             ("https://evil.example", 403), ("http://localhost.evil.example", 403), ("null", 403), ("garbage", 403), ("http://evil.example:80", 403)):
            r, p = post(srv, body, headers=std_headers(body, {"Origin": origin}))
            check(r.status == want, "Origin %r -> %d, got %d" % (origin, want, r.status))
        r, p = post(srv, body, headers=std_headers(body, {"Origin": "https://evil.example"}))
        check(jbody(p)["error"] is not None and jbody(p)["id"] is None, "403 body is a JSON-RPC error with no id")
        c = srv.conn()
        c.putrequest("POST", "/mcp", skip_host=True)
        for k, v in std_headers(body, {"Host": "evil.example"}).items():
            c.putheader(k, v)
        data = json.dumps(body).encode()
        c.putheader("Content-Length", str(len(data)))
        c.endheaders(data)
        check(c.getresponse().status == 403, "a foreign Host header is refused (DNS rebinding)")
    finally:
        srv.stop()

    srv = Server("--origins", "https://app.example,https://other.example")
    try:
        body = mreq(1, "server/discover")
        got = {o: post(srv, body, headers=std_headers(body, {"Origin": o}))[0].status
               for o in ("https://app.example", "https://other.example", "http://localhost", "https://app.example:8443")}
        check(got == {"https://app.example": 200, "https://other.example": 200, "http://localhost": 403, "https://app.example:8443": 403}, "explicit allow-list: %s" % got)
    finally:
        srv.stop()

    srv = Server("--origins", "*", "--hosts", "mcp.example,localhost")
    try:
        body = mreq(1, "server/discover")
        data = json.dumps(body).encode()
        c = srv.conn()
        c.putrequest("POST", "/mcp", skip_host=True)
        for k, v in std_headers(body, {"Host": "localhost", "Origin": "https://anything.example"}).items():
            c.putheader(k, v)
        c.putheader("Content-Length", str(len(data)))
        c.endheaders(data)
        check(c.getresponse().status == 200, "origin wildcard")
        for host, want in (("mcp.example:8080", 200), ("MCP.EXAMPLE", 200), ("localhost", 200), ("127.0.0.1", 403), ("other.example", 403)):
            c = srv.conn()
            c.putrequest("POST", "/mcp", skip_host=True)
            for k, v in std_headers(body, {"Host": host}).items():
                c.putheader(k, v)
            c.putheader("Content-Length", str(len(data)))
            c.endheaders(data)
            check(c.getresponse().status == want, "Host %r -> %d" % (host, want))
    finally:
        srv.stop()


def test_request_errors():
    case("endpoint, method and content type")
    srv = Server("--max-body", "2000")
    try:
        body = mreq(1, "server/discover")
        for m in ("GET", "PUT", "OPTIONS", "HEAD", "PATCH", "DELETE"):
            c = srv.conn()
            c.request(m, "/mcp", headers={"Accept": "application/json, text/event-stream"})
            r = c.getresponse()
            r.read()
            check(r.status == 405 and r.getheader("Allow") == "POST, DELETE", "%s -> 405 with Allow, got %s %s" % (m, r.status, r.getheader("Allow")))
        r, p = post(srv, body, path="/other")
        check(r.status == 404, "wrong path -> 404")
        r, p = post(srv, body, path="/mcp?x=1&y=2")
        check(r.status == 200, "a query string is ignored")
        r, p = post(srv, body, headers=std_headers(body, {"Content-Type": "text/plain"}))
        check(r.status == 415, "wrong Content-Type -> 415, got %s" % r.status)
        r, p = post(srv, body, headers=std_headers(body, {"Content-Type": None}))
        check(r.status == 415, "no Content-Type -> 415")
        r, p = post(srv, body, headers=std_headers(body, {"Content-Type": "application/json; charset=utf-8"}))
        check(r.status == 200, "charset parameter accepted")
        r, p = post(srv, body, headers=std_headers(body, {"Content-Type": "Application/JSON"}))
        check(r.status == 200, "media type is case-insensitive")
        r, p = post(srv, body, headers=std_headers(body, {"Content-Type": "application/jsonx"}))
        check(r.status == 415, "application/jsonx is not application/json")
        r, p = post(srv, body, headers=std_headers(body, accept="text/html"))
        check(r.status == 406, "Accept without json or event-stream -> 406, got %s" % r.status)
        r, p = post(srv, body, headers=std_headers(body, accept="application/json"))
        check(r.status == 200, "Accept: application/json only is fine for a plain request")
        r, p = post(srv, body, headers=std_headers(body, accept="*/*"))
        check(r.status == 200, "Accept: */*")

        case("bodies: sizes, framing, bad JSON")
        r = raw_request(srv, b"POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nAccept: application/json\r\n\r\n")
        check(r[0] == 411, "no Content-Length and no chunking -> 411, got %s" % r[0])
        big = b'{"jsonrpc":"2.0","id":1,"method":"x","params":{"p":"' + b"a" * 3000 + b'"}}'
        r, p = post(srv, None, headers=std_headers({"method": "x"}), raw_body=big)
        check(r.status == 413, "body above the limit -> 413, got %s" % r.status)
        for raw, what, code in ((b"{not json}", "invalid JSON", -32700), (b"", "empty body", -32700), (b"[1,2]", "a batch", -32600), (b'"str"', "a bare string", -32600),
                                (b'{"jsonrpc":"2.0","id":1}', "no method", -32600), (b'{"jsonrpc":"2.0","method":5,"id":1}', "numeric method", -32600)):
            r, p = post(srv, None, headers={"Content-Type": "application/json", "Accept": "application/json", "MCP-Protocol-Version": "2026-07-28"}, raw_body=raw)
            e = jbody(p)["error"] if p else None
            check(r.status == 400 and e and e["code"] == code, "%s -> 400/%d, got %s %s" % (what, code, r.status, e))
        r, p = post(srv, None, headers={"Content-Type": "application/json", "Accept": "application/json"}, raw_body=b'{"jsonrpc":"2.0","id":1,"result":{}}')
        check(r.status == 202, "a response from the client is accepted and ignored, got %s" % r.status)

        data = json.dumps(body).encode()
        hdr = ("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nAccept: application/json\r\nMCP-Protocol-Version: 2026-07-28\r\n"
               "Mcp-Method: server/discover\r\n")
        chunks = b"".join(b"%x\r\n%s\r\n" % (len(data[i:i + 17]), data[i:i + 17]) for i in range(0, len(data), 17)) + b"0\r\n\r\n"
        r = raw_request(srv, (hdr + "Transfer-Encoding: chunked\r\n\r\n").encode() + chunks)
        check(r[0] == 200 and jbody(r[2])["result"]["resultType"] == "complete", "a chunked request body: %s" % (r[0],))
        r = raw_request(srv, (hdr + "Transfer-Encoding: chunked\r\n\r\n").encode() + b"zz\r\nxx\r\n")
        check(r[0] == 400, "malformed chunk -> 400, got %s" % r[0])
        r = raw_request(srv, (hdr + "Transfer-Encoding: chunked\r\n\r\n").encode() + b"%x\r\n" % 5000 + b"a" * 5000 + b"\r\n0\r\n\r\n")
        check(r[0] == 413, "oversized chunked body -> 413, got %s" % r[0])

        # Expect: 100-continue is answered before the body is sent
        s = srv.sock()
        s.sendall((hdr + "Content-Length: %d\r\nExpect: 100-continue\r\n\r\n" % len(data)).encode())
        first = s.recv(100)
        check(first.startswith(b"HTTP/1.1 100 Continue\r\n\r\n"), "100 Continue first: %r" % first[:40])
        s.sendall(data)
        r = read_response(s)
        check(r[0] == 200, "final response after the body: %s" % (r[0],))
        s.close()

        case("request smuggling attempts are refused")
        smug = {
            "CL + TE": (hdr + "Content-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n").encode(),
            "two different CLs": (hdr + "Content-Length: 4\r\nContent-Length: 5\r\n\r\nabcde").encode(),
            "obs-fold": (hdr + "Content-Length: 4\r\nX-A: b\r\n c\r\n\r\nabcd").encode(),
            "bare LF head": hdr.replace("\r\n", "\n").encode() + b"Content-Length: 4\n\nabcd",
            "TE: gzip": (hdr + "Transfer-Encoding: gzip, chunked\r\n\r\n").encode(),
            "space before colon": (hdr + "Content-Length : 4\r\n\r\nabcd").encode(),
            "absolute-form target": b"POST http://evil/mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
            "HTTP/2 preface": b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
        }
        for name, raw in smug.items():
            r = raw_request(srv, raw)
            check(r is None or r[0] in (400, 501), "%s -> 400 or 501, got %s" % (name, r[0] if r else None))
        # the second of two pipelined requests is a request of its own and cannot hide in the first one's body
        one = hdr + "Content-Length: %d\r\n\r\n" % len(data)
        s = srv.sock()
        s.sendall((one.encode() + data) * 2)
        a, b = read_response(s), read_response(s)
        check(a and b and a[0] == 200 and b[0] == 200, "two pipelined requests are both served: %s %s" % (a and a[0], b and b[0]))
        s.close()
        # a body with a request line inside is only data
        evil = b'{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"x":"\\r\\nGET /secret HTTP/1.1"}}'
        s = srv.sock()
        s.sendall(hdr.encode() + b"Content-Length: %d\r\n\r\n" % len(evil) + evil)
        r = read_response(s)
        check(r is not None and r[0] in (200, 400), "embedded request line is just text")
        check(s.recv(1, socket.MSG_DONTWAIT) if False else True, "")
        s.close()
        r, p = post(srv, body)
        check(r.status == 200, "the server is still healthy afterwards")
    finally:
        srv.stop()


def test_timeouts_limits():
    case("timeouts and connection limits")
    srv = Server("--header-timeout", "500", "--idle-timeout", "700", "--max-conns", "3")
    try:
        t0 = time.time()
        s = srv.sock()
        s.sendall(b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Le")
        r = read_response(s)
        check(r is not None and r[0] == 408 and time.time() - t0 < 3, "a stalled request head -> 408 after the header timeout: %s" % (r and r[0]))
        s.close()

        s = srv.sock()
        body = mreq(1, "server/discover")
        data = json.dumps(body).encode()
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nMCP-Protocol-Version: 2026-07-28\r\nMcp-Method: server/discover\r\n"
                   "Content-Length: %d\r\n\r\n" % (len(data) + 50)).encode() + data)
        t0 = time.time()
        r = read_response(s)
        check(r is None or r[0] in (400, 408) or time.time() - t0 < 3, "a stalled body does not hold the connection forever")
        s.close()

        t0 = time.time()
        s = srv.sock()
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nAccept: application/json\r\nMCP-Protocol-Version: 2026-07-28\r\n"
                   "Mcp-Method: server/discover\r\nContent-Length: %d\r\n\r\n" % len(data)).encode() + data)
        r = read_response(s)
        check(r[0] == 200, "request on a keep-alive connection")
        s.settimeout(5)
        closed = s.recv(10) == b""
        check(closed and time.time() - t0 < 3, "an idle keep-alive connection is closed after the idle timeout (%.1fs)" % (time.time() - t0))
        s.close()

        held = [srv.sock() for _ in range(3)]
        time.sleep(0.2)
        s = srv.sock()
        r = read_response(s)
        check(r is not None and r[0] == 503, "connection number 4 gets 503 when max_conns is 3, got %s" % (r and r[0]))
        for h in held:
            h.close()
        time.sleep(0.3)
        check(post(srv, body)[0].status == 200, "capacity is available again once connections close")
    finally:
        srv.stop()


def test_streaming():
    case("SSE responses")
    srv = Server()
    try:
        work = mreq(1, "tools/call", {"name": "work", "arguments": {"who": "S", "steps": 3, "loud": True}, "_meta": {"progressToken": "t"}},
                    **{"io.modelcontextprotocol/logLevel": "warning"})
        s = srv.sock()
        body = json.dumps(work).encode()
        hh = std_headers(work)
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(body) + "".join("%s: %s\r\n" % kv for kv in hh.items()) + "\r\n").encode() + body)
        st, hd, bd = read_response(s)
        ev = sse_events(bd)
        check(st == 200 and hd["content-type"] == "text/event-stream" and hd.get("x-accel-buffering") == "no" and "no-cache" in hd.get("cache-control", ""),
              "SSE headers: %s %s" % (st, hd))
        kinds = [e.get("method") or "response" for e in ev]
        check(kinds == ["notifications/progress"] * 3 + ["notifications/message"] * 2 + ["response"], "progress, log messages, then the result: %s" % kinds)
        check(ev[-1]["id"] == 1 and ev[-1]["result"]["resultType"] == "complete", "the result ends the stream")
        # the same connection serves the next request
        d = mreq(2, "server/discover")
        dbody = json.dumps(d).encode()
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(dbody) + "".join("%s: %s\r\n" % kv for kv in std_headers(d).items()) + "\r\n").encode() + dbody)
        r = read_response(s)
        check(r and r[0] == 200 and jbody(r[2])["id"] == 2, "the connection is reusable after a stream")
        s.close()

        # a client that cannot take a stream gets a single JSON answer without the notifications
        r, p = post(srv, work, headers=std_headers(work, accept="application/json"))
        check(r.status == 200 and r.getheader("Content-Type") == "application/json" and jbody(p)["id"] == 1, "no SSE without text/event-stream in Accept")

        # a request that never produces events stays plain JSON even when SSE is welcome
        r, p = post(srv, mreq(3, "tools/list"))
        check(r.getheader("Content-Type") == "application/json", "plain JSON when there is nothing to stream")

        # HTTP/1.0: no chunking, the stream ends with the connection
        s = srv.sock()
        s.sendall(("POST /mcp HTTP/1.0\r\nContent-Length: %d\r\n" % len(body) + "".join("%s: %s\r\n" % kv for kv in hh.items()) + "\r\n").encode() + body)
        buf = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        head, _, rest = buf.partition(b"\r\n\r\n")
        check(b"chunked" not in head.lower() and b"Connection: close" in head and len(sse_events(rest)) == 6, "HTTP/1.0 gets an unchunked stream closed at the end: %d events" % len(sse_events(rest)))
        s.close()

        case("multi round-trip request over HTTP is stateless")
        ask = mreq(10, "tools/call", {"name": "ask", "arguments": {"what": "form"}}, caps=ELICIT)
        r, p = post(srv, ask)
        res = jbody(p)["result"]
        check(r.status == 200 and res["resultType"] == "input_required" and "mcpc-1" in res["inputRequests"], "input_required: %s" % p[:100])
        ask2 = mreq(11, "tools/call", {"name": "ask", "arguments": {"what": "form"}, "inputResponses": {"mcpc-1": {"action": "accept", "content": {"colour": "red"}}}}, caps=ELICIT)
        c2 = srv.conn()  # a different connection: nothing is remembered between them
        r, p = post(srv, ask2, conn=c2)
        check(jbody(p)["result"]["content"][0]["text"] == 'ok action=0 content={"colour": "red"}', "the retry completes on another connection: %s" % p[:140])
    finally:
        srv.stop()


def test_listen():
    case("subscriptions/listen over HTTP")
    srv = Server()
    try:
        listen = mreq("sub-1", "subscriptions/listen", {"notifications": {"toolsListChanged": True, "resourceSubscriptions": ["file:///readme.txt"]}})
        s = srv.sock()
        body = json.dumps(listen).encode()
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(body) + "".join("%s: %s\r\n" % kv for kv in std_headers(listen).items()) + "\r\n").encode() + body)
        s.settimeout(5)
        buf = b""
        while b"acknowledged" not in buf:
            buf += s.recv(65536)
        head = buf.split(b"\r\n\r\n")[0].decode().lower()
        check("200 ok" in head and "text/event-stream" in head and "transfer-encoding: chunked" in head, "stream headers: %s" % head[:120])
        check(b'"io.modelcontextprotocol/subscriptionId":"sub-1"' in buf and b"toolsListChanged" in buf, "the acknowledgment comes first")

        # a change made through another connection reaches the stream
        t = mreq(2, "tools/call", {"name": "touch"})
        r, p = post(srv, t)
        check(r.status == 200 and jbody(p)["result"]["content"][0]["text"] == "touched", "the touching call itself is a plain answer")
        while buf.count(b"event: message") < 3:
            buf += s.recv(65536)
        text = buf.decode()
        check('"method":"notifications/resources/updated"' in text and '"method":"notifications/tools/list_changed"' in text, "both notifications arrive on the stream")
        check(text.count('"io.modelcontextprotocol/subscriptionId":"sub-1"') >= 3, "each carries the subscription id")

        # the client leaving cancels the stream, the server carries on
        s.close()
        time.sleep(0.3)
        r, p = post(srv, mreq(3, "tools/call", {"name": "touch"}))
        check(r.status == 200, "the server keeps serving after the stream was dropped")
        check(srv.p.poll() is None, "and has not crashed")
        r, p = post(srv, mreq(4, "tools/call", {"name": "touch"}))
        check(r.status == 200, "no write to the dead stream breaks later notifications")

        # a listen request that cannot be answered with a stream
        r, p = post(srv, listen, headers=std_headers(listen, accept="application/json"))
        check(r.status == 406, "subscriptions/listen needs text/event-stream, got %s" % r.status)

        # server shutdown closes open streams the proper way
        s = srv.sock()
        listen2 = mreq(7, "subscriptions/listen", {"notifications": {"toolsListChanged": True}})
        body = json.dumps(listen2).encode()
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(body) + "".join("%s: %s\r\n" % kv for kv in std_headers(listen2).items()) + "\r\n").encode() + body)
        buf = b""
        while b"acknowledged" not in buf:
            buf += s.recv(65536)
        t0 = time.time()
        srv.p.terminate()
        data = b""
        s.settimeout(8)
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                data += d
        except socket.timeout:
            pass
        closed_in = time.time() - t0
        check(b'"id":7' in data and b'"resultType":"complete"' in data and data.rstrip().endswith(b"0"), "graceful completion then a clean end of the chunked stream: %r" % data[-160:])
        check(srv.p.wait(10) == 0 and closed_in < 5, "the server exits cleanly and promptly (%.1fs)" % closed_in)
        s.close()
    finally:
        srv.stop()


def test_cancellation():
    case("a client that disconnects cancels the request")
    srv = Server()
    try:
        w = mreq(1, "tools/call", {"name": "wait", "arguments": {"ms": 8000}})
        s = srv.sock()
        body = json.dumps(w).encode()
        s.sendall(("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n" % len(body) + "".join("%s: %s\r\n" % kv for kv in std_headers(w).items()) + "\r\n").encode() + body)
        time.sleep(0.3)
        t0 = time.time()
        s.close()
        for _ in range(40):
            if any("cancelled" in l for l in srv.err):
                break
            time.sleep(0.1)
        check(any("cancelled" in l for l in srv.err), "the tool noticed the disconnect: %s" % srv.err)
        r, p = post(srv, mreq(2, "server/discover"))
        check(r.status == 200 and time.time() - t0 < 4, "and the server is free again within moments (%.1fs)" % (time.time() - t0))
        r, p = post(srv, mreq(3, "tools/call", {"name": "wait", "arguments": {"ms": 100}}))
        check(jbody(p)["result"]["content"][0]["text"] == "waited", "an undisturbed call runs to the end")
    finally:
        srv.stop()


def test_robustness():
    case("garbage, abrupt closes and concurrency")
    srv = Server()
    try:
        import random
        rnd = random.Random(7)
        for i in range(60):
            s = srv.sock()
            n = rnd.choice([0, 1, 5, 40, 300, 3000])
            s.sendall(bytes(rnd.randrange(256) for _ in range(n)))
            if rnd.random() < 0.5:
                try:
                    s.settimeout(0.3)
                    s.recv(100)
                except OSError:
                    pass
            s.close()
        for i in range(30):
            s = srv.sock()
            s.sendall(b"POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nab")
            s.close()
        for i in range(30):
            srv.sock().close()
        body = mreq(1, "server/discover")
        check(post(srv, body)[0].status == 200 and srv.p.poll() is None, "still serving after garbage and half requests")

        results = []

        def worker(k):
            try:
                c = srv.conn()
                for j in range(15):
                    r, p = post(srv, mreq(k * 100 + j, "tools/call", {"name": "calc", "arguments": {"a": k, "b": j, "op": "+"}}), conn=c)
                    results.append(jbody(p)["result"]["content"][0]["text"] == str(k + j))
                c.close()
            except Exception as e:  # noqa
                results.append(repr(e))
        ths = [threading.Thread(target=worker, args=(k,)) for k in range(8)]
        [t.start() for t in ths]
        [t.join() for t in ths]
        check(len(results) == 120 and all(r is True for r in results), "8 clients x 15 calls with correct answers: %s" % [r for r in results if r is not True][:3])
        check(srv.p.poll() is None, "server alive after the load")
    finally:
        srv.stop()


def test_fuzz_http():
    case("fuzz: mutated bodies never hurt the HTTP server")
    srv = Server()
    try:
        import random
        rnd = random.Random(int(__import__("os").environ.get("MCPC_FUZZ_SEED", "99")))
        seeds = [mreq(1, "tools/call", {"name": "typed", "arguments": {"flag": True, "ratio": 1.5, "tag": "x", "data": {"a": [1, 2]}}}),
                 mreq(2, "tools/list", {"cursor": "1"}), mreq(3, "resources/read", {"uri": "item://book/1"}),
                 mreq(4, "tools/call", {"name": "regional", "arguments": {"region": "x", "n": 3, "opts": {"zone": "z"}}}),
                 {"jsonrpc": "2.0", "id": 5, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "f", "version": "1"}}},
                 {"jsonrpc": "2.0", "id": 6, "method": "tools/list"}]
        def mutate(raw):
            b = bytearray(raw)
            for _ in range(rnd.randrange(1, 4)):
                k = rnd.randrange(6)
                if k == 0 and b:
                    b[rnd.randrange(len(b))] = rnd.randrange(256)
                elif k == 1 and b:
                    del b[rnd.randrange(len(b)):]
                elif k == 2 and b:
                    i = rnd.randrange(len(b))
                    b[i:i] = rnd.choice([b'"', b'\\', b'\\u12', b'{', b'}', b'[', b'null', b'\x00', b'\xff', b'\\ud800'])
                elif k == 3 and b:
                    i = rnd.randrange(len(b))
                    b[i:i] = b"[" * rnd.choice([10, 1000])
                elif k == 4 and b:
                    i = rnd.randrange(len(b))
                    b[i:i] = b"A" * rnd.choice([300, 9000])
                else:
                    b += bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 10)))
            return bytes(b)
        statuses = {}
        for i in range(int(__import__("os").environ.get("MCPC_FUZZ_ROUNDS", "250"))):
            seed = rnd.choice(seeds)
            body = mutate(json.dumps(seed).encode())
            hdrs = std_headers(seed)
            if rnd.random() < 0.3:
                hdrs = {k: v for k, v in hdrs.items() if rnd.random() > 0.3}
                hdrs["Content-Type"] = "application/json"
            if rnd.random() < 0.2:
                hdrs["Mcp-Param-Region"] = rnd.choice(["x", "y", "=?base64?Zm9v?=", "=?base64?!!?="])
            c = srv.conn()
            try:
                r, p = post(srv, None, headers=hdrs, raw_body=body, conn=c)
                statuses[r.status] = statuses.get(r.status, 0) + 1
                if p and r.getheader("Content-Type") == "application/json":
                    json.loads(p)
            except (http.client.HTTPException, OSError) as e:
                failures.append("fuzz request %d broke the connection: %r" % (i, e))
                break
            finally:
                c.close()
        check(srv.p.poll() is None, "server alive after the fuzz run")
        r, p = post(srv, mreq(90, "tools/call", {"name": "calc", "arguments": {"a": 20, "b": 22, "op": "+"}}))
        check(r.status == 200 and jbody(p)["result"]["content"][0]["text"] == "42", "and still correct: %s %s" % (r.status, statuses))
        check(len(statuses) > 1, "the fuzz produced a variety of outcomes: %s" % statuses)
    finally:
        srv.stop()


def main():
    only = __import__("os").environ.get("MCPC_ONLY")
    for t in [x for x in (test_basics, test_required_headers, test_version_and_errors, test_legacy, test_sessions, test_ask_over_http, test_origin_host, test_request_errors,
              test_timeouts_limits, test_streaming, test_listen, test_cancellation, test_robustness, test_fuzz_http) if not only or x.__name__ == only]:
        try:
            t()
        except Exception as e:  # noqa
            import traceback
            traceback.print_exc()
            failures.append("%s raised %r" % (t.__name__, e))
    if failures:
        print("FAILED:")
        for f in failures:
            print("  - " + f)
        sys.exit(1)
    print("httptst: all passed")


main()
