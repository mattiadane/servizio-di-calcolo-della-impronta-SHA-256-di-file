#ifndef PROTOCOL_H
#define PROTOCOL_H

#define SERVER_REQUEST_FIFO "/tmp/sha256_server_request_fifo" // FIFO principale per le richieste al server
#define MAX_PATH_LENGTH 1024   // Lunghezza massima del percorso del file
#define MAX_FIFO_PATH_LENGTH 256 // Lunghezza massima del percorso per la FIFO di risposta del client
#define HASH_HEX_LENGTH (32 * 2 + 1) // Lunghezza dell'hash esadecimale (32 byte * 2 caratteri/byte + '\0')

// Delimitatore per separare le informazioni nella richiesta: percorso_file | dimensione_file | percorso_fifo_risposta
#define MESSAGE_DELIMITER "|"

// Lunghezza massima del messaggio di richiesta: percorso_file + delimitatore + dimensione_file (long long max cifre) + delimitatore + percorso_fifo_risposta + null_terminator
#define MAX_DIGITS_LONG_LONG 20 
#define MAX_REQUEST_MESSAGE_LENGTH (MAX_PATH_LENGTH + 1 + MAX_DIGITS_LONG_LONG + 1 + MAX_FIFO_PATH_LENGTH + 1)

/**
 * @brief Codici di stato per la risposta del server.
 */
typedef enum {
    STATUS_OK = 0,
    STATUS_FILE_NOT_FOUND,
    STATUS_READ_ERROR,
    STATUS_GENERIC_ERROR,
    STATUS_INVALID_REQUEST
} server_status_code;

// Struttura della richiesta da inviare/ricevere
typedef struct {
    char file_path[MAX_PATH_LENGTH];
    long long file_size;
    char client_response_fifo_path[MAX_FIFO_PATH_LENGTH];
} client_request_t;

#endif 