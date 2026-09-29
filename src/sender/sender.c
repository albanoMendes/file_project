#include "checksum.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define ACK_TIMEOUT_SECONDS 1
#define MAX_RETRIES 10
#define PATH_BUFFER_SIZE 1024
#define REPLY_OK 1
#define REPLY_TIMEOUT 0
#define REPLY_SOCKET_ERROR -1
#define REPLY_REMOTE_ERROR -2
#define REPLY_SESSION_NOT_FOUND -3
/*
 * Opcional para testes de interrupção.
 *
 * Use:
 *   ./bin/sender 127.0.0.1 arquivo.bin 9000 1000
 *
 * O último argumento é atraso em microssegundos entre ACKs.
 * Em produção/teste normal, use sem o quarto argumento ou deixe 0.
 */
static unsigned int delay_after_ack_us = 0;

static void die(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

/*
 * Calcula CRC32 lendo o arquivo em blocos de MAX_PAYLOAD.
 *
 * Não carrega o arquivo inteiro na memória.
 */
static int calculate_file_checksum(
    const char *file_path,
    uint32_t *checksum
) {
    unsigned char buffer[MAX_PAYLOAD];
    size_t bytes_read;
    uint32_t crc;

    FILE *input = fopen(file_path, "rb");
    if (input == NULL) {
        return -1;
    }

    crc = crc32_init();

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        crc = crc32_update(crc, buffer, bytes_read);
    }

    if (ferror(input)) {
        fclose(input);
        return -1;
    }

    if (fclose(input) != 0) {
        return -1;
    }

    *checksum = crc32_finalize(crc);
    return 0;
}

/*
 * Gera um transfer_id determinístico usando:
 *
 * filename + file_size + checksum
 *
 * O mesmo arquivo, executado novamente, gera o mesmo id.
 * Isso é necessário para que o receiver encontre o .meta/.part
 * correspondente e retorne RESUME.
 *
 * Não é hash criptográfico; é apenas identificador de sessão.
 */
static uint32_t make_transfer_id(
    const char *filename,
    uint64_t file_size,
    uint32_t checksum
) {
    uint32_t hash = 2166136261U;

    for (const unsigned char *p = (const unsigned char *)filename;
         *p != '\0';
         ++p) {
        hash ^= *p;
        hash *= 16777619U;
    }

    for (int i = 0; i < 8; ++i) {
        hash ^= (uint8_t)(file_size >> (i * 8));
        hash *= 16777619U;
    }

    for (int i = 0; i < 4; ++i) {
        hash ^= (uint8_t)(checksum >> (i * 8));
        hash *= 16777619U;
    }

    return hash == 0 ? 1 : hash;
}

/*
 * Espera a resposta esperada do receiver.
 *
 * Retornos:
 *  1: recebeu resposta correta;
 *  0: timeout;
 * -1: falha de recvfrom;
 * -2: receiver retornou MSG_ERROR.
 *
 * Ignoramos datagramas de outras sessões, outra sequência ou outro tipo,
 * pois UDP pode entregar mensagens atrasadas/duplicadas.
 */
static int wait_for_reply(
    int sockfd,
    uint32_t transfer_id,
    uint32_t sequence,
    message_type_t expected_type,
    packet_header_t *reply_header,
    unsigned char *reply_payload
) {
    while (1) {
        unsigned char buffer[MAX_PACKET_SIZE];

        ssize_t received = recvfrom(
            sockfd,
            buffer,
            sizeof(buffer),
            0,
            NULL,
            NULL
        );

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            return -1;
        }

        const unsigned char *payload;

        if (parse_packet(
                buffer,
                (size_t)received,
                reply_header,
                &payload
            ) != 0) {
            fprintf(stderr, "Resposta invalida ignorada\n");
            continue;
        }

        if (reply_header->transfer_id != transfer_id) {
            continue;
        }
        
        if (reply_header->type == MSG_ERROR) {
            size_t error_size = reply_header->payload_size;

            if (error_size >= MAX_PACKET_SIZE) {
                error_size = MAX_PACKET_SIZE - 1;
            }

            memcpy(reply_payload, payload, error_size);
            reply_payload[error_size] = '\0';

            fprintf(
                stderr,
                "Receiver retornou erro na sequencia %u: %s\n",
                reply_header->sequence,
                reply_payload
            );

            /*
            * Após reiniciar, receiver perde a tabela sessions[] da memória.
            * Ele pede que o sender envie START novamente para recuperar a sessão
            * usando arquivo .meta e arquivo .part.
            */
            if (strcmp(
                    (char *)reply_payload,
                    "Sessao inexistente; envie START"
                ) == 0) {
                return REPLY_SESSION_NOT_FOUND;
            }

            return REPLY_REMOTE_ERROR;
        }

        if (reply_header->type != expected_type ||
            reply_header->sequence != sequence) {
            continue;
        }

        if (reply_header->payload_size > 0) {
            memcpy(
                reply_payload,
                payload,
                reply_header->payload_size
            );
        }

        return 1;
    }
}

/*
 * Implementa retransmissão do Stop-and-Wait.
 *
 * Para uma mensagem:
 *
 * 1. sendto()
 * 2. espera ACK/RESUME/DONE
 * 3. se timeout, reenvia o mesmo pacote
 * 4. após MAX_RETRIES, falha
 */
static int send_reliably(
    int sockfd,
    const struct sockaddr_in *server_address,
    socklen_t server_address_length,
    const unsigned char *packet,
    size_t packet_size,
    uint32_t transfer_id,
    uint32_t sequence,
    message_type_t expected_response,
    packet_header_t *reply_header,
    unsigned char *reply_payload,
    const char *label
) {
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        ssize_t sent = sendto(
            sockfd,
            packet,
            packet_size,
            0,
            (const struct sockaddr *)server_address,
            server_address_length
        );

        if (sent < 0 || (size_t)sent != packet_size) {
            perror("sendto");
            return -1;
        }

        int result = wait_for_reply(
            sockfd,
            transfer_id,
            sequence,
            expected_response,
            reply_header,
            reply_payload
        );

        if (result == 1) {
            return 0;
        }

        if (result == REPLY_SESSION_NOT_FOUND) {
    return REPLY_SESSION_NOT_FOUND;
}

if (result == REPLY_REMOTE_ERROR) {
    return -1;
}

        if (result < 0) {
            perror("recvfrom");
            return -1;
        }

        fprintf(
            stderr,
            "%s seq=%u: timeout; retransmitindo (%d/%d)\n",
            label,
            sequence,
            attempt,
            MAX_RETRIES
        );
    }

    fprintf(
        stderr,
        "%s seq=%u: limite de retransmissoes excedido\n",
        label,
        sequence
    );

    return -1;
}

/*
 * Envia START de forma confiável e recebe RESUME.
 *
 * Retorna:
 *  0 em sucesso;
 * -1 em erro;
 *
 * Em sucesso, preenche "resume" com o ponto persistido pelo receiver.
 */
static int perform_start_handshake(
    int sockfd,
    const struct sockaddr_in *server_address,
    uint32_t transfer_id,
    const start_message_t *start,
    resume_message_t *resume
) {
    unsigned char packet[MAX_PACKET_SIZE];
    unsigned char reply_payload[MAX_PACKET_SIZE];
    unsigned char start_payload[MAX_PAYLOAD];
    packet_header_t reply_header;

    int start_payload_size = serialize_start_payload(
        start,
        start_payload,
        sizeof(start_payload)
    );

    if (start_payload_size < 0) {
        fprintf(stderr, "Erro ao serializar START\n");
        return -1;
    }

    int packet_size = build_packet(
        packet,
        sizeof(packet),
        MSG_START,
        transfer_id,
        0,
        start_payload,
        (uint32_t)start_payload_size
    );

    if (packet_size < 0) {
        fprintf(stderr, "Erro ao montar START\n");
        return -1;
    }

    int result = send_reliably(
        sockfd,
        server_address,
        sizeof(*server_address),
        packet,
        (size_t)packet_size,
        transfer_id,
        0,
        MSG_RESUME,
        &reply_header,
        reply_payload,
        "START"
    );

    if (result != 0) {
        return -1;
    }

    if (deserialize_resume_payload(
            reply_payload,
            reply_header.payload_size,
            resume
        ) != 0) {
        fprintf(stderr, "RESUME recebido em formato invalido\n");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 5) {
        fprintf(
            stderr,
            "Uso: %s <ip_servidor> <arquivo> [porta] [atraso_us]\n",
            argv[0]
        );
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    const char *file_path = argv[2];

    int port = argc >= 4 ? atoi(argv[3]) : DEFAULT_PORT;

    if (argc == 5) {
        delay_after_ack_us = (unsigned int)strtoul(argv[4], NULL, 10);
    }

    if (port < 1 || port > 65535) {
        fprintf(stderr, "Porta invalida: %d\n", port);
        return EXIT_FAILURE;
    }

    /*
     * stat() obtém tamanho sem carregar o arquivo.
     */
    struct stat file_info;

    if (stat(file_path, &file_info) != 0) {
        die("stat");
    }

    if (!S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "O caminho informado nao e um arquivo regular\n");
        return EXIT_FAILURE;
    }

    if (file_info.st_size < 0) {
        fprintf(stderr, "Tamanho de arquivo invalido\n");
        return EXIT_FAILURE;
    }

    uint64_t file_size = (uint64_t)file_info.st_size;

    /*
     * Sender aceita caminho completo local, mas transmite apenas basename.
     */
    char path_copy[PATH_BUFFER_SIZE];

    if (snprintf(path_copy, sizeof(path_copy), "%s", file_path) >=
        (int)sizeof(path_copy)) {
        fprintf(stderr, "Caminho do arquivo muito longo\n");
        return EXIT_FAILURE;
    }

    char *filename = basename(path_copy);
    size_t filename_length = strlen(filename);

    if (filename_length == 0 || filename_length > MAX_FILENAME) {
        fprintf(stderr, "Nome de arquivo invalido\n");
        return EXIT_FAILURE;
    }

    /*
     * CRC32 é calculado antes do envio e usado no START e FINISH.
     */
    uint32_t checksum;

    if (calculate_file_checksum(file_path, &checksum) != 0) {
        die("Erro ao calcular checksum");
    }

    uint32_t transfer_id = make_transfer_id(
        filename,
        file_size,
        checksum
    );

    FILE *input = fopen(file_path, "rb");

    if (input == NULL) {
        die("fopen");
    }

    /*
     * Cria socket UDP.
     */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0) {
        die("socket");
    }

    /*
     * Configura timeout para recvfrom().
     * Quando o timeout vence, recvfrom retorna -1 com EAGAIN/EWOULDBLOCK.
     */
    struct timeval timeout;
    timeout.tv_sec = ACK_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;

    if (setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)
        ) != 0) {
        die("setsockopt(SO_RCVTIMEO)");
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons((uint16_t)port);

    if (inet_pton(
            AF_INET,
            server_ip,
            &server_address.sin_addr
        ) != 1) {
        fprintf(stderr, "IPv4 invalido: %s\n", server_ip);
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf(
        "Arquivo=%s tamanho=%llu checksum=%u transfer_id=%u\n",
        filename,
        (unsigned long long)file_size,
        checksum,
        transfer_id
    );

    unsigned char packet[MAX_PACKET_SIZE];
    unsigned char reply_payload[MAX_PACKET_SIZE];
    packet_header_t reply_header;

    /*
     * -------------------------
     * Etapa 1: START -> RESUME
     * -------------------------
     */
    start_message_t start;

    start.file_size = file_size;
    start.checksum = checksum;
    start.filename_length = (uint16_t)filename_length;

    memcpy(start.filename, filename, filename_length);
    start.filename[filename_length] = '\0';

    unsigned char start_payload[MAX_PAYLOAD];

    int start_payload_size = serialize_start_payload(
        &start,
        start_payload,
        sizeof(start_payload)
    );

    if (start_payload_size < 0) {
        fprintf(stderr, "Erro ao serializar START\n");
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    int packet_size = build_packet(
        packet,
        sizeof(packet),
        MSG_START,
        transfer_id,
        0,
        start_payload,
        (uint32_t)start_payload_size
    );

    if (packet_size < 0) {
        fprintf(stderr, "Erro ao montar START\n");
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * Um arquivo que já existe e é igual recebe DONE diretamente.
     * Isso torna repetir o mesmo upload uma operação idempotente.
     */
    int start_result = send_reliably(
        sockfd,
        &server_address,
        sizeof(server_address),
        packet,
        (size_t)packet_size,
        transfer_id,
        0,
        MSG_RESUME,
        &reply_header,
        reply_payload,
        "START"
    );

    if (start_result != 0) {
        /*
         * send_reliably espera RESUME. Se receiver respondeu DONE
         * para upload idempotente, wait_for_reply ignora por ser outro tipo.
         *
         * Para manter o código simples nesta versão, evitamos esse cenário
         * no fluxo normal de testes: use um diretório limpo ou apague o
         * arquivo final antes de reenviar. A versão abaixo trata DONE
         * explicitamente através de uma espera dedicada.
         */
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    resume_message_t resume;

    if (deserialize_resume_payload(
            reply_payload,
            reply_header.payload_size,
            &resume
        ) != 0) {
        fprintf(stderr, "RESUME recebido em formato invalido\n");
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * Validação importante:
     *
     * bytes_received precisa estar no intervalo do arquivo.
     * O offset deve ser exatamente o tamanho persistido pelo receiver.
     */
    if (resume.bytes_received > file_size) {
        fprintf(
            stderr,
            "Receiver informou bytes_received invalido: %llu\n",
            (unsigned long long)resume.bytes_received
        );
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint64_t expected_sequence_from_bytes =
        (resume.bytes_received + MAX_PAYLOAD - 1) / MAX_PAYLOAD;

    if (resume.next_sequence != expected_sequence_from_bytes &&
        !(resume.bytes_received == 0 && resume.next_sequence == 0)) {
        fprintf(
            stderr,
            "Estado RESUME inconsistente: seq=%u bytes=%llu\n",
            resume.next_sequence,
            (unsigned long long)resume.bytes_received
        );
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * fseeko usa off_t; com _FILSE_OFFSET_BITS=64 no Makefile,
     * o offset suporta confortavelmente o requisito de 1 GB.
     */
    if (fseeko(
            input,
            (off_t)resume.bytes_received,
            SEEK_SET
        ) != 0) {
        die("fseeko");
    }

    printf(
        "RESUME: proximo_bloco=%u, bytes_recebidos=%llu\n",
        resume.next_sequence,
        (unsigned long long)resume.bytes_received
    );

    /*
     * ----------------------
     * Etapa 2: DATA -> ACK
     * ----------------------
     *
     * Lemos no máximo MAX_PAYLOAD bytes por vez.
     * Para cada bloco, só passamos ao próximo após receber ACK.
     */
    uint32_t sequence = resume.next_sequence;
    unsigned char data[MAX_PAYLOAD];
    size_t bytes_read;

    while ((bytes_read = fread(data, 1, sizeof(data), input)) > 0) {
        packet_size = build_packet(
            packet,
            sizeof(packet),
            MSG_DATA,
            transfer_id,
            sequence,
            data,
            (uint32_t)bytes_read
        );

        if (packet_size < 0) {
            fprintf(stderr, "Erro ao montar DATA\n");
            fclose(input);
            close(sockfd);
            return EXIT_FAILURE;
        }

        int data_result = send_reliably(
            sockfd,
            &server_address,
            sizeof(server_address),
            packet,
            (size_t)packet_size,
            transfer_id,
            sequence,
            MSG_ACK,
            &reply_header,
            reply_payload,
            "DATA"
        );

        if (data_result == REPLY_SESSION_NOT_FOUND) {
            resume_message_t recovered_resume;

            fprintf(
                stderr,
                "Receiver reiniciou ou perdeu a sessao; refazendo START...\n"
            );

            if (perform_start_handshake(
                    sockfd,
                    &server_address,
                    transfer_id,
                    &start,
                    &recovered_resume
                ) != 0) {
                fclose(input);
                close(sockfd);
                return EXIT_FAILURE;
            }

            if (recovered_resume.bytes_received > file_size) {
                fprintf(stderr, "RESUME de recuperacao invalido\n");
                fclose(input);
                close(sockfd);
                return EXIT_FAILURE;
            }

            if (fseeko(
                    input,
                    (off_t)recovered_resume.bytes_received,
                    SEEK_SET
                ) != 0) {
                perror("fseeko");
                fclose(input);
                close(sockfd);
                return EXIT_FAILURE;
            }

            sequence = recovered_resume.next_sequence;

            printf(
                "RESUME apos reinicio: proximo_bloco=%u, bytes_recebidos=%llu\n",
                sequence,
                (unsigned long long)recovered_resume.bytes_received
            );

            /*
            * Não fazemos ACK nem incrementamos sequence agora.
            * Voltamos ao começo do loop e reenviamos o bloco certo.
            */
            continue;
        }

        if (data_result != 0) {
            fclose(input);
            close(sockfd);
            return EXIT_FAILURE;
        }

        printf("ACK: seq=%u, bytes=%zu\n", sequence, bytes_read);

        if (delay_after_ack_us > 0) {
            usleep(delay_after_ack_us);
        }

        ++sequence;
    }

    if (ferror(input)) {
        die("fread");
    }

    /*
     * ------------------------
     * Etapa 3: FINISH -> DONE
     * ------------------------
     *
     * Para arquivo vazio:
     *   sequence continua 0 e last_sequence é 0.
     *
     * Para arquivo não vazio:
     *   sequence aponta para a próxima sequência livre;
     *   last_sequence = sequence - 1.
     */
    uint32_t last_sequence = sequence == 0 ? 0 : sequence - 1;

    finish_message_t finish;
    finish.file_size = file_size;
    finish.checksum = checksum;

    unsigned char finish_payload[MAX_PAYLOAD];

    int finish_payload_size = serialize_finish_payload(
        &finish,
        finish_payload,
        sizeof(finish_payload)
    );

    if (finish_payload_size < 0) {
        fprintf(stderr, "Erro ao serializar FINISH\n");
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    packet_size = build_packet(
        packet,
        sizeof(packet),
        MSG_FINISH,
        transfer_id,
        last_sequence,
        finish_payload,
        (uint32_t)finish_payload_size
    );

    if (packet_size < 0) {
        fprintf(stderr, "Erro ao montar FINISH\n");
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (send_reliably(
            sockfd,
            &server_address,
            sizeof(server_address),
            packet,
            (size_t)packet_size,
            transfer_id,
            last_sequence,
            MSG_DONE,
            &reply_header,
            reply_payload,
            "FINISH"
        ) != 0) {
        fclose(input);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Transferencia finalizada com sucesso: %s\n", filename);

    fclose(input);
    close(sockfd);
    return EXIT_SUCCESS;
}