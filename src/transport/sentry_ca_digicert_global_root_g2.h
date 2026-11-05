/**
 * The root certificate the Zephyr and ESP-IDF WiFi transports trust unless the application
 * passes its own. One declaration for both, beside the one definition.
 */
#ifndef SENTRY_MICRO_CA_DIGICERT_GLOBAL_ROOT_G2_H_INCLUDED
#define SENTRY_MICRO_CA_DIGICERT_GLOBAL_ROOT_G2_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DigiCert Global Root G2, PEM, NUL-terminated, which Sentry's ingest certificate chains to.
 * Valid until 2038-01-15. See sentry_ca_digicert_global_root_g2.c for how it was checked.
 */
extern const char sentry_ca_digicert_global_root_g2[];

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_CA_DIGICERT_GLOBAL_ROOT_G2_H_INCLUDED */
