# 01. GMATCH, MatchInfo, Arbitragem e Cartoes (PES2021.exe Steam 1.7.2.0)

Documento de referencia tecnica com enderecos, RVAs, VTables e layouts de structs verificados por decompilacao estatica (`pes2021-re` / Ghidra) e disassembly Capstone no executavel `<Steam>\steamapps\common\eFootball PES 2021\PES2021.exe` (`ImageBase = 0x140000000`).

> Status de verificacao: enderecos, offsets, prologos de hook e layout da tabela `MatchRoster` confirmados por decompilacao estatica (`pes2021-re` / Ghidra) e validados ao vivo em memoria com Cheat Engine contra `PES2021.exe` em execucao.

## 1. Raiz Global da Partida (`GMATCH`)

- **Ponteiro global:** `GMATCH = *(uint64_t*)0x143702208` (RVA `0x3702208`)
- **Ponteiro auxiliar de dados da partida (nomes de jogadores / roster):** retornado por `0x1414BC550()` (`*(ret + 0x50) + 0x35960`, verificado em `0x1420F4580`)
- **Singletons auxiliares de replay:** `0x143703558` e `0x143703568` (usados por `Replay@match` e `MatchFSM`)

### Campos verificados em `GMATCH`

| Offset | Tipo | Descricao | Evidencia |
|---|---|---|---|
| `+0x18` | `void*` | Configuracoes / regras de partida (`+0x1038` flags de time) | `0x140452F00`, `0x1420D46A0` |
| `+0x40` | `Ball*` | Objeto da bola (`0x140A57ED0` retorna `Vec3*` posicao, `+0x27B8` pos anterior, `+0x2920` estado) | `0x14050F120`, `0x140445740`, `0x14050EE50` |
| `+0x180` | `PlayerStatus*` | Array de 24 entradas (`0x18`), stride `0x74` (controladores/slots humanos e CPU) | `0x140422A3F`, `0x140448D70`, `0x1405B8A10` |
| `+0x1A8` | `void*` | Controlador de transicao/camera (`0x140A60D10`, `0x140A60C40`) | `0x140446DF0`, `0x14044C310` |
| `+0x270` | `MatchInfo*` | Registro publico de estado da partida, bola parada, falta e vantagem | `0x140422A3F`, `0x1404530C0`, `0x140A141F0` |
| `+0x298` | `void*` | Controlador de pausa / solicitacao de pausa (`0x140A67E60`) | `0x140444F90` |
| `+0x2C0` | `void*` | Regras de competicao (`+0x17` limite de substituicoes/expulsoes, `0x140A3FBB0`) | `0x140452F00`, `0x1405B7E80` |
| `+0x310` | `void*` | Controlador de disputa de penaltis (shootout, `0x140A6A9F0`) | `0x14050BE40`, `0x14050D5F0` |
| `+0x388` | `ReplayManager*` | Estado, modo, foco de camera e timeline do Replay | `0x1420F3D40`, `0x1420F4580`, `0x140A86C80` |
| `+0x3B0` | `void*` | Flag/controlador de modo treino / tutorial (`0x140A436E0`) | `0x14044DB80`, `0x14050F120` |
| `+0x400` | `void*` | Controlador de modo especial / replay demo (`0x140A59E20`) | `0x14044DB80`, `0x1404541A0` |
| `+0x1928` | `TeamTactics*[2]` | `GMATCH_GetTeamTactics`: `(idx + 0xA1) * 0x28` (`+0x254` direcao de ataque `float` `+1.0/-1.0`) | `0x140A49020` |
| `+0x1978` | `TeamInfo*[2]` | `GMATCH_GetTeamInfo`: `(idx + 0xA3) * 0x28` (`+0x04` `int32_t[11]` rosterIdx ativo, `+0xA8 + idx * 0x128` `PlayerRecord`) | `0x140A48FC0`, `0x140A3B500` |
| `+0x19C8` | `PlayerAction*[22]` | `GMATCH_GetPlayerAction`: `(idx + 0xA5) * 0x28` (`+0x01` actionId: 13=Carrinho, 11/12=Bote, 15=Queda, 56/61=Carga e `+0x08` bit 17 = hit) | `0x140A48F00`, `0x1405B9400` |
| `+0x1DD8` | `PlayerSpatial*[22]` | `GMATCH_GetPlayerSpatial`: `(idx + 0xBF) * 0x28` (`+0x55C` pos Vec3) | `0x140A49000` |
| `+0x25F8` | `PlayerEntity*[22]` | `GMATCH_GetPlayerEntity`: `(idx + 0xF3) * 0x28` | `0x140A48FE0` |
| `+0x2AA8` | `float*` | Dimensoes do gramado (`[0]` meia largura/comprimento, `[1]` linha lateral, `[4..8]` area/marca penal) | `0x14050F120`, `0x1405B8A10`, `0x14050CE40` |

### Getters nativos baseados em `GMATCH`

| Funcao | VA | RVA | Assinatura / Efeito |
|---|---|---|---|
| `GMATCH_GetTeamInfo` | `0x140A48FC0` | `0xA48FC0` | `TeamInfo* (GMATCH*, int teamIdx)` (`0` = Home, `1` = Away) |
| `GMATCH_GetTeamTactics` | `0x140A49020` | `0xA49020` | `TeamTactics* (GMATCH*, int teamIdx)` (`+0x254` direcao de ataque `float` `+1.0/-1.0`) |
| `GMATCH_GetPlayerEntity` | `0x140A48FE0` | `0xA48FE0` | `PlayerEntity* (GMATCH*, uint32_t slot)` (`slot 0..21`) |
| `GMATCH_GetPlayerAction` | `0x140A48F00` | `0xA48F00` | `PlayerAction* (GMATCH*, uint32_t slot)` |
| `GMATCH_GetPlayerSpatial` | `0x140A49000` | `0xA49000` | `PlayerSpatial* (GMATCH*, uint32_t slot)` (`+0x55C` pos Vec3) |

---

## 2. VTables Resolvidas (MSVC x64 RTTI)

| Classe RTTI | VTable VA | Metodos relevantes |
|---|---|---|
| `Referee@referee@match` | `0x14264AF18` | Ator visual do arbitro em campo |
| `RefereeChief@referee@match` | `0x14264B190` | `[2]=0x140A36D90`, `[4]=0x140A37120`, `[8]=0x140A37A20` |
| `Judge@ai@match` | `0x1425CE1A0` | Embutido em `MatchFSM + 0x908`. Logica de regras e falta |
| `MatchControl@match` | `0x1425A15A0` | `[6]=0x1404238F0`, `[8]=0x140424390`, `[9]=0x140422670` |
| `Match@match` | `0x14259FEA8` | Controlador principal da partida |
| `ObserverFoul@record@match` | `0x1426498F0` | `[1]=0x140A141F0` (le `MatchInfo + 0x1584`, `+0x11BB`, `+0x135C`) |
| `ObserverPenaltyKick@record@match` | `0x142649940` | `[1]=0x140A14BA0` |
| `ObserverFreeKickKind@record@match` | `0x142649878` | `[1]=0x140A14350` (le `MatchInfo + 0x1480`) |
| `ObserverRestart@record@match` | `0x142649918` | `[1]=0x140A14F40` (le `MatchInfo + 0x11D4`) |
| `ObserverOutOfPlay@record@match` | `0x142649118` | `[1]=0x140A14AE0` (le `MatchInfo + 0x1554`) |
| `ObserverReplay@record@match` | `0x142649990` | `[1]=0x140A14CE0` (le `ReplayManager + 0x00`) |
| `ObserverTackle@record@match` | `0x14264A170` | `[1]=0x140A1B120` |
| `ModelYellowCard@Screen@match2D` | `0x142BEBD40` | `[8]=0x1420EAC40` (UI de cartao amarelo) |

---

## 3. Layout de `MatchInfo` (`*(GMATCH + 0x270)`)

| Offset | Tipo | Campo | Semantica verificada |
|---|---|---|---|
| `+0x1168` | `int32_t` | `MatchPhase` | `0/1` = 1T (+0m), `2/3` = 2T (+45m), `4/5` = ET1 (+90m), `6/7` = ET2 (+105m), `8/9/10` = Penaltis / Fim |
| `+0x116C` | `int32_t` | `MatchStatus` | `1` = `InPlay` (bola rolando), `2` = `OutOfPlay` (parada/cutscene), `3` = `SetPlayReady` (esperando cobranca) |
| `+0x1174` | `float` | `PhaseElapsedTime` | Base usada por `0x140A3C680(MatchInfo + 0x1168)` (retorna float em ticks 60Hz da metade atual) |
| `+0x1198` | `uint32_t` | `MatchTick` | Contador monotono de frames da simulacao (60Hz) |
| `+0x11B0` | `uint8_t` | `RestartSyncPhase` | Usado na sincronizacao dos estados `0x14` (`0x14`) e `0x15` (`0x15/0x16`) |
| `+0x11BB` | `uint8_t` | `FoulNotifyFlagA` | Lido por `ObserverFoul` (`0x140A141F0`) |
| `+0x11D4` | `int32_t` | `RestartSubState` | `== 2` dispara `ObserverRestart` |
| `+0x135C` | `uint8_t` | `FoulNotifyFlagB` | Lido por `ObserverFoul` (`0x140A141F0`) |
| `+0x1480` | `int32_t` | `FreeKickKind` | Tipo de falta (direta/indireta), lido por `ObserverFreeKickKind` |
| `+0x1544` | `16B` | `AdvantageInfo` | `+0x1548` e `+0x154C` verificados por `CheckAdvantageData` (`0x14145F360`) |
| `+0x1554` | `0x30B` | `SetPlayInfo` | Bloco principal da parada / bola parada (ver tabela 3.1) |
| `+0x1584` | `0x3C B` | `FoulInfo` | Bloco principal da falta / cartao (ver tabela 3.2) |
| `+0x15C0` | `0xCC B` | `GoalAndCardExtra` | Informacoes adicionais de gol/artilheiro (`+0x15C0..0x168B`) |
| `+0x1690` | `uint8_t` | `DemoSceneKind` | `0x13` = sem demo especial, usado em `0x140444A80` e `0x1404530C0` |
| `+0x173C` | `int32_t[2]` | `KickerSlots` | Slots `0..21` dos cobradores designados (primario/secundario) |
| `+0x1748` | `uint8_t[22]` | `PlayerOnPitch` | `1` = jogador ativo em campo, `0` = expulso/ausente. Escrito via `0x140A3D0E0(MatchInfo, slot, bool)` |
| `+0x175E` | `uint8_t[22]` | `PlayerInjuredOff` | `1` = fora de campo temporariamente por lesao (`0x14044B430`, `0x14044CA20`) |
| `+0x1775` | `uint8_t` | `SpecialModeKind` | `0` = partida normal. Valores `> 0` = treino, tutorial ou cenarios especiais |
| `+0x17AC` | `RingBuf` | `FoulSnapshotPool` | Pool de 3 snapshots de posicoes dos 22 jogadores (stride `0x3E4`, entrada `0x2C`), bola (`+0x3CC`) e IDs em `+0x2358` (`0x17AC + 3 * 0x3E4`), indexado por `FoulInfo+0x28` (`0x1404B4B40` / `0x140A82120`) |

### 3.1. Layout de `SetPlayInfo` (`MatchInfo + 0x1554` / `JudgeResult + 0x00`)

| Offset em `JudgeResult` | Offset em `MatchInfo` | Tipo | Campo | Valores verificados |
|---|---|---|---|---|
| `+0x00` | `+0x1554` | `int32_t` | `SetPlayKind` | `0`=Nenhum, `1`=Kickoff, `2`=Lateral (Throw-In), `3`=Escanteio (Corner), `4`=Tiro de Meta (Goal Kick), `5`=Falta (Free Kick), `6`=Penalti (Penalty Kick), `7`=Apito Fim de Tempo/Jogo |
| `+0x04` | `+0x1558` | `int32_t` | `BenefitedTeam` | `0` = Home, `1` = Away |
| `+0x08` | `+0x155C` | `float[3]` | `SetPlayPosA` | Coordenada 3D `(x, y, z)` gravada por `0x140A3D330` |
| `+0x14` | `+0x1568` | `float[3]` | `SetPlayPosB` | Espelho da coordenada 3D gravada por `0x140A3D330` |
| `+0x20` | `+0x1574` | `int32_t` | `StoppageReason` | `0`=Kickoff/Init, `1`=Fim de Tempo, `2`=Quick/Out, `3`=Gol, `4`=Saida de Bola, `5`=Falta, `6`=RebuildSetPlay, `7`=Reinicio direto sem cutscene, `8/9`=Shootout/Especial, `10`=Especial |
| `+0x28` | `+0x157C` | `uint32_t` | `EventPlayerSlot` | Slot `0..21` ou `0xFF` |

### 3.2. Layout de `FoulInfo` (`MatchInfo + 0x1584` / `JudgeResult + 0x30`)

| Offset em `JudgeResult` | Offset em `MatchInfo` | Tipo | Campo | Valores verificados |
|---|---|---|---|---|
| `+0x30` | `+0x1584` | `uint8_t` | `IsValid` | `1` quando ha evento de falta valido (`0x1405B7E80`) |
| `+0x34` | `+0x1588` | `int32_t` | `FoulKind` | `1` = Impedimento (Offside), `2` = Falta comum (Standard Foul), `3` = Simulacao/Antidesportiva (forca amarelo), `4` = Mao/Indireta (`0x14050F640`) |
| `+0x38` | `+0x158C` | `int32_t` | `FoulJudgementKind` | `0`=Nenhum, `1`=Falta simples (sem cartao), `2`=Advertencia verbal, `3`=**Cartao Amarelo**, `4`=**Cartao Vermelho Direto** |
| `+0x3C` | `+0x1590` | `int32_t` | `OffenderSlot` | Slot do infrator: `0..10` = Home, `11..21` = Away (`0xFF` se invalido) |
| `+0x40` | `+0x1594` | `int32_t` | `VictimSlot` | Slot da vitima: `0..21` (`0xFF` se invalido) |
| `+0x50` | `+0x15A4` | `uint8_t` | `WasAdvantageFoul` | `0` = falta imediata, `1` = falta apitada apos vantagem frustrada (`0x14050BE40`) |
| `+0x51` | `+0x15A5` | `uint8_t` | `GoalOnAdvantage` | `!= 0` redireciona parada de falta para ramo de gol em `0x1404530C0` |
| `+0x58` | `+0x15AC` | `uint32_t` | `SnapshotTickIndex` | Indice do snapshot no ring buffer `MatchInfo + 0x17AC` (`0x140A82070` / `0x140A82120`) |
| `+0x5C` | `+0x15B0` | `uint32_t` | `ContactCategory` | Categoria do contato (`0..8`) gravada por `0x1405B7E80` |

---

## 4. Pipeline de Decisao de Arbitragem e Cartoes

### 4.1. Fluxo no `Judge@ai@match` (`MatchFSM + 0x908`)

1. Em `MatchFSM::State_InPlay` (`0x140449F40`), a cada frame ao vivo executa:
   - `0x14050BC60(Judge, MatchFSM + 0x49C0, ctx)` (vincula ponteiro `*(Judge + 0x3C0) = MatchFSM + 0x49C0`)
   - `0x14050CD90(Judge)` (`Judge::Evaluate`): chama `0x14050DFC0` e, durante jogo normal, `0x14050BE40(Judge)`
   - `0x14050CD70(Judge)` (finalizacao do frame do Judge)
2. Quando ocorre evento de colisao/falta (`0x0F` em `0x14050EE50`), chama `0x1405B7E80(Judge, offenderSlot, victimSlot)` -> `0x1405B8A10`:
   - Classifica zona do campo via `Pitch_ClassifyZone` (`0x140A73470(PitchDims, x, z, &attackDir)`):
     - **Bit 11 (`(flags >> 11) & 1`) == 1:** lance dentro da grande area -> `SetPlayKind = 6` (Penalti), coordenada obtida via `Judge_CalcPenaltySpot` (`0x14050DC00(outVec3, 6, PitchDims, posVec3)`).
     - **Bit 11 == 0:** lance fora da grande area -> `SetPlayKind = 5` (Falta), mantendo a coordenada `(x, y, z)` do contato.
   - Grava o candidato em `Judge + 0x14` (imediato) ou promove para `Judge + 0x58` (vantagem ativa, contador regressivo em `Judge + 0x0C`).
3. Quando o arbitro apita a falta (`0x14050BE40`), chama `0x14050F640(Judge, candidate)` -> `0x14050F120` (`Judge::CommitDecision`):
   - Preenche `JudgeResult` (`MatchFSM + 0x49C0`) com `SetPlayInfo` e `FoulInfo`.
   - **Nota critica de seguranca binaria:** `0x14050F640` e `0x14050F890` contem constantes ofuscadas via XOR em regioes fora de `.text` (`0x14BC236xx` / `0x143FA9xxx`). Nunca instalar hooks nem patchear dentro dessas duas funcoes. Usar sempre os pontos limpos em `.text` dentro de `MatchFSM` e `MatchControl`.

### 4.2. Registro Oficial de Cartoes e Expulsao (`MatchControl::CommitToRegistry`)

No commit de frame em `MatchControl::CommitToRegistry` (`0x140422A3F`, codigo nativo limpo em `.text`):
1. Copia `MatchControl + 0x2910` (`SetPlayInfo`), `+0x2940` (`FoulInfo`), `+0x297C` (`AdvantageInfo`) e `+0x298C` para `MatchInfo`.
2. Atualiza a tabela `MatchInfo + 0x1748[0..21]` (`PlayerOnPitch`) via loop em `0x140423142..0x14042316E` chamando `0x140A3D0E0(MatchInfo, slot, flag)`.
3. Em **`0x1404231DC..0x140423205`** aplica os cartoes diretamente no `PlayerRecord` do time infrator:
   - Resolve `PlayerRecord*` via `TeamInfo_GetPlayerBySlot` (`0x140A3B5B0(teamInfo, offenderSlot)`).
   - `PlayerRecord_GetCards` (`0x141032A40`) retorna `rcx` diretamente (`mov rax, rcx; ret`), comprovando que:
     - `*(uint8_t*)(PlayerRecord + 0x00)` = contador de **Cartoes Amarelos** do jogador na partida.
     - `*(uint8_t*)(PlayerRecord + 0x01)` = contador de **Cartoes Vermelhos** do jogador na partida.
   - Logica exata em `0x1404231EF..0x140423205`:
     - Se `FoulJudgementKind == 3` (Amarelo): `yellows++`. Se `yellows >= 2`, tambem executa `reds++`.
     - Se `FoulJudgementKind == 4` (Vermelho direto): `reds++`.
4. Todas as leituras de elegibilidade/expulsao no motor (ex: `0x140444C40`, `0x140452F00` linhas 98-105 e `0x140444ED2`) checam consistentemente `(reds != 0) || (yellows >= 2)`.
5. Cartao amarelo tardio pos-vantagem: implementado nativamente pelo estado `0x20` (`0x14044B9D0`), que remonta `FoulInfo` em `MatchFSM + 0x49F0` com `FoulKind = 2`, `FoulJudgementKind = 3` na parada subsequente.

### 4.3. Layout Validado de `MatchRoster` (Times, Uniformes, Camisas e Jogadores)

Verificado em `0x1414BC550`, `0x1414C6A30`, `0x1414C9580` e validado ao vivo em partidas reais (BMPES 16.3):
- `matchRoster = *(uint64_t*)0x143714240` (retornado por `0x1414BC550()`)
- `dataPtr = *(uint64_t*)(matchRoster + 0x50)` e `rosterTable = dataPtr + 0x35960`
- `editedKitPtr = *(uint64_t*)(matchRoster + 0x58)` e `balNameString = matchRoster + 0x98` (`std::string` MSVC)
- **Entrada por time (`teamEntry = rosterTable + 0x138 + teamIdx * 0x690`):**
  - `+0x00`: `uint32_t TeamId`
  - `+0x04`: `char[70] FullName` (UTF-8)
  - `+0x4A`: `char[16] ShortName` (sigla de 3 letras do placar)
  - `+0x29A`: `uint16_t[40] ShirtNumbers` (numeros de camisa na mesma ordem das 40 entradas de jogadores)
  - `+0x5D8`: `KitTable` licenciada (10 entradas de 8 bytes, RGB primario em `+2..+4`, secundario em `+5..+7`). Para uniformes editados/BMPES, `editedKitPtr + teamIdx * 0x108` traz RGB primario em `+0x1991` e secundario em `+0x1994`.
- **Entrada por jogador (`entry = rosterTable + 0x1308 + teamIdx * 0x3D40 + i * 0x188`, ate 40 por time):**
  - `+0x2C`: `uint64_t PlayerKey` (`key >> 32` casa com `*(uint64_t*)(PlayerRecord + 0x30) >> 32`, com fallback em `TeamInfo + 0x04` para substituicoes ao vivo)
  - `+0x38`: `DisplayName` UTF-8
  - `+0x75`: `ShirtName`
  - `+0x144`: `int32_t BalAvatarFlag` (`< 0` indica avatar do Rumo ao Estrelato, cujo nome vem de `matchRoster + 0x98`)
- Complementarmente, `ReplayMgr + 0x1C` (slot `1..22`) e `ReplayMgr + 0x20` (`PlayerKey`) informam em tempo real qual jogador esta focado na camera de Replay.
