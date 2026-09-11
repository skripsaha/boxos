#ifndef EFI_RSA_H
#define EFI_RSA_H


#include "ktypes.h"

typedef struct {
    const uint8_t *modulus;
    uint32_t       modulus_len;
    const uint8_t *exponent;
    uint32_t       exponent_len;
} EfiRsaPublicKey;

bool efi_rsa_verify_pkcs1_sha256(const EfiRsaPublicKey *key,
                                  const uint8_t *sha256,
                                  const uint8_t *signature,
                                  uint32_t       sig_len);

#endif