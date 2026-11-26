# Changelog

## v4.4.7

Verificação tripla de estabilidade: auditoria de crash e travamento em todo o código nativo, auditoria de cada escrita na memória do jogo conferida no executável, gate de release (DLL, Lua, `sider.ini`, logs e pacote) e a suíte inteira rodada com AddressSanitizer e UndefinedBehaviorSanitizer (20 de 20, sem erro de memória).

- **Desfazer da bola parada completo**: no fim do `MainTick` a engine já copia o resultado do árbitro para a estrutura que registra o cartão. Agora o mod restaura também essa cópia quando a cobrança não chega a ser montada, então nenhum cartão fica registrado sem a decisão. O contrato foi travado por teste no executável real.
- **Nunca terceiro amarelo nem segundo vermelho**, em qualquer caminho de ajuste de cartão.
- **Instalação dos ganchos sem risco de crash**: as outras threads ficam congeladas durante a escrita do código, e o mod confere que nenhuma está parada no meio das instruções substituídas, como faz a biblioteca Detours.
- **Sem perda de fps sem controle**: quem joga só no teclado não paga mais a consulta cara aos slots vazios do XInput a cada frame com o painel aberto.
- **Campinho protegido contra posição inválida** vinda do jogo (sem laço infinito ao desenhar).

## v4.4.6

- **Interface do mesmo tamanho relativo em qualquer resolução**: a escala segue a altura da tela (1,0 em 1080p) e é limitada pela largura, então o painel ocupa a mesma fração da tela de 720p a 4K, em ultrawide, 4:3 e 5:4. Antes, em 720p o painel ficava proporcionalmente maior que o normal, em 4:3 e 5:4 a largura não era considerada, e acima de 4K ele ficava pequeno. Há um piso de legibilidade em telas muito pequenas e um teto em 5K e 8K. O tamanho escolhido em Opções (Compacto, Padrão, Grande) continua valendo por cima.
- **Avisos fora do painel**: os avisos do topo e de confirmação são centralizados na parte livre da tela quando o painel aberto ficaria por baixo deles.
- **Validado por teste em 12 resoluções**: a escala proporcional é conferida em 12 resoluções, e em 7 delas, nos dois idiomas, o painel desenhado fica inteiro dentro da tela e com largura proporcional.

## v4.4.5

Verificação tripla final por três ângulos novos (temporização do executor contra o binário, detecção e classificação de lances, empacotamento e documentação) e rodada de convergência.

- **Asserts do ImGui realmente desligados**: o cache do build não recompilava a imgui quando um header mudava, então a desativação dos asserts da v4.4.2 nunca chegou à DLL publicada. Agora a imgui é recompilada quando seus headers mudam, e a DLL saiu sem nenhum assert (`_wassert` ausente).
- **Bola parada nunca fica pela metade**: em `SetPlayReady` a engine pode sair do estado sem montar a cobrança (troca de jogador, menu). O mod agora confere isso no fim do mesmo tick, antes do registro do cartão, e desfaz tudo o que escreveu (cartões, cobrança e falta da partida, campos da FSM). A decisão volta a esperar.
- **Linha do tempo fiel**:
  - a falta apitada depois de uma vantagem fica no momento do contato e não no apito
  - cada marcador oficial usa o lance mais forte da jogada
  - duas disputas simultâneas não viram dezenas de marcadores
  - a zona (dentro ou fora da área) usa o lado do campo do momento do lance, também depois do intervalo
- **Vantagem para o time certo**: o time beneficiado vem de quem sofreu o contato.
- **Simulação**: o jogador que simulou é o infrator do lance, e a área considerada é a do adversário.
- **Pacote reenfileirado** se junta ao que foi armado no meio tempo, sem perder nada.
- **Build**: a DLL só vai para `dist/` depois que os testes passam.
- **Instalador**: um erro no menu (por exemplo, o jogo aberto) volta ao menu em vez de fechar. A desinstalação guarda o `var_mod.ini` com os atalhos. O `sider.ini` mantém o final de arquivo original.
- **README**: instalação manual com `lua.enabled = 1`, limite de 32 ajustes de cartão e aviso de jogo aberto.

## v4.4.4

Nova verificação tripla: simulação passo a passo de oito combinações de decisões, auditoria da máquina de estados do painel, revisão das mudanças da v4.4.3 contra o executável (o buffer duplo do TeamInfo foi confirmado em todos os pontos) e uma rodada de convergência.

- **Decisão nunca aplicada fora de hora**: uma falta ou reinício decidido numa parada só vale para aquela parada. Se o jogo seguir (cobrança, gol, intervalo) antes de a decisão rodar, ela é descartada com aviso e só os cartões do pacote continuam. Antes, uma decisão adiada podia virar uma falta no segundo tempo, com o local espelhado, ou trocar a saída de bola depois de um gol.
- **Saída de bola nunca é trocada** por uma decisão do VAR.
- **Segundo amarelo e vermelho direto**: um jogador com vermelho direto ainda pode receber o amarelo da falta, sem terceiro amarelo nem segundo vermelho. O histórico mostra o cartão realmente dado, e o segundo amarelo usa o aviso vermelho.
- **Painel**:
  - confirmar sem jogador mostra o aviso na própria tela de decisão e leva ao campo Jogador
  - clicar no campinho não muda mais o reinício de uma anulação
  - `[` e `]` não trocam o lance com uma decisão aberta
  - o mouse não troca mais a seleção da página recém-aberta
  - redecidir o mesmo lance aparece como "Atualizar" e não como "Substituir"
  - uma decisão recusada avisa em vez de dizer que foi enviada
  - as teclas longas cabem na página de controles
  - os textos do pool não são mais refeitos a cada frame.
- **Robustez**: a validação do buffer do time acontece antes de consumir o pacote, que continua armado até poder ser aplicado. O reenfileiramento tem limite.

## v4.4.3

Verificação tripla em rodadas sucessivas: quatro auditorias por novos ângulos (fluxo de decisão de ponta a ponta, contrato com a memória do jogo conferido no binário, concorrência e ciclo de vida, interface e idiomas), depois reauditorias independentes e revisões do diff até não sobrar defeito confirmado.

- **Cartões agora persistem de verdade**: o jogo guarda os dados dos times em dois buffers e copia o de escrita por cima do de leitura em todo frame. O mod escrevia só no de leitura, então revogar ou ajustar um cartão podia ser desfeito no frame seguinte. Agora toda escrita de cartão vai para os dois buffers, validados antes, e toda leitura de cartão usa o buffer de escrita. Sem buffer validado o mod não aplica nada pela metade.
- **Posição vazia nunca recebe cartão**: o jogo devolve o registro do primeiro jogador do elenco para uma posição sem ninguém. O mod agora recusa essa posição.
- **Anular a falta atual e marcar uma falta anterior no mesmo pacote**: o recurso anunciado na v4.4.0 agora funciona nas duas ordens. A falta anterior substitui a cobrança e a anulação retira o cartão e marca o lance como anulado.
- **Infrator nunca é inventado**: sem jogador identificado, a decisão pede a escolha do jogador em vez de assumir o goleiro.
- **Identidade do jogador em todo lugar**: lances do replay, edições diretas e decisões armadas antes de uma substituição guardam quem estava na posição naquele momento.
- **Cartão da falta nunca em dobro nem perdido**: o mod só completa o cartão à mão quando a engine comprovadamente não o aplicou (contagem igual à de antes da injeção).
- **Histórico fiel ao jogo**: contagens de cartão lidas do próprio jogo para o lance atual, registro mesmo quando só o cartão de uma decisão foi aplicado, e anulação vencida (de outra parada) só retira o cartão.
- **Decisão não se perde**: se a engine gerou o próprio resultado no instante da aplicação, o pacote volta a esperar a próxima bola parada.
- **Segurança de execução**: pacote de outra partida descartado, nada é armado com o VAR desligado, leitura do render suspensa entre o fim da partida e o próximo jogo, ponteiros da partida revalidados a cada frame, texto UTF-8 cortado nunca lido além do fim, falta de memória no HUD nunca encerra o jogo, ganchos nunca liberam código em uso.
- **Interface**: nomes com letras do Latin Extended (Modrić, Çalhanoğlu), nomes de times e títulos longos com reticências, seletor de idioma e página de controles sem corte, campinho menor quando falta espaço, botão "Substituir no pool" quando a decisão troca outra, pool atualizado no mesmo frame da ação, resumo recalculado quando uma edição é incorporada.
- **Confirmar decisão**: o botão da tela de decisão agora diz o que o usuário faz ("Confirmar decisão", ou "Atualizar decisão" e "Substituir decisão" quando for o caso), e não mais "Adicionar ao pool". Ao confirmar, um aviso curto na parte de baixo da tela diz o que aconteceu: decisão enviada, atualizada ou substituída no pool de decisões, ou cartão incorporado à decisão do lance.
- **Configuração**: `var_mod.ini` gravado de forma atômica.

## v4.4.2

Revisão profunda final com quatro auditorias independentes (motor de execução, estado e painel, render e infraestrutura, módulo Lua e instalador). Cada achado foi conferido no código e no binário antes da correção.

- **Cartões seguem o jogador, não a posição**: depois de uma substituição, revisar um lance antigo tirava ou dava o cartão para o reserva que entrou na mesma posição. Agora cada lance guarda quem estava na posição naquele momento, e cartões e faltas vão para esse jogador. Um jogador substituído nunca vira infrator nem vítima de uma falta nova.
- **Cartão da falta nunca fica pela metade**: se o batedor cobrava a falta no mesmo instante em que a decisão era aplicada, o cartão original podia sair sem o novo entrar. Agora o mod confirma se a engine registrou a falta e, se não registrou, aplica o cartão novo direto.
- **Reinício coerente**: falta com a bola rolando que perde o infrator, ou que ficou sem cartão depois de uma expulsão no mesmo pacote, reinicia sem cutscene de outra falta e sem cobrança rápida que deixaria o expulso em campo.
- **Inverter papéis (Tab) e depois converter** não deixa mais infrator e time beneficiado do mesmo lado.
- **"Manter decisão"** não apaga mais uma decisão de outro lance.
- **"Anular falta marcada"** só se liga à parada atual, nunca a uma falta antiga do mesmo jogador. Depois dela o painel volta ao lance que estava selecionado.
- **Editar o cartão de um lance que já tem decisão** ajusta essa decisão em vez de reverter o cartão original duas vezes.
- **Linha do tempo**: o rótulo "CARTÃO VERMELHO" sai quando o VAR retira o vermelho.
- **Anular falta sem infrator identificado** usa o lado certo do campo, e uma falta injetada sem infrator não é registrada de novo como lance do árbitro.
- **Nova partida sem aviso do Sider** (revanche, sequências de liga e copa) também limpa o pool, o histórico e o painel.
- **Estabilidade do render**: os asserts do ImGui não encerram mais o jogo, um frame que não pode ser desenhado libera o bloqueio de controle, os ponteiros da partida são revalidados a cada frame e o encerramento do processo não depende mais de travas.
- **Instalador**: recusa instalar ou desinstalar com o PES aberto, copia a DLL antes do Lua, preserva a codificação do `sider.ini` (BOM e quebras de linha), aceita pastas com colchetes, garante `lua.enabled = 1` e prioriza o Sider do BMPES na detecção.
- **Tecla 7 no overlay do Sider**: segurar a tecla não liga e desliga mais várias vezes.

## v4.4.1

Análise do dump completo do travamento ao avançar depois do fim da partida. Evidências em `re_notes/07_engine_heap_segment_release.md`.

- **Travamento e crash depois do apito final corrigidos**: na troca de cena do pós-jogo a engine compacta os heaps e devolve ao Windows os blocos livres. A rotina nativa de liberação (`0x1403FC0D0`) liberava a alocação inteira, falhava no meio e em seguida escrevia no bloco que tinha acabado de liberar. O jogo travava enquanto o Windows gravava o dump e caía em `PES2021.exe+0x3FE31D`.
- **Gatilho**: módulos Lua do BMPES escrevem na memória do jogo pelo Sider e deixam páginas soltas do heap com proteção de execução. Uma única página assim dentro de um bloco basta para a rotina nativa se perder. O defeito existe com ou sem o VAR.
- **Correção**: o mod substitui essa rotina por uma versão atômica que valida o bloco inteiro antes de liberar e libera por alocação do sistema. Ou o bloco sai inteiro ou nada é tocado. A proteção vale para o processo todo, inclusive com o VAR desligado.

## v4.4.0

Pesquisa e evidências em `re_notes/06_replay_cutscene_and_classification.md`.

- **Lados do campo corrigidos**: o mod lia a direção de ataque como número decimal quando a engine guarda um byte com sinal. O lado nunca invertia no segundo tempo, e por isso marca de pênalti, tiro de meta, escanteio, "dentro ou fora da área" e o campinho podiam ficar no lado errado, a ponto de um pênalti a favor sair no próprio gol. Agora o lado vem do byte da engine, calibrado em cada saída de bola pelas posições dos jogadores.
- **Classificação de lances refeita**: a tabela de ações estava trocada (o bote em pé virava "carrinho" e defesa de goleiro virava "tranco"). Agora o lance segue o próprio observador de contato da engine: a vítima entra em queda ou desequilíbrio e a engine informa quem causou. Carrinho, bote e disputa de corpo saem da ação do causador.
- **Quase falta com marcação amarela**: contato que derrubou um jogador, com o causador identificado e sem apito.
- **Simulação**: mergulho aparece como "Simulação?". Ações de goleiro e bloqueios não geram mais lance, e lances diferentes não são mais fundidos só por estarem perto.
- **Anular a falta ou o pênalti marcado em qualquer contexto**: com a partida parada por falta, o menu sempre oferece anular essa marcação, mesmo com o cursor em outro lance. Assim dá para marcar uma falta num lance anterior e anular a atual no mesmo pool.
- **Anular depois da cobrança** só retira o cartão e avisa isso, sem trocar a bola parada atual por uma antiga.
- **Proteções**: tipos de reinício que travariam a máquina de estados são recusados, e a vantagem registra o time certo.
- **Diagnóstico do replay** (só leitura) no `var_core.log`, base para guardar o lance anterior à falta no replay manual numa próxima versão.

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
