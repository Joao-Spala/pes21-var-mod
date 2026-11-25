# PES 2021 - VAR Mod v4.2 (PT-BR & EN)

![Version](https://img.shields.io/badge/Version-v4.2-38BDF8?style=for-the-badge)
![PES 2021](https://img.shields.io/badge/eFootball_PES_2021-v1.7.2.0_(Steam)-0095D9?style=for-the-badge)
![Idiomas](https://img.shields.io/badge/Idiomas-PT--BR_%7C_English-2EA043?style=for-the-badge)
![Sider Compatible](https://img.shields.io/badge/Sider_7.3_%2F_BMPES_16.3-100%25_Compatible-F59E0B?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-8B5CF6?style=for-the-badge)

> **[Português (PT-BR)](#português-pt-br)** | **[English](#english)** | **[Build & Testes](#build--testes)**

![Painel VAR](docs/images/painel.png)

---

## Português (PT-BR)

Árbitro de vídeo (**VAR**) para partidas offline do **eFootball PES 2021 (`v1.7.2.0`, Steam)**: revise o lance no **Replay Manual** e aplique ou reverta faltas, pênaltis, cartões e bolas paradas direto na engine. Módulo Sider independente (**DLL nativa x64 + Lua**), feito via engenharia reversa e compatível com **Sider 7.3+** e grandes patches (**BMPES 16.3**).

### Como usar

1. Baixe o pacote em [`dist/`](dist):
   - **Módulo Lua**: [`modules/var_mod.lua`](dist/modules/var_mod.lua)
   - **Núcleo nativo**: [`modules/var_mod/var_core.dll`](dist/modules/var_mod/var_core.dll)
   - **Instalador**: [`VAR_Mod_Gerenciar.bat`](dist/VAR_Mod_Gerenciar.bat) (com [`VAR_Mod_Gerenciar.ps1`](dist/VAR_Mod_Gerenciar.ps1))
2. Rode o **`VAR_Mod_Gerenciar.bat`** e escolha **Instalar**. Ele acha a pasta do Sider ou do BMPES, copia os arquivos, ativa `luajit.ext.enabled = 1` e registra `lua.module = "var_mod.lua"` no `sider.ini` (com backup datado do `.ini`).
   - **Manual**: copie `var_mod.lua` para `<sider>\modules\` e `var_core.dll` para `<sider>\modules\var_mod\`, e adicione as duas linhas acima ao `sider.ini`.
3. Abra o jogo pelo **Sider**. A DLL só é carregada ao montar a primeira partida (zero impacto na abertura do jogo).
4. Numa partida offline, pause e abra o **Replay Manual**: o painel VAR abre sozinho. **`V`**, **`F7`** ou **`R3`** recolhem e reabrem o painel. Idioma e atalhos ficam na aba **`OPÇÕES`**.

### Funcionalidades

- **Painel VAR no Replay Manual**: abre ao entrar no replay da pausa, com barra de lances clicável, `Seek` nativo do replay 3D, `Play / Pause` e foco da câmera no infrator, na vítima ou na bola.
- **Detecção de lances a 60 Hz**: carrinhos, quedas, divididas, trancos e os eventos oficiais de falta, pênalti, cartão e vantagem viram marcadores na barra do replay, com infrator, vítima, local e zona (`DENTRO` ou `FORA DA ÁREA`) já preenchidos.
- **Pool de decisões compostas**: acumule múltiplas decisões na mesma revisão (por exemplo, retirar um cartão injusto de um jogador e marcar falta com cartão contra outro) para aplicá-las juntas ao sair do replay. O pool mantém no máximo 1 falta ou reinício por vez (evitando duas faltas simultâneas) mais cartões independentes por jogador, com remoção individual no painel.
- **Decisões aplicadas na engine (`MatchFSM`)**: anular falta ou pênalti, converter em pênalti, inverter falta, falta ou pênalti retroativo, aplicar ou retirar cartão de qualquer jogador e bola parada (escanteio, tiro de meta ou lateral), inclusive quando o cobrador já está posicionado aguardando o apito (`SetPlayReady`).
- **Nomes, números e uniformes reais**: lidos da memória do jogo (`MatchRoster`), incluindo substituições ao vivo, avatar do BAL e uniformes licenciados ou editados (BMPES).
- **Campo tático 2D**: escala `1:1` com zoom automático na área, linha do penúltimo defensor, contato infrator x vítima e clique no gramado para mover o local da cobrança.
- **Atalhos e idioma**: remap de cada ação com 2 teclas, troca automática em conflito e proteção contra ficar sem atalho essencial. Interface em **Português (BR)** ou **English**, trocada na hora.
- **Teclado e mouse no painel**: o controle só abre e recolhe o painel (`R3`, configurável) e continua controlando o replay. Enquanto o painel está em foco, teclado e mouse não chegam ao jogo.
- **Leve e seguro**: DLL de ~860 KB, hook nativo em `GrSwapChain::Present0` (sem dispositivo DX11 extra), texto nítido de 720p a 4K, somente partidas offline e nenhum arquivo do jogo alterado.

---

## English

Video assistant referee (**VAR**) for **eFootball PES 2021 (`v1.7.2.0`, Steam)** offline matches: review the play in the **Manual Replay** and apply or overturn fouls, penalties, cards and set pieces straight in the engine. Standalone Sider module (**native x64 DLL + Lua**), built via reverse engineering and compatible with **Sider 7.3+** and major patches (**BMPES 16.3**).

### Quick Start

1. Download the package from [`dist/`](dist):
   - **Lua module**: [`modules/var_mod.lua`](dist/modules/var_mod.lua)
   - **Native core**: [`modules/var_mod/var_core.dll`](dist/modules/var_mod/var_core.dll)
   - **Installer**: [`VAR_Mod_Gerenciar.bat`](dist/VAR_Mod_Gerenciar.bat) (with [`VAR_Mod_Gerenciar.ps1`](dist/VAR_Mod_Gerenciar.ps1))
2. Run **`VAR_Mod_Gerenciar.bat`** and pick **Install**. It finds your Sider or BMPES folder, copies the files, enables `luajit.ext.enabled = 1` and registers `lua.module = "var_mod.lua"` in `sider.ini` (with a timestamped `.ini` backup).
   - **Manual**: copy `var_mod.lua` to `<sider>\modules\` and `var_core.dll` to `<sider>\modules\var_mod\`, then add the two lines above to `sider.ini`.
3. Launch the game through **Sider**. The DLL only loads when the first match is set up (zero impact on game startup).
4. In an offline match, pause and open the **Manual Replay**: the VAR panel opens by itself. **`V`**, **`F7`** or **`R3`** collapse and reopen it. Language and shortcuts live in the **`OPTIONS`** tab.

### Features

- **VAR panel in the Manual Replay**: opens when you enter the pause menu replay, with a clickable incident bar, native 3D replay `Seek`, `Play / Pause` and camera focus on the offender, the fouled player or the ball.
- **60 Hz incident detection**: slide tackles, falls, challenges, shoulder charges and the official foul, penalty, card and advantage events become markers on the replay bar, with offender, fouled player, spot and zone (`INSIDE` or `OUTSIDE THE BOX`) already filled in.
- **Composite Decision Pool**: queue multiple decisions in the same review (for example, rescind an unfair card on one player and award a free kick plus card against another) and apply them together when leaving the replay. The pool enforces at most 1 foul or restart at a time alongside independent per-player card edits, each individually removable.
- **Decisions applied in the engine (`MatchFSM`)**: overturn a foul or penalty, upgrade to penalty, reverse a foul, retroactive free kick or penalty, show or remove a card for any player and set pieces (corner, goal kick or throw-in), including when the kicker is already standing over the ball (`SetPlayReady`).
- **Real names, numbers and kits**: read from game memory (`MatchRoster`), including live substitutions, Become a Legend avatar and licensed or edited kits (BMPES).
- **2D tactical pitch**: `1:1` scale with automatic box zoom, second-to-last defender line, offender vs fouled contact and click-to-move restart spot.
- **Shortcuts and language**: remap every action with 2 keys, automatic swap on conflict and a guard that never leaves an essential action unbound. UI in **English** or **Português (BR)**, switched instantly.
- **Keyboard and mouse panel**: the gamepad only opens and collapses the panel (`R3`, configurable) and keeps controlling the replay. While the panel is focused, keyboard and mouse never reach the game.
- **Light and safe**: ~860 KB DLL, native `GrSwapChain::Present0` hook (no extra DX11 device), crisp text from 720p to 4K, offline matches only and no game file modified.

---

## Build & Testes

Requer **LLVM-MinGW** (`x86_64-w64-mingw32-clang++`) ou **MinGW-w64 UCRT x64**. O build gera o pacote em `dist/` e roda a suíte de testes.

```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
$env:PES2021_EXE = "C:\Program Files (x86)\Steam\steamapps\common\eFootball PES 2021\PES2021.exe"  # opcional: valida os AOBs no executável real
$env:VAR_HUD_SNAPSHOT_DIR = "$env:TEMP\var_hud"; native\build_tmp\test_var_core.exe                  # opcional: capturas do painel
```

Notas de engenharia reversa (structs, hooks, AOBs e a máquina de estados da partida) em [`re_notes/`](re_notes). Projeto independente, sem afiliação com a Konami. Nenhum arquivo do jogo é distribuído ou alterado.

## License

[MIT](LICENSE). Inclui [Dear ImGui](https://github.com/ocornut/imgui) ([MIT](native/third_party/imgui/LICENSE.txt)).
