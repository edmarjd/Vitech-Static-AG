"""
modelo_estatico.py - ANS estático agnóstico POR PLANO (fonte única dos modelos).

Cada plano de bit N tem UM modelo fixo (L, M, f0):
    M  = precisão das frequências   ->  P(0) = f0 / M,  P(1) = (M - f0) / M
    L  = k·M = tamanho da tabela    ->  estados em [L, 2L-1]
O modelo de cada plano é escolhido por sweep_tabelas.py (o que comprime melhor
a imagem a ser comprimida) e gravado em modelos_planos.json. Dentro da imagem
não há varredura nenhuma: o plano N usa sempre o modelo N, do primeiro ao
último símbolo.

Fluxo completo (sweep -> tabelas pelo rANS -> recompila -> comprime):
    python3 sweep_tabelas.py --imagem Kodak/kodim12.png --comprimir
"""

import json
import os
from collections import deque
from typing import NamedTuple

NUM_PLANOS = 8
HEADER_BYTES = 5  # [num_symbols 4B] [plano (3 bits) | assinatura do modelo (5 bits) 1B]
ARQ_MODELOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modelos_planos.json")


class Modelo(NamedTuple):
    L: int   # estados em [L, 2L-1]
    M: int   # soma das frequências (precisão)
    f0: int  # frequência do símbolo 0

    @property
    def freqs(self):
        return (self.f0, self.M - self.f0)

    @property
    def flush_bits(self):
        return (self.L - 1).bit_length()  # ceil(log2 L): estado final - L

    def assinatura(self) -> int:
        """5 bits gravados no cabeçalho para o decoder detectar modelo diferente."""
        return (self.L * 31 + self.M * 7 + self.f0) % 32

    def valido(self) -> bool:
        return 1 <= self.f0 < self.M and self.L % self.M == 0 and 2 <= self.L <= 4096

    def descricao(self) -> str:
        f0, f1 = self.freqs
        return f"P(0)={f0}/{self.M} ({f0 / self.M:.1%})  L={self.L}"


# Modelo usado quando modelos_planos.json não existe (antes do primeiro sweep)
MODELO_PADRAO = Modelo(L=64, M=16, f0=8)


def carregar_modelos(path: str = ARQ_MODELOS) -> list[Modelo]:
    if not os.path.exists(path):
        return [MODELO_PADRAO] * NUM_PLANOS
    with open(path, encoding="utf-8") as f:
        dados = json.load(f)
    modelos = [Modelo(**{k: dados["planos"][str(p)][k] for k in ("L", "M", "f0")})
               for p in range(NUM_PLANOS)]
    assert all(m.valido() for m in modelos), "modelo inválido em modelos_planos.json"
    return modelos


def salvar_modelos(modelos: list[Modelo], origem: str, extras=None, path: str = ARQ_MODELOS):
    dados = {"origem": origem, "planos": {}}
    for p, m in enumerate(modelos):
        dados["planos"][str(p)] = {"L": m.L, "M": m.M, "f0": m.f0, **((extras or {}).get(p, {}))}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dados, f, indent=2, ensure_ascii=False)


# ---------------------------------------------------------------------------
# rANS streaming (b = 2). É daqui que saem as tabelas tANS.
# ---------------------------------------------------------------------------


def _encode_symbol(x: int, s: int, m: Modelo, bitstream: list) -> int:
    """Renormaliza x para [k·F(s), 2k·F(s)) emitindo LSBs e aplica a equação (7):
    x' = (x // F(s))·M + C(s) + (x mod F(s))  ->  x' em [L, 2L)."""
    fs = m.freqs[s]
    cs = 0 if s == 0 else m.f0
    lim = 2 * (m.L // m.M) * fs
    while x >= lim:
        bitstream.append(x & 1)
        x >>= 1
    return (x // fs) * m.M + cs + (x % fs)


def _decode_symbol(x: int, m: Modelo) -> tuple[int, int]:
    """Equação (8): (símbolo, estado anterior ainda sem renormalizar)."""
    slot = x % m.M
    s = 1 if slot >= m.f0 else 0
    cs = 0 if s == 0 else m.f0
    return s, m.freqs[s] * (x // m.M) + slot - cs


def encode_all(symbols, m: Modelo) -> tuple[list[int], int]:
    x = m.L
    bits: list[int] = []
    for s in symbols:
        x = _encode_symbol(x, s, m, bits)
    return bits, x


def decode_all(bitstream, final_state: int, n: int, m: Modelo) -> list[int]:
    bits = deque(reversed(bitstream))
    out = [0] * n
    x = final_state
    for i in range(n - 1, -1, -1):
        s, x = _decode_symbol(x, m)
        while x < m.L and bits:
            x = (x << 1) | bits.popleft()
        out[i] = s
    return out


def tabela_tans(m: Modelo):
    """Tabela tANS do modelo: para s em {0,1} e idx = x - L,
    (novo_idx, valor_bits MSB=1º emitido, nbits)."""
    tab = {0: [], 1: []}
    for s in (0, 1):
        for x in range(m.L, 2 * m.L):
            emit: list[int] = []
            novo = _encode_symbol(x, s, m, emit)
            valor = 0
            for b in emit:
                valor = (valor << 1) | b
            tab[s].append((novo - m.L, valor, len(emit)))
    return tab


def custo_bits(symbols, m: Modelo, tab=None) -> int:
    """Bits do arquivo comprimido completo (cabeçalho + bitstream + estado +
    stop bit + preenchimento), simulando exatamente a tabela usada pelo C."""
    tab = tab or tabela_tans(m)
    n0 = [e[0] for e in tab[0]]
    n1 = [e[0] for e in tab[1]]
    b0 = [e[2] for e in tab[0]]
    b1 = [e[2] for e in tab[1]]
    st = bits = 0
    for s in symbols:
        if s:
            bits += b1[st]
            st = n1[st]
        else:
            bits += b0[st]
            st = n0[st]
    payload = bits + m.flush_bits + 1
    return 8 * (HEADER_BYTES + (payload + 7) // 8)


def ler_imagem_cinza(path: str):
    """Imagem em tons de cinza com a MESMA conversão do C (leitor de PNG do Encoder_Ans.c):
    Y = (9798·R + 19235·G + 3735·B + 16384) >> 15  (BT.601, = cv2.cvtColor BGR2GRAY).
    Não usa cv2.IMREAD_GRAYSCALE, que aplica a gama do PNG e não é reproduzível em C."""
    import cv2
    import numpy as np

    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        return None
    if img.dtype != np.uint8:
        raise ValueError(f"'{path}': só imagens de 8 bits por canal são suportadas")
    if img.ndim == 2:
        return img
    b, g, r = (img[..., i].astype(np.uint32) for i in range(3))
    return ((9798 * r + 19235 * g + 3735 * b + 16384) >> 15).astype(np.uint8)


def load_symbols(path: str) -> list[int]:
    """Aceita '0 1 0', '0,1,0', um por linha ou '010...' contínuo."""
    with open(path, "r") as f:
        content = f.read().replace(",", " ")
    tokens = content.split()
    if all(len(t) == 1 for t in tokens):
        return [int(t) for t in tokens if t in ("0", "1")]
    return [int(c) for c in content if c in ("0", "1")]
