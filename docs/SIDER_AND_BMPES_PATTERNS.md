# Padrões Reais do BMPES 16.3 e Sider 7.3.3

Estes padrões e endereços foram extraídos diretamente dos módulos instalados em `<Steam>\steamapps\common\BMPES\modules\`.

## 1. Endereços Já Validados no `PES2021.exe` (Steam 1.7.2.0)

Extraídos de `<Steam>\steamapps\common\BMPES/modules/goal_pyro.lua` (tabela `m._gp.MAP`, que converte endereços CPY/FL26 para Steam 1.7.2.0):

| Símbolo / Função | VA Steam 1.7.2.0 | VA CPY/FL26 | O que representa |
| :--- | :--- | :--- | :--- |
| Assinatura de detecção do EXE | File offset `0x3FF480` (`4188800`) | File offset `0x3FF430` (`4188720`) | 32 bytes usados por `goal_pyro.lua` e `PESCustomCelebrationsReload.lua` para confirmar build Steam vs CPY no disco antes do unpack |
| `GMATCH_GLOBAL` | `0x143702208` | `0x1436F3DC8` | Ponteiro global (`uint64_t*`) para o objeto ativo de partida (`GMATCH`) |
| `GMATCH_REPLAY_MANAGER_OFF` | `GMATCH + 0x388` | `GMATCH + 0x388` | Ponteiro para o Replay Manager ativo da partida |
| Replay Manager campos conhecidos | `+0x00` (state), `+0x0C` (seq), `+0x58` (source-current) | Mesmo | Estado e sequência do replay nativo da Fox Engine |
| `SOURCE_RECORD_SITE` | `0x140423974` | `0x1404230D4` | Hook de bookmark de gravação de replay (`return = 0x140423984`, `writer = 0x140A8A4C0`) |
| `native_get_scorer` call site | `0x140478C3F` | `0x14047826F` | Fluxo nativo de evento de gol (`call native_get_scorer`, `mov ebx, eax`, `call native_get_context` em `0x140478C46`, `mov rdx, [rax+0x438]`) |

**Nota crítica sobre `GMATCH+0x388`:** conforme documentado no próprio `goal_pyro.lua`, a Fox Engine limpa temporariamente `GMATCH+0x388` durante certas transições de câmera. Leituras em runtime devem sempre validar o ponteiro com `VirtualQuery` / checagem de faixa antes de dereferenciar.

## 2. Como o BMPES Faz Hooks e UI no Sider 7.3.3

Existem dois padrões principais comprovados na pasta `modules\` do BMPES:

### Padrão A: LuaJIT + FFI puro com trampolim x64 (`goal_pyro.lua`, `PESCustomCelebrationsReload.lua`)
- Usa `ffi` do LuaJIT com `VirtualAlloc(nil, PAGE_SIZE, 0x3000, 0x40)` e `VirtualProtect` para instalar detours x64 diretamente na memória do `PES2021.exe`.
- Prós: arquivo único `.lua`, zero compilação extra.
- Contras: muito limitado para UI rica (só tem o texto monoespaçado do overlay do Sider via `overlay_on`), difícil de manter código x64 complexo em strings hex e limite de 200 variáveis locais por escopo no LuaJIT.

### Padrão B: Módulo Sider Lua como controlador + DLL nativa x64 (`BMPES_LeagueChain.dll`, `BMPES_Dia1_Launcher.dll`)
- Um `.lua` leve no Sider registra eventos (`init`, `set_teams`, `overlay_on`, `key_down`, `gamepad_input`) e carrega uma DLL C++ x64 via `ffi.load(...)`.
- A DLL C++ x64 cuida de:
  - Hooks x64 seguros (MinHook ou trampolins estruturados em C++ com mutex/ring-buffer lock-free).
  - Hook de `IDXGISwapChain::Present` (DirectX 11) para renderizar uma **interface visual nativa de transmissão VAR com Dear ImGui** dentro da tela de Replay/Pausa do jogo, com controle completo por Gamepad (XInput) e Teclado.
  - Estado persistente de gravação de decisões de arbitragem durante a partida, exposto via FFI ao módulo Sider.
- **Avaliação para o VAR Mod:** o Padrão B (Sider Lua para liga/desliga, config e ciclo de vida + DLL C++ x64 para hooks de arbitragem/replay e HUD DX11 dentro do Replay) é drasticamente superior em estabilidade, manutenibilidade e qualidade visual.

## 3. Eventos e API do Sider 7.3.3 Relevantes

Verificados em `ReplayCam.lua`, `goal_pyro.lua` e `doc\scripting.html`:
- `overlay_on`: menu textual do Sider (`Espaço`) para ativar/desativar o mod, configurar atalhos, modo de exibição e diagnósticos.
- `key_down` / `key_up`: captura de teclado.
- `gamepad_input`: captura de controle no Sider.
- `set_teams`: dispara ao preparar cada partida (ideal para zerar o histórico de lances do VAR e revalidar ponteiros de `GMATCH`).
- `memory.read`, `memory.write`, `memory.search`: utilitários nativos do Sider para AOB e memória.

## 4. Padrão BMPES de Ativação/Desativação Opcional

Verificado em `<Steam>\steamapps\common\BMPES/GoalPyro_Opcional.ps1` e `<Steam>\steamapps\common\BMPES/Comemoracoes_Opcional.ps1`:
- O mod deve fornecer scripts `Ativar VAR.bat` / `Desativar VAR.bat` + `VAR_Opcional.ps1` que adicionam/comentam `lua.module = "var_mod.lua"` nos 3 arquivos (`sider.ini`, `sider-normal.ini`, `sider-capacidade.ini`) usando encoding `28591` (Latin-1), verificando se `PES2021.exe` está fechado e criando backup `.antes-var-opcional` antes da primeira edição.
- Além disso, dentro do jogo, o usuário pode ligar/desligar a captura e o HUD do VAR em tempo real pelo menu do Sider (`overlay_on`).
