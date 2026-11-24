# 02. Maquina de Estados da Partida (`MatchFSM`) e Analise de Intervencoes VAR

Mapeamento completo da maquina de estados `MatchFSM` (`*(MatchControl + 0x38)`) em `PES2021.exe` (Steam 1.7.2.0) e analise tecnica deterministica dos 4 cenarios de intervencao VAR.

## 1. Arquitetura da `MatchFSM`

### 1.1. Ciclo de Tick por Frame

1. `MatchControl::vfunc[9]` (`0x140422670`) prepara buffers e dispara o ciclo de simulacao.
2. `MatchFSM::MainTick` (`0x1404468F0`):
   - Sincroniza `MatchInfo + 0x1748[0..21]` (`PlayerOnPitch`) para `MatchFSM + 0x2790` via `0x140441920`.
   - Processa comandos/flags de jogadores e modo de jogo.
   - Chama `MatchFSM::DispatchState` (`0x14044D2F0`), que executa o estado atual na fila circular/linear `*(int32_t*)(MatchFSM + 0x878 + *(uint8_t*)(MatchFSM + 0x8F0) * 4)`.
   - Executa atualizadores pos-estado (`0x14044A6C0`, `0x14044B430`, `0x14044D8F0`, `0x14044B040`, `0x14044C050`).
   - Copia o estado consolidado `MatchFSM + 0x2790` -> `MatchControl + 0x6E0` via `0x140443DB0`.
3. `MatchControl::CommitToRegistry` (`0x140422A3F`) publica `SetPlayInfo`, `FoulInfo`, `AdvantageInfo`, `PlayerOnPitch` e incrementa cartoes no `PlayerRecord` (`0x1404231DC`).

### 1.2. Offsets Criticos em `MatchFSM`

| Offset | Tipo | Campo | Funcao |
|---|---|---|---|
| `+0x874` | `int32_t` | `SequenceCategory` | Categoria da fila atual (`1`=OutOfPlay/Rebuild, `3`=Kickoff, `4`=GoalKick, `5`=Corner, `6`=ThrowIn, `7`=Foul/Penalty, `8`=Half/FullTime, `10`=Goal) |
| `+0x878` | `int32_t[30]` | `StateQueue` | Fila de ate 30 estados (`0x1E`), preenchida com `0x48` (vazio) por `MatchFSM_ResetQueue` (`0x1404529F0`) |
| `+0x8F0` | `uint8_t` | `QueueHead` | Indice do estado em execucao |
| `+0x8F1` | `uint8_t` | `QueueTail` | Indice de insercao na fila |
| `+0x8F2` | `uint8_t` | `SubState` | Sub-estado interno do handler atual (resetado para `0` ao avancar `QueueHead`) |
| `+0x908` | `Judge` | `EmbeddedJudge` | Instancia de `Judge@ai@match` |
| `+0x2790` | `struct` | `WorkingMatchState` | Buffer de trabalho copiado ao `MatchControl` (`+0x2794` = `MatchStatus` 1/2/3) |
| `+0x27FC` | `uint8_t` | `QuickRestartFlag` | `1/2/3` quando elegivel ou em cobranca rapida |
| `+0x49C0` | `JudgeResult` | `LiveJudgeResult` | Resultado do `Judge` no frame (`+0x00` SetPlayInfo, `+0x30` FoulInfo, `+0x6C` AdvantageInfo, `+0x7C` GoalInfo) |
| `+0x57F8` | `uint8_t` | `DelayedCardPending` | Flag de cartao tardio pos-vantagem (aciona estado `0x20`) |
| `+0x17860` | `void**` | `MatchContextPtr` | Ponteiro para contexto interno de atores/eventos |

### 1.3. Mapa dos Estados Principais de `MatchFSM::DispatchState` (`0x14044D2F0`)

Extraido diretamente da tabela de despacho (`idx` em `0x14044D7C0`, `rva` em `0x14044D734`, 72 estados `0x00..0x47`):

| Estado | Handler VA | Papel na Partida |
|---|---|---|
| `0x00` | `0x14044A560` | Inicializacao / entrada de periodo |
| `0x01` | `0x14044DB80` | Despacho de `OutOfPlay` -> chama `MatchFSM::BuildOutOfPlayQueue` (`0x1404530C0`) |
| `0x03..0x0B` | `0x140446DF0` | Cutscenes e reacoes de parada (gol `0x04`, falta/cartao `0x06` `0x140446F80`, saida/lesao `0x09` `0x140447150`) |
| `0x0C..0x11` | `0x140449340` | Transicoes de fade/camera entre cutscene e campo |
| `0x12` | inline `0x14044D442` | Preparacao pre-substituicao / sincronizacao de periodo |
| `0x13` | `0x1404485B0` | Aplicacao de substituicoes, checagem de goleiro expulso (`0x140446480`) e selecao de cobrador (`0x1404A8730`) |
| `0x14` | `0x14044AD00` | Sincronizacao de entidades e inicio de posicionamento de bola parada |
| `0x15` | `0x14044C5B0` | Finalizacao de posicionamento dos 22 atores e definicao de `MatchStatus = 3` |
| `0x16` | `0x14044A780` | Processamento de substituicoes pendentes e eventos de troca |
| `0x17` | inline `0x14044D580` | Montagem de barreira (enfileirado somente quando `SetPlayKind == 5`) |
| `0x18` | `0x14044E810` | Cobranca rapida de falta (Quick Free Kick) |
| `0x19` | `0x14044CA20` | Entrada final em bola parada (hud/camera/notificacoes) |
| `0x1A` | `0x14044DD50` | **`SetPlayReady`:** aguardando cobranca de falta, penalti, escanteio, lateral ou tiro de meta |
| `0x1C` | `0x140449F40` | **`InPlay`:** bola rolando, executa `Judge::Evaluate` a cada frame |
| `0x1D` | `0x14044BC30` | Replay automatico pos-lance (gol / falta grave) |
| `0x20` | `0x14044B9D0` | Cartao amarelo tardio pos-vantagem |
| `0x23` | `0x1404498B0` | Limpeza pre-setup da fila de bola parada (`0x140452F00`) |
| `0x33` | `0x14044C310` | Lateral rapido (Quick Throw-In) |
| `0x34` | `0x14044AE40` | Validacao de troca de cobrador / estado pre-`0x19` |
| `0x45` | `0x140446DB0` | Reset de flag de HUD/transicao |

---

## 2. Primitivas Nativas de Transicao da `MatchFSM`

Duas funcoes limpas da propria Fox Engine governam quase todas as intervencoes VAR sem hacks de posicionamento manual:

1. **`MatchFSM_EnterOutOfPlay` (`0x1404541A0`, RVA `0x4541A0`)**:
   - Chamada automaticamente em `State_InPlay` (`0x14044A408`) e `State_SetPlayReady` (`0x14044E1C5`) sempre que `*(int32_t*)(MatchFSM + 0x49C0) != 0`.
   - Chama `0x1404829E0` (processamento de lesao/falta), reseta `Queue` com `MatchFSM_ResetQueue(MatchFSM, 1)` (`0x1404529F0`), empilha estado `1` (`0x14044DB80` -> `0x1404530C0`) e seta `*(MatchFSM + 0x2794) = 2` (`OutOfPlay`).
   - O estado `1` constroi toda a sequencia nativa: cutscene de falta/cartao (se houver cartao), setup de barreira (`0x17` se falta), posicionamento de cobrador/goleiro/jogadores (`0x16 -> 0x12 -> 0x13 -> [0x17] -> 0x14 -> 0x15 -> 0x0C/0x0E -> 0x34 -> 0x19 -> 0x1A`).

2. **`MatchFSM_RebuildSetPlay` (`0x140454280`, RVA `0x454280`)**:
   - Usada nativamente em `State_SetPlayReady` (`0x14044DD50` linhas 188, 199, 238) quando o usuario muda o cobrador ou ha substituicao durante uma bola parada.
   - Chama `MatchFSM_ResetQueue(MatchFSM, 1)` e empilha a sequencia limpa **sem cutscene de falta/replay**:
     `0x0F -> 0x16 -> 0x12 -> 0x13 -> [0x17 se SetPlayKind == 5] -> 0x14 -> 0x15 -> 0x0C -> 0x19 -> 0x1A`.
   - Ideal para reconfigurar uma bola parada ja existente (ex: converter falta <-> penalti ou aplicar Bola ao Chao / devolucao de posse em anulacao) com transicao rapida, deterministica e livre de soft-lock.

3. **`MatchFSM_EnterInPlay` (`0x140453F10`, RVA `0x453F10`)**:
   - Transiciona `*(MatchFSM + 0x2794) = 1` (`InPlay`) a partir de `SetPlayReady` ou modos especiais. Nao deve ser chamada arbitrariamente no meio de cutscenes `0x03..0x11` sem desarmar camera/atores de bola parada, sob risco de travar camera fixa.

---

## 3. Analise Deterministica dos 4 Cenarios de Intervencao VAR

### Cenario 1: Alterar Cartao em Parada Existente (Nenhum / Amarelo / Vermelho)

- **Aplicar ou subir cartao (Nenhum -> Amarelo, Nenhum -> Vermelho, Amarelo -> Vermelho/2o Amarelo):**
  - **Viabilidade:** Alta (deterministico).
  - **Mecanica:** Atualizar `PlayerRecord[0]` (`yellows`) e `PlayerRecord[1]` (`reds`) via `0x140A3B5B0(TeamInfo, offenderSlot)`, sincronizar `FoulInfo.FoulJudgementKind` (`MatchInfo + 0x158C` e `MatchFSM + 0x49F8`), suprimir incremento duplicado no hook `0x1404231DC` via flag `var_suppress_card_commit`.
  - Se resultar em expulsao (`reds >= 1` ou `yellows >= 2`): definir `MatchInfo_SetPlayerOnPitch(MatchInfo, offenderSlot, 0)` (`0x140A3D0E0`) e chamar `MatchFSM_RebuildSetPlay` (`0x140454280`) para que o estado `0x13` (`0x1404485B0`) retire o ator de campo e acione a logica nativa de goleiro (`0x140446480`) caso o expulso seja goleiro.
- **Retirar cartao Amarelo (sem expulsao):**
  - **Viabilidade:** Alta (deterministico).
  - **Mecanica:** Decrementar `PlayerRecord[0]` (clamp `>= 0`), atualizar `FoulJudgementKind = 1` e limpar indicador em `PlayerStatus` (`GMATCH + 0x180`).
- **Retirar cartao Vermelho ou reduzir Vermelho -> Amarelo (Reintegracao de Expulso):**
  - **Viabilidade:** Alta antes do estado `0x13` (durante cutscenes `0x03..0x11`) e tratada com seguranca apos o estado `0x13` (`0x1A` SetPlayReady ou `0x1C` InPlay).
  - **Limitacao real da engine:** Antes de `0x13`, o ator 3D ainda esta em campo: zerar `PlayerRecord[1] = 0` e manter `PlayerOnPitch[slot] = 1` evita a remocao completamente. Apos `0x13`, o ator ja foi retirado da cena e o mod reintegra `PlayerRecord` + `PlayerOnPitch[slot] = 1` exibindo aviso claro de que o ator 3D retorna na proxima parada/reconstrucao.
  - **Regra de produto:** A opcao nunca e travada na UI, preservando controle total sobre cartoes de qualquer jogador.

### Cenario 2: Converter Falta <-> Penalti (com opcao de cartao) em Parada Existente

- **Viabilidade:** Alta (deterministico).
- **Falta -> Penalti:**
  1. Calcular coordenada exata da marca penal via `Judge_CalcPenaltySpot` (`0x14050DC00(outPos, 6, PitchDims, origPos)`).
  2. Atualizar `JudgeResult` (`MatchFSM + 0x49C0`) e `MatchInfo + 0x1554`: `SetPlayKind = 6`, `BenefitedTeam`, `SetPlayPosA/B = outPos`, `StoppageReason = 5`, `FoulKind = 2`, `FoulJudgementKind` (conforme decisao do usuario).
  3. Ajustar delta de cartao no `PlayerRecord` (evitando duplicacao via `0x1404231DC`).
  4. Disparar `MatchFSM_RebuildSetPlay` (`0x140454280`) (ou `MatchFSM_EnterOutOfPlay` `0x1404541A0` se houver novo cartao e o usuario desejar animacao de cartao). A engine posiciona batedor na marca penal, goleiro na linha e demais jogadores fora da area.
- **Penalti -> Falta fora da area:**
  1. Usar coordenada do contato gravada no ring buffer. Se `Pitch_ClassifyZone` (`0x140A73470`) ainda indicar bit 11 (dentro da area), projetar `(x, z)` 0.5m para fora da borda mais proxima da grande area.
  2. Atualizar `SetPlayKind = 5`, `SetPlayPosA/B = adjustedPos`, `StoppageReason = 5`.
  3. Ajustar delta de cartao no `PlayerRecord`.
  4. Disparar `MatchFSM_RebuildSetPlay` (`0x140454280`). A propria funcao detecta `SetPlayKind == 5` e insere o estado `0x17` (montagem de barreira) na fila.

### Cenario 3: Apitar Falta ou Penalti Retroativamente a Partir de Bola Rolando ("Nao Falta")

- **Viabilidade:** Muito Alta (100% deterministico, utiliza o fluxo oficial do `Judge`).
- **Mecanica:**
  1. Durante `InPlay`, o gravador passivo salva snapshots de jogo rolando (posicoes dos 22 jogadores + bola + pares de contato mais proximos + timestamp float 60Hz).
  2. No Replay Manual, o usuario pausa no frame exato do lance nao marcado, escolhe infrator/vitima (pre-sugeridos pelo contato mais proximo ou pelo foco da camera `ReplayMgr + 0x1C`) e seleciona o veredito (Falta simples / Falta + Amarelo / Falta + Vermelho / Penalti + cartao opcional).
  3. Ao sair do Replay e voltar a `State_InPlay` (`0x140449F40`), os hooks de entrada em `Judge::FinalizeFrame` (`0x14050CD70`) e `MatchFSM::MainTick` (`0x1404468F0`) preenchem `MatchFSM + 0x49C0` (`JudgeResult`) com `SetPlayKind = 5 ou 6`, `BenefitedTeam`, `SetPlayPosA/B`, `StoppageReason = 5`, `FoulInfo.IsValid = 1`, `FoulKind = 2`, `FoulJudgementKind = 1/3/4`, `OffenderSlot`, `VictimSlot`, sentinelas `0xFF` nos campos internos da engine e um `SnapshotTickIndex` valido garantido no pool `MatchInfo + 0x17AC` (`EnsureValidFoulSnapshotId`, eliminando qualquer risco de crash em `0x1404B4C25`).
  4. A propria Fox Engine aciona `MatchFSM_EnterOutOfPlay` (`0x1404541A0`), aplica o cartao, monta a bola parada e entrega o controle em `0x1A`.
- **Nota de escopo sobre gol ja validado apos o lance:** Se o usuario pausar antes da bola entrar (ainda em `InPlay`), o Cenario 3 funciona 100%. Se o gol ja foi validado (`StoppageReason == 3` e placar incrementado), anular gol requer reverter placar + historico de artilheiros (e evitar colisao com `goal_pyro.lua`), ficando documentado como extensao controlada de fase posterior apos validacao live do placar.

### Cenario 4: Anular Marcacao de Falta ou Penalti (Overturn / Cancel)

- **Analise de risco real da FSM:**
  - Forcar `MatchStatus = 1` (`0x140453F10`) no meio de `OutOfPlay` (`0x03..0x15`) ou `SetPlayReady` (`0x1A`) apresenta risco alto de soft-lock de camera/AI, pois a Fox Engine ja desativou a camera live e posicionou atores em modo de bola parada.
- **Mecanica 100% Deterministica e Fiel a Regra 8.2 da IFAB (Bola ao Chao / Posse Devolvida):**
  1. Reverter qualquer cartao aplicado pelo lance anulado (ajustando `PlayerRecord[0/1]` e `PlayerOnPitch`).
  2. Limpar `FoulInfo` (`IsValid = 0`, `FoulJudgementKind = 0`) e suprimir commit de cartao no hook `0x1404231DC`.
  3. Definir reinicio limpo devolvendo a posse ao time que sofreu a marcacao anulada (conforme protocolo oficial IFAB de Bola ao Chao apos revisao VAR):
     - **Penalti anulado:** `SetPlayKind = 4` (Tiro de Meta para o goleiro defensor) ou `SetPlayKind = 5` (Tiro livre defensivo dentro da area para o goleiro/defesa), `BenefitedTeam = time defensor`, `StoppageReason = 7` (reinicio sem cutscene de falta).
     - **Falta anulada:** `SetPlayKind = 5`, `BenefitedTeam = time que sofreu a marcacao anulada` (ou o time que tinha a posse no snapshot do apito), `SetPlayPosA/B` no local da bola, `StoppageReason = 7` (sem cutscene).
  4. Disparar `MatchFSM_RebuildSetPlay` (`0x140454280`). A engine realiza transicao curta e deterministica para `0x1A` com a bola nos pes da defesa, sem animacao de falta, sem cartao e com zero risco de soft-lock.
  5. **Alternativa de retomada instantanea em `0x1C`:** sera testada em sandbox na Fase 2 exclusivamente para pausas feitas nos primeiros frames do apito (antes de `0x12`), mas a **Bola ao Chao via `0x140454280`** sera o mecanismo padrao garantido.

## 4. Salvaguardas Anti-Crash e Anti-Soft-Lock

1. **Execucao exclusiva na thread de simulacao:** A UI ImGui (thread DX11) nunca escreve direto na `MatchFSM` ou `MatchInfo`. Ela enfileira um `VarCommand` atomico (`std::atomic<uint32_t> seq`), consumido apenas nos hook sites sincronos `0x14044A065` (`InPlay`) ou `0x14044E1B9` (`SetPlayReady`) / `0x1404468F0` (`MainTick`).
2. **Gate de estado valido:** Nunca injetar comandos se `GMATCH == 0`, `MatchInfo == 0`, `MatchInfo->SpecialModeKind != 0` (treino/tutorial), `MatchPhase >= 8` (disputa de penaltis / fim de jogo), ou se `ReplayMgr->Mode != 0` (espera sair completamente do Replay e da Pausa).
3. **Gate de modo offline:** O controlador Sider Lua bloqueia a ativacao se detectar modo online ou lobby.
4. **Anti-reentrancia e anti-duplicacao de cartao:** Flag de transacao `var_tx_active` garante que `MatchControl::CommitToRegistry` (`0x1404231DC`) nunca conte o mesmo cartao duas vezes ao reconstruir um SetPlay.
5. **Clamp de coordenadas:** Toda posicao injetada em `SetPlayPosA/B` passa por verificacao de limites do campo (`GMATCH + 0x2AA8`) e `Pitch_ClassifyZone` (`0x140A73470`).
