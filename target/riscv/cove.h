#ifndef QEMU_RISCV_COVE_H
#define QEMU_RISCV_COVE_H

#ifndef CONFIG_USER_ONLY
#include CONFIG_DEVICES /* CONFIG_COVE */
#endif

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

#ifdef CONFIG_COVE
bool is_cove_vm(void);
#else
#define is_tdx_vm() 0
#endif /* CONFIG_COVE */

int cove_kvm_init(MachineState *ms, Error **errp);

#endif /* QEMU_RISCV_COVE_H */
