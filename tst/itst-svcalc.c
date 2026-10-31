
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <mcpc/_c23_keywords.h>
#include <mcpc/mcpc.h>

#include <ext_string.h>
#include <hmac.h>
#include <jsonutil.h>
#include <http.h>
#include <net.h>
#include <signal.h>
#include <mjson.h>

typedef int8_t i8_t;
typedef uint8_t u8_t;
typedef int16_t i16_t;
typedef uint16_t u16_t;
typedef int32_t i32_t;
typedef uint32_t u32_t;
typedef int64_t i64_t;
typedef uint64_t u64_t;

static void
cb_calc (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  i32_t a = 0;
  i32_t b = 0;
  char8_t op[64];
  i32_t res = 0;

  if (0 != mcpc_tool_get_tpropval_i32 (tool, u8c1 ("a"), &a) || 0 != mcpc_tool_get_tpropval_i32 (tool, u8c1 ("b"), &b)
      || 0 != mcpc_tool_get_tpropval_u8str (tool, u8c1 ("op"), op, sizeof (op), nullptr))
    {
      mcpc_ucbr_toolcall_add_errmsg_printf8 (ucbr, u8c1 ("bad arguments"));
      return;
    }

  if (false)
    ;
  else if (u8streq (op, u8c1 ("+")))
    {
      res = (i32_t) ((u32_t) a + (u32_t) b);
    }
  else if (u8streq (op, u8c1 ("＋")))
    {
      res = (i32_t) ((u32_t) a + (u32_t) b);
    }
  else if (u8streq (op, u8c1 ("-")))
    {
      res = (i32_t) ((u32_t) a - (u32_t) b);
    }
  else if (u8streq (op, u8c1 ("*")) || u8streq (op, u8c1 ("times")))
    {
      res = (i32_t) ((u32_t) a * (u32_t) b);
    }
  else if (u8streq (op, u8c1 ("/")) || u8streq (op, u8c1 ("div")))
    {
      if (b == 0 || (a == INT32_MIN && b == -1))
	{
	  mcpc_ucbr_toolcall_add_errmsg_printf8 (ucbr, u8c1 ("cannot divide"));
	  return;
	}
      res = a / b;
    }
  else
    {
      mcpc_ucbr_toolcall_add_errmsg_printf8 (ucbr, u8c1 ("not matching \"any\" operator:%s"), op);
      return;
    }

  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%d"), res);
}

// answers with a text far larger than any fixed-size response buffer
static void
cb_bigtext (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  char8_t chunk[4097];
  memset (chunk, 'x', sizeof (chunk) - 1);
  chunk[sizeof (chunk) - 1] = 0;
  for (int i = 0; i < 5; i++)
    mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s"), chunk);
}

static void
cb_nothing (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("ok"));
}

static mcpc_server_t *g_server;

// exercises progress, logging and the raw argument accessors
static void
cb_work (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  char8_t who[64] = "nobody";
  double steps = 0;
  bool loud = false;
  mcpc_ctx_arg_str (ctx, u8c1 ("who"), who, sizeof (who), nullptr);
  mcpc_ctx_arg_f64 (ctx, u8c1 ("steps"), &steps);
  mcpc_ctx_arg_bool (ctx, u8c1 ("loud"), &loud);

  mcpc_ctx_log (ctx, MCPC_LOG_DEBUG, u8c1 ("work"), u8c1 ("debug detail"));
  for (int i = 1; i <= (int) steps; i++)
    mcpc_ctx_progress (ctx, i, steps, u8c1 ("stepping"));
  if (loud)
    mcpc_ctx_log (ctx, MCPC_LOG_WARNING, u8c1 ("work"), u8c1 ("done with %s"), who);
  mcpc_ctx_log (ctx, MCPC_LOG_ERROR, nullptr, u8c1 ("an error for %s"), who);

  const char8_t *args;
  size_t args_len;
  mcpc_ctx_get_args_json (ctx, &args, &args_len);
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s|%s|%.*s"), who, mcpc_ctx_get_protocol_version (ctx), (int) args_len, args);
}

static const char *
ec_name (mcpc_errcode_t ec)
{
  switch (ec)
    {
    case MCPC_EC_0: return "ok";
    case MCPC_EC_CLIENT_NOCAPA: return "nocapa";
    case MCPC_EC_CLIENT_ERR: return "clienterr";
    case MCPC_EC_CONN_CLOSED: return "closed";
    case MCPC_EC_CANCELLED: return "cancelled";
    case MCPC_EC_UNSUPPORTED: return "unsupported";
    case MCPC_EC_NEEDS_INPUT: return "needs_input";
    case MCPC_EC_CAPA_OFF: return "capa_off";
    default: return "other";
    }
}

// remembers a step across the client's retry in the sealed state
static void
cb_stateful (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  char8_t st[64] = "none", out[256] = "";
  size_t out_len = 0;
  mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
  const bool have_state = mcpc_ctx_state_get (ctx, st, sizeof (st), nullptr) == MCPC_EC_0;
  mcpc_errcode_t ec = mcpc_ctx_elicit_form (ctx, u8c1 ("Confirm?"), u8c1 ("{\"type\":\"object\",\"properties\":{}}"), &act, out, sizeof (out), &out_len);
  if (ec == MCPC_EC_NEEDS_INPUT)
    {
      mcpc_errcode_t sec = mcpc_ctx_state_set (ctx, u8c1 ("visited"));
      mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("state_set=%s"), ec_name (sec));
      return;
    }
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s state=%s have=%d act=%d"), ec_name (ec), st, (int) have_state, (int) act);
}

// two independent questions in one round trip
static void
cb_ask2 (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  char8_t a[256] = "", b[256] = "";
  mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
  mcpc_errcode_t e1 = mcpc_ctx_elicit_form (ctx, u8c1 ("First?"), u8c1 ("{\"type\":\"object\",\"properties\":{}}"), &act, a, sizeof (a), nullptr);
  mcpc_errcode_t e2 = mcpc_ctx_list_roots (ctx, b, sizeof (b), nullptr);
  if (e1 == MCPC_EC_NEEDS_INPUT || e2 == MCPC_EC_NEEDS_INPUT)
    return;
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s %s | %s %s"), ec_name (e1), a, ec_name (e2), b);
}

// echoes its arguments, its schema asks for some of them to be mirrored in headers
static void
cb_regional (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  const char8_t *args;
  size_t args_len;
  mcpc_ctx_get_args_json (ctx, &args, &args_len);
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("args=%.*s"), (int) args_len, args);
}

// waits up to "ms" milliseconds, giving up as soon as the request is cancelled
static void
cb_wait (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  double ms = 100;
  mcpc_ctx_arg_f64 (ctx, u8c1 ("ms"), &ms);
  for (double t = 0; t < ms; t += 10)
    {
      if (mcpc_ctx_cancelled (ctx))
	{
	  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("cancelled after %d ms"), (int) t);
	  fprintf (stderr, "wait: cancelled\n");
	  return;
	}
      mcpc_net_sleep_ms (10);
    }
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("waited"));
}

// asks the client for each kind of input and reports what came back
static void
cb_ask (const mcpc_tool_t *tool, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  char8_t what[16] = "form";
  mcpc_ctx_arg_str (ctx, u8c1 ("what"), what, sizeof (what), nullptr);
  char8_t out[512] = "";
  size_t out_len = 0;
  mcpc_errcode_t ec;

  if (u8streq (what, u8c1 ("form")))
    {
      mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
      ec = mcpc_ctx_elicit_form (ctx, u8c1 ("Which colour?"),
				 u8c1 ("{\"type\":\"object\",\"properties\":{\"colour\":{\"type\":\"string\"}}}"), &act, out, sizeof (out), &out_len);
      mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s action=%d content=%s"), ec_name (ec), (int) act, out);
    }
  else if (u8streq (what, u8c1 ("url")))
    {
      mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
      ec = mcpc_ctx_elicit_url (ctx, u8c1 ("Sign in"), u8c1 ("https://example.com/auth"), u8c1 ("e1"), &act);
      mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s action=%d"), ec_name (ec), (int) act);
    }
  else if (u8streq (what, u8c1 ("sample")))
    {
      ec = mcpc_ctx_sample (ctx, u8c1 ("{\"messages\":[],\"maxTokens\":5}"), out, sizeof (out), &out_len);
      mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s %s"), ec_name (ec), out);
    }
  else
    {
      ec = mcpc_ctx_list_roots (ctx, out, sizeof (out), &out_len);
      mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s %s"), ec_name (ec), out);
    }
  if (mcpc_ctx_cancelled (ctx))
    mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("(cancelled flag set)"));
}

static void
cb_touch (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  // sent from inside a request: the notifications precede the reply
  assert (0 == mcpc_server_notify_rsc_updated (g_server, u8c1 ("file:///readme.txt")));
  assert (0 == mcpc_server_notify_tools_list_changed (g_server));
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("touched"));
}

// returns one of each richer result type
static void
cb_rich (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  static const unsigned char png[] = { 0x89, 'P', 'N', 'G' };
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("here you go"));
  mcpc_ucbr_toolcall_add_image (ucbr, png, sizeof (png), u8c1 ("image/png"));
  mcpc_ucbr_toolcall_add_audio (ucbr, "abc", 3, u8c1 ("audio/wav"));
  mcpc_ucbr_toolcall_add_resource_link (ucbr, u8c1 ("file:///readme.txt"), u8c1 ("readme"), u8c1 ("the readme"),
					u8c1 ("text/plain"));
  mcpc_ucbr_toolcall_add_embedded_text (ucbr, u8c1 ("file:///inline.txt"), u8c1 ("text/plain"), u8c1 ("inline"));
  mcpc_ucbr_toolcall_add_embedded_blob (ucbr, u8c1 ("file:///inline.bin"), u8c1 ("application/octet-stream"), "\x01\x02", 2);
}

// structured result that matches the output schema
static void
cb_structured (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  assert (0 == mcpc_ucbr_toolcall_set_structured_json (ucbr, u8c1 ("{\"sum\":5}")));
  assert (0 != mcpc_ucbr_toolcall_set_structured_json (ucbr, u8c1 ("{not json")));
  assert (0 == mcpc_ucbr_set_meta_json (ucbr, u8c1 ("{\"took\":\"1ms\"}")));
}

static void
cb_rsc_readme (const mcpc_rsc_t *rsc, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr)
{
  mcpc_ucbr_rscread_add_text (ucbr, nullptr, nullptr, u8c1 ("hello %s"), "world");
}

static void
cb_rsc_blob (const mcpc_rsc_t *rsc, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr)
{
  mcpc_ucbr_rscread_add_blob (ucbr, nullptr, nullptr, "\xff\x00\xfe", 3);
}

static void
cb_rsc_item (const mcpc_rsc_t *tpl, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr)
{
  char8_t id[64], kind[64];
  if (0 != mcpc_rsctpl_get_var (tpl, uri, uri_len, u8c1 ("kind"), kind, sizeof (kind), nullptr)
      || 0 != mcpc_rsctpl_get_var (tpl, uri, uri_len, u8c1 ("id"), id, sizeof (id), nullptr))
    return;			// a value too long for the buffer: answer with no contents
  mcpc_ucbr_rscread_add_text (ucbr, nullptr, u8c1 ("text/plain"), u8c1 ("%s #%s"), kind, id);
}

static void
cb_greet (const mcpc_prmpt_t *prmpt, mcpc_ucbr_t *ucbr)
{
  char8_t who[64], tone[64] = "plain";
  if (0 != mcpc_prmptarg_get_u8str (mcpc_prmpt_getbynament_prmptarg (prmpt, u8c1 ("who")), who, sizeof (who), nullptr))
    snprintf ((char *) who, sizeof (who), "someone");
  const mcpc_prmptarg_t *t = mcpc_prmpt_getbynament_prmptarg (prmpt, u8c1 ("tone"));
  if (mcpc_prmptarg_is_provided (t))
    mcpc_prmptarg_get_u8str (t, tone, sizeof (tone), nullptr);
  mcpc_ucbr_prmptget_add_user_printf8 (ucbr, u8c1 ("greet %s in a %s way"), who, tone);
}

// a prompt that wants the user's confirmation first
static void
cb_confirm (const mcpc_prmpt_t *prmpt, mcpc_ctx_t *ctx, mcpc_ucbr_t *ucbr)
{
  char8_t out[128] = "";
  mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
  mcpc_errcode_t ec = mcpc_ctx_elicit_form (ctx, u8c1 ("Really?"), u8c1 ("{\"type\":\"object\",\"properties\":{}}"), &act, out, sizeof (out), nullptr);
  if (ec == MCPC_EC_NEEDS_INPUT)
    return;
  mcpc_ucbr_prmptget_add_user_printf8 (ucbr, u8c1 ("confirmed=%d ec=%s"), (int) act, ec_name (ec));
}

static void
cb_rsc_ask (const mcpc_rsc_t *rsc, mcpc_ctx_t *ctx, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr)
{
  char8_t out[128] = "";
  mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
  mcpc_errcode_t ec = mcpc_ctx_elicit_form (ctx, u8c1 ("Show it?"), u8c1 ("{\"type\":\"object\",\"properties\":{}}"), &act, out, sizeof (out), nullptr);
  if (ec == MCPC_EC_NEEDS_INPUT)
    return;
  mcpc_ucbr_rscread_add_text (ucbr, nullptr, nullptr, u8c1 ("shown=%d"), (int) act);
}

// a prompt whose messages carry every kind of content
static void
cb_rich_prompt (const mcpc_prmpt_t *prmpt, mcpc_ucbr_t *ucbr)
{
  static const unsigned char png[] = { 0x89, 'P', 'N', 'G' };
  mcpc_ucbr_prmptget_add_user_printf8 (ucbr, u8c1 ("look at this"));
  mcpc_ucbr_prmptget_add_user_image (ucbr, png, sizeof (png), u8c1 ("image/png"));
  mcpc_ucbr_prmptget_add_assist_audio (ucbr, "abc", 3, u8c1 ("audio/wav"));
  mcpc_ucbr_prmptget_add_user_embedded_text (ucbr, u8c1 ("file:///doc.txt"), u8c1 ("text/plain"), u8c1 ("doc text"));
  mcpc_ucbr_prmptget_add_assist_embedded_blob (ucbr, u8c1 ("file:///doc.bin"), u8c1 ("application/octet-stream"), "\x01\x02", 2);
  mcpc_ucbr_prmptget_add_user_resource_link (ucbr, u8c1 ("file:///readme.txt"), u8c1 ("readme"), nullptr, nullptr);
}

static void
add_test_prompts (mcpc_server_t *server)
{
  mcpc_prmpt_t *rp = mcpc_prmpt_new2 (u8c1 ("rich"), u8c1 ("Messages of every kind"));
  mcpc_prmpt_set_callcb (rp, &cb_rich_prompt);
  assert (0 == mcpc_server_add_prmpt (server, rp));

  mcpc_prmpt_t *cf = mcpc_prmpt_new2 (u8c1 ("confirm"), u8c1 ("Asks first"));
  mcpc_prmpt_set_callcb2 (cf, &cb_confirm);
  assert (0 == mcpc_server_add_prmpt (server, cf));
  mcpc_rsc_t *ask = mcpc_rsc_new2 (u8c1 ("file:///ask.txt"), u8c1 ("ask"));
  mcpc_rsc_set_read_cb2 (ask, &cb_rsc_ask);
  assert (0 == mcpc_server_add_rsc (server, ask));

  mcpc_prmpt_t *p = mcpc_prmpt_new2 (u8c1 ("greet"), u8c1 ("Greets someone"));
  assert (0 == mcpc_prmpt_add_prmptarg (p, mcpc_prmptarg_new2 (u8c1 ("who"), u8c1 ("the person"))));
  mcpc_prmptarg_t *tone = mcpc_prmptarg_new2 (u8c1 ("tone"), u8c1 ("how to say it"));
  mcpc_prmptarg_set_required (tone, false);
  assert (0 == mcpc_prmpt_add_prmptarg (p, tone));
  mcpc_prmpt_set_callcb (p, &cb_greet);
  mcpc_prmpt_set_title (p, u8c1 ("Greeting"));
  assert (0 == mcpc_server_add_prmpt (server, p));
}

static void
add_test_resources (mcpc_server_t *server)
{
  mcpc_rsc_t *readme = mcpc_rsc_new2 (u8c1 ("file:///readme.txt"), u8c1 ("readme"));
  mcpc_rsc_set_read_cb (readme, &cb_rsc_readme);
  mcpc_rsc_set_description (readme, u8c1 ("the readme"));
  mcpc_rsc_set_size (readme, 11);
  mcpc_rsc_set_title (readme, u8c1 ("Read me"));
  mcpc_rsc_set_annotations_json (readme, u8c1 ("{\"audience\":[\"user\"],\"priority\":0.5}"));
  assert (0 == mcpc_server_add_rsc (server, readme));

  mcpc_rsc_t *blob = mcpc_rsc_new2 (u8c1 ("file:///data.bin"), u8c1 ("data"));
  mcpc_rsc_set_mime_str (blob, u8c1 ("application/x-custom"));
  mcpc_rsc_set_read_cb (blob, &cb_rsc_blob);
  assert (0 == mcpc_server_add_rsc (server, blob));

  // no reader on purpose
  assert (0 == mcpc_server_add_rsc (server, mcpc_rsc_new2 (u8c1 ("file:///mute.txt"), u8c1 ("mute"))));

  mcpc_rsctpl_t *tpl = mcpc_rsctpl_new (u8c1 ("item://{kind}/{id}"), u8c1 ("items"));
  mcpc_rsctpl_set_description (tpl, u8c1 ("any item"));
  mcpc_rsctpl_set_read_cb (tpl, &cb_rsc_item);
  assert (0 == mcpc_server_add_rsctpl (server, tpl));
}

// echoes every argument it got, to show type conversion and optional handling
static void
cb_typed (const mcpc_tool_t *tool, mcpc_ucbr_t *ucbr)
{
  bool flag = false;
  double ratio = 0;
  char8_t tag[64] = "-";
  char8_t data[256] = "-";
  mcpc_tool_get_tpropval_bool (tool, u8c1 ("flag"), &flag);
  mcpc_tool_get_tpropval_f64 (tool, u8c1 ("ratio"), &ratio);
  if (mcpc_tool_has_tpropval (tool, u8c1 ("tag")))
    mcpc_tool_get_tpropval_u8str (tool, u8c1 ("tag"), tag, sizeof (tag), nullptr);
  if (mcpc_tool_has_tpropval (tool, u8c1 ("data")))
    mcpc_tool_get_tpropval_json (tool, u8c1 ("data"), data, sizeof (data), nullptr);
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("flag=%d ratio=%g tag=%s data=%s"), flag, ratio, tag, data);
}

static mcpc_tool_t *
new_tool_typed ()
{
  mcpc_tool_t *t = mcpc_tool_new2 (u8c1 ("typed"), u8c1 ("takes one argument of every type"));
  mcpc_tool_add_toolprop (t, mcpc_toolprop_new2 (u8c1 ("flag"), u8c1 ("a switch"), MCPC_BOOL));
  mcpc_tool_add_toolprop (t, mcpc_toolprop_new2 (u8c1 ("ratio"), u8c1 ("a fraction"), MCPC_F64));
  mcpc_toolprop_t *tag = mcpc_toolprop_new2 (u8c1 ("tag"), u8c1 ("optional label"), MCPC_U8STR);
  mcpc_toolprop_set_required (tag, false);
  mcpc_tool_add_toolprop (t, tag);
  mcpc_toolprop_t *data = mcpc_toolprop_new2 (u8c1 ("data"), u8c1 ("optional object or array"), MCPC_JSON);
  mcpc_toolprop_set_required (data, false);
  mcpc_tool_add_toolprop (t, data);
  mcpc_tool_set_call_cb (t, &cb_typed);
  return t;
}

static void
add_test_tools (mcpc_server_t *server, int n_extra_tools)
{
  assert (0 == mcpc_server_add_tool (server, new_tool_typed ()));

  mcpc_tool_t *work = mcpc_tool_new2 (u8c1 ("work"), u8c1 ("reports progress and logs"));
  mcpc_tool_set_call_cb2 (work, &cb_work);
  assert (0 == mcpc_tool_set_input_schema_json (work, u8c1 ("{\"type\":\"object\"}")));
  assert (0 == mcpc_server_add_tool (server, work));

  mcpc_tool_t *ask = mcpc_tool_new2 (u8c1 ("ask"), u8c1 ("asks the client for input"));
  mcpc_tool_set_call_cb2 (ask, &cb_ask);
  assert (0 == mcpc_tool_set_input_schema_json (ask, u8c1 ("{\"type\":\"object\"}")));
  assert (0 == mcpc_server_add_tool (server, ask));

  const char *stnames[] = { "stateful", "stateful2" };
  for (int i = 0; i < 2; i++)
    {
      mcpc_tool_t *sf = mcpc_tool_new2 ((const char8_t *) stnames[i], u8c1 ("keeps state across a retry"));
      mcpc_tool_set_call_cb2 (sf, &cb_stateful);
      assert (0 == mcpc_tool_set_input_schema_json (sf, u8c1 ("{\"type\":\"object\"}")));
      assert (0 == mcpc_server_add_tool (server, sf));
    }
  mcpc_tool_t *rg = mcpc_tool_new2 (u8c1 ("regional"), u8c1 ("some parameters travel in headers too"));
  mcpc_tool_set_call_cb2 (rg, &cb_regional);
  assert (0 == mcpc_tool_set_input_schema_json (
	    rg, u8c1 ("{\"type\":\"object\",\"properties\":{\"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"},"
		     "\"n\":{\"type\":\"integer\",\"x-mcp-header\":\"N\"},\"flag\":{\"type\":\"boolean\",\"x-mcp-header\":\"Flag\"},"
		     "\"opts\":{\"type\":\"object\",\"properties\":{\"zone\":{\"type\":\"string\",\"x-mcp-header\":\"Zone\"}}},"
		     "\"query\":{\"type\":\"string\"}}}")));
  assert (0 == mcpc_server_add_tool (server, rg));

  mcpc_tool_t *wt = mcpc_tool_new2 (u8c1 ("wait"), u8c1 ("waits, watching for cancellation"));
  mcpc_tool_set_call_cb2 (wt, &cb_wait);
  assert (0 == mcpc_tool_set_input_schema_json (wt, u8c1 ("{\"type\":\"object\"}")));
  assert (0 == mcpc_server_add_tool (server, wt));

  mcpc_tool_t *ask2 = mcpc_tool_new2 (u8c1 ("ask2"), u8c1 ("asks two things at once"));
  mcpc_tool_set_call_cb2 (ask2, &cb_ask2);
  assert (0 == mcpc_tool_set_input_schema_json (ask2, u8c1 ("{\"type\":\"object\"}")));
  assert (0 == mcpc_server_add_tool (server, ask2));

  mcpc_tool_t *touch = mcpc_tool_new2 (u8c1 ("touch"), u8c1 ("notifies subscribers"));
  mcpc_tool_set_call_cb (touch, &cb_touch);
  assert (0 == mcpc_server_add_tool (server, touch));

  mcpc_tool_t *rich = mcpc_tool_new2 (u8c1 ("rich"), u8c1 ("returns every content type"));
  mcpc_tool_set_call_cb (rich, &cb_rich);
  assert (0 == mcpc_server_add_tool (server, rich));

  mcpc_tool_t *st = mcpc_tool_new2 (u8c1 ("structured"), u8c1 ("returns structured content"));
  mcpc_tool_set_call_cb (st, &cb_structured);
  assert (0 == mcpc_tool_set_output_schema_json (
	    st, u8c1 ("{\"type\":\"object\",\"properties\":{\"sum\":{\"type\":\"integer\"}},\"required\":[\"sum\"]}")));
  assert (0 == mcpc_server_add_tool (server, st));

  mcpc_tool_t *raw = mcpc_tool_new2 (u8c1 ("rawschema"), u8c1 ("has a hand written schema"));
  assert (0 == mcpc_tool_set_input_schema_json (
	    raw, u8c1 ("{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"n\"]}")));
  assert (0 != mcpc_tool_set_input_schema_json (raw, u8c1 ("[]")));
  mcpc_tool_set_call_cb (raw, &cb_nothing);
  assert (0 == mcpc_server_add_tool (server, raw));

  mcpc_tool_t *big = mcpc_tool_new2 (u8c1 ("bigtext"), u8c1 ("returns about 20k bytes of text"));
  mcpc_tool_set_call_cb (big, &cb_bigtext);
  assert (0 == mcpc_server_add_tool (server, big));

  for (int i = 0; i < n_extra_tools; i++)
    {
      char8_t name[32];
      snprintf ((char *) name, sizeof (name), "extra-tool-%03d", i);
      mcpc_tool_t *t = mcpc_tool_new2 (name, u8c1 ("an extra tool that only exists to make tools/list large"));
      mcpc_tool_set_call_cb (t, &cb_nothing);
      assert (0 == mcpc_server_add_tool (server, t));
    }
}

mcpc_tool_t *
new_tool_calc ()
{
  const char8_t *name = u8c1 ("calc");
  const char8_t *desc = u8c1 ("simple calculator");
  mcpc_tool_t *tool = mcpc_tool_new2 (name, desc);

  {
    const char8_t *name = u8c1 ("a");
    const mcpc_anytype_t type = MCPC_I32;
    const char8_t *desc = u8c1 ("the first number(\"a\") to calculate");
    mcpc_toolprop_t *prop = mcpc_toolprop_new2 (name, desc, type);
    mcpc_tool_add_toolprop (tool, prop);
  }
  {
    const char8_t *name = u8c1 ("b");
    const mcpc_anytype_t type = MCPC_I32;
    const char8_t *desc = u8c1 ("the second number to calculate");
    mcpc_toolprop_t *prop = mcpc_toolprop_new2 (name, desc, type);
    mcpc_tool_add_toolprop (tool, prop);
  }
  {
    const char8_t *name = u8c1 ("op");
    const mcpc_anytype_t type = MCPC_U8STR;
    const char8_t *desc =	//
      u8c1 ("the calculating operator, available values " "are: \"+\",'-',\"*\",'/'");
    mcpc_toolprop_t *prop = mcpc_toolprop_new2 (name, desc, type);
    mcpc_tool_add_toolprop (tool, prop);
  }

  mcpc_tool_set_call_cb (tool, &cb_calc);

  assert (0 == mcpc_tool_set_title (tool, u8c1 ("Calculator")));
  assert (0 == mcpc_tool_set_hints (tool, 1, 0, 1, -1));
  assert (0 == mcpc_tool_set_icons_json (tool, u8c1 ("[{\"src\":\"https://example.com/calc.png\"}]")));
  assert (0 == mcpc_tool_set_meta_json (tool, u8c1 ("{\"vendor\":\"mcpc\"}")));
  assert (mcpc_tool_set_meta_json (tool, u8c1 ("[1]")) != 0);	// wrong JSON kind is refused

  return tool;
}

// ---- the fixture set the official conformance harness expects of a server ("everything server") ----

static const char *const PNG_B64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
static const char *const WAV_B64 = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA=";

static size_t
b64bytes (const char *b64, unsigned char *out)
{
  long n = mcpc_b64_decode (b64, strlen (b64), out);
  assert (n > 0);
  return (size_t) n;
}

static void
cf_simple_text (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("This is a simple text response for testing."));
}

static void
cf_image (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  unsigned char b[256];
  size_t n = b64bytes (PNG_B64, b);
  mcpc_ucbr_toolcall_add_image (u, b, n, u8c1 ("image/png"));
}

static void
cf_audio (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  unsigned char b[256];
  size_t n = b64bytes (WAV_B64, b);
  mcpc_ucbr_toolcall_add_audio (u, b, n, u8c1 ("audio/wav"));
}

static void
cf_embedded (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  mcpc_ucbr_toolcall_add_embedded_text (u, u8c1 ("test://embedded-resource"), u8c1 ("text/plain"), u8c1 ("This is an embedded resource content."));
}

static void
cf_mixed (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  unsigned char b[256];
  size_t n = b64bytes (PNG_B64, b);
  mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("Multiple content types test:"));
  mcpc_ucbr_toolcall_add_image (u, b, n, u8c1 ("image/png"));
  mcpc_ucbr_toolcall_add_embedded_text (u, u8c1 ("test://mixed-content-resource"), u8c1 ("application/json"), u8c1 ("{\"test\":\"data\",\"value\":123}"));
}

static void
cf_logging (const mcpc_tool_t *t, mcpc_ctx_t *c, mcpc_ucbr_t *u)
{
  mcpc_ctx_log (c, MCPC_LOG_INFO, u8c1 ("test"), u8c1 ("Tool execution started"));
  mcpc_net_sleep_ms (50);
  mcpc_ctx_log (c, MCPC_LOG_INFO, u8c1 ("test"), u8c1 ("Tool processing data"));
  mcpc_net_sleep_ms (50);
  mcpc_ctx_log (c, MCPC_LOG_INFO, u8c1 ("test"), u8c1 ("Tool execution completed"));
  mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("Logging test completed"));
}

static void
cf_error (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  mcpc_ucbr_toolcall_add_errmsg_printf8 (u, u8c1 ("This tool intentionally returns an error for testing"));
}

static void
cf_progress (const mcpc_tool_t *t, mcpc_ctx_t *c, mcpc_ucbr_t *u)
{
  mcpc_ctx_progress (c, 0, 100, nullptr);
  mcpc_net_sleep_ms (50);
  mcpc_ctx_progress (c, 50, 100, nullptr);
  mcpc_net_sleep_ms (50);
  mcpc_ctx_progress (c, 100, 100, nullptr);
  mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("Progress test completed"));
}

static void
cf_sampling (const mcpc_tool_t *t, mcpc_ctx_t *c, mcpc_ucbr_t *u)
{
  char8_t prompt[512] = "", params[1024], res[2048];
  mcpc_ctx_arg_str (c, u8c1 ("prompt"), prompt, sizeof (prompt), nullptr);
  char *esc = mjson_aprintf ("%Q", prompt);
  snprintf ((char *) params, sizeof (params), "{\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":%s}}],\"maxTokens\":100}", esc);
  free (esc);
  mcpc_errcode_t ec = mcpc_ctx_sample (c, params, res, sizeof (res), nullptr);
  if (ec == MCPC_EC_NEEDS_INPUT)
    return;
  if (ec != MCPC_EC_0)
    mcpc_ucbr_toolcall_add_errmsg_printf8 (u, u8c1 ("sampling failed: %s"), ec_name (ec));
  else
    {
      char8_t txt[512] = "";
      const char *tok;
      int tl;
      if (mjson_find ((const char *) res, (int) strlen ((const char *) res), "$.content.text", &tok, &tl) == MJSON_TOK_STRING)
	mjson_get_string (tok, tl, "$", (char *) txt, sizeof (txt));
      mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("LLM response: %s"), txt);
    }
}

// shared by the three elicitation fixtures
static void
cf_elicit (mcpc_ctx_t *c, mcpc_ucbr_t *u, const char *message, const char *schema, const char *prefix)
{
  char8_t out[1024] = "";
  mcpc_elicit_action_t act = MCPC_ELICIT_CANCEL;
  mcpc_errcode_t ec = mcpc_ctx_elicit_form (c, (const char8_t *) message, (const char8_t *) schema, &act, out, sizeof (out), nullptr);
  if (ec == MCPC_EC_NEEDS_INPUT)
    return;
  if (ec != MCPC_EC_0)
    {
      mcpc_ucbr_toolcall_add_errmsg_printf8 (u, u8c1 ("elicitation failed: %s"), ec_name (ec));
      return;
    }
  static const char *const names[] = { "accept", "decline", "cancel" };
  mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("%s action=%s, content=%s"), prefix, names[act], out);
}

static void
cf_elicitation (const mcpc_tool_t *t, mcpc_ctx_t *c, mcpc_ucbr_t *u)
{
  char8_t msg[256] = "Please provide your information";
  mcpc_ctx_arg_str (c, u8c1 ("message"), msg, sizeof (msg), nullptr);
  cf_elicit (c, u, (const char *) msg,
	     "{\"type\":\"object\",\"properties\":{\"username\":{\"type\":\"string\",\"description\":\"User's response\"},"
	     "\"email\":{\"type\":\"string\",\"description\":\"User's email address\"}},\"required\":[\"username\",\"email\"]}", "User response:");
}

static void
cf_elicit_defaults (const mcpc_tool_t *t, mcpc_ctx_t *c, mcpc_ucbr_t *u)
{
  cf_elicit (c, u, "Please confirm", "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"default\":\"John Doe\"},"
	     "\"age\":{\"type\":\"integer\",\"default\":30},\"score\":{\"type\":\"number\",\"default\":95.5},"
	     "\"status\":{\"type\":\"string\",\"enum\":[\"active\",\"inactive\",\"pending\"],\"default\":\"active\"},"
	     "\"verified\":{\"type\":\"boolean\",\"default\":true}}}", "Elicitation completed:");
}

static void
cf_elicit_enums (const mcpc_tool_t *t, mcpc_ctx_t *c, mcpc_ucbr_t *u)
{
  cf_elicit (c, u, "Pick options",
	     "{\"type\":\"object\",\"properties\":{\"untitledSingle\":{\"type\":\"string\",\"enum\":[\"option1\",\"option2\",\"option3\"]},"
	     "\"titledSingle\":{\"type\":\"string\",\"oneOf\":[{\"const\":\"value1\",\"title\":\"First Option\"},{\"const\":\"value2\",\"title\":\"Second Option\"}]},"
	     "\"legacyEnum\":{\"type\":\"string\",\"enum\":[\"opt1\",\"opt2\",\"opt3\"],\"enumNames\":[\"Option One\",\"Option Two\",\"Option Three\"]},"
	     "\"untitledMulti\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"option1\",\"option2\",\"option3\"]}},"
	     "\"titledMulti\":{\"type\":\"array\",\"items\":{\"anyOf\":[{\"const\":\"value1\",\"title\":\"First Choice\"},{\"const\":\"value2\",\"title\":\"Second Choice\"}]}}}}",
	     "Elicitation completed:");
}

static void
cf_noop (const mcpc_tool_t *t, mcpc_ucbr_t *u)
{
  mcpc_ucbr_toolcall_add_text_printf8 (u, u8c1 ("ok"));
}

static void
cf_rsc_text (const mcpc_rsc_t *r, const char8_t *uri, size_t ul, mcpc_ucbr_t *u)
{
  mcpc_ucbr_rscread_add_text (u, nullptr, u8c1 ("text/plain"), u8c1 ("This is the content of the static text resource."));
}

static void
cf_rsc_binary (const mcpc_rsc_t *r, const char8_t *uri, size_t ul, mcpc_ucbr_t *u)
{
  unsigned char b[256];
  size_t n = b64bytes (PNG_B64, b);
  mcpc_ucbr_rscread_add_blob (u, nullptr, u8c1 ("image/png"), b, n);
}

static void
cf_rsc_watched (const mcpc_rsc_t *r, const char8_t *uri, size_t ul, mcpc_ucbr_t *u)
{
  mcpc_ucbr_rscread_add_text (u, nullptr, u8c1 ("text/plain"), u8c1 ("Watched resource content"));
}

static void
cf_rsc_template (const mcpc_rsc_t *tpl, const char8_t *uri, size_t ul, mcpc_ucbr_t *u)
{
  char8_t id[64] = "";
  mcpc_rsctpl_get_var (tpl, uri, ul, u8c1 ("id"), id, sizeof (id), nullptr);
  mcpc_ucbr_rscread_add_text (u, nullptr, u8c1 ("application/json"), u8c1 ("{\"id\":\"%s\",\"templateTest\":true,\"data\":\"Data for ID: %s\"}"), id, id);
}

static void
cf_prompt_simple (const mcpc_prmpt_t *p, mcpc_ucbr_t *u)
{
  mcpc_ucbr_prmptget_add_user_printf8 (u, u8c1 ("This is a simple prompt for testing."));
}

static void
cf_prompt_args (const mcpc_prmpt_t *p, mcpc_ucbr_t *u)
{
  char8_t a1[128] = "", a2[128] = "";
  mcpc_prmptarg_get_u8str (mcpc_prmpt_getbynament_prmptarg (p, u8c1 ("arg1")), a1, sizeof (a1), nullptr);
  mcpc_prmptarg_get_u8str (mcpc_prmpt_getbynament_prmptarg (p, u8c1 ("arg2")), a2, sizeof (a2), nullptr);
  mcpc_ucbr_prmptget_add_user_printf8 (u, u8c1 ("Prompt with arguments: arg1='%s', arg2='%s'"), a1, a2);
}

static void
cf_prompt_embedded (const mcpc_prmpt_t *p, mcpc_ucbr_t *u)
{
  char8_t uri[256] = "";
  mcpc_prmptarg_get_u8str (mcpc_prmpt_getbynament_prmptarg (p, u8c1 ("resourceUri")), uri, sizeof (uri), nullptr);
  mcpc_ucbr_prmptget_add_user_embedded_text (u, uri, u8c1 ("text/plain"), u8c1 ("Embedded resource content for testing."));
  mcpc_ucbr_prmptget_add_user_printf8 (u, u8c1 ("Please process the embedded resource above."));
}

static void
cf_prompt_image (const mcpc_prmpt_t *p, mcpc_ucbr_t *u)
{
  unsigned char b[256];
  size_t n = b64bytes (PNG_B64, b);
  mcpc_ucbr_prmptget_add_user_image (u, b, n, u8c1 ("image/png"));
  mcpc_ucbr_prmptget_add_user_printf8 (u, u8c1 ("Please analyze the image above."));
}

static void
add_conformance_fixtures (mcpc_server_t *server)
{
#define CF_TOOL(NAME, DESC, SETCB, CB, SCHEMA)                                                                                     \
  do                                                                                                                              \
    {                                                                                                                             \
      mcpc_tool_t *t_ = mcpc_tool_new2 (u8c1 (NAME), u8c1 (DESC));                                                                  \
      SETCB (t_, CB);                                                                                                             \
      assert (0 == mcpc_tool_set_input_schema_json (t_, u8c1 (SCHEMA)));                                                            \
      assert (0 == mcpc_server_add_tool (server, t_));                                                                            \
    }                                                                                                                             \
  while (0)
  const char *none = "{\"type\":\"object\",\"properties\":{}}";
  CF_TOOL ("test_simple_text", "Returns simple text", mcpc_tool_set_call_cb, &cf_simple_text, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_image_content", "Returns an image", mcpc_tool_set_call_cb, &cf_image, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_audio_content", "Returns audio", mcpc_tool_set_call_cb, &cf_audio, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_embedded_resource", "Returns an embedded resource", mcpc_tool_set_call_cb, &cf_embedded, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_multiple_content_types", "Returns several kinds of content", mcpc_tool_set_call_cb, &cf_mixed, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_tool_with_logging", "Sends log messages", mcpc_tool_set_call_cb2, &cf_logging, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_error_handling", "Always fails", mcpc_tool_set_call_cb, &cf_error, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_tool_with_progress", "Reports progress", mcpc_tool_set_call_cb2, &cf_progress, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_sampling", "Asks the client's model", mcpc_tool_set_call_cb2, &cf_sampling,
	   "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"}},\"required\":[\"prompt\"]}");
  CF_TOOL ("test_elicitation", "Asks the user", mcpc_tool_set_call_cb2, &cf_elicitation,
	   "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},\"required\":[\"message\"]}");
  CF_TOOL ("test_elicitation_sep1034_defaults", "Elicitation with defaults", mcpc_tool_set_call_cb2, &cf_elicit_defaults, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("test_elicitation_sep1330_enums", "Elicitation with enums", mcpc_tool_set_call_cb2, &cf_elicit_enums, "{\"type\":\"object\",\"properties\":{}}");
  CF_TOOL ("json_schema_2020_12_tool", "Tool with JSON Schema 2020-12 features", mcpc_tool_set_call_cb, &cf_noop,
	   "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"type\":\"object\",\"$defs\":{\"address\":{\"type\":\"object\","
	   "\"properties\":{\"street\":{\"type\":\"string\"},\"city\":{\"type\":\"string\"}}}},\"properties\":{\"name\":{\"type\":\"string\"},"
	   "\"address\":{\"$ref\":\"#/$defs/address\"}},\"additionalProperties\":false}");
  (void) none;

  mcpc_rsc_t *r1 = mcpc_rsc_new2 (u8c1 ("test://static-text"), u8c1 ("static-text"));
  mcpc_rsc_set_description (r1, u8c1 ("A static text resource"));
  mcpc_rsc_set_mime_str (r1, u8c1 ("text/plain"));
  mcpc_rsc_set_read_cb (r1, &cf_rsc_text);
  assert (0 == mcpc_server_add_rsc (server, r1));
  mcpc_rsc_t *r2 = mcpc_rsc_new2 (u8c1 ("test://static-binary"), u8c1 ("static-binary"));
  mcpc_rsc_set_description (r2, u8c1 ("A static binary resource"));
  mcpc_rsc_set_mime_str (r2, u8c1 ("image/png"));
  mcpc_rsc_set_read_cb (r2, &cf_rsc_binary);
  assert (0 == mcpc_server_add_rsc (server, r2));
  mcpc_rsc_t *r3 = mcpc_rsc_new2 (u8c1 ("test://watched-resource"), u8c1 ("watched-resource"));
  mcpc_rsc_set_description (r3, u8c1 ("A resource to subscribe to"));
  mcpc_rsc_set_mime_str (r3, u8c1 ("text/plain"));
  mcpc_rsc_set_read_cb (r3, &cf_rsc_watched);
  assert (0 == mcpc_server_add_rsc (server, r3));
  mcpc_rsctpl_t *tp = mcpc_rsctpl_new (u8c1 ("test://template/{id}/data"), u8c1 ("template-data"));
  mcpc_rsctpl_set_description (tp, u8c1 ("Data by id"));
  mcpc_rsctpl_set_mime_str (tp, u8c1 ("application/json"));
  mcpc_rsctpl_set_read_cb (tp, &cf_rsc_template);
  assert (0 == mcpc_server_add_rsctpl (server, tp));

  mcpc_prmpt_t *p1 = mcpc_prmpt_new2 (u8c1 ("test_simple_prompt"), u8c1 ("A simple prompt"));
  mcpc_prmpt_set_callcb (p1, &cf_prompt_simple);
  assert (0 == mcpc_server_add_prmpt (server, p1));
  mcpc_prmpt_t *p2 = mcpc_prmpt_new2 (u8c1 ("test_prompt_with_arguments"), u8c1 ("A prompt with two arguments"));
  mcpc_prmptarg_t *a1 = mcpc_prmptarg_new2 (u8c1 ("arg1"), u8c1 ("First test argument"));
  assert (0 == mcpc_prmptarg_add_hint_printf8 (a1, u8c1 ("paris")));
  assert (0 == mcpc_prmptarg_add_hint_printf8 (a1, u8c1 ("park")));
  assert (0 == mcpc_prmpt_add_prmptarg (p2, a1));
  assert (0 == mcpc_prmpt_add_prmptarg (p2, mcpc_prmptarg_new2 (u8c1 ("arg2"), u8c1 ("Second test argument"))));
  mcpc_prmpt_set_callcb (p2, &cf_prompt_args);
  assert (0 == mcpc_server_add_prmpt (server, p2));
  mcpc_prmpt_t *p3 = mcpc_prmpt_new2 (u8c1 ("test_prompt_with_embedded_resource"), u8c1 ("A prompt embedding a resource"));
  assert (0 == mcpc_prmpt_add_prmptarg (p3, mcpc_prmptarg_new2 (u8c1 ("resourceUri"), u8c1 ("URI of the resource to embed"))));
  mcpc_prmpt_set_callcb (p3, &cf_prompt_embedded);
  assert (0 == mcpc_server_add_prmpt (server, p3));
  mcpc_prmpt_t *p4 = mcpc_prmpt_new2 (u8c1 ("test_prompt_with_image"), u8c1 ("A prompt with an image"));
  mcpc_prmpt_set_callcb (p4, &cf_prompt_image);
  assert (0 == mcpc_server_add_prmpt (server, p4));
}

static bool g_conformance;

static void
configure_server (mcpc_server_t *server, int n_extra_tools, size_t page_size, unsigned long ttl_ms, bool cache_public, unsigned pmask)
{
  mcpc_server_capa_enable_prmt (server);
  mcpc_server_capa_enable_tool (server);
  mcpc_server_capa_enable_rsc (server);
  mcpc_server_capa_enable_complt (server);
  mcpc_server_capa_enable_log (server);
  mcpc_server_capa_enable_tool_listchg (server);
  mcpc_server_capa_enable_rsc_subscr (server);
  g_server = server;
  assert (0 == mcpc_server_set_state_secret (server, "0123456789abcdef-test-key", 25));

  assert (0 == mcpc_server_set_version (server, u8c1 ("1.2.3")));
  assert (0 == mcpc_server_set_title (server, u8c1 ("Calc Server")));
  assert (0 == mcpc_server_set_description (server, u8c1 ("A calculator for tests")));
  assert (0 == mcpc_server_set_website_url (server, u8c1 ("https://example.com/calc")));
  assert (0 == mcpc_server_set_instructions (server, u8c1 ("Use calc for arithmetic.")));
  assert (0 == mcpc_server_set_icons_json (server, u8c1 ("[{\"src\":\"https://example.com/i.png\",\"mimeType\":\"image/png\"}]")));
  mcpc_server_set_page_size (server, page_size);
  mcpc_server_set_cache (server, ttl_ms, cache_public);
  assert (0 == mcpc_server_set_protocol_mask (server, pmask));
  assert (0 == mcpc_server_set_extensions_json (server, u8c1 ("{\"io.modelcontextprotocol/test\":{}}")));

  mcpc_tool_t *tool = new_tool_calc ();
  assert (0 == mcpc_server_add_tool (server, tool));
  add_test_tools (server, n_extra_tools);
  add_test_prompts (server);
  add_test_resources (server);
  if (g_conformance)
    add_conformance_fixtures (server);
}

int
main_iostrm (int argc, const char *argv[])
{
  u8_t in_src = 0;		// 1stdin 2file
  char in_fpath[4096];
  int n_extra_tools = 0;
  size_t page_size = 0;
  unsigned long ttl_ms = 0;
  bool cache_public = false;
  unsigned pmask = MCPC_PROTO_ALL;

  for (int i = 2; i < argc; i++)
    {
      const char8_t *curr_arg = (const char8_t *) argv[i];
      const char8_t *nex_arg = nullptr;
      if (i < argc - 1)
	nex_arg = (const char8_t *) argv[i + 1];
      if (u8streq (curr_arg, u8c1 ("--cache-public")))
	cache_public = true;
      if (nex_arg != nullptr)
	{
	  if (u8streq (curr_arg, u8c1 ("--ttl-ms")))
	    ttl_ms = (unsigned long) atol ((const char *) nex_arg);
	  if (u8streq (curr_arg, u8c1 ("--mask")))
	    pmask = (unsigned) atoi ((const char *) nex_arg);
	  if (u8streq (curr_arg, u8c1 ("--page-size")))
	    {
	      page_size = (size_t) atoi ((const char *) nex_arg);
	    }
	  if (u8streq (curr_arg, u8c1 ("--extra-tools")))
	    {
	      n_extra_tools = atoi ((const char *) nex_arg);
	    }
	  if (u8streq (curr_arg, u8c1 ("-i")))
	    {
	      if (u8streq (nex_arg, u8c1 ("-")))
		{
		  in_src = 1;
		}
	      else if (beg_with_alph ((const char8_t *) nex_arg))
		{
		  // TODO nonascii filename?
		  /* memcpy (in_fpath, nex_arg, strlen (nex_arg)); */
		  assert (strlen ((const char *) nex_arg) < sizeof (in_fpath));
		  snprintf (in_fpath, sizeof (in_fpath), "%s", (const char *) nex_arg);
		  in_src = 2;
		}
	    }
	}
    }

  assert (in_src);

  FILE *in_stream = nullptr;
  switch (in_src)
    {
    case 1:
      in_stream = stdin;
      break;
    case 2:
      in_stream = fopen (in_fpath, "r");
      if (!in_stream)
	{
	  fprintf (stdout, "file:%s, err:%s", in_fpath, strerror (errno));
	}
      break;
    }
  assert (in_stream);

  mcpc_server_t *server = mcpc_server_new_iostrm (in_stream, stdout);
  configure_server (server, n_extra_tools, page_size, ttl_ms, cache_public, pmask);

  mcpc_errcode_t ec = mcpc_server_start (server);

  mcpc_server_close (server);
  fclose (in_stream);

  return ec;
}

static void
on_listening (unsigned short port, void *ud)
{
  printf ("listening %u\n", (unsigned) port);
  fflush (stdout);
}

static void
on_signal (int sig)
{
  if (g_server != nullptr)
    mcpc_server_stop (g_server);
}

static const char *
split_list (const char *csv, const char **out, int cap)
{
  static char store[1024];
  snprintf (store, sizeof (store), "%s", csv);
  int n = 0;
  for (char *p = store; *p && n < cap - 1;)
    {
      out[n++] = p;
      char *c = strchr (p, ',');
      if (!c)
	break;
      *c = 0;
      p = c + 1;
    }
  out[n] = nullptr;
  return store;
}

int
main_http (int argc, const char *argv[])
{
  int n_extra_tools = 0;
  size_t page_size = 0;
  unsigned long ttl_ms = 0;
  bool cache_public = false;
  unsigned pmask = MCPC_PROTO_ALL;
  mcpc_http_opts_t opts;
  memset (&opts, 0, sizeof (opts));
  opts.on_listening = on_listening;
  const char *origins[16], *hosts[16];
  static char ostore[1024], hstore[1024];
  (void) ostore;
  (void) hstore;

  for (int i = 2; i < argc; i++)
    {
      const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : nullptr;
      if (strcmp (k, "--cache-public") == 0)
	cache_public = true;
      if (strcmp (k, "--conformance") == 0)
	g_conformance = true;
      if (v == nullptr)
	continue;
      if (strcmp (k, "--port") == 0)
	opts.port = (unsigned short) atoi (v);
      else if (strcmp (k, "--host") == 0)
	opts.host = v;
      else if (strcmp (k, "--path") == 0)
	opts.path = v;
      else if (strcmp (k, "--max-body") == 0)
	opts.max_body = (size_t) atol (v);
      else if (strcmp (k, "--max-conns") == 0)
	opts.max_conns = (size_t) atol (v);
      else if (strcmp (k, "--header-timeout") == 0)
	opts.header_timeout_ms = (unsigned) atoi (v);
      else if (strcmp (k, "--idle-timeout") == 0)
	opts.idle_timeout_ms = (unsigned) atoi (v);
      else if (strcmp (k, "--origins") == 0)
	{
	  static char s1[1024];
	  snprintf (s1, sizeof (s1), "%s", v);
	  int n = 0;
	  for (char *p = s1; *p && n < 15;)
	    {
	      origins[n++] = p;
	      char *c = strchr (p, ',');
	      if (!c)
		break;
	      *c = 0;
	      p = c + 1;
	    }
	  origins[n] = nullptr;
	  opts.allowed_origins = origins;
	}
      else if (strcmp (k, "--hosts") == 0)
	{
	  static char s2[1024];
	  snprintf (s2, sizeof (s2), "%s", v);
	  int n = 0;
	  for (char *p = s2; *p && n < 15;)
	    {
	      hosts[n++] = p;
	      char *c = strchr (p, ',');
	      if (!c)
		break;
	      *c = 0;
	      p = c + 1;
	    }
	  hosts[n] = nullptr;
	  opts.allowed_hosts = hosts;
	}
      else if (strcmp (k, "--ttl-ms") == 0)
	ttl_ms = (unsigned long) atol (v);
      else if (strcmp (k, "--mask") == 0)
	pmask = (unsigned) atoi (v);
      else if (strcmp (k, "--page-size") == 0)
	page_size = (size_t) atoi (v);
      else if (strcmp (k, "--extra-tools") == 0)
	n_extra_tools = atoi (v);
    }

  mcpc_server_t *server = mcpc_server_new_http ();
  configure_server (server, n_extra_tools, page_size, ttl_ms, cache_public, pmask);
  signal (SIGTERM, on_signal);
  signal (SIGINT, on_signal);

  mcpc_errcode_t ec = mcpc_server_start_http (server, &opts);
  mcpc_server_close (server);
  return ec;
}

int
main_tcp (int argc, const char *argv[])
{
  mcpc_server_t *server = mcpc_server_new_tcp ();
  mcpc_server_capa_enable_prmt (server);
  mcpc_server_capa_enable_tool (server);
  mcpc_server_capa_enable_rsc (server);
  mcpc_server_capa_enable_complt (server);

  mcpc_tool_t *tool = new_tool_calc ();
  assert (0 == mcpc_server_add_tool (server, tool));

  mcpc_errcode_t ec = mcpc_server_start (server);

  mcpc_server_close (server);
  return ec;
}

static int
hexval (char c)
{
  return c >= 'a' ? c - 'a' + 10 : c - '0';
}

static size_t
unhex (const char *h, unsigned char *out)
{
  size_t n = strlen (h) / 2;
  for (size_t i = 0; i < n; i++)
    out[i] = (unsigned char) (hexval (h[2 * i]) * 16 + hexval (h[2 * i + 1]));
  return n;
}

static void
puthex (const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    printf ("%02x", p[i]);
  printf ("\n");
}

// helpers for black-box tests of the internal primitives
static int
main_crypto (int argc, const char *argv[])
{
  static unsigned char a[1 << 20], b[1 << 20];
  uint8_t d[MCPC_SHA256_LEN];
  if (argc >= 4 && strcmp (argv[2], "sha256") == 0)
    {
      mcpc_sha256 (a, unhex (argv[3], a), d);
      puthex (d, sizeof (d));
    }
  else if (argc >= 5 && strcmp (argv[2], "hmac") == 0)
    {
      size_t kl = unhex (argv[3], a), ml = unhex (argv[4], b);
      mcpc_hmac_sha256 (a, kl, b, ml, d);
      puthex (d, sizeof (d));
    }
  else if (argc >= 4 && strcmp (argv[2], "b64enc") == 0)
    {
      static char out[1 << 21];
      mcpc_b64_encode (a, unhex (argv[3], a), out);
      printf ("%s\n", out);
    }
  else if (argc >= 4 && strcmp (argv[2], "b64dec") == 0)
    {
      long n = mcpc_b64_decode (argv[3], strlen (argv[3]), a);
      if (n < 0)
	printf ("invalid\n");
      else
	puthex (a, (size_t) n);
    }
  else if (argc >= 5 && strcmp (argv[2], "member") == 0)
    {
      const char *v = nullptr;
      size_t vl = 0;
      int t = mcpc_json_member (argv[3], strlen (argv[3]), argv[4], &v, &vl);
      if (t == MJSON_TOK_INVALID)
	printf ("none\n");
      else
	printf ("%d %.*s\n", t, (int) vl, v);
    }
  else if (argc >= 4 && strcmp (argv[2], "httphead") == 0)
    {
      // parse a request head given as hex and describe what came out
      static struct mcpc_http_req rq;
      size_t n = unhex (argv[3], a);
      const char *err = "";
      int rc = mcpc_http_parse_head ((const char *) a, n, &rq, &err);
      printf ("rc=%d err=%s", rc, err);
      if (rc == MCPC_HTTP_OK)
	{
	  printf (" method=%s target=%.*s minor=%d head=%zu cl=%d:%zu chunked=%d keep=%d expect=%d", rq.method, (int) rq.target_len,
		  rq.target, rq.minor, rq.head_len, rq.has_cl, rq.content_length, rq.chunked, rq.keep_alive, rq.expect_continue);
	  for (size_t i = 0; i < rq.nhdrs; i++)
	    printf (" [%.*s=%.*s]", (int) rq.hdrs[i].name_len, rq.hdrs[i].name, (int) rq.hdrs[i].val_len, rq.hdrs[i].val);
	}
      printf ("\n");
    }
  else if (argc >= 5 && strcmp (argv[2], "dechunk") == 0)
    {
      // decode a chunked body given as hex, in pieces of argv[4] bytes to exercise the incremental state
      struct mcpc_chunk_dec d;
      mcpc_chunk_init (&d, 1 << 16);
      size_t n = unhex (argv[3], a), piece = (size_t) atoi (argv[4]), off = 0, total = 0;
      int rc = MCPC_HTTP_INCOMPLETE;
      while (off < n && rc == MCPC_HTTP_INCOMPLETE)
	{
	  size_t take = n - off < piece ? n - off : piece, used = 0, outn = 0;
	  rc = mcpc_chunk_feed (&d, (const char *) a + off, take, &used, (char *) b + total, &outn);
	  total += outn;
	  off += used;
	  if (used == 0 && rc == MCPC_HTTP_INCOMPLETE && take > 0)
	    break;
	}
      printf ("rc=%d used=%zu body=", rc, off);
      puthex (b, total);
    }
  else if (argc >= 5 && strcmp (argv[2], "httpfuzz") == 0)
    {
      // mutate valid requests and chunked bodies, the readers must never misbehave on any of them
      static const char *corpus[] = {
	"POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Length: 12\r\nContent-Type: application/json\r\nMcp-Method: tools/call\r\n\r\n",
	"POST /mcp HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nExpect: 100-continue\r\n\r\n",
	"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
	"5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n",
      };
      unsigned long long x = strtoull (argv[3], nullptr, 10) | 1;
      long iters = atol (argv[4]);
      static unsigned char buf[4096], out[4096];
      long ok_heads = 0, ok_bodies = 0;
      for (long it = 0; it < iters; it++)
	{
#define RND() (x ^= x << 13, x ^= x >> 7, x ^= x << 17, x)
	  const char *src = corpus[RND () % 4];
	  size_t n = strlen (src);
	  memcpy (buf, src, n);
	  int muts = (int) (RND () % 6);
	  for (int m = 0; m < muts; m++)
	    {
	      switch (RND () % 5)
		{
		case 0: buf[RND () % n] = (unsigned char) RND (); break;
		case 1: buf[RND () % n] = "\r\n :,;\t0a"[RND () % 9]; break;
		case 2: if (n > 1) n -= RND () % (n < 8 ? n : 8); break;
		case 3: if (n + 8 < sizeof (buf)) { size_t at = RND () % n; memmove (buf + at + 4, buf + at, n - at); memcpy (buf + at, "\r\n\r\n", 4); n += 4; } break;
		default: if (n + 1 < sizeof (buf)) { buf[n++] = (unsigned char) RND (); } break;
		}
	      if (n == 0)
		n = 1;
	    }
	  static struct mcpc_http_req rq;
	  const char *err = nullptr;
	  size_t cut = n;
	  for (int pass = 0; pass < 2; pass++)
	    {
	      int rc = mcpc_http_parse_head ((const char *) buf, cut, &rq, &err);
	      if (rc == MCPC_HTTP_OK)
		{
		  assert (rq.head_len <= cut);
		  assert (rq.nhdrs <= MCPC_HTTP_MAX_HEADERS);
		  for (size_t i = 0; i < rq.nhdrs; i++)
		    {
		      assert ((const unsigned char *) rq.hdrs[i].name >= buf && (const unsigned char *) rq.hdrs[i].val + rq.hdrs[i].val_len <= buf + cut);
		      assert (!(rq.chunked && rq.has_cl));
		    }
		  ok_heads++;
		}
	      cut = n / 2;
	    }
	  struct mcpc_chunk_dec d;
	  mcpc_chunk_init (&d, 64);
	  size_t used = 0, outn = 0;
	  int rc = mcpc_chunk_feed (&d, (const char *) buf, n, &used, (char *) out, &outn);
	  assert (used <= n && outn <= n && outn <= 64);
	  if (rc == MCPC_HTTP_OK)
	    ok_bodies++;
	}
      printf ("fuzz done heads=%ld bodies=%ld\n", ok_heads, ok_bodies);
    }
  else if (argc >= 4 && strcmp (argv[2], "hdrvalue") == 0)
    {
      size_t ol = 0;
      char *v = mcpc_http_decode_value (argv[3], strlen (argv[3]), &ol);
      if (v == nullptr)
	printf ("invalid\n");
      else
	{
	  puthex ((const unsigned char *) v, ol);
	  free (v);
	}
    }
  else
    return 2;
  return 0;
}

int
main (int argc, const char *argv[])
{
  assert (argc >= 2);
  if (strcmp (argv[1], "crypto") == 0)
    return main_crypto (argc, argv);
  bool as_iostrm = strcmp (argv[1], "iostrm") == 0;
  bool as_tcp = strcmp (argv[1], "tcp") == 0;
  if (strcmp (argv[1], "http") == 0)
    return main_http (argc, argv);
  assert (as_iostrm || as_tcp);
  // bool as_iostrm = false;
  // bool as_tcp = true;
  if (as_tcp)
    return main_tcp (argc, argv);
  if (as_iostrm)
    return main_iostrm (argc, argv);
}
