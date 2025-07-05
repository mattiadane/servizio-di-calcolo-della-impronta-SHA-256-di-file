#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "../common/protocol.h"

/**
 * @brief Genera un nome univoco per una FIFO client.
 *
 * Utilizza il PID del processo e un numero casuale per creare un nome distintivo.
 *
 * @param fifo_name_buffer Buffer per memorizzare il nome generato.
 * @param buffer_size Dimensione del buffer.
 */
void generate_unique_fifo_name(char *fifo_name_buffer, size_t buffer_size) {
    snprintf(fifo_name_buffer, buffer_size, "/tmp/sha256_client_fifo_%d_%ld", getpid(), random());
}

int main(int argc, char const *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <percorso_file>\n", argv[0]);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    const char *file_path = argv[1];

    if (strlen(file_path) >= MAX_PATH_LENGTH) {
        fprintf(stderr, "Errore: Il percorso del file è troppo lungo (max %d caratteri).\n", MAX_PATH_LENGTH - 1);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    // Ottieni la dimensione del file
    struct stat st;
    if (stat(file_path, &st) == -1) {
        perror("Errore nel recupero delle informazioni del file");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    long long file_size = st.st_size;

    char client_response_fifo_path[MAX_FIFO_PATH_LENGTH];
    int server_request_fifo_fd = -1;
    int client_response_fifo_fd = -1;
    char response_buffer[HASH_HEX_LENGTH + 100];
    server_status_code status;

    srandom(time(NULL));
    generate_unique_fifo_name(client_response_fifo_path, sizeof(client_response_fifo_path));

    // Crea la FIFO di risposta del client
    if (mkfifo(client_response_fifo_path, 0666) == -1) {
        if (errno != EEXIST) {
            perror("Errore nella creazione della FIFO di risposta del client");
            fflush(stderr);
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "La FIFO di risposta del client '%s' esiste già, tentativo di rimozione...\n", client_response_fifo_path);
        fflush(stderr);
        unlink(client_response_fifo_path);
        if (mkfifo(client_response_fifo_path, 0666) == -1) {
            perror("Errore critico: impossibile creare la FIFO di risposta dopo il tentativo di rimozione");
            fflush(stderr);
            exit(EXIT_FAILURE);
        }
    }

    printf("FIFO di risposta creata: '%s'\n", client_response_fifo_path);
    fflush(stdout);

    // Apre la FIFO del server in scrittura per inviare la richiesta
    server_request_fifo_fd = open(SERVER_REQUEST_FIFO, O_WRONLY);
    if (server_request_fifo_fd == -1) {
        perror("Errore nell'apertura della FIFO di richiesta del server. Assicurati che il server sia in esecuzione.");
        fflush(stderr);
        unlink(client_response_fifo_path);
        exit(EXIT_FAILURE);
    }

    // Costruisci il messaggio di richiesta: percorso_file | dimensione_file | percorso_fifo_risposta
    char request_message[MAX_REQUEST_MESSAGE_LENGTH];
    snprintf(request_message, sizeof(request_message), "%s%s%lld%s%s",
             file_path, MESSAGE_DELIMITER, file_size, MESSAGE_DELIMITER, client_response_fifo_path);

    printf("Invio richiesta al server: '%s'\n", request_message);
    fflush(stdout);

    // Invia la richiesta alla FIFO del server
    if (write(server_request_fifo_fd, request_message, strlen(request_message) + 1) < 0) {
        perror("Errore nell'invio della richiesta alla FIFO del server");
        fflush(stderr);
        close(server_request_fifo_fd);
        unlink(client_response_fifo_path);
        exit(EXIT_FAILURE);
    }
    close(server_request_fifo_fd);

    // Apre la propria FIFO di risposta in lettura
    printf("In attesa di risposta sulla FIFO: '%s'\n", client_response_fifo_path);
    fflush(stdout);
    client_response_fifo_fd = open(client_response_fifo_path, O_RDONLY);
    if (client_response_fifo_fd == -1) {
        perror("Errore nell'apertura della FIFO di risposta del client per la lettura");
        fflush(stderr);
        unlink(client_response_fifo_path);
        exit(EXIT_FAILURE);
    }

    // Ricezione dello stato e del risultato dal server
    ssize_t bytes_read = read(client_response_fifo_fd, &status, sizeof(status));
    if (bytes_read <= 0) {
        perror("Errore nella ricezione dello stato dal server via FIFO");
        fflush(stderr);
        close(client_response_fifo_fd);
        unlink(client_response_fifo_path);
        exit(EXIT_FAILURE);
    }

    memset(response_buffer, 0, sizeof(response_buffer));
    bytes_read = read(client_response_fifo_fd, response_buffer, sizeof(response_buffer) - 1);
    if (bytes_read <= 0) {
        perror("Errore nella ricezione del risultato dal server via FIFO");
        fflush(stderr);
        close(client_response_fifo_fd);
        unlink(client_response_fifo_path);
        exit(EXIT_FAILURE);
    }
    response_buffer[bytes_read] = '\0';

    if (status == STATUS_OK) {
        printf("SHA-256 hash ricevuto: %s\n", response_buffer);
        fflush(stdout);
    } else {
        fprintf(stderr, "Errore dal server (Stato: %d): %s\n", status, response_buffer);
        fflush(stderr);
    }

    // Pulizia: chiudi e rimuovi la FIFO di risposta del client
    close(client_response_fifo_fd);
    unlink(client_response_fifo_path);

    return 0;
}