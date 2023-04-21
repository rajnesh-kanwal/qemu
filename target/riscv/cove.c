/*
 * QEMU CoVE support
 *
 * Copyright Rivos Inc.
 *
 * Author:
 *      Rajnesh Kanwal <rkanwal@rivosinc.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory
 *
 */

#include "qemu/osdep.h"
#include "sysemu/runstate.h"
#include "qom/object_interfaces.h"
#include "hw/boards.h"
#include "sysemu/kvm_int.h"
#include "cove.h"
#include "qapi/error.h"

OBJECT_DEFINE_TYPE_WITH_INTERFACES(CoveGuest,
                                   cove_guest,
                                   COVE_GUEST,
                                   CONFIDENTIAL_GUEST_SUPPORT,
                                   { TYPE_USER_CREATABLE },
                                   { NULL })

#define PAGE_SIZE qemu_real_host_page_size()

static CoveGuest *cove_guest;

struct CoveImage {
    hwaddr base;
    size_t size;
};

/* This is to maintain a list of CoveImage structure containing information
 * of the images loaded for the VM. Given images are part of the rom and
 * loaded at a later stage into RAM. The measurement process must be done
 * once the images are loaded into RAM.
 */
static GSList *cove_images;

/* It's valid after kvm_arch_init()->cove_kvm_init() */
bool is_cove_vm(void)
{
    return !!cove_guest;
}

static int kvm_cove_measure_region(KVMState *s, unsigned long uaddr,
                             unsigned long gpa, unsigned long rsize)
{
    int ret;

    if(!is_cove_vm())
        return 0;

    struct kvm_riscv_cove_measure_region mr = {
        .userspace_addr = uaddr,
        .gpa = gpa,
        .size = rsize,
    };

    ret = kvm_vm_ioctl(s, KVM_RISCV_COVE_MEASURE_REGION, &mr);
    if (ret < 0) {
        error_setg_errno(&error_fatal, -ret,
                    "COVE: ioctl(KVM_RISCV_COVE_MEASURE_REGION) failed: %d %s\n", -ret, strerror(-ret));
    }

    return ret;
}

static void cove_measure_images(gpointer data, gpointer user_data)
{
    struct CoveImage *image = data;
    void *host_addr;
    int ret;

    ret = kvm_host_addr_from_physical_memory(kvm_state, image->base, &host_addr);
    if (ret == 0) {
        error_setg_errno(&error_fatal, -ret,
                         "COVE: failed to translate GPA range (0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx")",
                         image->base, image->size);
        return;
    }
    
    uint64_t *a = host_addr;
    fprintf(stderr, "Addr %p %lx %lx %lx %lx %lx\n", host_addr, image->base, image->size, a[0], a[1], a[2]);
    
    ret = kvm_cove_measure_region(kvm_state, (unsigned long)host_addr, (unsigned long)image->base, image->size);
    if (ret) {
        error_setg_errno(&error_fatal, -ret,
                         "RME: failed to measure GPA range (0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx")",
                         image->base, image->size);
    }
}

static void kvm_cove_vm_state_change(void *opaque, bool running, RunState state)
{
    if (state != RUN_STATE_RUNNING) {
        return;
    }

    g_slist_foreach(cove_images, cove_measure_images, NULL);
    g_slist_free_full(g_steal_pointer(&cove_images), g_free);
}

void kvm_cove_add_blob(hwaddr base, size_t size)
{
    struct CoveImage *image;

    if (!is_cove_vm()) {
        return;
    }

    base = QEMU_ALIGN_DOWN(base, PAGE_SIZE);
    size = QEMU_ALIGN_UP(size, PAGE_SIZE);

    image = g_malloc0(sizeof(*image));
    image->base = base;
    image->size = size;

    cove_images = g_slist_prepend(cove_images, image);
}


int cove_kvm_init(MachineState *ms, Error **errp)
{
    cove_guest = (CoveGuest *)object_dynamic_cast(OBJECT(ms->cgs),
                                                    TYPE_COVE_GUEST);

    if (!cove_guest) {
        return 0;
    }

    qemu_add_vm_change_state_handler(kvm_cove_vm_state_change, NULL);
    ms->cgs->ready = true;

    return 0;
}

static void cove_guest_init(Object *obj)
{
    CoveGuest *cove = COVE_GUEST(obj);

    cove->attributes = 0;
}

static void cove_guest_finalize(Object *obj)
{
}

static void cove_guest_class_init(ObjectClass *oc, void *data)
{
}
