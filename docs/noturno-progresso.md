# Turno noturno - progresso

Checklist de uma linha por etapa. `[x]` feita, `[~]` parcial, `[ ]` nao comecada.
Este arquivo permite retomar o trabalho sem ler o historico da conversa.

Base do turno: commit `fe29851` (v1.0), `.\check.ps1` limpo, regressao verde.

| # | Etapa | Estado | Inicio | Fim | Commit |
| --- | --- | --- | --- | --- | --- |
| 4.1 | Filas de espera e poll (v1.1) | [ ] | 22:52 | - | - |
| 4.2 | Sinais (v1.2) | [ ] | - | - | - |
| 4.3 | Escrita no FAT e redirecionamentos (v1.3) | [ ] | - | - | - |
| 4.4 | Teclas estendidas e edicao de linha (v1.4) | [ ] | - | - | - |
| 4.5 | Fork copy-on-write (v1.5) | [ ] | - | - | - |
| 4.6 | Rede (v1.6) | [ ] | - | - | - |
| 4.7 | GUI: boot direto e apps (v1.7) | [ ] | - | - | - |
| 4.8 | Extras | [ ] | - | - | - |

## Como retomar

    cd c:\dev\orion
    .\check.ps1          # precisa terminar com "Nenhum erro ou warning."
    .\regressao.ps1      # sequencias A (texto) e B (grafica); exit 0 = verde
    git log --oneline -5

A proxima etapa e a primeira linha com `[ ]` na tabela acima.

## Diario

### 4.1 Filas de espera e poll - inicio 22:52
Objetivo: trocar toda espera ocupada (`thread_sleep_ms(2..5)`) por filas de
espera de verdade, e expor `SYS_POLL`.
