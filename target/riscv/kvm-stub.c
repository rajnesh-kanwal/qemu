/*
 * QEMU KVM RISC-V specific function stubs
 *
 * Copyright (c) 2020 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "kvm_riscv.h"

int kvm_get_vm_type(MachineState *ms, const char *vm_type)
{
    return 0;
}

int kvm_cove_measure_region(KVMState *s, unsigned long uaddr,
                             unsigned long gpa, unsigned long rsize)
{
    return 0;
}

void kvm_riscv_reset_vcpu(RISCVCPU *cpu)
{
    abort();
}

void kvm_riscv_set_irq(RISCVCPU *cpu, int irq, int level)
{
    abort();
}
