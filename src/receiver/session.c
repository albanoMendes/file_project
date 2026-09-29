#include "session.h"

#include "checksum.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static session_t sessions[MAX_SESSIONS];

/*
 * Inicializa a tabela fixa de sessões.
 *
 * Um único receiver UDP pode atender várias sessões porque cada datagrama
 * traz transfer_id. Para este trabalho, 64 sessões simultâneas é suficiente.
 */
void session_table_init(void) {
    memset(sessions, 0, sizeof(sessions));
}

/*
 * Impede que o sender use o nome recebido para escapar do diretório storage.
 *
 * Permitimos somente nome-base, como:
 *   relatorio.pdf
 *
 * Rejeitamos:
 *   ../../etc/passwd
 *   pasta/arquivo.txt
 *   pasta\\arquivo.txt
 */
int filename_is_safe(const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        return 0;
    }

    if (strlen(filename) > MAX_FILENAME) {
        return 0;
    }

    if (strstr(filename, "..") != NULL) {
        return 0;
    }

    if (strchr(filename, '/') != NULL) {
        return 0;
    }

    if (strchr(filename, '\\') != NULL) {
        return 0;
    }

    return 1;
}

/*
 * Busca sessão ativa pelo transfer_id.
 */
session_t *session_find_by_transfer_id(uint32_t transfer_id) {
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active &&
            sessions[i].transfer_id == transfer_id) {
            return &sessions[i];
        }
    }

    return NULL;
}

/*
 * Busca sessão ativa pelo nome final do arquivo.
 *
 * É usada para impedir que dois transfer_id diferentes escrevam
 * simultaneamente em "storage/mesmo_nome.part".
 */
session_t *session_find_by_filename(const char *filename) {
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active &&
            strcmp(sessions[i].filename, filename) == 0) {
            return &sessions[i];
        }
    }

    return NULL;
}

/*
 * Reserva uma posição livre na tabela.
 */
static session_t *session_allocate(void) {
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!sessions[i].active) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].active = 1;
            sessions[i].status = SESSION_IN_PROGRESS;
            return &sessions[i];
        }
    }

    return NULL;
}

/*
 * Salva metadados de forma simples:
 *
 * transfer_id=...
 * filename=...
 * file_size=...
 * checksum=...
 * bytes_received=...
 * expected_sequence=...
 *
 * Para a atividade, isso permite retomar após reiniciar sender ou receiver.
 */
static int session_save_metadata(const session_t *session) {
    FILE *meta;

    if (session == NULL) {
        return -1;
    }

    meta = fopen(session->meta_path, "w");
    if (meta == NULL) {
        return -1;
    }

    int result = fprintf(
        meta,
        "transfer_id=%u\n"
        "filename=%s\n"
        "file_size=%llu\n"
        "checksum=%u\n"
        "bytes_received=%llu\n"
        "expected_sequence=%u\n",
        session->transfer_id,
        session->filename,
        (unsigned long long)session->file_size,
        session->expected_checksum,
        (unsigned long long)session->bytes_received,
        session->expected_sequence
    );

    if (fclose(meta) != 0) {
        return -1;
    }

    return result < 0 ? -1 : 0;
}

/*
 * Carrega o .meta de uma transferência interrompida.
 *
 * Retornos:
 *  1: metadados carregados;
 *  0: .meta não existe;
 * -1: .meta existe, mas é inválido ou não combina com START.
 */
static int session_load_metadata(
    session_t *session,
    const start_message_t *start
) {
    FILE *meta;
    unsigned int transfer_id;
    unsigned int checksum;
    unsigned int sequence;
    unsigned long long file_size;
    unsigned long long bytes_received;
    char filename[MAX_FILENAME + 1];

    meta = fopen(session->meta_path, "r");
    if (meta == NULL) {
        if (errno == ENOENT) {
            return 0;
        }

        return -1;
    }

    int fields = fscanf(
        meta,
        "transfer_id=%u\n"
        "filename=%255[^\n]\n"
        "file_size=%llu\n"
        "checksum=%u\n"
        "bytes_received=%llu\n"
        "expected_sequence=%u\n",
        &transfer_id,
        filename,
        &file_size,
        &checksum,
        &bytes_received,
        &sequence
    );

    fclose(meta);

    if (fields != 6) {
        return -1;
    }

    if ((uint32_t)transfer_id != session->transfer_id ||
        strcmp(filename, start->filename) != 0 ||
        (uint64_t)file_size != start->file_size ||
        (uint32_t)checksum != start->checksum) {
        return -1;
    }

    session->bytes_received = (uint64_t)bytes_received;
    session->expected_sequence = (uint32_t)sequence;
    session->expected_checksum = (uint32_t)checksum;

    return 1;
}

/*
 * Obtém tamanho real de "arquivo.part".
 *
 * O arquivo parcial é a fonte física dos bytes; o .meta é a fonte do
 * estado lógico. Os dois precisam ser consistentes para uma retomada segura.
 */
static int session_partial_file_size(
    const session_t *session,
    uint64_t *size
) {
    struct stat info;

    if (stat(session->part_path, &info) != 0) {
        if (errno == ENOENT) {
            *size = 0;
            return 0;
        }

        return -1;
    }

    if (info.st_size < 0) {
        return -1;
    }

    *size = (uint64_t)info.st_size;
    return 0;
}

/*
 * Verifica se já existe um arquivo finalizado com o mesmo nome.
 *
 * Se existir, calcula CRC32 e compara com o START:
 * - tamanho igual;
 * - checksum igual.
 *
 * Assim, reenviar o mesmo arquivo concluído é idempotente: receiver pode
 * devolver DONE sem criar uma segunda cópia.
 */
int session_completed_file_matches(
    const start_message_t *start,
    const char *storage_directory
) {
    char final_path[MAX_PATH_LENGTH];
    struct stat info;
    FILE *file;
    unsigned char buffer[MAX_PAYLOAD];
    size_t bytes_read;
    uint32_t crc;

    if (start == NULL || storage_directory == NULL) {
        return -1;
    }

    if (snprintf(
            final_path,
            sizeof(final_path),
            "%s/%s",
            storage_directory,
            start->filename
        ) >= (int)sizeof(final_path)) {
        return -1;
    }

    if (stat(final_path, &info) != 0) {
        if (errno == ENOENT) {
            return 0;
        }

        return -1;
    }

    if (!S_ISREG(info.st_mode) ||
        info.st_size < 0 ||
        (uint64_t)info.st_size != start->file_size) {
        return -1;
    }

    file = fopen(final_path, "rb");
    if (file == NULL) {
        return -1;
    }

    crc = crc32_init();

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        crc = crc32_update(crc, buffer, bytes_read);
    }

    if (ferror(file) || fclose(file) != 0) {
        return -1;
    }

    crc = crc32_finalize(crc);

    return crc == start->checksum ? 1 : -1;
}

/*
 * Garante que o arquivo parcial exista.
 *
 * Para arquivos normais ele será preenchido quando chegarem MSG_DATA.
 * Para arquivo vazio, ele precisa existir antes de FINISH, pois não haverá
 * nenhuma mensagem DATA que o crie.
 */
static int session_ensure_part_file_exists(const session_t *session) {
    FILE *part_file;

    if (session == NULL) {
        return -1;
    }

    part_file = fopen(session->part_path, "ab");
    if (part_file == NULL) {
        return -1;
    }

    return fclose(part_file) == 0 ? 0 : -1;
}

/*
 * Cria uma sessão nova ou recupera uma sessão que estava interrompida.
 *
 * O fluxo é:
 *
 * 1. Se a sessão já está em memória, retorna ela.
 * 2. Se o arquivo final já existe, esta função retorna NULL; receiver.c
 *    trata esse cenário de upload idempotente.
 * 3. Se há .meta + .part, valida consistência e recupera.
 * 4. Se não há .meta e nem .part, cria nova sessão.
 */
session_t *session_create_or_resume(
    uint32_t transfer_id,
    const start_message_t *start,
    const char *storage_directory,
    const struct sockaddr_in *client_address,
    socklen_t client_address_length
) {
    session_t *session;

    if (start == NULL ||
        storage_directory == NULL ||
        client_address == NULL) {
        return NULL;
    }

    session = session_find_by_transfer_id(transfer_id);

    if (session != NULL) {
        if (strcmp(session->filename, start->filename) != 0 ||
            session->file_size != start->file_size ||
            session->expected_checksum != start->checksum) {
            return NULL;
        }

        memcpy(
            &session->client_address,
            client_address,
            sizeof(*client_address)
        );
        session->client_address_length = client_address_length;

        return session;
    }

    /*
     * Impede colisão de dois uploads para o mesmo nome.
     */
    session_t *same_filename = session_find_by_filename(start->filename);

    if (same_filename != NULL) {
        return NULL;
    }

    session = session_allocate();
    if (session == NULL) {
        return NULL;
    }

    session->transfer_id = transfer_id;
    session->file_size = start->file_size;
    session->expected_checksum = start->checksum;

    snprintf(
        session->filename,
        sizeof(session->filename),
        "%s",
        start->filename
    );

    if (snprintf(
            session->part_path,
            sizeof(session->part_path),
            "%s/%s.part",
            storage_directory,
            start->filename
        ) >= (int)sizeof(session->part_path) ||
        snprintf(
            session->meta_path,
            sizeof(session->meta_path),
            "%s/%s.meta",
            storage_directory,
            start->filename
        ) >= (int)sizeof(session->meta_path) ||
        snprintf(
            session->final_path,
            sizeof(session->final_path),
            "%s/%s",
            storage_directory,
            start->filename
        ) >= (int)sizeof(session->final_path)) {
        session_destroy(session);
        return NULL;
    }

    memcpy(
        &session->client_address,
        client_address,
        sizeof(*client_address)
    );
    session->client_address_length = client_address_length;

    int metadata_status = session_load_metadata(session, start);

    if (metadata_status < 0) {
        session_destroy(session);
        return NULL;
    }

    uint64_t actual_part_size;

    if (session_partial_file_size(session, &actual_part_size) != 0) {
        session_destroy(session);
        return NULL;
    }

    if (metadata_status == 1) {
        /*
         * Regra de segurança:
         *
         * .meta declara bytes_received.
         * .part precisa ter EXATAMENTE esses bytes.
         *
         * Se não tiver, não sabemos se o último bloco foi persistido
         * completamente antes de uma queda. Em vez de adivinhar e
         * arriscar corrupção, recusamos a retomada.
         */
        if (actual_part_size != session->bytes_received) {
            fprintf(
                stderr,
                "Estado inconsistente para %s: meta=%llu, part=%llu\n",
                session->filename,
                (unsigned long long)session->bytes_received,
                (unsigned long long)actual_part_size
            );
            session_destroy(session);
            return NULL;
        }

        if (session->bytes_received > session->file_size) {
            session_destroy(session);
            return NULL;
        }

        uint64_t expected_offset =
            (uint64_t)session->expected_sequence * MAX_PAYLOAD;

        /*
         * Para todos os blocos completos, expected_offset é igual a
         * bytes_received. No último bloco parcial, o número de sequência
         * aponta para o próximo bloco e expected_offset pode ser maior.
         *
         * O teste correto é:
         * next_sequence = ceil(bytes_received / MAX_PAYLOAD)
         *
         * Porém, Stop-and-Wait sempre grava um bloco por inteiro antes de
         * avançar sequence. Logo:
         *
         * expected_sequence == quantidade de DATA já confirmados.
         *
         * Abaixo validamos limites sem exigir múltiplo exato do payload.
         */
        if (session->expected_sequence == 0 &&
            session->bytes_received != 0) {
            session_destroy(session);
            return NULL;
        }

        if (expected_offset < session->bytes_received &&
            session->bytes_received - expected_offset >= MAX_PAYLOAD) {
            session_destroy(session);
            return NULL;
        }
    } else {
        /*
         * Não existe .meta.
         *
         * Se também não existe .part, é upload novo.
         * Se existe .part sem .meta, não retomamos por segurança: não há
         * transfer_id/checksum/sequence confiáveis associados a ele.
         */
        if (actual_part_size != 0) {
            fprintf(
                stderr,
                "Existe .part sem .meta para %s; removendo estado incompleto\n",
                session->filename
            );

            if (unlink(session->part_path) != 0) {
                session_destroy(session);
                return NULL;
            }
        }

        session->bytes_received = 0;
        session->expected_sequence = 0;

        if (session_save_metadata(session) != 0) {
            session_destroy(session);
            return NULL;
        }
    }

    if (session_ensure_part_file_exists(session) != 0) {
        session_destroy(session);
        return NULL;
    }

    return session;
}

/*
 * Persiste um DATA novo.
 *
 * Ordem correta:
 *
 * 1. Abre .part em append.
 * 2. Grava payload.
 * 3. Fecha arquivo.
 * 4. Atualiza bytes_received e expected_sequence.
 * 5. Escreve .meta.
 * 6. Receiver envia ACK.
 *
 * O ACK nunca deve ser enviado antes da gravação bem-sucedida.
 */
int session_append_data(
    session_t *session,
    const unsigned char *data,
    uint32_t data_size
) {
    FILE *part_file;
    size_t written;

    if (session == NULL || data == NULL ||
        data_size == 0 || data_size > MAX_PAYLOAD) {
        return -1;
    }

    if (session->bytes_received + data_size > session->file_size) {
        return -1;
    }

    part_file = fopen(session->part_path, "ab");
    if (part_file == NULL) {
        return -1;
    }

    written = fwrite(data, 1, data_size, part_file);

    if (fclose(part_file) != 0) {
        return -1;
    }

    if (written != data_size) {
        return -1;
    }

    session->bytes_received += data_size;
    session->expected_sequence += 1;

    if (session_save_metadata(session) != 0) {
        return -1;
    }

    return 0;
}

/*
 * Finaliza uma transferência:
 *
 * 1. Confere o tamanho declarado no FINISH.
 * 2. Confere se todos os bytes esperados chegaram.
 * 3. Calcula CRC32 do .part.
 * 4. Compara CRC32 calculado com CRC32 de START e FINISH.
 * 5. Renomeia .part para o arquivo final.
 * 6. Remove .meta.
 *
 * rename() publica o arquivo como disponível somente no final.
 */
int session_finish(
    session_t *session,
    uint64_t finish_file_size,
    uint32_t finish_checksum
) {
    FILE *part_file;
    unsigned char buffer[MAX_PAYLOAD];
    size_t bytes_read;
    uint32_t crc;

    if (session == NULL) {
        return -1;
    }

    if (finish_file_size != session->file_size ||
        finish_checksum != session->expected_checksum ||
        session->bytes_received != session->file_size) {
        return -1;
    }

    part_file = fopen(session->part_path, "rb");
    if (part_file == NULL) {
        return -1;
    }

    crc = crc32_init();

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), part_file)) > 0) {
        crc = crc32_update(crc, buffer, bytes_read);
    }

    if (ferror(part_file) || fclose(part_file) != 0) {
        return -1;
    }

    crc = crc32_finalize(crc);

    if (crc != session->expected_checksum) {
        return -1;
    }

    if (rename(session->part_path, session->final_path) != 0) {
        return -1;
    }

    if (unlink(session->meta_path) != 0 && errno != ENOENT) {
        return -1;
    }

    session->status = SESSION_COMPLETED;
    return 0;
}

/*
 * Libera uma entrada da tabela em memória.
 *
 * Não remove .part ou .meta: isso seria errado em uma transferência
 * interrompida. A remoção dos arquivos temporários só acontece em
 * session_finish(), após validação.
 */
void session_destroy(session_t *session) {
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
    }
}