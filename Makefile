CC := gcc

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 \
	-D_DEFAULT_SOURCE \
	-D_FILE_OFFSET_BITS=64

INCLUDES := -Isrc/common -Isrc/receiver

COMMON_SOURCES := \
	src/common/protocol.c \
	src/common/checksum.c

SENDER_SOURCES := \
	src/sender/sender.c \
	$(COMMON_SOURCES)

RECEIVER_SOURCES := \
	src/receiver/receiver.c \
	src/receiver/session.c \
	$(COMMON_SOURCES)

BIN_DIR := bin
STORAGE_DIR := storage
TEST_FILES_DIR := arquivos_testes

.PHONY: all clean dirs run-receiver run-web

all: dirs $(BIN_DIR)/sender $(BIN_DIR)/receiver

#
# Cria todos os diretórios usados pelo projeto.
#
# O -p não falha caso eles já existam.
#
dirs:
	mkdir -p $(BIN_DIR)
	mkdir -p $(STORAGE_DIR)
	mkdir -p $(TEST_FILES_DIR)

$(BIN_DIR)/sender: $(SENDER_SOURCES) | dirs
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SENDER_SOURCES)

$(BIN_DIR)/receiver: $(RECEIVER_SOURCES) | dirs
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(RECEIVER_SOURCES)

#
# Compila, cria os diretórios e inicia o receiver UDP.
#
run-receiver: all
	./$(BIN_DIR)/receiver 9000 ./$(STORAGE_DIR)

#
# Cria os diretórios e inicia a API HTTP.
#
run-web: dirs
	python3 src/web/web_server.py ./$(STORAGE_DIR) 8080

#
# Remove tudo que foi gerado:
# - binários compilados;
# - arquivos recebidos/parciais/metadados;
# - arquivos de entrada usados nos testes.
#
clean:
	rm -rf $(BIN_DIR) $(STORAGE_DIR) $(TEST_FILES_DIR)