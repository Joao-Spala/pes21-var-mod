# Changelog

## v4.3.0

Novo motor de execução das decisões, montado sobre o fluxo nativo da engine (detalhes em `re_notes/05_native_decision_pipeline.md`).

- **A engine aplica o cartão e expulsa**: o cartão da falta marcada pelo VAR passa pelo `CommitToRegistry` nativo, que conta uma única vez, e a expulsão sai nos estados `0x12` e `0x13` da própria engine. Acabaram a pré-gravação do cartão, a anulação da contagem da engine e a escrita do flag "em campo".
- **Expulsão só de cartão na bola parada** usa o `RebuildSetPlay` nativo, o mesmo que a engine usa ao trocar cobrador.
- **Máquina de estados explícita**: ocioso, aguardando o gancho nativo, aguardando a transição e intervalo, com prazos em ticks da simulação. Reinício de partida aborta a execução.
- **Nada é gravado antes da hora**: os cartões só são escritos no mesmo tick em que a engine executa a decisão. Se ela não executar, nada fica pela metade.
- **Verificação de verdade**: a decisão só conta como aplicada, e o histórico só é gravado, quando a FSM sai do estado em que o pedido foi feito e não volta para a bola rolando.
- **Falta injetada com os valores padrão da engine** (sentinelas `0xFF`, `0x18`, `9`, `4`), nunca com zeros.
- **Correções da revisão independente**: falta convertida contra quem o cartão original expulsou não perde mais o cartão, edições absorvidas vão para o jogador escolhido, o resultado do árbitro do mesmo frame nunca é sobrescrito e uma expulsão com a bola rolando só roda quando o próprio lance faz a engine processar o elenco.
- **Ponteiros resolvidos por assinatura** só são aceitos se a chamada cair na seção de código do executável.

## v4.2.4

Revisão profunda com três auditorias independentes (memória do jogo, código novo e interface).

- **Nada é executado durante cutscenes**: o mod chamava `EnterOutOfPlay` no meio de cutscenes (estados `0x03` a `0x11`), algo que a engine nunca faz, abortando a cutscene e deixando objetos pendurados até o fim da partida. Agora o pool espera a partida chegar a `SetPlayReady` ou `InPlay`.
- **Expulsão e revogação só na bola parada**: um pool só de cartões que tira ou devolve jogador de campo espera a próxima parada, quando a engine retira ou recoloca o jogador no fluxo dela.
- **Falta injetada só com jogadores em campo**: quem já estava fora de campo (substituído ou expulso antes) nunca entra como infrator ou vítima.
- **Sem escrita em campo sem escritor nativo**: o mod não grava mais o byte `PlayerStatus + 0x0D`, e o `QuickRestartFlag` é gravado com o mesmo tamanho que a engine usa.
- **Edição de cartão presa ao lance certo**: editar o cartão de um lance antigo não altera mais a falta que está sendo cobrada.
- **Decisão sem reinício não fica registrada**: se o lance muda antes da injeção, o histórico não ganha um pênalti que não aconteceu, e o aviso diz que só os cartões foram aplicados.
- **Pool e painel**: "Manter decisão" também descarta as edições de cartão daquele lance, o vermelho direto respeita a edição pendente, o pool cheio avisa quando descarta a edição mais antiga, os resumos seguem o idioma atual, o resultado do cartão não é mais cortado e o painel não corta a barra de atalhos em 720p ou com o pool cheio.
- **Enter na tela de decisão** leva ao botão Confirmar em vez de mudar o valor da linha.
- **Instalador** ativa `luajit.ext.enabled = 1` mesmo quando a linha não existe ou está comentada.
- **README** com mensagens de erro reais, momento de carga da DLL e nomes de atalhos iguais aos do painel.

## v4.2.3

- **Linha de spray corrigida**: marcar uma falta pelo VAR com o jogo já parado em outra falta não desenha mais uma linha de spray do local antigo até o novo. O spray do árbitro (`VanishingSprayInfo`) é limpo pela mesma rotina da engine (`0x140A5CCE0`) antes da nova cobrança, como acontece em toda falta normal.

## v4.2.2

- **Jogador fantasma corrigido**: editar os cartões de um jogador que já estava fora de campo (substituído ou lesionado) não o recoloca mais em campo. O mod só tira um jogador de campo por vermelho ou segundo amarelo e só devolve quem tinha saído por expulsão. Antes, isso podia deixar um time com 12 jogadores na engine.

## v4.2.1

- **Módulo do Sider em português e inglês**: o texto do módulo no menu do Sider, incluindo as mensagens de erro, segue o idioma escolhido no painel. A escolha fica salva no `var_mod.ini` e vale na próxima vez que o jogo for aberto.
- **Versão única**: o número da versão vem de um só lugar (`native/src/version.h`) e aparece igual no Sider, no log e no `var_mod.ini`.
- **README com guia completo**: requisitos, instalação pela página de Releases, uso passo a passo, tabela de atalhos e solução de problemas.

## v4.2

- **Correção do crash ao aplicar falta** (`PES2021.exe+0x4B4C25`): o snapshot da falta passa a ser registrado no pool do árbitro (`MatchFSM + 0x4B08`), que a engine copia para a `MatchInfo` em todo commit. Antes ele era apagado no commit seguinte e o consumidor de eventos lia um ponteiro nulo. Proteção adicional no `FoulSnapshotPool::Find` (`0x140A82120`) para os 3 pontos da engine que não checam nulo.
- **Cartão nunca contado duas vezes**: o commit de cartão da engine é neutralizado só para a falta do VAR, mesmo quando chega atrasado por cutscene.
- **Pool de decisões sem perdas**: editar o mesmo lance de novo substitui a edição anterior, lances diferentes do mesmo jogador coexistem e "Manter decisão" não apaga mais as outras decisões.
- **Execução mais segura**: fallback da injeção só em `InPlay` ou `SetPlayReady`, limpeza da falta da FSM só quando ainda é a injetada, flags de campo alteradas só quando o jogador entra ou sai e checagem da alocação da `MatchInfo` antes de escrever no buffer de escrita.
- **Ciclo de vida**: DLL fixada na memória, saída do jogo sem desinstalar hooks sob o loader lock e comandos de replay descartados ao sair do replay.

## v4.1

- **Pool de Decisões compostas (`VerdictPool`)**: permite empilhar múltiplas decisões na mesma ida ao Replay Manual (como retirar cartão de um jogador na aba `CARTÕES` e marcar falta ou pênalti com cartão em outro lance na aba `REVISÃO`) e aplicá-las atomicamente ao sair do replay. Garante no máximo 1 falta ou reinício ativo por vez (substituindo o lance anterior com aviso visual) combinado a ajustes independentes de cartão por jogador, com botão `[X]` individual e `Limpar`.
- **Correção definitiva de falta e bola parada em `SetPlayReady` (`0x1A`)**: identificada e resolvida a causa raiz em que aplicar falta quando o batedor já estava posicionado gerava apenas fade de tela sem alterar a cobrança. A Fox Engine usa alocação RCU dupla em `MatchInfo` (`0x46F8` bytes, `Read Buffer [0]` em `+0x0000` e `Write Buffer [1]` em `+0x2378` copiado por `0x14041BEC0` em `State_16` e `State_45`), além de exigir passagem por `State_01` (`0x14044DB80` -> `0x1404530C0` com `StoppageReason::SilentRestart = 7`, `0x14044F950` e `0x1404A8730`) e limpeza de `MatchFSM + 0x49C0` no tick seguinte após o `CommitToRegistry`.

## v4.0

- **Correção de crash ao aplicar decisão**: a falta deixada na máquina de estados da partida passa a usar os sentinelas da própria engine (`0xFF`) e um snapshot existente (crash em `PES2021.exe+0x4B4C25`).
- **Nomes e números reais**: número da camisa lido da ficha do time (`TeamEntry + 0x29A`). Se o elenco não puder ser lido, o painel cai para `CASA` / `FORA` e `Casa N` / `Fora N` (traduzidos), nunca para texto vazio.
- **Leitura do elenco mais robusta**: checagem de memória que atravessa regiões, cache de times travado só com os dois nomes lidos e nova sincronização após cada reinício de partida.
- **Correção de crash ao fechar o jogo** (`ucrtbase.dll`, `0xC0000409`) e log persistente com diagnóstico do elenco (`RosterDiag`).
- **Controle só abre e recolhe o painel** (`R3`, configurável). O painel é operado por teclado e mouse e o controle continua no replay.
- **Nova interface**: visual de transmissão, texto em português com acentos, teclas desenhadas nas dicas e fontes geradas na resolução real (720p a 4K).
- **Remap de atalhos e idioma**: página `OPÇÕES > Controles` e seletor Português (BR) / English.
- **Desempenho**: DLL de 1,88 MB para ~860 KB, carregamento sob demanda e 1 lock por frame no replay.
- **Módulo Lua compatível com o Sider**: removido `pcall`, que não existe no ambiente Lua do Sider.

## v3.0

- Painel simplificado com navegação por teclado, controle e mouse e cursor visível sobre o painel.
- Bloqueio de entrada do jogo enquanto o painel está em foco.
- Correções de inicialização lenta e de crash ao aplicar cartão.

## v2.0

- Reformulação da interface e das funcionalidades do painel.

## v1.0

- Primeira versão: módulo Sider com DLL nativa, hooks da partida e painel VAR no Replay Manual.
