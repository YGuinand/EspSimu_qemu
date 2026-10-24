/*
 * ESP32 Analog Sensors peripheral
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 Yves Guinand — EspSimu : modele ADC a caracteristique
 * d'usine (calibration line-fitting inversee) et duree de conversion SAR.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 *
 * ============================== Modele ADC ==============================
 *
 * EspSimu injecte les tensions de broches analogiques en millivolts (API
 * qemu_picsimlab_set_apin_mv, bornee 0..3300). A chaque demarrage de
 * conversion (front montant du bit meas{x}_start_sar dans les registres
 * SENS_SAR_MEAS_START1/2), le modele produit le compte brut que livrerait
 * une vraie ESP32 pour cette tension, en inversant la caracteristique
 * d'usine que le firmware mesure ensuite via adc_cali.
 *
 * La caracteristique reproduite est exactement celle d'IDF v5.x
 * (components/esp_adc/esp32/adc_cali_line_fitting.c, schema LINE_FITTING,
 * avec la vref fixee par l'eFuse ADC_VREF: voir la constante
 * ESP32_EFUSE_ADC_VREF_USINE gravee par defaut dans hw/nvram/esp32_efuse.c,
 * a garder synchronisee avec ADC_VREF_MV ci-dessous). Le modele inverse
 * cette fonction par dichotomie : raw = G(unite, atten, mV).
 *   - atten 0/1/2 (0/2.5/6 dB) : droite, zone morte propre a l'atten,
 *     saturation raw=4095 au-dela de ~1051/1375/1900 mV (ADC1) ;
 *   - atten 3 (11/12 dB) : droite jusqu'a raw 2880, zone de croisement
 *     droite/LUT jusqu'a raw 2944, puis LUT bilineaire (20 points, pas de
 *     64 counts) ; saturation au-dela de ~3145 mV (ADC1) / 3131 mV (ADC2).
 * Ainsi analogReadMilliVolts() du firmware renvoie (aux arrondis pres) la
 * tension injectee, comme sur la puce reellement gravee.
 *
 * Resolution (9..12 bits) et attenuation sont relevees dans les registres
 * SENS a chaque conversion (IDF les reconfigure a chaque mesure via
 * adc_oneshot_ll_set_atten/_set_output_bits) :
 *   - attenuation: SENS_SAR_ATTEN1/2 (0x34/0x38), 2 bits par canal, reset
 *     0xFFFFFFFF (atten max avant reconfiguration IDF) ;
 *   - largeur: SENS_SAR_START_FORCE (0x2c) bits [1:0] (ADC1) et [3:2]
 *     (ADC2), defaut 12 bits ; resultats 9..11 bits obtenus par
 *     troncature >> (12 - bits), la convention supposée par IDF qui
 *     remonte << au moment de calibrer.
 *
 * Duree de conversion (horloge virtuelle) :
 *   T = (SAMPLE_CYCLE + largeur_bits + 4) * (CLK_DIV + 1) cycles de
 *   RTC_FAST_CLK (10 MHz => 100 ns par cycle APB).
 * Avec la configuration IDF5 par defaut (CLK_DIV=1, SAMPLE_CYCLE=9,
 * 12 bits) : T = 25 * 200 ns = 5.0 us, la borne du datasheet (200 ksp/s
 * via le controleur RTC). Le bit done (bit 16 du registre de start) passe
 * a 1 a l'echeance ; SENS_MEAS_STATUS ([29:22] de SLAVE_ADDR1) vaut 0xFF
 * tant qu'une conversion est en cours (attente IDF sans ancrage d'horloge).
 *
 * Ce qui n'est PAS modele : bruit/DNL (datasheet: +-7 LSB), derive
 * thermique, arbitre ADC2/WiFi, dispersion inter-puces (la puce simulee
 * est celle dont l'eFuse a ete gravee).
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32_sens.h"

/* Registres SENS (DR_REG_SENS_BASE = 0x3FF48800) */
#define A_READ_CTRL     0x000 /* SENS_SAR_READ_CTRL  : div/cycle/largeur ADC1 */
#define A_START_FORCE   0x02c /* SENS_SAR_START_FORCE: largeur ADC1/ADC2 */
#define A_ATTEN1        0x034 /* SENS_SAR_ATTEN1     : attenuation par canal ADC1 */
#define A_ATTEN2        0x038 /* SENS_SAR_ATTEN2     : idem ADC2 */
#define A_SLAVE_ADDR1   0x03c /* SENS_SAR_SLAVE_ADDR1: [29:22] = MEAS_STATUS (RO) */
#define A_MEAS_START1   0x054 /* SENS_SAR_MEAS_START1*/
#define A_TOUCH_CTRL2   0x084
#define A_READ_CTRL2    0x090 /* SENS_SAR_READ_CTRL2 : idem ADC2 */
#define A_MEAS_START2   0x094 /* SENS_SAR_MEAS_START2*/

/*
 * Caracteristique d'usine : constantes IDF (adc_cali_line_fitting.c).
 * ADC_VREF_MV DOIT rester synchrone de l'eFuse grave par defaut
 * (ESP32_EFUSE_ADC_VREF_USINE dans hw/nvram/esp32_efuse.c).
 */
#define ADC_VREF_MV     1114
#define ADC_LUT_LOW     2880  /* raw seuil bas d'usage de la LUT (atten 11/12 dB) */
#define ADC_LUT_STEP    64    /* pas (counts) entre deux points de la LUT */
#define ADC_LUT_SIZE    20

/* Filling d'usine IDF (axes: unite x atten ; 1000..1200 mV pour la LUT) */
static const uint32_t adc_scale[2][4] = {
    { 57431, 76236, 105481, 196602 },  /* ADC1 */
    { 57236, 76175, 105678, 197170 },  /* ADC2 */
};
static const uint32_t adc_offs[2][4] = {
    { 75, 78, 107, 142 },              /* ADC1 : zone morte (mV) par atten */
    { 63, 66, 89, 128 },               /* ADC2 */
};
static const uint32_t adc_lut_low[2][ADC_LUT_SIZE] = {
    { 2240, 2297, 2352, 2405, 2457, 2512, 2564, 2616, 2664, 2709,
      2754, 2795, 2832, 2868, 2903, 2937, 2969, 3000, 3030, 3060 },  /* ADC1 */
    { 2238, 2293, 2347, 2399, 2451, 2507, 2561, 2613, 2662, 2710,
      2754, 2792, 2831, 2869, 2904, 2937, 2968, 2999, 3029, 3059 },  /* ADC2 */
};
static const uint32_t adc_lut_high[2][ADC_LUT_SIZE] = {
    { 2667, 2706, 2745, 2780, 2813, 2844, 2873, 2901, 2928, 2956,
      2982, 3006, 3032, 3059, 3084, 3110, 3135, 3160, 3184, 3209 },  /* ADC1 */
    { 2657, 2698, 2738, 2774, 2807, 2838, 2867, 2894, 2921, 2946,
      2971, 2996, 3020, 3043, 3067, 3092, 3116, 3139, 3162, 3185 },  /* ADC2 */
};

/* LUT bilineaire sur l'axe vref 1000..1200 mV, calculee a notre vref usine */
static uint32_t adc_lut_mv(int unit, int raw)
{
    uint32_t idx = (raw - ADC_LUT_LOW) / ADC_LUT_STEP;  /* 0..18 pour raw<=4095 */
    int y1d = raw - (int)(idx * ADC_LUT_STEP + ADC_LUT_LOW);
    int y2d = (int)((idx + 1) * ADC_LUT_STEP + ADC_LUT_LOW) - raw;
    int x1d = ADC_VREF_MV - 1000;
    int x2d = 1200 - ADC_VREF_MV;
    int64_t v;

    v =  (int64_t)adc_lut_low[unit][idx]     * x2d * y2d
       + (int64_t)adc_lut_high[unit][idx]    * x1d * y2d
       + (int64_t)adc_lut_low[unit][idx + 1] * x2d * y1d
       + (int64_t)adc_lut_high[unit][idx + 1] * x1d * y1d;
    v = (v + ((1200 - 1000) * ADC_LUT_STEP) / 2)
      / ((1200 - 1000) * ADC_LUT_STEP);
    return (uint32_t)v;
}

/* Tension equivalente IDF d'un count brut (memes arrondis que le firmware) */
static uint32_t adc_idf_mv(int unit, int atten, int raw)
{
    uint32_t a   = (ADC_VREF_MV * adc_scale[unit][atten]) / 4096;
    uint32_t lin = ((a * raw + 32768) >> 16) + adc_offs[unit][atten];
    uint32_t lut;
    int step = ADC_LUT_STEP;
    int x;
    int v;

    if (atten != 3 || raw < ADC_LUT_LOW) {
        return lin;
    }
    lut = adc_lut_mv(unit, raw);
    if (raw <= ADC_LUT_LOW + ADC_LUT_STEP) {
        /* zone de croisement : IDF moyenne les deux courbes */
        x = raw - ADC_LUT_LOW;
        /* maths signees : lut peut passer sous lin au voisinage du seuil */
        v = ((int)lin * (step - x) + (int)lut * x + step / 2) / step;
        return (uint32_t)v;
    }
    return lut;
}

/* Inverse de adc_idf_mv : plus grand raw tel que adc_idf_mv(raw) <= mv.
 * adc_idf_mv est strictement croissante sur 0..4095 pour chaque (unite, atten). */
static int adc_mv2raw(int unit, int atten, int mv)
{
    int lo = 0;
    int hi = 4095;

    if (mv <= 0) {
        return 0;
    }
    while (lo < hi) {
        int raw = (lo + hi + 1) / 2;

        if ((int)adc_idf_mv(unit, atten, raw) <= mv) {
            lo = raw;
        } else {
            hi = raw - 1;
        }
    }
    return lo;
}

/* conversion en cours ? (le bit done se releve une fois la latence ecoulee :
 * toute lecture de l'unite met l'etat a jour) */
static bool sens_busy(Esp32SensState *s, int unit)
{
    if (s->meas_busy[unit]
        && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->meas_end_ns[unit]) {
        s->meas_busy[unit] = false;
    }
    return s->meas_busy[unit];
}

/*
 * Lecture de SENS_SAR_MEAS_STARTx : le lecteur est preleve a la conversion
 * en cours, sinon le bit done reste a 1 avec la valeur figee jusqu'au
 * prochain front de start (comportement du controleur RTC).
 */
static uint32_t sens_meas_read(Esp32SensState *s, int unit)
{
    uint32_t r = s->meas_start[unit] | ((uint32_t)((s->start_bit >> unit) & 1) << 17);

    if (!sens_busy(s, unit)) {
        r |= 0x10000 | s->meas_data[unit];
    }
    return r;
}

/*
 * Demarrage d'une conversion : la configuration (atten, largeur) est
 * prelevée au front de start, la valeur brute calculee par inversion de la
 * caracteristique d'usine, et l'echeance posee en temps virtuel.
 */
static void sens_start_conv(Esp32SensState *s, int unit, int chan)
{
    int atten = (s->sar_atten[unit] >> (2 * chan)) & 3;
    int width = 9 + (int)((s->sar_start_force >> (2 * unit)) & 3);
    int idx = unit ? (8 + chan) : chan;
    int raw = adc_mv2raw(unit, atten, s->apin_mv[idx]);

    if (width < 12) {
        raw >>= (12 - width);  /* troncature 9..11 bits */
    }
    s->meas_data[unit] = (uint16_t)raw;
    s->meas_busy[unit] = true;
    s->meas_end_ns[unit] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                           + (int64_t)(((s->sar_read_ctrl[unit] >> 8) & 0xFF)
                                       + width + 4)
                             * ((s->sar_read_ctrl[unit] & 0xFF) + 1) * 100;
}

/*
 * Ecriture de SENS_SAR_MEAS_STARTx : le IDF pulse le bit 17 (0 puis 1),
 * en_pad [30:19] portant le canal. Le RMW de en_pad peut transporter un
 * start deja a 1 de la conversion precedente : seul un front montant (bit
 * passe de 0 a 1 entre deux ecritures) demarre une conversion.
 */
static void sens_meas_write(Esp32SensState *s, int unit, uint64_t value)
{
    uint32_t start = (value >> 17) & 1;
    uint32_t prev = (s->start_bit >> unit) & 1;
    uint32_t en_pad = (value >> 19) & 0xFFF;

    s->meas_start[unit] = value & 0xFFFC0000u;  /* [31] en_pad_force, [30:19]
                                                 * en_pad, [18] start_force */
    if (start && !prev && en_pad) {
        sens_start_conv(s, unit, ctz32(en_pad));
    }
    if (start) {
        s->start_bit |= (1u << unit);
    } else {
        s->start_bit &= ~(1u << unit);
    }
}

static uint64_t esp32_sens_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);
    uint32_t r = 0;

    switch (addr) {
    case A_READ_CTRL:
        return s->sar_read_ctrl[0];
    case A_START_FORCE:
        return s->sar_start_force;
    case A_ATTEN1:
        return s->sar_atten[0];
    case A_ATTEN2:
        return s->sar_atten[1];
    case A_MEAS_START1:
        return sens_meas_read(s, 0);
    case A_SLAVE_ADDR1:
        /* status : unitee occupee tant qu'une conversion tourne (un FD wait
         * IDF sans ancrage horloge attend d'abord ici) */
        if (sens_busy(s, 0) || sens_busy(s, 1)) {
            r |= 0xFFu << 22;
        }
        return r | s->sar_slave_addr1;
    case A_TOUCH_CTRL2:
        return (1u << 10);
    case A_READ_CTRL2:
        return s->sar_read_ctrl[1];
    case A_MEAS_START2:
        return sens_meas_read(s, 1);
    default:
        break;
    }
    /* touches capacitives 0x70..0x80 : lecture passee d'un ancien code,
     * l'ecran tactile appellateur un dans un autre tableau (st7789v.c) */
    if (addr >= 0x70 && addr < 0x84) {
        int n1 = ((addr - 0x70) / 4) * 2;
        return ((1500 - s->touch_sensor[n1] + rand() % 20) << 16)
             | (1500 - s->touch_sensor[n1 + 1] + rand() % 20);
    }
    return r;
}

static void esp32_sens_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);

    switch (addr) {
    case A_READ_CTRL:
        s->sar_read_ctrl[0] = value;
        break;
    case A_START_FORCE:
        s->sar_start_force = value;
        break;
    case A_ATTEN1:
        s->sar_atten[0] = value;
        break;
    case A_ATTEN2:
        s->sar_atten[1] = value;
        break;
    case A_SLAVE_ADDR1:
        s->sar_slave_addr1 = value & 0x3FFFFFu;
        break;
    case A_MEAS_START1:
        sens_meas_write(s, 0, value);
        break;
    case A_READ_CTRL2:
        s->sar_read_ctrl[1] = value;
        break;
    case A_MEAS_START2:
        sens_meas_write(s, 1, value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps esp32_sens_ops = {
    .read =  esp32_sens_read,
    .write = esp32_sens_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_sens_init(Object *obj)
{
    Esp32SensState *s = ESP32_SENS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_sens_ops, s,
                          TYPE_ESP32_SENS, 0x400);
    sysbus_init_mmio(sbd, &s->iomem);

    /* valeurs de reset des registres miroirs (IDF v5.5.4 soc/sens_reg.h) */
    memset(s->touch_sensor, 0, sizeof(s->touch_sensor));
    memset(s->apin_mv, 0, sizeof(s->apin_mv));
    s->sar_read_ctrl[0] = (1u << 18)        /* SAR1_CLK_GATED          : 1 */
                        | (3u << 16)        /* SAR1_SAMPLE_BIT         : 3 (12 bits) */
                        | (9u << 8)         /* SAR1_SAMPLE_CYCLE       : 9 */
                        | 2u;               /* SAR1_CLK_DIV            : 2 */
    s->sar_read_ctrl[1] = s->sar_read_ctrl[0];
    s->sar_start_force = 0xF;               /* SAR1/SAR2_BIT_WIDTH     : 3 (12 bits) */
    s->sar_atten[0] = 0xFFFFFFFF;           /* atten max avant reconfig IDF */
    s->sar_atten[1] = 0xFFFFFFFF;
    s->sar_slave_addr1 = 0;
    s->meas_start[0] = 0;
    s->meas_start[1] = 0;
    s->start_bit = 0;
    s->meas_busy[0] = s->meas_busy[1] = false;
    s->meas_end_ns[0] = s->meas_end_ns[1] = 0;
    s->meas_data[0] = s->meas_data[1] = 0;
}

static const TypeInfo esp32_sens_info = {
    .name = TYPE_ESP32_SENS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SensState),
    .instance_init = esp32_sens_init,
};

static void esp32_sens_register_types(void)
{
    type_register_static(&esp32_sens_info);
}

type_init(esp32_sens_register_types)
