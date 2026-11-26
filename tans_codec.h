/*
 tans_codec.h - núcleo do tANS estático agnóstico: codificação e decodificação
 (usadas por Encoder_Ans.c e Decoder.c) e a medição de custo do sweep
 (sweep_tabelas.py compila esta parte como sweep_custo.so).

 O plano N usa SEMPRE a tabela fixa MODELOS[N] de tans_tables.h (gerada pelo
 rANS). Nada é escolhido em tempo de execução.

 Formato do arquivo .bin:
   [num_symbols 4B] [plano (bits 0-2) | assinatura do modelo (bits 3-7) 1B]
   payload = bitstream tANS + estado final (flush_bits) + stop bit '1' + zeros
*/
#ifndef TANS_CODEC_H
#define TANS_CODEC_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tans_tables.h"

typedef struct {
    long long bits_bitstream; /* bits emitidos pelos símbolos */
    int bits_estado;          /* estado final + stop bit */
    int bits_preenchimento;   /* zeros até completar o byte */
    size_t payload_bytes;
} TansStats;

/* Capacidade segura de saída: no pior caso cada símbolo emite ~log2(2L) bits */
static inline size_t tans_capacidade(size_t n)
{
    return 2 * n + 16;
}

/* ================================================================ encoder */

/* Escreve os 'nb' bits menos significativos de 'valor' (do mais significativo
   para o menos) na posição 'pos' de 'out' e avança 'pos'.
   Retorna 0 se não couber em 'limite' bits. */
static inline int tans_escrever_bits(uint8_t *out, long long *pos, long long limite, uint32_t valor,
                                     int nb)
{
    for (int i = nb - 1; i >= 0; i--) {
        if (*pos >= limite) {
            return 0;
        }
        if ((valor >> i) & 1) {
            out[*pos >> 3] |= (uint8_t)(0x80 >> (*pos & 7));
        }
        (*pos)++;
    }
    return 1;
}

/* Codifica n símbolos (0/1) com a tabela fixa do plano. 'out' é zerado aqui.
   Retorna o tamanho do payload em bytes, ou 0 se não couber em 'cap'. */
static inline size_t tans_encode(const uint8_t *sym, size_t n, const ModeloPlano *m, uint8_t *out,
                                 size_t cap, TansStats *st)
{
    memset(out, 0, cap);
    long long pos = 0;
    long long limite = (long long)cap * 8;

    uint32_t idx = 0; /* estado atual - L (o estado inicial é L) */
    for (size_t i = 0; i < n; i++) {
        int ok;
        if (sym[i] == 0) {
            ok = tans_escrever_bits(out, &pos, limite, m->a_bits[idx], m->a_nb[idx]);
            idx = m->a_ns[idx];
        } else {
            ok = tans_escrever_bits(out, &pos, limite, m->b_bits[idx], m->b_nb[idx]);
            idx = m->b_ns[idx];
        }
        if (!ok) {
            return 0;
        }
    }
    long long bits_bitstream = pos;

    /* estado final - L, seguido do stop bit '1' */
    if (!tans_escrever_bits(out, &pos, limite, idx, m->flush_bits) ||
        !tans_escrever_bits(out, &pos, limite, 1u, 1)) {
        return 0;
    }

    size_t bytes = (size_t)((pos + 7) / 8);
    if (st) {
        st->bits_bitstream = bits_bitstream;
        st->bits_estado = m->flush_bits + 1;
        st->bits_preenchimento = (int)(bytes * 8 - pos);
        st->payload_bytes = bytes;
    }
    return bytes;
}

static inline int tans_write_file(const char *path, uint32_t n, int plano, const uint8_t *payload,
                                  size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    uint8_t id = (uint8_t)(plano | (MODELOS[plano].assinatura << 3));
    fwrite(&n, 4, 1, f);
    fwrite(&id, 1, 1, f);
    fwrite(payload, 1, bytes, f);
    fclose(f);
    return 0;
}

/* ================================================================ decoder */

typedef struct {
    uint8_t sym;
    uint16_t base;
} TansDecEntry;

/* Inverte a tabela do encoder: a origem L+idx emitiu nb bits (seus nb LSBs) e
   ficou reduzida a base = (L+idx) >> nb. Chegar em 'dest' significa: símbolo s
   e estado anterior = base seguido dos próximos bits, lidos ENQUANTO x < L
   (renormalização do rANS). Para alguns modelos (ex.: L=6, 1/6) origens com o
   mesmo base emitem quantidades diferentes de bits; ler até x >= L resolve. */
static inline int tans_build_decode(const ModeloPlano *m, TansDecEntry *dt)
{
    static uint8_t visto[4096];
    memset(visto, 0, sizeof(visto));

    for (uint32_t idx = 0; idx < m->L; idx++) {
        for (int s = 0; s < 2; s++) {
            uint32_t dest = s ? m->b_ns[idx] : m->a_ns[idx];
            uint8_t nb = s ? m->b_nb[idx] : m->a_nb[idx];
            TansDecEntry e = {(uint8_t)s, (uint16_t)((m->L + idx) >> nb)};

            if (visto[dest] && (dt[dest].sym != e.sym || dt[dest].base != e.base)) {
                return -1;
            }
            dt[dest] = e;
            visto[dest] = 1;
        }
    }

    for (uint32_t i = 0; i < m->L; i++) {
        if (!visto[i]) {
            return -1;
        }
    }
    return 0;
}

/* Lê o bit da posição 'pos' (0 = MSB do primeiro byte). Fora do buffer vale 0. */
static inline uint32_t tans_ler_bit(const uint8_t *buf, long long pos)
{
    if (pos < 0) {
        return 0u;
    }
    return (uint32_t)((buf[pos >> 3] >> (7 - (pos & 7))) & 1);
}

/* Decodifica n símbolos do payload (lido de trás para frente). 0 = ok. */
static inline int tans_decode(const uint8_t *buf, size_t bytes, size_t n, const ModeloPlano *m,
                              uint8_t *sym)
{
    static TansDecEntry dt[4096];
    if (tans_build_decode(m, dt) != 0) {
        fprintf(stderr, "ERRO: tabela não inversível.\n");
        return 1;
    }

    /* Procura o stop bit (o último '1' do arquivo) */
    long long pos = (long long)bytes * 8 - 1;
    while (pos >= 0 && tans_ler_bit(buf, pos) == 0) {
        pos--;
    }
    if (pos < 0) {
        fprintf(stderr, "ERRO: stop bit não encontrado.\n");
        return 1;
    }
    pos--;

    /* Estado final - L (gravado com flush_bits bits) */
    uint32_t idx = 0;
    for (int i = 0; i < m->flush_bits; i++) {
        idx |= tans_ler_bit(buf, pos) << i;
        pos--;
    }
    if (idx >= m->L) {
        fprintf(stderr, "ERRO: estado final inválido.\n");
        return 1;
    }

    /* Símbolos do último para o primeiro (ANS é LIFO) */
    for (long long i = (long long)n - 1; i >= 0; i--) {
        const TansDecEntry *e = &dt[idx];
        uint32_t x = e->base;

        while (x < m->L && pos >= 0) {
            x = (x << 1) | tans_ler_bit(buf, pos);
            pos--;
        }
        if (x < m->L) {
            fprintf(stderr, "ERRO: bitstream acabou antes do fim.\n");
            return 1;
        }

        sym[i] = e->sym;
        idx = x - m->L;
    }

    if (idx != 0 || pos != -1) {
        fprintf(stderr, "ERRO: fim inconsistente (estado %u, bits restantes %lld).\n", idx,
                pos + 1);
        return 1;
    }
    return 0;
}

/* ================================================================ sweep */

/* Medição RÁPIDA do custo de um modelo (L, M, f0) para o sweep_tabelas.py.
   Faz o mesmo que custo_bits() de modelo_estatico.py, só que em C:
     1. monta a tabela tANS do modelo pelo rANS streaming (b = 2):
        renormaliza x enquanto x >= 2k·F(s) (k = L/M), depois
        equação (7): x' = (x / F(s))·M + C(s) + (x mod F(s));
     2. percorre os símbolos do plano somando os bits emitidos;
     3. soma cabeçalho + estado final + stop bit + preenchimento.
   Só serve para ESCOLHER o modelo; as tabelas do codec continuam vindo do rANS
   em Python. Só é compilada quando o sweep gera a biblioteca:
     gcc -O3 -shared -fPIC -x c -DTANS_SWEEP tans_codec.h -o sweep_custo.so */
#ifdef TANS_SWEEP

#define L_MAX 4096

long long tans_custo(const uint8_t *sym, long long n, int L, int M, int f0, int header_bytes)
{
    if (L < 2 || L > L_MAX || M < 2 || f0 < 1 || f0 >= M || L % M) {
        return -1;
    }
    /* tab[2*idx + s] = (novo idx) | (nbits << 16) */
    static __thread uint32_t tab[2 * L_MAX];
    int k = L / M;
    for (int s = 0; s < 2; s++) {
        uint32_t fs = s ? (uint32_t)(M - f0) : (uint32_t)f0;
        uint32_t cs = s ? (uint32_t)f0 : 0u;
        uint32_t lim = 2u * (uint32_t)k * fs;
        for (int idx = 0; idx < L; idx++) {
            uint32_t x = (uint32_t)(L + idx), nb = 0;
            while (x >= lim) {
                x >>= 1;
                nb++;
            }
            uint32_t novo = (x / fs) * (uint32_t)M + cs + (x % fs);
            tab[2 * idx + s] = (novo - (uint32_t)L) | (nb << 16);
        }
    }
    long long bits = 0;
    uint32_t st = 0;
    for (long long i = 0; i < n; i++) {
        uint32_t e = tab[2 * st + sym[i]];
        bits += e >> 16;
        st = e & 0xFFFF;
    }
    int flush = 0; /* ceil(log2 L) = bit_length(L - 1) */
    for (int v = L - 1; v; v >>= 1) {
        flush++;
    }
    long long payload = bits + flush + 1;
    return 8 * (header_bytes + (payload + 7) / 8);
}

#endif /* TANS_SWEEP */

#endif /* TANS_CODEC_H */
