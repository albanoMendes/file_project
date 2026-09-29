#include "session.h"

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Envia um pacote de resposta ao cliente que originou o datagrama.
 *
 * É usado para RESUME, ACK, DONE e ERROR.
 */
static int send_packet_to_client(
    int sockfd,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length,
    message_type_t type,
    uint32_t transfer_id,
    uint32_t sequence,
    const void *payload,
    uint32_t payload_size
) {
    unsigned char packet[MAX_PACKET_SIZE];

    int packet_size = build_packet(
        packet,
        sizeof(packet),
        type,
        transfer_id,
        sequence,
        payload,
        payload_size
    );

    if (packet_size < 0) {
        return -1;
    }

    ssize_t sent = sendto(
        sockfd,
        packet,
        (size_t)packet_size,
        0,
        (const struct sockaddr *)client_address,
        client_address_length
    );

    return sent == packet_size ? 0 : -1;
}

/*
 * Envia uma resposta MSG_ERROR com texto legível.
 *
 * O sender mostra essa mensagem no terminal e encerra o upload.
 */
static void send_error(
    int sockfd,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length,
    uint32_t transfer_id,
    uint32_t sequence,
    const char *message
) {
    size_t message_size = strlen(message);

    if (message_size > MAX_ERROR_MESSAGE) {
        message_size = MAX_ERROR_MESSAGE;
    }

    if (send_packet_to_client(
            sockfd,
            client_address,
            client_address_length,
            MSG_ERROR,
            transfer_id,
            sequence,
            message,
            (uint32_t)message_size
        ) != 0) {
        perror("sendto(MSG_ERROR)");
    }
}

/*
 * Formata IP:porta do cliente para logs.
 */
static void print_client(const struct sockaddr_in *client_address) {
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(
            AF_INET,
            &client_address->sin_addr,
            ip,
            sizeof(ip)
        ) == NULL) {
        snprintf(ip, sizeof(ip), "?");
    }

    printf("%s:%u", ip, ntohs(client_address->sin_port));
}

/*
 * Trata START.
 *
 * Cenários:
 *
 * A) Arquivo final já existe e é idêntico:
 *    responde DONE; operação idempotente.
 *
 * B) Há sessão existente/parcial compatível:
 *    responde RESUME com o próximo bloco.
 *
 * C) Não existe estado:
 *    cria .part/.meta e responde RESUME(seq=0).
 */
static void handle_start(
    int sockfd,
    const packet_header_t *header,
    const unsigned char *payload,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length,
    const char *storage_directory
) {
    start_message_t start;
    resume_message_t resume;
    unsigned char response_payload[MAX_PAYLOAD];

    if (deserialize_start_payload(
            payload,
            header->payload_size,
            &start
        ) != 0) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "START invalido"
        );
        return;
    }

    if (!filename_is_safe(start.filename)) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Nome de arquivo invalido"
        );
        return;
    }

    /*
     * Primeiro verifica se o arquivo já foi enviado completamente.
     */
    int completed = session_completed_file_matches(
        &start,
        storage_directory
    );

    if (completed == 1) {
        if (send_packet_to_client(
                sockfd,
                client_address,
                client_address_length,
                MSG_DONE,
                header->transfer_id,
                0,
                NULL,
                0
            ) != 0) {
            perror("sendto(MSG_DONE idempotente)");
        }

        printf("START idempotente: ");
        print_client(client_address);
        printf(
            " id=%u arquivo=%s ja disponivel\n",
            header->transfer_id,
            start.filename
        );
        return;
    }

    if (completed < 0) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Arquivo final ja existe, mas nao corresponde ao upload"
        );
        return;
    }

    session_t *session = session_create_or_resume(
        header->transfer_id,
        &start,
        storage_directory,
        client_address,
        client_address_length
    );

    if (session == NULL) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Nao foi possivel criar/retomar sessao ou arquivo esta ocupado"
        );
        return;
    }

    resume.next_sequence = session->expected_sequence;
    resume.bytes_received = session->bytes_received;

    int response_size = serialize_resume_payload(
        &resume,
        response_payload,
        sizeof(response_payload)
    );

    if (response_size < 0) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Erro interno ao montar RESUME"
        );
        return;
    }

    if (send_packet_to_client(
            sockfd,
            client_address,
            client_address_length,
            MSG_RESUME,
            header->transfer_id,
            0,
            response_payload,
            (uint32_t)response_size
        ) != 0) {
        perror("sendto(MSG_RESUME)");
        return;
    }

    printf("START: ");
    print_client(client_address);
    printf(
        " id=%u arquivo=%s tamanho=%llu bytes_recebidos=%llu proximo_seq=%u\n",
        header->transfer_id,
        start.filename,
        (unsigned long long)start.file_size,
        (unsigned long long)session->bytes_received,
        session->expected_sequence
    );
}

/*
 * Trata DATA.
 *
 * Stop-and-Wait garante que o sender normalmente manda apenas a sequência
 * esperada. Ainda assim, o receiver trata todos os casos:
 *
 * seq == expected:
 *   bloco novo; grava e responde ACK.
 *
 * seq < expected:
 *   duplicata; provavelmente o ACK anterior foi perdido.
 *   Não grava novamente; apenas reenvia ACK.
 *
 * seq > expected:
 *   fora de ordem; responde ERROR.
 */
static void handle_data(
    int sockfd,
    const packet_header_t *header,
    const unsigned char *payload,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length
) {
    session_t *session = session_find_by_transfer_id(header->transfer_id);

    if (session == NULL) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Sessao inexistente; envie START"
        );
        return;
    }

    if (header->payload_size == 0 ||
        header->payload_size > MAX_PAYLOAD) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "DATA invalido"
        );
        return;
    }

    if (header->sequence == session->expected_sequence) {
        /*
         * Só envia ACK depois de persistir o bloco e salvar .meta.
         */
        if (session_append_data(
                session,
                payload,
                header->payload_size
            ) != 0) {
            send_error(
                sockfd,
                client_address,
                client_address_length,
                header->transfer_id,
                header->sequence,
                "Falha ao gravar bloco"
            );
            return;
        }

        if (send_packet_to_client(
                sockfd,
                client_address,
                client_address_length,
                MSG_ACK,
                header->transfer_id,
                header->sequence,
                NULL,
                0
            ) != 0) {
            perror("sendto(MSG_ACK)");
            return;
        }

        printf(
            "DATA: id=%u seq=%u bytes=%u total=%llu/%llu\n",
            header->transfer_id,
            header->sequence,
            header->payload_size,
            (unsigned long long)session->bytes_received,
            (unsigned long long)session->file_size
        );

        return;
    }

    if (header->sequence < session->expected_sequence) {
        /*
         * O DATA já foi gravado anteriormente.
         * Reenviar ACK é essencial para a confiabilidade do Stop-and-Wait.
         */
        if (send_packet_to_client(
                sockfd,
                client_address,
                client_address_length,
                MSG_ACK,
                header->transfer_id,
                header->sequence,
                NULL,
                0
            ) != 0) {
            perror("sendto(ACK duplicado)");
        }

        printf(
            "DATA duplicado: id=%u seq=%u; ACK reenviado\n",
            header->transfer_id,
            header->sequence
        );

        return;
    }

    send_error(
        sockfd,
        client_address,
        client_address_length,
        header->transfer_id,
        header->sequence,
        "Bloco fora de ordem"
    );
}

/*
 * Trata FINISH.
 *
 * Receiver só publica o arquivo final se:
 * - bytes_received == file_size;
 * - CRC32 do .part == checksum anunciado;
 * - FINISH repete os mesmos dados de START.
 */
static void handle_finish(
    int sockfd,
    const packet_header_t *header,
    const unsigned char *payload,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length
) {
    finish_message_t finish;

    if (deserialize_finish_payload(
            payload,
            header->payload_size,
            &finish
        ) != 0) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "FINISH invalido"
        );
        return;
    }

    session_t *session = session_find_by_transfer_id(header->transfer_id);

    if (session == NULL) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Sessao inexistente"
        );
        return;
    }

    if (session_finish(
            session,
            finish.file_size,
            finish.checksum
        ) != 0) {
        send_error(
            sockfd,
            client_address,
            client_address_length,
            header->transfer_id,
            header->sequence,
            "Arquivo incompleto ou checksum invalido"
        );
        return;
    }

    if (send_packet_to_client(
            sockfd,
            client_address,
            client_address_length,
            MSG_DONE,
            header->transfer_id,
            header->sequence,
            NULL,
            0
        ) != 0) {
        perror("sendto(MSG_DONE)");
        return;
    }

    printf(
        "FINISH: id=%u arquivo=%s tamanho=%llu checksum=%u\n",
        header->transfer_id,
        session->filename,
        (unsigned long long)session->bytes_received,
        finish.checksum
    );

    /*
     * Neste ponto .part virou arquivo final e .meta foi removido.
     * A entrada em memória pode ser liberada.
     */
    session_destroy(session);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    const char *storage_directory = ".";
    struct stat storage_info;

    if (argc > 3) {
        fprintf(stderr, "Uso: %s [porta] [diretorio]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    if (argc >= 3) {
        storage_directory = argv[2];
    }

    if (port < 1 || port > 65535) {
        fprintf(stderr, "Porta invalida: %d\n", port);
        return EXIT_FAILURE;
    }

    if (stat(storage_directory, &storage_info) != 0 ||
        !S_ISDIR(storage_info.st_mode)) {
        fprintf(
            stderr,
            "Diretorio invalido: %s\n",
            storage_directory
        );
        return EXIT_FAILURE;
    }

    session_table_init();

    /*
     * SOCK_DGRAM seleciona UDP.
     */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse_address = 1;

    if (setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
        ) != 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(sockfd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons((uint16_t)port);

    /*
     * bind() associa este socket UDP à porta em todas as interfaces.
     */
    if (bind(
            sockfd,
            (struct sockaddr *)&server_address,
            sizeof(server_address)
        ) != 0) {
        perror("bind");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf(
        "Receiver UDP escutando em 0.0.0.0:%d; diretorio=%s\n",
        port,
        storage_directory
    );

    while (1) {
        unsigned char buffer[MAX_PACKET_SIZE];
        struct sockaddr_in client_address;
        socklen_t client_address_length = sizeof(client_address);

        /*
         * recvfrom() bloqueia até chegar um datagrama.
         * Além do conteúdo, ele preenche IP e porta de quem enviou.
         */
        ssize_t received = recvfrom(
            sockfd,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr *)&client_address,
            &client_address_length
        );

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("recvfrom");
            continue;
        }

        packet_header_t header;
        const unsigned char *payload;

        if (parse_packet(
                buffer,
                (size_t)received,
                &header,
                &payload
            ) != 0) {
            fprintf(stderr, "Datagrama invalido descartado\n");
            continue;
        }

        switch ((message_type_t)header.type) {
            case MSG_START:
                handle_start(
                    sockfd,
                    &header,
                    payload,
                    &client_address,
                    client_address_length,
                    storage_directory
                );
                break;

            case MSG_DATA:
                handle_data(
                    sockfd,
                    &header,
                    payload,
                    &client_address,
                    client_address_length
                );
                break;

            case MSG_FINISH:
                handle_finish(
                    sockfd,
                    &header,
                    payload,
                    &client_address,
                    client_address_length
                );
                break;

            default:
                send_error(
                    sockfd,
                    &client_address,
                    client_address_length,
                    header.transfer_id,
                    header.sequence,
                    "Tipo de mensagem inesperado"
                );
                break;
        }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}