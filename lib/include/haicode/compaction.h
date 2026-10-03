#pragma once
#include "db.h"
#include "provider.h"
#include <string>
#include <vector>

namespace haicode {

// CompactionCheckpoint lives in db.h with the other storage types.

// Token budget arithmetic: the largest request input that still counts as
// "fits". min(floor(window * threshold_frac), window - max(output_allowance, buffer)),
// except the reserve never exceeds half the window (a buffer larger than the
// window would otherwise zero the threshold and silently disable compaction).
// Returns 0 when window <= 0 (unknown window disables compaction).
int usable_input_tokens(int window, int output_allowance, int buffer,
                        double threshold_frac);

// Split the conversation into an older slice (to summarize) and a recent slice
// (to retain), walking backward over complete turns / tool exchanges until
// recent_budget_tokens is spent. Returns the seq of the LAST message in the
// older slice (== through_seq), or -1 when there is nothing new worth
// summarizing. prev_through_seq is a floor: rows at or below it are already
// summarized and are skipped without spending budget. When the whole history
// fits the budget but two or more exchanges exist, the oldest exchange is
// still kept for summarization (the trigger fired for a reason). An exchange
// that alone exceeds the budget is never split.
int split_history(const std::vector<SessionMessage>& msgs,
                  int recent_budget_tokens,
                  int prev_through_seq);

// Serialize stored messages into readable text for the summarizer and for the
// retained-recent-context measurement. Preserves roles, tool names, arguments,
// outputs and errors. Tool outputs longer than max_tool_output bytes are
// truncated with an explicit "[truncated: N more bytes]" marker; image
// attachments render as "[image attachment: path, media_type]", text
// attachments as "[text attachment: path]", and unreadable/empty ones as
// "[attachment unavailable: path]".
std::string serialize_history(const std::vector<SessionMessage>& msgs,
                              size_t max_tool_output_bytes);

// The fixed template for the summarization request. Merges the previous
// summary (when non-empty) with the aged retained context and the newly
// selected older history.
std::string build_summary_prompt(const std::string& previous_summary,
                                 const std::string& aged_recent_context,
                                 const std::string& serialized_older);

// Inline (cache-friendly) summarization. Instead of a standalone prompt that
// re-sends the history as serialized text — a request sharing no prefix with
// the conversation, so every prompt cache (llama.cpp KV, Anthropic/OpenAI
// prompt caching) misses and the whole history is re-processed — the engine
// sends the conversation's own next request with this instruction appended
// as the final user text. The model summarizes the ENTIRE visible
// conversation (earlier checkpoint summary included); the engine still keeps
// a shorter verbatim tail after the checkpoint, so that tail is covered twice
// by design. The instruction starts with kInlineSummaryMarker so tests (and
// logs) can recognize the request.
inline constexpr const char* kInlineSummaryMarker = "[CONTEXT COMPACTION REQUEST]";
std::string build_inline_summary_instruction();

// Corrective follow-up for an inline summary that failed validate_summary:
// re-states the required headings. Sent as a new user turn after the
// invalid attempt (itself appended as an assistant turn), so the cached
// prefix still applies.
std::string build_inline_summary_correction();

// True when the request's final message is a user turn whose text carries
// kInlineSummaryMarker — i.e. an inline summarization call (first attempt
// or corrective retry), not an agentic step.
bool is_inline_summary_request(const LLMRequest& request);

// Single source of truth for the section headings validate_summary expects.
std::vector<std::string> required_summary_sections();

// Format validation only (not factual completeness): non-empty, contains
// every required section heading (case-insensitive, "&" matches "and"), and
// fits the summary token budget.
bool validate_summary(const std::string& summary, int max_tokens);

// The context-facing text for a completed checkpoint: labels its content as
// historical conversation. The retained recent context is deliberately NOT
// rendered here — those rows are still live in the request, and re-embedding
// them would double-count ~10k tokens (compaction that shrinks nothing).
std::string render_checkpoint_block(const CompactionCheckpoint& cp);

// Rough token estimate (~4 chars/token) for a whole request, including tool
// schemas. Image blocks count as a bounded number of vision tokens instead
// of raw base64 chars.
int estimate_request_tokens(const std::string& system,
                            const std::string& system_dynamic,
                            const std::vector<nlohmann::json>& messages,
                            const std::vector<ToolDefinition>& tools);

// Rough token estimate for plain text (chars/4).
int estimate_text_tokens(const std::string& text);

// Checkpoint-aware context slice: the synthetic compaction_summary row
// carrying the rendered checkpoint block, followed by only the records with
// seq > through_seq. Pure — engine and tests share it.
std::vector<SessionMessage> apply_checkpoint(
    const std::vector<SessionMessage>& msgs,
    const CompactionCheckpoint& cp);

} // namespace haicode
