/*
 * POC-B : shim C pour le canal callbacks EspSimu.
 *
 * Problème: les fonctions JS ajoutées via addFunction() vivent dans la table
 * du thread qui les a créées ; emcc 3.1.50 ne partage pas la table entre les
 * instances pthreads (cf. cb_test : « table index is out of bounds »). Le
 * driver JS ne peut pas s'enregistrer directement.
 *
 * Solution: un callbacks_t construit côté C (fonctions statiques -> même
 * index de table dans toutes les instances) + un anneau mémoire écrit par
 * le shim uart_tx_event (mémoire partagée, lisible depuis n'importe quel
 * thread JS via HEAPU8). Le driver enregistre EXACTEMENT comme EspSimu :
 *   register_callbacks(qemu_picsimlab_ring_callbacks());
 * puis sonde qemu_picsimlab_uart_ring_info().
 *
 * Layout ring_info (32-bit words):
 *   [0] magic 'RING'  [1] head (producteur vCPU)  [2] tail (consommateur JS)
 *   [3] size (o)      [4] pointeur data (2 o par événement : id, val)
 */
#include "qemu/osdep.h"

#define RING_MAGIC 0x52494E47u /* 'RING' */
#define RING_SIZE  262144u     /* 256 Ko = 128K evenements (2 o/event), puissance de 2 */

typedef struct {
    void (*picsimlab_write_pin)(int pin, int value);
    void (*picsimlab_dir_pin)(int pin, int value);
    int (*picsimlab_i2c_event)(const uint8_t id, const uint8_t addr, const uint16_t event);
    uint8_t (*picsimlab_spi_event)(const uint8_t id, const uint16_t event);
    void (*picsimlab_uart_tx_event)(const uint8_t id, const uint8_t value);
    const short int *pinmap;
    void (*picsimlab_rmt_event)(const uint8_t channel, const uint32_t config0, const uint32_t value);
} callbacks_t;

extern void qemu_picsimlab_register_callbacks(void *arg);

/* .bss, mémoire partagée entre toutes les instances pthreads */
static volatile uint32_t ring_info[5];
static uint8_t ring_data[RING_SIZE];

/* pinmap inerte : aucune GPIO filée (EspSimu desktop fournit la sienne) */
static const short int ring_pinmap[102] = { [0] = 100, [1 ... 100] = -1 };

static void ring_uart_tx(const uint8_t id, const uint8_t value)
{
    uint32_t h = __atomic_load_n(&ring_info[1], __ATOMIC_RELAXED);
    uint32_t off = (h * 2u) & (RING_SIZE - 1u);
    ring_data[off] = id;
    ring_data[(off + 1u) & (RING_SIZE - 1u)] = value;
    __atomic_store_n(&ring_info[1], h + 1u, __ATOMIC_RELEASE);
}

static void ring_write_pin(int __attribute__((unused)) pin, int __attribute__((unused)) value) {}
static void ring_dir_pin(int __attribute__((unused)) pin, int __attribute__((unused)) value) {}
static int ring_i2c(const uint8_t __attribute__((unused)) id, const uint8_t __attribute__((unused)) addr,
                    const uint16_t __attribute__((unused)) event)
{
    return 0;
}
static uint8_t ring_spi(const uint8_t __attribute__((unused)) id, const uint16_t __attribute__((unused)) event)
{
    return 0;
}
static void ring_rmt(const uint8_t __attribute__((unused)) channel,
                     const uint32_t __attribute__((unused)) config0, const uint32_t __attribute__((unused)) value)
{}

static const callbacks_t ring_callbacks = {
    .picsimlab_write_pin = ring_write_pin,
    .picsimlab_dir_pin = ring_dir_pin,
    .picsimlab_i2c_event = ring_i2c,
    .picsimlab_spi_event = ring_spi,
    .picsimlab_uart_tx_event = ring_uart_tx,
    .pinmap = ring_pinmap,
    .picsimlab_rmt_event = ring_rmt,
};

/* Enregistrement exactement comme EspSimu : le driver appelle
 * qemu_picsimlab_register_callbacks(qemu_picsimlab_ring_callbacks()). */
const callbacks_t *qemu_picsimlab_ring_callbacks(void)
{
    return &ring_callbacks;
}

const uint32_t *qemu_picsimlab_uart_ring_info(void)
{
    ring_info[0] = RING_MAGIC;
    ring_info[3] = RING_SIZE;
    ring_info[4] = (uint32_t)(uintptr_t)ring_data;
    return ring_info;
}
