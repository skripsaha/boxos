#ifndef EFI_AUTHENTICODE_H
#define EFI_AUTHENTICODE_H


#include "ktypes.h"

typedef enum {
    EFI_AC_RESULT_VALID                       = 0,
    EFI_AC_RESULT_BAD_PE                      = 1,
    EFI_AC_RESULT_NO_CERT_TABLE               = 2,
    EFI_AC_RESULT_BAD_CERT_TABLE              = 3,
    EFI_AC_RESULT_BAD_SIGNED_DATA             = 4,
    EFI_AC_RESULT_AUTH_HASH_MISMATCH          = 5,
    EFI_AC_RESULT_UNSUPPORTED_DIGEST          = 6,
    EFI_AC_RESULT_SIGNER_NOT_FOUND            = 7,
    EFI_AC_RESULT_BAD_SIGNER_CERT             = 8,
    EFI_AC_RESULT_BAD_RSA_KEY                 = 9,
    EFI_AC_RESULT_SIGNATURE_INVALID           = 10,
    EFI_AC_RESULT_CHAIN_UNTRUSTED             = 11,
    EFI_AC_RESULT_REVOKED_BY_DBX              = 12,
    EFI_AC_RESULT_SB_UNAVAILABLE              = 13,
} EfiAuthenticodeResult;

EfiAuthenticodeResult efi_authenticode_verify_pe(const uint8_t *buf,
                                                  uint32_t        len,
                                                  uint8_t        *out_pe_hash,
                                                  uint8_t        *out_signer_hash);

bool efi_authenticode_compute_pe_hash(const uint8_t *buf, uint32_t len,
                                       uint8_t out_sha256[32]);

const char *efi_authenticode_result_slug(EfiAuthenticodeResult r);

#endif