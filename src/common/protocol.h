#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_VERSION 1
#define DEFAULT_PORT 9000

/*
 * Quantidade máxima de bytes do ARQUIVO em cada DATA.
 *
 * O enunciado limita o uso de memória a 32.768 bytes por vez.
 * 1024 é uma escolha conservadora para evitar fragmentação IP em LAN
 * e está muito abaixo do limite de memória.
 */
#define MAX_PAYLOAD 1024

#define MAX_FILENAME 255
#define MAX_ERROR_MESSAGE 255

/*
 * Layout do cabeçalho no datagrama:
 *
 * byte 0:       version       (1 byte)
 * byte 1:       type          (1 byte)
 * bytes 2..3:   header_size   (2 bytes)
 * bytes 4..7:   transfer_id   (4 bytes)
 * bytes 8..11:  sequence      (4 bytes)
 * bytes 12..15: payload_size  (4 bytes)
 */
#define PACKET_HEADER_SIZE 16
#define MAX_PACKET_SIZE (PACKET_HEADER_SIZE + MAX_PAYLOAD)

typedef enum {
    MSG_START  = 1,
    MSG_RESUME = 2,
    MSG_DATA   = 3,
    MSG_ACK    = 4,
    MSG_FINISH = 5,
    MSG_DONE   = 6,
    MSG_ERROR  = 7
} message_type_t;

/*
 * Códigos de erro enviados no payload de MSG_ERROR.
 *
 * O sender pode tomar ações diferentes dependendo do erro:
 * - SESSION_NOT_FOUND: refaz START e retoma.
 * - outros: encerra e mostra a mensagem.
 */
typedef enum {
    ERR_UNKNOWN = 0,
    ERR_SESSION_NOT_FOUND = 1,
    ERR_INVALID_PACKET = 2,
    ERR_OUT_OF_ORDER = 3,
    ERR_FILE_BUSY = 4,
    ERR_STORAGE = 5,
    ERR_INTEGRITY = 6
} error_code_t;
/*
 * Esta struct representa o header DEPOIS de convertido para host byte order.
 * Não a envie diretamente pela rede; build_packet() faz a serialização.
 */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t header_size;
    uint32_t transfer_id;
    uint32_t sequence;
    uint32_t payload_size;
} packet_header_t;

/*
 * Payload de START:
 *
 * +--------------------+----------------------+-----------------+-------------------+
 * | file_size (8 bytes)| checksum (4 bytes)   | filename_len(2) | filename (N b.)  |
 * +--------------------+----------------------+-----------------+-------------------+
 */
typedef struct {
    uint64_t file_size;
    uint32_t checksum;
    uint16_t filename_length;
    char filename[MAX_FILENAME + 1];
} start_message_t;

/*
 * Payload de RESUME:
 *
 * +--------------------------+------------------------------+
 * | next_sequence (4 bytes)  | bytes_received (8 bytes)     |
 * +--------------------------+------------------------------+
 */
typedef struct {
    uint32_t next_sequence;
    uint64_t bytes_received;
} resume_message_t;

/*
 * Payload de FINISH:
 *
 * +--------------------+----------------------+
 * | file_size (8 bytes)| checksum (4 bytes)   |
 * +--------------------+----------------------+
 */
typedef struct {
    uint64_t file_size;
    uint32_t checksum;
} finish_message_t;

/*
 * Converte 64 bits entre host byte order e network byte order.
 * Para 16 e 32 bits usamos htons/htonl/ntohs/ntohl da biblioteca padrão.
 */
uint64_t htonll(uint64_t value);
uint64_t ntohll(uint64_t value);

/*
 * Monta:
 *
 * +----------------------------------------------+
 * | header de 16 bytes | payload de N bytes      |
 * +----------------------------------------------+
 *
 * Retorna tamanho total do datagrama ou -1 em erro.
 */
int build_packet(
    unsigned char *buffer,
    size_t buffer_capacity,
    message_type_t type,
    uint32_t transfer_id,
    uint32_t sequence,
    const void *payload,
    uint32_t payload_size
);

/*
 * Valida e interpreta um datagrama recebido.
 *
 * header recebe valores em host byte order.
 * payload aponta para dentro de buffer.
 *
 * Retorna 0 em sucesso e -1 em pacote inválido.
 */
int parse_packet(
    const unsigned char *buffer,
    size_t received_size,
    packet_header_t *header,
    const unsigned char **payload
);

int serialize_start_payload(
    const start_message_t *start,
    unsigned char *buffer,
    size_t buffer_capacity
);

int deserialize_start_payload(
    const unsigned char *buffer,
    size_t payload_size,
    start_message_t *start
);

int serialize_resume_payload(
    const resume_message_t *resume,
    unsigned char *buffer,
    size_t buffer_capacity
);

int deserialize_resume_payload(
    const unsigned char *buffer,
    size_t payload_size,
    resume_message_t *resume
);

int serialize_finish_payload(
    const finish_message_t *finish,
    unsigned char *buffer,
    size_t buffer_capacity
);

int deserialize_finish_payload(
    const unsigned char *buffer,
    size_t payload_size,
    finish_message_t *finish
);

#endif