/*
 * ESP32 Interrupt Matrix
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/xtensa/esp32_intc.h"
#include "hw/misc/esp32_reg.h"   /* [QDIAG] numeros des sources ETS_* */
#include "qemu/timer.h"   /* [QDIAG] horodatage des livraisons IRQ */

#define INTMATRIX_UNINT_VALUE   6

#define IRQ_MAP(cpu, input) s->irq_map[cpu][input]

/* QDIAG : sources du fenetrage (timers/watchdog des 2 groupes) */
#define QDIAG_INTM_SRC_INTERESSES   { \
    ETS_TG0_T0_LEVEL_INTR_SOURCE,  ETS_TG1_T0_LEVEL_INTR_SOURCE,   \
    ETS_TG0_T1_LEVEL_INTR_SOURCE,  ETS_TG1_T1_LEVEL_INTR_SOURCE,   \
    ETS_TG0_WDT_LEVEL_INTR_SOURCE, ETS_TG1_WDT_LEVEL_INTR_SOURCE,  \
    ETS_TG0_T0_EDGE_INTR_SOURCE,   ETS_TG1_T0_EDGE_INTR_SOURCE,    \
    ETS_TG0_T1_EDGE_INTR_SOURCE,   ETS_TG1_T1_EDGE_INTR_SOURCE,    \
    ETS_TG0_WDT_EDGE_INTR_SOURCE,  ETS_TG1_WDT_EDGE_INTR_SOURCE,   \
    ETS_TG0_LACT_LEVEL_INTR_SOURCE, ETS_TG1_LACT_LEVEL_INTR_SOURCE }

static bool esp32_intmatrix_src_interessante(int n)
{
    static const int map[] = QDIAG_INTM_SRC_INTERESSES;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (map[i] == n) return true;
    }
    return false;
}

static void esp32_intmatrix_irq_handler(void *opaque, int n, int level)
{
    Esp32IntMatrixState *s = ESP32_INTMATRIX(opaque);

    s->irq_raw[n] = level;
    /* QDIAG : trace des changements de niveau des IRQ timers/WDT vers les CPU */
    for (int i = 0; i < ESP32_CPU_COUNT; ++i) {
        if (s->outputs[i] == NULL || IRQ_MAP(i, n) == INTMATRIX_UNINT_VALUE) {
            continue;
        }
        int out_index = IRQ_MAP(i, n);
        for (int int_index = 0; int_index < s->cpu[i]->env.config->nextint; ++int_index) {
            if (s->cpu[i]->env.config->extint[int_index] == out_index) {
                if (esp32_intmatrix_src_interessante(n)) {
                    static int der_niveau[2][32];
                    static uint32_t qdiag_intm_cnt;
                    if (int_index < 32 && der_niveau[i][int_index] != level) {
                        der_niveau[i][int_index] = level;
                        ++qdiag_intm_cnt;
                        fprintf(stderr, "[QDIAG] INTM TG src=%d -> cpu%d int%d (=%d) n%u vclk=%llums\n",
                                n, i, int_index, level, qdiag_intm_cnt,
                                (unsigned long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
                    }
                }
                qemu_set_irq(s->outputs[i][int_index], level);
                break;
            }
        }
    }
}

static inline uint8_t* get_map_entry(Esp32IntMatrixState* s, hwaddr addr)
{
    int source_index = addr / sizeof(uint32_t);
    if (source_index > ESP32_INT_MATRIX_INPUTS * ESP32_CPU_COUNT) {
        error_report("%s: source_index %d out of range", __func__, source_index);
        return NULL;
    }
    int cpu_index = source_index / ESP32_INT_MATRIX_INPUTS;
    source_index = source_index % ESP32_INT_MATRIX_INPUTS;
    return &IRQ_MAP(cpu_index, source_index);
}

static uint64_t esp32_intmatrix_read(void* opaque, hwaddr addr, unsigned int size)
{
    Esp32IntMatrixState *s = ESP32_INTMATRIX(opaque);
    uint8_t* map_entry = get_map_entry(s, addr);
    return (map_entry != NULL) ? *map_entry : 0;
}

static void esp32_intmatrix_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32IntMatrixState *s = ESP32_INTMATRIX(opaque);
    int source_index = (addr / sizeof(uint32_t)) % ESP32_INT_MATRIX_INPUTS;
    uint8_t* map_entry = get_map_entry(s, addr);
    /* QDIAG : trace des routages ecrits par le guest */
    if (map_entry != NULL && *map_entry != (value & 0x1f)) {
        int cpu_index = (addr / sizeof(uint32_t)) / ESP32_INT_MATRIX_INPUTS;
        fprintf(stderr, "[QDIAG] INTM map cpu%d src%d -> int%d vclk=%llums\n",
                cpu_index, source_index, (int)(value & 0x1f),
                (unsigned long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
    }
    if (value == INTMATRIX_UNINT_VALUE) {
        int si = s->irq_raw[source_index];
        esp32_intmatrix_irq_handler(s, source_index, 0);
        s->irq_raw[source_index] = si;
    }
    if (map_entry != NULL) {
        *map_entry = value & 0x1f;
    }
    if (value != INTMATRIX_UNINT_VALUE && s->irq_raw[source_index]) {
        esp32_intmatrix_irq_handler(s, source_index, 1);
    }
}

static const MemoryRegionOps esp_intmatrix_ops = {
    .read =  esp32_intmatrix_read,
    .write = esp32_intmatrix_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_intmatrix_reset_hold(Object *obj, ResetType type)
{
    Esp32IntMatrixState *s = ESP32_INTMATRIX(obj);
    memset(s->irq_map, INTMATRIX_UNINT_VALUE, sizeof(s->irq_map));
    for (int i = 0; i < ESP32_CPU_COUNT; ++i) {
        if (s->outputs[i] == NULL) {
            continue;
        }
        for (int int_index = 0; int_index < s->cpu[i]->env.config->nextint; ++int_index) {
            qemu_irq_lower(s->outputs[i][int_index]);
        }
    }

}

static void esp32_intmatrix_realize(DeviceState *dev, Error **errp)
{
    Esp32IntMatrixState *s = ESP32_INTMATRIX(dev);

    for (int i = 0; i < ESP32_CPU_COUNT; ++i) {
        if (s->cpu[i]) {
            s->outputs[i] = xtensa_get_extints(&s->cpu[i]->env);
        }
    }
    esp32_intmatrix_reset_hold(OBJECT(dev), RESET_TYPE_COLD);
}

static void esp32_intmatrix_init(Object *obj)
{
    Esp32IntMatrixState *s = ESP32_INTMATRIX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp_intmatrix_ops, s,
                          TYPE_ESP32_INTMATRIX, ESP32_INT_MATRIX_INPUTS * ESP32_CPU_COUNT * sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in(DEVICE(s), esp32_intmatrix_irq_handler, ESP32_INT_MATRIX_INPUTS);
}

static Property esp32_intmatrix_properties[] = {
    DEFINE_PROP_LINK("cpu0", Esp32IntMatrixState, cpu[0], TYPE_XTENSA_CPU, XtensaCPU *),
    DEFINE_PROP_LINK("cpu1", Esp32IntMatrixState, cpu[1], TYPE_XTENSA_CPU, XtensaCPU *),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_intmatrix_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_intmatrix_reset_hold;
    dc->realize = esp32_intmatrix_realize;
    device_class_set_props(dc, esp32_intmatrix_properties);
}

static const TypeInfo esp32_intmatrix_info = {
    .name = TYPE_ESP32_INTMATRIX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32IntMatrixState),
    .instance_init = esp32_intmatrix_init,
    .class_init = esp32_intmatrix_class_init
};

static void esp32_intmatrix_register_types(void)
{
    type_register_static(&esp32_intmatrix_info);
}

type_init(esp32_intmatrix_register_types)
