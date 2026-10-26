/* esp_gcrypt_shim.h : mini compat layer libgcrypt pour EspSimu QEMU wasm.
 *
 * Couvre exactement le sous-ensemble de libgcrypt utilisé par la lignée
 * (hw/misc/esp32_rsa.c, esp_rsa.c, esp_aes.c, esp32_flash_enc.c) :
 *   - MPI : mpi_t/new/scan/print/powm/mul/mulm/invm/set_bit/release (USG big-endian)
 *   - Cipher : cipher_hd_t/open/setkey/setiv/setctr/encrypt/close
 *              (AES128/AES256 × modes ECB/CBC/CFB/CFB8/CTR/OFB)
 * L'AES s'appuie sur les primitives internes de QEMU (crypto/aes.h).
 */
#ifndef HW_MISC_ESP_GCRYPT_SHIM_H
#define HW_MISC_ESP_GCRYPT_SHIM_H

#include "qemu/osdep.h"

typedef unsigned int gcry_error_t;
typedef struct esp_mpi *gcry_mpi_t;
typedef struct esp_gcipher *gcry_cipher_hd_t;

enum { GCRYMPI_FMT_USG = 5 };

enum gcry_cipher_algos {
    GCRY_CIPHER_NONE = 0,
    GCRY_CIPHER_AES128,
    GCRY_CIPHER_AES192,
    GCRY_CIPHER_AES256,
};

enum gcry_cipher_modes {
    GCRY_CIPHER_MODE_NONE = 0,
    GCRY_CIPHER_MODE_ECB,
    GCRY_CIPHER_MODE_CBC,
    GCRY_CIPHER_MODE_CFB,
    GCRY_CIPHER_MODE_CFB8,
    GCRY_CIPHER_MODE_CTR,
    GCRY_CIPHER_MODE_OFB,
};

/* --- MPI (unsigned only, USG big-endian à l'interface) --- */
gcry_mpi_t gcry_mpi_new(size_t nbytes);
void gcry_mpi_release(gcry_mpi_t a);
gcry_error_t gcry_mpi_scan(gcry_mpi_t *out, int fmt, const void *buf,
                           size_t len, size_t *scanned);
gcry_error_t gcry_mpi_print(int fmt, void *buf, size_t bufsz, size_t *written,
                            const gcry_mpi_t a);
void gcry_mpi_set_bit(gcry_mpi_t a, unsigned int bit);
void gcry_mpi_mul(gcry_mpi_t res, const gcry_mpi_t x, const gcry_mpi_t y);
void gcry_mpi_mulm(gcry_mpi_t res, const gcry_mpi_t x, const gcry_mpi_t y,
                   const gcry_mpi_t m);
void gcry_mpi_powm(gcry_mpi_t res, const gcry_mpi_t x, const gcry_mpi_t y,
                   const gcry_mpi_t m);
int gcry_mpi_invm(gcry_mpi_t a, const gcry_mpi_t r, const gcry_mpi_t m);

/* --- Cipher --- */
gcry_error_t gcry_cipher_open(gcry_cipher_hd_t *hd, int algo, int mode,
                              unsigned int flags);
void gcry_cipher_close(gcry_cipher_hd_t hd);
gcry_error_t gcry_cipher_setkey(gcry_cipher_hd_t hd, const void *key,
                                size_t keylen);
gcry_error_t gcry_cipher_setiv(gcry_cipher_hd_t hd, const void *iv,
                               size_t ivlen);
gcry_error_t gcry_cipher_setctr(gcry_cipher_hd_t hd, const void *ctr,
                                size_t ctrlen);
gcry_error_t gcry_cipher_encrypt(gcry_cipher_hd_t hd, void *out, size_t outsz,
                                 const void *in, size_t insz);
gcry_error_t gcry_cipher_decrypt(gcry_cipher_hd_t hd, void *out, size_t outsz,
                                 const void *in, size_t insz);

/* messages d'erreur factices (les .c lignée ne font qu'afficher) */
const char *gcry_strerror(gcry_error_t err);

#endif /* HW_MISC_ESP_GCRYPT_SHIM_H */
