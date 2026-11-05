#include "../sentry_coredump_device.h"

#include <stdio.h>
#include <string.h>

/*
 * Explicit, not left to esp_core_dump.h to pull in transitively — whether that happens
 * depends on how deep in someone else's build this file sits. When compiled as a
 * library nested under another project's own usermod or plugin rather than as a
 * top-level project source, esp_core_dump.h's own includes may not reach sdkconfig.h,
 * leaving every CONFIG_ESP_COREDUMP_* check below silently undefined regardless of the
 * actual configuration. Including it here directly makes the check reflect reality no
 * matter what pulled this file in.
 */
// clang-format off
#include "sdkconfig.h"
#include "esp_core_dump.h"
#include "esp_idf_version.h"
// clang-format on

/*
 * The whole path depends on the panic handler having somewhere to write. Both conditions
 * hold by default in the Arduino core (verified in its sdkconfig.h), but a custom ESP-IDF
 * build can switch them off, and silently producing no backtraces would be worse than not
 * compiling. Degrade to "no core dump available" with a reason attached instead.
 *
 * ESP-IDF 6.0 dropped the binary format, and with it the ELF one as a choice: 6.x still
 * `select`s ESP_COREDUMP_DATA_FORMAT_ELF from ESP_COREDUMP_ENABLE but defines it nowhere, so it
 * never reaches sdkconfig.h (checked in the v6.1 tree). Requiring it there would compile crash
 * reporting out of every 6.x build, without a word. From 6.0 on, a dump in flash is ELF by
 * construction, so flash is the only condition. Below 6.0, which is what both Arduino cores
 * are built on, the two-symbol check stands as it always has.
 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#    if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
#        define SENTRY_COREDUMP_SUPPORTED 1
#    else
#        define SENTRY_COREDUMP_SUPPORTED 0
#    endif
#elif defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
#    define SENTRY_COREDUMP_SUPPORTED 1
#else
#    define SENTRY_COREDUMP_SUPPORTED 0
#endif

/*
 * Core dumps are on, yet none of the destinations this file knows is set: the only way to get
 * here is ESP-IDF renaming the symbols, which is how the ELF one above went. Loud on purpose,
 * because the alternative is a build that reports no crash and says nothing. ESP-IDF compiles
 * with -Werror, so there this stops the build; a dump going to UART (or nowhere) is a choice
 * and does not trip it.
 */
#if defined(CONFIG_ESP_COREDUMP_ENABLE) && !defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)           \
    && !defined(CONFIG_ESP_COREDUMP_ENABLE_TO_UART)
#    warning                                                                                       \
        "sentry-micro: core dumps are enabled but neither CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH nor _TO_UART is defined; ESP-IDF may have renamed them, and crash reporting is compiled out"
#endif

/*
 * The two architectures report a crash through genuinely different structures, not merely
 * different field names:
 *
 *   Xtensa  exc_cause / exc_vaddr, plus a backtrace ESP-IDF has already unwound into an
 *           array of program counters.
 *   RISC-V  mcause / mtval, and *no* unwound backtrace — the summary carries a raw stack
 *           dump instead, because RISC-V has no fixed frame layout to walk without DWARF.
 *
 * So this is a real per-architecture implementation rather than an aliasing exercise.
 */
#if defined(__riscv)
#    define SENTRY_COREDUMP_ARCH_RISCV 1
#else
#    define SENTRY_COREDUMP_ARCH_RISCV 0
#endif

#if SENTRY_COREDUMP_SUPPORTED && SENTRY_COREDUMP_ARCH_RISCV

/* The names come from core/sentry_coredump.c, shared with the Zephyr port. */

/** `mtval` holds the faulting address for the fault classes that have one. */
static bool cause_has_address(uint32_t mcause)
{
    switch (mcause) {
    case 0:
    case 1:
    case 4:
    case 5:
    case 6:
    case 7:
    case 12:
    case 13:
    case 15:
        return true;
    default:
        return false;
    }
}

static void fill_arch_details(sentry_coredump_t *out, const esp_core_dump_summary_t *summary)
{
    snprintf(out->exception_type, sizeof(out->exception_type), "%s",
        sentry_riscv_mcause_name(summary->ex_info.mcause));
    out->exception_addr = summary->ex_info.mtval;
    out->exception_addr_valid = cause_has_address(summary->ex_info.mcause);

    /*
     * Two frames, not a backtrace. ESP-IDF hands RISC-V a raw stack dump rather than an
     * unwound list, and unwinding it on-device would mean carrying DWARF the device does not
     * have. The PC and the return address are the two the hardware hands us for free, and
     * they are enough to name the crashing function and its caller.
     *
     * Sending the stack dump as an attachment and letting Sentry unwind it server-side is
     * the real answer here, and is not done yet.
     */
    out->frames[0] = summary->exc_pc;
    out->frame_count = 1;
    if (summary->ex_info.ra != 0 && summary->ex_info.ra != summary->exc_pc) {
        out->frames[1] = summary->ex_info.ra;
        out->frame_count = 2;
    }
    /* Say so, rather than letting two frames look like the whole story. */
    out->truncated = true;
}

#elif SENTRY_COREDUMP_SUPPORTED

/* The names and the data-address rule come from core/sentry_coredump.c, shared with the
 * Zephyr port. */

static void fill_arch_details(sentry_coredump_t *out, const esp_core_dump_summary_t *summary)
{
    snprintf(out->exception_type, sizeof(out->exception_type), "%s",
        sentry_xtensa_exccause_name(summary->ex_info.exc_cause));
    out->exception_addr = summary->ex_info.exc_vaddr;
    /* `exc_vaddr` only holds a faulting data address for load/store exceptions; for the
     * rest it is stale. Reporting it regardless would attach a confident-looking address to
     * an IllegalInstruction that has nothing to do with it. */
    out->exception_addr_valid = sentry_xtensa_exccause_has_data_addr(summary->ex_info.exc_cause);

    uint32_t depth = summary->exc_bt_info.depth;
    if (depth > SENTRY_MICRO_MAX_FRAMES) {
        depth = SENTRY_MICRO_MAX_FRAMES;
    }
    for (uint32_t i = 0; i < depth; i++) {
        out->frames[i] = summary->exc_bt_info.bt[i];
    }
    out->frame_count = depth;
    out->truncated = summary->exc_bt_info.corrupted;
}

#endif /* SENTRY_COREDUMP_SUPPORTED */

bool sentry_coredump_is_supported(void) { return SENTRY_COREDUMP_SUPPORTED; }

bool sentry_coredump_available(void)
{
#if SENTRY_COREDUMP_SUPPORTED
    /* Verifies the stored image's checksum, so a dump interrupted by the reset it was
     * reporting does not come back as plausible-looking garbage. */
    return esp_core_dump_image_check() == ESP_OK;
#else
    return false;
#endif
}

bool sentry_coredump_read(sentry_coredump_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

#if SENTRY_COREDUMP_SUPPORTED
    if (esp_core_dump_image_check() != ESP_OK) {
        return false;
    }

    /*
     * ~2.5 KB. Deliberately on the caller's stack rather than the heap: this runs early in
     * boot right after a crash, which is exactly when the heap is least trustworthy. The
     * cost is that the calling task needs the headroom — documented on the declaration.
     */
    esp_core_dump_summary_t summary;
    /* Zeroed first: ESP-IDF fills only the parts it finds (checked in v6.1). The backtrace is
     * written only when a stack segment follows the crashed task's TCB in the dump, yet the
     * call still returns ESP_OK, so an unset depth would be stack garbage taken as frames. */
    memset(&summary, 0, sizeof(summary));
    if (esp_core_dump_get_summary(&summary) != ESP_OK) {
        return false;
    }

    snprintf(out->task_name, sizeof(out->task_name), "%s", summary.exc_task);
    out->exception_pc = summary.exc_pc;

    /* ESP-IDF stores the ELF hash as a printable string already. */
    snprintf(out->app_elf_sha256, sizeof(out->app_elf_sha256), "%s",
        (const char *)summary.app_elf_sha256);

    fill_arch_details(out, &summary);

    out->available = true;
    return true;
#else
    return false;
#endif
}

bool sentry_coredump_erase(void)
{
#if SENTRY_COREDUMP_SUPPORTED
    /* Erasing a partition that holds nothing is the desired end state, not a failure. */
    esp_err_t err = esp_core_dump_image_erase();
    return err == ESP_OK || err == ESP_ERR_NOT_FOUND;
#else
    return false;
#endif
}
