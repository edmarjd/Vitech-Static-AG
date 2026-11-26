# Projeto ANS — tANS estático agnóstico para planos de bits

Compressão **sem perdas** de imagens em tons de cinza, plano de bit por plano de bit, usando
**ANS (Asymmetric Numeral Systems)**.

| Parte | Linguagem | Função |
|---|---|---|
| **rANS** (streaming) | Python | Modelo de referência. **É o rANS que gera as tabelas do tANS.** |
| **tANS** (tabelas) | C | Encoder e decoder rápidos, que só consultam as tabelas geradas pelo rANS. O `./encoder` recebe a imagem PNG e faz tudo: planos → tANS → arquivos → decodificação → verificação. |

---

## Como rodar (rápido)

**Tabelas ideais para a imagem que você quer comprimir** (recomendado):

```bash
python3 sweep_tabelas.py --imagem Kodak/kodim12.png --comprimir
```

Faz o sweep **nessa imagem**, gera as tabelas pelo rANS, recompila e comprime/verifica com o
`./encoder` (≈2 s na Kodak, ≈13 s numa imagem do DIV2K). Veja *Tabelas ideais por imagem (sweep)*.

**Com as tabelas que vêm no repositório** (as da Kodak 19; se você já rodou o sweep, restaure-as antes
com `git checkout modelos_planos.json tans_tables.h`):

```bash
gcc -O2 Encoder_Ans.c -o encoder -lm -lz      # compila (letra O em -O2; precisa da zlib)
./encoder Kodak/kodim19.png                   # comprime a Kodak 19 e mostra a taxa de cada plano
```

Outras formas:

```bash
./encoder Kodak/*.png                         # as 24 imagens da Kodak (relatório de cada uma + resumo)
./encoder -r Kodak/*.png                      # só o resumo, uma linha por imagem
```

Saída esperada na Kodak 19: plano 6 com 12,789%, plano 7 com 16,473% e **total de 3,787%**, com
`VERIFICACAO LOSSLESS` igual a `OK` em todos os planos. Os arquivos comprimidos ficam em `comprimidos/`.

O `tans_tables.h` já vem gerado. Só é preciso gerá-lo de novo se `modelos_planos.json` mudar (veja
*Compilar o tANS*).

---

## Como funciona

1. **Planos de bits.** Cada pixel (8 bits) é separado em 8 planos binários, do bit 0 ao bit 7. Cada plano
   vira uma sequência de `0` e `1` gravada em `plano_bit_N.txt`. Os planos altos (6 e 7) são previsíveis;
   os baixos (0 a 5) são quase ruído (perto de 50/50).
2. **Uma tabela fixa por plano.** Cada plano N tem um único modelo `(L, M, f0)`:
   - `P(0) = f0/M` e `P(1) = (M − f0)/M`: a probabilidade **nunca muda**;
   - `L = k·M`: tamanho da tabela (estados em `[L, 2L−1]`).
3. **Estático e agnóstico.** Em tempo de execução não há varredura por bloco, nem contagem de 0s e 1s,
   nem escolha de tabela, nem contexto, nem frequências no cabeçalho. O plano N usa **sempre** a tabela N,
   em qualquer imagem, do primeiro ao último símbolo.
4. **Escolha das tabelas (sweep, antes de comprimir).** O `sweep_tabelas.py` analisa **a imagem que
   vai ser comprimida**, testa milhares de combinações de `L`, `M` e `f0` para cada plano, mede o tamanho
   **real** do arquivo comprimido e grava a melhor de cada plano em `modelos_planos.json`. A partir daí
   cada plano tem **uma probabilidade fixa** e a tabela tANS correspondente. A medição do custo roda em C
   (parte `TANS_SWEEP` do `tans_codec.h`, mesma equação do rANS, mesmos bits que `custo_bits` em
   Python), por isso cabe fazer o sweep a cada imagem.
5. **O rANS gera o tANS.** O `ANS-Streaming Encoder.py --gerar-tabelas` aplica o encoder rANS
   (`_encode_symbol` em `modelo_estatico.py`) a cada estado `x ∈ [L, 2L−1]` e a cada símbolo:
   - renormalização: enquanto `x ≥ 2k·F(s)`, emite `x & 1` e faz `x >>= 1`;
   - equação (7): `novo estado = (x / F(s))·M + C(s) + (x mod F(s))`.

   O resultado (novo estado, bits emitidos, quantidade de bits) vira o `tans_tables.h`. O encoder em C
   só consulta essa tabela; o decoder em C monta a tabela inversa a partir dela e
   renormaliza como o rANS (lê bits enquanto `x < L`), o que vale para qualquer modelo que o sweep escolher.
6. **Sem RAW.** Todo plano é sempre codificado com tANS.

```text
imagem a comprimir ──► sweep_tabelas.py ──► modelos_planos.json
                                                   │
                                rANS (modelo_estatico.py)
                                                   ▼
                    ANS-Streaming Encoder.py --gerar-tabelas ──► tans_tables.h
                                                                     │  gcc
                                                                     ▼
                imagem.png ──► ./encoder  (planos ─► tANS ─► .bin ─► decodifica ─► confere)
```

---

## Requisitos

- Python 3.9+ com `numpy`, `opencv-python` e `matplotlib`
- `gcc` (ou outro compilador C99), a biblioteca matemática (`-lm`) e a **zlib** (`-lz`, para ler PNG)

```bash
pip install numpy opencv-python matplotlib
sudo apt install zlib1g-dev        # se o gcc reclamar de zlib.h
```

As imagens **não** estão no git: coloque o Kodak (`kodim01.png` a `kodim24.png`) em `Kodak/` e, se for
usar, o DIV2K em `DIV2K/`.

O Python só é necessário para **gerar as tabelas** (rANS) e para o sweep. Para comprimir imagens basta o C.

Execute tudo **de dentro da pasta do projeto**. Atenção: `-O2` é a letra **O**, não o zero, e o nome
`"ANS-Streaming Encoder.py"` tem espaço (use as aspas).

---

## Compilar o tANS

As tabelas já vêm prontas em `modelos_planos.json` (escolhidas pelo sweep na Kodak 19). Para gerar o
`tans_tables.h` a partir delas pelo rANS e compilar:

```bash
python3 "ANS-Streaming Encoder.py" --gerar-tabelas      # rANS -> tans_tables.h
gcc -O2 Encoder_Ans.c -o encoder -lm -lz                # encoder (imagem PNG ou plano .txt)
gcc -O2 Decoder.c -o decoder                            # decoder dos arquivos .bin
```

A primeira linha da saída deve ser `tans_tables.h gerado com 8 tabelas fixas (uma por plano):`, seguida do
modelo de cada plano.

**Sempre que `modelos_planos.json` mudar, gere as tabelas e recompile os programas em C.** O decoder
recusa arquivos gerados com outras tabelas (assinatura no cabeçalho).

---

## Rodar com imagens do Kodak (em C)

```bash
./encoder Kodak/kodim19.png                     # relatório completo de uma imagem
./encoder Kodak/kodim01.png Kodak/kodim05.png
./encoder Kodak/*.png                           # as 24: relatório de cada uma + resumo final
./encoder -r Kodak/*.png                        # -r: só o resumo (uma linha por imagem)
```

Kodak: 24 imagens de 768×512 = **393.216 símbolos por plano**.

## Rodar com imagens do DIV2K (em C)

```bash
./encoder DIV2K/0001.png
./encoder -r DIV2K/000*.png                     # 0001 a 0009, só o resumo
./encoder -r DIV2K/*.png                        # as 800
```

DIV2K: 800 imagens de até 2040×1404 ≈ **2,9 milhões de símbolos por plano**.

Com uma imagem PNG, o `./encoder`:
1. lê o PNG e converte para tons de cinza (leitor de PNG dentro do `Encoder_Ans.c`);
2. separa os 8 planos de bits;
3. codifica cada plano com a **sua tabela fixa** (`tans_codec.h` + `tans_tables.h`);
4. grava `comprimidos/<imagem>_plano_N.bin` (abre com `./decoder`);
5. decodifica e confere bit a bit;
6. imprime a tabela com uma coluna por plano e a coluna `TOTAL`.

Os tempos do relatório medem só o tANS (sem leitura do PNG nem gravação de arquivo).

**Conversão para cinza:** `Y = (9798·R + 19235·G + 3735·B + 16384) >> 15` (BT.601, idêntica ao
`cv2.cvtColor(..., COLOR_BGR2GRAY)`). O C e o Python usam exatamente a mesma fórmula
(`modelo_estatico.ler_imagem_cinza`); a gama gravada no PNG é ignorada. Suporta PNG de 8 bits por canal sem
entrelaçamento (cinza, RGB, RGBA, paleta), que é o caso de todas as imagens do Kodak e do DIV2K.

### Conferir com o rANS em Python (`Bit_Planes.py`)

```bash
python3 Bit_Planes.py Kodak/kodim19.png         # grava plano_bit_N.txt + figura e codifica com o rANS
python3 Bit_Planes.py                           # usa os plano_bit_N.txt já existentes
```

Separa a imagem em planos (`plano_bit_N.txt` e a figura `resultado_planos_bits.png`) e codifica cada um
com o rANS de referência em Python. O tamanho é idêntico ao do `./encoder` (3,787% na Kodak 19), só que
bem mais lento.

---

## Tabelas ideais por imagem (sweep)

O sweep analisa **a imagem que você quer comprimir** e define, para cada plano, a probabilidade fixa e a
tabela tANS que dão o menor arquivo:

```bash
# sweep + tabelas (rANS) + recompila + comprime e verifica
python3 sweep_tabelas.py --imagem Kodak/kodim12.png --comprimir

# busca extensa (~22.500 modelos por plano; ~20 s na Kodak)
python3 sweep_tabelas.py --imagem Kodak/kodim12.png --extenso --comprimir

# só gerar as tabelas e recompilar / só o sweep (grava modelos_planos.json, sem gerar tabelas)
python3 sweep_tabelas.py --imagem Kodak/kodim12.png --aplicar
python3 sweep_tabelas.py --imagem Kodak/kodim12.png
```

Todos os modos **sobrescrevem `modelos_planos.json`** (e, com `--aplicar`/`--comprimir`, o
`tans_tables.h`). Para voltar às tabelas da Kodak 19 que vêm no repositório:

```bash
git checkout modelos_planos.json tans_tables.h
gcc -O2 Encoder_Ans.c -o encoder -lm -lz && gcc -O2 Decoder.c -o decoder
```

| Modo | Modelos por plano | Kodak (393 mil símbolos/plano) | DIV2K 0001 (2,9 milhões) |
|---|---|---|---|
| normal | ~2.200 | ~2 s | ~13 s |
| `--extenso` | ~22.500 | ~22 s | ~2 min |

Com `--comprimir`, os modelos usados ficam também em `comprimidos/<imagem>_modelos.json`. Os `.bin`
dependem das tabelas com que foram gerados: para decodificar depois um arquivo de outra imagem, gere as
tabelas dela e recompile o decoder:

```bash
python3 "ANS-Streaming Encoder.py" --gerar-tabelas --modelos comprimidos/kodim12_modelos.json
gcc -O2 Decoder.c -o decoder
./decoder comprimidos/kodim12_plano_7.bin plano_7.txt
```

O codec continua estático: dentro da imagem o plano N usa sempre a tabela N, do primeiro ao último
símbolo, e nada vai no cabeçalho além da assinatura de 5 bits do modelo.

### Resultado: tabelas por imagem × tabela única da Kodak 19

Sweep normal em cada uma das 24 imagens da Kodak (todas sem perdas):

| Imagem | Tabelas da Kodak 19 | Tabelas da própria imagem |
|---|---|---|
| kodim01 | −0,111% | 1,258% |
| kodim02 | 9,330% | 21,245% |
| kodim09 | −3,586% | 3,192% |
| kodim12 | −6,166% | 7,564% |
| kodim20 | 3,782% | 14,197% |
| kodim21 | −0,495% | 1,305% |
| **Kodak inteira** | **2,735%** | **≈5,39%** |

Nenhuma imagem fica maior que a original. No DIV2K 0001 o total é 8,789%.

### Tabelas que vêm no repositório (sweep extenso na Kodak 19)

| Plano | P(0) real | Tabela fixa | P(0) da tabela | Comprimido (bits) | Taxa |
|---|---|---|---|---|---|
| 0 | 0,487 | L=864, 13/27 | 0,481 | 393.080 | 0,035% |
| 1 | 0,506 | L=3424, 54/107 | 0,505 | 393.232 | −0,004% |
| 2 | 0,502 | L=2, 1/2 | 0,500 | 393.264 | −0,012% |
| 3 | 0,494 | L=3712, 57/116 | 0,491 | 393.248 | −0,008% |
| 4 | 0,453 | L=46, 9/23 | 0,391 | 389.240 | 1,011% |
| 5 | 0,502 | L=206, 41/103 | 0,398 | 393.152 | 0,016% |
| 6 | 0,343 | L=14, 2/7 | 0,286 | 342.928 | 12,789% |
| 7 | 0,677 | L=22, 7/11 | 0,636 | 328.440 | 16,473% |
| **Total** | | | | **3.026.584** | **3,787%** |

Original: 393.216 bits por plano, 3.145.728 no total.

Às vezes o melhor `f0` não é a proporção real de zeros (planos 4 e 5). O estado do tANS com tabelas
pequenas aproveita um pouco da correlação entre pixels vizinhos, e o sweep mede o tamanho real do arquivo,
não a entropia. A decodificação continua exata.

> **Atenção:** usar as tabelas de uma imagem em outra continua sem perdas, mas planos com proporção de
> 0s e 1s diferente podem ficar **maiores** que o original (não há RAW). Por isso o recomendado é fazer o
> sweep na própria imagem (`--comprimir`).

---

## Encoder e decoder em C com um plano em texto

```bash
./encoder plano_bit_7.txt                    # plano deduzido do nome; gera plano_bit_7_encoded.bin
./encoder -p 7 qualquer_nome.txt             # plano informado explicitamente
./decoder plano_bit_7_encoded.bin plano_7.txt
```

## Versão Python (rANS de referência)

```bash
python3 "ANS-Streaming Encoder.py" --input plano_bit_7.txt --output comprimido.txt
python3 ANS-Streaming-Decoder.py --encoded comprimido.txt --output plano_7.txt --verify plano_bit_7.txt
```

Para todos os planos de uma vez, use o `Bit_Planes.py`.

O codec Python usa os mesmos modelos de `modelos_planos.json` e produz exatamente o mesmo bitstream que o
tANS em C.

---

## Formato do arquivo comprimido (C)

| Campo | Tamanho |
|---|---|
| Número de símbolos | 4 bytes |
| Plano (bits 0–2) + assinatura do modelo (bits 3–7) | 1 byte |
| Bitstream tANS | variável |
| Estado final − L | `ceil(log2 L)` bits |
| *Stop bit* (`1`) | 1 bit |
| Preenchimento até completar o byte | 0 a 7 bits |

O decoder lê o bitstream **de trás para frente** (o ANS é LIFO). O tamanho do payload vem do tamanho do
arquivo.

---

## Métricas do relatório (`./encoder` e `Bit_Planes.py`)

| Linha | Significado |
|---|---|
| TAMANHO ORIGINAL | Símbolos do plano (1 bit cada) |
| TABELA FIXA (L f0/M) | Tabela usada pelo plano, por exemplo `L22 7/11` |
| OVERHEAD | Cabeçalho: 5 bytes = 40 bits |
| BITS DE PREENCHIMENTO | Bits que completam o último byte |
| ESTADO | Estado final + *stop bit* |
| BITSTREAM | Bits produzidos pela codificação dos símbolos |
| COMPRIMIDO GLOBAL | Overhead + preenchimento + estado + bitstream |
| GANHO TOTAL | Original − comprimido (negativo se o arquivo cresceu) |
| TAXA DE COMPRESSAO | (1 − comprimido / original) × 100 |
| TEMPO ENCODER / DECODER | Só o tANS (no `./encoder` com PNG, sem leitura da imagem nem gravação do `.bin`) |
| VERIFICACAO LOSSLESS | `OK` quando o plano decodificado é idêntico ao original |

---

## Estrutura de arquivos

```text
Projeto-ANS-main/
├── Encoder_Ans.c              Encoder tANS (C): imagem PNG (8 planos + verificação) ou plano .txt
├── Decoder.c                  Decoder dos arquivos .bin (C)
├── tans_codec.h               Núcleo do tANS (codifica/decodifica) + custo em C usado pelo sweep
├── tans_tables.h              Tabelas do tANS (GERADO: não editar à mão)
├── modelo_estatico.py         rANS de referência e definição dos modelos (L, M, f0)
├── modelos_planos.json        Tabela fixa de cada plano (gerada pelo sweep)
├── sweep_tabelas.py           Sweep na imagem a comprimir: escolhe a tabela ideal de cada plano
├── ANS-Streaming Encoder.py   Gera o tans_tables.h a partir do rANS; encoder Python
├── ANS-Streaming-Decoder.py   Decoder Python
├── Bit_Planes.py              Separa a imagem em planos e codifica com o rANS em Python
├── .clang-format              Estilo do código C (K&R)
├── Kodak/                     kodim01.png a kodim24.png (fora do git)
├── DIV2K/                     0001.png a 0800.png (fora do git)
└── comprimidos/               Saída do ./encoder com imagens (fora do git)
```

Gerados ao compilar ou rodar (fora do git): `encoder`, `decoder`, `sweep_custo.so`, `plano_bit_N.txt`,
`resultado_planos_bits.png`.

---

## Parâmetros

| O quê | Onde | Observação |
|---|---|---|
| Tabela de cada plano | `modelos_planos.json` (via `sweep_tabelas.py`) | Depois, gere as tabelas e recompile |
| Faixa testada no sweep | `KS`, `MS`, `L_MAX`, `JANELA_F0` em `sweep_tabelas.py` | `L` vai até 4096 |
| Limite de símbolos por plano | `MAX_SYMBOLS` (encoder) e `MAX_DECODED_SYMBOLS` (decoder) | 12 milhões (DIV2K cabe) |

---

## Problemas comuns

| Mensagem ou sintoma | Causa e solução |
|---|---|
| `Ficheiro ou pasta inexistente` / `No such file` | Terminal na pasta errada. Use `cd` para a pasta do projeto. |
| `unrecognized command-line option '-02'` | Zero em vez da letra O. Use `-O2`. |
| `can't open file ... ANS-Streaming_Encoder.py` | O nome tem espaço: `"ANS-Streaming Encoder.py"`, entre aspas. |
| `'./encoder' não encontrado` | Compile antes (seção *Compilar o tANS*). |
| `fatal error: zlib.h` | Instale a zlib: `sudo apt install zlib1g-dev`. |
| `undefined reference to uncompress` | Faltou `-lz` no fim do `gcc` do encoder. |
| `não suportado (precisa PNG 8 bits/canal...)` | PNG de 16 bits ou entrelaçado. Converta para PNG de 8 bits. |
| `ERRO: o plano N foi codificado com outro modelo` | O `.bin` foi gerado com outras tabelas. Gere as tabelas, recompile e codifique de novo. |
| `ERRO: plano desconhecido` | O nome do arquivo não tem `plano_bit_N`. Use `./encoder -p N arquivo.txt`. |
| `ERRO: muitos símbolos` | A imagem passa de `MAX_SYMBOLS`. Aumente no encoder e no decoder. |
| Taxa negativa em alguns planos | Tabelas ajustadas para outra imagem. Faça o sweep na própria imagem (*Tabelas ideais por imagem*). |
