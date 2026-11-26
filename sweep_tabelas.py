#!/usr/bin/env python3
"""
sweep_tabelas.py — tabelas e probabilidades IDEAIS para a imagem a comprimir.

Para cada plano de bit da imagem, testa milhares de tabelas (L) e
probabilidades (f0/M), mede o tamanho REAL do arquivo comprimido (cabeçalho +
bitstream + estado + stop bit + preenchimento, simulando exatamente a tabela
tANS do C) e fixa o melhor modelo de cada plano em modelos_planos.json.
O codec continua estático: dentro da imagem o plano N usa sempre o modelo N.

Uso:
    python3 sweep_tabelas.py --imagem Kodak/kodim12.png --comprimir
        sweep na imagem -> tabelas (rANS) -> recompila -> comprime e verifica
    python3 sweep_tabelas.py --imagem Kodak/kodim12.png --aplicar
        só gera as tabelas e recompila (sem comprimir)
    python3 sweep_tabelas.py --imagem Kodak/kodim12.png
        só mostra o resultado e grava modelos_planos.json
Opções:
    --extenso    busca bem mais ampla (~10x mais modelos)
    --comprimir  implica --aplicar; roda ./encoder na imagem e guarda os
                 modelos usados em comprimidos/<imagem>_modelos.json
"""

import argparse
import ctypes
import math
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

from modelo_estatico import (
    HEADER_BYTES,
    NUM_PLANOS,
    Modelo,
    custo_bits,
    load_symbols,
    salvar_modelos,
)

PROJECT = os.path.dirname(os.path.abspath(__file__))

L_MAX = 4096
KS = (1, 2, 4, 8, 16, 32, 64, 128)                      # L = k·M
MS = tuple(range(2, 33)) + (40, 48, 64, 96, 128, 256, 512, 1024)
JANELA_F0 = 3                                           # f0 testados ao redor de p0·M (M > 16)


def candidatos(p0: float, extenso=False):
    if extenso:
        # todas as precisões M <= 128 e f0 numa janela de ±20% ao redor de p0
        for M in range(2, 129):
            lo = max(1, math.floor((p0 - 0.2) * M))
            hi = min(M - 1, math.ceil((p0 + 0.2) * M))
            k = 1
            while k * M <= L_MAX:
                for f0 in range(lo, hi + 1):
                    yield Modelo(k * M, M, f0)
                k *= 2
        return
    for M in MS:
        if M <= 16:
            f0s = range(1, M)  # todas as probabilidades possíveis com essa precisão
        else:
            c = round(p0 * M)
            f0s = range(max(1, c - JANELA_F0), min(M - 1, c + JANELA_F0) + 1)
        for k in KS:
            L = k * M
            if L > L_MAX:
                break
            for f0 in f0s:
                yield Modelo(L, M, f0)


# ---------------------------------------------------------------------------
# Medição do custo: em C (tans_codec.h, parte TANS_SWEEP) quando possível,
# senão em Python.
# As duas dão exatamente o mesmo número de bits.
# ---------------------------------------------------------------------------


def _carregar_lib():
    """Compila a parte TANS_SWEEP de tans_codec.h como biblioteca (sweep_custo.so)."""
    src = os.path.join(PROJECT, "tans_codec.h")
    so = os.path.join(PROJECT, "sweep_custo.so")
    if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
        r = subprocess.run(["gcc", "-O3", "-shared", "-fPIC", "-x", "c", "-DTANS_SWEEP", src,
                            "-o", so], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"AVISO: não compilou o custo em C, usando Python (lento).\n{r.stderr}")
            return None
    lib = ctypes.CDLL(so)
    lib.tans_custo.restype = ctypes.c_longlong
    lib.tans_custo.argtypes = [ctypes.c_void_p, ctypes.c_longlong] + [ctypes.c_int] * 4
    return lib


_LIB = _carregar_lib()


def custo(sym: np.ndarray, m: Modelo) -> int:
    if _LIB is None:
        return custo_bits(sym.tolist(), m)
    return _LIB.tans_custo(sym.ctypes.data, len(sym), m.L, m.M, m.f0, HEADER_BYTES)


def _melhor_do_lote(sym, modelos):
    # desempate determinístico: tabela menor, depois M e f0 menores
    return min((custo(sym, m), m.L, m.M, m.f0) for m in modelos)


def otimizar(planos, extenso):
    """Avalia (plano x modelos) em paralelo (o C libera o GIL)."""
    tarefas = []
    for b, sym in enumerate(planos):
        p0 = 1.0 - float(sym.mean())
        cands = list(candidatos(p0, extenso))
        tarefas += [(b, cands[i:i + 64]) for i in range(0, len(cands), 64)]
    nthreads = (os.cpu_count() or 1) if _LIB is not None else 1
    with ThreadPoolExecutor(nthreads) as ex:
        res = list(ex.map(lambda t: (t[0], _melhor_do_lote(planos[t[0]], t[1])), tarefas))
    melhores = {}
    for b, r in res:
        if b not in melhores or r < melhores[b]:
            melhores[b] = r
    return {b: (c, Modelo(L, M, f0)) for b, (c, L, M, f0) in melhores.items()}


# ---------------------------------------------------------------------------


def planos_da_imagem(path):
    from modelo_estatico import ler_imagem_cinza

    img = ler_imagem_cinza(path)
    if img is None:
        sys.exit(f"Erro ao carregar '{path}'.")
    flat = np.ascontiguousarray(img.flatten())
    return [np.ascontiguousarray((flat >> b) & 1, dtype=np.uint8) for b in range(NUM_PLANOS)]


def executar(cmd):
    r = subprocess.run(cmd, cwd=PROJECT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"Falhou: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def gerar_compilar():
    passos = [
        [sys.executable, "ANS-Streaming Encoder.py", "--gerar-tabelas"],
        ["gcc", "-O2", "-Wall", "Encoder_Ans.c", "-o", "encoder", "-lm", "-lz"],
        ["gcc", "-O2", "-Wall", "Decoder.c", "-o", "decoder"],
    ]
    for cmd in passos:
        print(executar(cmd).strip() or "OK: " + " ".join(cmd))


def comprimir(imagem):
    """Roda o tANS em C na imagem com as tabelas recém-geradas e guarda os modelos."""
    print(executar(["./encoder", os.path.abspath(imagem)]))
    base = os.path.splitext(os.path.basename(imagem))[0]
    destino = os.path.join(PROJECT, "comprimidos", f"{base}_modelos.json")
    shutil.copyfile(os.path.join(PROJECT, "modelos_planos.json"), destino)
    print(f"Modelos desta imagem guardados em comprimidos/{base}_modelos.json\n"
          f"(para decodificar depois: python3 \"ANS-Streaming Encoder.py\" --gerar-tabelas "
          f"--modelos comprimidos/{base}_modelos.json e recompile o decoder)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--imagem", help="imagem a comprimir (senão usa plano_bit_N.txt)")
    ap.add_argument("--extenso", action="store_true", help="busca bem mais ampla (mais lenta)")
    ap.add_argument("--aplicar", action="store_true", help="gera tans_tables.h e recompila")
    ap.add_argument("--comprimir", action="store_true",
                    help="--aplicar e depois comprime a imagem com ./encoder")
    args = ap.parse_args()
    if args.comprimir and not args.imagem:
        sys.exit("--comprimir precisa de --imagem.")

    if args.imagem:
        planos = planos_da_imagem(args.imagem)
        origem = args.imagem
    else:
        planos = [np.array(load_symbols(os.path.join(PROJECT, f"plano_bit_{b}.txt")), dtype=np.uint8)
                  for b in range(NUM_PLANOS)]
        origem = "plano_bit_N.txt"

    total_cfg = sum(1 for _ in candidatos(0.5, args.extenso))
    motor = "C" if _LIB is not None else "Python"
    print(f"Sweep sobre '{origem}' ({len(planos[0])} símbolos/plano): "
          f"~{total_cfg} modelos por plano x {NUM_PLANOS} planos (custo medido em {motor})...")
    t0 = time.time()
    melhores = otimizar(planos, args.extenso)
    print(f"Concluído em {time.time() - t0:.1f}s\n")

    W = 108
    print("=" * W)
    print(f"{'PLANO':<6} {'P(0) real':>9} {'H(bits)':>8} | {'MODELO IDEAL':<30} | "
          f"{'ORIGINAL':>9} {'COMPRIM.':>9} {'GANHO':>7} {'TAXA':>8}")
    print("-" * W)
    tot_n = tot_c = 0
    modelos, extras = [], {}
    for plano, sym in enumerate(planos):
        n = len(sym)
        p0 = 1.0 - float(sym.mean())
        c, m = melhores[plano]
        H = -(p0 * math.log2(p0) + (1 - p0) * math.log2(1 - p0)) if 0 < p0 < 1 else 0.0
        print(f"{plano:<6} {p0:>9.4f} {H:>8.5f} | {m.descricao():<30} | "
              f"{n:>9} {c:>9} {n - c:>7} {100 * (1 - c / n):>7.3f}%")
        tot_n += n
        tot_c += c
        modelos.append(m)
        extras[plano] = {"p0_real": round(p0, 6), "bits_original": n, "bits_comprimido": c}
    print("-" * W)
    print(f"{'TOTAL':<6} {'':>9} {'':>8} | {'':<30} | {tot_n:>9} {tot_c:>9} "
          f"{tot_n - tot_c:>7} {100 * (1 - tot_c / tot_n):>7.3f}%")
    print("=" * W)

    salvar_modelos(modelos, origem, extras)
    print("\nModelos fixados em modelos_planos.json")
    if args.aplicar or args.comprimir:
        gerar_compilar()
    if args.comprimir:
        comprimir(args.imagem)


if __name__ == "__main__":
    main()
