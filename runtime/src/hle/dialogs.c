/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceMsgDialog, libSceErrorDialog, libSceSaveDataDialog: dialogs with no
 * UI. Each is the common-dialog state machine (NONE -> INITIALIZED ->
 * RUNNING -> FINISHED) and answers by itself as if the user pressed the
 * default button (OK / Yes):
 *   - Open sets RUNNING (so the game sees one RUNNING status, as on a PS4);
 *     the next UpdateStatus finishes it with result OK.
 *   - Dialogs that have no button (progress bars, "please wait" messages,
 *     save "progress" system messages) stay RUNNING until the game closes
 *     them, which is what the game expects.
 * Messages are logged so the smoke log shows what the game wanted to say.
 *
 * Semantics ported from shadPS4 (GPL-2.0, commit 14ada48):
 *   src/core/libraries/system/msgdialog.cpp, msgdialog_ui.h
 *   src/core/libraries/ime/error_dialog.cpp
 *   src/core/libraries/save_data/dialog/savedatadialog.cpp, savedatadialog_ui.{h,cpp}
 * libSceCommonDialog itself (Initialize, IsUsed) is implemented elsewhere,
 * so the "common dialog system not initialized" and "another dialog is
 * busy" checks are not done here. */
#include <string.h>

#include "../log.h"
#include "hle.h"

/* Implemented in savedata.c. */
bool hle_savedata_dir_exists(const char *dir_name);
bool hle_savedata_read_param(const char *dir_name, void *param_out);

/* CommonDialog status, result and error codes (shadPS4 commondialog.h). */
enum { CD_NONE = 0, CD_INITIALIZED = 1, CD_RUNNING = 2, CD_FINISHED = 3 };
enum { CD_RESULT_OK = 0, CD_RESULT_USER_CANCELED = 1 };
#define CD_ERR_NOT_INITIALIZED ((int32_t)0x80B80003)
#define CD_ERR_ALREADY_INITIALIZED ((int32_t)0x80B80004)
#define CD_ERR_NOT_FINISHED ((int32_t)0x80B80005)
#define CD_ERR_INVALID_STATE ((int32_t)0x80B80006)
#define CD_ERR_PARAM_INVALID ((int32_t)0x80B8000A)
#define CD_ERR_NOT_RUNNING ((int32_t)0x80B8000B)
#define CD_ERR_ARG_NULL ((int32_t)0x80B8000D)
#define CD_ERR_NOT_SUPPORTED ((int32_t)0x80B8000F)

enum { BUTTON_INVALID = 0, BUTTON_OK = 1, BUTTON_YES = 1, BUTTON_NO = 2 };

typedef struct {
    uint64_t size;
    uint8_t reserved[36];
    uint32_t magic;
} cd_base_param;
_Static_assert(sizeof(cd_base_param) == 48, "SceCommonDialogBaseParam");

/* A dialog: status, plus whether it closes itself on the next update. */
typedef struct {
    plat_mutex *lock;
    uint32_t status;
    bool auto_finish;
} dialog;

static dialog g_msg, g_err, g_save;

__attribute__((constructor)) static void dialogs_init(void) {
    g_msg.lock = plat_mutex_new();
    g_err.lock = plat_mutex_new();
    g_save.lock = plat_mutex_new();
}

static int32_t dlg_initialize(dialog *d) {
    plat_mutex_lock(d->lock);
    int32_t ret = d->status != CD_NONE ? CD_ERR_ALREADY_INITIALIZED : SCE_OK;
    if (ret == SCE_OK) d->status = CD_INITIALIZED;
    plat_mutex_unlock(d->lock);
    return ret;
}

static int32_t dlg_check_open(dialog *d, const void *param) {
    if (d->status != CD_INITIALIZED && d->status != CD_FINISHED) return CD_ERR_INVALID_STATE;
    if (!param) return CD_ERR_ARG_NULL;
    return SCE_OK;
}

/* ===================== libSceMsgDialog ===================== */

enum { MSG_MODE_USER = 1, MSG_MODE_PROGRESS_BAR = 2, MSG_MODE_SYSTEM = 3 };
enum {
    MSG_BTN_OK = 0, MSG_BTN_YESNO = 1, MSG_BTN_NONE = 2, MSG_BTN_OK_CANCEL = 3, MSG_BTN_WAIT = 5,
    MSG_BTN_WAIT_CANCEL = 6, MSG_BTN_YESNO_FOCUS_NO = 7, MSG_BTN_OK_CANCEL_FOCUS_CANCEL = 8,
    MSG_BTN_TWO_BUTTONS = 9,
};

typedef struct {
    uint32_t button_type;
    int32_t pad;
    const char *msg;
    const void *buttons_param;
    char reserved[24];
} msg_user_param;

typedef struct {
    uint32_t bar_type;
    int32_t pad;
    const char *msg;
    char reserved[64];
} msg_progress_param;

typedef struct {
    uint32_t sys_msg_type;
    char reserved[32];
} msg_system_param;

typedef struct {
    cd_base_param base;
    uint64_t size;
    uint32_t mode;
    int32_t pad;
    const msg_user_param *user_msg;
    const msg_progress_param *prog_bar;
    const msg_system_param *sys_msg;
    int32_t user_id;
    char reserved[40];
    int32_t pad2;
} msg_param;
_Static_assert(sizeof(msg_param) == 136, "SceMsgDialogParam");

typedef struct {
    uint32_t mode;
    int32_t result;
    uint32_t button_id;
    char reserved[32];
} msg_result;

static uint32_t g_msg_mode;
static msg_result g_msg_result;

static void msg_finish(int32_t result, uint32_t button) {
    g_msg_result.mode = g_msg_mode;
    g_msg_result.result = result;
    g_msg_result.button_id = button;
    g_msg.status = CD_FINISHED;
    g_msg.auto_finish = false;
}

static SYSV int32_t ps4_sceMsgDialogInitialize(void) { return dlg_initialize(&g_msg); }

static SYSV int32_t ps4_sceMsgDialogOpen(const msg_param *p) {
    plat_mutex_lock(g_msg.lock);
    int32_t ret = dlg_check_open(&g_msg, p);
    if (ret == SCE_OK) {
        g_msg_mode = p->mode;
        memset(&g_msg_result, 0, sizeof(g_msg_result));
        bool has_button = true;
        switch (p->mode) {
        case MSG_MODE_USER: {
            uint32_t bt = p->user_msg ? p->user_msg->button_type : MSG_BTN_OK;
            has_button = bt != MSG_BTN_NONE && bt != MSG_BTN_WAIT && bt != MSG_BTN_WAIT_CANCEL;
            log_info("msgdialog: \"%s\" (buttons %u)%s",
                     p->user_msg && p->user_msg->msg ? p->user_msg->msg : "", bt,
                     has_button ? " -> OK" : "");
            break;
        }
        case MSG_MODE_PROGRESS_BAR:
            has_button = false;
            log_info("msgdialog: progress \"%s\"",
                     p->prog_bar && p->prog_bar->msg ? p->prog_bar->msg : "");
            break;
        case MSG_MODE_SYSTEM:
            log_info("msgdialog: system message %u -> OK", p->sys_msg ? p->sys_msg->sys_msg_type : 0);
            break;
        default:
            log_warn("msgdialog: unknown mode %u -> OK", p->mode);
            break;
        }
        g_msg.status = CD_RUNNING;
        g_msg.auto_finish = has_button;
    }
    plat_mutex_unlock(g_msg.lock);
    return ret;
}

static SYSV int32_t ps4_sceMsgDialogUpdateStatus(void) {
    plat_mutex_lock(g_msg.lock);
    uint32_t status = g_msg.status;
    if (status == CD_RUNNING && g_msg.auto_finish) msg_finish(CD_RESULT_OK, BUTTON_OK);
    plat_mutex_unlock(g_msg.lock);
    return (int32_t)status;
}

static SYSV int32_t ps4_sceMsgDialogGetStatus(void) { return (int32_t)g_msg.status; }

static SYSV int32_t ps4_sceMsgDialogGetResult(msg_result *r) {
    plat_mutex_lock(g_msg.lock);
    int32_t ret = g_msg.status != CD_FINISHED ? CD_ERR_NOT_FINISHED : !r ? CD_ERR_ARG_NULL : SCE_OK;
    if (ret == SCE_OK) *r = g_msg_result;
    plat_mutex_unlock(g_msg.lock);
    return ret;
}

static SYSV int32_t ps4_sceMsgDialogClose(void) {
    plat_mutex_lock(g_msg.lock);
    int32_t ret = g_msg.status != CD_RUNNING ? CD_ERR_NOT_RUNNING : SCE_OK;
    if (ret == SCE_OK) msg_finish(CD_RESULT_OK, BUTTON_INVALID);
    plat_mutex_unlock(g_msg.lock);
    return ret;
}

static SYSV int32_t ps4_sceMsgDialogTerminate(void) {
    plat_mutex_lock(g_msg.lock);
    if (g_msg.status == CD_RUNNING) msg_finish(CD_RESULT_OK, BUTTON_INVALID);
    int32_t ret = g_msg.status == CD_NONE ? CD_ERR_NOT_INITIALIZED : SCE_OK;
    g_msg.status = CD_NONE;
    plat_mutex_unlock(g_msg.lock);
    return ret;
}

static int32_t msg_progress_check(uint32_t target) {
    if (g_msg.status != CD_RUNNING) return CD_ERR_NOT_RUNNING;
    if (g_msg_mode != MSG_MODE_PROGRESS_BAR) return CD_ERR_NOT_SUPPORTED;
    if (target != 0) return CD_ERR_PARAM_INVALID;
    return SCE_OK;
}

static SYSV int32_t ps4_sceMsgDialogProgressBarInc(uint32_t target, uint32_t delta) {
    (void)delta;
    return msg_progress_check(target);
}
static SYSV int32_t ps4_sceMsgDialogProgressBarSetValue(uint32_t target, uint32_t value) {
    (void)value;
    return msg_progress_check(target);
}
static SYSV int32_t ps4_sceMsgDialogProgressBarSetMsg(uint32_t target, const char *msg) {
    (void)msg;
    return msg_progress_check(target);
}

HLE_LIBRARY(hle_msgdialog, "libSceMsgDialog",
    HLE(sceMsgDialogInitialize),
    HLE(sceMsgDialogOpen),
    HLE(sceMsgDialogUpdateStatus),
    HLE(sceMsgDialogGetStatus),
    HLE(sceMsgDialogGetResult),
    HLE(sceMsgDialogClose),
    HLE(sceMsgDialogTerminate),
    HLE(sceMsgDialogProgressBarInc),
    HLE(sceMsgDialogProgressBarSetValue),
    HLE(sceMsgDialogProgressBarSetMsg),
)

/* ===================== libSceErrorDialog ===================== */

typedef struct {
    int32_t size;
    int32_t error_code;
    int32_t user_id;
    int32_t reserved;
} err_param;

static SYSV int32_t ps4_sceErrorDialogInitialize(void) { return dlg_initialize(&g_err); }

static SYSV int32_t ps4_sceErrorDialogOpen(const err_param *p) {
    plat_mutex_lock(g_err.lock);
    int32_t ret = dlg_check_open(&g_err, p);
    if (ret == SCE_OK) {
        log_warn("errordialog: error code %#x -> OK", (uint32_t)p->error_code);
        g_err.status = CD_RUNNING;
        g_err.auto_finish = true;
    }
    plat_mutex_unlock(g_err.lock);
    return ret;
}

static SYSV int32_t ps4_sceErrorDialogUpdateStatus(void) {
    plat_mutex_lock(g_err.lock);
    uint32_t status = g_err.status;
    if (status == CD_RUNNING && g_err.auto_finish) g_err.status = CD_FINISHED;
    plat_mutex_unlock(g_err.lock);
    return (int32_t)status;
}

static SYSV int32_t ps4_sceErrorDialogGetStatus(void) { return (int32_t)g_err.status; }

static SYSV int32_t ps4_sceErrorDialogClose(void) {
    plat_mutex_lock(g_err.lock);
    int32_t ret = g_err.status != CD_RUNNING ? CD_ERR_NOT_RUNNING : SCE_OK;
    if (ret == SCE_OK) g_err.status = CD_FINISHED;
    plat_mutex_unlock(g_err.lock);
    return ret;
}

static SYSV int32_t ps4_sceErrorDialogTerminate(void) {
    plat_mutex_lock(g_err.lock);
    int32_t ret = g_err.status == CD_NONE ? CD_ERR_NOT_INITIALIZED : SCE_OK;
    g_err.status = CD_NONE;
    plat_mutex_unlock(g_err.lock);
    return ret;
}

HLE_LIBRARY(hle_errordialog, "libSceErrorDialog",
    HLE(sceErrorDialogInitialize),
    HLE(sceErrorDialogOpen),
    HLE(sceErrorDialogUpdateStatus),
    HLE(sceErrorDialogGetStatus),
    HLE(sceErrorDialogClose),
    HLE(sceErrorDialogTerminate),
)

/* ===================== libSceSaveDataDialog ===================== */

enum {
    SDD_MODE_LIST = 1, SDD_MODE_USER_MSG = 2, SDD_MODE_SYSTEM_MSG = 3, SDD_MODE_ERROR_CODE = 4,
    SDD_MODE_PROGRESS_BAR = 5,
};
enum { SDD_TYPE_SAVE = 1, SDD_TYPE_LOAD = 2, SDD_TYPE_DELETE = 3 };
enum { SDD_BTN_OK = 0, SDD_BTN_YESNO = 1, SDD_BTN_NONE = 2, SDD_BTN_OKCANCEL = 3 };
enum { SDD_SYS_PROGRESS = 5 };
enum { SDD_FOCUS_DIRNAME = 6 };

typedef struct { char data[32]; } sdd_dir_name;

typedef struct {
    int32_t user_id;
    int32_t pad;
    const void *title_id;
    const sdd_dir_name *dir_name;
    uint32_t dir_name_num;
    int32_t pad2;
    const void *new_item;
    uint32_t focus_pos;
    int32_t pad3;
    const sdd_dir_name *focus_pos_dir_name;
    uint32_t item_style;
    uint8_t reserved[32];
} sdd_items;

typedef struct {
    uint32_t button_type;
    uint32_t msg_type;
    const char *msg;
    uint8_t reserved[32];
} sdd_user_msg;

typedef struct {
    uint32_t msg_type;
    int32_t pad;
    uint64_t value;
    uint8_t reserved[32];
} sdd_sys_msg;

typedef struct {
    uint32_t error_code;
    uint8_t reserved[32];
} sdd_error_code;

typedef struct {
    cd_base_param base;
    int32_t size;
    uint32_t mode;
    uint32_t disp_type;
    int32_t pad;
    const void *anim_param;
    const sdd_items *items;
    const sdd_user_msg *user_msg;
    const sdd_sys_msg *sys_msg;
    const sdd_error_code *error_code;
    const void *progress_bar;
    void *user_data;
    const void *option;
    uint8_t reserved[24];
} sdd_param;
_Static_assert(sizeof(sdd_param) == 152, "SceSaveDataDialogParam");

typedef struct {
    uint32_t mode;
    int32_t result;
    uint32_t button_id;
    int32_t pad;
    sdd_dir_name *dir_name;
    void *param; /* SceSaveDataParam, filled from the chosen save */
    void *user_data;
    uint8_t reserved[32];
} sdd_result;

static struct {
    uint32_t mode;
    int32_t result;
    uint32_t button_id;
    char dir_name[33];
    void *user_data;
    /* what Open decided the user answers */
    int32_t pending_result;
    uint32_t pending_button;
} g_sdd;

static void copy_dir_name(char out[33], const sdd_dir_name *dn) {
    size_t n = 0;
    while (n < 32 && dn->data[n]) n++;
    memcpy(out, dn->data, n);
    out[n] = 0;
}

static void sdd_finish(int32_t result, uint32_t button) {
    g_sdd.result = result;
    g_sdd.button_id = button;
    g_save.status = CD_FINISHED;
    g_save.auto_finish = false;
}

/* The save a list dialog picks: the focused directory if it is named, else
 * the first listed one that exists. A save dialog with a "new item" and
 * nothing focused picks the new item (empty dir name). */
static bool sdd_pick(const sdd_param *p, char out[33]) {
    const sdd_items *it = p->items;
    out[0] = 0;
    if (!it) return false;
    if (it->focus_pos == SDD_FOCUS_DIRNAME && it->focus_pos_dir_name) {
        copy_dir_name(out, it->focus_pos_dir_name);
        if (p->disp_type == SDD_TYPE_SAVE || hle_savedata_dir_exists(out)) return true;
    }
    if (p->disp_type == SDD_TYPE_SAVE && it->new_item) {
        out[0] = 0;
        return true;
    }
    for (uint32_t i = 0; it->dir_name && i < it->dir_name_num; i++) {
        copy_dir_name(out, &it->dir_name[i]);
        if (out[0] && hle_savedata_dir_exists(out)) return true;
    }
    out[0] = 0;
    /* Saving with nothing existing: take the first listed name. */
    if (p->disp_type == SDD_TYPE_SAVE && it->dir_name && it->dir_name_num > 0)
        copy_dir_name(out, &it->dir_name[0]);
    return out[0] != 0;
}

static SYSV int32_t ps4_sceSaveDataDialogInitialize(void) { return dlg_initialize(&g_save); }

static SYSV int32_t ps4_sceSaveDataDialogIsReadyToDisplay(void) { return 1; }

static SYSV int32_t ps4_sceSaveDataDialogOpen(const sdd_param *p) {
    plat_mutex_lock(g_save.lock);
    int32_t ret = dlg_check_open(&g_save, p);
    if (ret == SCE_OK) {
        memset(&g_sdd, 0, sizeof(g_sdd));
        g_sdd.mode = p->mode;
        g_sdd.user_data = p->user_data;
        g_sdd.pending_result = CD_RESULT_OK;
        g_sdd.pending_button = BUTTON_YES;
        bool has_button = true;
        /* Non-list dialogs report the first listed save (shadPS4 Finish()). */
        if (p->items && p->items->dir_name && p->items->dir_name_num > 0) {
            for (uint32_t i = 0; i < p->items->dir_name_num; i++) {
                char name[33];
                copy_dir_name(name, &p->items->dir_name[i]);
                if (name[0] && hle_savedata_dir_exists(name)) {
                    memcpy(g_sdd.dir_name, name, sizeof(name));
                    break;
                }
            }
        }
        switch (p->mode) {
        case SDD_MODE_LIST:
            g_sdd.pending_button = BUTTON_INVALID;
            if (!sdd_pick(p, g_sdd.dir_name)) g_sdd.pending_result = CD_RESULT_USER_CANCELED;
            log_info("savedatadialog: list (type %u) -> %s '%s'", p->disp_type,
                     g_sdd.pending_result == CD_RESULT_OK ? "picked" : "cancel", g_sdd.dir_name);
            break;
        case SDD_MODE_USER_MSG: {
            uint32_t bt = p->user_msg ? p->user_msg->button_type : SDD_BTN_OK;
            has_button = bt != SDD_BTN_NONE;
            log_info("savedatadialog: \"%s\"%s",
                     p->user_msg && p->user_msg->msg ? p->user_msg->msg : "",
                     has_button ? " -> OK" : "");
            break;
        }
        case SDD_MODE_SYSTEM_MSG: {
            uint32_t t = p->sys_msg ? p->sys_msg->msg_type : 0;
            has_button = t != SDD_SYS_PROGRESS;
            log_info("savedatadialog: system message %u (type %u)%s", t, p->disp_type,
                     has_button ? " -> OK" : "");
            break;
        }
        case SDD_MODE_ERROR_CODE:
            g_sdd.pending_button = BUTTON_OK;
            log_warn("savedatadialog: error code %#x -> OK",
                     p->error_code ? p->error_code->error_code : 0);
            break;
        case SDD_MODE_PROGRESS_BAR:
            has_button = false;
            log_info("savedatadialog: progress bar (type %u)", p->disp_type);
            break;
        default:
            log_warn("savedatadialog: unknown mode %u -> OK", p->mode);
            break;
        }
        g_save.status = CD_RUNNING;
        g_save.auto_finish = has_button;
    }
    plat_mutex_unlock(g_save.lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataDialogUpdateStatus(void) {
    plat_mutex_lock(g_save.lock);
    uint32_t status = g_save.status;
    if (status == CD_RUNNING && g_save.auto_finish)
        sdd_finish(g_sdd.pending_result, g_sdd.pending_button);
    plat_mutex_unlock(g_save.lock);
    return (int32_t)status;
}

static SYSV int32_t ps4_sceSaveDataDialogGetStatus(void) { return (int32_t)g_save.status; }

static SYSV int32_t ps4_sceSaveDataDialogGetResult(sdd_result *r) {
    plat_mutex_lock(g_save.lock);
    int32_t ret =
        g_save.status != CD_FINISHED ? CD_ERR_NOT_FINISHED : !r ? CD_ERR_ARG_NULL : SCE_OK;
    if (ret == SCE_OK) {
        r->mode = g_sdd.mode;
        r->result = g_sdd.result;
        r->button_id = g_sdd.button_id;
        if (r->dir_name) {
            memset(r->dir_name, 0, sizeof(*r->dir_name));
            memcpy(r->dir_name->data, g_sdd.dir_name, strlen(g_sdd.dir_name));
        }
        if (r->param && g_sdd.dir_name[0]) hle_savedata_read_param(g_sdd.dir_name, r->param);
        r->user_data = g_sdd.user_data;
    }
    plat_mutex_unlock(g_save.lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataDialogClose(void) {
    plat_mutex_lock(g_save.lock);
    int32_t ret = g_save.status != CD_RUNNING ? CD_ERR_NOT_RUNNING : SCE_OK;
    if (ret == SCE_OK) sdd_finish(CD_RESULT_OK, BUTTON_INVALID);
    plat_mutex_unlock(g_save.lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataDialogTerminate(void) {
    plat_mutex_lock(g_save.lock);
    if (g_save.status == CD_RUNNING) sdd_finish(CD_RESULT_OK, BUTTON_INVALID);
    int32_t ret = g_save.status == CD_NONE ? CD_ERR_NOT_INITIALIZED : SCE_OK;
    g_save.status = CD_NONE;
    plat_mutex_unlock(g_save.lock);
    return ret;
}

static int32_t sdd_progress_check(uint32_t target) {
    if (g_save.status != CD_RUNNING) return CD_ERR_NOT_RUNNING;
    if (g_sdd.mode != SDD_MODE_PROGRESS_BAR) return CD_ERR_NOT_SUPPORTED;
    if (target != 0) return CD_ERR_PARAM_INVALID;
    return SCE_OK;
}

static SYSV int32_t ps4_sceSaveDataDialogProgressBarSetValue(uint32_t target, uint32_t rate) {
    (void)rate;
    return sdd_progress_check(target);
}

static SYSV int32_t ps4_sceSaveDataDialogProgressBarInc(uint32_t target, uint32_t delta) {
    (void)delta;
    return sdd_progress_check(target);
}

HLE_LIBRARY(hle_savedatadialog, "libSceSaveDataDialog",
    HLE(sceSaveDataDialogInitialize),
    HLE(sceSaveDataDialogIsReadyToDisplay),
    HLE(sceSaveDataDialogOpen),
    HLE(sceSaveDataDialogUpdateStatus),
    HLE(sceSaveDataDialogGetStatus),
    HLE(sceSaveDataDialogGetResult),
    HLE(sceSaveDataDialogClose),
    HLE(sceSaveDataDialogTerminate),
    HLE(sceSaveDataDialogProgressBarSetValue),
    HLE(sceSaveDataDialogProgressBarInc),
)
