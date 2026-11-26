<div align="center">

<img src="docs/images/hero_pt.png" alt="VAR para PES 2021: pause, reveja, decida. O jogo obedece." width="100%">

![Version](https://img.shields.io/badge/Version-v4.4.10-38BDF8?style=for-the-badge)
![PES 2021](https://img.shields.io/badge/eFootball_PES_2021-v1.7.2.0_(Steam)-0095D9?style=for-the-badge)
![Sider](https://img.shields.io/badge/Sider_7.3+-Compatible-F59E0B?style=for-the-badge)
![Native](https://img.shields.io/badge/Native-C%2B%2B20_%2B_DX11_%2B_ImGui-84CC16?style=for-the-badge)
![Languages](https://img.shields.io/badge/Languages-PT--BR_%7C_English-2EA043?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-8B5CF6?style=for-the-badge)

[English](README.md) · **Português (BR)** · [Apoie](#apoie-o-projeto)

### [Baixar a última versão](https://github.com/Joao-Spala/pes21-var-mod/releases/latest)

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Pague_um_caf%C3%A9-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/AlfaceJao) [![Pix](https://img.shields.io/badge/Pix-QR_Code-32BCAD?style=for-the-badge&logo=pix&logoColor=white)](#apoie-o-projeto)

</div>

---

## O que é

O **VAR para PES 2021** adiciona uma revisão manual de vídeo às partidas offline. Você pausa, abre o replay, escolhe o lance e decide. A decisão é aplicada dentro da engine do PES, então a cobrança, o cartão e a expulsão seguem como se o árbitro tivesse tomado aquela decisão desde o começo. Quando é uma falta ou um pênalti com a bola rolando, a engine depois mostra o lance de novo.

É um módulo do Sider (DLL nativa x64 e Lua) para a versão Steam do jogo.

## Veja em movimento

<p align="center"><img src="docs/images/pt/replay.gif" alt="O painel do VAR acompanhando o replay quadro a quadro" width="450"></p>

<p align="center"><sub>O replay corre e o painel acompanha quadro a quadro. Primeiro uma dividida no meio-campo, depois o campinho entra sozinho na área quando o ataque chega lá. O marcador dourado da barra não para de pulsar: é o lance com mais cara de falta que o árbitro deixou passar, um carrinho que derrubou o centroavante dentro da área.</sub></p>

## O que você pode fazer

| Situação | O que o VAR faz |
| --- | --- |
| **O pênalti que não foi marcado.** Seu atacante foi derrubado e o jogo seguiu. | Volte ao lance no replay e escolha **Marcar pênalti**. A partida vai para a marca da cal com o cartão que você decidir, e a engine repete o lance revisado depois do apito. |
| **A marcação injusta.** O árbitro deu falta e amarelo num lance limpo. | Escolha **Anular falta**. A falta some, o cartão é retirado e a posse volta para o time certo. |
| **O cartão errado.** Era o segundo amarelo e o jogador ficou em campo, ou o contrário. | Ajuste o cartão de qualquer jogador na aba **CARTÕES**. A engine tira ou devolve o jogador na bola parada. |

## Do lance à decisão

```mermaid
flowchart LR
    A["Lance<br/>polêmico"] --> B["Pause e abra o<br/>Replay Manual"]
    B --> C["O lance suspeito<br/>pulsa em dourado"]
    C --> D["Confirme<br/>a decisão"]
    D --> E["Saia do replay:<br/>a engine aplica"]
    E --> F["A engine repete<br/>o lance revisado"]
```

O painel encontra sozinho os momentos que importam. Ele acompanha a partida 60 vezes por segundo e marca na barra do replay carrinhos, botes, trancos, quedas e simulações, além das faltas, cartões e vantagens do árbitro. Cada marcador já chega com infrator, vítima, local e zona. Você confere, ajusta o que quiser e aperta **Confirmar decisão**. A decisão vai para o **pool de decisões** e, ao sair do replay, a engine aplica tudo de uma vez.

## De perto

<table>
<tr>
<td width="50%"><img src="docs/images/pt/feat_timeline.png" alt="Barra do replay com o lance suspeito"></td>
<td><b>O lance suspeito salta aos olhos.</b><br>Cada lance que o árbitro deixou passar ganha uma nota de falta não marcada: o tipo de entrada, se o adversário caiu por causa daquele contato, a área e quão justo foi o contato. O mais forte pulsa em dourado até você abri-lo. Marcações oficiais e simulações nunca entram.</td>
</tr>
<tr>
<td><img src="docs/images/pt/feat_card.png" alt="Cartão do lance"></td>
<td><b>Quem fez o quê, num relance.</b><br>Infrator em laranja e vítima em ciano, cores que nenhum uniforme e nenhum estado da interface usam. A distância diz o que mede: <i>7,8 m dentro da área</i>.</td>
</tr>
<tr>
<td><img src="docs/images/pt/feat_pitch.png" alt="Campinho tático"></td>
<td><b>Uma prancheta tática, não um radar.</b><br>Enquadrada no lance e acompanhando o movimento, com número da camisa legível em qualquer uniforme, seta de direção nos jogadores perto da bola, rastro de 1,5 s dos dois jogadores do lance e um rastro pontilhado da bola, setas que mostram para onde cada time ataca, linha de impedimento e rótulos que ficam onde não cobrem ninguém.</td>
</tr>
</table>

**Também novo na v4.4.9 e v4.4.10**

- **A câmera acompanha o salto.** Clique num lance da barra e a câmera 3D do replay vai para o novo quadro, mantendo ângulo e distância.
- **Replay depois da decisão.** Uma falta ou um pênalti marcado com a bola rolando é mostrado de novo pela própria engine, depois das cenas da parada. Dá para desligar em **OPÇÕES**.
- **Legível em qualquer tela.** Tamanho mínimo de texto e de ficha em 720p, 4:3 e 800x600.
- **Mais leve.** Atlas de fonte menor e sem cópia na RAM, sombra do painel mais barata e DLL menor.

## O painel

<table>
<tr>
<td align="center" width="33%"><img src="docs/images/pt/revisao.png" alt="Revisão"><br><b>Revisão</b><br><sub>A barra de lances, o lance em foco e o campinho tático com o contato entre infrator e vítima.</sub></td>
<td align="center" width="33%"><img src="docs/images/pt/decisao.png" alt="Decisão"><br><b>Decisão</b><br><sub>Ajuste jogador, cartão, time e local. O X branco com o rótulo COBRANÇA é onde o jogo recomeça, um clique o move.</sub></td>
<td align="center" width="33%"><img src="docs/images/pt/pool.png" alt="Pool de decisões"><br><b>Pool de decisões</b><br><sub>Um lance e até 32 ajustes de cartão, aplicados juntos e em ordem ao sair do replay.</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/images/pt/cartoes.png" alt="Cartões"><br><b>Cartões</b><br><sub>Os 22 jogadores com nomes e números reais. Corrija o cartão de qualquer um, mesmo sem lance.</sub></td>
<td align="center"><img src="docs/images/pt/opcoes.png" alt="Opções"><br><b>Opções</b><br><sub>Lado da tela, tamanho, campinho, avisos, o replay depois da decisão e idioma, salvos para a próxima partida.</sub></td>
<td align="center"><img src="docs/images/pt/controles.png" alt="Controles"><br><b>Controles</b><br><sub>Duas teclas por ação, troca automática em conflito e nenhum atalho essencial perdido.</sub></td>
</tr>
</table>

<p align="center"><img src="docs/images/pt/recolhido.png" alt="Painel recolhido com o pool pronto"><br><sub>Recolhido, o painel vira uma pílula que lembra o que vai ser aplicado.</sub></p>

## Ver, decidir, aplicar, lembrar

| | |
| --- | --- |
| **Ver** | A partida gravada a 60 Hz, um quadro por quadro da simulação, então o campinho acompanha o replay quadro a quadro e suaviza as posições em câmera lenta. Os lances vêm do modelo de contato da própria engine, e a falta apitada depois de uma vantagem fica no momento do contato. Nomes, números e uniformes vêm da memória do jogo, inclusive os editados por patches. |
| **Decidir** | Marcar falta ou pênalti num lance anterior, anular a falta ou o pênalti marcado, converter em pênalti, inverter a falta, ajustar cartões e dar escanteio, tiro de meta ou lateral, mesmo com o cobrador já posicionado. |
| **Aplicar** | O cartão passa pelo registro da própria engine, que conta uma vez só, e a expulsão sai pelo fluxo dela. Se o jogo não montar a cobrança, o mod desfaz tudo o que escreveu naquele instante. Numa falta ou num pênalti marcado com a bola rolando, a engine depois repete o momento revisado pelo seu próprio replay de lances passados. |
| **Lembrar** | Cartões e faltas seguem o jogador do lance, não a posição, mesmo depois de uma substituição. Uma decisão sobre uma parada que já passou nunca é aplicada fora de hora. |
| **Respeitar** | Nunca terceiro amarelo nem segundo vermelho. Nenhum arquivo do jogo é alterado. O painel ocupa a mesma fração da tela de 720p a 4K, em ultrawide e 4:3, nos tamanhos Compacto, Padrão e Grande. |

## Limitações

- Funciona só no **PES 2021 `v1.7.2.0` da Steam**. Outras versões do executável têm outros endereços e não são suportadas.
- **Só partidas offline**: Exibição, Master Liga, Rumo ao Estrelato e Copas. Nada de rede, DRM ou modo online é tocado.
- Não volta o jogo no tempo. Uma decisão vale para a parada em que foi tomada, e **gols não podem ser anulados**.
- O replay depois da decisão vale para falta ou pênalti marcado com a bola rolando, com o lance revisado ainda na memória de replay do jogo (cerca de 11 s de jogo). A engine pode mostrar antes o replay instantâneo dela. Se faltar alguma condição, a decisão é aplicada sem o replay extra.
- O controle abre e recolhe o painel e continua controlando o replay. Dentro do painel, a navegação é por teclado e mouse.

## Instalação

1. Baixe o **`PES2021_VAR_Mod_v4.4.10.zip`** em [**Releases**](https://github.com/Joao-Spala/pes21-var-mod/releases/latest) e extraia em qualquer pasta.
2. Com o jogo fechado, rode o **`VAR_Mod_Gerenciar.bat`** e escolha **`1 - Instalar ou atualizar`**.
3. Abra o jogo pelo **Sider** (num patch, pelo atalho do próprio patch). Pronto.

O instalador acha o Sider sozinho (ou pergunta o caminho), faz backup datado do `sider.ini` antes de qualquer mudança e não mexe em nada com o jogo aberto. A DLL só carrega na primeira partida, então a abertura do jogo continua igual.

<details>
<summary><b>Opções do instalador, instalação manual e requisitos</b></summary>

| Opção | O que faz |
| --- | --- |
| **1 - Instalar ou atualizar** | Copia a versão nova e ativa o mod no `sider.ini` |
| **2 - Ativar** | Volta a ligar o mod sem copiar nada |
| **3 - Desativar** | Desliga o mod e mantém os arquivos |
| **4 - Desinstalar** | Guarda seu `var_mod.ini` (atalhos e idioma) ao lado do `sider.ini`, apaga os arquivos e a linha do mod e mantém `luajit.ext.enabled = 1`, que outros módulos também usam |

**Manual:** copie `modules\var_mod.lua` para `<pasta do sider>\modules\` e `modules\var_mod\var_core.dll` para `<pasta do sider>\modules\var_mod\`. Depois adicione ao `sider.ini`:

```ini
lua.enabled = 1
luajit.ext.enabled = 1
lua.module = "var_mod.lua"
```

**Requisitos:** PES 2021 `v1.7.2.0` (Steam), Sider 7.3+ com LuaJIT (sozinho ou dentro de um patch) e Windows 10 ou 11 x64.

</details>

## Atalhos

| Ação | Teclado | Controle |
| --- | --- | --- |
| Abrir ou recolher o painel | `V` ou `F7` | `R3` |
| Confirmar / Voltar | `Enter` / `Esc` ou `Backspace` | |
| Aba anterior / próxima | `Page Up` ou `Q` / `Page Down` ou `E` | |
| Lance anterior / próximo | `[` / `]` | |
| Inverter papéis ou trocar time | `Tab` | |
| Play / Pause do replay | `P` | |
| Navegar | Setas ou `W` `A` `S` `D` | |
| Limpar o pool | `Delete` | |
| Telemetria | `F10` | |

Todos remapeáveis em **OPÇÕES > Controles**. Enquanto o painel está em foco, teclado e mouse não chegam ao jogo. No menu do Sider, a tecla **`7`** liga e desliga o VAR e a **`8`** mostra a telemetria ao vivo.

<details>
<summary><b>Solução de problemas</b></summary>

| Sintoma | O que fazer |
| --- | --- |
| **O menu do Sider mostra `Status: ERRO (...)`** | A DLL não carregou e o texto diz o motivo. Confira se `var_core.dll` está em `<pasta do sider>\modules\var_mod\`, se `lua.enabled = 1` e `luajit.ext.enabled = 1` estão no `sider.ini` e veja o `sider.log`. |
| **O painel não abre no replay** | Confirme que a partida é offline e que o VAR está ligado (tecla `7` no menu do Sider). `V`, `F7` ou `R3` reabrem o painel recolhido. |
| **Não houve replay depois da decisão** | Ele só acontece numa falta ou pênalti marcado com a bola rolando, e pode estar desligado em **OPÇÕES**. Fora isso, o log diz o motivo numa linha `Decision replay: ...` (o lance já tinha saído da memória de replay, a engine não enfileirou replay naquela parada ou o espaço de replay dela estava ocupado). |
| **Algo deu errado numa partida** | O log fica em `<pasta do sider>\modules\var_mod\var_core.log` e é recriado a cada abertura do jogo. Copie antes de abrir de novo e anexe numa [issue](https://github.com/Joao-Spala/pes21-var-mod/issues). |
| **Voltar ao padrão** | Apague `<pasta do sider>\modules\var_mod\var_mod.ini`. |

</details>

---

## Por dentro

Um mod que escreve na memória de um jogo fechado só merece confiança se for previsível. Estas são as escolhas de engenharia que sustentam isso.

```mermaid
sequenceDiagram
    participant P as Painel
    participant Pool as Pool
    participant X as Executor
    participant E as Engine (MatchFSM)
    participant R as Replay da engine
    P->>Pool: Confirmar decisão
    Note over Pool: Um lance + até 32 cartões
    X->>Pool: Na parada certa
    X->>E: Resultado do árbitro
    E->>E: Monta a cobrança e registra o cartão
    X-->>E: Confere no mesmo tick
    Note over X,E: Cobrança não montada? Tudo é desfeito
    X->>R: Clipe pendente do momento revisado
    Note over X,R: Só depois que a engine enfileirou o estado de replay
    R->>R: Mostra depois da cena do cartão
```

| | |
| --- | --- |
| **Nativo** | DLL x64 em C++20 com cerca de 780 KB, um único gancho de render em `GrSwapChain::Present0`, sem dispositivo DX11 extra e sem trabalho de interface fora do replay. Medido nos caminhos reais do código num benchmark: 3 a 6 µs por tick da simulação e 2 µs por quadro. |
| **Na engine** | Decisões entram pela máquina de estados da partida (`MatchFSM`) e pelo registro de cartões da própria engine, nos dois buffers que ela usa. O replay depois da decisão é o clipe pendente da própria engine, preenchido campo a campo como o construtor dela faz. |
| **Atômico** | A cobrança é conferida no mesmo tick e, se o jogo não a montou, cartões e dados da partida voltam ao estado anterior. Um pedido de replay que ninguém consome volta sozinho ao estado ocioso. |
| **Seguro** | Ganchos instalados com as outras threads congeladas, escrita só em estruturas validadas, correção de um defeito do alocador do jogo que derrubava o PES depois do apito final e um cache que mantém essa correção barata no loading. |
| **Testado** | 23 suítes, contratos conferidos no executável real, painel desenhado em 7 resoluções nos dois idiomas e tudo validado com AddressSanitizer e UndefinedBehaviorSanitizer. |

As notas de engenharia reversa estão em [`re_notes/`](re_notes), o histórico em [`CHANGELOG.md`](CHANGELOG.md) e as instruções de build e testes no [README em inglês](README.md#build--tests).

---

## Apoie o projeto

<table>
<tr>
<td width="160" align="center"><img src="docs/images/pix_qr.png" alt="QR Code Pix" width="140"></td>
<td>

Este mod é gratuito e de código aberto, e vai continuar assim. Se ele melhorou suas partidas e você quiser me pagar um café, vou ficar muito agradecido. Um relato de bug ou um obrigado vale tanto quanto.

**Pix:** escaneie o QR ou use a chave aleatória `925fd607-2011-4ca2-83a4-b3cf145b408b`<br>
**De fora do Brasil:** [![Ko-fi](https://img.shields.io/badge/Ko--fi-Pague_um_caf%C3%A9-FF5E5B?style=flat&logo=ko-fi&logoColor=white)](https://ko-fi.com/AlfaceJao)

</td>
</tr>
</table>

---

<div align="center">

Projeto independente, sem afiliação com a Konami. Nenhum arquivo do jogo é distribuído ou alterado.

[MIT](LICENSE) · Inclui [Dear ImGui](https://github.com/ocornut/imgui) ([MIT](native/third_party/imgui/LICENSE.txt))

</div>
