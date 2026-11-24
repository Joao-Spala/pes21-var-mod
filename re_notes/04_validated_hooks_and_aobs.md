# 04. Hook Sites Validados, Assinaturas AOB e Matriz de Colisao Sider/BMPES

Todos os 17 AOBs e hook sites abaixo foram validados em `PES2021.exe` (Steam 1.7.2.0) usando `pes2021-re:verify_hook_site` e `decompile_function`, confirmados por teste automatizado no binario real (`test_var_core.exe`) e cruzados contra os modulos ativos do Sider 7.3.3 e BMPES 16.3.

## 1. Enderecos Ocupados pelo BMPES 16.3 (Proibidos para Hook)

Extraido diretamente de `<Steam>\steamapps\common\BMPES\modules\goal_pyro.lua` e `PESCustomCelebrationsReload.lua`:

| VA | Modulo BMPES | Uso |
|---|---|---|
| `0x140423974` | `goal_pyro.lua` | `SOURCE_RECORD_SITE` dentro de `MatchControl::vfunc[6]` (`0x1404238F0`) |
| `0x140478C3F` | `goal_pyro.lua` | Call site `native_get_scorer` |
| `0x140478C52` | `goal_pyro.lua` | Hook `scorerpost` |
| `0x141AC7214` | `goal_pyro.lua` | Hook `SmokeBomb2` |
| `0x141AC9050` | `goal_pyro.lua` | Funcao auditada como original canonica |
| `0x141E2xxxx..0x142xxxxxx` | `PESCustomCelebrationsReload.lua` | Tabelas de celebracoes |

**Status de colisao do VAR Mod v4.0:** `0` colisoes com qualquer modulo BMPES ou Sider.

---

## 2. Arquitetura Final de Hooks Nativos (`var_core.dll` v4.0)

Para eliminar qualquer fragilidade de registradores volateis e garantir inicializacao instantanea (zero imports estaticos de `d3d11.dll` / `dxgi.dll`, zero dispositivo DX11 dummy), a v4.0 usa exclusivamente hooks de entrada de funcao Microsoft x64 ABI com escrita atomica (`InterlockedExchange64` / `cmpxchg16b`) e publicacao previa de trampolim com barreira de memoria.

### Hook 1: `MatchFSM::MainTick` (Monitoramento 60 Hz, Snapshots 10 Hz e Execucao Cenarios 1, 2 e 4)

- **VA / RVA:** `0x1404468F0` / `0x4468F0`
- **AOB unico (30 bytes):**
  `48 89 5C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60`
- **Stolen bytes (5 bytes, 0 NOPs):** `48 89 5C 24 20` (`mov qword ptr [rsp + 0x20], rbx`)
- **Conflitos Sider/BMPES:** nenhum (`[]`)
- **Papel no VAR Mod:** captura `MatchFSM*`, resolve `GMATCH` (`0x143702208`) em `MainTick + 0x30`, executa deteccao fisica a 60 Hz (`PlayerAction + 0x01 / +0x08` e `BallState + 0x2601C`), grava snapshots taticos a 10 Hz (90 s), sincroniza nomes reais (`#N NOME`), numeros de camisa, substituicoes e cores dos uniformes (licenciados e editados/BMPES) e aplica vereditos dos Cenarios 1, 2 e 4.

### Hook 2: `Judge::FinalizeFrame` (Injecao Nativa do Cenario 3 em `InPlay`)

- **VA / RVA:** `0x14050CD70` / `0x50CD70`
- **AOB unico (17 bytes):**
  `33 C0 48 89 81 B8 03 00 00 48 89 81 C0 03 00 00 C3`
- **Stolen bytes (9 bytes, 4 NOPs, `cmpxchg16b` em endereco alinhado a 16 bytes):** `33 C0 48 89 81 B8 03 00 00`
- **Conflitos Sider/BMPES:** nenhum (`[]`)
- **Papel no VAR Mod:** injeta `RawSetPlayInfo` + `RawFoulInfo` em `MatchFSM + 0x49C0` / `+0x49F0` durante `InPlay` (`0x1C`) para faltas e penaltis retroativos.

### Hook 3: `MatchControl::UpdateFoulCounters` (Auditoria Oficial e Anti-Duplicacao)

- **VA / RVA:** `0x140424410` / `0x424410`
- **AOB unico (16 bytes):**
  `41 80 B8 60 22 00 00 00 74 13 8B 82 44 01 00 00`
- **Stolen bytes (8 bytes, 3 NOPs):** `41 80 B8 60 22 00 00 00` (trampolim preserva integralmente `EFLAGS`/`ZF`)
- **Conflitos Sider/BMPES:** nenhum (`[]`)
- **Papel no VAR Mod:** registra eventos oficiais de falta, penalti e cartao (filtrando impedimento) e suprime duplicacao de contagem em rebuilds do VAR.

### Hook 4: `ManualReplayController::SyncToReplayManager` (Seek Nativo, Play/Pause e Foco de Camera 3D)

- **VA / RVA:** `0x1420C16A0` / `0x20C16A0` (vtable `0x142BDFD60`)
- **AOB unico (20 bytes):**
  `40 53 55 56 57 41 54 41 56 41 57 48 83 EC 40 48 8B 99 30 04`
- **Stolen bytes (5 bytes, 0 NOPs, zero RIP-relative):** `40 53 55 56 57` (`push rbx`, `push rbp`, `push rsi`, `push rdi`)
- **Conflitos Sider/BMPES:** nenhum (`[]`)
- **Papel no VAR Mod:** executa na propria thread do Replay, imediatamente antes de espelhar o estado para `ReplayManager`, comandos enfileirados pela interface:
  - **Seek nativo:** busca o frame exato na janela circular `[winStart..winEnd]` do `ReplaySingleton` (`0x143703558`, `+0x6A8 -> +0x40`, stride `0x1FB0`) compativel com a metade da partida, chama `ReplayPlayer::SetFrame` (`0x14217E800`), pausa opcionalmente (`ctrl + 0x5C = 0`, `ctrl + 0x310 = 0.0f`) e atualiza `LookAtPos` da camera 3D.
  - **Play/Pause:** alterna `ctrl + 0x5C` (`0`/`1`) e `ctrl + 0x310` (`0.0f`/`1.0f`).
  - **Foco de Camera 3D:** alterna `ctrl + 0x64D` (`FocusSelector`), `ctrl + 0x80` (`ReplayPlayer + 0x30`), sai de Free Cam se `ctrl + 0x64C == 3` e atualiza `ReplayCamera` (`+0x18 -> +0x00 / +0x44`) via `ReplayPlayer::GetFocusPos` (`0x14217EC70`).

### Hook 5: `GrSwapChain::Present0` (Overlay DX11 Nativo)

- **VA / RVA:** `0x141FF46A0` / `0x1FF46A0`
- **AOB unico (14 bytes):**
  `48 8B 49 20 45 33 C0 48 8B 01 48 FF 60 40`
- **Stolen bytes (7 bytes):** `48 8B 49 20 45 33 C0`
- **Papel no VAR Mod:** intercepta o `IDXGISwapChain*` real em `[rcx + 0x20]`, com fast-path lock-free fora do Replay e RTV transiente por frame (compativel com `ResizeBuffers`, Alt+Tab, Sider e Steam Overlay).

### Hooks de Input no Replay Manual (Bloqueio de Teclado/Mouse e Atalho `R3` no Controle)

- **`DInputKeyboard_Poll` (`0x141B638B0`, 5 bytes):** zera buffer de 256 bytes em `[self + 0x268]` enquanto o painel VAR esta focado ou na janela de graca de 260 ms.
- **`DInputMouse_Poll` (`0x141B63950`, 5 bytes):** zera deltas e botoes em `DIMOUSESTATE` (`[self + 0x268]`) enquanto o painel VAR esta focado.
- **`DInputJoystick_Poll` (`0x141B63900`, 5 bytes) e `PesPad_Update` (`0x1403F86E0`, 8 bytes):** repassam o estado original sem zerar o gamepad, mantendo o controle livre para operar o Replay 3D enquanto teclado e mouse operam o painel.
- **IAT Hooks (`xinput1_3.dll` ordinal `2` e `user32.dll!GetKeyboardState`):** leem o atalho configuravel do controle (`R3` para abrir/recolher o painel) e bloqueiam `GetKeyboardState` no jogo durante foco do painel sem afetar o overlay do Sider.

---

## 3. Funcoes Nativas e Globais Resolvidos por AOB (17/17 Validados)

| Simbolo Interno | VA | RVA | Uso |
|---|---|---|---|
| `MatchFSM::MainTick` | `0x1404468F0` | `0x4468F0` | Hook principal de partida + `GMATCH` (`+0x30` -> `0x143702208`) |
| `Judge::FinalizeFrame` | `0x14050CD70` | `0x50CD70` | Hook Cenario 3 em `InPlay` |
| `MatchControl::UpdateFoulCounters` | `0x140424410` | `0x424410` | Hook auditoria oficial de faltas/cartoes |
| `ManualReplayController::SyncToReplayManager` | `0x1420C16A0` | `0x20C16A0` | Hook nativo de Seek/Play/Camera + `ReplaySingleton` (`+0x32` -> `0x143703558`) |
| `ReplayPlayer::SetFrame` | `0x14217E800` | `0x217E800` | Salto circular de frame no Replay 3D |
| `ReplayPlayer::GetFocusPos` | `0x14217EC70` | `0x217EC70` | Posicao 3D de bola (0) ou jogador (1..22) no frame atual |
| `MatchFSM_EnterOutOfPlay` | `0x1404541A0` | `0x4541A0` | Entrar em parada completa (cutscene + setplay) |
| `MatchFSM_RebuildSetPlay` | `0x140454280` | `0x454280` | Reconstruir setplay limpo sem cutscene |
| `MatchInfo_GetTimeFloat` | `0x140A3C680` | `0xA3C680` | Tempo atual da metade em float 60 Hz |
| `MatchInfo_SetPlayerOnPitch` | `0x140A3D0E0` | `0xA3D0E0` | Atualizar `MatchInfo + 0x1748[slot]` |
| `TeamInfo_GetPlayerBySlot` | `0x140A3B5B0` | `0xA3B5B0` | Obter `PlayerRecord*` via call rel32 em `0x1404231DC + 6` |
| `MatchRosterGetter` | `0x1414BC550` | `0x14BC550` | Resolver `MatchRoster*` (`0x143714240`: nomes, camisas, times e uniformes) |
| `GrSwapChain::Present0` | `0x141FF46A0` | `0x1FF46A0` | Hook DX11 nativo sem dummy device |
| `DInputKeyboard_Poll` | `0x141B638B0` | `0x1B638B0` | Bloqueio nativo de teclado no Replay quando o painel esta focado |
| `DInputJoystick_Poll` | `0x141B63900` | `0x1B63900` | Pass-through limpo do joystick DInput para controle livre do Replay |
| `DInputMouse_Poll` | `0x141B63950` | `0x1B63950` | Bloqueio nativo de mouse DInput no Replay quando o painel esta focado |
| `PesPad_Update` | `0x1403F86E0` | `0x3F86E0` | Pass-through do gamepad unificado mantendo XInput/DInput ativos |
