/* esp_gcrypt_shim.c : mini compat layer libgcrypt pour les builds wasm d'EspSimu.
 *
 * Ne couvre que le sous-ensemble utilisé par hw/misc/{esp32_rsa,esp_rsa,esp_aes,
 * esp32_flash_enc}.c :
 *   MPI : gcry_mpi_t/new/scan/print/powm/mul/mulm/invm/set_bit/release (USG BE)
 *   Cipher : open/setkey/setiv/setctr/encrypt/close (AES128/256,
 *            modes ECB/CBC/CFB/CFB8/CTR/OFB), construits sur crypto/aes.c de QEMU.
 *
 * MPI non signés en limbs u32 (word little-endian) ; capacité fixe
 * (RSA max 4096 bits + marges de multiplication). Division bit-par-bit
 * (décalage/soustraction), Euclide étendu pour l'inverse modulaire.
 */
#include "qemu/osdep.h"
#include "crypto/aes.h"
#include "esp_gcrypt_shim.h"

/* max utile : mul de deux nombres 4096 bits = 256 limbs ; un peu de marge */
#define ESP_MPI_LIMBS 264u

struct esp_mpi {
    uint32_t n;
    uint32_t d[ESP_MPI_LIMBS];
};

struct esp_gcipher {
    int algo;
    int mode;
    AES_KEY ek;
    AES_KEY dk;
    uint8_t iv[16];
    uint8_t ctr[16];
};

static const char *shim_err_ok = "ok";

const char *gcry_strerror(gcry_error_t err)
{
    (void)err;
    return shim_err_ok;
}

/* ------------------------------------------------------------------ */
/* petit outillage MPI                                                */
/* ------------------------------------------------------------------ */

static void esp_mpi_norm(struct esp_mpi *a)
{
    while (a->n > 0 && a->d[a->n - 1] == 0) {
        a->n--;
    }
}

static uint32_t esp_mpi_val_bits(const struct esp_mpi *a)
{
    if (a->n == 0) {
        return 0;
    }
    uint32_t top = a->n - 1;
    uint32_t v = a->d[top];
    uint32_t b = 0;
    while (v) {
        v >>= 1;
        b++;
    }
    return top * 32u + b;
}

static int esp_mpi_cmp(const struct esp_mpi *a, const struct esp_mpi *b)
{
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (uint32_t i = a->n; i > 0; i--) {
        if (a->d[i - 1] != b->d[i - 1]) {
            return a->d[i - 1] < b->d[i - 1] ? -1 : 1;
        }
    }
    return 0;
}

/* z <- a ; z != a : esp résultat de copie */
static void esp_mpi_cpy(struct esp_mpi *z, const struct esp_mpi *a)
{
    memcpy(z->d, a->d, ESP_MPI_LIMBS * sizeof(uint32_t));
    z->n = a->n;
}

static void esp_mpi_zero(struct esp_mpi *z)
{
    memset(z->d, 0, ESP_MPI_LIMBS * sizeof(uint32_t));
    z->n = 0;
}

/* z = a + b (z distinct de a et b) */
static void esp_mpi_add(struct esp_mpi *z, const struct esp_mpi *a,
                        const struct esp_mpi *b)
{
    esp_mpi_zero(z);
    uint32_t n = a->n > b->n ? a->n : b->n;
    uint32_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t s = carry;
        if (i < a->n) {
            s += a->d[i];
        }
        if (i < b->n) {
            s += b->d[i];
        }
        z->d[i] = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
    z->n = n;
    if (carry) {
        z->d[z->n] = carry;
        z->n++;
    }
    esp_mpi_norm(z);
}

/* q = a - b (q distinct) ; suppose a >= b */
static void esp_mpi_sub(struct esp_mpi *z, const struct esp_mpi *a,
                        const struct esp_mpi *b)
{
    esp_mpi_zero(z);
    z->n = a->n;
    uint32_t borrow = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t bi = i < b->n ? b->d[i] : 0;
        bi += borrow;
        uint64_t ai = a->d[i];
        if (ai >= bi) {
            z->d[i] = (uint32_t)(ai - bi);
            borrow = 0;
        } else {
            z->d[i] = (uint32_t)(ai + 0x100000000ULL - bi);
            borrow = 1;
        }
    }
    esp_mpi_norm(z);
}

/* z = a * b (z distinct de a et b) */
static void esp_mpi_mul(struct esp_mpi *z, const struct esp_mpi *a,
                        const struct esp_mpi *b)
{
    esp_mpi_zero(z);
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < b->n; j++) {
            uint64_t s = (uint64_t)a->d[i] * b->d[j] + z->d[i + j] + carry;
            z->d[i + j] = (uint32_t)s;
            carry = (uint32_t)(s >> 32);
        }
        for (uint32_t k = i + b->n; carry && k < ESP_MPI_LIMBS; k++) {
            uint64_t s = (uint64_t)z->d[k] + carry;
            z->d[k] = (uint32_t)s;
            carry = (uint32_t)(s >> 32);
        }
    }
    esp_mpi_norm(z);
}

/* division bit-par-bit : q = a / m, rem = a mod m (q/rem peuvent être NULL).
 * rem peut être le même buffer que a (lecture seule). */
static void esp_mpi_divmod(struct esp_mpi *q, struct esp_mpi *rem,
                           const struct esp_mpi *a, const struct esp_mpi *m)
{
    uint32_t nbits = esp_mpi_val_bits(a);
    uint32_t mlimbs = m->n;

    uint32_t r[ESP_MPI_LIMBS + 1];
    memset(r, 0, sizeof(r));

    if (q) {
        esp_mpi_zero(q);
        if (nbits) {
            q->n = (nbits + 31u) / 32u;
        }
    }

    for (uint32_t k = nbits; k > 0; k--) {
        uint32_t bit = (a->d[(k - 1) / 32] >> ((k - 1) % 32)) & 1u;
        uint32_t carry = bit;
        for (uint32_t i = 0; i <= mlimbs; i++) {
            uint32_t nc = r[i] >> 31;
            r[i] = (r[i] << 1) | carry;
            carry = nc;
        }
        int ge = (r[mlimbs] != 0);
        if (!ge && mlimbs) {
            for (uint32_t i = mlimbs; i > 0; i--) {
                if (r[i - 1] != m->d[i - 1]) {
                    ge = r[i - 1] > m->d[i - 1];
                    break;
                }
            }
        }
        if (ge) {
            uint32_t borrow = 0;
            for (uint32_t i = 0; i < mlimbs; i++) {
                uint64_t bi = (uint64_t)m->d[i] + borrow;
                if (r[i] >= bi) {
                    r[i] -= (uint32_t)bi;
                    borrow = 0;
                } else {
                    r[i] = (uint32_t)(r[i] + 0x100000000ULL - bi);
                    borrow = 1;
                }
            }
            r[mlimbs] -= borrow;
            if (q) {
                q->d[(k - 1) / 32] |= 1u << ((k - 1) % 32);
            }
        }
    }

    if (q) {
        esp_mpi_norm(q);
    }
    if (rem) {
        memset(rem->d, 0, ESP_MPI_LIMBS * sizeof(uint32_t));
        if (mlimbs) {
            memcpy(rem->d, r, mlimbs * sizeof(uint32_t));
            rem->n = mlimbs;
            esp_mpi_norm(rem);
        } else {
            rem->n = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* API MPI publique                                                   */
/* ------------------------------------------------------------------ */

gcry_mpi_t gcry_mpi_new(size_t nbytes)
{
    (void)nbytes; /* capacité fixe */
    struct esp_mpi *a = g_malloc0(sizeof(struct esp_mpi));
    return (gcry_mpi_t)a;
}

void gcry_mpi_release(gcry_mpi_t p)
{
    g_free(p);
}

gcry_error_t gcry_mpi_scan(gcry_mpi_t *out, int fmt, const void *buf,
                           size_t len, size_t *scanned)
{
    if (!out || fmt != GCRYMPI_FMT_USG || (len && !buf)) {
        return (gcry_error_t)-1;
    }
    if (len > ESP_MPI_LIMBS * 4u) {
        return (gcry_error_t)-2;
    }
    struct esp_mpi *a = g_malloc0(sizeof(struct esp_mpi));
    const uint8_t *b = buf;
    uint32_t limb = 0;
    uint32_t shift = 0;
    for (size_t i = len; i > 0; i--) {
        limb |= (uint32_t)b[i - 1] << shift;
        shift += 8;
        if (shift == 32) {
            a->d[a->n++] = limb;
            limb = 0;
            shift = 0;
        }
    }
    if (shift) {
        a->d[a->n++] = limb;
    }
    esp_mpi_norm(a);
    *out = (gcry_mpi_t)a;
    if (scanned) {
        *scanned = len;
    }
    return 0;
}

gcry_error_t gcry_mpi_print(int fmt, void *buf, size_t bufsz, size_t *written,
                            const gcry_mpi_t a)
{
    if (!buf || fmt != GCRYMPI_FMT_USG) {
        return (gcry_error_t)-1;
    }
    struct esp_mpi v;
    esp_mpi_cpy(&v, (const struct esp_mpi *)a);
    esp_mpi_norm(&v);
    uint32_t bits = esp_mpi_val_bits(&v);
    size_t bytes = (bits + 7u) / 8u;
    if (bufsz < bytes) {
        return (gcry_error_t)-2;
    }
    memset(buf, 0, bufsz);
    uint8_t *o = buf;
    for (size_t k = 0; k < bytes; k++) {
        uint32_t bitoff = (uint32_t)((bytes - 1u - k) * 8u);
        o[k] = (uint8_t)((v.d[bitoff / 32u] >> (bitoff % 32u)) & 0xffu);
    }
    if (written) {
        *written = bytes;
    }
    return 0;
}

void gcry_mpi_set_bit(gcry_mpi_t p, unsigned int bit)
{
    struct esp_mpi *a = (struct esp_mpi *)p;
    uint32_t limb = bit / 32u;
    if (limb >= ESP_MPI_LIMBS) {
        return; /* au-delà de la capacité : ignoré (jamais atteint par la lignée) */
    }
    if (limb >= a->n) {
        memset(a->d + a->n, 0, (limb + 1u - a->n) * sizeof(uint32_t));
        a->n = limb + 1u;
    }
    a->d[limb] |= 1u << (bit % 32u);
}

void gcry_mpi_mul(gcry_mpi_t z, const gcry_mpi_t x, const gcry_mpi_t y)
{
    struct esp_mpi X, Y;
    esp_mpi_cpy(&X, (const struct esp_mpi *)x);
    esp_mpi_cpy(&Y, (const struct esp_mpi *)y);
    esp_mpi_mul((struct esp_mpi *)z, &X, &Y);
}

void gcry_mpi_mulm(gcry_mpi_t res, const gcry_mpi_t x, const gcry_mpi_t y,
                   const gcry_mpi_t m)
{
    struct esp_mpi X, Y, M, prod;
    esp_mpi_cpy(&X, (const struct esp_mpi *)x);
    esp_mpi_cpy(&Y, (const struct esp_mpi *)y);
    esp_mpi_cpy(&M, (const struct esp_mpi *)m);
    esp_mpi_mul(&prod, &X, &Y);
    if (!M.n) {
        esp_mpi_zero((struct esp_mpi *)res); /* m == 0 : résultat 0 */
        return;
    }
    struct esp_mpi rem;
    esp_mpi_zero(&rem);
    esp_mpi_divmod(NULL, &rem, &prod, &M);
    esp_mpi_cpy((struct esp_mpi *)res, &rem);
}

void gcry_mpi_powm(gcry_mpi_t res, const gcry_mpi_t x, const gcry_mpi_t y,
                   const gcry_mpi_t m)
{
    struct esp_mpi X, Y, M, base, acc, tmp, mul;
    esp_mpi_cpy(&X, (const struct esp_mpi *)x);
    esp_mpi_cpy(&Y, (const struct esp_mpi *)y);
    esp_mpi_cpy(&M, (const struct esp_mpi *)m);

    esp_mpi_zero((struct esp_mpi *)res);
    if (!M.n) {
        return;
    }
    if (M.n == 1 && M.d[0] == 1) {
        return; /* x^y mod 1 = 0 */
    }

    esp_mpi_divmod(NULL, &base, &X, &M); /* base = x mod m */

    esp_mpi_zero(&acc);
    acc.n = 1;
    acc.d[0] = 1;

    uint32_t ybits = esp_mpi_val_bits(&Y);
    for (uint32_t k = ybits; k > 0; k--) {
        esp_mpi_mul(&tmp, &acc, &acc);
        esp_mpi_divmod(NULL, &acc, &tmp, &M);
        if ((Y.d[(k - 1) / 32] >> ((k - 1) % 32)) & 1u) {
            esp_mpi_mul(&mul, &acc, &base);
            esp_mpi_divmod(NULL, &acc, &mul, &M);
        }
    }

    esp_mpi_cpy((struct esp_mpi *)res, &acc);
}

int gcry_mpi_invm(gcry_mpi_t a, const gcry_mpi_t r, const gcry_mpi_t m)
{
    /* Euclide étendu : cherche t tel que r*t ≡ 1 (mod m). */
    struct esp_mpi A, B, Mv, X0, X1, Q, R, Tsub, tmp;
    esp_mpi_cpy(&Mv, (const struct esp_mpi *)m);
    if (!Mv.n) {
        return 0; /* module nul : non inversible */
    }
    esp_mpi_cpy(&A, &Mv);
    esp_mpi_cpy(&B, (const struct esp_mpi *)r);
    esp_mpi_zero(&X0);
    esp_mpi_zero(&X1);
    X1.n = 1;
    X1.d[0] = 1;

    while (B.n != 0) {
        esp_mpi_divmod(&Q, &R, &A, &B);
        esp_mpi_mul(&tmp, &Q, &X1);
        /* X_next = X0 - tmp (mod m) ; on reste non négatif */
        if (esp_mpi_cmp(&tmp, &X0) > 0) {
            esp_mpi_sub(&Tsub, &tmp, &X0);   /* diff = tmp - X0 < m */
            esp_mpi_sub(&Tsub, &Mv, &Tsub);  /* m - diff ≡ -diff (mod m) */
        } else {
            esp_mpi_sub(&Tsub, &X0, &tmp);
        }
        esp_mpi_cpy(&A, &B);
        esp_mpi_cpy(&B, &R);
        esp_mpi_cpy(&X0, &X1);
        esp_mpi_cpy(&X1, &Tsub);
    }

    if (!(A.n == 1 && A.d[0] == 1)) {
        return 0; /* non inversible */
    }
    esp_mpi_cpy((struct esp_mpi *)a, &X0);
    esp_mpi_norm((struct esp_mpi *)a);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Cipher                                                             */
/* ------------------------------------------------------------------ */

static void esp_cipher_inc_ctr(uint8_t *c)
{
    for (int i = 15; i >= 0; i--) {
        if (++c[i]) {
            break;
        }
    }
}

gcry_error_t gcry_cipher_open(gcry_cipher_hd_t *hd, int algo, int mode,
                              unsigned int flags)
{
    if (!hd || (flags != 0)) {
        return (gcry_error_t)-1;
    }
    int ok = ((algo == GCRY_CIPHER_AES128 || algo == GCRY_CIPHER_AES192 ||
               algo == GCRY_CIPHER_AES256) &&
              (mode >= GCRY_CIPHER_MODE_ECB && mode <= GCRY_CIPHER_MODE_OFB));
    if (!ok) {
        return (gcry_error_t)-2;
    }
    struct esp_gcipher *g = g_malloc0(sizeof(struct esp_gcipher));
    g->algo = algo;
    g->mode = mode;
    *hd = (gcry_cipher_hd_t)g;
    return 0;
}

void gcry_cipher_close(gcry_cipher_hd_t hd)
{
    g_free(hd);
}

gcry_error_t gcry_cipher_setkey(gcry_cipher_hd_t hd, const void *key,
                                size_t keylen)
{
    struct esp_gcipher *g = (struct esp_gcipher *)hd;
    int bits;
    if (!g || !key) {
        return (gcry_error_t)-1;
    }
    if (g->algo == GCRY_CIPHER_AES256 && keylen == 32) {
        bits = 256;
    } else if (g->algo == GCRY_CIPHER_AES192 && keylen == 24) {
        bits = 192;
    } else if (g->algo == GCRY_CIPHER_AES128 && keylen == 16) {
        bits = 128;
    } else {
        return (gcry_error_t)-2;
    }
    if (AES_set_encrypt_key(key, bits, &g->ek) < 0) {
        return (gcry_error_t)-3;
    }
    /* les usages lignée (esp_aes decrypt) dérivent la clé inverse de la même
     * clé : gcrypt gère les deux sens depuis un seul setkey */
    if (AES_set_decrypt_key(key, bits, &g->dk) < 0) {
        return (gcry_error_t)-3;
    }
    return 0;
}

gcry_error_t gcry_cipher_setiv(gcry_cipher_hd_t _hd, const void *iv,
                               size_t ivlen)
{
    struct esp_gcipher *g = (struct esp_gcipher *)_hd;
    const uint8_t *b = iv;
    if (!g || !iv || ivlen > 16) {
        return (gcry_error_t)-1;
    }
    memset(g->iv, 0, 16);
    memcpy(g->iv + (16 - ivlen), b, ivlen);
    return 0;
}

gcry_error_t gcry_cipher_setctr(gcry_cipher_hd_t hd, const void *ctr,
                                size_t ctrlen)
{
    struct esp_gcipher *g = (struct esp_gcipher *)hd;
    const uint8_t *b = ctr;
    if (!g || !ctr || ctrlen > 16) {
        return (gcry_error_t)-1;
    }
    memset(g->ctr, 0, 16);
    memcpy(g->ctr + (16 - ctrlen), b, ctrlen);
    return 0;
}

static gcry_error_t esp_cipher_op(gcry_cipher_hd_t hd, void *out, size_t outsz,
                                  const void *in, size_t insz, int decrypt)
{
    struct esp_gcipher *g = (struct esp_gcipher *)hd;
    if (!g || !out) {
        return (gcry_error_t)-1;
    }
    if (!in && insz == 0) {
        in = out;
        insz = outsz;
    }
    if (outsz < insz || !in) {
        return (gcry_error_t)-2;
    }

    uint8_t blk[16];
    uint8_t cbc[16];
    const uint8_t *src = in;
    uint8_t *dst = out;
    size_t off = 0;

    switch (g->mode) {
    case GCRY_CIPHER_MODE_ECB:
        if (insz % 16) {
            return (gcry_error_t)-3;
        }
        for (off = 0; off + 16 <= insz; off += 16) {
            if (decrypt) {
                AES_decrypt(src + off, dst + off, &g->dk);
            } else {
                AES_encrypt(src + off, dst + off, &g->ek);
            }
        }
        break;
    case GCRY_CIPHER_MODE_CBC:
        if (insz % 16) {
            return (gcry_error_t)-3;
        }
        for (off = 0; off + 16 <= insz; off += 16) {
            if (decrypt) {
                /* P_i = DEC(C_i) ^ C_(i-1) ; en-place safe : C_i sauvegardé */
                memcpy(cbc, src + off, 16);
                AES_decrypt(cbc, blk, &g->dk);
                for (int i = 0; i < 16; i++) {
                    dst[off + i] = blk[i] ^ g->iv[i];
                }
                memcpy(g->iv, cbc, 16);
            } else {
                for (int i = 0; i < 16; i++) {
                    blk[i] = src[off + i] ^ g->iv[i];
                }
                AES_encrypt(blk, dst + off, &g->ek);
                memcpy(g->iv, dst + off, 16);
            }
        }
        break;
    case GCRY_CIPHER_MODE_CTR:
        for (off = 0; off < insz; off += 16) {
            size_t n = insz - off < 16 ? insz - off : 16;
            AES_encrypt(g->ctr, blk, &g->ek);
            for (size_t i = 0; i < n; i++) {
                dst[off + i] = src[off + i] ^ blk[i];
            }
            esp_cipher_inc_ctr(g->ctr);
        }
        break;
    case GCRY_CIPHER_MODE_OFB:
        for (off = 0; off < insz; off += 16) {
            size_t n = insz - off < 16 ? insz - off : 16;
            AES_encrypt(g->iv, blk, &g->ek);
            for (size_t i = 0; i < n; i++) {
                dst[off + i] = src[off + i] ^ blk[i];
            }
            memcpy(g->iv, blk, 16);
        }
        break;
    case GCRY_CIPHER_MODE_CFB:
        for (off = 0; off < insz; off += 16) {
            size_t n = insz - off < 16 ? insz - off : 16;
            AES_encrypt(g->iv, blk, &g->ek);
            for (size_t i = 0; i < n; i++) {
                dst[off + i] = src[off + i] ^ blk[i];
            }
            memcpy(g->iv, dst + off, 16); /* feedback sur le bloc chiffré */
        }
        break;
    case GCRY_CIPHER_MODE_CFB8:
        for (size_t i = 0; i < insz; i++) {
            AES_encrypt(g->iv, blk, &g->ek);
            dst[i] = src[i] ^ blk[0];
            memmove(g->iv, g->iv + 1, 15);
            g->iv[15] = dst[i];
        }
        break;
    default:
        return (gcry_error_t)-4;
    }

    return 0;
}

/* CTR/OFB/CFB/CFB8 sont symétriques : decrypt == encrypt ; ECB/CBC passent
 * par la clé inverse (dk). */
gcry_error_t gcry_cipher_encrypt(gcry_cipher_hd_t hd, void *out, size_t outsz,
                                 const void *in, size_t insz)
{
    return esp_cipher_op(hd, out, outsz, in, insz, 0);
}

gcry_error_t gcry_cipher_decrypt(gcry_cipher_hd_t hd, void *out, size_t outsz,
                                 const void *in, size_t insz)
{
    return esp_cipher_op(hd, out, outsz, in, insz, 1);
}
