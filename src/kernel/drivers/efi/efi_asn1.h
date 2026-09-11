#ifndef EFI_ASN1_H
#define EFI_ASN1_H


#include "ktypes.h"

#define ASN1_TAG_BOOLEAN          0x01
#define ASN1_TAG_INTEGER          0x02
#define ASN1_TAG_BIT_STRING       0x03
#define ASN1_TAG_OCTET_STRING     0x04
#define ASN1_TAG_NULL             0x05
#define ASN1_TAG_OID              0x06
#define ASN1_TAG_UTF8_STRING      0x0C
#define ASN1_TAG_PRINTABLE_STRING 0x13
#define ASN1_TAG_TELETEX_STRING   0x14
#define ASN1_TAG_UTC_TIME         0x17
#define ASN1_TAG_GENERALIZED_TIME 0x18
#define ASN1_TAG_SEQUENCE         0x30
#define ASN1_TAG_SET              0x31

#define ASN1_TAG_CONTEXT(n)             (0xA0u | (n))
#define ASN1_TAG_CONTEXT_PRIMITIVE(n)   (0x80u | (n))

typedef struct {
    const uint8_t *p;
    uint32_t       len;
} Asn1Reader;

static inline Asn1Reader asn1_reader(const uint8_t *buf, uint32_t len)
{
    Asn1Reader r = { .p = buf, .len = len };
    return r;
}

uint8_t asn1_peek_tag(const Asn1Reader *r);

bool asn1_read_tlv(Asn1Reader *r, uint8_t expect_tag,
                    uint8_t *out_tag, const uint8_t **out_value,
                    uint32_t *out_value_len);

bool asn1_read_typed(Asn1Reader *r, uint8_t expect_tag,
                      const uint8_t **out_value, uint32_t *out_value_len);

bool asn1_open(Asn1Reader *r, uint8_t expect_tag, Asn1Reader *out_inner);

bool asn1_read_octet_string(Asn1Reader *r, const uint8_t **out, uint32_t *out_len);
bool asn1_read_bit_string(Asn1Reader *r, const uint8_t **out, uint32_t *out_len);

bool asn1_read_integer(Asn1Reader *r, const uint8_t **out, uint32_t *out_len);

bool asn1_skip(Asn1Reader *r);

bool asn1_oid_equal(const uint8_t *enc1, uint32_t len1,
                     const uint8_t *enc2, uint32_t len2);


extern const uint8_t OID_RSA_ENCRYPTION[];
extern const uint32_t OID_RSA_ENCRYPTION_LEN;

extern const uint8_t OID_SHA256_WITH_RSA[];
extern const uint32_t OID_SHA256_WITH_RSA_LEN;

extern const uint8_t OID_SHA384_WITH_RSA[];
extern const uint32_t OID_SHA384_WITH_RSA_LEN;

extern const uint8_t OID_SHA256[];
extern const uint32_t OID_SHA256_LEN;

extern const uint8_t OID_PKCS7_SIGNED_DATA[];
extern const uint32_t OID_PKCS7_SIGNED_DATA_LEN;

extern const uint8_t OID_SPC_INDIRECT_DATA[];
extern const uint32_t OID_SPC_INDIRECT_DATA_LEN;

extern const uint8_t OID_PKCS9_MESSAGE_DIGEST[];
extern const uint32_t OID_PKCS9_MESSAGE_DIGEST_LEN;

extern const uint8_t OID_PKCS9_CONTENT_TYPE[];
extern const uint32_t OID_PKCS9_CONTENT_TYPE_LEN;

#endif