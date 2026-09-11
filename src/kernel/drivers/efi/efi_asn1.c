
#include "efi_asn1.h"
#include "klib.h"


const uint8_t OID_RSA_ENCRYPTION[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01
};
const uint32_t OID_RSA_ENCRYPTION_LEN = sizeof(OID_RSA_ENCRYPTION);

const uint8_t OID_SHA256_WITH_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B
};
const uint32_t OID_SHA256_WITH_RSA_LEN = sizeof(OID_SHA256_WITH_RSA);

const uint8_t OID_SHA384_WITH_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C
};
const uint32_t OID_SHA384_WITH_RSA_LEN = sizeof(OID_SHA384_WITH_RSA);

const uint8_t OID_SHA256[] = {
    0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01
};
const uint32_t OID_SHA256_LEN = sizeof(OID_SHA256);

const uint8_t OID_PKCS7_SIGNED_DATA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x07,0x02
};
const uint32_t OID_PKCS7_SIGNED_DATA_LEN = sizeof(OID_PKCS7_SIGNED_DATA);

const uint8_t OID_SPC_INDIRECT_DATA[] = {
    0x2B,0x06,0x01,0x04,0x01,0x82,0x37,0x02,0x01,0x04
};
const uint32_t OID_SPC_INDIRECT_DATA_LEN = sizeof(OID_SPC_INDIRECT_DATA);

const uint8_t OID_PKCS9_MESSAGE_DIGEST[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x09,0x04
};
const uint32_t OID_PKCS9_MESSAGE_DIGEST_LEN = sizeof(OID_PKCS9_MESSAGE_DIGEST);

const uint8_t OID_PKCS9_CONTENT_TYPE[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x09,0x03
};
const uint32_t OID_PKCS9_CONTENT_TYPE_LEN = sizeof(OID_PKCS9_CONTENT_TYPE);


uint8_t asn1_peek_tag(const Asn1Reader *r)
{
    if (!r || r->len == 0) return 0;
    return r->p[0];
}

static bool read_der_length(const uint8_t *p, uint32_t remaining,
                             uint32_t *out_len, uint32_t *out_consumed)
{
    if (remaining == 0) return false;
    uint8_t first = p[0];

    if ((first & 0x80) == 0) {
        *out_len = first;
        *out_consumed = 1;
        return true;
    }
    if (first == 0x80) return false;

    uint32_t nbytes = first & 0x7F;
    if (nbytes == 0 || nbytes > 4) return false;
    if (1 + nbytes > remaining)    return false;

    uint32_t v = 0;
    for (uint32_t i = 0; i < nbytes; i++) {
        v = (v << 8) | p[1 + i];
    }
    if (nbytes > 1 && p[1] == 0)   return false;
    *out_len = v;
    *out_consumed = 1 + nbytes;
    return true;
}

bool asn1_read_tlv(Asn1Reader *r, uint8_t expect_tag,
                    uint8_t *out_tag, const uint8_t **out_value,
                    uint32_t *out_value_len)
{
    if (!r || r->len < 2) return false;
    uint8_t tag = r->p[0];
    if (expect_tag && tag != expect_tag) return false;

    if ((tag & 0x1F) == 0x1F) return false;

    uint32_t vlen = 0, consumed = 0;
    if (!read_der_length(r->p + 1, r->len - 1, &vlen, &consumed)) return false;

    uint32_t header = 1 + consumed;
    if (header + vlen > r->len) return false;

    if (out_tag)       *out_tag       = tag;
    if (out_value)     *out_value     = r->p + header;
    if (out_value_len) *out_value_len = vlen;

    r->p   += header + vlen;
    r->len -= header + vlen;
    return true;
}

bool asn1_read_typed(Asn1Reader *r, uint8_t expect_tag,
                      const uint8_t **out_value, uint32_t *out_value_len)
{
    uint8_t got = 0;
    return asn1_read_tlv(r, expect_tag, &got, out_value, out_value_len);
}

bool asn1_open(Asn1Reader *r, uint8_t expect_tag, Asn1Reader *out_inner)
{
    if (!out_inner) return false;
    const uint8_t *v = NULL;
    uint32_t vlen = 0;
    if (!asn1_read_typed(r, expect_tag, &v, &vlen)) return false;
    out_inner->p   = v;
    out_inner->len = vlen;
    return true;
}

bool asn1_read_octet_string(Asn1Reader *r, const uint8_t **out, uint32_t *out_len)
{
    return asn1_read_typed(r, ASN1_TAG_OCTET_STRING, out, out_len);
}

bool asn1_read_bit_string(Asn1Reader *r, const uint8_t **out, uint32_t *out_len)
{
    const uint8_t *v = NULL;
    uint32_t vlen = 0;
    if (!asn1_read_typed(r, ASN1_TAG_BIT_STRING, &v, &vlen)) return false;
    if (vlen < 1) return false;
    if (v[0] != 0) return false;
    *out     = v + 1;
    *out_len = vlen - 1;
    return true;
}

bool asn1_read_integer(Asn1Reader *r, const uint8_t **out, uint32_t *out_len)
{
    const uint8_t *v = NULL;
    uint32_t vlen = 0;
    if (!asn1_read_typed(r, ASN1_TAG_INTEGER, &v, &vlen)) return false;
    if (vlen == 0) return false;
    if (vlen >= 2 && v[0] == 0x00) {
        v++;
        vlen--;
    }
    *out     = v;
    *out_len = vlen;
    return true;
}

bool asn1_skip(Asn1Reader *r)
{
    const uint8_t *v = NULL;
    uint32_t vlen = 0;
    uint8_t  tag  = 0;
    return asn1_read_tlv(r, 0, &tag, &v, &vlen);
}

bool asn1_oid_equal(const uint8_t *enc1, uint32_t len1,
                     const uint8_t *enc2, uint32_t len2)
{
    if (len1 != len2) return false;
    return memcmp(enc1, enc2, len1) == 0;
}