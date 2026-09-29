Guia de Testes — Transferência de Arquivos Distribuída
Este guia explica como compilar, executar e testar o sistema de transferência confiável de arquivos via UDP.

O projeto possui três processos independentes:

Sender: cliente em C que envia um arquivo ao receiver usando UDP e Stop-and-Wait.

Receiver: servidor em C que recebe, valida, armazena e permite retomar uploads interrompidos.

Web server: servidor HTTP que disponibiliza GET /files e informa se cada arquivo está disponivel ou em_transferencia.

O objetivo dos testes é validar os requisitos principais da atividade:

Transferência UDP confiável usando Stop-and-Wait.

Retransmissão após timeout.

Numeração de sequência e ACKs.

Arquivos de vários tamanhos, inclusive vazio e último bloco parcial.

Retomada após interrupção do sender.

Recuperação após reinício do receiver.

Transferências concorrentes.

API REST GET /files.

Integridade dos arquivos transferidos.

1. Pré-requisitos
O guia foi preparado para Linux/WSL. É necessário ter:
 
# gcc
# make
# python3
# curl
# cmp
# sha256sum
# dd

Verifique rapidamente:

# gcc --version
# make --version
# python3 --version
# curl --version

2. Estrutura esperada
A partir da raiz do projeto, a estrutura básica é:

file_project/
├── Makefile
├── src/
│   ├── common/
│   │   ├── protocol.c
│   │   ├── protocol.h
│   │   ├── checksum.c
│   │   └── checksum.h
│   ├── sender/
│   │   └── sender.c
│   ├── receiver/
│   │   ├── receiver.c
│   │   ├── session.c
│   │   └── session.h
│   └── web/
│       └── web_server.py
├── bin/                 # criado pelo make
├── arquivos_testes/     # criado pelo make
└── storage/             # criado pelo make

Durante uma transferência incompleta, o receiver cria
# storage/nome_arquivo.part
# storage/nome_arquivo.meta

Obs: Os arquivos .part e .meta deixam de existir após a conclusão correta.

3. Compilar e iniciar
Todos os comandos deste guia devem ser executados na raiz do projeto:

# cd ~/file_project

Limpar o ambiente

# make clean

Esse comando remove artefatos gerados:

# bin/
# storage/
# arquivos_testes/

Compilar e preparar diretórios

# make 

Iniciar o receiver

Abra o Terminal 1:

# ./bin/receiver 9000 ./storage
ou
# make run-receiver 

Iniciar o servidor web
Abra o Terminal 2:

# python3 src/web/web_server.py ./storage 8080
ou
# make run-web

- Casos de teste

1. (Teste 01) API sem arquivos e rota inválida
Objetivo: validar o servidor HTTP e o endpoint GET /files antes de transferir arquivos.

Com o servidor web em execução:

# curl -i http://127.0.0.1:8080/files

2. Arquivo pequeno, um único bloco

Objetivo: validar o fluxo completo básico:

# printf "teste UDP\n" > arquivos_testes/teste.txt
    wc -c arquivos_testes/teste.txt

Enviar:

# ./bin/sender 127.0.0.1 ./arquivos_testes/teste.txt

Comparar os arquivos:

# cmp arquivos_testes/teste.txt storage/teste.txt
    echo $?

    sha256sum arquivos_testes/teste.txt storage/teste.txt

3. Múltiplos blocos completos
Objetivo: validar Stop-and-Wait com muitas sequências e ACKs.

Crie um arquivo de 100 KiB:

# dd if=/dev/urandom \
   of=arquivos_testes/arquivo_100kb.bin \
   bs=1024 \
   count=100

Envie: 
# ./bin/sender 127.0.0.1 ./arquivos_testes/arquivo_100kb.bin

Validar o arquivo: 

# cmp arquivos_testes/arquivo_100kb.bin storage/arquivo_100kb.bin
    echo $?

    sha256sum \
    arquivos_testes/arquivo_100kb.bin \
    storage/arquivo_100kb.bin

4. Último bloco parcial
Objetivo: garantir que o último pacote é gravado com seu tamanho real, não com 1024 bytes fixos.

Crie um arquivo de 2500 bytes:

# dd if=/dev/urandom \
   of=arquivos_testes/arquivo_2500.bin \
   bs=1 \
   count=2500

Enviar:

# ./bin/sender 127.0.0.1 ./arquivos_testes/arquivo_2500.bin

Validar o arquivo:

# cmp arquivos_testes/arquivo_2500.bin storage/arquivo_2500.bin
    echo $?

    sha256sum \
    arquivos_testes/arquivo_2500.bin \
    storage/arquivo_2500.bin

5. Arquivo vazio
Objetivo: validar o fluxo sem mensagens DATA:

Crie o arquivo vazio:

# touch arquivos_testes/vazio.txt
    wc -c arquivos_testes/vazio.txt

Enviar:

# ./bin/sender 127.0.0.1 ./arquivos_testes/vazio.txt

Validar o arquivo:

# cmp arquivos_testes/vazio.txt storage/vazio.txt
    echo $?

    sha256sum arquivos_testes/vazio.txt storage/vazio.txt
    ls -la storage/vazio.txt*

6. Arquivo de 10 MiB
Objetivo: validar uma transferência maior e a limitação de memória por blocos.

Crie o arquivo:

# dd if=/dev/urandom \
   of=arquivos_testes/arquivo_10mb.bin \
   bs=1M \
   count=10

Eniviar:

# ./bin/sender 127.0.0.1 ./arquivos_testes/arquivo_10mb.bin

Validar o arquivo:

# cmp arquivos_testes/arquivo_10mb.bin storage/arquivo_10mb.bin
    echo $?

    sha256sum \
    arquivos_testes/arquivo_10mb.bin \
    storage/arquivo_10mb.bin

    ls -la storage/arquivo_10mb.bin*

7. (Teste 7: ) API durante e após transferência
Objetivo: validar a mudança de estado:
# em_transferencia → disponivel

Crie um arquivo novo:

# dd if=/dev/urandom \
   of=arquivos_testes/web_status_10mb.bin \
   bs=1M \
   count=10

Inicie o sender com atraso de 1 ms entre ACKs:

# ./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/web_status_10mb.bin \
  9000 \
  1000

Enquanto o sender ainda está executando, em outro terminal:

# curl -s http://127.0.0.1:8080/files

Depois que o sender concluir, consulte novamente:

# curl -s http://127.0.0.1:8080/files

Valide o conteúdo:

# cmp \
  arquivos_testes/web_status_10mb.bin \
  storage/web_status_10mb.bin

echo $?

T8 — Interrupção do sender e retomada
Objetivo: comprovar que a transferência é retomada sem duplicação ou corrupção após interrupção do cliente.

Crie o arquivo:

# dd if=/dev/urandom \
   of=arquivos_testes/retomada_10mb.bin \
   bs=1M \
   count=10

Inicie com atraso:

# ./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/retomada_10mb.bin \
  9000 \
  1000

Enquanto o upload está em andamento, interrompa o sender com:

# Ctrl+C

Verifique o estado persistido:

# ls -lh storage/retomada_10mb.bin*
# cat storage/retomada_10mb.bin.meta

Consulte a API:

# curl -s http://127.0.0.1:8080/files

Retome usando o mesmo comando:

# ./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/retomada_10mb.bin \
  9000 \
  1000

Esperado no sender:

RESUME: proximo_bloco=N, bytes_recebidos=M

Ao terminar:

# cmp \
  arquivos_testes/retomada_10mb.bin \
  storage/retomada_10mb.bin

echo $?

sha256sum \
  arquivos_testes/retomada_10mb.bin \
  storage/retomada_10mb.bin

ls -la storage/retomada_10mb.bin*

9. (Teste 09): Receiver indisponível e timeout
Objetivo: validar timeout e retransmissão quando não há receiver respondendo.

# Pare o receiver com Ctrl+C.

Crie um arquivo pequeno:

# printf "teste de timeout\n" > arquivos_testes/timeout.txt

Tente enviar:

# ./bin/sender 127.0.0.1 ./arquivos_testes/timeout.txt

- Reinicie o receiver para os próximos testes:

# make run-receiver

10.  (Teste 10) Reinício do receiver durante transferência
Objetivo: validar recuperação do estado persistente após falha do receiver.

Crie um arquivo novo:

# dd if=/dev/urandom \
   of=arquivos_testes/reinicio_receiver_10mb.bin \
   bs=1M \
   count=10

Inicie o sender com atraso:


# ./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/reinicio_receiver_10mb.bin \
  9000 \
  1000

Durante o upload, interrompa o receiver com:

# Ctrl+C

O sender deve começar a mostrar timeouts de DATA.

Reinicie o receiver antes de esgotar as tentativas:

# make run-receiver

Valide o arquivo:

# cmp \
  arquivos_testes/reinicio_receiver_10mb.bin \
  storage/reinicio_receiver_10mb.bin

echo $?

sha256sum \
  arquivos_testes/reinicio_receiver_10mb.bin \
  storage/reinicio_receiver_10mb.bin

11. (Teste 11): Dois clientes concorrentes, nomes diferentes

Objetivo: validar que o receiver mantém estados independentes para transferências concorrentes.

Crie dois arquivos:

# dd if=/dev/urandom \
   of=arquivos_testes/cliente_a.bin \
   bs=1M \
   count=5

# dd if=/dev/urandom \
   of=arquivos_testes/cliente_b.bin \
   bs=1M \
   count=5

No Terminal 3:

# ./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/cliente_a.bin \
  9000 \
  1000

No Terminal 4, imediatamente depois:

# ./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/cliente_b.bin \
  9000 \
  1000

Durante as transferências:

# curl -s http://127.0.0.1:8080/files

Esperado: 
ambos aparecem com em_transferencia.

Depois da conclusão:

# cmp arquivos_testes/cliente_a.bin storage/cliente_a.bin
echo $?

# cmp arquivos_testes/cliente_b.bin storage/cliente_b.bin
echo $?

# sha256sum arquivos_testes/cliente_a.bin storage/cliente_a.bin

# ha256sum arquivos_testes/cliente_b.bin storage/cliente_b.bin

12. (Teste 12) Colisão de nome entre clientes

Objetivo: confirmar que dois clientes não escrevem ao mesmo tempo no mesmo .part.

Crie dois arquivos diferentes, mas com o mesmo nome-base:

# mkdir -p arquivos_testes/cliente_1
# mkdir -p arquivos_testes/cliente_2

# dd if=/dev/urandom \
   of=arquivos_testes/cliente_1/mesmo_nome.bin \
   bs=1M \
   count=5

# dd if=/dev/urandom \
   of=arquivos_testes/cliente_2/mesmo_nome.bin \
   bs=1M \
   count=5

Inicie os dois senders em terminais diferentes:

# bash
./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/cliente_1/mesmo_nome.bin \
  9000 \
  1000

# bash
./bin/sender \
  127.0.0.1 \
  ./arquivos_testes/cliente_2/mesmo_nome.bin \
  9000 \
  1000

Verifique qual arquivo venceu:

# sha256sum \
  arquivos_testes/cliente_1/mesmo_nome.bin \
  arquivos_testes/cliente_2/mesmo_nome.bin \
  storage/mesmo_nome.bin

13. (Teste 13) — Reenvio de arquivo já concluído

Objetivo: verificar o comportamento de upload repetido de um arquivo finalizado.

Envie novamente um arquivo concluído:

# ./bin/sender 127.0.0.1 ./arquivos_testes/teste.txt

14.  (Teste 14) Arquivo grande, até 1 GiB

Objetivo: demonstrar que a implementação lê e grava em blocos, usa contadores de 64 bits e pode suportar o limite solicitado de até 1 GB.

  - Obs: Este teste pode demorar bastante porque o protocolo Stop-and-Wait espera um ACK para cada bloco de 1024 bytes. Garanta espaço em disco antes de começar.

Verifique espaço:
# df -h .

Crie 1 GiB:

# dd if=/dev/zero \
   of=arquivos_testes/grande_1gb.bin \
   bs=1M \
   count=1024 \
   status=progress

Envie:

# ./bin/sender 127.0.0.1 ./arquivos_testes/grande_1gb.bin


Valide:

# cmp \
  arquivos_testes/grande_1gb.bin \
  storage/grande_1gb.bin

echo $?