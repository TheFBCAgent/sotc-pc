/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceSysmodule: the game asks for system libraries by id. Everything it can
 * use is either linked into the boot image or implemented here, so loading
 * always succeeds. */
#include "../log.h"
#include "hle.h"

static SYSV int32_t ps4_sceSysmoduleLoadModule(uint16_t id) {
    log_debug("sceSysmoduleLoadModule(%#x)", id);
    return SCE_OK;
}

static SYSV int32_t ps4_sceSysmoduleUnloadModule(uint16_t id) {
    log_debug("sceSysmoduleUnloadModule(%#x)", id);
    return SCE_OK;
}

static SYSV int32_t ps4_sceSysmoduleLoadModuleInternalWithArg(uint32_t id, int32_t argc,
                                                             const void *argv, uint64_t unk,
                                                             int32_t *result) {
    (void)argc; (void)argv; (void)unk;
    log_debug("sceSysmoduleLoadModuleInternalWithArg(%#x)", id);
    if (result) *result = 0;
    return SCE_OK;
}

const hle_library hle_sysmodule = {"libSceSysmodule", (const hle_symbol[]){
    HLE(sceSysmoduleLoadModule),
    HLE(sceSysmoduleUnloadModule),
    HLE(sceSysmoduleLoadModuleInternalWithArg),
    HLE_END,
}};
