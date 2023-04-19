#ifndef QEMU_RISCV_COVE_H
#define QEMU_RISCV_COVE_H

#include "exec/confidential-guest-support.h"

#define TYPE_COVE_GUEST "cove-guest"
#define COVE_GUEST(obj)  OBJECT_CHECK(CoveGuest, (obj), TYPE_COVE_GUEST)

typedef struct CoveGuestClass {
    ConfidentialGuestSupportClass parent_class;
} CoveGuestClass;

typedef struct CoveGuest {
    ConfidentialGuestSupport parent_obj;

    uint64_t attributes;
} CoveGuest;

int cove_kvm_init(MachineState *ms, Error **errp);

#endif /* QEMU_RISCV_COVE_H */
