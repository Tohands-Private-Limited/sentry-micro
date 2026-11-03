/*
 * Crash capture on Zephyr.
 *
 * ESP-IDF writes a core dump to flash from its panic handler and the ESP32 port reads a
 * summary of it back. Zephyr has no such summary, so this file keeps one of its own: a small
 * fixed record — the fatal reason, the exception cause, the faulting PC and data address, up
 * to SENTRY_MICRO_MAX_FRAMES program counters and the thread name — written from Zephyr's
 * fatal-error hook into memory that survives the reboot, and read back on the next boot by
 * the same `sentry_coredump_*()` calls the ESP32 port answers.
 *
 * The pieces, in the order a crash meets them:
 *
 * - `k_sys_fatal_error_handler()` is Zephyr's weak hook, run by `z_fatal_error()` after it has
 *   printed the fatal banner and after the optional `CONFIG_DEBUG_COREDUMP` dump has been
 *   written, so this does not stand in that subsystem's way. With
 *   CONFIG_SENTRY_MICRO_FATAL_HANDLER (the default) this file provides it: record, then do
 *   exactly what the weak default does. An application with its own handler turns that off
 *   and calls `sentry_zephyr_record_fatal()` itself.
 * - The record lives in `.rtc_noinit` on Espressif (device/sentry_soc.h), beside the
 *   trace and the log ring, so nothing is written to flash from inside a fault handler.
 * - `sentry_soc_mark_panic()` before the reboot. On Espressif it sets the reset-reason hint;
 *   the HAL reads it back on the next boot and hwinfo reports RESET_CPU_LOCKUP, which the
 *   platform layer maps to `panic`. Without it the reboot reads as a plain software reset.
 * - On Xtensa the stack is walked with the same building blocks, and the same loop, as
 *   Zephyr's own console backtrace (`xtensa_backtrace_print()`), so the frames in Sentry are
 *   the frames on the serial console.
 *
 * What is and is not verified:
 *
 * - Verified on an ESP32-S3 (esp32s3_devkitc/esp32s3/procpu build): the strong handler
 *   replaces the kernel's weak one, the record survives `esp_restart()` in RTC slow memory,
 *   the hint turns the next boot's reset reason into `panic`, and the six recorded frames
 *   are the six the console backtrace printed, resolving with addr2line to the expected
 *   functions.
 * - Off Xtensa, nothing has run. The Cortex-M paths compile with zero warnings for
 *   nrf52840dk/nrf52840, both the PC-only one and, with CONFIG_EXTRA_EXCEPTION_INFO=y, the
 *   `arch_stack_walk()` one. The RISC-V branch is written against the headers but compiled by
 *   no build so far.
 * - Frames are raw addresses. Zephyr links with `--build-id=none`, so nothing yet ties an
 *   event to the ELF that would symbolicate it.
 *
 * Wrapped whole, includes and all, for the same reason as sentry_device_zephyr.c.
 */
#if defined(__ZEPHYR__)

#    include "sentry_coredump_zephyr.h"
#    include "../../device/sentry_coredump_device.h"
#    include "../../device/sentry_soc.h"

#    include <stdio.h>
#    include <string.h>

/* kernel.h first: it brings in the architecture interface (arch_stack_walk() among it) in
 * the order those headers need, and including that interface directly ahead of it fails. */
#    include <zephyr/kernel.h>

#    include <zephyr/arch/exception.h>
#    include <zephyr/fatal.h>
#    include <zephyr/fatal_types.h>
#    include <zephyr/logging/log.h>
#    include <zephyr/logging/log_ctrl.h>
#    include <zephyr/sys/reboot.h>
#    include <zephyr/toolchain.h>

/*
 * Xtensa's exception frame is only a placeholder in the public headers (`struct arch_esf {
 * int dummy; }`); what the pointer really addresses, and the helpers that walk from it, are
 * declared in arch/xtensa/include, which zephyr/CMakeLists.txt puts on this library's include
 * path for Xtensa builds only. See the comment there for why an internal header is acceptable.
 */
#    if defined(CONFIG_XTENSA)
#        include <xtensa_asm2_context.h>
#        include <xtensa_stack.h>
#        include <zephyr/arch/xtensa/xtensa_ptr.h>
#        if defined(CONFIG_XTENSA_ENABLE_BACKTRACE) && XCHAL_HAVE_WINDOWED
#            include <xtensa_backtrace.h>
#            define SENTRY_XTENSA_WALK 1
#        endif
#    endif
#    ifndef SENTRY_XTENSA_WALK
#        define SENTRY_XTENSA_WALK 0
#    endif

/*
 * Only for EXCEPTION_DUMP() in the handler below, which logs through LOG_ERR when logging is
 * on. Declared into the kernel's own `os` module, the one the weak default logs through, so
 * "Resetting system" reads exactly as it does without sentry-micro.
 */
#    if defined(CONFIG_SENTRY_MICRO_FATAL_HANDLER)
LOG_MODULE_DECLARE(os, CONFIG_KERNEL_LOG_LEVEL);
#    endif

/*
 * The record.
 *
 * Plain 32-bit fields with no pointers, so it means the same thing to the boot that reads it
 * as to the one that wrote it. `thread_name` matches `sentry_coredump_t::task_name`.
 */
struct sentry_crash_record {
    uint32_t magic;
    uint32_t layout_id;
    uint32_t reason; /* Zephyr's K_ERR_* code */
    uint32_t exccause; /* Xtensa EXCCAUSE; 0 and meaningless elsewhere */
    uint32_t pc; /* The faulting PC as the hardware saved it */
    uint32_t excvaddr; /* Xtensa EXCVADDR; meaningful only for load/store causes */
    uint32_t frames[SENTRY_MICRO_MAX_FRAMES];
    uint32_t frame_count;
    bool truncated;
    char thread_name[16];
};

/* Same idea as the trace and log-ring blocks: the magic says "somebody stored this", the
 * layout id says "in this shape". An OTA that changes the struct leaves the magic valid and
 * the bytes meaningless, and the layout id is what refuses them. */
#    define SENTRY_CRASH_MAGIC 0x53435253u /* "SCRS" */
#    define SENTRY_CRASH_LAYOUT_VERSION 1u
#    define SENTRY_CRASH_LAYOUT_ID                                                                 \
        ((SENTRY_CRASH_LAYOUT_VERSION << 16) | (uint32_t)sizeof(struct sentry_crash_record))

/* A distinctive name rather than a short one: it is what to look for in `nm` and the map file
 * to confirm the record landed in RTC slow memory. */
static SENTRY_PERSIST("sentry_crash") struct sentry_crash_record g_sentry_crash_record;

/* Ordinary .bss: it only has to last while one fault is being recorded. */
static bool g_recording;

#    if SENTRY_XTENSA_WALK
/*
 * The pieces of xtensa_backtrace.c that are file-static there, reproduced exactly so the walk
 * stops where the console backtrace stops. The public halves they call
 * (`xtensa_soc_stack_ptr_is_sane`, `xtensa_soc_ptr_executable`) are what the Espressif SoC
 * code overrides in soc/espressif/common/soc_ptr_check.c.
 */
#        define SENTRY_XTENSA_DEFAULT_PC_MASK 0x40000000u

/* A saved return address carries the window increment in its top two bits; swap in the code
 * region's, then step back from the return address to the call instruction. */
static uint32_t xtensa_frame_pc(uint32_t pc, uint32_t mask, int cause)
{
    if (pc & 0x80000000u) {
        pc = (pc & 0x3fffffffu)
            | (cause != EXCCAUSE_INSTR_PROHIBITED ? mask : SENTRY_XTENSA_DEFAULT_PC_MASK);
    }
    return pc - 3;
}

static bool xtensa_sp_is_sane(uint32_t sp)
{
    return xtensa_soc_stack_ptr_is_sane(sp) && !xtensa_is_outside_stack_bounds(sp, 0, UINT32_MAX);
}

/*
 * The loop of `xtensa_backtrace_print()`, writing into the record instead of the console.
 *
 * Its quirks are kept on purpose, so the two can be compared line for line: the faulting PC
 * goes through the same `- 3` as every return address (so frames[0] is three bytes before the
 * PC in `exception_pc`), and the frame fetched by a step that fails its sanity check is still
 * recorded before the walk stops, exactly as the console prints it before "CORRUPTED".
 */
static void xtensa_walk(
    struct sentry_crash_record *rec, const _xtensa_irq_bsa_t *bsa, const struct arch_esf *esf)
{
    struct xtensa_backtrace_frame_t frame;
    frame.cause = (int)bsa->exccause;
    frame.mask = SENTRY_XTENSA_DEFAULT_PC_MASK;

    /* Spills the register windows to the stack first, so the base save areas it reads next
     * hold the callers' real values. */
    xtensa_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc, (int *)esf);
    if (frame.cause != EXCCAUSE_INSTR_PROHIBITED) {
        frame.mask = frame.pc & 0xc0000000u;
    }

    uint32_t first = xtensa_frame_pc(frame.pc, frame.mask, frame.cause);
    rec->frames[0] = first;
    rec->frame_count = 1;

    /* An InstrFetchProhibited fault is a jump to a non-executable address, so its PC fails the
     * executable check by definition; the console ignores that one failure and so does this. */
    bool corrupted = !(xtensa_sp_is_sane(frame.sp)
        && (xtensa_soc_ptr_executable((const void *)(uintptr_t)first)
            || frame.cause == EXCCAUSE_INSTR_PROHIBITED));

    while (rec->frame_count < SENTRY_MICRO_MAX_FRAMES && frame.next_pc != 0 && !corrupted) {
        if (!xtensa_backtrace_get_next_frame(&frame)) {
            corrupted = true;
        }
        rec->frames[rec->frame_count++] = xtensa_frame_pc(frame.pc, frame.mask, frame.cause);
    }

    /* Either the walk gave up, or there were more frames than the record holds. */
    rec->truncated = corrupted || frame.next_pc != 0;
}
#    endif /* SENTRY_XTENSA_WALK */

#    if !defined(CONFIG_XTENSA) && defined(CONFIG_ARCH_STACKWALK)
/* One call per frame from arch_stack_walk(), innermost first, which is the record's order. */
static bool stack_walk_frame(void *cookie, unsigned long addr)
{
    struct sentry_crash_record *rec = cookie;
    if (rec->frame_count >= SENTRY_MICRO_MAX_FRAMES) {
        rec->truncated = true;
        return false;
    }
    rec->frames[rec->frame_count++] = (uint32_t)addr;
    return true;
}
#    endif

/* Everything the exception frame can tell, per architecture. */
static void record_exception(
    struct sentry_crash_record *rec, unsigned int reason, const struct arch_esf *esf)
{
    /* The stack is exactly what a stack-check failure says is broken. Walking it would at
     * best return garbage and at worst fault again inside the fault handler. */
    bool walk = reason != K_ERR_STACK_CHK_FAIL;
    if (!walk) {
        rec->truncated = true;
    }

#    if defined(CONFIG_XTENSA)
    /* The same guard xtensa_backtrace_print() and xtensa_dump_stack() use: the frame and its
     * base save area have to be inside a stack before anything is read through them. */
    _xtensa_irq_stack_frame_raw_t *frame = (void *)esf;
    if (!xtensa_is_frame_pointer_valid(frame)) {
        rec->truncated = true;
        return;
    }
    const _xtensa_irq_bsa_t *bsa = frame->ptr_to_bsa;
    rec->exccause = (uint32_t)bsa->exccause;
    rec->pc = (uint32_t)bsa->pc;
    rec->excvaddr = (uint32_t)bsa->excvaddr;
#        if SENTRY_XTENSA_WALK
    if (walk) {
        xtensa_walk(rec, bsa, esf);
    }
#        else
    /* No walker in this build (CONFIG_XTENSA_ENABLE_BACKTRACE=n): the PC alone, which the
     * envelope sends as the one frame. */
    rec->truncated = true;
#        endif
#    else
    /* Never run; see the file header for which of these branches have been compiled. */
#        if defined(CONFIG_RISCV)
    rec->pc = (uint32_t)esf->mepc;
#        elif defined(CONFIG_CPU_CORTEX_M)
    rec->pc = (uint32_t)esf->basic.pc;
#        endif
#        if defined(CONFIG_ARCH_STACKWALK)
    if (walk) {
        arch_stack_walk(stack_walk_frame, rec, k_current_get(), esf);
    }
#        else
    rec->truncated = true;
#        endif
#    endif
}

void sentry_zephyr_record_fatal(unsigned int reason, const struct arch_esf *esf)
{
    /* A fault inside this function re-enters it through the fatal path. The second time
     * round, record nothing and let the caller get on with the reboot: a half-written record
     * is already refused by its zero magic. */
    if (g_recording) {
        return;
    }
    g_recording = true;

    /* First, before anything that could fault again: whatever else goes wrong, the next boot
     * should at least read `panic` rather than `software`. */
    sentry_soc_mark_panic();

    struct sentry_crash_record *rec = &g_sentry_crash_record;
    /* Invalidate before writing, and publish the magic last, so a record is either whole
     * or ignored — never a mix of this crash and the previous one. */
    rec->magic = 0;
    compiler_barrier();
    memset(rec, 0, sizeof(*rec));
    rec->layout_id = SENTRY_CRASH_LAYOUT_ID;
    rec->reason = reason;

#    if defined(CONFIG_THREAD_NAME)
    k_tid_t thread = k_current_get();
    const char *name = thread ? k_thread_name_get(thread) : NULL;
    if (name) {
        /* Not snprintf: nothing on this path should depend on more of libc than it must. */
        size_t i = 0;
        for (; i < sizeof(rec->thread_name) - 1 && name[i]; i++) {
            rec->thread_name[i] = name[i];
        }
        rec->thread_name[i] = '\0';
    }
#    endif

    if (esf) {
        record_exception(rec, reason, esf);
    } else {
        rec->truncated = true;
    }

    compiler_barrier();
    rec->magic = SENTRY_CRASH_MAGIC;

    /* Cleared for an application handler that returns, which Zephyr answers by aborting the
     * faulting thread and carrying on: the next fatal error deserves a record too. */
    g_recording = false;
}

#    if defined(CONFIG_SENTRY_MICRO_FATAL_HANDLER)
/*
 * Overrides the kernel's weak handler (kernel/fatal.c). After recording, the body is the weak
 * default's, line for line, so turning sentry-micro on changes what is remembered about a
 * crash and nothing about what the system does next. `k_fatal_halt()` is the public wrapper
 * around the `arch_system_halt()` the default calls.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    sentry_zephyr_record_fatal(reason, esf);

    LOG_PANIC();

#        if defined(CONFIG_RESET_ON_FATAL_ERROR)
    EXCEPTION_DUMP("Resetting system");
    sys_reboot(SYS_REBOOT_WARM);
#        else
    EXCEPTION_DUMP("Halting system");
    k_fatal_halt(reason);
#        endif

    CODE_UNREACHABLE;
}
#    endif

/* Zephyr's own vocabulary from the fatal banner ("Stack overflow", "Kernel oops"), spelled
 * the way the ESP32 port spells exception names, so it reads as an issue title. */
static const char *reason_name(uint32_t reason)
{
    switch (reason) {
    case K_ERR_CPU_EXCEPTION:
        return "CpuException";
    case K_ERR_SPURIOUS_IRQ:
        return "UnhandledInterrupt";
    case K_ERR_STACK_CHK_FAIL:
        return "StackOverflow";
    case K_ERR_KERNEL_OOPS:
        return "KernelOops";
    case K_ERR_KERNEL_PANIC:
        return "KernelPanic";
    default:
        return "UnknownError";
    }
}

static bool record_is_valid(void)
{
    const struct sentry_crash_record *rec = &g_sentry_crash_record;
    /* frame_count is checked too: off Espressif the record may sit in SRAM that came back from
     * reset as noise, and a count past the array is the one field that would turn noise into
     * an out-of-bounds read rather than merely a wrong answer. */
    return rec->magic == SENTRY_CRASH_MAGIC && rec->layout_id == SENTRY_CRASH_LAYOUT_ID
        && rec->frame_count <= SENTRY_MICRO_MAX_FRAMES;
}

/* The record can exist in every build of this file: the default handler writes it, and an
 * application handler can call sentry_zephyr_record_fatal() with the option off. */
bool sentry_coredump_is_supported(void) { return true; }

bool sentry_coredump_available(void) { return record_is_valid(); }

bool sentry_coredump_read(sentry_coredump_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!record_is_valid()) {
        return false;
    }
    const struct sentry_crash_record *rec = &g_sentry_crash_record;

    /*
     * The reason first, the hardware cause only for a hardware exception. Software-raised
     * faults (k_oops(), k_panic(), a stack-check failure) reach the handler through a
     * deliberate illegal instruction, and Xtensa then reports a custom EXCCAUSE that no
     * architecture table names; the reason is the honest title for those.
     */
    const char *type = reason_name(rec->reason);
#    if defined(CONFIG_XTENSA)
    if (rec->reason == K_ERR_CPU_EXCEPTION) {
        const char *cause = sentry_xtensa_exccause_name(rec->exccause);
        if (cause[0]) {
            type = cause;
        }
        out->exception_addr = rec->excvaddr;
        out->exception_addr_valid = sentry_xtensa_exccause_has_data_addr(rec->exccause);
    }
#    endif
    snprintf(out->exception_type, sizeof(out->exception_type), "%s", type);

    out->exception_pc = rec->pc;
    memcpy(out->frames, rec->frames, rec->frame_count * sizeof(rec->frames[0]));
    out->frame_count = rec->frame_count;
    out->truncated = rec->truncated;

    /* Copied with a bound and terminated, rather than trusted to hold a terminator. */
    memcpy(out->task_name, rec->thread_name, sizeof(out->task_name) - 1);
    out->task_name[sizeof(out->task_name) - 1] = '\0';

    /* Left empty: Zephyr records no ELF hash the way ESP-IDF does. */
    out->app_elf_sha256[0] = '\0';

    out->available = true;
    return true;
}

bool sentry_coredump_erase(void)
{
    /* The magic alone: without it the rest is ignored, and the next crash overwrites it. */
    g_sentry_crash_record.magic = 0;
    return true;
}

#endif /* __ZEPHYR__ */
