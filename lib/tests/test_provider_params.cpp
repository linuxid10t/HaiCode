// Task 23 — provider parameter correctness for the Anthropic provider:
// capability-driven effort mapping (never a literal "off"), adaptive
// thinking config only where supported, sampling-param gating, thinking
// block fragment accumulation, and verbatim replay ahead of tool_use.
#include <haicode/model_capabilities.h>
#include <haicode/anthropic_params.h>
#include <haicode/openai_params.h>
#include <haicode/util.h>
#include <haicode/model_context_parse.h>
#include <haicode/provider_error.h>
#include <haicode/engine.h>
#include <haicode/db.h>
#include <functional>
#include <iostream>
#include <vector>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using json = nlohmann::json;
using namespace haicode;

// ---- anthropic_map_effort / anthropic_model_caps ----

static bool test_effort_off_maps_low() {
    CHECK(anthropic_map_effort("claude-opus-5", "off") == "low",
          "\"off\" must map to the lowest valid effort, never be sent verbatim");
    CHECK(anthropic_map_effort("claude-opus-5", "minimal") == "low",
          "\"minimal\" is not an Anthropic value; maps to low");
    CHECK(anthropic_map_effort("claude-sonnet-5-5", "low") == "low",
          "low passes through");
    CHECK(anthropic_map_effort("claude-opus-5", "xhigh") == "xhigh",
          "xhigh passes through on 5.x");
    CHECK(anthropic_map_effort("claude-opus-5", "") == "",
          "empty effort omits the param");
    std::cout << "[OK] effort off/minimal -> low, passthrough elsewhere\n";
    return true;
}

static bool test_effort_unsupported_models() {
    // 4.5/3-7 (budgeted) and 3-5 (none) have no effort support at all.
    CHECK(anthropic_map_effort("claude-opus-4-5", "high") == "",
          "4.5 must not receive output_config");
    CHECK(anthropic_map_effort("claude-3-7-sonnet", "low") == "",
          "3-7 must not receive output_config");
    CHECK(anthropic_map_effort("claude-3-5-haiku", "low") == "",
          "3-5 must not receive output_config");
    // Unknown model ids fail closed.
    CHECK(anthropic_map_effort("llama-3", "high") == "",
          "unknown models fail closed");
    std::cout << "[OK] effort omitted on budgeted/none/unknown models\n";
    return true;
}

static bool test_effort_level_stepdown() {
    // 4.6 supports max but not xhigh: xhigh steps down to high.
    CHECK(anthropic_map_effort("claude-opus-4-6", "xhigh") == "high",
          "xhigh steps down to high on 4.6");
    CHECK(anthropic_map_effort("claude-opus-4-6", "max") == "max",
          "max is valid on 4.6");
    // Dated variants resolve through the prefix rule.
    CHECK(anthropic_map_effort("claude-sonnet-5-20250929", "xhigh") == "xhigh",
          "dated 5.x ids keep xhigh");
    std::cout << "[OK] unsupported effort levels step down\n";
    return true;
}

static bool test_thinking_modes() {
    CHECK(anthropic_model_caps("claude-opus-5").thinking == ThinkingMode::Adaptive,
          "5.x is adaptive");
    CHECK(anthropic_model_caps("claude-fable-5").thinking == ThinkingMode::Adaptive,
          "fable-5 is adaptive");
    CHECK(anthropic_model_caps("claude-mythos-5-1").thinking == ThinkingMode::Adaptive,
          "mythos-5.1 is adaptive");
    CHECK(anthropic_model_caps("claude-opus-4-6").thinking == ThinkingMode::Adaptive,
          "4.6 is adaptive");
    CHECK(anthropic_model_caps("claude-opus-4-5").thinking == ThinkingMode::Budgeted,
          "4.5 is budgeted (adaptive would 400)");
    CHECK(anthropic_model_caps("claude-3-7-sonnet").thinking == ThinkingMode::Budgeted,
          "3-7 is budgeted");
    CHECK(anthropic_model_caps("claude-3-5-sonnet").thinking == ThinkingMode::None,
          "3-5 has no thinking");
    CHECK(anthropic_model_caps("totally-unknown").thinking == ThinkingMode::None,
          "unknown models fail closed");
    std::cout << "[OK] per-model thinking modes\n";
    return true;
}

// ---- build_anthropic_body ----

static LLMRequest base_request(const std::string& model) {
    LLMRequest req;
    req.model_id = model;
    req.system = "SYSTEM";
    req.messages = {
        json{{"role", "user"}, {"content", "hello"}},
    };
    return req;
}

static bool test_body_adaptive_thinking() {
    LLMRequest req = base_request("claude-opus-5");
    req.reasoning_effort = "off";
    req.temperature = 0.7;
    req.top_p = 0.9;
    json body = build_anthropic_body(req);

    CHECK(body["thinking"]["type"] == "adaptive",
          "adaptive models get thinking config");
    CHECK(body["thinking"]["display"] == "summarized",
          "display summarized so the reasoning panel receives text");
    CHECK(!body.contains("temperature"),
          "5.x rejects non-default sampling params; omit temperature");
    CHECK(!body.contains("top_p"), "omit top_p on 5.x");
    CHECK(body["output_config"]["effort"] == "low",
          "\"off\" lands on effort low, never sent verbatim");
    CHECK(body["max_tokens"] == kDefaultMaxTokens,
          "max_tokens fallback intact");
    std::cout << "[OK] adaptive body: thinking+summarized, no sampling, off->low\n";
    return true;
}

static bool test_body_budgeted_and_none() {
    LLMRequest req = base_request("claude-opus-4-5");
    req.temperature = 0.7;
    req.reasoning_effort = "high";
    json body = build_anthropic_body(req);
    CHECK(!body.contains("thinking"), "4.5 gets no auto thinking config");
    CHECK(body.contains("temperature"), "4.5 still accepts temperature");
    CHECK(!body.contains("output_config"), "4.5 gets no effort param");

    LLMRequest old = base_request("claude-3-5-haiku");
    old.temperature = 0.5;
    json body2 = build_anthropic_body(old);
    CHECK(!body2.contains("thinking"), "3-5 gets no thinking param");
    CHECK(body2.contains("temperature"), "3-5 keeps temperature");
    std::cout << "[OK] budgeted/none models: no thinking, sampling kept\n";
    return true;
}

// ---- ThinkingBlockAcc: SSE fragment accumulation ----

static bool test_thinking_block_fragments() {
    ThinkingBlockAcc acc;
    acc.apply_delta(json{{"type", "thinking_delta"}, {"thinking", "Let me "}});
    acc.apply_delta(json{{"type", "text_delta"}, {"text", "not here"}});
    acc.apply_delta(json{{"type", "thinking_delta"}, {"thinking", "think."}});
    acc.apply_delta(json{{"type", "signature_delta"}, {"signature", "SigA"}});
    acc.apply_delta(json{{"type", "signature_delta"}, {"signature", "SigB"}});
    CHECK(acc.thinking == "Let me think.",
          "thinking fragments concatenate in order");
    CHECK(acc.signature == "SigASigB",
          "signature fragments concatenate (needed for verbatim replay)");
    std::cout << "[OK] thinking/signature deltas accumulate across fragments\n";
    return true;
}

// ---- Replay: assemble_messages emits thinking before tool_use ----

static haicode::SessionMessage row(const std::string& type,
                                   const std::string& data) {
    haicode::SessionMessage m;
    m.type = type;
    m.data_json = data;
    return m;
}

static bool test_replay_thinking_before_tool_use() {
    std::vector<haicode::SessionMessage> msgs = {
        row("user_prompted", R"({"text":"read config"})"),
        row("assistant_text", R"({
            "role": "assistant",
            "text": "Checking.",
            "thinking_blocks": [
                {"thinking": "T1", "signature": "Sig1"},
                {"thinking": "T2", "signature": "Sig2"}
            ],
            "tool_calls": [
                {"id": "toolu_1", "name": "read",
                 "input": {"path": "config.h"}}
            ]
        })"),
        row("tool_result", R"({"call_id":"toolu_1","success":true,"output":"int x = 1;"})"),
    };
    haicode::ContextBuilder builder;
    auto out = builder.assemble_messages(msgs, true);
    CHECK(out.size() == 3, "three wire messages");
    auto& asst = out[1];
    CHECK(asst["role"] == "assistant", "assistant row kept");
    CHECK(asst["content"].is_array(), "tool-carrying row uses content array");
    CHECK(asst["content"].size() == 4,
          "thinking T1, thinking T2, text, tool_use");
    CHECK(asst["content"][0]["type"] == "thinking"
          && asst["content"][0]["thinking"] == "T1"
          && asst["content"][0]["signature"] == "Sig1",
          "first thinking block replays verbatim and first");
    CHECK(asst["content"][1]["type"] == "thinking"
          && asst["content"][1]["signature"] == "Sig2",
          "second thinking block replays verbatim");
    CHECK(asst["content"][2]["type"] == "text", "text after thinking");
    CHECK(asst["content"][3]["type"] == "tool_use", "tool_use last");
    std::cout << "[OK] thinking blocks replay verbatim ahead of tool_use\n";
    return true;
}

static bool test_replay_ignored_without_tools() {
    std::vector<haicode::SessionMessage> msgs = {
        row("user_prompted", R"({"text":"hi"})"),
        row("assistant_text", R"({
            "role": "assistant",
            "text": "Hello.",
            "thinking_blocks": [{"thinking": "T", "signature": "S"}]
        })"),
    };
    haicode::ContextBuilder builder;
    auto out = builder.assemble_messages(msgs, true);
    CHECK(out[1]["content"] == "Hello.",
          "text-only rows stay strings; thinking replay is tool-loop-only");
    std::cout << "[OK] no thinking replay on text-only rows\n";
    return true;
}

// ---- OpenAI translation must keep dropping thinking blocks ----

// ---- Anthropic prompt-cache layout: dynamic text trails the messages ----
//
// Regression: system_dynamic (todos, step budget, offline note) used to be a
// second system block. The cache prefix is tools → system → messages, so
// every todo change re-billed the whole conversation as a cache write.

static int count_breakpoints(const json& body) {
    int n = 0;
    std::function<void(const json&)> walk = [&](const json& j) {
        if (j.is_object()) {
            if (j.contains("cache_control")) ++n;
            for (auto& [k, v] : j.items()) walk(v);
        } else if (j.is_array()) {
            for (auto& v : j) walk(v);
        }
    };
    walk(body);
    return n;
}

static bool test_anthropic_dynamic_tail_cache_layout() {
    LLMRequest req;
    req.model_id = "claude-opus-5";
    req.system = "STABLE";
    req.system_dynamic = "\n\n# Active todos\n\n- [ ] A\n";
    req.tools.push_back({"read", "Read", json{{"type", "object"}}});
    req.messages = {
        {{"role", "user"}, {"content", "go"}},
        {{"role", "assistant"}, {"content", json::array({
            {{"type", "tool_use"}, {"id", "t1"}, {"name", "read"},
             {"input", json::object()}}})}},
        {{"role", "user"}, {"content", json::array({
            {{"type", "tool_result"}, {"tool_use_id", "t1"},
             {"content", "data"}}})}},
    };
    json b1 = build_anthropic_body(req);
    CHECK(b1["system"].size() == 1 && b1["system"][0]["text"] == "STABLE",
          "system carries only the stable block");
    auto& last = b1["messages"].back();
    CHECK(last["role"] == "user" && last["content"].size() == 2,
          "dynamic text appended to the last user message");
    CHECK(last["content"][0]["type"] == "tool_result"
          && last["content"][0].contains("cache_control"),
          "breakpoint on the last block before the dynamic text");
    CHECK(last["content"][1]["type"] == "text"
          && last["content"][1]["text"] == req.system_dynamic
          && !last["content"][1].contains("cache_control"),
          "dynamic text trails, uncached");
    CHECK(b1["messages"][1]["content"].back().contains("cache_control"),
          "second-to-last message keeps its fallback breakpoint");
    CHECK(count_breakpoints(b1) == 4, "never more than the API's 4 breakpoints");

    // Next step: the todo list changed and one exchange was appended. The
    // step-1 prefix (minus its dynamic block) must reappear unchanged,
    // ignoring where the breakpoints sit.
    LLMRequest req2 = req;
    req2.system_dynamic = "\n\n# Active todos\n\n- [x] A\n";
    req2.messages.push_back({{"role", "assistant"}, {"content", "done"}});
    req2.messages.push_back({{"role", "user"}, {"content", "thanks"}});
    json b2 = build_anthropic_body(req2);
    auto strip = [](json j) {
        std::function<void(json&)> walk = [&](json& x) {
            if (x.is_object()) {
                x.erase("cache_control");
                for (auto& [k, v] : x.items()) walk(v);
            } else if (x.is_array()) {
                for (auto& v : x) walk(v);
            }
        };
        walk(j);
        return j;
    };
    CHECK(b2["system"] == b1["system"], "system identical across a todo change");
    CHECK(b2["tools"] == b1["tools"], "tools identical across steps");
    json p1 = strip(b1["messages"]);
    p1.back()["content"].erase(p1.back()["content"].size() - 1);  // drop dyn
    json p2 = strip(b2["messages"]);
    for (size_t i = 0; i < p1.size(); ++i) {
        json a = p1[i], c = p2[i];
        // String vs one-text-block array render identically on the API.
        auto norm = [](json& m) {
            if (m["content"].is_string())
                m["content"] = json::array({{{"type", "text"},
                                             {"text", m["content"]}}});
        };
        norm(a); norm(c);
        CHECK(a == c, "step-1 messages are an unchanged prefix of step 2");
    }
    CHECK(b2["messages"].back()["content"][0]["text"] == "thanks"
          && b2["messages"].back()["content"][1]["text"] == req2.system_dynamic,
          "string user prompt promoted, dynamic text after it");

    // Conversation ending on an assistant turn: dynamic becomes its own
    // trailing user message.
    LLMRequest req3 = req;
    req3.messages.push_back({{"role", "assistant"}, {"content", "ok"}});
    json b3 = build_anthropic_body(req3);
    CHECK(b3["messages"].back()["role"] == "user"
          && b3["messages"].back()["content"][0]["text"] == req.system_dynamic,
          "assistant-final history gets a trailing user message");

    // No messages at all: dynamic falls back to a second system block.
    LLMRequest req4;
    req4.model_id = "claude-opus-5";
    req4.system = "STABLE";
    req4.system_dynamic = "DYN";
    json b4 = build_anthropic_body(req4);
    CHECK(b4["system"].size() == 2 && b4["system"][1]["text"] == "DYN",
          "messageless request keeps dynamic in system");

    // No dynamic text: messages untouched apart from breakpoints.
    LLMRequest req5 = req;
    req5.system_dynamic.clear();
    json b5 = build_anthropic_body(req5);
    CHECK(b5["messages"].back()["content"].size() == 1,
          "no dynamic text, no extra block");
    std::cout << "[OK] anthropic: dynamic text trails messages, prefix cache-stable\n";
    return true;
}

static bool test_openai_translate_drops_thinking() {
    json thinking = {{"type", "thinking"}, {"thinking", "T"},
                     {"signature", "Sig"}};
    json tool_use = {{"type", "tool_use"}, {"id", "toolu_1"},
                     {"name", "read"}, {"input", json{{"path", "x"}}}};
    auto out = haicode::translate_messages("", "", {
        json{{"role", "assistant"},
             {"content", json::array({thinking, tool_use})}},
    });
    CHECK(out.size() == 1, "one translated message");
    std::string dumped = out[0].dump();
    CHECK(dumped.find("Sig") == std::string::npos
          && dumped.find("\"thinking\"") == std::string::npos,
          "thinking/signature must not leak into OpenAI requests");
    CHECK(out[0].contains("tool_calls") && out[0]["tool_calls"].size() == 1,
          "tool_calls survive translation");
    std::cout << "[OK] openai translate drops thinking blocks\n";
    return true;
}

// ---- openai_map_effort basics (Task 24 exercises the body matrix) ----

static bool test_openai_effort_mapping() {
    CHECK(haicode::openai_map_effort("gpt-5", "off") == "none",
          "gpt-5 off maps to none");
    CHECK(haicode::openai_map_effort("o3", "off") == "",
          "o-series has no none; off omits the param");
    CHECK(haicode::openai_map_effort("gpt-5", "max") == "xhigh",
          "chat completions caps at xhigh");
    CHECK(haicode::openai_map_effort("gpt-4o", "high") == "high",
          "plain levels pass through");
    std::cout << "[OK] openai effort mapping basics\n";
    return true;
}

// ---- build_openai_body: flavor × reasoning-model matrix (Task 24) ----

static bool test_openai_body_reasoning_model() {
    LLMRequest req = base_request("o3");
    req.max_tokens = 4096;
    req.temperature = 0.7;
    req.top_p = 0.9;
    req.reasoning_effort = "high";
    json body = build_openai_body(req, ServerFlavor::Generic);
    CHECK(body["max_completion_tokens"] == 4096,
          "reasoning models get max_completion_tokens");
    CHECK(!body.contains("max_tokens"),
          "max_tokens is a 400 unsupported_parameter on reasoning models");
    CHECK(!body.contains("temperature"), "temperature omitted on o-series");
    CHECK(!body.contains("top_p"), "top_p omitted on o-series");
    CHECK(body["reasoning_effort"] == "high", "effort passes through");

    // Non-reasoning model keeps the legacy shape.
    LLMRequest plain = base_request("gpt-4o");
    plain.max_tokens = 1000;
    plain.temperature = 0.5;
    json body2 = build_openai_body(plain, ServerFlavor::Generic);
    CHECK(body2["max_tokens"] == 1000, "non-reasoning keeps max_tokens");
    CHECK(body2.contains("temperature"), "non-reasoning keeps temperature");
    CHECK(!body2.contains("reasoning_effort"),
          "effort omitted for models without reasoning support");
    std::cout << "[OK] openai body: reasoning-model token/sampling gating\n";
    return true;
}

static bool test_openai_body_flavor_effort() {
    // Flavored local server: off → chat_template_kwargs, no reasoning_effort.
    LLMRequest off = base_request("qwen3-32b");
    off.reasoning_effort = "off";
    json vllm = build_openai_body(off, ServerFlavor::VLLM);
    CHECK(vllm["chat_template_kwargs"]["enable_thinking"] == false,
          "vLLM off suppresses thinking via chat_template_kwargs");
    CHECK(!vllm.contains("reasoning_effort"),
          "flavored servers never get reasoning_effort");

    json ollama = build_openai_body(off, ServerFlavor::Ollama);
    CHECK(ollama["chat_template_kwargs"]["enable_thinking"] == false,
          "Ollama flavor keeps the enable_thinking mapping");

    // Non-off effort on flavored servers: nothing sent either way.
    LLMRequest high = base_request("qwen3-32b");
    high.reasoning_effort = "high";
    json vllm2 = build_openai_body(high, ServerFlavor::VLLM);
    CHECK(!vllm2.contains("chat_template_kwargs"),
          "non-off effort sends no template kwargs");
    CHECK(!vllm2.contains("reasoning_effort"),
          "flavored servers skip reasoning_effort entirely");

    // Generic: gpt-5 off → "none"; o-series off → omitted.
    LLMRequest g5 = base_request("gpt-5");
    g5.reasoning_effort = "off";
    json gen5 = build_openai_body(g5, ServerFlavor::Generic);
    CHECK(gen5["reasoning_effort"] == "none", "gpt-5 off maps to none");
    LLMRequest o3 = base_request("o3");
    o3.reasoning_effort = "off";
    json gen3 = build_openai_body(o3, ServerFlavor::Generic);
    CHECK(!gen3.contains("reasoning_effort"), "o-series off omits the param");
    CHECK(!gen3.contains("chat_template_kwargs"),
          "Generic never gets template kwargs");
    std::cout << "[OK] openai body: flavor x effort matrix\n";
    return true;
}

// llama.cpp prefill-progress opt-in and the per-flavor stall limit: a large
// uncached prompt (compaction's summarizer) prefills silently for minutes on
// a local server, and the hosted-API 60 s stall limit killed it mid-prefill.
static bool test_openai_local_server_liveness() {
    LLMRequest req = base_request("qwen3-32b");
    CHECK(build_openai_body(req, ServerFlavor::LlamaCpp)["return_progress"] == true,
          "llama.cpp asks for prompt_progress chunks");
    for (ServerFlavor f : {ServerFlavor::Generic, ServerFlavor::OpenRouter,
                           ServerFlavor::VLLM, ServerFlavor::LMStudio,
                           ServerFlavor::Ollama})
        CHECK(!build_openai_body(req, f).contains("return_progress"),
              "return_progress is llama.cpp-only");

    for (ServerFlavor f : {ServerFlavor::VLLM, ServerFlavor::LlamaCpp,
                           ServerFlavor::LMStudio, ServerFlavor::Ollama})
        CHECK(openai_stall_timeout(f) == kLocalServerStallTimeoutSec,
              "local servers get the long stall limit");
    CHECK(kLocalServerStallTimeoutSec >= 10 * 60,
          "local stall limit covers a multi-minute prefill");
    CHECK(openai_stall_timeout(ServerFlavor::Generic)
              == HttpClient::kDefaultStallTimeoutSec,
          "hosted OpenAI keeps the 60 s default");
    CHECK(openai_stall_timeout(ServerFlavor::OpenRouter)
              == HttpClient::kDefaultStallTimeoutSec,
          "OpenRouter keeps the 60 s default");
    std::cout << "[OK] openai body: llama.cpp return_progress + local stall limit\n";
    return true;
}

static bool test_openai_usage_cached_tokens() {
    TokenUsage usage;
    parse_openai_usage(json{
        {"prompt_tokens", 1000},
        {"completion_tokens", 200},
        {"prompt_tokens_details", {{"cached_tokens", 640}}},
        {"completion_tokens_details", {{"reasoning_tokens", 128}}}
    }, usage);
    CHECK(usage.input == 360, "input excludes cached prompt tokens");
    CHECK(usage.total_input() == 1000, "cached prompt counted exactly once");
    CHECK(usage.output == 200, "completion_tokens parsed");
    CHECK(usage.cache_read == 640,
          "cached_tokens lifted into cache_read for discount billing");
    parse_openai_usage(json{{"prompt_tokens", 1000}}, usage);
    CHECK(usage.input == 1000 && usage.cache_read == 0,
          "missing details clear previous cached usage");
    for (int cached : {1000, 1500, -100}) {
        parse_openai_usage(json{{"prompt_tokens", 1000},
            {"prompt_tokens_details", {{"cached_tokens", cached}}}}, usage);
        CHECK(usage.total_input() == 1000 && usage.input >= 0,
              "inconsistent cache counts preserve the total");
        CHECK(usage.cache_read == (cached < 0 ? 0 : 1000),
              "cache count clamped to the prompt size");
    }
    parse_openai_usage(json{{"prompt_tokens", -100},
        {"prompt_tokens_details", {{"cached_tokens", 200}}}}, usage);
    CHECK(usage.total_input() == 0, "negative prompt count clamped");
    std::cout << "[OK] openai usage: cached_tokens -> cache_read\n";
    return true;
}

// ---- classify_provider_error / backoff / Retry-After (Task 25) ----

static bool test_error_classification() {
    CHECK(classify_provider_error("Overloaded error") == ProviderErrorKind::Transient,
          "overload is transient");
    CHECK(classify_provider_error("HTTP 429: too many requests") == ProviderErrorKind::Transient,
          "429 is transient");
    CHECK(classify_provider_error("HTTP 529: service overloaded") == ProviderErrorKind::Transient,
          "529 (Anthropic) is transient");
    CHECK(classify_provider_error("rate limit exceeded") == ProviderErrorKind::Transient,
          "rate limit text is transient");
    CHECK(classify_provider_error("connection timed out") == ProviderErrorKind::Transient,
          "timeout is transient");
    CHECK(classify_provider_error("context_length_exceeded") == ProviderErrorKind::Overflow,
          "openai marker is overflow");
    CHECK(classify_provider_error("Prompt is too long: 100 > 200") == ProviderErrorKind::Overflow,
          "anthropic marker is overflow");
    CHECK(classify_provider_error("maximum context length is 8192") == ProviderErrorKind::Overflow,
          "maximum-context-length marker is overflow");
    CHECK(classify_provider_error("request_too_large") == ProviderErrorKind::Overflow,
          "request_too_large is overflow");
    // False-positive guards: quota/billing errors must NOT compact.
    CHECK(classify_provider_error("You exceeded your current quota") == ProviderErrorKind::Fatal,
          "quota error must not be overflow");
    CHECK(classify_provider_error("file size exceeds limit") == ProviderErrorKind::Fatal,
          "bare 'exceeds' must not be overflow");
    CHECK(classify_provider_error("invalid api key") == ProviderErrorKind::Fatal,
          "auth errors are fatal");
    std::cout << "[OK] provider error classification matrix\n";
    return true;
}

static bool test_retry_backoff_and_parse() {
    CHECK(retry_backoff_ms(0) == 500, "first backoff 0.5s");
    CHECK(retry_backoff_ms(1) == 2000, "second backoff 2s");
    CHECK(retry_backoff_ms(2) == 8000, "third backoff 8s");
    CHECK(retry_backoff_ms(3) == 10000, "backoff caps at 10s");
    CHECK(parse_retry_after_seconds("HTTP 429 [retry-after: 7]") == 7.0,
          "retry-after parsed from error text");
    CHECK(parse_retry_after_seconds("HTTP 429") == 0.0,
          "absent marker parses as 0");
    std::cout << "[OK] backoff schedule + retry-after parse\n";
    return true;
}

// ---- Anthropic Models API parse (Task 26 discovery) ----

static bool test_anthropic_models_parse() {
    std::vector<AnthropicModelEntry> entries;
    CHECK(parse_anthropic_models(json{
        {"data", json::array({
            json{{"id", "claude-opus-5"}, {"max_input_tokens", 1000000}},
            json{{"id", "claude-haiku-4-5"}, {"max_input_tokens", 200000}},
            json{{"id", ""}, {"max_input_tokens", 999}},   // skipped
            json{{"max_input_tokens", 5}},                  // no id, skipped
            json{{"id", "proxy-model-no-limits"}},
        })}
    }, entries), "valid models document parses");
    CHECK(entries.size() == 3, "empty-id entries skipped");
    CHECK(entries[0].id == "claude-opus-5"
          && entries[0].max_input_tokens == 1000000,
          "max_input_tokens discovered per model");
    CHECK(entries[2].id == "proxy-model-no-limits"
          && entries[2].max_input_tokens == 0,
          "missing limits parse as 0 (table/config fallback)");

    std::vector<AnthropicModelEntry> bad;
    CHECK(!parse_anthropic_models(json{{"error", "x"}}, bad),
          "non-models document rejected");
    std::cout << "[OK] anthropic models-API parse\n";
    return true;
}

int main() {
    bool ok = true;
    ok = test_effort_off_maps_low() && ok;
    ok = test_effort_unsupported_models() && ok;
    ok = test_effort_level_stepdown() && ok;
    ok = test_thinking_modes() && ok;
    ok = test_body_adaptive_thinking() && ok;
    ok = test_body_budgeted_and_none() && ok;
    ok = test_thinking_block_fragments() && ok;
    ok = test_replay_thinking_before_tool_use() && ok;
    ok = test_replay_ignored_without_tools() && ok;
    ok = test_anthropic_dynamic_tail_cache_layout() && ok;
    ok = test_openai_translate_drops_thinking() && ok;
    ok = test_openai_effort_mapping() && ok;
    ok = test_openai_body_reasoning_model() && ok;
    ok = test_openai_body_flavor_effort() && ok;
    ok = test_openai_local_server_liveness() && ok;
    ok = test_openai_usage_cached_tokens() && ok;
    ok = test_error_classification() && ok;
    ok = test_retry_backoff_and_parse() && ok;
    ok = test_anthropic_models_parse() && ok;
    if (!ok) return 1;
    std::cout << "All provider param tests passed\n";
    return 0;
}
