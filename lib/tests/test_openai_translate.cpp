#include <haicode/model_context_parse.h>
#include <haicode/provider.h>
#include <iostream>
#include <cassert>

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
    // must land AFTER it, as its own user message.
    auto out = run("DYN-TEXT");
    // order: [0] system, [1] user, [2] assistant w/ tool_calls, [3] tool,
    // [4] dynamic user tail
    CHECK(out.size() == 5, "expect system + user + assistant + tool + tail");
    CHECK(out[out.size() - 1].at("role") == "user",
          "last message must be user (the dynamic tail)");
    CHECK(out[out.size() - 1].at("content") == "DYN-TEXT",
          "dynamic tail content must be verbatim");
    CHECK(out[out.size() - 2].at("role") == "tool",
          "message before the tail must be the tool result");
    std::cout << "[OK] dynamic tail placed after tool result as user message\n";
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
    CHECK(out[out.size() - 1].at("content") == "DYN",
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
    std::cout << (ok ? "ALL PASS\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
