#include "sentry_coredump.h"

/*
 * These are the strings a maker has already seen on their serial console next to "Guru
 * Meditation Error", so using the same vocabulary means the Sentry issue title matches what
 * they searched the forums for. A raw cause number would not.
 */
const char *sentry_xtensa_exccause_name(uint32_t cause)
{
    switch (cause) {
    case 0:
        return "IllegalInstruction";
    case 1:
        return "Syscall";
    case 2:
        return "InstructionFetchError";
    case 3:
        return "LoadStoreError";
    case 4:
        return "Level1Interrupt";
    case 5:
        return "Alloca";
    case 6:
        return "IntegerDivideByZero";
    case 8:
        return "Privileged";
    case 9:
        return "LoadStoreAlignment";
    case 12:
        return "InstrPIFDataError";
    case 13:
        return "LoadStorePIFDataError";
    case 14:
        return "InstrPIFAddrError";
    case 15:
        return "LoadStorePIFAddrError";
    case 16:
        return "InstTLBMiss";
    case 17:
        return "InstTLBMultiHit";
    case 18:
        return "InstFetchPrivilege";
    case 20:
        return "InstFetchProhibited";
    case 24:
        return "LoadStoreTLBMiss";
    case 25:
        return "LoadStoreTLBMultiHit";
    case 26:
        return "LoadStorePrivilege";
    case 28:
        return "LoadProhibited";
    case 29:
        return "StoreProhibited";
    default:
        return "";
    }
}

bool sentry_xtensa_exccause_has_data_addr(uint32_t cause)
{
    switch (cause) {
    case 3: /* LoadStoreError */
    case 9: /* LoadStoreAlignment */
    case 24: /* LoadStoreTLBMiss */
    case 25: /* LoadStoreTLBMultiHit */
    case 26: /* LoadStorePrivilege */
    case 28: /* LoadProhibited */
    case 29: /* StoreProhibited */
        return true;
    default:
        return false;
    }
}

const char *sentry_riscv_mcause_name(uint32_t mcause)
{
    switch (mcause) {
    case 0:
        return "InstructionAddressMisaligned";
    case 1:
        return "InstructionAccessFault";
    case 2:
        return "IllegalInstruction";
    case 3:
        return "Breakpoint";
    case 4:
        return "LoadAddressMisaligned";
    case 5:
        return "LoadAccessFault";
    case 6:
        return "StoreAddressMisaligned";
    case 7:
        return "StoreAccessFault";
    case 8:
    case 9:
    case 11:
        return "EnvironmentCall";
    case 12:
        return "InstructionPageFault";
    case 13:
        return "LoadPageFault";
    case 15:
        return "StorePageFault";
    default:
        return "";
    }
}
