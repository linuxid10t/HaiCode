#pragma once
#include <SupportDefs.h>

// Engine → UI
static const uint32 MSG_TEXT_DELTA     = 'TXdl';
static const uint32 MSG_TOOL_CALLED    = 'TLcl';
static const uint32 MSG_TOOL_RESULT    = 'TLrs';
static const uint32 MSG_STEP_STARTED   = 'STst';
static const uint32 MSG_STEP_ENDED     = 'STen';
static const uint32 MSG_STEP_FAILED    = 'STfl';
static const uint32 MSG_PERMISSION_REQ = 'PRrq';
static const uint32 MSG_PLAN_PROPOSED  = 'PLpr';   // plan_str + path_str
static const uint32 MSG_TODOS_UPDATED  = 'TDup';   // repeated "todo_content"/"todo_active"/"todo_status" strings

// UI → Engine / UI internal
static const uint32 MSG_SUBMIT_PROMPT  = 'PMpt';
static const uint32 MSG_INTERRUPT      = 'INTr';
static const uint32 MSG_NEW_SESSION    = 'NSes';
static const uint32 MSG_SELECT_SESSION = 'SLss';
static const uint32 MSG_PERMISSION_REP = 'PRrp';  // reply from PermissionWindow
static const uint32 MSG_TOGGLE_MODE    = 'TGmd';  // mode button pressed
static const uint32 MSG_PLAN_DECISION  = 'PLdc';  // reply from PlanReviewWindow (approved bool)

// Settings
static const uint32 MSG_SHOW_SETTINGS   = 'SHst';
static const uint32 MSG_SETTINGS_SAVED  = 'SVst';  // carries "providers" JSON string

// SettingsWindow internal
static const uint32 MSG_PROVIDER_ADD    = 'PVad';  // add button
static const uint32 MSG_PROVIDER_EDIT   = 'PVed';  // edit button
static const uint32 MSG_PROVIDER_REMOVE = 'PVrm';  // remove button
static const uint32 MSG_PROVIDER_DIALOG_DONE = 'PVdd'; // add/edit sub-dialog OK
static const uint32 MSG_LIST_SEL        = 'PVls';  // list selection changed

// Model list
static const uint32 MSG_FETCH_MODELS    = 'FTmd';  // MainWindow → be_app; "provider_id" string
static const uint32 MSG_MODELS_LOADED   = 'MLld';  // be_app → MainWindow; repeated "model" strings
static const uint32 MSG_MODEL_SELECTED  = 'MDsl';  // model menu item → MainWindow (no args; read marked item)

// Working directory
static const uint32 MSG_CHOOSE_DIR      = 'CHdr';  // dir button pressed → open BFilePanel
static const uint32 MSG_DIR_CHANGED     = 'DChr';  // MainWindow → be_app; "path" string

// Permission management
static const uint32 MSG_ADD_PERMISSION  = 'ADpm';  // MainWindow → be_app; "action"+"resource" strings
static const uint32 MSG_AUTO_ALLOW_EDITS = 'AAed'; // checkbox → be_app; "be:value" int32
static const uint32 MSG_YOLO             = 'YOLO'; // checkbox → be_app; "be:value" int32

// Provider/model persistence — MainWindow → be_app; "provider"+"model" strings
static const uint32 MSG_PERSIST_PM      = 'PMps';

// Session tracking
static const uint32 MSG_ACTIVE_SESSION  = 'ACSs';  // MainWindow → be_app; "session_id" string
static const uint32 MSG_DELETE_SESSION  = 'DLss';  // SessionListView → MainWindow; "index" int32
