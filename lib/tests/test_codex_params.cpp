// ChatGPT (Codex backend) request building — prompt-cache layout and
// reasoning replay. Regressions for the usage blow-up where per-step dynamic
// content (todos, step budget) rode in `instructions` ahead of the whole
// history, re-billing it uncached on every todo change; for the missing
// prompt_cache_key; and for reasoning being discarded between steps
// (store:false without encrypted_content), which made the model re-derive it
// every step of a tool loop.
#include <haicode/codex_params.h>
#include <haicode/engine.h>
#include <haicode/db.h>
#include <iostream>
#include <vector>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using json = nlohmann::json;
using namespace haicode;

static LLMRequest base_request(const std::vector<json>& messages,
                               const std::string& dynamic) {
    LLMRequest req;
    req.model_id = "gpt-5.5-codex";
    req.system = "STABLE SYSTEM PROMPT";
    req.system_dynamic = dynamic;
    req.messages = messages;
    req.tools.push_back({"read", "Read a file", json{{"type", "object"}}});
    req.cache_key = "sess_123";
    return req;
}

static bool test_dynamic_tail_keeps_prefix_stable() {
    std::vector<json> step1 = {
        {{"role", "user"}, {"content", "fix the build"}},
    };
    json b1 = build_codex_body(base_request(step1,
        "\n\n# Active todos\n\n- [ ] Fix build\n"));
    CHECK(b1["instructions"] == "STABLE SYSTEM PROMPT",
          "instructions carry only the stable system prompt");
    CHECK(b1["input"].size() == 2, "history item + dynamic tail");
    CHECK(b1["input"].back()["role"] == "user"
          && b1["input"].back()["content"].get<std::string>().find("Active todos")
             != std::string::npos,
          "dynamic content is the last input item");

    // Next step: a tool exchange was appended and the todo list changed.
    std::vector<json> step2 = step1;
    step2.push_back({{"role", "assistant"}, {"content", json::array({
        {{"type", "tool_use"}, {"id", "call_1"}, {"name", "read"},
         {"input", {{"path", "Makefile"}}}}})}});
    step2.push_back({{"role", "user"}, {"content", json::array({
        {{"type", "tool_result"}, {"tool_use_id", "call_1"},
         {"content", "all: build"}}})}});
    json b2 = build_codex_body(base_request(step2,
        "\n\n# Active todos\n\n- [x] Fix build\n"));
    CHECK(b2["instructions"] == b1["instructions"],
          "a todo change does not touch instructions");
    // Cacheable prefix: everything in step 1 except its dynamic tail must
    // reappear byte-identically at the head of step 2.
    for (size_t i = 0; i + 1 < b1["input"].size(); ++i)
        CHECK(b2["input"][i].dump() == b1["input"][i].dump(),
              "step-1 history is a byte-identical prefix of step 2");
    CHECK(b2["input"].back()["content"].get<std::string>().find("[x]")
              != std::string::npos,
          "updated dynamic content rides at the new tail");

    json b3 = build_codex_body(base_request(step1, ""));
    CHECK(b3["input"].size() == 1, "empty dynamic content adds no item");
    std::cout << "[OK] dynamic content trails input; instructions stable\n";
    return true;
}

static bool test_cache_key_and_include() {
    json b = build_codex_body(base_request({}, ""));
    CHECK(b["prompt_cache_key"] == "sess_123", "cache key forwarded");
    CHECK(b["store"] == false, "store stays false");
    CHECK(b.contains("include") && b["include"].size() == 1
          && b["include"][0] == "reasoning.encrypted_content",
          "reasoning models request encrypted reasoning");

    LLMRequest no_key = base_request({}, "");
    no_key.cache_key.clear();
    CHECK(!build_codex_body(no_key).contains("prompt_cache_key"),
          "no key, no prompt_cache_key");

    LLMRequest plain = base_request({}, "");
    plain.model_id = "gpt-4.1";
    CHECK(!build_codex_body(plain).contains("include"),
          "non-reasoning models never get include (rejected there)");
    CHECK(codex_model_reasons("codex-mini-latest")
          && codex_model_reasons("GPT-5-Codex")
          && codex_model_reasons("o4-mini")
          && !codex_model_reasons("gpt-4o"),
          "reasoning-model detection");
    std::cout << "[OK] prompt_cache_key + encrypted reasoning include\n";
    return true;
}

static bool test_reasoning_item_sanitize() {
    json streamed = {
        {"id", "rs_abc"}, {"type", "reasoning"}, {"status", "completed"},
        {"summary", json::array({{{"type", "summary_text"}, {"text", "S"}}})},
        {"encrypted_content", "ENC"},
    };
    json r = codex_reasoning_item_for_replay(streamed);
    CHECK(!r.is_null(), "reasoning item accepted");
    CHECK(!r.contains("id") && !r.contains("status"),
          "server id dropped (store:false cannot reference it)");
    CHECK(r["type"] == "reasoning" && r["encrypted_content"] == "ENC"
          && r["summary"].size() == 1, "replayable fields kept");

    json no_summary = {{"type", "reasoning"}, {"encrypted_content", "E"}};
    CHECK(codex_reasoning_item_for_replay(no_summary)["summary"].is_array(),
          "missing summary becomes []");
    CHECK(codex_reasoning_item_for_replay(
              json{{"type", "reasoning"}, {"summary", json::array()}}).is_null(),
          "no encrypted_content → not replayable");
    CHECK(codex_reasoning_item_for_replay(
              json{{"type", "message"}, {"encrypted_content", "E"}}).is_null(),
          "non-reasoning item rejected");
    std::cout << "[OK] reasoning item reduced to replayable shape\n";
    return true;
}

static bool test_translate_reasoning_order() {
    std::vector<json> msgs = {
        {{"role", "user"}, {"content", "go"}},
        {{"role", "assistant"}, {"content", json::array({
            {{"type", "thinking"}, {"thinking", "anthropic"}, {"signature", "S"}},
            {{"type", kOpenAIReasoningBlock}, {"item", {
                {"type", "reasoning"}, {"summary", json::array()},
                {"encrypted_content", "ENC1"}}}},
            {{"type", "text"}, {"text", "Reading."}},
            {{"type", "tool_use"}, {"id", "c1"}, {"name", "read"},
             {"input", json::object()}},
        })}},
    };
    auto items = translate_to_responses_items(msgs);
    CHECK(items.size() == 4, "user, reasoning, assistant text, function_call");
    CHECK(items[1]["type"] == "reasoning"
          && items[1]["encrypted_content"] == "ENC1",
          "reasoning replays first in the assistant turn");
    CHECK(items[2]["role"] == "assistant" && items[2]["content"] == "Reading.",
          "text follows reasoning");
    CHECK(items[3]["type"] == "function_call" && items[3]["call_id"] == "c1",
          "function_call follows text");
    for (auto& it : items)
        CHECK(it.dump().find("anthropic") == std::string::npos,
              "Anthropic thinking never reaches this backend");
    std::cout << "[OK] translate: reasoning → text → function_call\n";
    return true;
}

static SessionMessage row(const std::string& type, const std::string& data) {
    SessionMessage m;
    m.type = type;
    m.data_json = data;
    return m;
}

static bool test_assemble_replay_gating() {
    std::vector<SessionMessage> msgs = {
        row("user_prompted", R"({"text":"go"})"),
        row("assistant_text", R"({
            "role": "assistant", "text": "Reading.",
            "reasoning_items": [
                {"model": "gpt-5.5-codex", "item": {"type":"reasoning","summary":[],"encrypted_content":"MINE"}},
                {"model": "gpt-5.4",       "item": {"type":"reasoning","summary":[],"encrypted_content":"OTHER"}}
            ],
            "tool_calls": [{"id":"c1","name":"read","input":{}}]
        })"),
        row("tool_result", R"({"call_id":"c1","success":true,"output":"x"})"),
        row("assistant_text", R"({
            "role": "assistant", "text": "Done.",
            "reasoning_items": [
                {"model": "gpt-5.5-codex", "item": {"type":"reasoning","summary":[],"encrypted_content":"FINAL"}}
            ]
        })"),
    };

    ContextBuilder off;
    auto plain = off.assemble_messages(msgs, true);
    CHECK(json(plain).dump().find("openai_reasoning") == std::string::npos,
          "no replay model → reasoning never on the wire (Anthropic/OpenAI)");
    CHECK(plain[3]["content"] == "Done.", "text-only row stays a string");

    ContextBuilder on;
    on.replay_reasoning_model = "gpt-5.5-codex";
    auto out = on.assemble_messages(msgs, true);
    auto dumped = json(out).dump();
    CHECK(dumped.find("MINE") != std::string::npos
          && dumped.find("FINAL") != std::string::npos,
          "same-model reasoning replays (tool and text-only rows)");
    CHECK(dumped.find("OTHER") == std::string::npos,
          "other-model encrypted reasoning is not replayed");
    CHECK(out[1]["content"][0]["type"] == kOpenAIReasoningBlock,
          "reasoning block leads the assistant content");
    CHECK(out[3]["content"].is_array() && out[3]["content"].size() == 2
          && out[3]["content"][1]["text"] == "Done.",
          "text-only row becomes [reasoning, text]");

    // End to end through the Codex translator.
    auto items = translate_to_responses_items(out);
    int reasoning = 0;
    for (auto& it : items)
        if (it.value("type", "") == "reasoning") ++reasoning;
    CHECK(reasoning == 2, "both same-model reasoning items reach the wire");
    std::cout << "[OK] assemble_messages replays reasoning only when asked\n";
    return true;
}

int main() {
    bool ok = true;
    ok = test_dynamic_tail_keeps_prefix_stable() && ok;
    ok = test_cache_key_and_include() && ok;
    ok = test_reasoning_item_sanitize() && ok;
    ok = test_translate_reasoning_order() && ok;
    ok = test_assemble_replay_gating() && ok;
    if (!ok) return 1;
    std::cout << "All codex param tests passed\n";
    return 0;
}
