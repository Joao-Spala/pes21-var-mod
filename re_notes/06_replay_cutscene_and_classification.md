# Replay, cutscene do cartão e classificação de lances

Pesquisa da v4.4. Endereços do `PES2021.exe` Steam `1.7.2.0`. **[P]** comprovado por disassembly, **[I]** inferido.

## 1. Anel do replay

- O sistema de replay em `*(0x143703558)` (ctor `0x140A8A800`) tem 21 buffers de `0x50` bytes em `+0x8`. O índice do slot ativo fica em `+0x698` e o ponteiro do buffer ativo em `+0x6A8` (`SelectBuffer` em `0x140A8C440`). O slot 0 é o anel ao vivo e os slots 3 a 20 guardam clipes salvos **[P]**.
- Campos do buffer:

  | Offset | Conteúdo |
  |---|---|
  | `+0x10` | u16 índice de escrita |
  | `+0x12` | u16 capacidade |
  | `+0x14` | tick da última escrita |
  | `+0x40` | quadros, passo `0x1FB0` |

- O gravador `0x1404655C0` roda a cada tick quando `MatchInfo + 0x11B0 == 0x16` **[P]**.
- A capacidade vem de `0x140A8A710`: `*(u32*)0x143703554` ou `660` quadros quando é zero. Nada no jogo escreve esse valor **[P]**.
- Quadro: `+0x00` tempo, fase no nibble baixo de `+0x07`, `+0x24C == 0x7FFF` marca quadro inválido, 25 entidades de `0x12C` bytes a partir de `+0x264` **[P]**.
- A janela do replay manual é recalculada a cada abertura a partir dos quadros válidos (`0x14217D2A0` -> `0x14217D3C0`, `0x140A8D580`, `0x140A8D3E0`) **[P]**. Mover o início da janela não recupera nada.
- Limpeza: quando `MatchInfo + 0x11B8` está ativo, o tick do controlador (`0x14042E9F0`) chama `0x140A8F310`, que invalida todos os quadros **[P]**. Quem ativa `+0x11B8` não foi encontrado.
- Os quadros não têm ponteiros, e a engine os copia como blocos brutos entre slots (`0x140A8EE70`) **[P]**. Guardar uma cópia própria e reinjetar é viável, mas depende de validar ao vivo qual slot o replay manual toca e se o elenco bate depois de substituições. A v4.4 grava esses dados no `var_core.log` (linhas `ReplayDiag`), só com leitura.

## 2. Cutscene do cartão

- Motivo 5 com falta comum enfileira `0x03`, `0x05`, `0x45` (`0x140452C80`) **[P]**.
- O estado `0x05` escolhe a demo em `0x14047FB40`. A demo depende do julgamento do evento de falta mais recente no histórico (categoria 4, gravado pelo observador `0x140A141F0` na transição de status 1 para 2 com motivo 5) e dos amarelos **já gravados** no `PlayerRecord` no momento da escolha **[P]**.

  | Caso | Falta | Pênalti |
  |---|---|---|
  | amarelo simples | `0x20019` | `0x2000F` |
  | segundo amarelo | `0x2001C` | `0x20012` |
  | vermelho de quem já tinha amarelo | `0x2001D` | `0x20013` |
  | vermelho direto | `0x2001A` | `0x20010` |

- Como a engine grava o cartão no tick da injeção (v4.3), a falta marcada pelo VAR com a bola rolando escolhe a demo correta, inclusive o segundo amarelo **[P]**. As versões até a v4.2 neutralizavam esse commit, e por isso tocavam o amarelo simples **[I]**.
- A partir de `SetPlayReady` o mod usa motivo 7, que não tem cutscene. Usar motivo 5 ali exigiria: corrigir a demo (o observador não dispara na transição 3 para 2 e a escolha leria a falta antiga), evitar a cobrança rápida das faltas sem cartão (`0x140445740`) e entender a flag `MatchFSM + 0x904` lida pelo estado `0x23` **[P]** para os requisitos, sem validação ao vivo.

## 3. Replay automático da falta

- No caminho de falta comum, `0x1D` e `0x1F` são enfileirados logo depois de `0x11`, a menos que `MatchInfo + 0x177F` esteja ativo **[P]**.
- O estado `0x1D` (`0x14044BC30`) não lê `FoulInfo` nem tick de snapshot **[P]**. A janela que ele toca não foi provada. A v4.4 registra no log a janela real de cada `0x1D` comparada ao tick da parada.

## 4. IDs de ação e classificação

- `PlayerAction + 0x01` é o enum `ANIME_*`, lido como byte com sinal. Tabela de nomes em `0x1434B3470`, categorias em `0x142651590` **[P]**:

  | ID | Nome |
  |---|---|
  | 11, 12 | `ANIME_SLIDING` (carrinho) |
  | 13 | `ANIME_TACKLE` (bote em pé) |
  | 14 | `ANIME_BLOCK` |
  | 15 | `ANIME_DIVE` (fora do filtro do Judge) |
  | 16, 17 | `ANIME_STAGGER`, `ANIME_FALL_DOWN` |
  | 55 a 71 | goleiro |
  | 72 | `ANIME_JOSTLE` |

- `ObserverFallDownOrStagger` (`0x140A1AF50`) dispara para a vítima com reação de contato e lê o causador em `(int8)PlayerAction + 0x3D` **[P]**. A v4.4 usa esse mesmo modelo.
- `TeamTactics + 0x254` é o lado do time como byte com sinal (`movsbl` em `0x140438475`), e inverte no intervalo **[P]**. O mod lia como float, o sinal saía de `+0x257` e nunca invertia. A convenção é calibrada em cada saída de bola pelas posições dos jogadores.
