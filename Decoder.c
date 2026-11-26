/*
 tANS ESTÁTICO AGNÓSTICO - DECODER (uma tabela fixa por plano de bit)

 O plano vem no cabeçalho; o decoder usa a MESMA tabela fixa do encoder
 (MODELOS[plano] de tans_tables.h) e confere a assinatura do modelo.
 A decodificação em si está em tans_codec.h.

 Uso: ./decoder <arquivo_encoded.bin> <arquivo_decoded.txt>
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tans_codec.h"

#define MAX_DECODED_SYMBOLS 12000000

static uint8_t bitstream_buffer[2 * MAX_DECODED_SYMBOLS + 16];
static uint8_t decoded_symbols[MAX_DECODED_SYMBOLS];

static int decode_file(const char *input_file, const char *output_file)
{
    FILE *f = fopen(input_file, "rb");
    if (!f) {
        perror("fopen entrada");
        return 1;
    }
    uint32_t num_symbols = 0;
    uint8_t id = 0;
    if (fread(&num_symbols, 4, 1, f) != 1 || fread(&id, 1, 1, f) != 1) {
        fprintf(stderr, "ERRO: cabeçalho inválido ou arquivo truncado.\n");
        fclose(f);
        return 1;
    }
    size_t payload = fread(bitstream_buffer, 1, sizeof(bitstream_buffer), f);
    int sobrou = fgetc(f) != EOF;
    fclose(f);

    int plano = id & 7, assin = id >> 3;
    if (num_symbols == 0 || num_symbols > MAX_DECODED_SYMBOLS || sobrou || payload == 0) {
        fprintf(stderr, "ERRO: arquivo inválido (%u símbolos, %zu bytes).\n", num_symbols, payload);
        return 1;
    }
    const ModeloPlano *m = &MODELOS[plano];
    if (assin != m->assinatura) {
        fprintf(stderr,
                "ERRO: o plano %d foi codificado com outro modelo (assinatura %d, "
                "esperado %d). Regenere tans_tables.h e recompile.\n",
                plano, assin, m->assinatura);
        return 1;
    }
    if (tans_decode(bitstream_buffer, payload, num_symbols, m, decoded_symbols) != 0) {
        return 1;
    }

    FILE *out = fopen(output_file, "w");
    if (!out) {
        perror("fopen saida");
        return 1;
    }
    for (uint32_t i = 0; i < num_symbols; i++) {
        fputc('0' + decoded_symbols[i], out);
        if (i + 1 < num_symbols) {
            fputc(' ', out);
        }
    }
    fclose(out);
    printf("Arquivo decodificado com sucesso: %s (%u simbolos, plano %d, "
           "tabela fixa L=%u %u/%u)\n",
           output_file, num_symbols, plano, m->L, m->f0, m->M);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <arquivo_encoded.bin> <arquivo_decoded.txt>\n", argv[0]);
        return 1;
    }
    return decode_file(argv[1], argv[2]);
}
