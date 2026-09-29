#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

/*
 * Máquinas x86 normalmente usam little-endian.
 * A rede usa big-endian.
 *
 * Esta função converte um inteiro de 64 bits para network byte order.
 */
uint64_t htonll(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t lower_32 = (uint32_t)(value & 0xFFFFFFFFULL);
    uint32_t upper_32 = (uint32_t)(value >> 32);

    return ((uint64_t)htonl(lower_32) << 32) | htonl(upper_32);
#else
    return value;
#endif
}

/*
 * A conversão inversa é igual à conversão direta:
 * inverter os bytes duas vezes recupera o valor original.
 */
uint64_t ntohll(uint64_t value) {
    return htonll(value);
}

/*
 * Cria um datagrama com layout:
 *
 * [version][type][header_size][transfer_id][sequence][payload_size][payload]
 *
 * Os campos numéricos do header são convertidos para network byte order
 * antes de serem copiados para o buffer.
 */
int build_packet(
    unsigned char *buffer,
    size_t buffer_capacity,
    message_type_t type,
    uint32_t transfer_id,
    uint32_t sequence,
    const void *payload,
    uint32_t payload_size
) {
    packet_header_t network_header;

    if (buffer == NULL) {
        return -1;
    }

    if (payload_size > MAX_PAYLOAD) {
        return -1;
    }

    if (buffer_capacity < PACKET_HEADER_SIZE + payload_size) {
        return -1;
    }

    network_header.version = PROTOCOL_VERSION;
    network_header.type = (uint8_t)type;
    network_header.header_size = htons(PACKET_HEADER_SIZE);
    network_header.transfer_id = htonl(transfer_id);
    network_header.sequence = htonl(sequence);
    network_header.payload_size = htonl(payload_size);

    memcpy(buffer, &network_header, PACKET_HEADER_SIZE);

    if (payload_size > 0 && payload != NULL) {
        memcpy(buffer + PACKET_HEADER_SIZE, payload, payload_size);
    }

    return PACKET_HEADER_SIZE + (int)payload_size;
}

/*
 * Faz a operação inversa de build_packet().
 *
 * Primeiro copia o header recebido, depois converte números para a ordem
 * local da máquina e valida todos os tamanhos antes de usar o payload.
 */
int parse_packet(
    const unsigned char *buffer,
    size_t received_size,
    packet_header_t *header,
    const unsigned char **payload
) {
    packet_header_t network_header;

    if (buffer == NULL || header == NULL || payload == NULL) {
        return -1;
    }

    if (received_size < PACKET_HEADER_SIZE) {
        return -1;
    }

    memcpy(&network_header, buffer, PACKET_HEADER_SIZE);

    header->version = network_header.version;
    header->type = network_header.type;
    header->header_size = ntohs(network_header.header_size);
    header->transfer_id = ntohl(network_header.transfer_id);
    header->sequence = ntohl(network_header.sequence);
    header->payload_size = ntohl(network_header.payload_size);

    if (header->version != PROTOCOL_VERSION) {
        return -1;
    }

    if (header->header_size != PACKET_HEADER_SIZE) {
        return -1;
    }

    if (header->payload_size > MAX_PAYLOAD) {
        return -1;
    }

    if (received_size != PACKET_HEADER_SIZE + header->payload_size) {
        return -1;
    }

    *payload = buffer + PACKET_HEADER_SIZE;
    return 0;
}

/*
 * Serializa os metadados do START.
 *
 * O sender calcula tamanho e CRC32 do arquivo e envia:
 * - tamanho total;
 * - CRC32 esperado;
 * - tamanho do nome;
 * - nome do arquivo.
 */
int serialize_start_payload(
    const start_message_t *start,
    unsigned char *buffer,
    size_t buffer_capacity
) {
    uint64_t network_file_size;
    uint32_t network_checksum;
    uint16_t network_filename_length;
    size_t total_size;

    if (start == NULL || buffer == NULL) {
        return -1;
    }

    if (start->filename_length == 0 ||
        start->filename_length > MAX_FILENAME) {
        return -1;
    }

    total_size =
        sizeof(uint64_t) +
        sizeof(uint32_t) +
        sizeof(uint16_t) +
        start->filename_length;

    if (total_size > MAX_PAYLOAD || buffer_capacity < total_size) {
        return -1;
    }

    network_file_size = htonll(start->file_size);
    network_checksum = htonl(start->checksum);
    network_filename_length = htons(start->filename_length);

    size_t offset = 0;

    memcpy(buffer + offset, &network_file_size, sizeof(network_file_size));
    offset += sizeof(network_file_size);

    memcpy(buffer + offset, &network_checksum, sizeof(network_checksum));
    offset += sizeof(network_checksum);

    memcpy(
        buffer + offset,
        &network_filename_length,
        sizeof(network_filename_length)
    );
    offset += sizeof(network_filename_length);

    memcpy(buffer + offset, start->filename, start->filename_length);

    return (int)total_size;
}

/*
 * Interpreta o payload de START recebido pelo receiver.
 */
int deserialize_start_payload(
    const unsigned char *buffer,
    size_t payload_size,
    start_message_t *start
) {
    uint64_t network_file_size;
    uint32_t network_checksum;
    uint16_t network_filename_length;
    uint16_t filename_length;
    size_t offset = 0;
    size_t expected_size;

    if (buffer == NULL || start == NULL) {
        return -1;
    }

    if (payload_size < sizeof(uint64_t) +
                       sizeof(uint32_t) +
                       sizeof(uint16_t)) {
        return -1;
    }

    memcpy(&network_file_size, buffer + offset, sizeof(network_file_size));
    offset += sizeof(network_file_size);

    memcpy(&network_checksum, buffer + offset, sizeof(network_checksum));
    offset += sizeof(network_checksum);

    memcpy(
        &network_filename_length,
        buffer + offset,
        sizeof(network_filename_length)
    );
    offset += sizeof(network_filename_length);

    filename_length = ntohs(network_filename_length);

    if (filename_length == 0 || filename_length > MAX_FILENAME) {
        return -1;
    }

    expected_size = offset + filename_length;

    if (payload_size != expected_size) {
        return -1;
    }

    start->file_size = ntohll(network_file_size);
    start->checksum = ntohl(network_checksum);
    start->filename_length = filename_length;

    memcpy(start->filename, buffer + offset, filename_length);
    start->filename[filename_length] = '\0';

    return 0;
}

/*
 * Serializa o estado de retomada devolvido pelo receiver.
 */
int serialize_resume_payload(
    const resume_message_t *resume,
    unsigned char *buffer,
    size_t buffer_capacity
) {
    uint32_t network_sequence;
    uint64_t network_bytes_received;
    size_t offset = 0;

    if (resume == NULL || buffer == NULL) {
        return -1;
    }

    if (buffer_capacity < sizeof(uint32_t) + sizeof(uint64_t)) {
        return -1;
    }

    network_sequence = htonl(resume->next_sequence);
    network_bytes_received = htonll(resume->bytes_received);

    memcpy(buffer + offset, &network_sequence, sizeof(network_sequence));
    offset += sizeof(network_sequence);

    memcpy(
        buffer + offset,
        &network_bytes_received,
        sizeof(network_bytes_received)
    );

    return (int)(sizeof(network_sequence) + sizeof(network_bytes_received));
}

/*
 * Interpreta o RESUME recebido pelo sender.
 */
int deserialize_resume_payload(
    const unsigned char *buffer,
    size_t payload_size,
    resume_message_t *resume
) {
    uint32_t network_sequence;
    uint64_t network_bytes_received;
    size_t offset = 0;

    if (buffer == NULL || resume == NULL) {
        return -1;
    }

    if (payload_size != sizeof(uint32_t) + sizeof(uint64_t)) {
        return -1;
    }

    memcpy(&network_sequence, buffer + offset, sizeof(network_sequence));
    offset += sizeof(network_sequence);

    memcpy(
        &network_bytes_received,
        buffer + offset,
        sizeof(network_bytes_received)
    );

    resume->next_sequence = ntohl(network_sequence);
    resume->bytes_received = ntohll(network_bytes_received);

    return 0;
}

/*
 * Serializa FINISH: o sender repete tamanho e CRC32.
 * O receiver usa esses valores para validar a sessão antes de publicar
 * o arquivo como disponível.
 */
int serialize_finish_payload(
    const finish_message_t *finish,
    unsigned char *buffer,
    size_t buffer_capacity
) {
    uint64_t network_file_size;
    uint32_t network_checksum;
    size_t offset = 0;

    if (finish == NULL || buffer == NULL) {
        return -1;
    }

    if (buffer_capacity < sizeof(uint64_t) + sizeof(uint32_t)) {
        return -1;
    }

    network_file_size = htonll(finish->file_size);
    network_checksum = htonl(finish->checksum);

    memcpy(buffer + offset, &network_file_size, sizeof(network_file_size));
    offset += sizeof(network_file_size);

    memcpy(buffer + offset, &network_checksum, sizeof(network_checksum));

    return (int)(sizeof(network_file_size) + sizeof(network_checksum));
}

/*
 * Interpreta FINISH recebido pelo receiver.
 */
int deserialize_finish_payload(
    const unsigned char *buffer,
    size_t payload_size,
    finish_message_t *finish
) {
    uint64_t network_file_size;
    uint32_t network_checksum;
    size_t offset = 0;

    if (buffer == NULL || finish == NULL) {
        return -1;
    }

    if (payload_size != sizeof(uint64_t) + sizeof(uint32_t)) {
        return -1;
    }

    memcpy(&network_file_size, buffer + offset, sizeof(network_file_size));
    offset += sizeof(network_file_size);

    memcpy(&network_checksum, buffer + offset, sizeof(network_checksum));

    finish->file_size = ntohll(network_file_size);
    finish->checksum = ntohl(network_checksum);

    return 0;
}