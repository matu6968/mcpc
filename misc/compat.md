# Compatibility and limits

## What is implemented

| Area                         | Server | Notes                                                                 |
| ---------------------------- | ------ | --------------------------------------------------------------------- |
| Tools                        | yes    | typed or raw JSON Schema input, output schema, structured output, annotations, all content types |
| Resources                    | yes    | read, templates, subscribe (initialize-based) / `subscriptions/listen` (2026-07-28) |
| Prompts                      | yes    | text, image, audio, embedded resource, resource link messages         |
| Completion                   | yes    | prompt arguments and resource references                              |
| Logging, progress            | yes    | `logging/setLevel` (initialize-based) or `_meta` log level (2026-07-28) |
| Cancellation                 | yes    | disconnect (HTTP), `notifications/cancelled` while waiting (stdio)    |
| Pagination                   | yes    | opaque cursors, page size set by the application                      |
| Elicitation, sampling, roots | yes    | blocking on initialize-based revisions, multi round-trip on 2026-07-28 |
| Caching hints                | yes    | `ttlMs` / `cacheScope` on list and read results (2026-07-28)          |
| stdio transport              | yes    | newline delimited; objects may also follow each other directly        |
| Streamable HTTP transport    | yes    | single POST endpoint, JSON or SSE replies, see below                  |
| Client side                  | no     |                                                                       |
| Tasks extension              | no     |                                                                       |
| Authorization (OAuth)        | no     | the server does not authenticate callers                              |
| TLS                          | no     | use a reverse proxy                                                   |
| HTTP+SSE (2024-11-05)        | no     | deprecated by the specification                                       |

## Revisions

| Revision     | Handshake    | Sessions over HTTP        | Server requests              | Notes                                              |
| ------------ | ------------ | ------------------------- | ---------------------------- | -------------------------------------------------- |
| 2024-11-05   | `initialize` | `Mcp-Session-Id` optional | on the request's SSE stream  | no titles, annotations, structured output          |
| 2025-03-26   | `initialize` | optional                  | same                         | + tool annotations, audio, progress messages       |
| 2025-06-18   | `initialize` | optional                  | same                         | + titles, structured output, resource links, elicitation |
| 2025-11-25   | `initialize` | optional                  | same                         | + icons, URL elicitation, sampling with tools      |
| 2026-07-28   | none         | none (header ignored)     | multi round-trip instead     | `server/discover`, `subscriptions/listen`, `resultType`, cache hints |

A server speaks all of them at once. The era of a request is decided per request: `initialize` starts an
initialize-based conversation, a request whose `_meta` carries `io.modelcontextprotocol/protocolVersion` is
served statelessly, and over HTTP a request without either names its revision in `MCP-Protocol-Version`
(2025-03-26 when absent) and may name a session in `Mcp-Session-Id`.

## Behaviour worth knowing

* **Requests run one at a time** (per server, not per connection). A callback that waits for the client
  releases the lock meanwhile.
* **stdio framing**: a line that ends inside an unfinished JSON object is answered with a parse error
  (`-32700`, id `null`) and dropped, so a garbled message cannot swallow the next one. Pretty-printed
  (multi-line) JSON is therefore not accepted; the specification forbids embedded newlines anyway.
* **JSON strings**: `\uXXXX` escapes (including surrogate pairs) are decoded to UTF-8; invalid UTF-8 and
  control characters in anything the server prints are replaced or escaped, so replies are always valid
  JSON. Ids that cannot be echoed verbatim are rejected as invalid requests.
* **Limits**: messages up to 16 MiB on stdio, 4 MiB bodies over HTTP (configurable), tool and prompt
  names up to 255 bytes, 1024 resource subscriptions and 64 listen streams per connection, 256 HTTP
  sessions (least recently used are dropped), 250 simultaneous HTTP connections.
* **Legacy HTTP caveats**: no standalone GET stream, so `list_changed` and `resources/updated`
  notifications cannot be delivered to initialize-based HTTP clients; `logging/setLevel` has no effect
  (log level is the default, info). Asking the client needs a session.

## Security notes

* The HTTP server binds to `127.0.0.1` and accepts only loopback `Host` and `Origin` values unless told
  otherwise (DNS rebinding protection). Binding to another address without setting `allowed_hosts` and
  `allowed_origins` is the application's decision.
* There is no authentication. Do not expose the server to untrusted networks without a proxy that does it.
* `requestState` is attacker-controlled input: it is accepted only if it carries a valid HMAC for the
  same method and target and has not expired.
* Answers to server-initiated questions over HTTP must come from the session that was asked, and the
  question ids are random.

## Platforms

Built and tested here on Linux with gcc 12/13/15 and clang, in C23 and the C11/C17 fallbacks, under
AddressSanitizer, ThreadSanitizer and UndefinedBehaviorSanitizer. The code paths for Windows (Winsock
sockets, `CRITICAL_SECTION` locks, `rand_s`) and macOS are written against their APIs but were not built
or run in the development environment. The workflow has jobs for them; treat them as untested until those pass.
