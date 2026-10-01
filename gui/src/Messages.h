#pragma once
#include <SupportDefs.h>

// Engine → UI
static const uint32 MSG_TEXT_DELTA     = 'TXdl';
static const uint32 MSG_REASONING_DELTA= 'RSdl';  // reasoning delta for the thinking bubble
static const uint32 MSG_TOOL_CALLED    = 'TLcl';
static const uint32 MSG_TOOL_RESULT    = 'TLrs';
static const uint32 MSG_STEP_STARTED   = 'STst';
static const uint32 MSG_PROMPT_STARTED = 'UPst';  // persisted prompt starts its own turn
static const uint32 MSG_PROMPT_QUEUED  = 'PQnd';  // engine → UI: prompt queued behind the running turn; "queued_count" int32
static const uint32 MSG_TURN_ENDED     = 'TNen';  // engine → UI: foreground runner finished, queue empty
static const uint32 MSG_STEP_ENDED     = 'STen';
static const uint32 MSG_STEP_FAILED    = 'STfl';
static const uint32 MSG_PERMISSION_REQ = 'PRrq';  // broker → UI: show approval window; structured request + "request_id"
static const uint32 MSG_PERMISSION_WINDOW_CLOSED = 'PRwc';  // PermissionWindow → MainWindow; "session_id" string
static const uint32 MSG_PLAN_PROPOSED  = 'PLpr';   // plan_str + path_str
static const uint32 MSG_TODOS_UPDATED  = 'TDup';   // repeated "todo_content"/"todo_active"/"todo_status" strings
static const uint32 MSG_BUILD_HOOK_START = 'BHst';
static const uint32 MSG_BUILD_HOOK     = 'BHrs';   // "success" bool, "exit_code" int32
static const uint32 MSG_INTERRUPTED    = 'INtd';   // engine → UI: interrupt completed
static const uint32 MSG_COMPACTION     = 'CMpt';   // "phase" string ("start"/"end"), counts
static const uint32 MSG_SESSION_RENAMED= 'SRnm';   // "title" string — refresh session list

// UI → Engine / UI internal
static const uint32 MSG_SUBMIT_PROMPT  = 'PMpt';
static const uint32 MSG_INTERRUPT      = 'INTr';
static const uint32 MSG_COMPACT_NOW    = 'CMnw';  // Compact button pressed
static const uint32 MSG_NEW_SESSION    = 'NSes';
static const uint32 MSG_OFFLINE_MODE   = 'OFmd';
static const uint32 MSG_SELECT_SESSION = 'SLss';
static const uint32 MSG_PERMISSION_DECISION = 'PRdc';  // PermissionWindow → be_app; "request_id" string + "decision" int32 (0=deny, 1=allow once, 2=allow for session)
static const uint32 MSG_MODE_SELECTED   = 'MDmd';  // mode menu item; "mode" string ("build"/"plan"/"chat")
static const uint32 MSG_PLAN_DECISION  = 'PLdc';  // reply from PlanReviewWindow (approved bool)
static const uint32 MSG_ASK_USER_REQ   = 'AUrq';  // engine → UI: show AskUserWindow
static const uint32 MSG_ASK_USER_REPLY = 'AUrp';  // UI → engine: user's answer

// Settings
static const uint32 MSG_SHOW_SETTINGS   = 'SHst';
static const uint32 MSG_SETTINGS_SAVED  = 'SVst';  // carries "providers" JSON string
static const uint32 MSG_PROVIDERS_UPDATED = 'PVup'; // carries only "providers" JSON string

// SettingsWindow internal
static const uint32 MSG_PROVIDER_ADD    = 'PVad';  // add button
static const uint32 MSG_PROVIDER_EDIT   = 'PVed';  // edit button
static const uint32 MSG_PROVIDER_REMOVE = 'PVrm';  // remove button
static const uint32 MSG_PROVIDER_DIALOG_DONE = 'PVdd'; // add/edit sub-dialog OK
static const uint32 MSG_LIST_SEL        = 'PVls';  // list selection changed
static const uint32 MSG_SET_PROVIDER    = 'PVsp';  // provider dropdown changed (carries "provider_id")
static const uint32 MSG_WS_ENGINE_SELECTED = 'WSes'; // web-search engine menu item (carries "engine")

// ProviderEditWindow internal — Sign in with ChatGPT (Codex OAuth)
static const uint32 MSG_OAUTH_LOGIN     = 'OAlg';  // sign-in button pressed
static const uint32 MSG_OAUTH_RESULT    = 'OArs';  // worker thread → window; "ok" bool, "error"/"account_id" strings

// Vision fallback provider/model pair (SettingsWindow internal + app fetch)
static const uint32 MSG_FB_PROVIDER_SET = 'VBps';  // fallback provider dropdown changed (carries "provider_id")
static const uint32 MSG_FB_MODELS_LOADED = 'VBlm'; // be_app → SettingsWindow; repeated "model" strings
static const uint32 MSG_FB_MODEL_CHANGED = 'VBmc'; // fallback model dropdown item selected

static const uint32 MSG_FB_MODEL_REFRESH = 'VBfr';  // SettingsWindow internal; fallback model dropdown clicked

// Model list
static const uint32 MSG_FETCH_MODELS    = 'FTmd';  // MainWindow → be_app; "provider_id" string
static const uint32 MSG_MODELS_LOADED   = 'MLld';  // be_app → MainWindow; repeated "model" strings
static const uint32 MSG_MODEL_SELECTED  = 'MDsl';  // model menu item → MainWindow (no args; read marked item)
static const uint32 MSG_MODEL_REFRESH   = 'Mdfr';  // model dropdown clicked (MainWindow + SettingsWindow primary)
static const uint32 MSG_MODEL_CONTEXT   = 'MDcx';  // discovery thread → MainWindow; "provider_id"/"model_id" strings + "context" int32

// Working directory
static const uint32 MSG_CHOOSE_DIR      = 'CHdr';  // dir button pressed → open BFilePanel
static const uint32 MSG_DIR_CHANGED     = 'DChr';  // MainWindow → be_app; "path" string

// Image attachments
static const uint32 MSG_ATTACH          = 'ATch';  // "+" button pressed → open image BFilePanel
static const uint32 MSG_ATTACH_REFS     = 'ATrf';  // refs forwarded from the attachment panel
static const uint32 MSG_REMOVE_ATTACHMENT = 'RMat'; // chip "×" pressed; "index" int32

// Permission management
static const uint32 MSG_AUTO_ALLOW_EDITS = 'AAed'; // session flag → be_app; "be:value" int32 + "session_id"
static const uint32 MSG_YOLO             = 'YOLO'; // session flag → be_app; "be:value" int32 + "session_id"
static const uint32 MSG_READ_EVERYWHERE  = 'RDew'; // session flag → be_app; "be:value" int32 + "session_id"
static const uint32 MSG_SHOW_PERMISSIONS = 'PMct'; // MainWindow → be_app: open the Permissions center
static const uint32 MSG_PERMISSION_CENTER_CLOSED = 'PMcc'; // center → be_app (clear the stored messenger)

// Permissions center internal
static const uint32 MSG_PERM_SESSION_SELECTED = 'PSsl';  // session dropdown; "session_id" string
static const uint32 MSG_PERM_TOGGLE_AUTO      = 'PTaw';  // auto-writes checkbox (center-local)
static const uint32 MSG_PERM_TOGGLE_READ      = 'PTrd';  // read-outside checkbox (center) / dropdown item (MainWindow)
static const uint32 MSG_PERM_TOGGLE_BYPASS    = 'PTbp';  // bypass checkbox (center-local, confirmed)
static const uint32 MSG_PERM_PENDING_ALLOW    = 'PPal';  // approve selected pending row (once)
static const uint32 MSG_PERM_PENDING_DENY     = 'PPdn';  // deny selected pending row
static const uint32 MSG_PERM_REVOKE           = 'PMrv';  // revoke selected grant row
static const uint32 MSG_PERM_REVOKE_ALL       = 'PMra';  // revoke all grants (selected session)
static const uint32 MSG_PERM_REFRESH          = 'PMrf';  // be_app → center: re-read state
static const uint32 MSG_PERM_ACTIVATE         = 'PMac';  // be_app → center: Activate()
static const uint32 MSG_PERM_STATUS           = 'PMst';  // be_app → MainWindow: "status" string + repeated "pend_session" strings
static const uint32 MSG_PERM_SYNC             = 'PMsy';  // any thread → be_app: recompute permission status + badges
static const uint32 MSG_PERM_PRESET           = 'PMpr';  // MainWindow dropdown → MainWindow: "preset" string (standard/auto-write/unrestricted)

// PermissionRuleEditWindow → PermissionsCenterWindow
static const uint32 MSG_PERM_RULE_DONE = 'PRrd';  // "action"/"resource"/"effect" strings + "index" int32

// PermissionsCenterWindow → be_app
static const uint32 MSG_PERM_POLICY_SAVED = 'PMsv';  // policy file written; reload config + gate rules

// Provider/model persistence — MainWindow → be_app; "provider"+"model" strings
static const uint32 MSG_PERSIST_PM      = 'PMps';

// Inference settings — Inference tab fields and toolbar reasoning → MainWindow
static const uint32 MSG_APPLY_INFERENCE = 'APin';
static const uint32 MSG_INFERENCE_CHANGED = 'INch';
static const uint32 MSG_REASONING_SELECTED = 'RSsl';

// Skills — Skills tab list row selected (click toggles that skill);
// "index" int32 into the skills list
static const uint32 MSG_SKILL_TOGGLED   = 'SKtg';

// Session tracking
static const uint32 MSG_ACTIVE_SESSION  = 'ACSs';  // MainWindow → be_app; "session_id" string
static const uint32 MSG_DELETE_SESSION  = 'DLss';  // SessionListView → MainWindow; "index" int32
