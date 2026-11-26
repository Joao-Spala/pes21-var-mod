# Liberação de segmentos do heap da engine (crash pós-partida)

Pesquisa da v4.4.1. Endereços do `PES2021.exe` Steam `1.7.2.0`. **[P]** comprovado por disassembly ou dump, **[I]** inferido.

## 1. Sintoma

- Depois do apito final, ao avançar, o jogo travava e, ao ser fechado, gerava o dump. Os dois dumps (`PES2021.exe.4568.dmp` da v4.2.1 e `PES2021.exe.19524.dmp`, completo, da v4.2.4) têm a mesma falha: escrita em memória não mapeada em `PES2021.exe+0x3FE31D`, com o mesmo estado de heap `rsi = 0x56F0010` **[P]**.
- O travamento é o próprio crash: o Windows grava um dump completo de cerca de 5 GB antes de encerrar o processo **[I]**.

## 2. Onde

- A thread que falha está na máquina de estados de cena (`0x141FE0A80` em diante), que chama `0x1403FB8B0`. Essa função percorre os 50 heaps em `*(0x1436FD708)` e chama o método `+0x50` de cada um, que compacta o heap **[P]**.
- O heap é um dlmalloc: segmentos com rodapé `{base, tamanho, próximo, flags}` nos últimos `0x40` bytes, bins em `mstate + 0x258`, total em `+0x358` **[P]**.
- `release_unused_segments` (laço em `0x1403FE1C0`) chama o `win32munmap` (`0x1403FC0D0`) para cada segmento livre. Se ele devolve `-1`, o código assume que nada foi liberado e reinsere o chunk nos bins escrevendo em `chunk + 0x38` (`0x1403FE31D`) **[P]**.

## 3. Por que o win32munmap falha depois de liberar

- O `win32munmap` percorre o intervalo com `VirtualQuery` e exige, em cada trecho, `BaseAddress == AllocationBase == endereço`, estado `MEM_COMMIT` e tamanho dentro do que falta. Para cada trecho chama `VirtualFree(MEM_RELEASE)` **[P]**.
- `VirtualFree(MEM_RELEASE)` sempre libera a alocação inteira. Quando uma alocação tem páginas com proteções diferentes, o `VirtualQuery` devolve só o primeiro trecho. A função libera a alocação toda, consulta o trecho seguinte, encontra memória livre e devolve `-1` **[P]**.
- No dump completo, a região imediatamente anterior ao segmento que caiu (`0x118500000`) tem páginas soltas `PAGE_EXECUTE_READWRITE` (`0x40`) dentro de um bloco `PAGE_READWRITE`. A própria página do `mstate` em `0x56F0000` está como `0x40` **[P]**.
- O segmento que caiu (`0x118550000`, tamanho `0x50000`) estava mapeado antes da chamada, porque o laço lê `0x18(%rbx)` e `0x30(%rbx)` antes de chamar a liberação, e estava totalmente livre no dump. Isso confirma a liberação pela metade dentro da própria chamada **[P]**.

## 4. Quem muda a proteção

- O `var_core.dll` só chama `VirtualProtect` em código (`.text`), vtables e IAT, nunca em dados de heap **[P]**.
- Dezenas de módulos Lua do BMPES usam `memory.write` do Sider em endereços de dados do jogo **[P]**. Deixar páginas com proteção de execução é o efeito observado **[I]**.
- O defeito independe do VAR. Mais atividade na partida só aumenta a chance de um segmento afetado ser liberado no trim **[I]**.

## 5. Correção

- `heap_guard.cpp` substitui o `win32munmap` (AOB único, hint `0x3FC0D0`) por uma versão atômica:
  - percorre o intervalo por alocação (`AllocationBase`), não por trecho de proteção;
  - valida o intervalo inteiro antes de liberar qualquer coisa: cada alocação começa onde a anterior termina, está toda em `MEM_COMMIT` e cabe no intervalo;
  - só então libera alocação por alocação e devolve `0`. Se a validação falha, devolve `-1` sem tocar em nada, e o dlmalloc mantém o chunk mapeado.
- Os seis chamadores (`0x1403FC717`, `0x1403FC77A`, `0x1403FD410`, `0x1403FE2C3`, `0x1403FE6D3`, `0x1403FE7D8`) tratam `0` e `-1` com o mesmo contrato **[P]**.
- O gancho é instalado no `var_init`, separado dos ganchos da partida, e fica ativo no processo inteiro.
- Testes: padrão no binário real, planejamento com o layout do dump e liberação real com uma página `PAGE_EXECUTE_READWRITE` no meio da alocação.
