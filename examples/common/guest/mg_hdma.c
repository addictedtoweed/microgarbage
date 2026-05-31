/* ============================================================
 *  mg_hdma.c — guest-side HDMA ecall stubs.
 *  See mg_hdma.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_hdma.h"
#include "vm_runtime.h"

void mg_hdma_setup(const MgHdmaCfg *cfg) {
    (void)_vm_sys1(SYS_MG_HDMA_SETUP, (uint32_t)cfg);
}

MgResult mg_hdma_upload_table(uint8_t channel, const void *table, uint16_t len) {
    return (MgResult)(int32_t)_vm_sys3(SYS_MG_HDMA_UPLOAD,
                                       channel, (uint32_t)table, len);
}

void mg_hdma_enable(uint8_t channel, bool on) {
    (void)_vm_sys2(SYS_MG_HDMA_ENABLE, channel, on ? 1u : 0u);
}
