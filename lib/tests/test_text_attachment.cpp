#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/compaction.h>
#include <haicode/util.h>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using json = nlohmann::json;

// One user_prompted row with an attachments array as submit_prompt persists it.
static haicode::SessionMessage make_prompt_row(
        const std::vector<json>& attachments) {
    json data;
    data["role"] = "user";
    data["text"] = "look at this file";
    data["attachments"] = attachments;
    haicode::SessionMessage m;
    m.id = "m1"; m.session_id = "s1"; m.type = "user_prompted"; m.seq = 1;
    m.data_json = data.dump();
    return m;
}

static json text_attachment(const std::string& content) {
    return json{
        {"kind", "text"},
        {"media_type", "text/plain"},
        {"path", "/proj/src/main.cpp"},
        {"data_b64", haicode::util::base64_encode(content)}
    };
}

static json image_attachment() {
    return json{
        {"kind", "image"},
        {"media_type", "image/png"},
        {"path", "/proj/logo.png"},
        {"data_b64", "aGVsbG8="}
    };
}

// assemble_messages: a text attachment becomes a text content block with the
// decoded body + path, regardless of model_accepts_images.
static bool test_text_attachment_becomes_text_block() {
    haicode::ContextBuilder builder;
    std::vector<haicode::SessionMessage> msgs = {
        make_prompt_row({text_attachment("int main() { return 0; }")})
    };
    // model_accepts_images=false must NOT degrade text attachments — they are
    // text blocks, not vision blocks.
    auto out = builder.assemble_messages(msgs, false);
    CHECK(out.size() == 1, "one message");
    CHECK(out[0].value("role", "") == "user", "role user");
    const auto& content = out[0]["content"];
    CHECK(content.is_array(), "attachment prompts use array content");
    CHECK(content.size() == 2, "prompt text + attachment text block");
    CHECK(content[0].value("text", "") == "look at this file",
          "original prompt text first");
    std::string att = content[1].value("text", "");
    CHECK(content[1].value("type", "") == "text", "attachment is a text block");
    CHECK(att.find("Attached file: /proj/src/main.cpp") != std::string::npos,
          "attachment block labels the source path");
    CHECK(att.find("int main() { return 0; }") != std::string::npos,
          "attachment block carries the decoded body");
    std::cout << "[OK] text attachment assembles as a labeled text block\n";
    return true;
}

// Non-UTF-8 bytes inside a text attachment must not break serialization
// (nlohmann's strict serializer throws on invalid UTF-8 otherwise).
static bool test_text_attachment_sanitizes_utf8() {
    std::string dirty("ok\xC3\x28bad");   // invalid UTF-8 sequence
    haicode::ContextBuilder builder;
    std::vector<haicode::SessionMessage> msgs = {
        make_prompt_row({text_attachment(dirty)})
    };
    auto out = builder.assemble_messages(msgs, true);
    std::string dumped;
    try {
        dumped = out[0].dump();
    } catch (...) {
        std::cerr << "[FAIL] dump threw on non-UTF-8 text attachment\n";
        return false;
    }
    CHECK(dumped.find("ok") != std::string::npos, "valid bytes survive");
    std::cout << "[OK] non-UTF-8 text attachment serializes safely\n";
    return true;
}

// Images keep their vision-block behavior; text sits alongside them.
static bool test_mixed_text_and_image_attachments() {
    haicode::ContextBuilder builder;
    std::vector<haicode::SessionMessage> msgs = {
        make_prompt_row({text_attachment("BODY"), image_attachment()})
    };
    auto out = builder.assemble_messages(msgs, true);
    const auto& content = out[0]["content"];
    CHECK(content.size() == 3, "prompt + text block + image block");
    CHECK(content[1].value("type", "") == "text"
              && content[1].value("text", "").find("BODY") != std::string::npos,
          "text attachment stays text");
    CHECK(content[2].value("type", "") == "image"
              && content[2]["source"].value("media_type", "") == "image/png",
          "image attachment stays an image block");
    std::cout << "[OK] mixed attachments keep their kinds side by side\n";
    return true;
}

// serialize_history: text attachments render as [text attachment: path],
// images keep the existing [image attachment: name, media_type] line.
static bool test_serialize_history_text_attachment() {
    std::vector<haicode::SessionMessage> msgs = {
        make_prompt_row({text_attachment("x"), image_attachment()})
    };
    std::string s = haicode::serialize_history(msgs, 1024);
    CHECK(s.find("[text attachment: /proj/src/main.cpp]") != std::string::npos,
          "text attachment renders as [text attachment: path]");
    CHECK(s.find("[image attachment: ") != std::string::npos,
          "image attachment rendering unchanged");
    std::cout << "[OK] serialize_history distinguishes text from image\n";
    return true;
}

// base64_decode inverts base64_encode for every remainder class.
static bool test_base64_roundtrip() {
    const std::string cases[] = {"", "a", "ab", "abc", "abcd",
                                 "int main() { return 0; }\n",
                                 std::string("\x00\x01\xFE\xFF", 4)};
    for (const auto& c : cases) {
        std::string enc = haicode::util::base64_encode(c);
        std::string dec = haicode::util::base64_decode(enc);
        CHECK(dec == c, "base64 round-trip mismatch");
    }
    std::cout << "[OK] base64_decode inverts base64_encode\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_base64_roundtrip();
    ok &= test_text_attachment_becomes_text_block();
    ok &= test_text_attachment_sanitizes_utf8();
    ok &= test_mixed_text_and_image_attachments();
    ok &= test_serialize_history_text_attachment();
    std::cout << (ok ? "ALL PASS\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
