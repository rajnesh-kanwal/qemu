/*
 * RISC-V implementation of KVM hooks
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
#include <sys/ioctl.h>

#include <linux/kvm.h>

#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/kvm_int.h"
#include "cpu.h"
#include "trace.h"
#include "hw/pci/pci.h"
#include "exec/memattrs.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/intc/riscv_imsic.h"
#include "qemu/log.h"
#include "hw/loader.h"
#include "kvm_riscv.h"
#include "sbi_ecall_interface.h"
#include "chardev/char-fe.h"
#include "migration/migration.h"
#include "sysemu/runstate.h"
#include "cove.h"

int kvm_get_vm_type(MachineState *ms, const char *vm_type)
{
    if (ms->cgs && object_dynamic_cast(OBJECT(ms->cgs), TYPE_COVE_GUEST)) {
        fprintf(stderr, "cove type");
        return KVM_VM_TYPE_RISCV_COVE;
    }

    return 0;
}

static uint64_t kvm_riscv_reg_id(CPURISCVState *env, uint64_t type,
                                 uint64_t idx)
{
    uint64_t id = KVM_REG_RISCV | type | idx;

    switch (riscv_cpu_mxl(env)) {
    case MXL_RV32:
        id |= KVM_REG_SIZE_U32;
        break;
    case MXL_RV64:
        id |= KVM_REG_SIZE_U64;
        break;
    default:
        g_assert_not_reached();
    }
    return id;
}

#define RISCV_CORE_REG(env, name)  kvm_riscv_reg_id(env, KVM_REG_RISCV_CORE, \
                 KVM_REG_RISCV_CORE_REG(name))

#define RISCV_CSR_REG(env, name)  kvm_riscv_reg_id(env, KVM_REG_RISCV_CSR, \
                 KVM_REG_RISCV_CSR_REG(name))

#define RISCV_TIMER_REG(env, name)  kvm_riscv_reg_id(env, KVM_REG_RISCV_TIMER, \
                 KVM_REG_RISCV_TIMER_REG(name))

#define RISCV_FP_F_REG(env, idx)  kvm_riscv_reg_id(env, KVM_REG_RISCV_FP_F, idx)

#define RISCV_FP_D_REG(env, idx)  kvm_riscv_reg_id(env, KVM_REG_RISCV_FP_D, idx)

#define KVM_RISCV_GET_CSR(cs, env, csr, reg) \
    do { \
        int ret = kvm_get_one_reg(cs, RISCV_CSR_REG(env, csr), &reg); \
        if (ret) { \
            return ret; \
        } \
    } while (0)

#define KVM_RISCV_SET_CSR(cs, env, csr, reg) \
    do { \
        int ret = kvm_set_one_reg(cs, RISCV_CSR_REG(env, csr), &reg); \
        if (ret) { \
            return ret; \
        } \
    } while (0)

#define KVM_RISCV_GET_TIMER(cs, env, name, reg) \
    do { \
        int ret = kvm_get_one_reg(cs, RISCV_TIMER_REG(env, name), &reg); \
        if (ret) { \
            abort(); \
        } \
    } while (0)

#define KVM_RISCV_SET_TIMER(cs, env, name, reg) \
    do { \
        int ret = kvm_set_one_reg(cs, RISCV_TIMER_REG(env, time), &reg); \
        if (ret) { \
            abort(); \
        } \
    } while (0)

static int kvm_riscv_get_regs_core(CPUState *cs)
{
    int ret = 0;
    int i;
    target_ulong reg;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    ret = kvm_get_one_reg(cs, RISCV_CORE_REG(env, regs.pc), &reg);
    if (ret) {
        return ret;
    }
    env->pc = reg;

    for (i = 1; i < 32; i++) {
        uint64_t id = kvm_riscv_reg_id(env, KVM_REG_RISCV_CORE, i);
        ret = kvm_get_one_reg(cs, id, &reg);
        if (ret) {
            return ret;
        }
        env->gpr[i] = reg;
    }

    return ret;
}

static int kvm_riscv_put_regs_core(CPUState *cs)
{
    int ret = 0;
    int i;
    target_ulong reg;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    reg = env->pc;
    ret = kvm_set_one_reg(cs, RISCV_CORE_REG(env, regs.pc), &reg);
    if (ret) {
        return ret;
    }

    for (i = 1; i < 32; i++) {
        uint64_t id = kvm_riscv_reg_id(env, KVM_REG_RISCV_CORE, i);
        reg = env->gpr[i];
        ret = kvm_set_one_reg(cs, id, &reg);
        if (ret) {
            return ret;
        }
    }

    return ret;
}

static int kvm_riscv_get_regs_csr(CPUState *cs)
{
    int ret = 0;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    KVM_RISCV_GET_CSR(cs, env, sstatus, env->mstatus);
    KVM_RISCV_GET_CSR(cs, env, sie, env->mie);
    KVM_RISCV_GET_CSR(cs, env, stvec, env->stvec);
    KVM_RISCV_GET_CSR(cs, env, sscratch, env->sscratch);
    KVM_RISCV_GET_CSR(cs, env, sepc, env->sepc);
    KVM_RISCV_GET_CSR(cs, env, scause, env->scause);
    KVM_RISCV_GET_CSR(cs, env, stval, env->stval);
    KVM_RISCV_GET_CSR(cs, env, sip, env->mip);
    KVM_RISCV_GET_CSR(cs, env, satp, env->satp);
    return ret;
}

static int kvm_riscv_put_regs_csr(CPUState *cs)
{
    int ret = 0;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    KVM_RISCV_SET_CSR(cs, env, sstatus, env->mstatus);
    KVM_RISCV_SET_CSR(cs, env, sie, env->mie);
    KVM_RISCV_SET_CSR(cs, env, stvec, env->stvec);
    KVM_RISCV_SET_CSR(cs, env, sscratch, env->sscratch);
    KVM_RISCV_SET_CSR(cs, env, sepc, env->sepc);
    KVM_RISCV_SET_CSR(cs, env, scause, env->scause);
    KVM_RISCV_SET_CSR(cs, env, stval, env->stval);
    KVM_RISCV_SET_CSR(cs, env, sip, env->mip);
    KVM_RISCV_SET_CSR(cs, env, satp, env->satp);

    return ret;
}

static int kvm_riscv_get_regs_fp(CPUState *cs)
{
    int ret = 0;
    int i;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    if (riscv_has_ext(env, RVD)) {
        uint64_t reg;
        for (i = 0; i < 32; i++) {
            ret = kvm_get_one_reg(cs, RISCV_FP_D_REG(env, i), &reg);
            if (ret) {
                return ret;
            }
            env->fpr[i] = reg;
        }
        return ret;
    }

    if (riscv_has_ext(env, RVF)) {
        uint32_t reg;
        for (i = 0; i < 32; i++) {
            ret = kvm_get_one_reg(cs, RISCV_FP_F_REG(env, i), &reg);
            if (ret) {
                return ret;
            }
            env->fpr[i] = reg;
        }
        return ret;
    }

    return ret;
}

static int kvm_riscv_put_regs_fp(CPUState *cs)
{
    int ret = 0;
    int i;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    if (riscv_has_ext(env, RVD)) {
        uint64_t reg;
        for (i = 0; i < 32; i++) {
            reg = env->fpr[i];
            ret = kvm_set_one_reg(cs, RISCV_FP_D_REG(env, i), &reg);
            if (ret) {
                return ret;
            }
        }
        return ret;
    }

    if (riscv_has_ext(env, RVF)) {
        uint32_t reg;
        for (i = 0; i < 32; i++) {
            reg = env->fpr[i];
            ret = kvm_set_one_reg(cs, RISCV_FP_F_REG(env, i), &reg);
            if (ret) {
                return ret;
            }
        }
        return ret;
    }

    return ret;
}

static void kvm_riscv_get_regs_timer(CPUState *cs)
{
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    if (env->kvm_timer_dirty) {
        return;
    }

    KVM_RISCV_GET_TIMER(cs, env, time, env->kvm_timer_time);
    KVM_RISCV_GET_TIMER(cs, env, compare, env->kvm_timer_compare);
    KVM_RISCV_GET_TIMER(cs, env, state, env->kvm_timer_state);
    KVM_RISCV_GET_TIMER(cs, env, frequency, env->kvm_timer_frequency);

    env->kvm_timer_dirty = true;
}

static void kvm_riscv_put_regs_timer(CPUState *cs)
{
    uint64_t reg;
    CPURISCVState *env = &RISCV_CPU(cs)->env;

    if (!env->kvm_timer_dirty) {
        return;
    }

    KVM_RISCV_SET_TIMER(cs, env, time, env->kvm_timer_time);
    KVM_RISCV_SET_TIMER(cs, env, compare, env->kvm_timer_compare);

    /*
     * To set register of RISCV_TIMER_REG(state) will occur a error from KVM
     * on env->kvm_timer_state == 0, It's better to adapt in KVM, but it
     * doesn't matter that adaping in QEMU now.
     * TODO If KVM changes, adapt here.
     */
    if (env->kvm_timer_state) {
        KVM_RISCV_SET_TIMER(cs, env, state, env->kvm_timer_state);
    }

    /*
     * For now, migration will not work between Hosts with different timer
     * frequency. Therefore, we should check whether they are the same here
     * during the migration.
     */
    if (migration_is_running(migrate_get_current()->state)) {
        KVM_RISCV_GET_TIMER(cs, env, frequency, reg);
        if (reg != env->kvm_timer_frequency) {
            error_report("Dst Hosts timer frequency != Src Hosts");
        }
    }

    env->kvm_timer_dirty = false;
}

const KVMCapabilityInfo kvm_arch_required_capabilities[] = {
    KVM_CAP_LAST_INFO
};

int kvm_arch_get_registers(CPUState *cs)
{
    int ret = 0;

    ret = kvm_riscv_get_regs_core(cs);
    if (ret) {
        return ret;
    }

    ret = kvm_riscv_get_regs_csr(cs);
    if (ret) {
        return ret;
    }

    ret = kvm_riscv_get_regs_fp(cs);
    if (ret) {
        return ret;
    }

    return ret;
}

int kvm_arch_put_registers(CPUState *cs, int level)
{
    int ret = 0;

    ret = kvm_riscv_put_regs_core(cs);
    if (ret) {
        return ret;
    }

    ret = kvm_riscv_put_regs_csr(cs);
    if (ret) {
        return ret;
    }

    ret = kvm_riscv_put_regs_fp(cs);
    if (ret) {
        return ret;
    }

    return ret;
}

int kvm_arch_release_virq_post(int virq)
{
    return 0;
}

int kvm_arch_fixup_msi_route(struct kvm_irq_routing_entry *route,
                             uint64_t address, uint32_t data, PCIDevice *dev)
{
    return 0;
}

int kvm_arch_destroy_vcpu(CPUState *cs)
{
    return 0;
}

unsigned long kvm_arch_vcpu_id(CPUState *cpu)
{
    return cpu->cpu_index;
}

static void kvm_riscv_vm_state_change(void *opaque, bool running,
                                      RunState state)
{
    CPUState *cs = opaque;

    if (running) {
        kvm_riscv_put_regs_timer(cs);
    } else {
        kvm_riscv_get_regs_timer(cs);
    }
}

void kvm_arch_init_irq_routing(KVMState *s)
{
}

int kvm_arch_init_vcpu(CPUState *cs)
{
    int ret = 0;
    target_ulong isa;
    RISCVCPU *cpu = RISCV_CPU(cs);
    CPURISCVState *env = &cpu->env;
    uint64_t id;

    qemu_add_vm_change_state_handler(kvm_riscv_vm_state_change, cs);

    id = kvm_riscv_reg_id(env, KVM_REG_RISCV_CONFIG,
                          KVM_REG_RISCV_CONFIG_REG(isa));
    ret = kvm_get_one_reg(cs, id, &isa);
    if (ret) {
        return ret;
    }
    env->misa_ext = isa;

    return ret;
}

int kvm_arch_msi_data_to_gsi(uint32_t data)
{
    abort();
}

int kvm_arch_add_msi_route_post(struct kvm_irq_routing_entry *route,
                                int vector, PCIDevice *dev)
{
    return 0;
}

int kvm_arch_init(MachineState *ms, KVMState *s)
{
    Error *local_err = NULL;

    int ret = cove_kvm_init(ms, &local_err);
    if (ret < 0) {
        error_report_err(local_err);
        return ret;
    }

    return 0;
}

int kvm_arch_irqchip_create(KVMState *s)
{
    if (kvm_kernel_irqchip_split()) {
        error_report("-machine kernel_irqchip=split is not supported "
                     "on RISC-V.");
        exit(1);
    }

    /*
     * If we can create the VAIA using the newer device control API, we
     * let the device do this when it initializes itself, otherwise we
     * fall back to the old API
     */
    return kvm_check_extension(s, KVM_CAP_DEVICE_CTRL);
}

int kvm_arch_process_async_events(CPUState *cs)
{
    return 0;
}

void kvm_arch_pre_run(CPUState *cs, struct kvm_run *run)
{
}

MemTxAttrs kvm_arch_post_run(CPUState *cs, struct kvm_run *run)
{
    return MEMTXATTRS_UNSPECIFIED;
}

bool kvm_arch_stop_on_emulation_error(CPUState *cs)
{
    return true;
}

static int kvm_riscv_handle_sbi(CPUState *cs, struct kvm_run *run)
{
    int ret = 0;
    unsigned char ch;
    switch (run->riscv_sbi.extension_id) {
    case SBI_EXT_0_1_CONSOLE_PUTCHAR:
        ch = run->riscv_sbi.args[0];
        qemu_chr_fe_write(serial_hd(0)->be, &ch, sizeof(ch));
        break;
    case SBI_EXT_0_1_CONSOLE_GETCHAR:
        ret = qemu_chr_fe_read_all(serial_hd(0)->be, &ch, sizeof(ch));
        if (ret == sizeof(ch)) {
            run->riscv_sbi.ret[0] = ch;
        } else {
            run->riscv_sbi.ret[0] = -1;
        }
        ret = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: un-handled SBI EXIT, specific reasons is %lu\n",
                      __func__, run->riscv_sbi.extension_id);
        ret = -1;
        break;
    }
    return ret;
}

int kvm_arch_handle_exit(CPUState *cs, struct kvm_run *run)
{
    int ret = 0;
    switch (run->exit_reason) {
    case KVM_EXIT_RISCV_SBI:
        ret = kvm_riscv_handle_sbi(cs, run);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: un-handled exit reason %d\n",
                      __func__, run->exit_reason);
        ret = -1;
        break;
    }
    return ret;
}

void kvm_riscv_reset_vcpu(RISCVCPU *cpu)
{
    CPURISCVState *env = &cpu->env;

    if (!kvm_enabled()) {
        return;
    }
    env->pc = cpu->env.kernel_addr;
    env->gpr[10] = kvm_arch_vcpu_id(CPU(cpu)); /* a0 */
    env->gpr[11] = cpu->env.fdt_addr;          /* a1 */
    env->satp = 0;
}

void kvm_riscv_set_irq(RISCVCPU *cpu, int irq, int level)
{
    int ret;
    unsigned virq = level ? KVM_INTERRUPT_SET : KVM_INTERRUPT_UNSET;

    if (irq != IRQ_S_EXT) {
        perror("kvm riscv set irq != IRQ_S_EXT\n");
        abort();
    }

    ret = kvm_vcpu_ioctl(CPU(cpu), KVM_INTERRUPT, &virq);
    if (ret < 0) {
        perror("Set irq failed");
        abort();
    }
}

bool kvm_arch_cpu_check_are_resettable(void)
{
    return true;
}

void kvm_arch_accel_class_init(ObjectClass *oc)
{
}

void kvm_riscv_aia_create(DeviceState *aplic_s, bool msimode, int socket,
                          uint64_t aia_irq_num, uint64_t hart_count,
                          uint64_t aplic_base, uint64_t imsic_base)
{
    int ret;
    int aia_fd = -1;
    uint64_t aia_mode = KVM_DEV_RISCV_AIA_MODE_HWACCEL;
    uint64_t aia_nr_ids;
    uint64_t aia_hart_bits = find_last_bit(&hart_count, BITS_PER_LONG) + 1;

    if (!msimode) {
        error_report("Currently KVM AIA only supports aplic_imsic mode");
        exit(1);
    }

    aia_fd = kvm_create_device(kvm_state, KVM_DEV_TYPE_RISCV_AIA, false);

    if (aia_fd < 0) {
        error_report("Unable to create in-kernel irqchip");
        exit(1);
    }

    ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_CONFIG,
                            KVM_DEV_RISCV_AIA_CONFIG_MODE,
                            &aia_mode, true, NULL);

    ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_CONFIG,
                            KVM_DEV_RISCV_AIA_CONFIG_IDS,
                            &aia_nr_ids, false, NULL);

    //aia_irq_num = 0;
    //ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_CONFIG,
    //                        KVM_DEV_RISCV_AIA_CONFIG_SRCS,
    //                        &aia_irq_num, true, NULL);
   // if (ret < 0) {
     //   error_report("KVM AIA: fail to set number input irq lines");
       // exit(1);
    //}

    ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_CONFIG,
                            KVM_DEV_RISCV_AIA_CONFIG_HART_BITS,
                            &aia_hart_bits, true, NULL);
    if (ret < 0) {
        error_report("KVM AIA: fail to set number of harts");
        exit(1);
    }

    //ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_ADDR,
    //                        KVM_DEV_RISCV_AIA_ADDR_APLIC,
    //                        &aplic_base, true, NULL);
    //if (ret < 0) {
      //  error_report("KVM AIA: fail to set the base address of APLIC");
        //exit(1);
    //}

    for (int i = 0; i < hart_count; i++) {
        uint64_t imsic_addr = imsic_base + i * IMSIC_HART_SIZE(0);
        ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_ADDR,
                                KVM_DEV_RISCV_AIA_ADDR_IMSIC(i),
                                &imsic_addr, true, NULL);
        if (ret < 0) {
            error_report("KVM AIA: fail to set the base address of IMSICs");
            exit(1);
        }
    }

    if (kvm_has_gsi_routing()) {
        for (uint64_t idx = 0; idx < aia_irq_num + 1; ++idx) {
            kvm_irqchip_add_irq_route(kvm_state, idx, socket, idx);
        }
        kvm_gsi_routing_allowed = true;
        kvm_irqchip_commit_routes(kvm_state);
    }

    ret = kvm_device_access(aia_fd, KVM_DEV_RISCV_AIA_GRP_CTRL,
                            KVM_DEV_RISCV_AIA_CTRL_INIT,
                            NULL, true, NULL);
    if (ret < 0) {
        error_report("KVM AIA: initialized fail");
        exit(1);
    }
}
