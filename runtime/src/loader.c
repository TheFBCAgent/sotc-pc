/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Load a boot image: map, copy segments, resolve imports, relocate, patch, protect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"
#include "log.h"
#include "runtime.h"

runtime g_rt;

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

/* Preferred base: where shadPS4 loads the eboot. The image is position independent. */
#define PREFERRED_BASE 0x400000ull
#define STUB_SIZE 32

static const char *g_strings;
static const bootimg_import *g_imports;
static uint32_t g_import_count;
static uint8_t *g_seen;  /* per import: logged once */

static uint8_t *read_file(const char *path, uint64_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek64(f, 0, SEEK_END);
    int64_t size = ftell64(f);
    fseek64(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size_out = (uint64_t)size;
    return buf;
}

static const char *import_label(uint32_t i, char *buf, unsigned size) {
    const bootimg_import *imp = &g_imports[i];
    const char *name = g_strings + imp->name;
    snprintf(buf, size, "%s:%s", g_strings + imp->library, *name ? name : g_strings + imp->nid);
    return buf;
}

/* Called (SysV) by every unresolved function import's stub. */
static SYSV uint64_t trap_unresolved(uint32_t index, uint64_t arg0) {
    char label[160];
    if (g_rt.strict)
        log_fatal("unimplemented %s (arg0=%#llx), stopping (SOTC_STRICT)",
                  import_label(index, label, sizeof(label)), (unsigned long long)arg0);
    if (!g_seen[index]) {
        g_seen[index] = 1;
        log_warn("unimplemented %s (arg0=%#llx) -> 0", import_label(index, label, sizeof(label)),
                 (unsigned long long)arg0);
    }
    return 0;
}

/* mov rsi, rdi; mov edi, imm32; movabs rax, imm64; jmp rax */
static void emit_stub(uint8_t *p, uint32_t index) {
    static const uint8_t tmpl[] = {0x48, 0x89, 0xFE, 0xBF, 0, 0, 0, 0, 0x48, 0xB8,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    uint64_t handler = (uint64_t)(uintptr_t)trap_unresolved;
    memset(p, 0xCC, STUB_SIZE);
    memcpy(p, tmpl, sizeof(tmpl));
    memcpy(p + 4, &index, 4);
    memcpy(p + 10, &handler, 8);
}

static int prot_from_elf(uint32_t flags) {
    int p = 0;
    if (flags & BOOTIMG_PROT_R) p |= PLAT_PROT_R;
    if (flags & BOOTIMG_PROT_W) p |= PLAT_PROT_W;
    if (flags & BOOTIMG_PROT_X) p |= PLAT_PROT_X;
    return p;
}

static bool apply_patches(const bootimg_patch *patches, uint32_t count) {
    uint32_t fs = 0, sse4a = 0;
    for (uint32_t i = 0; i < count; i++) {
        const bootimg_patch *p = &patches[i];
        uint8_t *insn = g_rt.image + p->offset;
        if (p->kind == BOOTIMG_PATCH_FS) {
            if (p->c != BOOTIMG_FS_ABS || p->b == 0xFF || insn[p->a] != 0x64) {
                char where[96];
                log_error("fs access at %s has an unsupported form",
                          loader_describe(insn, where, sizeof(where)));
                return false;
            }
            uint32_t disp = plat_tls_patch_disp((int32_t)p->disp);
            if (disp == 0 && p->disp != 0) {
                log_error("fs:[%#llx] cannot be rewritten on this platform", (long long)p->disp);
                return false;
            }
            insn[p->a] = 0x65; /* fs -> gs */
            memcpy(insn + p->b, &disp, 4);
            fs++;
        } else {
            sse4a++;
        }
    }
    log_info("patched %u fs TLS reads; %u SSE4a instructions", fs, sse4a);
    if (sse4a && !plat_cpu_has_sse4a()) {
#ifdef _WIN32
        /* Fault-based emulation would clobber the guest's red zone on Windows. */
        log_error("this CPU lacks SSE4a; emulation on Windows is not implemented yet");
        return false;
#else
        log_warn("this CPU lacks SSE4a; emulating %u sites on SIGILL", sse4a);
        sse4a_set_sites(patches, count);
#endif
    }
    return true;
}

bool loader_load(const char *path) {
    uint64_t file_size;
    uint8_t *file = read_file(path, &file_size);
    if (!file) {
        log_error("cannot read boot image %s", path);
        return false;
    }
    const bootimg_header *h = (const bootimg_header *)file;
    if (file_size < sizeof(*h) || memcmp(h->magic, BOOTIMG_MAGIC, 8) != 0 ||
        h->version != BOOTIMG_VERSION) {
        log_error("%s is not a version %d boot image; rerun tools/sotcconv", path,
                  BOOTIMG_VERSION);
        return false;
    }
    if (h->module_count > RT_MAX_MODULES) {
        log_error("too many modules (%u)", h->module_count);
        return false;
    }

    const bootimg_module *mods = (const bootimg_module *)(h + 1);
    const bootimg_segment *segs = (const bootimg_segment *)(mods + h->module_count);
    const bootimg_reloc *relocs = (const bootimg_reloc *)(segs + h->segment_count);
    g_imports = (const bootimg_import *)(relocs + h->reloc_count);
    const bootimg_patch *patches = (const bootimg_patch *)(g_imports + h->import_count);
    g_strings = (const char *)(patches + h->patch_count);
    g_import_count = h->import_count;
    g_seen = calloc(g_import_count ? g_import_count : 1, 1);

    /* Map the whole image read-write first; protections are applied at the end. */
    uint8_t *image = plat_map(PREFERRED_BASE, h->image_size, PLAT_PROT_R | PLAT_PROT_W, true);
    if (!image) image = plat_map(0, h->image_size, PLAT_PROT_R | PLAT_PROT_W, false);
    if (!image) {
        log_error("cannot map %#llx bytes for the image", (unsigned long long)h->image_size);
        return false;
    }
    g_rt.image = image;
    g_rt.image_size = h->image_size;
    g_rt.entry = image + h->entry;
    g_rt.procparam = h->procparam ? image + h->procparam : NULL;
    g_rt.sdk_version = h->sdk_version;
    log_info("image mapped at %p (%.1f MB), sdk %#llx", (void *)image, h->image_size / 1e6,
             (unsigned long long)h->sdk_version);

    g_rt.module_count = h->module_count;
    for (uint32_t i = 0; i < h->module_count; i++) {
        const bootimg_module *m = &mods[i];
        guest_module *gm = &g_rt.modules[i];
        gm->name = g_strings + m->name;
        gm->tls_modid = m->tls_modid;
        gm->base = image + m->base;
        gm->size = m->size;
        gm->init = m->init == BOOTIMG_NO_INIT ? NULL : image + m->init;
        gm->tls_image = image + m->tls_image;
        gm->tls_filesz = m->tls_filesz;
        gm->tls_memsz = m->tls_memsz;
        gm->tls_align = m->tls_align;
        gm->eh_frame_hdr = m->eh_frame_hdr ? image + m->eh_frame_hdr : NULL;
        gm->eh_frame_hdr_size = m->eh_frame_hdr_size;
        gm->eh_frame = m->eh_frame ? image + m->eh_frame : NULL;
        gm->eh_frame_size = m->eh_frame_size;
        log_info("  %-12s at %p size %#llx tls %llu", gm->name, (void *)gm->base,
                 (unsigned long long)gm->size, (unsigned long long)gm->tls_memsz);
    }

    for (uint32_t i = 0; i < h->segment_count; i++)
        memcpy(image + segs[i].image_offset, file + segs[i].file_offset, segs[i].filesz);

    /* Resolve imports: HLE implementation, or a logging stub / zeroed object. */
    uint64_t *import_addr = calloc(g_import_count ? g_import_count : 1, sizeof(uint64_t));
    uint8_t *stubs = plat_map(0, (size_t)g_import_count * STUB_SIZE + 0x1000,
                              PLAT_PROT_R | PLAT_PROT_W, false);
    static uint8_t zero_objects[64][256];
    uint32_t resolved = 0, stubbed = 0, zero_used = 0;
    for (uint32_t i = 0; i < g_import_count; i++) {
        const bootimg_import *imp = &g_imports[i];
        void *addr = hle_lookup(g_strings + imp->library, g_strings + imp->name);
        if (addr) {
            resolved++;
        } else if (imp->kind == BOOTIMG_IMPORT_FUNC) {
            emit_stub(stubs + (size_t)i * STUB_SIZE, i);
            addr = stubs + (size_t)i * STUB_SIZE;
            stubbed++;
        } else {
            char label[160];
            if (zero_used == 64) log_fatal("too many unresolved data imports");
            addr = zero_objects[zero_used++];
            log_warn("unresolved data import %s -> zeroed object",
                     import_label(i, label, sizeof(label)));
        }
        import_addr[i] = (uint64_t)(uintptr_t)addr;
    }
    plat_protect(stubs, (size_t)g_import_count * STUB_SIZE + 0x1000, PLAT_PROT_R | PLAT_PROT_X);
    log_info("imports: %u implemented, %u stubbed, %u data stubbed", resolved, stubbed, zero_used);

    for (uint32_t i = 0; i < h->reloc_count; i++) {
        const bootimg_reloc *r = &relocs[i];
        uint64_t value;
        switch (r->kind) {
        case BOOTIMG_REL_IMAGE: value = (uint64_t)(uintptr_t)image + r->value + (uint64_t)r->addend; break;
        case BOOTIMG_REL_IMPORT: value = import_addr[r->value] + (uint64_t)r->addend; break;
        case BOOTIMG_REL_RAW: value = r->value + (uint64_t)r->addend; break;
        default: log_error("bad relocation kind %u", r->kind); return false;
        }
        memcpy(image + r->offset, &value, 8);
    }
    log_info("applied %u relocations", h->reloc_count);
    free(import_addr);

    if (!apply_patches(patches, h->patch_count)) return false;

    for (uint32_t i = 0; i < h->segment_count; i++) {
        uint64_t start = segs[i].image_offset & ~0x3FFFull;
        uint64_t end = (segs[i].image_offset + segs[i].memsz + 0x3FFF) & ~0x3FFFull;
        plat_protect(image + start, end - start, prot_from_elf(segs[i].prot));
    }
    plat_flush_icache(image, h->image_size);

    /* The tables (strings, imports, patches) stay referenced; keep the file. */
    return true;
}

const guest_module *loader_module_at(const void *addr) {
    const uint8_t *p = addr;
    for (uint32_t i = 0; i < g_rt.module_count; i++) {
        const guest_module *m = &g_rt.modules[i];
        if (p >= m->base && p < m->base + m->size) return m;
    }
    return NULL;
}

const char *loader_describe(const void *addr, char *buf, unsigned size) {
    const guest_module *m = loader_module_at(addr);
    if (m)
        snprintf(buf, size, "%s+%#llx", m->name,
                 (unsigned long long)((const uint8_t *)addr - m->base));
    else
        snprintf(buf, size, "%p", addr);
    return buf;
}
