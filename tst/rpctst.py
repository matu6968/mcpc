#!/usr/bin/env python3
# Black-box tests: drive itst-svcalc over stdin and assert on the JSON it
# prints. Usage: rpctst.py <path-to-itst-svcalc> [valgrind]

import json
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


def session(msgs, extra=()):
    rc, r = run([INIT, INITED, *msgs], extra)
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

    case("tools/list larger than 1 KiB is not truncated")
    n = 60
    r = session([{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}], ["--extra-tools", str(n)])
    check(len(r) == 1 and "result" in r[0], "tools/list replied with a parseable result")
    names = [t["name"] for t in r[0]["result"]["tools"]] if r and "result" in r[0] else []
    check(len(names) == n + 3, "expected %d tools, got %d" % (n + 3, len(names)))
    check("extra-tool-%03d" % (n - 1) in names, "last tool present")

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

    case("request before initialize -> -32600")
    rc, r = run([{"jsonrpc": "2.0", "id": 12, "method": "tools/list", "params": {}}])
    check(r and r[0].get("error", {}).get("code") == -32600, "not initialized rejected with -32600")

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
