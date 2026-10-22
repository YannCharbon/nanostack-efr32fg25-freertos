/*
 * mbedTLS v3.0.0 configuration for the EFR32FG25 Nanostack firmware.
 *
 * Feature set = what Nanostack's Wi-SUN security needs (EAP-TLS 1.2 with
 * TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 on P-256, X.509 + CRL + PEM, CTR_DRBG,
 * NIST key wrap, HMAC/MD5 for RADIUS, CCM for the MAC), i.e. the same list as
 * the emulator's examples/mbedtls-config.h, with these target changes
 * (GENERICITY_PLAN.md §5.4, D13, D14):
 *  - no MBEDTLS_HAVE_TIME / _DATE: there is no RTC, time() would be 1970 and
 *    every certificate would be "not yet valid";
 *  - MBEDTLS_NO_PLATFORM_ENTROPY (no /dev/urandom) + MBEDTLS_ENTROPY_HARDWARE_ALT:
 *    the SE TRNG is registered as a STRONG entropy source through
 *    mbedtls_hardware_poll() (port/freertos/random.c). Needed: the stack's own
 *    source (rand_get_8bit(), tls_sec_prot_lib.c) is registered as WEAK, and
 *    mbedtls_ctr_drbg_seed() fails ("drbg seed fail") without at least one
 *    strong source. The emulator never hit this because its config keeps the
 *    platform (/dev/urandom) source. Found with the host twin (tests/host_twin);
 *    GENERICITY_PLAN.md §5.4 only listed MBEDTLS_NO_PLATFORM_ENTROPY;
 *  - 4 kB TLS records instead of 16 kB (example certificates are ~500 B DER);
 *  - no MBEDTLS_VERSION_FEATURES (no runtime feature check on target).
 */
#ifndef MBEDTLS_CONFIG_TARGET_H
#define MBEDTLS_CONFIG_TARGET_H

/* TLS 1.2, client (router) and server (border router) */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_EXPORT_KEYS
#define MBEDTLS_SSL_IN_CONTENT_LEN  4096
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* Hashes, ciphers */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD5_C
#define MBEDTLS_MD_C
#define MBEDTLS_CCM_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_NIST_KW_C

/* ECC P-256, ECDHE-ECDSA */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_RESTARTABLE      /* chunked ECC (stack: max_ops 200) */
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ECDH_LEGACY_CONTEXT

/* X.509 / PK / ASN.1 / PEM */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* Random: CTR_DRBG seeded from the SE TRNG (strong) + the stack source (weak) */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT     /* mbedtls_hardware_poll() = SE TRNG */

/* Diagnostics */
#define MBEDTLS_VERSION_C
#define MBEDTLS_ERROR_C

/* Explicitly NOT defined on target (see header comment):
 * MBEDTLS_HAVE_TIME, MBEDTLS_HAVE_TIME_DATE, MBEDTLS_VERSION_FEATURES,
 * MBEDTLS_FS_IO, MBEDTLS_NET_C, MBEDTLS_TIMING_C. */

#endif /* MBEDTLS_CONFIG_TARGET_H */
