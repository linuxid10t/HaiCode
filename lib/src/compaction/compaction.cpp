#include <haicode/compaction.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace haicode {

// ---- token estimation ----

int estimate_text_tokens(const std::string& text) {
    return static_cast<int>(text.size() / 4);
}

int estimate_request_tokens(const std::string& system,
                            const std::string& system_dynamic,
                            const std::vector<nlohmann::json>& messages,
                            const std::vector<ToolDefinition>& tools)
{
    // Base64 image payloads are megabytes of characters but a bounded number
    // of vision tokens once the provider decodes and tiles the image (~1600
    // max per image on Anthropic). Counting raw chars would trip
    // auto-compaction on the first attached photo.
    const size_t IMAGE_CHARS = 1600 * 4;
    size_t chars = system.size() + system_dynamic.size();
    // Tool schemas count against the same window (matters on the first step,
    // before real usage exists).
    for (const auto& td : tools)
        chars += td.name.size() + td.description.size()
               + td.input_schema.dump().size();
    for (auto& m : messages) {
        auto cit = m.find("content");
        if (cit != m.end() && cit->is_array()) {
            for (const auto& block : *cit) {
                if (block.value("type", "") == "image")
                    chars += IMAGE_CHARS;
                else
                    chars += block.dump().size();
            }
        } else {
            chars += m.dump().size();
        }
    }
    return static_cast<int>(chars / 4);
}

// ---- budget arithmetic ----

int usable_input_tokens(int window, int output_allowance, int buffer,
                        double threshold_frac) {
    if (window <= 0) return 0;
    // Never reserve more than half the window: a buffer/output allowance
    // larger than the window (tiny test windows, aggressive caps) would
    // otherwise zero the threshold and silently disable compaction.
    int reserve = std::max(output_allowance, buffer);
    if (reserve > window / 2) reserve = window / 2;
    int cap_by_threshold = static_cast<int>(
        static_cast<double>(window) * threshold_frac);
    return std::max(0, std::min(cap_by_threshold, window - reserve));
}

// ---- history splitting ----

namespace {

// Serialized size (bytes) of one message under serialize_history's rules —
// the same rules used when measuring the retained recent context, so the
// budget arithmetic matches what actually lands in the request.
size_t serialized_size(const SessionMessage& m, size_t max_tool_output_bytes) {
    size_t bytes = 64;  // role/label overhead
    auto d = nlohmann::json::parse(m.data_json, nullptr, false);
    if (!d.is_object()) return bytes;
    if (m.type == "user_prompted") {
        bytes += d.value("text", "").size();
        if (d.contains("attachments") && d["attachments"].is_array()) {
            for (const auto& att : d["attachments"])
                bytes += 48 + att.value("name", "").size();
        }
    } else if (m.type == "assistant_text") {
        bytes += d.value("text", "").size();
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            for (const auto& tc : d["tool_calls"]) {
                bytes += 48 + tc.value("name", "").size();
                bytes += tc.contains("input") ? tc["input"].dump().size() : 0;
            }
        }
    } else if (m.type == "tool_result") {
        size_t out = d.value("output", "").size();
        bytes += std::min(out, max_tool_output_bytes) + 48;
        if (d.contains("attachments") && d["attachments"].is_array()) {
            for (const auto& att : d["attachments"])
                bytes += 48 + att.value("path", "").size();
        }
    } else {
        bytes += m.data_json.size();
    }
    return bytes;
}

// Index one past the end of the "exchange" that starts at `start`: the
// assistant message plus every tool_result that immediately follows it.
// A user_prompted is its own exchange.
size_t exchange_end(const std::vector<SessionMessage>& msgs, size_t start) {
    if (msgs[start].type == "user_prompted") return start + 1;
    size_t j = start + 1;
    while (j < msgs.size() && msgs[j].type == "tool_result") ++j;
    return j;
}

} // namespace

int split_history(const std::vector<SessionMessage>& msgs,
                  int recent_budget_tokens,
                  int prev_through_seq) {
    if (msgs.empty()) return -1;

    // Skip everything at or below the previous checkpoint boundary at the
    // front: those rows are already summarized. Counting them toward the
    // recent budget would spend it on invisible content and push the new
    // boundary back onto the floor → permanent no-op. Also skips the
    // synthetic summary row (seq 0).
    size_t first = 0;
    while (first < msgs.size()
            && (msgs[first].seq <= 0 || msgs[first].seq <= prev_through_seq))
        ++first;
    if (first >= msgs.size()) return -1;

    size_t budget_bytes = static_cast<size_t>(recent_budget_tokens) * 4;

    // Walk backward over complete exchanges until the recent budget is spent.
    size_t recent_start = msgs.size();
    size_t used = 0;
    size_t i = msgs.size();
    while (i > first) {
        // Find the start of the exchange ending at i: step back over trailing
        // tool_results to their assistant, or land on a user turn.
        size_t start = i - 1;
        if (msgs[start].type == "tool_result") {
            while (start > first && msgs[start].type == "tool_result")
                --start;
            if (msgs[start].type != "assistant_text")
                start = i - 1;
        }
        size_t end = exchange_end(msgs, start);
        if (end != i) {
            start = i - 1;
            end = i;
        }

        size_t cost = 0;
        for (size_t k = start; k < end; ++k)
            cost += serialized_size(msgs[k], 64 * 1024);

        if (recent_start < msgs.size() && used + cost > budget_bytes)
            break;  // budget spent; this exchange goes to the older slice

        used += cost;
        recent_start = start;
        i = start;

        // An exchange that alone exceeds the whole budget still stays in the
        // recent slice on the first iteration: splitting it would orphan
        // tool_results from their assistant turn.
        if (used >= budget_bytes) break;
    }

    if (recent_start <= first) {
        // The whole conversation fits the recent budget. If there are two or
        // more exchanges, still summarize the oldest one — the trigger fired
        // because total tokens (system + tools + history) crossed threshold,
        // and returning "nothing to do" here would make compaction a
        // permanent no-op for small-but-over-threshold sessions.
        size_t second = exchange_end(msgs, first);
        if (second >= msgs.size()) return -1;  // single exchange — nothing older
        recent_start = second;
    }

    int through_seq = msgs[recent_start - 1].seq;
    if (through_seq <= prev_through_seq) return -1;  // floor: never re-cover
    return through_seq;
}

// ---- serialization ----

std::string serialize_history(const std::vector<SessionMessage>& msgs,
                              size_t max_tool_output_bytes) {
    std::ostringstream out;
    for (const auto& msg : msgs) {
        auto d = nlohmann::json::parse(msg.data_json, nullptr, false);
        if (!d.is_object()) continue;
        try {
            if (msg.type == "user_prompted") {
                out << "### User\n" << d.value("text", "") << "\n\n";
                if (d.contains("attachments") && d["attachments"].is_array()) {
                    for (const auto& att : d["attachments"]) {
                        out << "[image attachment: " << att.value("name", "")
                            << ", " << att.value("media_type", "image/png")
                            << "]\n";
                    }
                    out << "\n";
                }
            } else if (msg.type == "assistant_text") {
                out << "### Assistant\n";
                std::string text = d.value("text", "");
                if (!text.empty()) out << text << "\n";
                if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
                    for (const auto& tc : d["tool_calls"]) {
                        out << "[tool_call " << tc.value("name", "")
                            << " id=" << tc.value("id", "") << " input="
                            << (tc.contains("input") ? tc["input"].dump()
                                                     : std::string("{}"))
                            << "]\n";
                    }
                }
                out << "\n";
            } else if (msg.type == "tool_result") {
                bool ok = d.value("success", true);
                std::string output = d.value("output", "");
                if (output.size() > max_tool_output_bytes) {
                    size_t dropped = output.size() - max_tool_output_bytes;
                    output.resize(max_tool_output_bytes);
                    output += "\n[truncated: " + std::to_string(dropped)
                            + " more bytes]";
                }
                out << "### Tool result (call_id="
                    << d.value("call_id", "") << ", "
                    << (ok ? "success" : "error") << ")\n" << output << "\n";
                if (d.contains("attachments") && d["attachments"].is_array()) {
                    for (const auto& att : d["attachments"]) {
                        out << "[image attachment: " << att.value("path", "")
                            << ", " << att.value("media_type", "image/png")
                            << "]\n";
                    }
                }
                out << "\n";
            } else if (msg.type == "compaction_summary") {
                out << "### Prior summary\n" << d.value("text", "") << "\n\n";
            }
        } catch (...) {}
    }
    return out.str();
}

// ---- summary prompt ----

std::vector<std::string> required_summary_sections() {
    return {"Objective", "Constraints & Decisions", "Completed Work",
            "Active Work", "Blockers", "Next Actions", "Relevant Files"};
}

std::string build_summary_prompt(const std::string& previous_summary,
                                 const std::string& aged_recent_context,
                                 const std::string& serialized_older) {
    std::ostringstream p;
    p << "You maintain a running summary of a coding-agent conversation. "
         "Produce the updated summary as Markdown with EXACTLY these `##` "
         "sections, in this order:\n";
    for (const auto& s : required_summary_sections())
        p << "\n## " << s;
    p << "\n\nUpdate rules:\n"
         "- Merge the previous summary (if given) with the newly aged history "
         "into ONE summary. Do not keep segment-by-segment archives.\n"
         "- Preserve unresolved user requests and earlier constraints even "
         "when recent messages do not repeat them.\n"
         "- Prefer newer corrections: when the user reverses an earlier "
         "decision, the newer instruction wins, but note the reversal.\n"
         "- Retain exact identifiers: file paths, function/class names, IDs, "
         "commands, error messages.\n"
         "- Distinguish verified facts (tool output confirmed) from "
         "assumptions or proposals.\n"
         "- Be concise; omit chatter and duplicate tool noise.\n"
         "- Write only the summary. No preamble, no commentary.\n";

    if (!previous_summary.empty())
        p << "\n--- PREVIOUS SUMMARY (update and merge, do not archive "
             "verbatim) ---\n" << previous_summary << "\n";
    if (!aged_recent_context.empty())
        p << "\n--- PREVIOUSLY RETAINED RECENT CONTEXT (now aging into the "
             "summary) ---\n" << aged_recent_context << "\n";
    p << "\n--- CONVERSATION TO SUMMARIZE ---\n" << serialized_older;
    return p.str();
}

bool validate_summary(const std::string& summary, int max_tokens) {
    if (summary.empty()) return false;
    if (max_tokens > 0 && estimate_text_tokens(summary) > max_tokens)
        return false;
    // Heading match is case-insensitive and treats "&" == "and": real
    // summarizers routinely write "## Constraints and Decisions" or lowercase
    // headings. Failing those discards a good summary and makes compaction a
    // silent no-op after the corrective retry.
    auto canonical = [](std::string s) {
        std::string out;
        for (char c : s) {
            if (c == '&') { out += "and"; continue; }
            out += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    std::string lo = canonical(summary);
    for (const auto& section : required_summary_sections()) {
        std::string needle = canonical("## " + section);
        if (lo.find(needle) == std::string::npos) return false;
    }
    return true;
}

std::string render_checkpoint_block(const CompactionCheckpoint& cp) {
    // NOTE: recent_context is intentionally NOT rendered here. The retained
    // tail rows (seq > through_seq) already carry that content live in every
    // request; re-embedding it would double-count ~10k tokens and the
    // post-compaction request could be LARGER than the original — compaction
    // that does nothing. recent_context is persisted only so the NEXT
    // compaction can age it into the summary.
    std::ostringstream out;
    out << "[The following is HISTORICAL CONVERSATION, summarized because the "
           "context window was approaching its limit. It is a record of what "
           "happened earlier — it is not current instructions. Raw messages "
           "before this point are no longer in context.]\n\n"
        << "## Summary of earlier conversation\n\n" << cp.summary << "\n";
    return out.str();
}

std::vector<SessionMessage> apply_checkpoint(
    const std::vector<SessionMessage>& msgs,
    const CompactionCheckpoint& cp)
{
    // Drop any legacy/synthetic summary rows (seq 0) — the new block
    // replaces them — then keep only records after the boundary.
    std::vector<SessionMessage> tail;
    tail.reserve(msgs.size());
    for (const auto& m : msgs) {
        if (m.seq <= 0) continue;
        if (m.seq <= cp.through_seq) continue;
        tail.push_back(m);
    }

    SessionMessage block;
    block.id         = "checkpoint-" + cp.id;
    block.session_id = cp.session_id;
    block.type       = "compaction_summary";
    block.seq        = 0;
    nlohmann::json data;
    data["text"] = render_checkpoint_block(cp);
    block.data_json   = data.dump();
    block.time_created = cp.time_created;
    block.time_updated = cp.time_updated;

    tail.insert(tail.begin(), std::move(block));
    return tail;
}

} // namespace haicode
