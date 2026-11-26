import argparse
import math
import os
import re
import time

from modelo_estatico import (
    HEADER_BYTES,
    NUM_PLANOS,
    carregar_modelos,
    encode_all,
    load_symbols,
    tabela_tans,
)

# ---------------------------------------------------------------------------
# Geração de tans_tables.h: UMA tabela fixa por plano de bit
#
# Cada entrada é o encoder rANS streaming (_encode_symbol, equação (7) do
# tutorial LASCAS 2025) aplicado a um estado x em [L, 2L-1] e a um símbolo s,
# com o modelo (L, M, f0) fixado para aquele plano em modelos_planos.json.
# O encoder em C não escolhe nada: o plano N usa sempre a tabela N.
# ---------------------------------------------------------------------------


def _array(tipo, nome, vals, por_linha=16):
    linhas = [
        "    " + ", ".join(vals[i:i + por_linha]) + ","
        for i in range(0, len(vals), por_linha)
    ]
    return f"static const {tipo} {nome}[{len(vals)}] = {{\n" + "\n".join(linhas) + "\n};\n"


def gerar(modelos):
    h = [
        "/* =============================================================================\n"
        " * tans_tables.h  -  tANS ESTÁTICO AGNÓSTICO: uma tabela FIXA por plano de bit\n"
        " *\n"
        " * ARQUIVO GERADO por ANS-Streaming Encoder.py (--gerar-tabelas) - não editar à mão.\n"
        " * Modelos escolhidos offline por sweep_tabelas.py (modelos_planos.json).\n"
        " *\n"
        " * O plano N usa SEMPRE o modelo N, do primeiro ao último símbolo, para qualquer\n"
        " * imagem: não há varredura por bloco nem escolha de tabela em tempo de execução.\n"
        " *\n",
    ]
    for p, m in enumerate(modelos):
        h.append(f" *   plano {p}: L = {m.L:<5} M = {m.M:<5} f0 = {m.f0:<5} -> P(0) = {m.f0 / m.M:.4f}\n")
    h.append(
        " *\n"
        " * Cada entrada = rANS streaming (b = 2) com estados em [L, 2L-1], k = L/M:\n"
        " *   1. Renormalização: enquanto x >= 2k·F(s), emite x & 1 e faz x >>= 1\n"
        " *      (valor guardado com o 1º bit emitido como MSB).\n"
        " *   2. Equação (7): novo estado = (x / F(s))·M + C(s) + (x mod F(s)).\n"
        " *   *_ns: novo estado - L    *_bits: bits emitidos    *_nb: quantidade de bits\n"
        " * ============================================================================= */\n\n"
        "#ifndef TANS_TABLES_H\n#define TANS_TABLES_H\n\n#include <stdint.h>\n\n"
        f"#define NUM_PLANOS {NUM_PLANOS}\n"
        f"#define HEADER_BYTES {HEADER_BYTES} /* [num_symbols 4B] [plano 3b | assinatura 5b] */\n\n"
        "typedef struct {\n"
        "    uint16_t L, M, f0;     /* estados [L, 2L-1], P(0) = f0/M */\n"
        "    uint8_t flush_bits;    /* ceil(log2 L) */\n"
        "    uint8_t assinatura;    /* 5 bits gravados no cabeçalho */\n"
        "    const uint16_t *a_ns, *a_bits, *b_ns, *b_bits;\n"
        "    const uint8_t *a_nb, *b_nb;\n"
        "} ModeloPlano;\n\n"
    )
    for p, m in enumerate(modelos):
        t = tabela_tans(m)
        h.append(f"/* ---- plano {p}: {m.descricao()} ---- */\n")
        for sym, nome in ((0, "a"), (1, "b")):
            h.append(_array("uint16_t", f"p{p}_{nome}_ns", [str(e[0]) for e in t[sym]]))
            h.append(_array("uint16_t", f"p{p}_{nome}_bits", [str(e[1]) for e in t[sym]]))
            h.append(_array("uint8_t", f"p{p}_{nome}_nb", [str(e[2]) for e in t[sym]], 32))
        h.append("\n")
    h.append("static const ModeloPlano MODELOS[NUM_PLANOS] = {\n")
    for p, m in enumerate(modelos):
        h.append(
            f"    {{{m.L}, {m.M}, {m.f0}, {m.flush_bits}, {m.assinatura()}, "
            f"p{p}_a_ns, p{p}_a_bits, p{p}_b_ns, p{p}_b_bits, p{p}_a_nb, p{p}_b_nb}},\n"
        )
    h.append("};\n\n#endif /* TANS_TABLES_H */\n")
    return "".join(h)


def plano_do_nome(path):
    m = re.search(r"plano_bit_(\d)", os.path.basename(path))
    return int(m.group(1)) if m else None


def save_encoded(path, plano, num_symbols, bitstream, final_state):
    """Texto: plano / num_symbols / bitstream ('0101...') / estado final."""
    with open(path, "w") as f:
        f.write(f"{plano}\n{num_symbols}\n{bitstream}\n{final_state}\n")


def main():
    parser = argparse.ArgumentParser(description="ANS Streaming Encoder - Estático Agnóstico por plano")
    parser.add_argument("--input", default="input.txt", help="Arquivo de entrada")
    parser.add_argument("--output", default="input_encoded.bin", help="Arquivo de saída")
    parser.add_argument("--plano", type=int, help="Plano de bit (0-7); padrão: deduz de plano_bit_N")
    parser.add_argument("--gerar-tabelas", action="store_true",
                        help="Gera tans_tables.h a partir de modelos_planos.json e sai")
    parser.add_argument("--saida", default="tans_tables.h", help="Arquivo das tabelas geradas")
    parser.add_argument("--modelos", default=None,
                        help="JSON dos modelos (padrão: modelos_planos.json); ex.: "
                             "comprimidos/<imagem>_modelos.json")
    args = parser.parse_args()

    modelos = carregar_modelos(args.modelos) if args.modelos else carregar_modelos()

    if args.gerar_tabelas:
        with open(args.saida, "w", encoding="utf-8") as f:
            f.write(gerar(modelos))
        print(f"{args.saida} gerado com {NUM_PLANOS} tabelas fixas (uma por plano):")
        for p, m in enumerate(modelos):
            print(f"  plano {p}: {m.descricao()}")
        return

    if not os.path.exists(args.input):
        print(f"Erro: arquivo '{args.input}' não encontrado.")
        return
    plano = args.plano if args.plano is not None else plano_do_nome(args.input)
    if plano is None or not 0 <= plano < NUM_PLANOS:
        print("Erro: informe --plano 0..7 (ou use um arquivo plano_bit_N.txt).")
        return
    m = modelos[plano]

    print("=" * 72)
    print(f"  ANS STREAMING ENCODER  |  plano {plano}  |  {m.descricao()} (fixo)")
    print("=" * 72)

    symbols = load_symbols(args.input)
    n = len(symbols)
    if n == 0:
        print("Erro: arquivo sem símbolos.")
        return

    t0 = time.time()
    bits, final_state = encode_all(symbols, m)
    dt = time.time() - t0

    total = 8 * HEADER_BYTES + len(bits) + m.flush_bits + 1
    p0 = symbols.count(0) / n
    H = -(p0 * math.log2(p0) + (1 - p0) * math.log2(1 - p0)) if 0 < p0 < 1 else 0.0
    print(f"Símbolos lidos          : {n}  (P(0) real = {p0:.4f}, H = {H:.6f})")
    print(f"Bits no bitstream       : {len(bits)}")
    print(f"Estado final            : {m.flush_bits} bits + 1 stop bit")
    print(f"Total (com cabeçalho)   : {total} bits")
    print(f"Taxa de compressão      : {100 * (1 - total / n):.3f}%")
    print(f"Tempo de codificação    : {dt:.4f}s")

    save_encoded(args.output, plano, n, "".join(map(str, bits)), final_state)
    print(f"Arquivo salvo: '{args.output}'")


if __name__ == "__main__":
    main()
