# 03. Replay Manager, Estado de Pausa, Menu do BMPES e Integracao DX11

Mapeamento tecnico do subsistema de Replay (`GMATCH + 0x388`), tela de Replay Manual (`Replay@Screen@match2D`), menu de Pausa (`MatchPause@menu`) e interacao com o ecossistema BMPES 16.3 e Sider 7.3.3.

## 1. Layout Verificado de `ReplayManager` (`*(GMATCH + 0x388)`)

Confirmado por decompilacao de `Replay@Screen@match2D::Init` (`0x1420F3D40`), `Replay@Screen@match2D::Update` (`0x1420F4580`), `ObserverReplay` (`0x140A14CE0`) e getters `0x140A86C40..0x140A86C80`:

| Offset | Tipo | Campo | Semantica verificada |
|---|---|---|---|
| `+0x00` | `uint8_t` | `ReplayMode` | `0` = inativo. `1..4` = replay ativo (`CheckReplayPlayback` `0x140A86C80`: `(mode - 1) <= 3`). Getters: `0x140A86C50` (`==1`), `0x140A86C70` (`==2`), `0x140A86C40` (`==3`), `0x140A86C60` (`==4`) |
| `+0x07` | `uint8_t` | `ManualReplayScreenFlag` | **`!= 0` enquanto a tela interativa de Replay Manual esta ativa.** `0x1420F4580` linha 47 fecha a tela se `*(+0x07) == 0` ou `*(+0x00) == 4` |
| `+0x0B` | `uint8_t` | `UiHiddenOrFreeCam` | `!= 0` oculta barra de nome (`nameBarVisible_off` em `0x1420F4580` linha 59) |
| `+0x0A` | `uint8_t` | `IsPlaying` | `1` = reproduzindo, `0` = pausado (copiado de `ReplayController + 0x5C` em `0x1420C1827`) |
| `+0x10` | `float` | `PlaybackSpeed` | Velocidade atual de reproducao (copiada de `ReplayController + 0x310`) |
| `+0x14` | `uint32_t` | `TimelinePos` | Frame relativo atual (`(cur - start + cap) % cap`, gravado por `mov dword ptr [rbx+0x14], eax` em `0x1420C1788`) |
| `+0x18` | `uint32_t` | `TimelineDuration` | Total de frames da janela (`(end - start + cap2) % cap2`, gravado por `mov dword ptr [rbx+0x18], eax` em `0x1420C17AA`) |
| `+0x1C` | `int32_t` | `CameraFocusSlot` | `0` = Bola, `1..11` = Jogadores Home, `12..22` = Jogadores Away (`0x1420F4580` linhas 110-135) |
| `+0x20` | `uint64_t` | `CameraFocusPlayerId` | High dword (`+0x24`) contem o `PlayerID` usado para buscar o nome exibido em `name_str` |
| `+0x48` | `float` | `WindowStartTime` | Inicio da janela de replay em ticks de 60Hz da metade atual |
| `+0x4C` | `int32_t` | `WindowStartPhase` | `MatchPhase` do inicio da janela |
| `+0x50` | `float` | `WindowEndTime` | Fim da janela de replay em ticks de 60Hz |
| `+0x54` | `int32_t` | `WindowEndPhase` | `MatchPhase` do fim da janela |
| `+0x58` | `float` | `CurrentCursorTime` | **Cursor atual do Replay em tempo real** (ticks de 60Hz da metade atual), atualizado durante scrub, slow-motion e rewind (`0x1420F4580` linhas 204-219) |
| `+0x5C` | `int32_t` | `CurrentCursorPhase` | **`MatchPhase` do cursor atual do Replay** |
| `+0x60/+0x64` | `float/int32` | `SubRangeStart` | Inicio do sub-intervalo visivel na barra |
| `+0x68/+0x6C` | `float/int32` | `SubRangeEnd` | Fim do sub-intervalo visivel na barra |

### 1.1. Sincronizacao Exata entre Cursor do Replay e Ring Buffer do VAR

Verificado em `0x1420C6760` e `0x140A6C3B0`:
- `0x140A6C3B0` converte `float` via `cvttss2si` e divide por `60` (`mul 0x88888889; shr edx, 5`) para obter segundos totais, depois divide por `60` para obter minutos e segundos.
- `0x1420C6760` soma `+45` minutos para `MatchPhase` `2/3` (2o tempo), `+90` para `4/5` (1a prorrogacao) e `+105` para `6/7` (2a prorrogacao).
- `MatchInfo_GetTimeFloat` (`0x140A3C680(MatchInfo + 0x1168)`) retorna um `float` **na mesma unidade exata de 60Hz da metade atual**.
- **Resultado:** Ao gravar cada evento e snapshot de jogo rolando com a chave `(MatchPhase, MatchTimeFloat, MatchTick)`, o mod sincroniza o cursor do Replay (`+0x5C`, `+0x58`) com o historico gravado com precisao de **1 frame (~16.6 ms)**, sem heuristica aproximada.

### 1.2. Condicao Exata de Deteccao do Replay Manual

Para distinguir **Replay Manual** (acionado pelo usuario via menu de pausa) de **Replay Automatico** (gol/falta/highlight):
- **Condicao primaria verificada:**
  `ReplayMgr != nullptr && ReplayMgr->Mode >= 1 && ReplayMgr->Mode <= 3 && ReplayMgr->ManualReplayScreenFlag (+0x07) != 0`
- **Validacao complementar `[Validar Live]`:** confirmar em partida real com Cheat Engine os valores exatos de `ReplayMgr+0x00` (`1` vs `2` vs `3` vs `4`) e `ReplayMgr+0x07` nos 3 contextos (Replay Manual da Pausa, Replay Automatico de Gol/Falta, Melhores Momentos pos-jogo) para garantir que o HUD VAR nunca apareca em replays automaticos indesejados.

---

## 2. Analise de `MatchPause@menu` e Menus do BMPES 16.3

### 2.1. Evidencia no Binario (`MatchPause@menu` vtable `0x1425BCDB8`)
- `MatchPause@menu::vfunc[1]` (`0x140E3DFC0`) e `vfunc[2]` (`0x140E40ED0`) gerenciam eventos de janela de menu (`0x1080001..0x1080009`) dirigidos por scripts de fluxo (`common\script\flow\Match\MatchPrePause.json` e `MatchPostPause.json`) e arquivos binarios de layout (`common\menu\general\game2dReplay.bin` e telas de pausa).
- O Sider documenta em `doc\scripting.html` que esses fluxos passam por `livecpk_data_ready`.

### 2.2. Evidencia no BMPES 16.3 (`modules\MenuServer.lua`)
Inspecao direta de `<Steam>\steamapps\common\BMPES\modules\MenuServer.lua` (linhas 9 e 104-118) e pastas do BMPES:
- `MenuServer.lua` intercepta e substitui dinamicamente por competicao (`map_competitions.txt`) todos os arquivos sob:
  - `common\menu\system`
  - `common\menu\general`
  - `common\script\flow`
- O patch tambem inclui `livecpk\Estilos de Menus` e `modules\MOD_MENU_SEM_LOBBIES.lua`.

### 2.3. Veredito Tecnico: Botao no Menu de Pausa vs Integracao Direta na Tela de Replay
- **Injetar botao extra no Menu de Pausa nativo: NAO RECOMENDADO.**
  - Patchear arquivos `.bin`/`.json` de menu entraria em conflito direto com dezenas de temas do `Servidor de Menus` e `Estilos de Menus` do BMPES 16.3, quebrando atualizacoes do patch e layouts por campeonato.
  - Hackear o vetor interno de itens em `MatchPause@menu+0x88` exige recriar handlers de foco, localizacao de texto e transicao de `ProcessMatchReplay` (`0x1412FF730`), adicionando risco real de soft-lock no menu de pausa sem ganho funcional, ja que o usuario ainda precisa abrir o Replay para ver o lance.
- **Integrar diretamente na Tela de Replay Manual: RECOMENDADO (100% seguro e nativo).**
  - Fluxo natural: usuario pausa -> entra no **Replay** nativo -> usa livremente camera, angulo, slow-motion e timeline do jogo -> aciona o painel VAR discreto (badge -> painel completo via `SELECT/Back` ou `F9`) -> escolhe o veredito -> sai do Replay normalmente para aplicar a decisao no campo.
  - Zero alteracao em qualquer arquivo de menu do BMPES ou CPK do jogo.

---

## 3. Coexistencia com Sider 7.3.3 (DX11 Overlay e Input)

Inspecao dos imports e strings de `<Steam>\steamapps\common\BMPES\sider.dll`:
- **Graficos:** Sider 7.3.3 faz hook em `IDXGISwapChain::Present` para renderizar sua faixa superior de overlay (`overlay.enabled = 1`).
- **Input:** Sider importa `XINPUT9_1_0.dll` (`XInputGetState`), `DINPUT8.dll` (`DirectInput8Create`) e `USER32.dll` (`SetWindowsHookExW`).
- **Ordem de inicializacao e encadeamento seguro:**
  - `var_mod.lua` carrega `var_core.dll` durante o `init(ctx)` do Sider (apos o Sider ja ter se anexado ao processo).
  - `var_core.dll` cria um dummy `IDXGISwapChain` temporario (ou captura o `SwapChain` real da janela do PES 2021) para obter o endereco da vtable de `Present` (`[8]`) e `ResizeBuffers` (`[13]`) e instala MinHook **encadeando** para o `oPresent` anterior (que preserva integralmente o overlay do Sider).
  - Em `ResizeBuffers`, libera e recria o `ID3D11RenderTargetView` do ImGui de forma limpa, garantindo suporte a troca de resolucao, Alt+Tab e fullscreen/borderless sem crash.
  - Se o usuario abrir o overlay do Sider (`Espaco` / `overlay_on`), `var_mod.lua` notifica `var_core.dll` para ceder foco de teclado/gamepad temporariamente.
