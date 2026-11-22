#include "box_trust.h"
#include "box_author.h"     /* BOX_AUTHOR_BOXOS_MODULUS — made by the build, never in the tree */
#include "crypto.h"
#include "efi_rsa.h"        /* the one RSA verify the kernel has: PKCS#1 v1.5 over SHA-256 */
#include "klib.h"

static const uint8_t g_boxos_modulus[BOX_SIGN_RSA2048_BYTES] = BOX_AUTHOR_BOXOS_MODULUS;
static const uint8_t g_exponent[3] = { 0x01, 0x00, 0x01 };   /* 65537, the one box_format.h signs with */

/* Who the program is: the SHA-256 of its author's key and its name, as many
 * of its first bits as a passport holds. */
static uint64_t PassportOf(const BoxSignature *signature)
{
    KSha256Ctx ctx;
    uint8_t    digest[BOX_HASH_BYTES];
    uint64_t   passport = 0;
    KSha256Init(&ctx);
    KSha256Update(&ctx, signature->Key, signature->KeyBytes);
    KSha256Update(&ctx, signature->Name, signature->NameLength);
    KSha256Final(&ctx, digest);
    for (unsigned i = 0; i < 8; i++)
        passport = passport << 8 | digest[i];
    return passport | TAGFS_PASSPORT_BIT;
}

BoxSigned BoxTrustCheck(const BoxSignature *signature, const uint8_t digest[BOX_HASH_BYTES])
{
    BoxSigned said = { false, false, 0 };
    if (signature->How != BOX_SIGN_RSA2048_SHA256)
        return said;
    EfiRsaPublicKey key = { signature->Key, signature->KeyBytes, g_exponent, sizeof(g_exponent) };
    if (!efi_rsa_verify_pkcs1_sha256(&key, digest, signature->Signature, signature->SignatureBytes))
        return said;
    said.Holds    = true;
    said.Trusted  = memcmp(signature->Key, g_boxos_modulus, BOX_SIGN_RSA2048_BYTES) == 0;
    said.Passport = PassportOf(signature);
    return said;
}
