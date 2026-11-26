/*
 Encoder_Ans.c - tANS ESTÁTICO AGNÓSTICO: ENCODER (uma tabela fixa por plano de bit)

 O plano N usa SEMPRE a tabela MODELOS[N] de tans_tables.h (gerada pelo rANS a
 partir de modelos_planos.json, escolhidos por sweep_tabelas.py). Em tempo de
 execução não há varredura por bloco, escolha de tabela, contexto nem
 cabeçalho de frequências. Formato do .bin: ver tans_codec.h.

 Dois tipos de entrada:

   IMAGEM PNG  ./encoder Kodak/kodim19.png        (várias: relatório + resumo)
               ./encoder -r Kodak/kodim*.png      (-r: só o resumo)
     1. lê o PNG e converte para tons de cinza
     2. separa os 8 planos de bits
     3. codifica cada plano com a SUA tabela fixa
     4. grava comprimidos/<imagem>_plano_N.bin (abre com ./decoder)
     5. decodifica de volta e confere bit a bit
     6. imprime o relatório (uma coluna por plano + TOTAL)

   PLANO .TXT  ./encoder plano_bit_7.txt          (plano deduzido do nome)
               ./encoder -p 7 qualquer_nome.txt   (plano informado)
     grava <nome>_encoded.bin ao lado do .txt

 Compilar:  gcc -O2 Encoder_Ans.c -o encoder -lm -lz
*/

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "tans_codec.h"

/* ======================================================================= */
/*
 LEITOR DE PNG (mínimo; só depende da zlib: compile com -lz).

 Suporta PNG de 8 bits por canal, sem entrelaçamento: cinza, cinza+alfa,
 RGB, RGBA e paleta (Kodak e DIV2K são RGB 8 bits). Devolve a imagem já em
 tons de cinza com a fórmula BT.601 em inteiros (idêntica ao
 cv2.cvtColor(..., COLOR_BGR2GRAY) e ao modelo_estatico.ler_imagem_cinza):
     Y = (9798·R + 19235·G + 3735·B + 16384) >> 15
 Metadados de gama (gAMA/sRGB) são ignorados: usa-se o valor gravado no arquivo.
*/

static inline uint8_t png_cinza(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint8_t)((9798u * r + 19235u * g + 3735u * b + 16384u) >> 15);
}

static inline uint32_t png_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline int png_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

/* Lê 'path' e devolve (malloc) w*h pixels em cinza; NULL em caso de erro. */
static inline uint8_t *png_ler_cinza(const char *path, uint32_t *w_out, uint32_t *h_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *arq = malloc((size_t)tam);
    if (!arq || fread(arq, 1, (size_t)tam, f) != (size_t)tam) {
        fclose(f);
        free(arq);
        fprintf(stderr, "ERRO: não foi possível ler '%s'.\n", path);
        return NULL;
    }
    fclose(f);

    static const uint8_t assinatura[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (tam < 8 || memcmp(arq, assinatura, 8) != 0) {
        fprintf(stderr, "ERRO: '%s' não é um PNG.\n", path);
        free(arq);
        return NULL;
    }

    uint32_t w = 0, h = 0;
    int prof = 0, tipo = -1, entrelacado = 0;
    uint8_t paleta[256][3] = {{0}};
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    long p = 8;
    while (p + 12 <= tam) {
        uint32_t len = png_be32(arq + p);
        const uint8_t *t = arq + p + 4, *d = arq + p + 8;
        if (p + 12 + (long)len > tam) {
            break;
        }
        if (!memcmp(t, "IHDR", 4)) {
            w = png_be32(d);
            h = png_be32(d + 4);
            prof = d[8];
            tipo = d[9];
            entrelacado = d[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            for (uint32_t i = 0; i < len / 3 && i < 256; i++) {
                memcpy(paleta[i], d + 3 * i, 3);
            }
        } else if (!memcmp(t, "IDAT", 4)) {
            uint8_t *novo = realloc(idat, idat_len + len);
            if (!novo) {
                break;
            }
            idat = novo;
            memcpy(idat + idat_len, d, len);
            idat_len += len;
        } else if (!memcmp(t, "IEND", 4)) {
            break;
        }
        p += 12 + (long)len;
    }
    free(arq);

    /* Canais por pixel de cada tipo de PNG (0 = tipo não suportado) */
    int canais;
    switch (tipo) {
        case 0: /* cinza */
            canais = 1;
            break;
        case 2: /* RGB */
            canais = 3;
            break;
        case 3: /* paleta */
            canais = 1;
            break;
        case 4: /* cinza + alfa */
            canais = 2;
            break;
        case 6: /* RGBA */
            canais = 4;
            break;
        default:
            canais = 0;
            break;
    }
    if (!idat || !w || !h || prof != 8 || !canais || entrelacado) {
        fprintf(stderr,
                "ERRO: '%s' não suportado (precisa PNG 8 bits/canal, sem "
                "entrelaçamento; lido: prof=%d tipo=%d entrel=%d).\n",
                path, prof, tipo, entrelacado);
        free(idat);
        return NULL;
    }

    size_t linha = (size_t)w * canais;
    uLongf cru_len = (uLongf)((linha + 1) * h);
    uint8_t *cru = malloc(cru_len);
    if (!cru || uncompress(cru, &cru_len, idat, (uLong)idat_len) != Z_OK ||
        cru_len != (linha + 1) * h) {
        fprintf(stderr, "ERRO: dados zlib inválidos em '%s'.\n", path);
        free(idat);
        free(cru);
        return NULL;
    }
    free(idat);

    /* Desfaz os filtros de cada linha (in-place), byte a byte */
    uint8_t *ant = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t filtro = cru[y * (linha + 1)];
        uint8_t *lin = cru + y * (linha + 1) + 1;
        for (size_t x = 0; x < linha; x++) {
            int a = x >= (size_t)canais ? lin[x - canais] : 0;
            int b = ant ? ant[x] : 0;
            int c = (ant && x >= (size_t)canais) ? ant[x - canais] : 0;
            int v = lin[x];
            switch (filtro) {
                case 0: /* None */
                    break;
                case 1: /* Sub: soma o byte da esquerda */
                    v += a;
                    break;
                case 2: /* Up: soma o byte de cima */
                    v += b;
                    break;
                case 3: /* Average: soma a média de esquerda e cima */
                    v += (a + b) / 2;
                    break;
                case 4: /* Paeth */
                    v += png_paeth(a, b, c);
                    break;
                default:
                    fprintf(stderr, "ERRO: filtro PNG inválido (%d).\n", filtro);
                    free(cru);
                    return NULL;
            }
            lin[x] = (uint8_t)v;
        }
        ant = lin;
    }

    uint8_t *cinza = malloc((size_t)w * h);
    if (!cinza) {
        free(cru);
        return NULL;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *lin = cru + y * (linha + 1) + 1;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *px = lin + (size_t)x * canais;
            uint8_t v;
            if (tipo == 0 || tipo == 4) {
                v = px[0];
            } else if (tipo == 3) {
                v = png_cinza(paleta[px[0]][0], paleta[px[0]][1], paleta[px[0]][2]);
            } else {
                v = png_cinza(px[0], px[1], px[2]);
            }
            cinza[(size_t)y * w + x] = v;
        }
    }
    free(cru);
    *w_out = w;
    *h_out = h;
    return cinza;
}

/* ======================================================================= */
/* ENTRADA PNG: a imagem inteira (8 planos)                                 */
/* ======================================================================= */

#define PASTA_SAIDA "comprimidos"

typedef struct {
    long long orig, overhead, pad, estado, bitstream, comp;
    double t_enc, t_dec;
    int ok;
} Linha;

static double agora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* 1234567 -> "1.234.567" (sinal preservado) */
static const char *milhar(long long v)
{
    static char buf[16][32];
    static int k = 0;
    char *out = buf[k++ & 15], tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v < 0 ? -v : v);
    int len = (int)strlen(tmp), j = 0;
    if (v < 0) {
        out[j++] = '-';
    }
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) {
            out[j++] = '.';
        }
        out[j++] = tmp[i];
    }
    out[j] = 0;
    return out;
}

static void nome_base(const char *path, char *out, size_t cap)
{
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    snprintf(out, cap, "%s", s);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = 0;
    }
}

static void imprimir_relatorio(const char *img, uint32_t w, uint32_t h, const Linha *L)
{
    const int W0 = 24, WC = 13, larg = W0 + WC * (NUM_PLANOS + 1);
    char sep[256], igual[256];
    memset(sep, '-', larg);
    sep[larg] = 0;
    memset(igual, '=', larg);
    igual[larg] = 0;

    printf("\n%s\n", igual);
    printf("  tANS ESTATICO AGNOSTICO (TABELA FIXA POR PLANO)  -  %s  (%ux%u)\n", img, w, h);
    printf("%s\n%*s", igual, W0, "");
    for (int b = 0; b < NUM_PLANOS; b++) {
        printf("%*s%d", WC - 1, "PLANO ", b);
    }
    printf("%*s\n%s\n", WC, "TOTAL", sep);

/* Imprime uma linha do relatório: o rótulo e o campo 'campo' de cada plano + TOTAL */
#define LINHA_INT(rotulo, campo)                                                                   \
    do {                                                                                           \
        printf("%-*s", W0, rotulo);                                                                \
        for (int b = 0; b <= NUM_PLANOS; b++) {                                                    \
            printf("%*s", WC, milhar(L[b].campo));                                                 \
        }                                                                                          \
        printf("\n");                                                                              \
    } while (0)

    LINHA_INT("TAMANHO ORIGINAL (BITS)", orig);
    printf("%s\n%-*s", sep, W0, "TABELA FIXA (L f0/M)");
    for (int b = 0; b < NUM_PLANOS; b++) {
        char t[32];
        snprintf(t, sizeof t, "L%u %u/%u", MODELOS[b].L, MODELOS[b].f0, MODELOS[b].M);
        printf("%*s", WC, t);
    }
    printf("%*s\n", WC, "-");
    LINHA_INT("OVERHEAD (BITS)", overhead);
    LINHA_INT("BITS DE PREENCHIMENTO", pad);
    LINHA_INT("ESTADO (BITS)", estado);
    LINHA_INT("BITSTREAM", bitstream);
    LINHA_INT("COMPRIMIDO GLOBAL", comp);
    printf("%s\n%-*s", sep, W0, "GANHO TOTAL (BITS)");
    for (int b = 0; b <= NUM_PLANOS; b++) {
        printf("%*s", WC, milhar(L[b].orig - L[b].comp));
    }
    printf("\n%-*s", W0, "TAXA DE COMPRESSAO");
    for (int b = 0; b <= NUM_PLANOS; b++) {
        char t[32];
        snprintf(t, sizeof t, "%.3f%%", 100.0 * (1.0 - (double)L[b].comp / L[b].orig));
        for (char *c = t; *c; c++) {
            if (*c == '.') {
                *c = ',';
            }
        }
        printf("%*s", WC, t);
    }
    printf("\n%s\n%-*s", sep, W0, "TEMPO ENCODER (ms)");
    for (int b = 0; b <= NUM_PLANOS; b++) {
        printf("%*.1f", WC, L[b].t_enc);
    }
    printf("\n%-*s", W0, "TEMPO DECODER (ms)");
    for (int b = 0; b <= NUM_PLANOS; b++) {
        printf("%*.1f", WC, L[b].t_dec);
    }
    printf("\n%-*s", W0, "VERIFICACAO LOSSLESS");
    for (int b = 0; b <= NUM_PLANOS; b++) {
        printf("%*s", WC, L[b].ok ? "OK" : "ERRO");
    }
    printf("\n%s\n", igual);
#undef LINHA_INT
}

/* Processa uma imagem. Preenche 'total' (coluna TOTAL). 0 = ok. */
static int processar_imagem(const char *path, int so_resumo, Linha *total)
{
    uint32_t w, h;
    uint8_t *cinza = png_ler_cinza(path, &w, &h);
    if (!cinza) {
        return 1;
    }
    size_t n = (size_t)w * h;
    size_t cap = tans_capacidade(n);
    uint8_t *sym = malloc(n), *dec = malloc(n), *payload = malloc(cap);
    if (!sym || !dec || !payload) {
        fprintf(stderr, "ERRO: memória insuficiente.\n");
        free(cinza);
        free(sym);
        free(dec);
        free(payload);
        return 1;
    }

    char base[256];
    nome_base(path, base, sizeof base);
    Linha L[NUM_PLANOS + 1];
    memset(L, 0, sizeof L);
    int tudo_ok = 1;

    for (int b = 0; b < NUM_PLANOS; b++) {
        for (size_t i = 0; i < n; i++) {
            sym[i] = (cinza[i] >> b) & 1;
        }
        const ModeloPlano *m = &MODELOS[b];
        TansStats st;

        double t0 = agora_ms();
        size_t bytes = tans_encode(sym, n, m, payload, cap, &st);
        double t1 = agora_ms();
        if (!bytes) {
            fprintf(stderr, "ERRO: bitstream excede a capacidade (plano %d).\n", b);
            tudo_ok = 0;
            continue;
        }

        char arq[512];
        snprintf(arq, sizeof arq, PASTA_SAIDA "/%s_plano_%d.bin", base, b);
        tans_write_file(arq, (uint32_t)n, b, payload, bytes);

        double t2 = agora_ms();
        int ok = tans_decode(payload, bytes, n, m, dec) == 0 && memcmp(dec, sym, n) == 0;
        double t3 = agora_ms();
        tudo_ok &= ok;

        L[b] = (Linha){(long long)n,   HEADER_BYTES * 8,  st.bits_preenchimento,
                       st.bits_estado, st.bits_bitstream, (long long)(HEADER_BYTES + bytes) * 8,
                       t1 - t0,        t3 - t2,           ok};
    }
    L[NUM_PLANOS].ok = tudo_ok;
    for (int b = 0; b < NUM_PLANOS; b++) {
        L[NUM_PLANOS].orig += L[b].orig;
        L[NUM_PLANOS].overhead += L[b].overhead;
        L[NUM_PLANOS].pad += L[b].pad;
        L[NUM_PLANOS].estado += L[b].estado;
        L[NUM_PLANOS].bitstream += L[b].bitstream;
        L[NUM_PLANOS].comp += L[b].comp;
        L[NUM_PLANOS].t_enc += L[b].t_enc;
        L[NUM_PLANOS].t_dec += L[b].t_dec;
    }

    if (!so_resumo) {
        imprimir_relatorio(path, w, h, L);
    }
    *total = L[NUM_PLANOS];
    free(cinza);
    free(sym);
    free(dec);
    free(payload);
    return tudo_ok ? 0 : 1;
}

/* ======================================================================= */
/* ENTRADA .TXT: um plano já separado (0/1)                                 */
/* ======================================================================= */

#define MAX_SYMBOLS 12000000 /* até ~12 Mpixels por plano (DIV2K cabe) */

static uint8_t symbols_buffer[MAX_SYMBOLS];
static uint8_t bitstream_buffer[2 * MAX_SYMBOLS + 16];

static int plano_do_nome(const char *filename)
{
    const char *p = strstr(filename, "plano_bit_");
    if (p && p[10] >= '0' && p[10] < '0' + NUM_PLANOS) {
        return p[10] - '0';
    }
    return -1;
}

static int process_file(const char *filename, int plano)
{
    if (plano < 0) {
        plano = plano_do_nome(filename);
    }
    if (plano < 0 || plano >= NUM_PLANOS) {
        fprintf(stderr, "ERRO: plano desconhecido para '%s' (use -p 0..%d).\n", filename,
                NUM_PLANOS - 1);
        return 1;
    }
    const ModeloPlano *m = &MODELOS[plano];

    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("fopen entrada");
        return 1;
    }
    /* Lê 0/1 separados por espaço/linha OU contínuos ("0101..."). */
    int symbol_count = 0, count0 = 0, ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch != '0' && ch != '1') {
            continue;
        }
        if (symbol_count >= MAX_SYMBOLS) {
            fprintf(stderr, "ERRO: muitos símbolos (limite %d)\n", MAX_SYMBOLS);
            fclose(file);
            return 1;
        }
        symbols_buffer[symbol_count++] = (uint8_t)(ch - '0');
        count0 += (ch == '0');
    }
    fclose(file);
    if (symbol_count == 0) {
        fprintf(stderr, "AVISO: arquivo vazio.\n");
        return 1;
    }

    /* Codificação tANS com a tabela fixa do plano — uma única passada. */
    TansStats st;
    size_t payload = tans_encode(symbols_buffer, (size_t)symbol_count, m, bitstream_buffer,
                                 sizeof(bitstream_buffer), &st);
    if (!payload) {
        fprintf(stderr, "ERRO: bitstream excede a capacidade.\n");
        return 1;
    }
    long long bits_bitstream = st.bits_bitstream;

    char output_filename[512];
    const char *slash = strrchr(filename, '/');
    const char *dot = strrchr(filename, '.');
    int base_len = (dot && (!slash || dot > slash)) ? (int)(dot - filename) : (int)strlen(filename);
    if (base_len > 490) {
        base_len = 490;
    }
    memcpy(output_filename, filename, (size_t)base_len);
    strcpy(output_filename + base_len, "_encoded.bin");

    if (tans_write_file(output_filename, (uint32_t)symbol_count, plano, bitstream_buffer,
                        payload)) {
        return 1;
    }

    /* Relatório */
    size_t total_bytes = HEADER_BYTES + payload;
    int count1 = symbol_count - count0;
    double p0 = (double)count0 / symbol_count, p1 = 1.0 - p0;
    double h = 0.0;
    if (p0 > 0.0) {
        h -= p0 * log2(p0);
    }
    if (p1 > 0.0) {
        h -= p1 * log2(p1);
    }
    double taxa = (1.0 - (double)(total_bytes * 8) / symbol_count) * 100.0;

    printf("\n========================================================================\n");
    printf("  tANS ESTATICO AGNOSTICO  |  plano %d  |  L=%u  P(0)=%u/%u (fixo)\n", plano, m->L,
           m->f0, m->M);
    printf("========================================================================\n");
    printf("Arquivo           : %s\n", filename);
    printf("Símbolos lidos    : %d (0s: %d, 1s: %d)\n", symbol_count, count0, count1);
    printf("Modo selecionado  : tANS (tabela fixa L=%u %u/%u)\n", m->L, m->f0, m->M);
    printf("Entropia H (ord0) : %.6f bits/símbolo\n", h);
    printf("Bits/símbolo real : %.6f\n", (double)bits_bitstream / symbol_count);
    printf("Total transmitido : %zu bytes  (%zu bits)\n", total_bytes, total_bytes * 8);
    printf("Taxa de compressão: %.3f%%\n", taxa);
    printf("Overhead (bits)   : %d\n", HEADER_BYTES * 8);
    printf("Preenchimento (bits): %d\n", st.bits_preenchimento);
    printf("Estado (bits)     : %d\n", m->flush_bits + 1);
    printf("Bitstream (bits)  : %lld\n", bits_bitstream);
    printf("Comprimido (bits) : %zu\n", total_bytes * 8);
    printf("Arquivo gerado    : %s\n", output_filename);
    printf("========================================================================\n");
    return 0;
}

/* ======================================================================= */
/* main                                                                     */
/* ======================================================================= */

static int eh_png(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && (strcmp(path + n - 4, ".png") == 0 || strcmp(path + n - 4, ".PNG") == 0);
}

static void imprimir_resumo(char **imagens, const Linha *res, int nimg)
{
    long long to = 0, tc = 0;
    printf("\n%-40s %14s %14s %12s %9s %6s\n", "IMAGEM", "ORIGINAL", "COMPRIMIDO", "GANHO", "TAXA",
           "OK");
    printf("--------------------------------------------------------------------------------------"
           "------------\n");
    for (int i = 0; i < nimg; i++) {
        if (!res[i].orig) {
            printf("%-40s %14s\n", imagens[i], "ERRO DE LEITURA");
            continue;
        }
        to += res[i].orig;
        tc += res[i].comp;
        printf("%-40s %14s %14s %12s %8.3f%% %6s\n", imagens[i], milhar(res[i].orig),
               milhar(res[i].comp), milhar(res[i].orig - res[i].comp),
               100.0 * (1.0 - (double)res[i].comp / res[i].orig), res[i].ok ? "OK" : "ERRO");
    }
    printf("--------------------------------------------------------------------------------------"
           "------------\n");
    if (to) {
        printf("%-40s %14s %14s %12s %8.3f%%\n", "TOTAL", milhar(to), milhar(tc), milhar(to - tc),
               100.0 * (1.0 - (double)tc / to));
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Uso: %s [-r] imagem.png [...]        comprime imagens (os 8 planos)\n"
                "     %s [-p N] plano_bit_N.txt [...] comprime um plano em texto\n"
                "  -r    só o resumo (uma linha por imagem)\n"
                "  -p N  plano dos .txt seguintes (sem -p, deduzido de plano_bit_N)\n",
                argv[0], argv[0]);
        return 1;
    }

    int so_resumo = 0, plano = -1, erros = 0, nimg = 0;
    char **imagens = calloc((size_t)argc, sizeof(char *));
    Linha *res = calloc((size_t)argc, sizeof(Linha));
    if (!imagens || !res) {
        fprintf(stderr, "ERRO: memória insuficiente.\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            so_resumo = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            plano = atoi(argv[++i]);
        } else if (eh_png(argv[i])) {
            if (nimg == 0 && mkdir(PASTA_SAIDA, 0755) != 0 && errno != EEXIST) {
                perror(PASTA_SAIDA);
                return 1;
            }
            imagens[nimg] = argv[i];
            erros += processar_imagem(argv[i], so_resumo, &res[nimg]);
            nimg++;
        } else {
            erros += process_file(argv[i], plano);
        }
    }

    if (nimg > 1 || (nimg > 0 && so_resumo)) {
        imprimir_resumo(imagens, res, nimg);
    }
    if (nimg > 0) {
        printf("\nArquivos comprimidos em %s/ (abrir com ./decoder)\n", PASTA_SAIDA);
    }
    free(imagens);
    free(res);
    return erros ? 1 : 0;
}
