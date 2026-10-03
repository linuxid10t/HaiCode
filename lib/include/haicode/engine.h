#pragma once
#include "db.h"
#include "events.h"
#include "provider.h"
#include "tool.h"
#include "config.h"
#include "model_db.h"
#include <string>
#include <map>
#include <set>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <utility>

namespace haicode {

class PermissionRequestBroker;

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

    // Appends a per-step status update (see next_status_update) as a text
    // block at the tail of `messages`: onto the last message's content when
    // it is a user turn, else as its own user turn. assemble_messages
    // replays persisted updates through this same function, so a step's
    // update renders identically when it is the live tail and in every later
    // request (append-only history). No-op for empty text.
    static void append_status_block(std::vector<nlohmann::json>& messages,
                                    const std::string& text);

    // Non-empty: replay the provider-native reasoning items persisted on
    // assistant rows (`reasoning_items`) that were produced by this model,
    // as leading "openai_reasoning" content blocks. Set by the engine only
    // when the active provider replays_reasoning_items(); empty (default)
    // keeps those blocks off every other provider's wire.
    std::string replay_reasoning_model;
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

    // Prompts currently queued behind the session's foreground work (they run
    // as their own turns when it ends). Used by the UI to restore the queued
    // indicator on session switch.
    size_t queued_prompt_count(const std::string& session_id);

    // Ids of every session whose agentic loop is currently executing (under
    // mu_). Used by the quit path to warn before interrupting live runs.
    std::vector<std::string> running_sessions();

    // Delete a session: mark it retiring (new runs/submissions refused,
    // queued prompts discarded), interrupt it (asks, approvals, scoped
    // provider streams), join its foreground and title workers OUTSIDE
    // engine locks, then remove its rows and every in-memory map entry,
    // including its PermissionGate rules and grants. Returns false with
    // `error` set when the DB delete fails (the session stays usable) or the
    // engine is shutting down. Idempotent: deleting an already-deleted
    // session succeeds without touching anything.
    bool delete_session(const std::string& session_id, std::string& error);

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

    // Route unresolved permission Asks through a broker instead of the plain
    // gate callback. The broker owns the wait state cancellably: interrupt()
    // denies that session's pending approvals, shutdown() denies all, and a
    // new run re-arms submissions. Passing nullptr detaches (the gate's own
    // ask callback is used again). The broker must outlive the engine.
    void set_permission_broker(PermissionRequestBroker* broker);

    const AppConfig& config() const { return config_; }

    // Live model-database overrides (context windows, vision, pricing).
    // Seeded from the config at construction; set_model_overrides() swaps
    // in a new snapshot that running loops pick up on their next lookup, so
    // a Model Database edit needs no engine recreation. Thread-safe. Read
    // these instead of config().model_contexts/model_vision/pricing, which
    // keep their construction-time values.
    std::shared_ptr<const ModelOverrides> model_overrides() const;
    void set_model_overrides(ModelOverrides overrides);

    // Read-only access to the provider registry (used by the UI to look up
    // discovered context windows for the context meter).
    ProviderRegistry& providers() { return providers_; }

private:
    void agentic_loop(const std::string& session_id);

    // Surface a DbError from a GUI-thread persist as a StepFailed event
    // ("database error: …") instead of letting it escape into the looper.
    void publish_db_error(const std::string& session_id, const std::string& what);

    // Entry point for the std::thread runners spawned by submit_prompt /
    // continue_session / retry_last_turn: persist the claimed prompt row (if
    // any), run the turn, then drain queued prompts — each as its own turn —
    // all behind the exception barrier. An exception escaping the loop must
    // never propagate out of the thread (std::terminate → abort); it is
    // logged and surfaced to the UI as a failed step instead, and the
    // running flag is always cleared.
    void runner_main(const std::string& session_id,
                     const std::string& initial_row = "");

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
    // the conversation's user prompts. Runs on its own tracked maintenance
    // worker (see spawn_title_refinement): baseline_title guards against a
    // stale job overwriting a newer title (the write only lands while the
    // stored title still equals the baseline captured at spawn), and cancel
    // aborts promptly. Best-effort: any error is logged to stderr and the
    // existing title is left in place.
    void refine_title_llm(const std::string& session_id,
                          Provider& provider,
                          const std::string& model_id,
                          const std::string& baseline_title,
                          const std::atomic<bool>* cancel,
                          const std::string& stream_token = "");

    // Vision fallback: explicit user-selected model and registered provider.
    bool vision_fallback_ready();

    // Persists one request's cost/usage (SessionStore::update_cost) and
    // publishes CostUpdated with the session's new persisted totals, so a
    // frontend never shows a total that drifts from the store. Every cost
    // write goes through here — agentic steps and maintenance calls alike.
    // `sets_context`: see SessionStore::update_cost.
    void record_usage(const std::string& session_id, double cost,
                      const TokenUsage& usage, bool sets_context);

    // One-shot describer call: sends the image to the fallback model and
    // returns its text description ("" on failure). Same synchronous
    // provider.stream pattern as refine_title_llm; runs on the engine's
    // worker thread only.
    std::string describe_image(Provider& provider,
                               const std::string& provider_id,
                               const std::string& session_id,
                               const std::string& model_id,
                               const nlohmann::json& att,
                               const std::atomic<bool>* interrupted,
                               const std::string& stream_token);

    // Backfill recent images once; persist complete/failed attempt status,
    // but leave cancellation retryable. Expired images need no description.
    void backfill_attachment_descriptions(const std::string& session_id,
                                          std::vector<SessionMessage>& messages,
                                          const std::atomic<bool>* interrupted,
                                          const std::string& stream_token);

    // Persist one prepared prompt row (shared by the immediate and queued
    // paths): append + heuristic autonaming + compaction rearm + Prompted.
    // Returns false when the row could not be persisted — the turn must not
    // run, so callers skip it. Publishes Prompted only on confirmed persist.
    bool store_prompt_row(const std::string& session_id,
                          const nlohmann::json& data);

    // Spawn the foreground runner for a claimed prompt (caller holds mu_):
    // arm a fresh interrupt flag, mark the session running, and hand the
    // prepared row to the worker, which persists it before the turn starts.
    // Returns the session's previous (already finished) thread handle for the
    // caller to join OUTSIDE mu_ — the retiring runner's final acts are a
    // publish and a mu_-scoped flag clear, and joining under mu_ could
    // deadlock against a publish handler that re-enters the engine.
    std::thread spawn_runner_locked(const std::string& session_id,
                                    const std::string& initial_row);

    // agentic_loop wrapped in the runner exception barrier (DbError →
    // "database error", anything else → "internal error" StepFailed); a
    // throwing turn must never kill the worker before the queue drains.
    // prompt_row, when non-empty, is persisted first via store_prompt_row —
    // a failed persist skips the turn (nothing was recorded to run).
    void run_turn_barried(const std::string& session_id,
                          const std::string& prompt_row);

    // Foreground drain: run each queued prompt as its own turn (fresh
    // interruption state, re-armed broker submissions, persisted row first),
    // then flip the session idle atomically with the queue check so a
    // submission at runner exit can never strand. TurnEnded is published
    // just before the flip and re-checked after — a prompt that raced in
    // during publication is drained, not lost.
    void drain_queue_and_finish(const std::string& session_id);

    // Join finished title workers (bounded growth); never called under mu_.
    void sweep_title_jobs();

    // Spawn a tracked maintenance worker for post-turn LLM title refinement,
    // so a slow title request never keeps the session's foreground state
    // busy and never blocks the next queued turn.
    void spawn_title_refinement(const std::string& session_id,
                                const std::shared_ptr<Provider>& provider,
                                const std::string& model_id);

    SessionStore& store_;
    ProviderRegistry& providers_;
    ToolRegistry& tools_;
    PermissionGate& permissions_;
    SessionEventBus& bus_;
    AppConfig config_;
    mutable std::mutex model_db_mu_;
    std::shared_ptr<const ModelOverrides> model_db_;  // guarded by model_db_mu_
    PermissionRequestBroker* perm_broker_ = nullptr;

    std::map<std::string, std::thread> runner_threads_;
    std::map<std::string, std::atomic<bool>*> interrupt_flags_;
    std::map<std::string, bool> session_running_;  // true while agentic_loop is executing
    std::map<std::string, SessionMode> session_modes_;
    // Fallback activity shares the foreground run token; interrupt/shutdown
    // must cancel this provider too, not just the primary. Guarded by mu_.
    std::map<std::string, std::shared_ptr<Provider>> fallback_providers_;
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

    // Prompts submitted while foreground work is running: rows are persisted
    // only when the prompt's turn actually starts, so a mid-turn submission
    // can never land between a tool_use row and its results. Guarded by mu_.
    std::map<std::string, std::deque<std::string>> prompt_queue_;

    // Sessions marked for deletion: every spawn path refuses, queued prompts
    // are discarded, and the drain stops instead of resetting the interrupt
    // flag for another queued turn. Guarded by mu_.
    std::set<std::string> retiring_sessions_;

    // Post-turn LLM title refinement runs on tracked maintenance workers so a
    // slow request never keeps the session's foreground state busy. cancel and
    // done are shared_ptr atomics captured by the worker, so sweeping or
    // clearing the vector while a worker still runs stays safe. Jobs are
    // joined on shutdown (and, later, deletion) — never detached. mu_ guards
    // the vector.
    struct TitleJob {
        std::string session_id;
        std::thread worker;
        std::shared_ptr<Provider> provider;
        std::string stream_token;
        std::shared_ptr<std::atomic<bool>> cancel =
            std::make_unique<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> done =
            std::make_unique<std::atomic<bool>>(false);
    };
    std::vector<std::unique_ptr<TitleJob>> title_jobs_;
    std::map<std::string, int> interrupts_in_progress_;
    std::condition_variable run_cv_;
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

// Per-step status the agentic loop keeps the model anchored to: the
// final-stretch step-budget text (render_dynamic_prompt), the rendered todo
// list (todos_tracked=false in Plan mode, which doesn't show it), and the
// offline flag.
struct StepStatus {
    std::string budget;
    std::string todos;
    bool todos_tracked = true;
    bool offline = false;
};

// Decides what status text the next request carries. The text lives in the
// conversation history (persisted on the step's assistant row as `status`
// and replayed in place), so only the parts that CHANGED since the last
// persisted update (`previous`, null when none is in context) are sent;
// unchanged state is already in context. Parts that disappeared get an
// explicit line (budget renewed, todo list empty, offline off) so a stale
// earlier copy is never the model's latest word. Returns null when nothing
// changed, else {"text", "budget", "todos", "offline"} — the full current
// state, which the next call compares against.
nlohmann::json next_status_update(const StepStatus& now,
                                  const nlohmann::json& previous);

// The most recent persisted status update among `messages` (the
// post-checkpoint context rows), or null.
nlohmann::json last_status_update(const std::vector<SessionMessage>& messages);

} // namespace haicode
