# mcpc guide: the current MCP protocol

This guide covers what the library does today and how to use it. `api.md` is the older reference
for the basic tool/prompt/resource API, which still works unchanged.

- [Building programs that use mcpc](#building-programs-that-use-mcpc)
- [Protocol revisions](#protocol-revisions)
- [Servers and transports](#servers-and-transports)
- [Tools](#tools)
- [The request context](#the-request-context)
- [Asking the client: elicitation, sampling, roots](#asking-the-client)
- [Resources](#resources)
- [Prompts](#prompts)
- [Notifications and subscriptions](#notifications-and-subscriptions)
- [Server information, pagination, caching](#server-information-pagination-caching)
- [Streamable HTTP](#streamable-http)
- [Testing](#testing)

## Building programs that use mcpc

```sh
cc -std=c23 prog.c -lmcpc -pthread                  # a compiler with C23 support: nothing else needed
cc -std=c11 -DMCPC_C23PTCH_KW1 -DMCPC_C23PTCH_UCHAR1 -DMCPC_C23GIVUP_FIXENUM prog.c -lmcpc -pthread
```

The public headers use a few C23 features (`nullptr`, `bool`, `char8_t`); the `MCPC_C23*` macros switch
on the shims in `mcpc/_c23_*.h` for compilers that lack them. Add `-Dis_win` on Windows and link `ws2_32`.
Include `<mcpc/mcpc.h>`.

## Protocol revisions

One server speaks every revision from `2024-11-05` to `2026-07-28`, chosen per client:

| Revision                         | How a client picks it                                                    |
| -------------------------------- | ------------------------------------------------------------------------ |
| `2024-11-05` ... `2025-11-25`    | `initialize` names it; over HTTP it is then named in `MCP-Protocol-Version` |
| `2026-07-28`                     | every request carries it in `_meta` (no handshake, no session)           |

You write your tools, resources and prompts once. Fields a revision does not know about (titles,
icons, annotations, structured output, ...) are left out of that revision's replies, and content a
revision cannot represent is turned into text. `mcpc_server_set_protocol_mask` restricts the set.

## Servers and transports

```c
mcpc_server_t *sv = mcpc_server_new_iostrm (stdin, stdout);   /* stdio, one message per line   */
mcpc_server_t *sv = mcpc_server_new_http ();                   /* Streamable HTTP, see below    */

mcpc_server_set_name (sv, ...);   mcpc_server_set_version (sv, u8c1 ("1.0.0"));
mcpc_server_capa_enable_tool (sv);        /* and _prmt, _rsc, _complt, _log, *_listchg, _rsc_subscr */
mcpc_server_add_tool (sv, tool);          /* also add_prmpt, add_rsc, add_rsctpl                   */
mcpc_server_start (sv);                   /* blocks */
mcpc_server_close (sv);
```

Requests are handled one at a time per server (the lock is released only while a callback waits
for the client, see below). A slow callback therefore delays the others.

## Tools

```c
static void
add (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  int32_t a, b;
  mcpc_tool_get_tpropval_i32 (tool, u8c1 ("a"), &a);
  mcpc_tool_get_tpropval_i32 (tool, u8c1 ("b"), &b);
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%d"), a + b);
}

mcpc_tool_t *t = mcpc_tool_new2 (u8c1 ("add"), u8c1 ("adds two integers"));
mcpc_tool_add_toolprop (t, mcpc_toolprop_new2 (u8c1 ("a"), u8c1 ("first"), MCPC_I32));
mcpc_tool_add_toolprop (t, mcpc_toolprop_new2 (u8c1 ("b"), u8c1 ("second"), MCPC_I32));
mcpc_tool_set_call_cb (t, &add);
```

**Property types**: the integer types, `MCPC_F64`, `MCPC_BOOL`, `MCPC_U8STR` and `MCPC_JSON` (any JSON
value, read back as text). Properties are required unless `mcpc_toolprop_set_required (p, false)`;
`mcpc_tool_has_tpropval` tells whether an optional one was given. A missing or mistyped argument
produces a tool *error result* (`isError`) the model can read, not a protocol error.

**Metadata**: `mcpc_tool_set_title`, `_set_icons_json`, `_set_meta_json`, `_set_hints` (read-only /
destructive / idempotent / open-world), `_set_input_schema_json` (replace the generated schema, any
JSON Schema 2020-12), `_set_output_schema_json`.

**Results** (`mcpc/ucbr.h`): text, `add_image`, `add_audio`, `add_resource_link`, `add_embedded_text`,
`add_embedded_blob`, `set_structured_json` (matches the output schema; a text copy is added for older
clients) and `mcpc_ucbr_set_meta_json`.

## The request context

`mcpc_tool_set_call_cb2` registers a callback that also gets an `mcpc_ctx_t *` (`mcpc/ctx.h`):

```c
static void
work (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  double n = 3;
  mcpc_ctx_arg_f64 (ctx, u8c1 ("steps"), &n);            /* raw arguments, any JSON      */
  for (int i = 1; i <= (int) n; i++)
    {
      if (mcpc_ctx_cancelled (ctx))                       /* client gave up               */
        return;
      mcpc_ctx_progress (ctx, i, n, u8c1 ("working"));    /* only if the client asked      */
      mcpc_ctx_log (ctx, MCPC_LOG_INFO, u8c1 ("work"), u8c1 ("step %d"), i);
    }
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("done"));
}
```

Cancellation: over HTTP a client that stops waiting closes its connection, which `mcpc_ctx_cancelled`
sees at once. On stdio the flag changes only while the callback waits for the client (requests are
read between callbacks, not during one).

Prefer the `mcpc_ctx_arg_*` accessors over property values in callbacks that wait for the client:
the property values live on the shared tool object.

## Asking the client

`mcpc_ctx_elicit_form`, `mcpc_ctx_elicit_url`, `mcpc_ctx_sample`, `mcpc_ctx_list_roots`. The calls look
the same on every revision, but the mechanics differ, so write the callback to be **re-runnable**:

```c
mcpc_elicit_action_t action;
char8_t answer[256];
mcpc_errcode_t ec = mcpc_ctx_elicit_form (ctx, u8c1 ("Which environment?"),
    u8c1 ("{\"type\":\"object\",\"properties\":{\"env\":{\"type\":\"string\"}}}"),
    &action, answer, sizeof (answer), nullptr);
if (ec == MCPC_EC_NEEDS_INPUT)
  return;                                   /* 2026-07-28: the client will retry with the answer */
if (ec != MCPC_EC_0 || action != MCPC_ELICIT_ACCEPT)
  { /* no capability, declined, cancelled, ... */ }
```

* **2025-11-25 and earlier**: the question is sent and the call waits for the answer. Over stdio
  other messages that arrive meanwhile are served after the callback; over HTTP the server needs a
  session (`Mcp-Session-Id`, issued by `initialize`) and a client that accepts `text/event-stream`;
  the question travels on the request's stream and the answer returns as a separate POST.
* **2026-07-28**: servers cannot send requests. The first call records the question and returns
  `MCPC_EC_NEEDS_INPUT`; the server answers the request with an `InputRequiredResult`, the client
  retries with the answers, and the callback runs again from the start. Questions are matched to
  answers by call order. Ask independent questions before returning, they are answered in one round
  trip.

Server state across the retry: `mcpc_ctx_state_set` / `mcpc_ctx_state_get`. The state is sealed with
HMAC-SHA256, bound to the method and target and given an expiry, so clients can neither read nor alter
it. It needs `mcpc_server_set_state_secret` (at least 16 random bytes) and is otherwise refused.

`prompts/get` and `resources/read` can ask too: register `mcpc_prmpt_set_callcb2` /
`mcpc_rsc_set_read_cb2`.

## Resources

```c
static void
readme (const mcpc_rsc_t *rsc, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr)
{
  mcpc_ucbr_rscread_add_text (ucbr, nullptr, nullptr, u8c1 ("hello"));   /* or _add_blob */
}

mcpc_rsc_t *r = mcpc_rsc_new2 (u8c1 ("file:///readme"), u8c1 ("readme"));
mcpc_rsc_set_read_cb (r, &readme);
mcpc_rsc_set_description (r, ...); mcpc_rsc_set_mime_str (r, u8c1 ("text/markdown")); mcpc_rsc_set_size (r, 5);
```

Templates (`{name}` matches a path segment, `{+name}` crosses `/`):

```c
mcpc_rsctpl_t *tp = mcpc_rsctpl_new (u8c1 ("item://{kind}/{id}"), u8c1 ("items"));
mcpc_rsctpl_set_read_cb (tp, &read_item);          /* mcpc_rsctpl_get_var (tp, uri, len, "id", buf, cap, &n) */
```

## Prompts

Messages may carry text, images, audio, embedded resources and resource links:
`mcpc_ucbr_prmptget_add_user_*` / `_add_assist_*`. Arguments are strings, required unless
`mcpc_prmptarg_set_required (arg, false)`; `mcpc_prmptarg_is_provided` tells whether an optional one
was given.

## Notifications and subscriptions

```c
mcpc_server_notify_tools_list_changed (sv);        /* needs mcpc_server_capa_enable_tool_listchg */
mcpc_server_notify_rscs_list_changed (sv);  mcpc_server_notify_prmpts_list_changed (sv);
mcpc_server_notify_rsc_updated (sv, u8c1 ("file:///readme"));   /* needs ..._capa_enable_rsc_subscr */
```

They are safe to call from any thread, including from inside a callback. Clients receive them via
`resources/subscribe` (initialize-based revisions, over stdio) or a `subscriptions/listen` stream
(2026-07-28, over stdio and HTTP). Over HTTP the initialize-based revisions have no stream to
receive them on, so those clients see no notifications.

## Server information, pagination, caching

`mcpc_server_set_title / _description / _website_url / _icons_json / _instructions`,
`mcpc_server_set_page_size` (0 = no pagination), `mcpc_server_set_cache (sv, ttl_ms, is_public)` for
the caching hints of 2026-07-28 (`is_public` promises that no result depends on who asks),
`mcpc_server_set_extensions_json`.

## Streamable HTTP

```c
static void ready (unsigned short port, void *ud) { printf ("listening on %u\n", port); }

mcpc_http_opts_t o = { .host = "127.0.0.1", .port = 8080, .on_listening = ready };
mcpc_errcode_t ec = mcpc_server_start_http (sv, &o);      /* until mcpc_server_stop (sv) */
```

All fields default when zero. The server accepts POST on one path (`/mcp`), answers with JSON or, when
a request reports progress/logs or opens a subscription, a Server-Sent Events stream. It validates
`Origin` and `Host` (loopback only by default) and the standard headers against the request body
(`MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name`, `Mcp-Param-*` for `x-mcp-header` tool parameters).
`mcpc_server_stop` is safe from a signal handler; open subscription streams are closed gracefully.

Not included: TLS (put a reverse proxy in front), authentication/OAuth (the library does not know who
is asking; combine `allowed_origins`/`allowed_hosts` with a proxy that does), and the standalone GET
stream of the older revisions (answered with 405).

## Testing

`make tst` runs the exit-code tests plus, when `python3` is installed:

* `tst/rpctst.py`: response contents for every method and revision, framing, errors, fuzzing;
* `tst/schematst.py`: every message checked against the official JSON schemas
  (needs `jsonschema` and a checkout of `modelcontextprotocol` next to the sources);
* `tst/httptst.py`: the HTTP transport over real sockets.

`make conformance` runs the official conformance suite against the HTTP server (needs node and network).
Sanitizer builds: `ASAN=1 make tst`, `make CC="cc -fsanitize=thread -g" tst`.
