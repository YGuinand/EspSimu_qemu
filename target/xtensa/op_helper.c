/*
 * Copyright (c) 2011, Max Filippov, Open Source and Linux Lab.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Open Source and Linux Lab nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/page-protection.h"
#include "qemu/host-utils.h"
#include "exec/exec-all.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"

#ifndef CONFIG_USER_ONLY

void HELPER(update_ccount)(CPUXtensaState *env)
{
    XtensaCPU *cpu = env_archcpu(env);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t prev = env->sregs[CCOUNT];
    uint64_t prev_time = env->ccount_time;

    env->ccount_time = now;
    env->sregs[CCOUNT] = env->ccount_base +
        (uint32_t)clock_ns_to_ticks(cpu->clock, now - env->time_base);

    /* QDIAG : saut de CCOUNT (rattrapage vclk) pour prouver le wrap CCOMPARE */
    {
        uint64_t d_us = (now - prev_time) / 1000;
        if (d_us > 5000) {
            fprintf(stderr, "[QDIAG] CCNTj cpu%d dt=%llums jump=%d ccount=%08x "
                            "vclk=%llums\n",
                    env_cpu(env)->cpu_index,
                    (unsigned long long)(d_us / 1000),
                    (int32_t)(env->sregs[CCOUNT] - prev),
                    (unsigned)env->sregs[CCOUNT],
                    (unsigned long long)(now / 1000000));
        }
    }
}

void HELPER(wsr_ccount)(CPUXtensaState *env, uint32_t v)
{
    int i;
    uint32_t delta;

    fprintf(stderr, "[QDIAG] WSRcc cpu%d v=%08x pc=%08x vclk=%llums\n",
            env_cpu(env)->cpu_index, v, (unsigned)env->pc,
            (unsigned long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
    HELPER(update_ccount)(env);
    delta = v - env->sregs[CCOUNT];
    env->ccount_base += v - env->sregs[CCOUNT];
    /*
     * Translater les CCOMPARE en attente du meme delta que la base recalee :
     * les echeances relatives du guest sont preservees. Sur du vrai materiel
     * le recalage RTC (esp_rtc_init) ne recale jamais le compteur de milliards
     * de ticks ; en QEMU les deux calendriers divergent et sans translation,
     * une alarme armee quelques microsecondes avant le recal (compare ~ancien
     * count + 1 ms) se retrouve 'far-head' : sdelta +2 milliards de ticks,
     * cadence du tick_rtos perdue des secondes apres le boot == lag vu en IHM.
     */
    for (i = 0; i < env->config->nccompare; ++i) {
        env->sregs[CCOMPARE + i] += delta;
        HELPER(update_ccompare)(env, i);
    }
}

/*
 * Changement de frequence de l'horloge CPU (machine esp32 / esp32-picsimlab) :
 * re-ancrer le CCOUNT au moment du changement. Sans cela, HELPER(update_ccount)
 * calcule CCOUNT = ccount_base + temps_ecoule_depuis_time_base (fixe au boot de
 * la VM) ; apres clock_update_hz, tout le temps ecoule est rejoue au NOUVEAU
 * taux : CCOUNT bondit instantanement de milliards de ticks. Une alarme
 * CCOMPARE ecrite juste avant devient alors sdelta > 2 milliards de ticks
 * (>= INT32_MAX en signe) : le tick_rtos FreeRTOS ne tire plus pendant des
 * secondes (gel du guest vu en IHM apres le premier tour de loop), ou au
 * contraire tempete d'alarmes echuees si le saut est en arriere. Sementique HW :
 * au changement de frequence la VALEUR du compteur est conservee, seule la
 * vitesse d'avance change ensuite ; les alarmes CCOMPARE en attente sont donc
 * retimisees au nouveau taux (comme un WSR.CCOUNT aurait refait au WSR).
 */
void xtensa_ccount_rebase(CPUXtensaState *env)
{
    XtensaCPU *cpu = env_archcpu(env);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int i;

    /* la conversion utilise la frequence ENCORE courante (ancienne) : la valeur
     * du compteur est conservee, seule la vitesse changera apres */
    env->sregs[CCOUNT] = env->ccount_base +
        (uint32_t)clock_ns_to_ticks(cpu->clock, now - env->time_base);
    env->ccount_base = env->sregs[CCOUNT];
    env->time_base = now;

    fprintf(stderr, "[QDIAG] CCREb cpu%d ccount=%08x vclk=%llums\n",
            env_cpu(env)->cpu_index, (unsigned)env->sregs[CCOUNT],
            (unsigned long long)(now / 1000000));

    for (i = 0; i < env->config->nccompare; ++i) {
        HELPER(update_ccompare)(env, i);
    }
}

void HELPER(update_ccompare)(CPUXtensaState *env, uint32_t i)
{
    XtensaCPU *cpu = env_archcpu(env);
    uint64_t dcc;
    int32_t sdelta;

    qatomic_and(&env->sregs[INTSET],
               ~(1u << env->config->timerint[i]));
    HELPER(update_ccount)(env);
    /*
     * Fix : le calcul historique dcc = (uint64_t)(CCOMPARE - CCOUNT - 1) + 1
     * empile en uint32 : si l'alarme est deja echue (CCOUNT >= CCOMPARE,
     * typiquement au rattrapage de l'horloge virtuelle qui fait bondir
     * CCOUNT), dcc devient ~2^32 ticks (~26,8 s a 160 MHz) et le timer est
     * reprogramme loin dans le futur au lieu de tirer tout de suite.
     * L'ISR tick de FreeRTOS, qui rattrape en re-ecrivant CCOMPARE derriere
     * CCOUNT, reprogramme alors l'echeance a chaque iteration : l'alarme
     * devient inatteignable et le CPU se fige. Semantique Xtensa : une
     * alarme echue (CCOUNT >= CCOMPARE) doit tirer immediat ; le callback
     * est one-shot, c'est la prochaine ecriture WSR.CCOMPARE qui rearme.
     */
    sdelta = (int32_t)(env->sregs[CCOMPARE + i] - env->sregs[CCOUNT]);
    dcc = (sdelta <= 0) ? 0 : (uint64_t)sdelta;

    /* QDIAG : alarme echue (CCOUNT >= CCOMPARE) -> dcc=0us, tirage immediat */
    {
        int qdiag_cpu = env_cpu(env)->cpu_index;
        if (sdelta <= 0) {
            fprintf(stderr, "[QDIAG] WRAPc cpu%d c%d ccmp=%08x ccount=%08x "
                            "sdelta=%d dcc=%lluus pc=%08x vclk=%llums\n",
                    qdiag_cpu, i,
                    (unsigned)env->sregs[CCOMPARE + i],
                    (unsigned)env->sregs[CCOUNT],
                    sdelta,
                    (unsigned long long)(clock_ticks_to_ns(cpu->clock, dcc) / 1000),
                    (unsigned)env->pc,
                    (unsigned long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
        }
    }

    /* QDIAG : re-armes ecrites par le guest (cadence du tick_rtos) */
    {
        static uint32_t qdiag_arm_cnt;
        static uint64_t qdiag_last_dcc[4][MAX_NCCOMPARE];
        int qdiag_cpu = env_cpu(env)->cpu_index;
        uint64_t old_dcc = qdiag_last_dcc[qdiag_cpu][i];
        if (old_dcc != dcc) {
            fprintf(stderr, "[QDIAG] CCMPa cpu%d c%d dcc=%lluus old=%lluus pc=%08x "
                            "ccmp=%08x ccount=%08x sdelta=%d vclk=%llums\n",
                    qdiag_cpu, i,
                    (unsigned long long)(clock_ticks_to_ns(cpu->clock, dcc) / 1000),
                    (unsigned long long)(clock_ticks_to_ns(cpu->clock, old_dcc) / 1000),
                    (unsigned)env->pc,
                    (unsigned)env->sregs[CCOMPARE + i],
                    (unsigned)env->sregs[CCOUNT],
                    sdelta,
                    (unsigned long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
            qdiag_last_dcc[qdiag_cpu][i] = dcc;
        } else if (++qdiag_arm_cnt % 256 == 0) {
            fprintf(stderr, "[QDIAG] CCMPa cpu%d c%d dcc=%lluus (identique n%u) vclk=%llums\n",
                    qdiag_cpu, i,
                    (unsigned long long)(clock_ticks_to_ns(cpu->clock, dcc) / 1000),
                    qdiag_arm_cnt,
                    (unsigned long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000));
        }
    }

    timer_mod(env->ccompare[i].timer,
              env->ccount_time + clock_ticks_to_ns(cpu->clock, dcc));
    env->yield_needed = 1;
}

/*!
 * Check vaddr accessibility/cache attributes and raise an exception if
 * specified by the ATOMCTL SR.
 *
 * Note: local memory exclusion is not implemented
 */
void HELPER(check_atomctl)(CPUXtensaState *env, uint32_t pc, uint32_t vaddr)
{
    uint32_t paddr, page_size, access;
    uint32_t atomctl = env->sregs[ATOMCTL];
    int rc = xtensa_get_physical_addr(env, true, vaddr, 1,
            xtensa_get_cring(env), &paddr, &page_size, &access);

    /*
     * s32c1i never causes LOAD_PROHIBITED_CAUSE exceptions,
     * see opcode description in the ISA
     */
    if (rc == 0 &&
            (access & (PAGE_READ | PAGE_WRITE)) != (PAGE_READ | PAGE_WRITE)) {
        rc = STORE_PROHIBITED_CAUSE;
    }

    if (rc) {
        HELPER(exception_cause_vaddr)(env, pc, rc, vaddr);
    }

    /*
     * When data cache is not configured use ATOMCTL bypass field.
     * See ISA, 4.3.12.4 The Atomic Operation Control Register (ATOMCTL)
     * under the Conditional Store Option.
     */
    if (!xtensa_option_enabled(env->config, XTENSA_OPTION_DCACHE)) {
        access = PAGE_CACHE_BYPASS;
    }

    switch (access & PAGE_CACHE_MASK) {
    case PAGE_CACHE_WB:
        atomctl >>= 2;
        /* fall through */
    case PAGE_CACHE_WT:
        atomctl >>= 2;
        /* fall through */
    case PAGE_CACHE_BYPASS:
        if ((atomctl & 0x3) == 0) {
            HELPER(exception_cause_vaddr)(env, pc,
                    LOAD_STORE_ERROR_CAUSE, vaddr);
        }
        break;

    case PAGE_CACHE_ISOLATE:
        HELPER(exception_cause_vaddr)(env, pc,
                LOAD_STORE_ERROR_CAUSE, vaddr);
        break;

    default:
        break;
    }
}

void HELPER(check_exclusive)(CPUXtensaState *env, uint32_t pc, uint32_t vaddr,
                             uint32_t is_write)
{
    uint32_t paddr, page_size, access;
    uint32_t atomctl = env->sregs[ATOMCTL];
    int rc = xtensa_get_physical_addr(env, true, vaddr, is_write,
                                      xtensa_get_cring(env), &paddr,
                                      &page_size, &access);

    if (rc) {
        HELPER(exception_cause_vaddr)(env, pc, rc, vaddr);
    }

    /* When data cache is not configured use ATOMCTL bypass field. */
    if (!xtensa_option_enabled(env->config, XTENSA_OPTION_DCACHE)) {
        access = PAGE_CACHE_BYPASS;
    }

    switch (access & PAGE_CACHE_MASK) {
    case PAGE_CACHE_WB:
        atomctl >>= 2;
        /* fall through */
    case PAGE_CACHE_WT:
        atomctl >>= 2;
        /* fall through */
    case PAGE_CACHE_BYPASS:
        if ((atomctl & 0x3) == 0) {
            HELPER(exception_cause_vaddr)(env, pc,
                                          EXCLUSIVE_ERROR_CAUSE, vaddr);
        }
        break;

    case PAGE_CACHE_ISOLATE:
        HELPER(exception_cause_vaddr)(env, pc,
                LOAD_STORE_ERROR_CAUSE, vaddr);
        break;

    default:
        break;
    }
}

void HELPER(wsr_memctl)(CPUXtensaState *env, uint32_t v)
{
    if (xtensa_option_enabled(env->config, XTENSA_OPTION_ICACHE)) {
        if (extract32(v, MEMCTL_IUSEWAYS_SHIFT, MEMCTL_IUSEWAYS_LEN) >
            env->config->icache_ways) {
            deposit32(v, MEMCTL_IUSEWAYS_SHIFT, MEMCTL_IUSEWAYS_LEN,
                      env->config->icache_ways);
        }
    }
    if (xtensa_option_enabled(env->config, XTENSA_OPTION_DCACHE)) {
        if (extract32(v, MEMCTL_DUSEWAYS_SHIFT, MEMCTL_DUSEWAYS_LEN) >
            env->config->dcache_ways) {
            deposit32(v, MEMCTL_DUSEWAYS_SHIFT, MEMCTL_DUSEWAYS_LEN,
                      env->config->dcache_ways);
        }
        if (extract32(v, MEMCTL_DALLOCWAYS_SHIFT, MEMCTL_DALLOCWAYS_LEN) >
            env->config->dcache_ways) {
            deposit32(v, MEMCTL_DALLOCWAYS_SHIFT, MEMCTL_DALLOCWAYS_LEN,
                      env->config->dcache_ways);
        }
    }
    env->sregs[MEMCTL] = v & env->config->memctl_mask;
}

#endif

uint32_t HELPER(rer)(CPUXtensaState *env, uint32_t addr)
{
#ifndef CONFIG_USER_ONLY
    return address_space_ldl(env->address_space_er, addr,
                             MEMTXATTRS_UNSPECIFIED, NULL);
#else
    return 0;
#endif
}

void HELPER(wer)(CPUXtensaState *env, uint32_t data, uint32_t addr)
{
#ifndef CONFIG_USER_ONLY
    address_space_stl(env->address_space_er, addr, data,
                      MEMTXATTRS_UNSPECIFIED, NULL);
#endif
}
