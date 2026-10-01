#include <haicode/engine.h>
#include <haicode/util.h>
#include "test_check.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

using namespace haicode;
using json = nlohmann::json;

static void wait_idle(SessionEngine& engine, const std::string& sid) {
    // 30 s ceiling: test_limits does four sequential turns with 4 MiB
    // base64 work, which is several times slower in unoptimized Debug.
    for (int i = 0; i < 3000 && engine.is_running(sid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_REQUIRE(!engine.is_running(sid), "bounded worker completion");
}

static json image(const std::string& name = "image.png") {
    return {{"kind", "image"}, {"media_type", "image/png"},
        {"path", name}, {"data_b64", util::base64_encode("image bytes")}};
}

static int image_count(const json& value) {
    int count = 0;
    if (value.is_object() && value.value("type", "") == "image") ++count;
    if (value.is_structured())
        for (const auto& item : value) count += image_count(item);
    return count;
}

static SessionMessage row(const std::string& type, const json& data, int seq) {
    SessionMessage msg;
    msg.type = type;
    msg.seq = seq;
    msg.session_id = "diagnostic-session";
    msg.id = "diagnostic-row";
    msg.data_json = data.dump();
    return msg;
}

class FakeProvider : public Provider {
public:
    explicit FakeProvider(std::string name) : name_(std::move(name)) {}
    std::string id() const override { return name_; }
    std::vector<std::string> list_models(std::string&) override { return {}; }
    void cancel(const std::string& token = "") override {
        std::lock_guard<std::mutex> lock(mu);
        cancelled_token = token;
        cancelled = true;
        cv.notify_all();
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lock(mu);
        TEST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }),
            "fallback stream entered");
    }
    void stream(const LLMRequest& req, StreamCallbacks cb, const std::string& token = "") override {
        if (req.system == "You are a precise conversation summarizer.") {
            ++summaries;
            cb.on_text_delta("t", "## Objective\no\n## Constraints & Decisions\nc\n"
                "## Completed Work\nw\n## Active Work\na\n## Blockers\nb\n"
                "## Next Actions\nn\n## Relevant Files\nf\n");
            cb.on_finish(FinishReason::EndTurn, {}, {});
            return;
        }
        if (req.system == "You describe images for a text-only coding assistant.") {
            ++descriptions;
            {
                std::unique_lock<std::mutex> lock(mu);
                describe_token = token;
                entered = true;
                cv.notify_all();
                if (park)
                    TEST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return cancelled; }),
                        "fallback cancelled promptly");
            }
            if (fail_description || park) cb.on_error("description failed");
            else cb.on_text_delta("t", "image description");
            cb.on_finish(FinishReason::EndTurn, {}, {});
            return;
        }
        requests.push_back(req);
        if (overflow && requests.size() == 1) {
            cb.on_error("maximum context exceeded");
            return;
        }
        cb.on_text_delta("t", "done");
        cb.on_finish(FinishReason::EndTurn, {}, {});
    }
    std::string name_;
    bool overflow = false, fail_description = false, park = false;
    int summaries = 0, descriptions = 0;
    std::vector<LLMRequest> requests;
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false, cancelled = false;
    std::string describe_token, cancelled_token;
};

struct Fixture {
    Database db{":memory:"};
    SessionStore store{db};
    ProviderRegistry registry;
    ToolRegistry tools;
    PermissionGate permissions;
    SessionEventBus bus;
    AppConfig config;
    std::shared_ptr<FakeProvider> primary = std::make_shared<FakeProvider>("custom");
    std::shared_ptr<FakeProvider> fallback = std::make_shared<FakeProvider>("vision");
    std::unique_ptr<SessionEngine> engine;
    std::string sid;
    Fixture() {
        db.migrate();
        registry.register_provider(primary);
        config.default_mode = "build";
        config.autoname_sessions = false;
        config.auto_compact = false;
        config.model = "text-model";
        config.provider = "custom";
        config.model_vision["text-model"] = false;
    }
    void start(bool vision_fallback = false) {
        if (vision_fallback) {
            registry.register_provider(fallback);
            config.vision_fallback_provider = "vision";
            config.vision_fallback_model = "vision-model";
        }
        engine = std::make_unique<SessionEngine>(store, registry, tools, permissions, bus, config);
        sid = engine->create_session("/tmp", "build");
        TEST_REQUIRE(!sid.empty(), "custom-only provider creates session");
        TEST_REQUIRE(json::parse(store.get(sid)->model_json)["provider_id"] == "custom",
            "first registered custom provider selected");
    }
    void append_image() {
        store.append_message(sid, "user_prompted",
            json{{"text", "look"}, {"attachments", json::array({image()})}}.dump());
    }
};

static void test_rebuild(bool overflow) {
    Fixture fx;
    fx.config.auto_compact = !overflow;
    fx.config.model_contexts["text-model"] = 2000;
    fx.primary->overflow = overflow;
    fx.start();
    for (int i = 0; i < 4; ++i) {
        fx.store.append_message(fx.sid, "user_prompted", json{{"text", "old turn"}}.dump());
        fx.store.append_message(fx.sid, "assistant_text", json{{"text", "old reply"}}.dump());
    }
    fx.append_image();
    fx.store.append_message(fx.sid, "assistant_text", json{{"text", "prior image reply"}}.dump());
    InferenceParams params;
    params.max_tokens = 777;
    params.has_temperature = true;
    params.temperature = 0.25;
    params.has_top_p = true;
    params.top_p = 0.8;
    params.reasoning_effort = "low";
    fx.engine->update_inference(fx.sid, params);
    fx.engine->submit_prompt(fx.sid, "current prompt");
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(fx.primary->summaries > 0, "rebuild performed checkpoint compaction");
    TEST_REQUIRE(fx.store.latest_complete_checkpoint(fx.sid).has_value(), "checkpoint committed");
    TEST_REQUIRE(fx.primary->requests.size() == (overflow ? 2 : 1), "expected conversation attempts");
    const auto& req = fx.primary->requests.back();
    TEST_REQUIRE(image_count(req.messages) == 0, "text-only rebuilt request excludes raw images");
    TEST_REQUIRE(req.max_tokens == 777 && req.temperature == 0.25 && req.top_p == 0.8
        && req.reasoning_effort == "low", "rebuild preserves inference overrides");
    TEST_REQUIRE(json(req.messages).dump().find("image attachment") != std::string::npos,
        "recent image retained as placeholder after compaction");
}

static void test_fallback() {
    Fixture fx;
    fx.fallback->fail_description = true;
    fx.start(true);
    fx.append_image();
    fx.engine->continue_session(fx.sid);
    wait_idle(*fx.engine, fx.sid);
    fx.engine->continue_session(fx.sid);
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(fx.fallback->descriptions == 1, "failed description tried once across steps");
    auto data = json::parse(fx.store.load_messages(fx.sid)[0].data_json);
    TEST_REQUIRE(data["attachments"][0]["description_status"] == "failed", "failure durable");
    fx.engine.reset();
    fx.engine = std::make_unique<SessionEngine>(fx.store, fx.registry, fx.tools,
        fx.permissions, fx.bus, fx.config);
    fx.engine->continue_session(fx.sid);
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(fx.fallback->descriptions == 1, "failure not retried after reopen");
}

static void test_fallback_cancel() {
    Fixture fx;
    fx.fallback->park = true;
    fx.start(true);
    fx.append_image();
    fx.engine->continue_session(fx.sid);
    fx.fallback->wait_entered();
    fx.engine->interrupt(fx.sid);
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(!fx.fallback->describe_token.empty()
        && fx.fallback->cancelled_token == fx.fallback->describe_token, "fallback scoped cancellation");
    TEST_REQUIRE(fx.primary->requests.empty(), "interrupted backfill never starts primary");
    auto data = json::parse(fx.store.load_messages(fx.sid)[0].data_json);
    TEST_REQUIRE(!data["attachments"][0].contains("description_status"), "cancel stays retryable");
    fx.fallback->park = false;
    fx.engine->continue_session(fx.sid);
    wait_idle(*fx.engine, fx.sid);
    data = json::parse(fx.store.load_messages(fx.sid)[0].data_json);
    TEST_REQUIRE(fx.fallback->descriptions == 2
        && data["attachments"][0]["description"] == "image description", "cancelled image retried");
}

static void test_aging_and_parse() {
    std::vector<SessionMessage> rows;
    for (int i = 0; i < 3; ++i) {
        rows.push_back(row("user_prompted", {{"text", "turn"},
            {"attachments", json::array({image("user" + std::to_string(i))})}}, i * 3));
        rows.push_back(row("assistant_text", {{"tool_calls", json::array({
            {{"id", "c" + std::to_string(i)}, {"name", "screenshot"}}})}}, i * 3 + 1));
        rows.push_back(row("tool_result", {{"call_id", "c" + std::to_string(i)},
            {"attachments", json::array({image("shot" + std::to_string(i))})}}, i * 3 + 2));
    }
    ContextBuilder builder;
    auto before = rows;
    auto messages = builder.assemble_messages(rows);
    TEST_REQUIRE(image_count(messages) == 4, "only current and preceding user/shot images retained");
    TEST_REQUIRE(json(messages).dump().find("image attachment: user0") != std::string::npos,
        "expired image placeholder");
    TEST_REQUIRE(rows[0].data_json == before[0].data_json, "aging leaves persisted payload unchanged");
    FILE* capture = tmpfile();
    TEST_REQUIRE(capture, "capture diagnostics");
    int saved = dup(STDERR_FILENO);
    TEST_REQUIRE(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0, "redirect diagnostics");
    rows[2].data_json = "{SECRET_IMAGE_PAYLOAD";
    messages = builder.assemble_messages(rows);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    rewind(capture);
    char buffer[2048]{};
    size_t bytes = fread(buffer, 1, sizeof(buffer), capture);
    fclose(capture);
    std::string log(buffer, bytes);
    TEST_REQUIRE(log.find("diagnostic-session") != std::string::npos
        && log.find("diagnostic-row") != std::string::npos && log.find("seq=2") != std::string::npos,
        "malformed row identified");
    TEST_REQUIRE(log.find("SECRET_IMAGE_PAYLOAD") == std::string::npos, "payload never logged");
    TEST_REQUIRE(messages[2]["content"][0]["is_error"] == true, "malformed result repaired");
}

static void test_expired_backfill() {
    Fixture fx;
    fx.start(true);
    fx.append_image();
    fx.store.append_message(fx.sid, "assistant_text", json{{"text", "reply"}}.dump());
    fx.store.append_message(fx.sid, "user_prompted", json{{"text", "next"}}.dump());
    fx.store.append_message(fx.sid, "assistant_text", json{{"text", "reply"}}.dump());
    fx.engine->submit_prompt(fx.sid, "current");
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(fx.fallback->descriptions == 0, "expired image not described unnecessarily");
    auto data = json::parse(fx.store.load_messages(fx.sid)[0].data_json);
    TEST_REQUIRE(data["attachments"][0]["data_b64"] == image()["data_b64"], "DB bytes intact");
}

static void test_limits() {
    Fixture fx;
    fx.start();
    constexpr size_t cap = 4 * 1024 * 1024;
    char path[] = "/tmp/haicode-image-limit-XXXXXX";
    int fd = mkstemp(path);
    TEST_REQUIRE(fd >= 0, "image scratch file");
    std::string raw(cap + 1, 'x');
    TEST_REQUIRE(write(fd, raw.data(), raw.size()) == static_cast<ssize_t>(raw.size()), "write image");
    close(fd);
    Attachment at;
    at.kind = "image";
    at.media_type = "image/png";
    at.path = path;
    fx.engine->submit_prompt(fx.sid, "oversize path", {at});
    wait_idle(*fx.engine, fx.sid);
    at.path.clear();
    at.data_b64 = util::base64_encode(raw);
    fx.engine->submit_prompt(fx.sid, "oversize base64", {at});
    wait_idle(*fx.engine, fx.sid);
    raw.resize(cap);
    at.data_b64 = util::base64_encode(raw);
    fx.engine->submit_prompt(fx.sid, "boundary base64", {at});
    wait_idle(*fx.engine, fx.sid);
    fd = open(path, O_WRONLY);
    TEST_REQUIRE(fd >= 0 && ftruncate(fd, cap) == 0, "boundary path");
    close(fd);
    at.data_b64.clear();
    at.path = path;
    fx.engine->submit_prompt(fx.sid, "boundary path", {at});
    wait_idle(*fx.engine, fx.sid);
    unlink(path);
    int users = 0;
    for (const auto& r : fx.store.load_messages(fx.sid)) {
        if (r.type != "user_prompted") continue;
        auto data = json::parse(r.data_json);
        const auto& attachment = data["attachments"][0];
        if (users++ < 2) {
            TEST_REQUIRE(attachment["absent"] == true && attachment.contains("unavailable_reason")
                && !attachment.contains("data_b64"), "oversize image explicit marker without bytes");
        } else {
            TEST_REQUIRE(util::base64_decode(attachment["data_b64"]).size() == cap,
                "image at limit retained");
        }
    }
    TEST_REQUIRE(users == 4, "all image submissions recorded");
}

static void test_no_provider() {
    Fixture fx;
    ProviderRegistry empty;
    SessionEngine engine(fx.store, empty, fx.tools, fx.permissions, fx.bus, fx.config);
    bool failed = false;
    fx.bus.subscribe(events::EventType::StepFailed, [&](const json& data) {
        failed = data.value("error", "").find("none registered") != std::string::npos;
    });
    TEST_REQUIRE(engine.create_session("/tmp").empty() && failed, "clear no-provider error");
}

int main() {
    test_rebuild(false);
    test_rebuild(true);
    test_fallback();
    test_fallback_cancel();
    test_aging_and_parse();
    test_expired_backfill();
    test_limits();
    test_no_provider();
    std::puts("engine correctness tests passed");
}
