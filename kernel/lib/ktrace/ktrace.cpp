// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <debug.h>
#include <err.h>
#include <string.h>

#include <arch/ops.h>
#include <arch/user_copy.h>
#include <kernel/cmdline.h>
#include <kernel/vm/vm_aspace.h>
#include <lib/ktrace.h>
#include <lk/init.h>
#include <magenta/user_thread.h>

#if __x86_64__
extern "C" uint64_t get_tsc_ticks_per_ms(void);
#define ktrace_timestamp() rdtsc();
#define ktrace_ticks_per_ms() get_tsc_ticks_per_ms()
#else
#include <platform.h>
#define ktrace_timestamp() current_time_hires()
#define ktrace_ticks_per_ms() (1000)
#endif

// implemented by thread.c
extern "C" void ktrace_report_live_threads(void);

typedef struct ktrace_state {
    // where the next record will be written
    int offset;

    // mask of groups we allow, 0 == tracing disabled
    int grpmask;

    // total size of the trace buffer
    uint32_t bufsize;

    // offset where tracing was stopped, 0 if tracing active
    uint32_t marker;

    // raw trace buffer
    uint8_t* buffer;
} ktrace_state_t;

static ktrace_state_t KTRACE_STATE;

int ktrace_read_user(void* ptr, uint32_t off, uint32_t len) {
    ktrace_state_t* ks = &KTRACE_STATE;

    // Buffer size is limited by the marker if set,
    // otherwise limited by offset (last written point).
    // Offset can end up pointing past the end, so clip
    // it to the actual buffer size to be safe.
    uint32_t max;
    if (ks->marker) {
        max = ks->marker;
    } else {
        max = ks->offset;
        if (max > ks->bufsize) {
            max = ks->bufsize;
        }
    }

    // null read is a query for trace buffer size
    if (ptr == NULL) {
        return max;
    }

    // constrain read to available buffer
    if (off >= max) {
        return 0;
    }
    if (len > (max - off)) {
        len = max - off;
    }

    if (arch_copy_to_user(ptr, ks->buffer + off, len) != NO_ERROR) {
        return ERR_INVALID_ARGS;
    }
    return len;
}

status_t ktrace_control(uint32_t action, uint32_t options) {
    ktrace_state_t* ks = &KTRACE_STATE;
    switch (action) {
    case KTRACE_ACTION_START:
        options = KTRACE_GRP_TO_MASK(options);
        ks->marker = 0;
        atomic_store(&ks->grpmask, options ? options : KTRACE_GRP_TO_MASK(KTRACE_GRP_ALL));
        ktrace_report_live_threads();
        break;
    case KTRACE_ACTION_STOP: {
        atomic_store(&ks->grpmask, 0);
        uint32_t n = ks->offset;
        if (n > ks->bufsize) {
            ks->marker = ks->bufsize;
        } else {
            ks->marker = n;
        }
        break;
    }
    case KTRACE_ACTION_REWIND:
        // roll back to just after the metadata
        atomic_store(&ks->offset, KTRACE_RECSIZE * 2);
        break;
    default:
        return ERR_INVALID_ARGS;
    }
    return NO_ERROR;
}

int trace_not_ready = 0;

void ktrace_init(unsigned level) {
    ktrace_state_t* ks = &KTRACE_STATE;

    uint32_t mb = cmdline_get_uint32("ktrace.bufsize", KTRACE_DEFAULT_BUFSIZE);
    uint32_t grpmask = cmdline_get_uint32("ktrace.grpmask", KTRACE_DEFAULT_GRPMASK);

    if (mb == 0) {
        dprintf(INFO, "ktrace: disabled\n");
        return;
    }

    mb *= (1024*1024);

    status_t status;
    VmAspace* aspace = VmAspace::kernel_aspace();
    if ((status = aspace->Alloc("ktrace", mb, (void**)&ks->buffer, 0, VMM_FLAG_COMMIT,
                                ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE)) < 0) {
        dprintf(INFO, "ktrace: cannot alloc buffer %d\n", status);
        return;
    }

    // The last packet written can overhang the end of the buffer,
    // so we reduce the reported size by the max size of a record
    ks->bufsize = mb - 256;

    dprintf(INFO, "ktrace: buffer at %p (%u bytes)\n", ks->buffer, mb);

    // write metadata to the first two event slots
    uint64_t n = ktrace_ticks_per_ms();
    ktrace_rec_32b_t* rec = (ktrace_rec_32b_t*) ks->buffer;
    rec[0].tag = TAG_VERSION;
    rec[0].a = KTRACE_VERSION;
    rec[1].tag = TAG_TICKS_PER_MS;
    rec[1].a = (uint32_t)n;
    rec[1].b = (uint32_t)(n >> 32);

    // enable tracing
    atomic_store(&ks->offset, KTRACE_RECSIZE * 2);
    atomic_store(&ks->grpmask, KTRACE_GRP_TO_MASK(grpmask));

    // report names of existing threads
    ktrace_report_live_threads();
}

void* ktrace_open(uint32_t tag) {
    ktrace_state_t* ks = &KTRACE_STATE;
    if (!(tag & atomic_load(&ks->grpmask))) {
        return NULL;
    }

    int off;
    if ((off = atomic_add(&ks->offset, KTRACE_LEN(tag))) >= (int)ks->bufsize) {
        // if we arrive at the end, stop
        atomic_store(&ks->grpmask, 0);
        return NULL;
    }

    ktrace_header_t* hdr = (ktrace_header_t*) (ks->buffer + off);
    hdr->ts = ktrace_timestamp();
    hdr->tag = tag;
    hdr->tid = (uint32_t)get_current_thread()->user_tid;
    return hdr + 1;
}

typedef struct {
    uint32_t id;
    uint32_t arg;
    char name[1];
} namerec_t;

void ktrace_name(uint32_t tag, uint32_t id, uint32_t arg, const char* name) {
    uint32_t len = static_cast<uint32_t>(strnlen(name, 31));

    // make size: sizeof(hdr) + sizeof(id) + sizeof(arg) + len + 1,
    // rounded up to multiple of 8
    tag = (tag & 0xFFFFFFF0) | ((KTRACE_HDRSIZE + 8 + len + 1 + 7) >> 3);

    namerec_t* rec = (namerec_t*) ktrace_open(tag);
    if (rec) {
        rec->id = id;
        rec->arg = arg;
        memcpy(rec->name, name, len);
        rec->name[len] = 0;
    }
}

LK_INIT_HOOK(ktrace, ktrace_init, LK_INIT_LEVEL_APPS - 1);