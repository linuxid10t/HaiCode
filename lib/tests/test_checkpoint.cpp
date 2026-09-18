#include <haicode/compaction.h>
#include <haicode/engine.h>
#include <algorithm>
#include <iostream>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using haicode::CompactionCheckpoint;
using haicode::SessionMessage;

static SessionMessage umsg(int seq, const std::string& text) {
    SessionMessage m; m.seq = seq; m.type = "user_prompted";
    m.data_json = nlohmann::json{{"text", text}}.dump();
    return m;
}
static SessionMessage amsg(int seq, const std::string& text,
                           bool with_tools = false) {
    SessionMessage m; m.seq = seq; m.type = "assistant_text";
    nlohmann::json d; d["text"] = text;
    if (with_tools)
        d["tool_calls"] = nlohmann::json::array({
            {{"id", "call-" + std::to_string(seq)}, {"name", "read"},
             {"input", {{"path", "/tmp/f.cpp"}}}}});
    m.data_json = d.dump();
    return m;
}
static SessionMessage tmsg(int seq, const std::string& call_id,
                           const std::string& out, bool ok = true) {
    SessionMessage m; m.seq = seq; m.type = "tool_result";
    m.data_json = nlohmann::json{{"call_id", call_id},
                                 {"output", out}, {"success", ok}}.dump();
    return m;
}

static bool test_usable_input_tokens() {
    CHECK(haicode::usable_input_tokens(100000, 8192, 8192, 0.80) == 80000,
          "0.80 of window should cap at 80000");
    CHECK(haicode::usable_input_tokens(100000, 32000, 8192, 0.95) == 68000,
          "window - max(out, buffer) should cap at 68000");
    CHECK(haicode::usable_input_tokens(0, 8192, 8192, 0.80) == 0,
          "unknown window disables");
    // Regression: buffer >= window used to zero the threshold (compaction
    // silently never fired on small windows).
    CHECK(haicode::usable_input_tokens(8000, 8192, 8192, 0.80) == 4000,
          "window<=buffer must still yield a positive threshold");
    std::cout << "[OK] usable_input_tokens arithmetic\n";
    return true;
}

static bool test_split_history_basic() {
    std::vector<SessionMessage> msgs;
    msgs.push_back(umsg(1, "please always use tabs in this project"));
    msgs.push_back(amsg(2, "ok", true));
    msgs.push_back(tmsg(3, "call-2", "file contents here"));
    msgs.push_back(umsg(4, "now check the build"));
    msgs.push_back(amsg(5, "running build", true));
    msgs.push_back(tmsg(6, "call-5", "build ok"));
    msgs.push_back(amsg(7, "build passes"));

    // Regression: whole-history-fits used to return -1 even though the
    // trigger fired; now the oldest exchange is still summarized.
    CHECK(haicode::split_history(msgs, 100000, 0) == 1,
          "huge budget still summarizes the oldest exchange");

    int ts = haicode::split_history(msgs, 8, 0);
    CHECK(ts == 1 || ts == 3 || ts == 6,
          "boundary lands on a complete exchange (got " + std::to_string(ts) + ")");

    // Regression: rows at/below the floor used to consume recent budget,
    // making the second compaction a permanent no-op.
    CHECK(haicode::split_history(msgs, 8, 6) == -1, "floor at 6 leaves nothing");
    CHECK(haicode::split_history(msgs, 8, 1) >= 3, "floor at 1 advances past it");

    std::vector<SessionMessage> one;
    one.push_back(umsg(1, "only turn"));
    CHECK(haicode::split_history(one, 100000, 0) == -1,
          "single exchange → nothing to summarize");
    std::cout << "[OK] split_history boundaries + floor + budget-skip\n";
    return true;
}

static bool test_split_history_oversized_exchange() {
    std::vector<SessionMessage> msgs;
    std::string huge(200000, 'x');
    msgs.push_back(umsg(1, "start"));
    msgs.push_back(amsg(2, "reading", true));
    msgs.push_back(tmsg(3, "call-2", huge));
    msgs.push_back(amsg(4, "done"));
    int ts = haicode::split_history(msgs, 500, 0);
    CHECK(ts == 1 || ts == 3,
          "oversized exchange never split (got " + std::to_string(ts) + ")");
    std::cout << "[OK] split_history oversized exchange\n";
    return true;
}

static bool test_serialize_history() {
    std::vector<SessionMessage> msgs;
    msgs.push_back(umsg(1, "always use tabs"));
    SessionMessage with_img = umsg(2, "see this");
    {
        nlohmann::json d = nlohmann::json::parse(with_img.data_json);
        d["attachments"] = nlohmann::json::array({{{"path", "screenshot.png"},
            {"media_type", "image/png"}, {"data_b64", "AAAA"}}});
        with_img.data_json = d.dump();
    }
    msgs.push_back(with_img);
    msgs.push_back(amsg(3, "reading file", true));
    msgs.push_back(tmsg(4, "call-3", std::string(20000, 'y'), false));

    std::string s = haicode::serialize_history(msgs, 1024);
    CHECK(s.find("### User") != std::string::npos, "user role present");
    CHECK(s.find("[image attachment: screenshot.png, image/png]")
              != std::string::npos, "attachment rendered as name+type");
    CHECK(s.find("AAAA") == std::string::npos, "no base64 leak");
    CHECK(s.find("[tool_call read id=call-3") != std::string::npos,
          "tool name + id present");
    CHECK(s.find("error") != std::string::npos, "failed tool marked");
    CHECK(s.find("[truncated: ") != std::string::npos, "truncation marker");
    std::cout << "[OK] serialize_history\n";
    return true;
}

static std::string valid_summary() {
    return "## Objective\nShip it.\n\n## Constraints & Decisions\n"
           "Always use tabs.\n\n## Completed Work\nNothing yet.\n\n"
           "## Active Work\nCompaction.\n\n## Blockers\nNone.\n\n"
           "## Next Actions\nTest.\n\n## Relevant Files\nnone\n";
}

static bool test_prompt_and_validation() {
    CHECK(haicode::validate_summary("", 4096) == false, "empty fails");
    std::string missing = valid_summary();
    missing.erase(missing.find("## Blockers"), 12);
    CHECK(!haicode::validate_summary(missing, 4096), "missing section fails");
    CHECK(!haicode::validate_summary(valid_summary(), 10), "oversize fails");
    CHECK(haicode::validate_summary(valid_summary(), 4096), "valid passes");
    // Regression: real summarizers write "and"/lowercase; strict match used
    // to discard good summaries → compaction no-op'd after the retry.
    std::string andv = valid_summary();
    andv.replace(andv.find("Constraints & Decisions"), 8, "Constraints and Decisions");
    CHECK(haicode::validate_summary(andv, 4096), "'and' variant validates");

    std::string p = haicode::build_summary_prompt("PREVSUM", "AGEDCTX", "OLDER");
    CHECK(p.find("PREVSUM") != std::string::npos, "previous summary included");
    CHECK(p.find("AGEDCTX") != std::string::npos, "aged context included");
    CHECK(p.find("merge") != std::string::npos, "merge rule present");
    std::cout << "[OK] build_summary_prompt + validate_summary\n";
    return true;
}

static bool test_block_not_duplicated() {
    // Regression: the block used to re-embed recent_context, double-counting
    // ~10k tokens so post-compaction requests barely shrank.
    CompactionCheckpoint cp;
    cp.id = "ckpt1";
    cp.summary = valid_summary();
    cp.recent_context = "### User\nrecent turn text UNIQUEMARKER";
    std::string b = haicode::render_checkpoint_block(cp);
    CHECK(b.find("UNIQUEMARKER") == std::string::npos,
          "block must not duplicate the retained recent context");
    CHECK(b.find("## Summary of earlier conversation") != std::string::npos,
          "summary section present");
    std::cout << "[OK] checkpoint block does not duplicate recent context\n";
    return true;
}

static bool test_apply_checkpoint() {
    std::vector<SessionMessage> msgs;
    msgs.push_back(umsg(1, "old1"));
    msgs.push_back(amsg(2, "old2"));
    msgs.push_back(umsg(3, "kept1"));
    msgs.push_back(amsg(4, "kept2"));

    CompactionCheckpoint cp;
    cp.id = "ckpt9"; cp.session_id = "s1"; cp.through_seq = 2;
    cp.summary = valid_summary(); cp.recent_context = "tail";

    auto out = haicode::apply_checkpoint(msgs, cp);
    CHECK(out.size() == 3, "checkpoint block + 2 tail rows");
    CHECK(out[0].type == "compaction_summary" && out[0].seq == 0,
          "synthetic block first at seq 0");
    CHECK(out[1].seq == 3 && out[2].seq == 4, "only seq > through_seq kept");

    haicode::ContextBuilder builder;
    auto assembled = builder.assemble_messages(out);
    CHECK(assembled.size() == 3, "three provider messages");
    CHECK(assembled[0].value("role", "") == "assistant", "summary as assistant");
    CHECK(assembled[1].value("role", "") == "user", "then user");
    CHECK(assembled[2].value("role", "") == "assistant", "then assistant");
    std::cout << "[OK] apply_checkpoint + alternation\n";
    return true;
}

static bool test_estimate_tokens() {
    CHECK(haicode::estimate_text_tokens("1234") == 1, "chars/4");
    nlohmann::json img_msg;
    img_msg["role"] = "user";
    img_msg["content"] = nlohmann::json::array({
        {{"type", "image"}, {"source", {{"data", std::string(100000, 'z')}}}}});
    CHECK(haicode::estimate_request_tokens("", "", {img_msg}, {}) < 10000,
          "image counted as bounded vision tokens");
    haicode::ToolDefinition td;
    td.name = "read"; td.description = "read a file";
    td.input_schema = nlohmann::json{{"path", "string"}};
    int with = haicode::estimate_request_tokens("", "", {}, {td});
    int without = haicode::estimate_request_tokens("", "", {}, {});
    CHECK(with > without, "tool schemas counted");
    std::cout << "[OK] estimate_request_tokens / estimate_text_tokens\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_usable_input_tokens();
    ok &= test_split_history_basic();
    ok &= test_split_history_oversized_exchange();
    ok &= test_serialize_history();
    ok &= test_prompt_and_validation();
    ok &= test_block_not_duplicated();
    ok &= test_apply_checkpoint();
    ok &= test_estimate_tokens();
    std::cout << (ok ? "ALL CHECKPOINT TESTS PASSED\n"
                     : "CHECKPOINT TESTS FAILED\n");
    return ok ? 0 : 1;
}
