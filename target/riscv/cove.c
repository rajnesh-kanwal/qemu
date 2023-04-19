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
#include "qom/object_interfaces.h"
#include "hw/boards.h"

#include "cove.h"

OBJECT_DEFINE_TYPE_WITH_INTERFACES(CoveGuest,
                                   cove_guest,
                                   COVE_GUEST,
                                   CONFIDENTIAL_GUEST_SUPPORT,
                                   { TYPE_USER_CREATABLE },
                                   { NULL })

static CoveGuest *cove_guest;

/* It's valid after kvm_arch_init()->cove_kvm_init() */
bool is_cove_vm(void)
{
    return !!cove_guest;
}

int cove_kvm_init(MachineState *ms, Error **errp)
{
    cove_guest = (CoveGuest *)object_dynamic_cast(OBJECT(ms->cgs),
                                                    TYPE_COVE_GUEST);

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
