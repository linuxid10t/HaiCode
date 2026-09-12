#include <haicode/haicode.h>
#include <haicode/model_info.h>
#include <haicode/config.h>
#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/model_context_parse.h>
#include <haicode/compaction.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cassert>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using json = nlohmann::json;
using namespace haicode;

static bool test_vision_detection() {
    CHECK(model_supports_vision("claude-sonnet-4-5", {}) == true,
          "claude-sonnet should be vision-capable via prefix table");
    CHECK(model_supports_vision("gpt-4o-mini", {}) == true,
          "gpt-4o should be vision-capable via prefix table");
    CHECK(model_supports_vision("llama3.2-vision:latest", {}) == true,
          "llama3.2-vision should be vision-capable via prefix table");
    CHECK(model_supports_vision("gpt-3.5-turbo", {}) == false,
          "gpt-3.5 should NOT be vision-capable");
    CHECK(model_supports_vision("totally-unknown-model", {}) == false,
          "unknown models must fail closed (no vision)");
    CHECK(model_supports_vision("claude-sonnet-4-5", {{"claude-sonnet-4-5", false}}) == false,
          "explicit false override must hide vision even for known models");
    CHECK(model_supports_vision("my-local-model", {{"my-local-model", true}}) == true,
          "explicit true override must grant vision to unknown models");
    std::cout << "[ok] vision detection\n";
    return true;
}

static bool test_config_vision_parse_merge() {
    std::string path = "/tmp/haicode_test_vision.json";
    {
        std::ofstream f(path);
        f << R"({"vision": {"model-a": true, "model-b": false},
                 "models": {"model-a": 100000},
                 "vision_fallback": {"provider": "openai", "model": "gpt-4o-mini"}})";
    }
    ConfigLoader loader;
    AppConfig cfg = loader.load_file(path);
    remove(path.c_str());
    CHECK(cfg.model_vision.size() == 2, "vision object should parse into model_vision");
    CHECK(cfg.model_vision["model-a"] == true, "model-a vision=true");
    CHECK(cfg.model_vision["model-b"] == false, "model-b vision=false");
    CHECK(cfg.model_contexts["model-a"] == 100000, "models object still parses");
    CHECK(cfg.vision_fallback_provider == "openai", "vision_fallback provider parses");
    CHECK(cfg.vision_fallback_model == "gpt-4o-mini", "vision_fallback model parses");

    AppConfig base;
    base.model_vision = {{"x", true}};
    base.vision_fallback_provider = "anthropic";
    base.vision_fallback_model = "claude-sonnet-4-5";
    AppConfig overlay;
    overlay.model_vision = {{"x", false}, {"y", true}};
    AppConfig merged = loader.merge(base, overlay);
    CHECK(merged.model_vision["x"] == false, "overlay false must win per key");
    CHECK(merged.model_vision["y"] == true, "overlay adds new keys");
    CHECK(merged.vision_fallback_provider == "anthropic" && merged.vision_fallback_model == "claude-sonnet-4-5",
          "empty overlay preserves base fallback");

    AppConfig overlay2;
    overlay2.vision_fallback_provider = "openai";
    overlay2.vision_fallback_model = "gpt-4o";
    AppConfig merged2 = loader.merge(base, overlay2);
    CHECK(merged2.vision_fallback_provider == "openai" && merged2.vision_fallback_model == "gpt-4o",
          "non-empty overlay wins for fallback pair");

    // Absent object leaves fields empty (feature off).
    std::string path2 = "/tmp/haicode_test_vision2.json";
    {
        std::ofstream f(path2);
        f << R"({"model": "m"})";
    }
    AppConfig cfg2 = loader.load_file(path2);
    remove(path2.c_str());
    CHECK(cfg2.vision_fallback_provider.empty() && cfg2.vision_fallback_model.empty(),
          "absent vision_fallback object leaves fields empty");
    std::cout << "[ok] config vision parse + merge\n";
    return true;
}

static std::vector<SessionMessage> make_msgs();

static bool test_assemble_messages_text_only_fallback() {
    ContextBuilder builder;
    auto msgs = make_msgs();
    // Flag=false: image blocks become text even with no description yet.
    auto out = builder.assemble_messages(msgs, false);
    bool saw_placeholder = false, saw_image = false;
    for (auto& m : out) {
        if (!m.contains("content") || !m["content"].is_array()) continue;
        for (auto& c : m["content"]) {
            if (!c.contains("content") || !c["content"].is_array()) continue;
            for (auto& b : c["content"]) {
                if (b.value("type", "") == "image") saw_image = true;
                if (b.value("type", "") == "text"
                        && b.value("text", "").find("[image attachment:") == 0)
                    saw_placeholder = true;
            }
        }
    }
    CHECK(!saw_image, "text-only assembly must not emit image blocks");
    CHECK(saw_placeholder, "undescribed attachment renders placeholder text");

    // With a persisted description, the text block embeds it.
    auto msgs2 = make_msgs();
    auto j2 = nlohmann::json::parse(msgs2[2].data_json);
    j2["attachments"][0]["description"] = "A desktop with a Tracker window.";
    msgs2[2].data_json = j2.dump();
    auto out2 = builder.assemble_messages(msgs2, false);
    bool saw_desc = false;
    for (auto& m : out2) {
        std::string s = m.dump();
        if (s.find("described by vision fallback: A desktop with a Tracker window.")
                != std::string::npos)
            saw_desc = true;
    }
    CHECK(saw_desc, "described attachment embeds the fallback description");

    // Flag=true regression: image block still emitted.
    auto out3 = builder.assemble_messages(make_msgs(), true);
    bool saw_image3 = false;
    for (auto& m : out3) {
        if (m.dump().find("\"type\":\"image\"") != std::string::npos)
            saw_image3 = true;
    }
    CHECK(saw_image3, "vision assembly still emits image blocks");
    std::cout << "[ok] assemble_messages text-only fallback\n";
    return true;
}

static bool test_screenshot_tool() {
    ToolRegistry registry;
    register_builtin_tools(registry);

    auto tool = registry.get("screenshot");
    CHECK(tool != nullptr, "screenshot tool must be registered");
    bool in_defs = false;
    for (auto& d : registry.definitions())
        if (d.name == "screenshot") in_defs = true;
    CHECK(in_defs, "screenshot must appear in definitions()");

    ToolContext ctx;
    ctx.working_dir = "/tmp";
    PermissionGate gate;  // no rules, no ask callback — must still execute
    ToolResult r = registry.execute("screenshot", json::object(), ctx, gate);
    CHECK(r.success, "screenshot execute should succeed: " + r.error);

    json out = json::parse(r.output, nullptr, false);
    CHECK(out.is_object(), "output must be a JSON object");
    CHECK(out.contains("attachments") && out["attachments"].is_array()
              && out["attachments"].size() == 1,
          "output must carry exactly one attachment");
    auto& att = out["attachments"][0];
    CHECK(att.value("media_type", "") == "image/png", "media_type must be image/png");
    std::string path = att.value("path", "");
    CHECK(path.find("haicode_shot_") != std::string::npos,
          "path must be a haicode_shot_ temp file: " + path);
    CHECK(!att.value("data_b64", "").empty(), "attachment must carry base64 data");

    std::ifstream f(path, std::ios::binary);
    CHECK(f.good(), "PNG file must exist at " + path);
    char magic[8] = {0};
    f.read(magic, 8);
    CHECK(magic[0] == '\x89' && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G',
          "file must start with PNG signature");
    std::string summary = out.value("summary", "");
    CHECK(summary.find(path) != std::string::npos,
          "summary must name the saved path");
    CHECK(summary.find("x") != std::string::npos,
          "summary should mention WxH dimensions");
    remove(path.c_str());
    std::cout << "[ok] screenshot tool capture\n";
    return true;
}

// Shared history: user prompt → assistant tool_call → tool_result with image.
static std::vector<SessionMessage> make_msgs() {
    std::vector<SessionMessage> msgs;
    SessionMessage u;
    u.type = "user_prompted"; u.seq = 1;
    u.data_json = json{{"text", "take a screenshot"}}.dump();
    msgs.push_back(u);

    SessionMessage a;
    a.type = "assistant_text"; a.seq = 2;
    a.data_json = json{{"text", ""},
        {"tool_calls", json::array({json{{"id", "t1"}, {"name", "screenshot"},
                                         {"input", json::object()}}})}}.dump();
    msgs.push_back(a);

    SessionMessage t;
    t.type = "tool_result"; t.seq = 3;
    t.data_json = json{{"call_id", "t1"},
                       {"output", "Screenshot saved to /tmp/shot.png (800x600)"},
                       {"success", true},
                       {"attachments", json::array({json{
                           {"media_type", "image/png"},
                           {"path", "/tmp/shot.png"},
                           {"data_b64", "QUJD"}}})}}.dump();
    msgs.push_back(t);
    return msgs;
}

static bool test_assemble_messages_image_blocks() {
    ContextBuilder builder;
    auto out = builder.assemble_messages(make_msgs());

    const json* tr = nullptr;
    for (auto& m : out) {
        if (!m.contains("content") || !m["content"].is_array()) continue;
        for (auto& c : m["content"])
            if (c.value("type", "") == "tool_result") tr = &c;
    }
    CHECK(tr != nullptr, "must emit a tool_result block");
    CHECK(tr->value("tool_use_id", "") == "t1", "tool_use_id must match call id");
    CHECK(tr->contains("content") && tr->at("content").is_array(),
          "image tool_result content must be a block array");
    auto& blocks = tr->at("content");
    CHECK(blocks.size() == 2, "tool_result must carry text + image blocks");
    CHECK(blocks[0].value("type", "") == "text"
              && blocks[0].value("text", "").find("800x600") != std::string::npos,
          "first block must be the text output");
    CHECK(blocks[1].value("type", "") == "image", "second block must be an image");
    CHECK(blocks[1]["source"].value("data", "") == "QUJD",
          "image source must carry the base64 payload");
    CHECK(blocks[1]["source"].value("media_type", "") == "image/png",
          "image source media_type");
    std::cout << "[ok] assemble_messages tool_result image blocks\n";
    return true;
}

static bool test_openai_translation_splits_tool_image() {
    ContextBuilder builder;
    auto src = builder.assemble_messages(make_msgs());
    auto out = translate_messages("SYS", "", src);

    const json* tool_msg = nullptr;
    int followup_idx = -1;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].value("role", "") == "tool") tool_msg = &out[i];
        if (tool_msg && (int)i > 0 && out[i].value("role", "") == "user")
            followup_idx = (int)i;
    }
    CHECK(tool_msg != nullptr, "must emit a tool-role message");
    CHECK(tool_msg->value("tool_call_id", "") == "t1", "tool_call_id must match");
    CHECK(tool_msg->at("content").is_string()
              && tool_msg->at("content").get<std::string>().find("800x600")
                     != std::string::npos,
          "tool message content must be the text output only");
    CHECK(followup_idx >= 0, "images must be split into a follow-up user message");
    auto& fu = out[(size_t)followup_idx];
    CHECK(fu["content"].is_array() && fu["content"].size() == 2,
          "follow-up must have a text block + image_url block");
    CHECK(fu["content"][0].value("text", "") == "[image from tool result]",
          "follow-up text marker");
    std::string url = fu["content"][1]["image_url"].value("url", "");
    CHECK(url.find("data:image/png;base64,QUJD") == 0,
          "follow-up image_url must be a data: URL with the payload");
    std::cout << "[ok] openai translation splits tool images\n";
    return true;
}

static bool test_serialize_history_renders_attachments() {
    auto hist = serialize_history(make_msgs(), 4096);
    CHECK(hist.find("[image attachment: /tmp/shot.png") != std::string::npos,
          "serialize_history must render tool_result attachments");
    CHECK(hist.find("image/png") != std::string::npos,
          "attachment rendering must include media type");
    std::cout << "[ok] serialize_history renders tool attachments\n";
    return true;
}

int main() {
    struct T { const char* name; bool (*fn)(); };
    T tests[] = {
        {"vision_detection", test_vision_detection},
        {"config_vision_parse_merge", test_config_vision_parse_merge},
        {"screenshot_tool", test_screenshot_tool},
        {"assemble_messages_image_blocks", test_assemble_messages_image_blocks},
        {"assemble_messages_text_only_fallback", test_assemble_messages_text_only_fallback},
        {"openai_translation_splits_tool_image", test_openai_translation_splits_tool_image},
        {"serialize_history_renders_attachments", test_serialize_history_renders_attachments},
    };
    int failed = 0;
    for (auto& t : tests) {
        if (!t.fn()) {
            std::cerr << "FAILED: " << t.name << "\n";
            failed++;
        }
    }
    if (failed) {
        std::cerr << failed << " test group(s) failed\n";
        return 1;
    }
    std::cout << "all screenshot tests passed\n";
    return 0;
}
