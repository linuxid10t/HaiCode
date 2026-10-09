#include <haicode/model_context_parse.h>
#include <haicode/provider.h>
#include <iostream>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using json = nlohmann::json;

// Step N of an agent loop: history ends with a tool_result (mid-loop).
static std::vector<json> make_history_tool_tail() {
    json tool_use = {
        {"type", "tool_use"},
        {"id", "toolu_1"},
        {"name", "read"},
        {"input", json{{"path", "config.h"}}}
    };
    json tool_result = {
        {"type", "tool_result"},
        {"tool_use_id", "toolu_1"},
        {"content", "int x = 1;"}
    };
    return {
        json{{"role", "user"}, {"content", "read the config file"}},
        json{{"role", "assistant"}, {"content", json::array({tool_use})}},
        json{{"role", "user"}, {"content", json::array({tool_result})}}
    };
}

// Fresh conversation: history ends with a plain user message.
static std::vector<json> make_history_user_tail() {
    return {
        json{{"role", "user"}, {"content", "hello there"}}
    };
}

// The full OpenAI message array for a given step's dynamic tail.
static std::vector<json> run(const std::string& sys_dyn) {
    std::vector<json> src = make_history_tool_tail();
    return haicode::translate_messages("STABLE SYSTEM PROMPT", sys_dyn, src);
}

static bool test_message0_is_stable_system() {
    // Step A and step B have different dynamic tails but identical prefix.
    auto a = run("DYNAMIC step 20: budget ok");
    auto b = run("DYNAMIC step 3: CRITICAL");

    CHECK(!a.empty(), "translated history should not be empty");
    CHECK(a[0].at("role") == "system", "message[0] role must be system");
    CHECK(a[0].at("content") == "STABLE SYSTEM PROMPT",
          "message[0] must be the byte-identical stable system prompt");

    // The cacheable prefix (everything except the dynamic tail) must be
    // byte-identical across steps.
    CHECK(a[0] == b[0], "system message must be identical across steps");
    CHECK(a.size() == b.size(), "message count must match across steps");
    for (size_t i = 0; i + 1 < a.size(); ++i)
        CHECK(a[i] == b[i], "prefix messages must match across steps");

    std::cout << "[OK] message[0] is the stable cacheable system prefix\n";
    return true;
}

static bool test_dynamic_tail_after_tool_result() {
    // Mid agent-loop: history ends with a tool message. The dynamic tail
    // folds into it — a separate user message would follow the prompt as a
    // second counted user turn on Mistral-style templates.
    auto out = run("DYN-TEXT");
    // order: [0] system, [1] user, [2] assistant w/ tool_calls, [3] tool
    CHECK(out.size() == 4, "expect system + user + assistant + tool");
    CHECK(out.back().at("role") == "tool",
          "last message must be the tool result carrying the tail");
    CHECK(out.back().at("content") == "int x = 1;\n\nDYN-TEXT",
          "dynamic tail must be appended verbatim to the tool content");
    std::cout << "[OK] dynamic tail folded into the trailing tool result\n";
    return true;
}

static bool test_dynamic_tail_merges_into_user_message() {
    // Fresh turn: history ends with a plain user message. The dynamic tail
    // is appended INTO that message's content to avoid user,user sequences.
    std::vector<json> src = make_history_user_tail();
    auto out = haicode::translate_messages("SYS", "DYN-TEXT", src);
    CHECK(out.size() == 2, "expect system + single user message");
    CHECK(out[1].at("role") == "user", "message[1] must be user");
    CHECK(out[1].at("content") == "hello there\n\nDYN-TEXT",
          "dynamic tail must be appended to the user content");
    std::cout << "[OK] dynamic tail merged into trailing user message\n";
    return true;
}

static bool test_empty_dynamic() {
    std::vector<json> src = make_history_user_tail();
    auto out = haicode::translate_messages("SYS", "", src);
    CHECK(out.size() == 2, "no dynamic → no extra message");
    CHECK(out[1].at("content") == "hello there",
          "user content must be untouched when dynamic is empty");
    std::cout << "[OK] empty dynamic tail leaves messages untouched\n";
    return true;
}

static bool test_no_system() {
    // system empty → no system message; tail still lands at the end
    std::vector<json> src = make_history_tool_tail();
    auto out = haicode::translate_messages("", "DYN", src);
    CHECK(out[0].at("role") != "system", "no system message when system is empty");
    CHECK(out.back().at("content") == "int x = 1;\n\nDYN",
          "tail must still be last");
    std::cout << "[OK] empty system handled\n";
    return true;
}

// User message with text + image blocks in Anthropic internal shape.
static std::vector<json> make_history_image_tail() {
    json image_block = {
        {"type", "image"},
        {"source", {
            {"type", "base64"},
            {"media_type", "image/png"},
            {"data", "aGVsbG8="}
        }}
    };
    json text_block = {{"type", "text"}, {"text", "what is in this picture?"}};
    return {
        json{{"role", "user"}, {"content", json::array({text_block, image_block})}}
    };
}

static bool test_image_translates_to_image_url() {
    std::vector<json> src = make_history_image_tail();
    auto out = haicode::translate_messages("SYS", "", src);
    CHECK(out.size() == 2, "expect system + one user message");
    const auto& content = out[1].at("content");
    CHECK(content.is_array(), "mixed text+image must stay array content");
    CHECK(content.size() == 2, "expect text + image_url parts");
    CHECK(content[0].at("type") == "text", "part[0] must be text");
    CHECK(content[0].at("text") == "what is in this picture?",
          "text part must carry the prompt");
    CHECK(content[1].at("type") == "image_url", "part[1] must be image_url");
    CHECK(content[1].at("image_url").at("url")
          == "data:image/png;base64,aGVsbG8=",
          "image_url must be a correct data: URL");
    std::cout << "[OK] image block translates to OpenAI image_url data URL\n";
    return true;
}

static bool test_image_only_no_text_block() {
    json image_block = {
        {"type", "image"},
        {"source", {
            {"type", "base64"},
            {"media_type", "image/jpeg"},
            {"data", "aGVsbG8="}
        }}
    };
    std::vector<json> src = {
        json{{"role", "user"}, {"content", json::array({image_block})}}
    };
    auto out = haicode::translate_messages("SYS", "", src);
    CHECK(out.size() == 2, "expect system + one user message");
    const auto& content = out[1].at("content");
    CHECK(content.is_array(), "image-only must be array content");
    CHECK(content.size() == 1, "no empty text part for image-only");
    CHECK(content[0].at("type") == "image_url", "sole part must be image_url");
    std::cout << "[OK] image-only user message emits no empty text block\n";
    return true;
}

static bool test_text_attachment_blocks_join_with_newline() {
    // A text attachment renders as a second text block (fenced body). It must
    // reach OpenAI as plain text — never an image_url — with the blocks
    // newline-joined so the body doesn't fuse into the prompt.
    json prompt_block = {{"type", "text"}, {"text", "look at this"}};
    json att_block = {
        {"type", "text"},
        {"text", "\n\nAttached file: /proj/main.cpp\n```\nint x;\n```\n"}
    };
    std::vector<json> src = {
        json{{"role", "user"}, {"content", json::array({prompt_block, att_block})}}
    };
    auto out = haicode::translate_messages("SYS", "", src);
    CHECK(out.size() == 2, "expect system + one user message");
    CHECK(out[1].at("content").is_string(),
          "text-only parts collapse back to a plain string");
    std::string content = out[1].at("content").get<std::string>();
    CHECK(content.find("data:") == std::string::npos,
          "no data: URL may appear for text attachments");
    CHECK(content == "look at this\n\n\nAttached file: /proj/main.cpp"
                     "\n```\nint x;\n```\n",
          "text blocks join with a single newline boundary");
    std::cout << "[OK] text attachment blocks translate to joined plain text\n";
    return true;
}

static bool test_text_only_stays_string() {
    // Regression: attachment-free histories must translate byte-identically
    // to the pre-image string form (llama.cpp prefix-cache).
    std::vector<json> src = make_history_user_tail();
    auto out = haicode::translate_messages("SYS", "", src);
    CHECK(out[1].at("content").is_string(),
          "pure-text user content must remain a string");
    CHECK(out[1].at("content") == "hello there",
          "pure-text content must be verbatim");
    std::cout << "[OK] text-only history still uses plain string content\n";
    return true;
}

// The alternation check of strict chat templates (Mistral/Devstral, Gemma,
// Llama-3): user messages and assistant messages WITHOUT tool_calls must
// alternate starting with user; tool messages and tool-calling assistant
// messages are skipped. Mirrors the template's pre-pass that raised
// "After the optional system message, conversation roles must alternate".
static bool alternates(const std::vector<json>& out, std::string* why) {
    size_t i = (!out.empty() && out[0].at("role") == "system") ? 1 : 0;
    size_t idx = 0;
    for (; i < out.size(); ++i) {
        const auto& m = out[i];
        std::string role = m.at("role");
        if (role == "system") { *why = "system after message 0"; return false; }
        if (role == "tool") continue;
        if (role == "assistant" && m.contains("tool_calls")
                && !m["tool_calls"].empty())
            continue;
        if ((role == "user") != (idx % 2 == 0)) {
            *why = "message " + std::to_string(i) + " (" + role + ")";
            return false;
        }
        if (role == "assistant" && m["content"].is_string()
                && m["content"].get<std::string>().empty()) {
            *why = "empty assistant message " + std::to_string(i);
            return false;
        }
        ++idx;
    }
    return true;
}

static json tool_use(const std::string& id) {
    return {{"type", "tool_use"}, {"id", id}, {"name", "read"},
            {"input", json{{"path", "README.md"}}}};
}
static json tool_result(const std::string& id) {
    return {{"type", "tool_result"}, {"tool_use_id", id}, {"content", "# Title"}};
}

static bool test_turn_without_reply_then_prompt() {
    // The reported failure (llama.cpp + Mistral Small 4): the turn's last
    // step returned only reasoning, so no assistant row was stored and the
    // next prompt followed the tool result directly → HTTP 500 from the
    // template. A placeholder reply now closes the turn.
    std::vector<json> src = {
        json{{"role", "user"}, {"content", "plan the README split"}},
        json{{"role", "assistant"}, {"content", json::array({
            json{{"type", "text"}, {"text", "I will read it."}},
            tool_use("call_0")})}},
        json{{"role", "user"}, {"content", json::array({tool_result("call_0")})}},
        json{{"role", "user"}, {"content", "?"}},
    };
    auto out = haicode::translate_messages("SYS", "", src);
    std::string why;
    CHECK(alternates(out, &why), "roles must alternate: " + why);
    CHECK(out.size() == 6, "system, user, assistant+calls, tool, stub, user");
    CHECK(out[4].at("role") == "assistant"
          && out[4].at("content") == haicode::kAlternationAssistantStub,
          "placeholder reply closes the turn that ended on a tool result");
    CHECK(out[5].at("content") == "?", "next prompt verbatim");

    // Two prompts in a row (a turn that failed before any output).
    auto out2 = haicode::translate_messages("SYS", "", {
        json{{"role", "user"}, {"content", "first"}},
        json{{"role", "user"}, {"content", "second"}}});
    CHECK(alternates(out2, &why), "user,user must alternate: " + why);
    CHECK(out2.size() == 4 && out2[1].at("content") == "first"
          && out2[3].at("content") == "second", "both prompts kept verbatim");
    std::cout << "[OK] turn ending without a text reply gets a placeholder\n";
    return true;
}

static bool test_status_after_tool_result_folds() {
    // A status update rides the tool-result turn (ContextBuilder's
    // append_status_block). It folds into the last tool message instead of
    // becoming a user message, so the following tool-calling step and the
    // final reply still alternate with the prompt.
    std::vector<json> src = {
        json{{"role", "user"}, {"content", "go"}},
        json{{"role", "assistant"}, {"content", json::array({
            tool_use("a"), tool_use("b")})}},
        json{{"role", "user"}, {"content", json::array({
            tool_result("a"), tool_result("b"),
            json{{"type", "text"}, {"text", "[status] todos: 1/3"}}})}},
        json{{"role", "assistant"}, {"content", "done"}},
    };
    auto out = haicode::translate_messages("SYS", "", src);
    std::string why;
    CHECK(alternates(out, &why), "roles must alternate: " + why);
    CHECK(out.size() == 6, "system, user, assistant, tool, tool, assistant");
    CHECK(out[3].at("content") == "# Title", "first result untouched");
    CHECK(out[4].at("role") == "tool"
          && out[4].at("content") == "# Title\n\n[status] todos: 1/3",
          "status folds into the LAST tool message");
    std::cout << "[OK] status text after tool results folds into the tool message\n";
    return true;
}

static bool test_leading_compaction_summary() {
    // After a compaction the context starts with the summary as an
    // assistant turn — Mistral templates require user first.
    std::vector<json> src = {
        json{{"role", "assistant"}, {"content", "Summary of the prior conversation: ..."}},
        json{{"role", "user"}, {"content", "next"}},
    };
    auto out = haicode::translate_messages("SYS", "", src);
    std::string why;
    CHECK(alternates(out, &why), "roles must alternate: " + why);
    CHECK(out.size() == 4 && out[1].at("role") == "user"
          && out[1].at("content") == haicode::kAlternationLeadStub,
          "a stub user turn introducing the summary leads");
    std::cout << "[OK] leading assistant gets a stub user turn\n";
    return true;
}

static bool test_consecutive_assistants_get_user_stub() {
    // Two plain assistant turns in a row get the generic user stub between
    // them (the lead-in wording is only for an assistant heading the list).
    auto out = haicode::translate_messages("SYS", "", {
        json{{"role", "user"}, {"content", "q"}},
        json{{"role", "assistant"}, {"content", "a1"}},
        json{{"role", "assistant"}, {"content", "a2"}}});
    std::string why;
    CHECK(alternates(out, &why), "roles must alternate: " + why);
    CHECK(out.size() == 5 && out[3].at("role") == "user"
          && out[3].at("content") == haicode::kAlternationUserStub,
          "generic user stub between the two replies");
    std::cout << "[OK] consecutive assistants get the generic user stub\n";
    return true;
}

static bool test_empty_assistant_dropped() {
    // Mistral templates raise on an assistant message with neither content
    // nor tool_calls. It is dropped BEFORE the parity check: dropping it
    // leaves user,user, which then gets the placeholder reply.
    for (const json& empty : {json(""), json::array(), json(nullptr)}) {
        auto out = haicode::translate_messages("SYS", "", {
            json{{"role", "user"}, {"content", "one"}},
            json{{"role", "assistant"}, {"content", empty}},
            json{{"role", "user"}, {"content", "two"}}});
        std::string why;
        CHECK(alternates(out, &why), "roles must alternate: " + why);
        CHECK(out.size() == 4
              && out[2].at("content") == haicode::kAlternationAssistantStub,
              "empty reply replaced by the placeholder, content " + empty.dump());
    }
    // A tool-calling assistant with empty content is legal and kept.
    auto out = haicode::translate_messages("SYS", "", {
        json{{"role", "user"}, {"content", "go"}},
        json{{"role", "assistant"}, {"content", json::array({tool_use("t")})}},
        json{{"role", "user"}, {"content", json::array({tool_result("t")})}}});
    CHECK(out.size() == 4 && out[2].at("role") == "assistant"
          && out[2].at("content") == "" && out[2].contains("tool_calls"),
          "tool-calling assistant with empty content is kept");
    std::cout << "[OK] empty assistant without tool_calls is dropped\n";
    return true;
}

static bool test_tool_image_followup_alternates() {
    // Tool-result images can't ride a tool message; their follow-up user
    // message is preceded by a placeholder reply.
    json img = {{"type", "image"}, {"source", {{"type", "base64"},
                {"media_type", "image/png"}, {"data", "QUJD"}}}};
    std::vector<json> src = {
        json{{"role", "user"}, {"content", "screenshot please"}},
        json{{"role", "assistant"}, {"content", json::array({tool_use("s")})}},
        json{{"role", "user"}, {"content", json::array({
            json{{"type", "tool_result"}, {"tool_use_id", "s"},
                 {"content", json::array({
                     json{{"type", "text"}, {"text", "800x600"}}, img})}},
            json{{"type", "text"}, {"text", "[status] offline"}}})}},
        json{{"role", "assistant"}, {"content", "I see a window."}},
    };
    auto out = haicode::translate_messages("SYS", "", src);
    std::string why;
    CHECK(alternates(out, &why), "roles must alternate: " + why);
    CHECK(out[3].at("role") == "tool"
          && out[3].at("content") == "800x600\n\n[status] offline",
          "status folds into the tool text");
    CHECK(out[5].at("role") == "user" && out[5].at("content").is_array(),
          "image follow-up stays a user message after a placeholder");
    std::cout << "[OK] tool image follow-up alternates\n";
    return true;
}

static bool test_alternation_fix_is_prefix_stable() {
    // Each request must be an exact prefix of the next (llama.cpp KV cache):
    // the fixes depend only on a message and those before it.
    std::vector<json> full = {
        json{{"role", "assistant"}, {"content", "Summary ..."}},
        json{{"role", "user"}, {"content", "one"}},
        json{{"role", "assistant"}, {"content", json::array({tool_use("x")})}},
        json{{"role", "user"}, {"content", json::array({tool_result("x"),
            json{{"type", "text"}, {"text", "[status]"}}})}},
        json{{"role", "user"}, {"content", "two"}},
        json{{"role", "assistant"}, {"content", json::array({tool_use("y")})}},
        json{{"role", "user"}, {"content", json::array({tool_result("y")})}},
        json{{"role", "assistant"}, {"content", "answer"}},
        json{{"role", "user"}, {"content", "three"}},
    };
    std::vector<json> prev;
    for (size_t n = 1; n <= full.size(); ++n) {
        std::vector<json> src(full.begin(), full.begin() + n);
        auto out = haicode::translate_messages("SYS", "", src);
        std::string why;
        CHECK(alternates(out, &why), "prefix " + std::to_string(n) + ": " + why);
        CHECK(out.size() >= prev.size(), "never shrinks");
        for (size_t i = 0; i < prev.size(); ++i)
            CHECK(out[i] == prev[i], "request " + std::to_string(n - 1)
                  + " must be a prefix of request " + std::to_string(n));
        prev = out;
    }
    std::cout << "[OK] alternation fixes keep requests prefix-stable\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_message0_is_stable_system();
    ok &= test_dynamic_tail_after_tool_result();
    ok &= test_dynamic_tail_merges_into_user_message();
    ok &= test_empty_dynamic();
    ok &= test_no_system();
    ok &= test_image_translates_to_image_url();
    ok &= test_image_only_no_text_block();
    ok &= test_text_attachment_blocks_join_with_newline();
    ok &= test_text_only_stays_string();
    ok &= test_turn_without_reply_then_prompt();
    ok &= test_status_after_tool_result_folds();
    ok &= test_leading_compaction_summary();
    ok &= test_consecutive_assistants_get_user_stub();
    ok &= test_empty_assistant_dropped();
    ok &= test_tool_image_followup_alternates();
    ok &= test_alternation_fix_is_prefix_stable();
    std::cout << (ok ? "ALL PASS\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
