#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_reg.h"

#define TYPE_ESP32_SENS "misc.esp32.sens"
#define ESP32_SENS(obj) OBJECT_CHECK(Esp32SensState, (obj), TYPE_ESP32_SENS)

typedef struct Esp32SensState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int touch_sensor[10];

    /* Tension injectee par EspSimu, en millivolts (API qemu_picsimlab_set_apin_mv).
     * Index 0..7  : canaux ADC1 CH0..CH7 (GPIO36..GPIO39/GPIO32..35...)
     * Index 8..17 : canaux ADC2 CH0..CH9  (GPIO0..GPIO4, 13..15, 25, 26)
     * (l'ancienne API counts-based set_apin a ete supprimee : le renommage
     * sert d'obstacle a un client compile pour l'ancienne convention). */
    int16_t apin_mv[32];

    /* Miroirs des registres SENS impliques dans la conversion SAR. */
    uint32_t sar_read_ctrl[2];  /* SENS_SAR_READ_CTRL / _CTRL2 : clk_div[7:0],
                                 * sample_cycle[15:8], sample_bit[17:16] */
    uint32_t sar_start_force;   /* SENS_SAR_START_FORCE : sar1_bit_width[1:0],
                                 * sar2_bit_width[3:2] */
    uint32_t sar_atten[2];      /* SENS_SAR_ATTEN1/2 : 2 bits par canal */
    uint32_t sar_slave_addr1;   /* SENS_SAR_SLAVE_ADDR1 bits [21:0] (status RO apart) */
    uint32_t meas_start[2];     /* bits [31:18] de SENS_SAR_MEAS_START1/2 */
    uint8_t start_bit;          /* etat du bit start (17) par unite : bit0 ADC1,
                                 * bit1 ADC2 (front montant = lancement) */

    /* Conversion en cours : resultat fige au demarrage, done releve a
     * l'echeance du temps virtuel (latence hardware modele, voir esp32_sens.c). */
    bool meas_busy[2];
    int64_t meas_end_ns[2];
    uint16_t meas_data[2];
} Esp32SensState;
