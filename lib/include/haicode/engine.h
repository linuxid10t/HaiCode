#pragma once
#include "db.h"
#include "events.h"
#include "provider.h"
#include "tool.h"
#include "config.h"
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

namespace haicode {

class ContextBuilder {
public:
    LLMRequest build(const std::vector<SessionMessage>& messages,
                     const std::string& system_prompt,
                     const std::string& system_dynamic,
                     const std::vector<ToolDefinition>& tools,
                     const std::string& model_id,
                     const std::string& provider_id);

    // Assemble stored SessionMessages into the provider-facing message list.
    // Public so SessionEngine::compact_history can build the head slice for a
    // summarization call without going through the full build() path.
    std::vector<nlohmann::json> assemble_messages(const std::vector<SessionMessage>& msgs);
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

    std::string create_session(const std::string& project_dir,
                                const std::string& agent_id = "",
                                const std::string& model_id = "",
                                const std::string& provider_id = "");

    void submit_prompt(const std::string& session_id, const std::string& text);
    // Resume the agentic loop without adding a new user message (used after plan approval).
    void continue_session(const std::string& session_id);
    // Append a user_prompted message + publish Prompted event without starting
    // the agentic loop. Used by approval handlers between set_mode and
    // continue_session to inject the plan-approved directive.
    void inject_message(const std::string& session_id, const std::string& text);
    void interrupt(const std::string& session_id);

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
    // loop that called ask_user. Called by GUI/TUI event handlers.
    void reply_to_ask(const std::string& session_id,
                      const std::string& call_id,
                      const std::string& answer);

    const AppConfig& config() const { return config_; }

    // Read-only access to the provider registry (used by the UI to look up
    // discovered context windows for the context meter).
    ProviderRegistry& providers() { return providers_; }

private:
    void agentic_loop(const std::string& session_id);

    // Summarize the conversation head (everything before the last user turn)
    // into a single compaction_summary row, replacing the head in the DB.
    // Returns true if compaction occurred (caller must reload messages).
    // Honors the interrupt flag: aborts and returns false if set mid-summary.
    bool compact_history(const std::string& session_id,
                         Provider& provider,
                         const std::string& model_id,
                         const std::string& provider_id,
                         std::atomic<bool>* interrupt_flag,
                         int prev_input_tokens,
                         int threshold_tokens);

    // One-shot LLM call that turns the first user prompt into a concise (≤6
    // word) descriptive session title. Replaces the heuristic title via
    // update_title and publishes SessionRenamed. Best-effort: any error is
    // logged to stderr and the existing heuristic title is left in place.
    void refine_title_llm(const std::string& session_id,
                          Provider& provider,
                          const std::string& model_id);

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
    // Step number at which the most recent successful compaction fired, per
    // session. Negative = "never compacted this turn". Reset to -1 in
    // submit_prompt so each new user turn rearms the trigger. Guarded by mu_.
    std::map<std::string, int> last_compaction_step_;
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

// Pick the summarization system instruction to send to the model. First-pass
// summarization uses one prompt; if the head already begins with a prior
// compaction_summary (re-compaction), uses the resegment prompt that tells the
// model to preserve the prior summary verbatim and add a second segment.
const char* select_summary_prompt(const std::vector<SessionMessage>& head);

// Pick the seq at which the compaction tail begins. Two modes:
//   1. Multi-turn (preferred): tail starts at the most recent user_prompted,
//      provided it has at least one message before it to summarize.
//   2. Single-turn fallback: when the only user_prompted is the very first
//      message (long agentic loop with one prompt), keep the last K
//      assistant_text-with-tool_calls round-trips intact and return the seq
//      of the K-th most recent such assistant.
// Returns -1 if there is nothing worth compacting (caller treats as no-op).
int choose_tail_start_seq(const std::vector<SessionMessage>& msgs, int K = 4);

// Tiered dynamic system block: wording escalates as the step budget depletes
// so the model reprioritizes before running out. The stable cached body is
// untouched — only this tail changes per step.
std::string render_dynamic_prompt(const std::string& model,
                                  const std::string& os_info,
                                  const std::string& project_dir,
                                  int steps_left);

} // namespace haicode
