"""
Bit_Planes.py - planos de bits + codec rANS de referência em Python.

    python3 Bit_Planes.py Kodak/kodim19.png     # separa os planos da imagem e codifica
    python3 Bit_Planes.py                       # usa os plano_bit_N.txt já existentes

1. Separa a imagem em 8 planos de bits (mesma conversão para cinza do C),
   grava plano_bit_N.txt e a figura resultado_planos_bits.png.
2. Codifica e decodifica cada plano com o rANS streaming em Python
   (modelo_estatico.py), usando o modelo fixo N de modelos_planos.json.
3. Imprime o relatório: o tamanho é o mesmo que o ./encoder em C produz.

O caminho rápido (tudo em C) é ./encoder imagem.png; este script serve para
ver os planos e conferir o C contra o rANS de referência.
"""

import os
import sys
import time

from modelo_estatico import (
    HEADER_BYTES,
    NUM_PLANOS,
    carregar_modelos,
    decode_all,
    encode_all,
    ler_imagem_cinza,
    load_symbols,
)


def separar_planos(image_path):
    """Grava plano_bit_N.txt (0/1 contínuos) e a figura com os 8 planos."""
    import numpy as np

    img = ler_imagem_cinza(image_path)
    if img is None:
        sys.exit(f"Erro ao carregar a imagem '{image_path}'.")

    planos = [((img >> b) & 1).astype(np.uint8) for b in range(NUM_PLANOS)]
    for b, plano in enumerate(planos):
        nome = f"plano_bit_{b}.txt"
        np.savetxt(nome, plano.flatten(), fmt="%d", delimiter="")
        print(f"Plano {b} salvo em: {nome}")

    try:
        import matplotlib

        matplotlib.use("Agg")  # só salva a figura, não abre janela
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib não encontrado: figura dos planos não gerada.")
        return

    plt.figure(figsize=(12, 8))
    for b, plano in enumerate(planos):
        plt.subplot(2, 4, b + 1)
        plt.imshow(plano * 255, cmap="gray")
        plt.title(f"Plano de Bit {b}")
        plt.axis("off")
    plt.tight_layout()
    plt.savefig("resultado_planos_bits.png", dpi=300)
    print("Figura dos planos salva em 'resultado_planos_bits.png'.")


def codificar_plano(symbols, m):
    """rANS em Python: codifica, decodifica e mede (mesmo formato do .bin em C)."""
    n = len(symbols)

    t0 = time.perf_counter()
    bitstream, final_state = encode_all(symbols, m)
    t_enc = time.perf_counter() - t0

    t0 = time.perf_counter()
    decoded = decode_all(bitstream, final_state, n, m)
    t_dec = time.perf_counter() - t0

    payload = len(bitstream) + m.flush_bits + 1  # bitstream + estado final + stop bit
    comp = 8 * (HEADER_BYTES + (payload + 7) // 8)
    return {
        "orig": n,
        "tabela": f"L{m.L} {m.f0}/{m.M}",
        "overhead": 8 * HEADER_BYTES,
        "pad": comp - 8 * HEADER_BYTES - payload,
        "estado": m.flush_bits + 1,
        "bitstream": len(bitstream),
        "comp": comp,
        "ganho": n - comp,
        "taxa": 100 * (1 - comp / n),
        "t_enc": t_enc * 1000,
        "t_dec": t_dec * 1000,
        "ok": "OK" if decoded == symbols else "ERRO",
    }


def imprimir_relatorio(dados, nome_img):
    somas = ("orig", "overhead", "pad", "estado", "bitstream", "comp", "ganho", "t_enc", "t_dec")
    tot = {k: sum(x[k] for x in dados) for k in somas}
    tot["taxa"] = 100 * (1 - tot["comp"] / tot["orig"])
    tot["tabela"] = "-"
    tot["ok"] = "OK" if all(x["ok"] == "OK" for x in dados) else "ERRO"
    dados = dados + [tot]

    def inteiro(k):
        return [f"{x[k]:,}".replace(",", ".") for x in dados]

    def pct(k):
        return [f"{x[k]:.3f}%".replace(".", ",") for x in dados]

    def ms(k):
        return [f"{x[k]:.1f}".replace(".", ",") for x in dados]

    linhas = [
        ("TAMANHO ORIGINAL (BITS)", inteiro("orig")),
        ("TABELA FIXA (L f0/M)", [x["tabela"] for x in dados]),
        ("OVERHEAD (BITS)", inteiro("overhead")),
        ("BITS DE PREENCHIMENTO", inteiro("pad")),
        ("ESTADO (BITS)", inteiro("estado")),
        ("BITSTREAM", inteiro("bitstream")),
        ("COMPRIMIDO GLOBAL", inteiro("comp")),
        ("GANHO TOTAL (BITS)", inteiro("ganho")),
        ("TAXA DE COMPRESSAO", pct("taxa")),
        ("TEMPO ENCODER (ms)", ms("t_enc")),
        ("TEMPO DECODER (ms)", ms("t_dec")),
        ("VERIFICACAO LOSSLESS", [x["ok"] for x in dados]),
    ]
    cab = [f"PLANO {i}" for i in range(NUM_PLANOS)] + ["TOTAL"]
    w0 = max(len(n) for n, _ in linhas) + 1
    wc = max(11, max(len(v) for _, vs in linhas for v in vs) + 1)
    larg = w0 + wc * len(cab)

    titulo = f"PLANOS DE BITS - rANS DE REFERENCIA (PYTHON, TABELA FIXA POR PLANO)  -  {nome_img}"
    print("\n" + "=" * larg)
    print(titulo.center(larg))
    print("=" * larg)
    print(" " * w0 + "".join(c.rjust(wc) for c in cab))
    print("-" * larg)
    for nome, vals in linhas:
        print(nome.ljust(w0) + "".join(v.rjust(wc) for v in vals))
        if nome in ("TAMANHO ORIGINAL (BITS)", "COMPRIMIDO GLOBAL", "TAXA DE COMPRESSAO"):
            print("-" * larg)
    print("=" * larg)


def main():
    if len(sys.argv) > 1:
        separar_planos(sys.argv[1])
        nome_img = sys.argv[1]
    else:
        nome_img = "plano_bit_N.txt"

    modelos = carregar_modelos()
    dados = []
    for b in range(NUM_PLANOS):
        arq = f"plano_bit_{b}.txt"
        if not os.path.exists(arq):
            sys.exit(f"'{arq}' não encontrado. Uso: python3 Bit_Planes.py imagem.png")
        dados.append(codificar_plano(load_symbols(arq), modelos[b]))
    imprimir_relatorio(dados, nome_img)


if __name__ == "__main__":
    main()
