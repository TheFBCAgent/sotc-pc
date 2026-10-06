/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared runtime state: the loaded boot image and its modules. */
#ifndef SOTC_RUNTIME_H
#define SOTC_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "bootimg.h"
#include "platform.h"

#define RT_MAX_MODULES 8

typedef struct guest_module {
    const char *name;
    uint32_t tls_modid;
    uint8_t *base;
    uint64_t size;
    uint8_t *init;          /* NULL if none */
    const uint8_t *tls_image;
    uint64_t tls_filesz, tls_memsz, tls_align;
    uint8_t *eh_frame_hdr, *eh_frame;
    uint64_t eh_frame_hdr_size, eh_frame_size;
} guest_module;

typedef struct runtime {
    uint8_t *image;
    uint64_t image_size;
    guest_module modules[RT_MAX_MODULES];
    uint32_t module_count;
    uint8_t *entry;
    void *procparam;
    uint64_t sdk_version;
    const char *game_dir;   /* host path mounted as /app0 */
    bool strict;            /* abort on unimplemented imports */
} runtime;

extern runtime g_rt;

/* loader.c */
bool loader_load(const char *path);
const guest_module *loader_module_at(const void *addr);
/* Describe a guest address as "module+0xoffset" (static buffer per call site). */
const char *loader_describe(const void *addr, char *buf, unsigned size);

/* sse4a.c: emulate EXTRQ/INSERTQ on CPUs without SSE4a. */
void sse4a_set_sites(const bootimg_patch *sites, uint32_t count);
bool sse4a_handle_fault(plat_fault *f);
void sse4a_extrq(uint64_t *dst, uint8_t len, uint8_t idx);
void sse4a_insertq(uint64_t *dst, uint64_t src, uint8_t len, uint8_t idx);

#endif
