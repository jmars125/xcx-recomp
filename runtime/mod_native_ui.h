// Copied native UI requests from a mod: which features it wants and which of its actions each squad screen option
// runs. No guest or controller pointer enters the registry.
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { MOD_NATIVE_ONLINE_ENTRY = 1, MOD_NATIVE_SQUAD_SELECT = 2, MOD_NATIVE_DETAILS = 4 };
enum { MOD_NATIVE_QUICK_ASSIGNMENT, MOD_NATIVE_PLAY_WITH_FRIENDS, MOD_NATIVE_CREATE_PRIVATE, MOD_NATIVE_PLAY_PRIVATE,
       MOD_NATIVE_OPTIONS,                       // the four squad screen options; the next two are the Details screen's buttons
       MOD_NATIVE_DETAILS_START = MOD_NATIVE_OPTIONS, MOD_NATIVE_DETAILS_LEAVE, MOD_NATIVE_ACTIONS };
typedef struct {char owner[65]; unsigned features; char action[MOD_NATIVE_ACTIONS][65]; uint64_t epoch;} ModNativeUi;
void mod_native_ui_reset(void);
void mod_native_ui_support(int available);
// NULL on success, otherwise "unsupported" (wrong build or not enabled), "full" (another mod holds the slot) or
// "unavailable" (the registry was invalidated).
const char *mod_native_ui_register(const ModNativeUi *request);
// The features of the registered request, readable without a lock from the guest UI thread; 0 when none.
unsigned mod_native_ui_features(void);
// Submits the action bound to a squad screen option (0-3) or a Details button (MOD_NATIVE_DETAILS_START, _LEAVE) as a native
// intent. 1 if the mod accepted it.
int mod_native_ui_confirm(unsigned option);
// The same with what the player typed (the Join passcode, at most 15 characters), delivered to the action as event field `text`.
int mod_native_ui_confirm_text(unsigned option, const char *text);
void mod_native_ui_invalidate(void);
// 1 when the healthy registered mod binds an action to the option; readable from the guest UI thread.
int mod_native_ui_has_action(unsigned option);
// What the mod reported after its action ran: the guest UI thread starts a wait with _begin, the mod answers with _report
// (only the registered owner may), and the UI thread reads the answer with _result. A failure may carry the text of the
// message window to show (see runtime/message_text.h): 1 to MOD_NATIVE_TEXT_MAX characters, printable ASCII or a newline.
enum { MOD_NATIVE_PENDING, MOD_NATIVE_OK, MOD_NATIVE_FAILED, MOD_NATIVE_TEXT_MAX = 75, MOD_NATIVE_TOAST_MAX = 46 };
void mod_native_ui_begin(void);
int mod_native_ui_report(const char *owner, unsigned status); // 1 when accepted
int mod_native_ui_report_message(const char *owner, unsigned status, const char *text); // text only with FAILED, may be NULL
int mod_native_ui_text_valid(const char *text);
unsigned mod_native_ui_result(void);
void mod_native_ui_message(char *out, size_t cap); // the text of the last report, "" when it had none
// A success may carry what the squad details screen shows: the passcode (1 to MOD_NATIVE_CODE_MAX letters, digits or
// hyphens) and the mode (created, joined, or in_squad for the already-in-a-squad explanation); MOD_NATIVE_MODE_NONE lets the game's
// screen use the one the option implies. The screen is opened only for a mod that registered MOD_NATIVE_DETAILS.
enum { MOD_NATIVE_CODE_MAX = 15, MOD_NATIVE_MODE_NONE = -1, MOD_NATIVE_MODE_CREATED = 0, MOD_NATIVE_MODE_JOINED = 1,
       MOD_NATIVE_MODE_IN_SQUAD = 2 };
// It may also carry two short texts that replace the screen's timer: a label (1 to MOD_NATIVE_LABEL_MAX characters, replacing
// the timer label) and a status (1 to MOD_NATIVE_STATUS_MAX characters, shown where the countdown is, for example "1 / 4");
// mod_native_ui_details_status changes the status while the screen is open.
enum { MOD_NATIVE_LABEL_MAX = 13, MOD_NATIVE_STATUS_MAX = 8 };
int mod_native_ui_code_valid(const char *code);
int mod_native_ui_label_valid(const char *text);
int mod_native_ui_status_valid(const char *text);
// An OK report with details; passcode, label and status may be NULL.
int mod_native_ui_report_details(const char *owner, const char *passcode, int mode, const char *label, const char *status);
int mod_native_ui_details_status(const char *owner, const char *status); // 1 when accepted
// The mod says whether a session is up. While it is, the join card does not ask for a passcode: it goes to the mod's action at once
// (which can answer with the Details screen of the session it is in). 1 when accepted (the registered owner, `details` registered).
int mod_native_ui_set_session(const char *owner, int active);
int mod_native_ui_session_active(void);
int mod_native_ui_details(char *code, size_t cap); // the last OK report's passcode ("" when none); returns its mode or _NONE
// The last report's label and status ("" when none); returns a version that changes whenever the status changes.
unsigned mod_native_ui_details_text(char *label, size_t label_cap, char *status, size_t status_cap);
// A toast with the mod's own words (1 to 46 printable ASCII characters, one line), shown by the game's own toast while Squad
// Select is open. Only the registered owner may ask; 1 when accepted. The guest UI thread takes the waiting text (the latest
// request wins) with _take_toast, which returns 1 once per accepted text.
int mod_native_ui_toast_valid(const char *text);
int mod_native_ui_toast(const char *owner, const char *text);
int mod_native_ui_take_toast(char *out, size_t cap);
// Called after a native intent is queued so the mod worker runs it without waiting for its idle poll; NULL clears.
void mod_native_ui_set_wake(void (*callback)(void));
