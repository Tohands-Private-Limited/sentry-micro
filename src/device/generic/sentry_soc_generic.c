/*
 * The SoC hooks for every Zephyr SoC that has no family file of its own (see
 * device/sentry_soc.h). It reports what Zephyr itself knows and does nothing else, so a new
 * board builds and runs; it is not a substitute for a family file where the SoC has
 * something to offer, such as a revision register.
 *
 * Wrapped whole, includes and all, for the same reason as sentry_soc_espressif.c.
 */
#if defined(__ZEPHYR__)

#    include "../sentry_soc.h"

const char *sentry_soc_chip_model(void)
{
    /* Zephyr's own SoC name, which is what a Zephyr user would search for. */
    return CONFIG_SOC;
}

uint16_t sentry_soc_chip_revision(void)
{
    /* Zephyr has no portable way to ask; 0 is the documented "unknown". */
    return 0;
}

void sentry_soc_mark_panic(void)
{
    /* Deliberately empty. A port for a SoC that needs this writes a flag the next boot can
     * read (in memory that survives a reset, SENTRY_PERSIST() being the place) and makes
     * the platform layer's reset-reason mapping consult it. It writes nothing if hwinfo
     * already reports a panic distinctly on that SoC. Until then a crash reads back as a
     * plain software reset. */
}

#endif /* __ZEPHYR__ */
