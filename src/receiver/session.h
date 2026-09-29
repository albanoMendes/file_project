#ifndef SESSION_H
#define SESSION_H

#include <netinet/in.h>
#include <stdint.h>

#include "protocol.h"

#define MAX_SESSIONS 64
#define MAX_PATH_LENGTH 1024

typedef enum {
    SESSION_IN_PROGRESS = 1,
    SESSION_COMPLETED = 2
} session_status_t;

/*
 * Estado de uma transferência mantido na memória pelo receiver.
 *
 * A parte indispensável desse estado também é salva no arquivo .meta,
 * permitindo recuperação se o processo receiver for encerrado.
 */
typedef struct {
    int active;

    uint32_t transfer_id;
    uint32_t expected_sequence;

    uint64_t file_size;
    uint64_t bytes_received;
    uint32_t expected_checksum;

    char filename[MAX_FILENAME + 1];
    char part_path[MAX_PATH_LENGTH];
    char meta_path[MAX_PATH_LENGTH];
    char final_path[MAX_PATH_LENGTH];

    struct sockaddr_in client_address;
    socklen_t client_address_length;

    session_status_t status;
} session_t;

void session_table_init(void);

int filename_is_safe(const char *filename);

session_t *session_find_by_transfer_id(uint32_t transfer_id);

session_t *session_find_by_filename(const char *filename);

session_t *session_create_or_resume(
    uint32_t transfer_id,
    const start_message_t *start,
    const char *storage_directory,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length
);

/*
 * Grava o bloco no .part, atualiza bytes/sequence e persiste .meta.
 * Deve ser chamado antes de o receiver enviar o ACK.
 */
int session_append_data(
    session_t *session,
    const unsigned char *data,
    uint32_t data_size
);

int session_finish(
    session_t *session,
    uint64_t finish_file_size,
    uint32_t finish_checksum
);

/*
 * Verifica se arquivo final já existe e coincide com START.
 *
 * Retornos:
 *  1: arquivo já está completo e é idêntico;
 *  0: arquivo final não existe;
 * -1: existe, mas é diferente, ou ocorreu erro.
 */
int session_completed_file_matches(
    const start_message_t *start,
    const char *storage_directory
);

void session_destroy(session_t *session);

#endif