# Pipeline nativo de decisões (falta, cartão, expulsão e bola parada)

Base do motor de execução da v4.3 (`match_hooks.cpp`, seção "Native-first VAR execution"). Endereços do `PES2021.exe` Steam `1.7.2.0`. **[P]** comprovado por disassembly, **[I]** inferido.

## 1. Commit do cartão

- Todo frame a engine zera `MatchControl + 0x6E0` (`0x1404228D0` chama `0x140441810`) e o `JudgeResult` da FSM em `MatchFSM + 0x49C0` (`0x140444A20` chama `0x14050B2D0`), antes do `MainTick` chamado em `0x140422AFE` **[P]**.
- O `CommitToRegistry` (`0x140422A3F`) roda depois do `MainTick`, na mesma função e na mesma thread. Ele chama `UpdateFoulCounters` (`0x1404231A8`) e logo em seguida aplica o cartão da falta comprometida (`0x1404231DC..0x140423202`) **[P]**:
  - julgamento 3: `yellows++` e, se `yellows >= 2`, `reds++`
  - julgamento 4: `reds++`
- Como os buffers são zerados todo frame, um `JudgeResult` injetado recebe o cartão **uma única vez**, no tick da injeção **[P]**. A limpeza "no tick seguinte" que o mod fazia era redundante.
- O mesmo commit copia o pool de snapshots de `MatchControl + 0x2A58` sobre `MatchInfo + 0x17AC` (`0x140423196` chama `0x140A3D3B0`) **[P]**.

## 2. Quem tira o jogador de campo

- O estado `0x12` (inline em `0x14044D442`) chama `0x140449050`, que recalcula `MatchInfo + 0x1748` a partir dos cartões do `PlayerRecord`. Com vermelho ou dois amarelos, o jogador sai. Sem cartão de expulsão e sem lesão, volta conforme `PlayerRecord + 0x18` **[P]**.
- O estado `0x13` (`0x1404485B0`) remove o ator (`0x1408C66C0`) e trata goleiro expulso (`0x140446480`) **[P]** para as chamadas, **[I]** para o efeito.
- O mod nunca grava `PlayerOnPitch`. Só escreve cartões no `PlayerRecord` e deixa os estados `0x12` e `0x13` fazerem o resto.

## 3. Filas de bola parada

- `BuildOutOfPlayQueue` (`0x1404530C0`) lê o motivo em `MatchInfo + 0x1574` e o tipo em `+0x1554` **[P]**.
- Motivo 7 (`SilentRestart`, tabela `0x14045333C`): todos os tipos montam `0x0F, 0x45, 0x16, 0x12, 0x13, [0x17 só falta], 0x14, 0x15, 0x0C, 0x19, 0x1A`. Sem cutscene, sem replay **[P]**.
- Motivo 5 (falta, tabela `0x453354`): falta com cartão (julgamento 3 ou 4) ou pênalti não vira cobrança rápida (`0x140445740`) e passa por `0x140452F00`, que também enfileira `0x12` e `0x13` **[P]**.
- `MatchFSM_RebuildSetPlay` (`0x140454280`) refaz a fila da mesma bola parada com `0x12` e `0x13` e não grava falta. A engine usa em `SetPlayReady` para troca de cobrador e mudança tática (`0x14044E286`, `0x14044E328`, `0x14044E3AF`) **[P]**.

## 4. Valores padrão das estruturas

Resets nativos `0x140A3CAE0` (FoulInfo) e `0x140A3CB50` (SetPlayInfo) **[P]**:

| Campo | Padrão |
|---|---|
| FoulInfo `+0x0C` / `+0x10` (infrator, vítima) | `0xFF` |
| FoulInfo `+0x14` / `+0x18` (atores, sentinela) | `0x18` |
| FoulInfo `+0x2C` (byte) | `9` |
| FoulInfo `+0x30` | `4` |
| SetPlayInfo `+0x04` (time) | `2` |
| SetPlayInfo `+0x28` (jogador do evento) | `0xFF` |

## 5. O que não existe nativamente

- Não há decremento de cartão **[I]** (busca de padrões sobre os 711 chamadores do getter `0x141032A40`). Revogar cartão é a única escrita manual de cartões que sobra no mod.

## 6. Como o mod aplica cada decisão

| Decisão | Caminho |
|---|---|
| Falta ou pênalti retroativo com a bola rolando | `JudgeResult` com motivo 5 no `Judge::FinalizeFrame`. A engine aplica o cartão e expulsa |
| Converter, inverter ou anular na bola parada | Revoga manualmente o cartão já contado e injeta com motivo 7. A engine aplica o cartão novo |
| Escanteio, tiro de meta ou lateral | Motivo 7 com o tipo do reinício e FoulInfo padrão |
| Só cartão, sem mudar quem está em campo | Escrita no `PlayerRecord` |
| Só cartão, expulsando ou reintegrando, na bola parada | Escrita no `PlayerRecord` e `RebuildSetPlay` |
| Só cartão, expulsando ou reintegrando, com a bola rolando | Espera a próxima bola parada |

Cada passo é verificado lendo a FSM de volta: a decisão só conta como aplicada, e o histórico só é gravado, quando a FSM sai do estado em que o pedido foi feito.
