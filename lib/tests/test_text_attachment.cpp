#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/compaction.h>
#include <haicode/util.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
    // Split the literal so "\x28" doesn't greedily eat "bad" as hex digits.
    std::string dirty("ok\xC3\x28" "bad");   // invalid UTF-8 sequence
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

// ---- Engine-side persistence of absent markers -----------------------------

static const char* kDbPath = "/tmp/haicode_test_text_att.db";

class FakeProvider : public haicode::Provider {
public:
    std::string id() const override { return "fake"; }
    void cancel() override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"fake-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb) override {
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
};

// Submits prompts with unreadable/empty attachments against a real engine +
// store, then inspects the persisted user_prompted row.
static bool test_engine_marks_absent_attachments() {
    remove(kDbPath);
    haicode::Database db(kDbPath);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<FakeProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.provider = "fake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";
    haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
    std::string sid = engine.create_session("/tmp/proj", "build",
                                             "fake-model", "fake");

    // Empty file: encodes to "" — must persist as an absent row, not vanish.
    FILE* f = fopen("/tmp/haicode_test_empty.txt", "w");
    CHECK(f != nullptr, "create empty file");
    fclose(f);

    std::vector<haicode::Attachment> atts;
    haicode::Attachment empty_att;
    empty_att.kind = "text";
    empty_att.media_type = "text/plain";
    empty_att.path = "/tmp/haicode_test_empty.txt";
    atts.push_back(empty_att);
    haicode::Attachment missing_att;
    missing_att.kind = "image";
    missing_att.media_type = "image/png";
    missing_att.path = "/tmp/haicode_test_does_not_exist.png";
    atts.push_back(missing_att);
    engine.submit_prompt(sid, "here you go", atts);

    auto msgs = store.load_messages(sid);
    CHECK(!msgs.empty(), "user_prompted row stored");
    json data = json::parse(msgs[0].data_json, nullptr, false);
    CHECK(data.contains("attachments") && data["attachments"].is_array()
              && data["attachments"].size() == 2,
          "both attachments persisted as rows (none dropped)");

    const auto& a0 = data["attachments"][0];
    CHECK(a0.value("path", "") == "/tmp/haicode_test_empty.txt"
              && a0.value("absent", false),
          "empty file persisted with absent=true");
    const auto& a1 = data["attachments"][1];
    CHECK(a1.value("path", "") == "/tmp/haicode_test_does_not_exist.png"
              && a1.value("absent", false),
          "unreadable file persisted with absent=true");

    // And the assembled request carries the markers as text blocks.
    haicode::ContextBuilder builder;
    auto out = builder.assemble_messages(msgs, false);
    std::string dumped = out[0].dump();
    CHECK(dumped.find("[attachment unavailable: /tmp/haicode_test_empty.txt]")
              != std::string::npos,
          "assembled request marks the empty attachment");
    CHECK(dumped.find(
              "[attachment unavailable: /tmp/haicode_test_does_not_exist.png]")
              != std::string::npos,
          "assembled request marks the missing attachment");

    // Compaction serialization shows the same markers.
    std::string ser = haicode::serialize_history(msgs, 4096);
    CHECK(ser.find("[attachment unavailable: /tmp/haicode_test_empty.txt]")
              != std::string::npos,
          "serialize_history marks the empty attachment");

    remove("/tmp/haicode_test_empty.txt");
    std::cout << "[OK] engine persists absent markers for empty/unreadable files\n";
    return true;
}

// Oversized text attachments are truncated at submit time to exactly the cap
// plus a trailing marker; files at or under the cap pass through untouched.
static bool test_engine_truncates_oversized_text() {
    remove(kDbPath);
    haicode::Database db(kDbPath);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<FakeProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.provider = "fake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";
    haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
    std::string sid = engine.create_session("/tmp/proj", "build",
                                             "fake-model", "fake");

    const size_t cap = 256 * 1024;
    const size_t big = 300 * 1024;
    {
        std::ofstream f("/tmp/haicode_test_big.txt",
                        std::ios::binary | std::ios::trunc);
        CHECK(f.good(), "create oversized file");
        f << std::string(big, 'x');
    }
    {
        std::ofstream f("/tmp/haicode_test_atcap.txt",
                        std::ios::binary | std::ios::trunc);
        CHECK(f.good(), "create at-cap file");
        f << std::string(cap, 'y');
    }

    std::vector<haicode::Attachment> atts;
    haicode::Attachment big_att;
    big_att.kind = "text";
    big_att.media_type = "text/plain";
    big_att.path = "/tmp/haicode_test_big.txt";
    atts.push_back(big_att);
    haicode::Attachment cap_att;
    cap_att.kind = "text";
    cap_att.media_type = "text/plain";
    cap_att.path = "/tmp/haicode_test_atcap.txt";
    atts.push_back(cap_att);
    engine.submit_prompt(sid, "two files", atts);

    auto msgs = store.load_messages(sid);
    json data = json::parse(msgs[0].data_json, nullptr, false);
    CHECK(data["attachments"].size() == 2, "both rows stored");

    std::string big_dec = haicode::util::base64_decode(
        data["attachments"][0].value("data_b64", ""));
    std::string marker = "\n[truncated: " + std::to_string(big - cap)
                       + " more bytes]";
    CHECK(big_dec.size() == cap + marker.size(),
          "oversized text clamped to cap + marker");
    CHECK(big_dec.compare(0, cap, std::string(cap, 'x')) == 0,
          "first cap bytes retained");
    CHECK(big_dec.compare(cap, marker.size(), marker) == 0,
          "truncation marker appended");

    std::string cap_dec = haicode::util::base64_decode(
        data["attachments"][1].value("data_b64", ""));
    CHECK(cap_dec.size() == cap && cap_dec.find("[truncated:") == std::string::npos,
          "at-cap file passes through untruncated");

    remove("/tmp/haicode_test_big.txt");
    remove("/tmp/haicode_test_atcap.txt");
    std::cout << "[OK] engine truncates oversized text at 256 KB + marker\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_base64_roundtrip();
    ok &= test_text_attachment_becomes_text_block();
    ok &= test_text_attachment_sanitizes_utf8();
    ok &= test_mixed_text_and_image_attachments();
    ok &= test_serialize_history_text_attachment();
    ok &= test_engine_marks_absent_attachments();
    ok &= test_engine_truncates_oversized_text();
    std::cout << (ok ? "ALL PASS\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
