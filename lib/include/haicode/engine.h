#pragma once
#include "db.h"
#include "events.h"
#include "provider.h"
#include "tool.h"
#include "config.h"
#include <string>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <utility>

namespace haicode {

namespace detail {
std::pair<int, std::string> run_build_hook(const std::string& command,
                                           const std::string& directory,
                                           int timeout_sec,
                                           const std::atomic<bool>* interrupted);
}

class ContextBuilder {
public:
    LLMRequest build(const std::vector<SessionMessage>& messages,
                     const std::string& system_prompt,
                     const std::string& system_dynamic,
                     const std::vector<ToolDefinition>& tools,
                     const std::string& model_id,
                     const std::string& provider_id,
                     bool model_accepts_images = true);

    // Assemble stored SessionMessages into the provider-facing message list.
    // Public so SessionEngine::compact_history can build the head slice for a
    // summarization call without going through the full build() path.
    // model_accepts_images=false swaps image blocks for their persisted
    // vision-fallback descriptions (or a placeholder when none exists) so
    // text-only primaries never receive raw image data.
    std::vector<nlohmann::json> assemble_messages(const std::vector<SessionMessage>& msgs,
                                                  bool model_accepts_images = true);
};

class SessionEngine {
public:
    SessionEngine(SessionStore& store,
                  ProviderRegistry& providers,
                  ToolRegistry& tools,
                  PermissionGate& permissions,
                  SessionEventBus& bus,
                  const AppConfig& config);
    ~SessionEngine();
    // Stop and join workers while event recipients still reference this engine.
    // Safe to call before destruction; the destructor then does nothing.
    void shutdown();

    std::string create_session(const std::string& project_dir,
                                const std::string& agent_id = "",
                                const std::string& model_id = "",
                                const std::string& provider_id = "");

    void submit_prompt(const std::string& session_id, const std::string& text);
    // Submit with attachments (images and text files). The engine reads each
    // file, base64-encodes it, and persists the payload inside the
    // user_prompted row so session replay survives the source file being
    // moved or deleted. Unreadable or empty attachments are persisted as
    // absent-marker rows (logged to stderr) so the transcript records that
    // something was attached instead of silently dropping it.
    void submit_prompt(const std::string& session_id, const std::string& text,
                       const std::vector<Attachment>& attachments);
    // Resume the agentic loop without adding a new user message (used after plan approval).
    void continue_session(const std::string& session_id);
    // Re-run the last turn: delete every message after the most recent
    // user_prompted row (assistant output, tool calls, tool results) and
    // restart the agentic loop on that stored prompt. Attachments, skill
    // blocks, and mode notices ride the user_prompted row, so context
    // assembly re-applies them automatically. No-op when the session has no
    // user_prompted row or the agentic loop is already running (same contract
    // as compact_now). Cost/token totals are not rolled back.
    void retry_last_turn(const std::string& session_id);
    // Append a user_prompted message + publish Prompted event without starting
    // the agentic loop. Used by approval handlers between set_mode and
    // continue_session to inject the plan-approved directive. An explicitly
    // injected message supersedes any queued mode-change notice, which is
    // cleared here.
    void inject_message(const std::string& session_id, const std::string& text);
    void interrupt(const std::string& session_id);

    // True while the session's agentic loop is executing. Lets the UI restore
    // the running/interrupt state when switching back to a background session.
    bool is_running(const std::string& session_id);

    // Per-session Plan/Build mode. Persisted into model_json so it survives
    // process restarts; also cached in session_modes_ for synchronous reads.
    void      set_mode(const std::string& session_id, SessionMode mode);
    SessionMode get_mode(const std::string& session_id);

    // Patch the active session's stored provider/model. Either string may be
    // empty to leave that field untouched. Takes effect on the next prompt.
    void update_provider_model(const std::string& session_id,
                               const std::string& provider_id,
                               const std::string& model_id);

    // Patch the active session's stored inference params (max_tokens,
    // temperature, top_p, max_steps, reasoning_effort). Persisted into
    // model_json; applied to the LLMRequest on the next step.
    void update_inference(const std::string& session_id,
                          const InferenceParams& params);

    // Read the current persisted todo list for a session (used by UIs on
    // session switch). Delegates to SessionStore::load_todos.
    std::vector<Todo> get_todos(const std::string& session_id);

    // Seed todos directly (e.g. from plan approval). Replaces the session's
    // todo list and publishes TodoUpdated so the UI refreshes immediately.
    void seed_todos(const std::string& session_id,
                    const std::vector<Todo>& todos);

    // Manually trigger compaction on a session, regardless of token count.
    // Runs compact_history on a background thread. No-op if the agentic loop
    // is already running for the session (to avoid DB contention).
    void compact_now(const std::string& session_id);

    // Supply an answer to a pending ask_user question. Unblocks the agentic
    // loop that called ask_user. Called by the GUI event handler.
    void reply_to_ask(const std::string& session_id,
                      const std::string& call_id,
                      const std::string& answer);

    // Cancel pending ask_user questions: mark each one replied with an
    // "(interrupted)" placeholder and wake the agentic-loop threads blocked
    // in asking_cv_.wait. Called by the GUI before swapping the engine
    // mid-run and by ~SessionEngine() before joining runner threads.
    // The no-arg form cancels every session; the session_id form cancels
    // only that session's asks (used by interrupt() so stopping one session
    // doesn't answer another session's open question).
    void cancel_pending_asks();
    void cancel_pending_asks(const std::string& session_id);

    const AppConfig& config() const { return config_; }

    // Read-only access to the provider registry (used by the UI to look up
    // discovered context windows for the context meter).
    ProviderRegistry& providers() { return providers_; }

private:
    void agentic_loop(const std::string& session_id);

    // Entry point for the std::thread runners spawned by submit_prompt /
    // continue_session / retry_last_turn: agentic_loop plus an exception
    // barrier. An exception escaping the loop must never propagate out of
    // the thread (std::terminate → abort — the crash this guards against);
    // it is logged and surfaced to the UI as a failed step instead, and the
    // running flag is always cleared.
    void runner_main(const std::string& session_id);

    // Load the provider-facing message list through the single checkpoint-
    // aware path: full stored history, then — if a completed checkpoint
    // exists — sliced to seq > through_seq with the rendered checkpoint
    // block prepended as a synthetic compaction_summary row.
    std::vector<SessionMessage> load_context_messages(const std::string& session_id);

    // Checkpoint compaction: summarize records seq <= split_history(...) into
    // a new checkpoint (pending → complete). Non-destructive — rows stay in
    // the DB; only context assembly changes. Returns true when a checkpoint
    // was committed (caller must rebuild the request). On failure/interrupt
    // the checkpoint is marked failed and the previous boundary stays active.
    bool compact_history(const std::string& session_id,
                         Provider& provider,
                         const std::string& model_id,
                         const std::string& provider_id,
                         std::atomic<bool>* interrupt_flag,
                         int prev_input_tokens,
                         int threshold_tokens,
                         const std::string& stream_token = "");

    // One-shot LLM call that produces a concise (≤6-word) session title from
    // the conversation's user prompts. On the first call (turn 1) it generates
    // a fresh title; on later calls (turns 6, 11, …) it reconsiders — shown the
    // current title and the prompt history, the model either repeats it verbatim
    // (no write) or returns a revised title. Replaces the title via update_title
    // and publishes SessionRenamed. Best-effort: any error is logged to stderr
    // and the existing title is left in place.
    void refine_title_llm(const std::string& session_id,
                          Provider& provider,
                          const std::string& model_id,
                          const std::string& stream_token = "");

    // Vision fallback: true when a usable fallback (provider registered +
    // model vision-capable per model_supports_vision) is configured. Warns
    // once on stderr when the configured fallback itself isn't vision-capable
    // (treated as not ready — fail-safe).
    bool vision_fallback_ready();

    // One-shot describer call: sends the image to the fallback model and
    // returns its text description ("" on failure). Same synchronous
    // provider.stream pattern as refine_title_llm; runs on the engine's
    // worker thread only.
    std::string describe_image(Provider& provider,
                               const std::string& model_id,
                               const nlohmann::json& att,
                               const std::string& stream_token = "");

    // Describe-once backfill: when the session's primary model is text-only
    // and a vision fallback is configured, find every persisted attachment
    // (user_prompted and tool_result rows) lacking a "description" key and
    // fill it via describe_image, persisting through update_message_data and
    // updating the in-memory copies so this step's request sees them.
    // Failures are skipped individually (placeholder rendering covers them).
    void backfill_attachment_descriptions(const std::string& session_id,
                                          std::vector<SessionMessage>& messages);

    SessionStore& store_;
    ProviderRegistry& providers_;
    ToolRegistry& tools_;
    PermissionGate& permissions_;
    SessionEventBus& bus_;
    AppConfig config_;

    std::map<std::string, std::thread> runner_threads_;
    std::map<std::string, std::atomic<bool>*> interrupt_flags_;
    std::map<std::string, bool> session_running_;  // true while agentic_loop is executing
    std::map<std::string, SessionMode> session_modes_;
    std::map<std::string, std::shared_ptr<Provider>> session_providers_;
    // Per-run stream token currently registered for a session (see
    // Provider::stream/cancel). A token is present only while that session's
    // agentic_loop or compact_now worker is between run_token mint/erase;
    // interrupt() cancels exactly this token, so two sessions sharing one
    // Provider object no longer cancel each other's in-flight streams.
    // Guarded by mu_.
    std::map<std::string, std::string> session_stream_tokens_;
    uint64_t next_run_seq_ = 0;  // token uniqueness; guarded by mu_
    // Single-flight guard: ids with a compaction attempt in flight.
    std::set<std::string> compaction_in_progress_;
    // Step number at which the most recent successful compaction fired, per
    // session. Negative = "never compacted this turn". Reset to -1 in
    // submit_prompt so each new user turn rearms the trigger. Guarded by mu_.
    std::map<std::string, int> last_compaction_step_;
    // Mode-change notice queued by set_mode, flushed as metadata on the next
    // user_prompted row by submit_prompt. Only the last flip before a send
    // survives; never persisted as its own message row. Guarded by mu_.
    std::map<std::string, std::string> pending_mode_notice_;
    std::mutex mu_;

    // Track pending ask_user questions per session. The agentic_loop blocks on
    // asking_cv_ after publishing AskUserRequested; reply_to_ask() sets the
    // answer and wakes the cv.
    struct PendingAsk {
        std::string session_id;
        std::string call_id;
        std::string question;
        std::vector<std::string> options;
        std::string answer;
        bool replied = false;
    };
    std::map<std::string, PendingAsk> pending_ask_;
    std::mutex ask_mu_;
    std::condition_variable asking_cv_;

    // Shutdown state, guarded by mu_. Set by ~SessionEngine() before it
    // starts joining; submit_prompt/continue_session/compact_now refuse to
    // spawn new runners once it is true, so no worker can appear after the
    // destructor has snapshotted and joined the thread set. Concurrent
    // sessions no longer serialize on a global mutex — each runner is
    // independently tracked in runner_threads_ and joined by the dtor.
    bool shutting_down_ = false;
};

// Parse a "## Tasks" checklist from plan markdown into seed-ready Todo items.
// Returns an empty vector if no "## Tasks" section is found.
std::vector<Todo> parse_plan_tasks(const std::string& markdown);

// Hysteresis gate for the auto-compaction trigger. Returns true if a
// compaction should fire this step given the previous compaction step
// (use a negative value to mean "never"). Blocks re-firing within 2 steps
// of the previous compaction to prevent runaway recompaction in the
// degenerate case where the post-compaction tail is itself above threshold.
bool should_compact_with_hysteresis(int prev_total_input,
                                    int threshold_tokens,
                                    int step,
                                    int last_compaction_step);

// Tiered dynamic system block: wording escalates as the turn's renewable
// step budget depletes so the model reprioritizes before running out. The
// stable cached body is untouched — only this tail changes per step. The
// whole block is emitted only in the final stretch of the turn: while
// steps_left exceeds max(1, min(10, max_steps / 2)), the budget is
// plentiful and the function returns an empty string.
std::string render_dynamic_prompt(const std::string& model,
                                  const std::string& os_info,
                                  const std::string& project_dir,
                                  int steps_left,
                                  int max_steps);

} // namespace haicode
