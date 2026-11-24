# Changelog

## v4.0

- **Correção de crash ao aplicar decisão**: a falta deixada na máquina de estados da partida passa a usar os sentinelas da própria engine (`0xFF`) e um snapshot existente (crash em `PES2021.exe+0x4B4C25`).
- **Nomes e números reais**: número da camisa lido da ficha do time (`TeamEntry + 0x29A`), sem nomes genéricos de reserva.
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
