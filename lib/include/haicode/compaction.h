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
// attachments render as "[image attachment: name, media_type]".
std::string serialize_history(const std::vector<SessionMessage>& msgs,
                              size_t max_tool_output_bytes);

// The fixed template for the summarization request. Merges the previous
// summary (when non-empty) with the aged retained context and the newly
// selected older history.
std::string build_summary_prompt(const std::string& previous_summary,
                                 const std::string& aged_recent_context,
                                 const std::string& serialized_older);

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
