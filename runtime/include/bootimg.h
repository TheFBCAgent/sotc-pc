/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Boot image format written by tools/sotcconv/sotcconv.py. Keep the two in sync. */
#ifndef SOTC_BOOTIMG_H
#define SOTC_BOOTIMG_H

#include <stdint.h>

#define BOOTIMG_MAGIC "SOTCIMG1"
#define BOOTIMG_VERSION 1

/* File layout: header, modules[], segments[], relocs[], imports[], patches[],
 * strings, padding to 16, segment data. All offsets in the tables below are
 * image offsets (relative to where the image is mapped) unless named file_*. */
typedef struct bootimg_header {
    char magic[8];
    uint32_t version;
    uint32_t module_count;
    uint64_t image_size;
    uint64_t entry;        /* eboot entry point */
    uint64_t procparam;    /* eboot PT_SCE_PROCPARAM, 0 if none */
    uint64_t sdk_version;
    uint64_t strings_size;
    uint32_t segment_count;
    uint32_t reloc_count;
    uint32_t import_count;
    uint32_t patch_count;
    uint32_t reserved[2];
    uint64_t data_offset;  /* file offset of the segment data */
} bootimg_header;

#define BOOTIMG_NO_INIT UINT64_MAX

typedef struct bootimg_module {
    uint32_t name;         /* string offset */
    uint32_t tls_modid;    /* 1-based; module 1 (eboot) owns the static TLS block */
    uint64_t base;
    uint64_t size;
    uint64_t init;         /* DT_INIT, or BOOTIMG_NO_INIT */
    uint64_t tls_image;    /* PT_TLS init image (image offset) */
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;
    uint64_t eh_frame_hdr; /* image offsets/sizes of unwind info, 0 if none */
    uint64_t eh_frame_hdr_size;
    uint64_t eh_frame;
    uint64_t eh_frame_size;
} bootimg_module;

/* Protection bits match ELF p_flags. */
#define BOOTIMG_PROT_X 1
#define BOOTIMG_PROT_W 2
#define BOOTIMG_PROT_R 4

typedef struct bootimg_segment {
    uint64_t image_offset;
    uint64_t file_offset;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t prot;
    uint32_t pad;
} bootimg_segment;

enum {
    BOOTIMG_REL_IMAGE = 0,  /* *(u64*)(base+offset) = base + value + addend */
    BOOTIMG_REL_IMPORT = 1, /* *(u64*)(base+offset) = import[value] + addend */
    BOOTIMG_REL_RAW = 2,    /* *(u64*)(base+offset) = value + addend */
};

typedef struct bootimg_reloc {
    uint64_t offset;
    uint64_t value;
    int64_t addend;
    uint32_t kind;
    uint32_t pad;
} bootimg_reloc;

enum {
    BOOTIMG_IMPORT_FUNC = 1,
    BOOTIMG_IMPORT_DATA = 2,
};

typedef struct bootimg_import {
    uint32_t library;      /* string offsets */
    uint32_t nid;
    uint32_t name;         /* "" when the converter had no name database */
    uint32_t kind;
} bootimg_import;

enum {
    BOOTIMG_PATCH_FS = 1,          /* a=prefix offset, b=disp32 offset (0xFF none), c=form */
    BOOTIMG_PATCH_EXTRQ_IMM = 2,   /* a=dst xmm, c=len, d=idx */
    BOOTIMG_PATCH_EXTRQ_REG = 3,   /* a=dst xmm, b=src xmm */
    BOOTIMG_PATCH_INSERTQ_IMM = 4, /* a=dst xmm, b=src xmm, c=len, d=idx */
    BOOTIMG_PATCH_INSERTQ_REG = 5, /* a=dst xmm, b=src xmm */
};

enum {
    BOOTIMG_FS_ABS = 1,   /* fs:[disp32], no base or index register */
    BOOTIMG_FS_OTHER = 2,
};

typedef struct bootimg_patch {
    uint64_t offset;
    uint8_t kind;
    uint8_t length;
    uint8_t a, b, c, d;
    uint8_t pad[2];
    int64_t disp;
} bootimg_patch;

_Static_assert(sizeof(bootimg_header) == 88, "bootimg_header layout");
_Static_assert(sizeof(bootimg_module) == 96, "bootimg_module layout");
_Static_assert(sizeof(bootimg_segment) == 40, "bootimg_segment layout");
_Static_assert(sizeof(bootimg_reloc) == 32, "bootimg_reloc layout");
_Static_assert(sizeof(bootimg_import) == 16, "bootimg_import layout");
_Static_assert(sizeof(bootimg_patch) == 24, "bootimg_patch layout");

#endif
