import argparse
import os
import time

from modelo_estatico import carregar_modelos, decode_all, load_symbols


def load_encoded(path: str) -> dict:
    """Lê o arquivo do encoder Python (4 linhas: plano, n, bitstream, estado)."""
    with open(path, "r") as f:
        lines = [ln.strip() for ln in f.readlines()]
    if len(lines) < 4:
        raise ValueError(f"Arquivo '{path}' tem formato inválido (4 linhas esperadas).")
    return {
        "plano": int(lines[0]),
        "num_symbols": int(lines[1]),
        "bitstream": lines[2],
        "final_state": int(lines[3]),
    }


def main():
    parser = argparse.ArgumentParser(description="ANS Streaming Decoder - Estático Agnóstico por plano")
    parser.add_argument("--encoded", default="input_encoded.bin", help="Arquivo comprimido")
    parser.add_argument("--output", default="output.txt", help="Arquivo de saída")
    parser.add_argument("--verify", default="", help="Arquivo original para verificação")
    args = parser.parse_args()

    if not os.path.exists(args.encoded):
        print(f"Erro: '{args.encoded}' não encontrado.")
        return

    data = load_encoded(args.encoded)
    m = carregar_modelos()[data["plano"]]
    n = data["num_symbols"]
    bits = [int(c) for c in data["bitstream"]]

    print("=" * 72)
    print(f"  ANS STREAMING DECODER  |  plano {data['plano']}  |  {m.descricao()} (fixo)")
    print("=" * 72)

    t0 = time.time()
    decoded = decode_all(bits, data["final_state"], n, m)
    print(f"Decodificados {n} símbolos em {time.time() - t0:.4f}s")

    with open(args.output, "w") as f:
        f.write(" ".join(map(str, decoded)))
    print(f"Saída: '{args.output}'")

    if args.verify and os.path.exists(args.verify):
        ok = decoded == load_symbols(args.verify)
        print("OK: idêntico ao original" if ok else "ERRO: diverge do original")


if __name__ == "__main__":
    main()
