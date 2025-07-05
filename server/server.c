#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

#include "../common/protocol.h"
#include "sha256_utils.h"

// --- Configurazione del Thread Pool ---
#define NUM_WORKER_THREADS 4 // Numero fisso di thread worker

// --- Strutture per la Coda di Lavoro e Cache ---

// Elemento della coda di lavoro
typedef struct work_item {
    client_request_t request;
    struct work_item *next;
} work_item_t;

// Cache entry
typedef struct cache_entry {
    char *file_path;
    char *hash_hex;
    struct cache_entry *next;
} cache_entry_t;

// --- Variabili Globali per Coda, Cache e Sincronizzazione ---
static work_item_t *work_queue_head = NULL;
static work_item_t *work_queue_tail = NULL;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

static cache_entry_t *cache_head = NULL;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Struttura per gestire le richieste in corso dello stesso file (per evitare calcoli duplicati)
typedef struct pending_request {
    char *file_path;
    pthread_cond_t cond; // Condvar specifica per questo file_path
    char *result_hash;   // Hash quando calcolato
    int status;          // Stato del calcolo
    int ref_count;       // Contatore di riferimenti per sapere quando pulire
    pthread_mutex_t mutex; // Mutex per proteggere questa singola pending_request
    struct pending_request *next;
} pending_request_t;

static pending_request_t *pending_requests_head = NULL;
static pthread_mutex_t pending_requests_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Aggiunge una richiesta alla coda di lavoro, mantenendola ordinata per dimensione del file.
 *
 * @param request La struttura client_request_t da aggiungere.
 */
void enqueue_request(client_request_t request) {
    work_item_t *new_item = (work_item_t *)malloc(sizeof(work_item_t));
    if (new_item == NULL) {
        perror("Failed to allocate new work item");
        fflush(stderr);
        return;
    }
    new_item->request = request;
    new_item->next = NULL;

    pthread_mutex_lock(&queue_mutex);

    // Inserimento ordinato (file più piccoli prima)
    if (work_queue_head == NULL || request.file_size < work_queue_head->request.file_size) {
        new_item->next = work_queue_head;
        work_queue_head = new_item;
    } else {
        work_item_t *current = work_queue_head;
        while (current->next != NULL && request.file_size >= current->next->request.file_size) {
            current = current->next;
        }
        new_item->next = current->next;
        current->next = new_item;
    }

    if (work_queue_tail == NULL) {
        work_queue_tail = new_item;
    }

    pthread_cond_signal(&queue_cond); // Segnala che c'è nuovo lavoro
    pthread_mutex_unlock(&queue_mutex);
}

/**
 * @brief Estrae una richiesta dalla coda di lavoro.
 *
 * @return La richiesta estratta o NULL se si verifica un errore.
 */
work_item_t *dequeue_request() {
    pthread_mutex_lock(&queue_mutex);
    while (work_queue_head == NULL) {
        pthread_cond_wait(&queue_cond, &queue_mutex); // Attendi nuovo lavoro
    }
    work_item_t *item = work_queue_head;
    work_queue_head = work_queue_head->next;
    if (work_queue_head == NULL) {
        work_queue_tail = NULL;
    }
    pthread_mutex_unlock(&queue_mutex);
    item->next = NULL; // Scollega l'elemento dalla lista
    return item;
}

/**
 * @brief Cerca un hash nella cache.
 *
 * @param file_path Il percorso del file da cercare.
 * @return L'hash esadecimale se trovato, altrimenti NULL.
 */
char *get_from_cache(const char *file_path) {
    pthread_mutex_lock(&cache_mutex);
    cache_entry_t *current = cache_head;
    while (current != NULL) {
        if (strcmp(current->file_path, file_path) == 0) {
            char *hash = strdup(current->hash_hex); // Restituisce una copia per sicurezza
            pthread_mutex_unlock(&cache_mutex);
            return hash;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&cache_mutex);
    return NULL;
}

/**
 * @brief Aggiunge un percorso e il suo hash alla cache.
 *
 * @param file_path Il percorso del file.
 * @param hash_hex L'hash esadecimale.
 */
void add_to_cache(const char *file_path, const char *hash_hex) {
    cache_entry_t *new_entry = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (new_entry == NULL) {
        perror("Failed to allocate new cache entry");
        fflush(stderr);
        return;
    }
    new_entry->file_path = strdup(file_path);
    new_entry->hash_hex = strdup(hash_hex);
    new_entry->next = NULL;

    if (new_entry->file_path == NULL || new_entry->hash_hex == NULL) {
        fprintf(stderr, "Failed to duplicate strings for cache.\n");
        fflush(stderr);
        free(new_entry->file_path);
        free(new_entry->hash_hex);
        free(new_entry);
        return;
    }

    pthread_mutex_lock(&cache_mutex);
    new_entry->next = cache_head;
    cache_head = new_entry;
    pthread_mutex_unlock(&cache_mutex);
    printf("Added to cache: %s -> %s\n", file_path, hash_hex);
    fflush(stdout);
}

/**
 * @brief Gestisce le richieste duplicate per lo stesso file.
 *
 * Se un calcolo per `file_path` è già in corso, attende il risultato.
 * Altrimenti, lo marca come in corso e procede con il calcolo.
 *
 * @param file_path Il percorso del file.
 * @param calculated_hash Buffer per il risultato dell'hash.
 * @param status_code Puntatore per il codice di stato del risultato.
 * @return 0 se il calcolo è avvenuto o il risultato è stato atteso con successo, -1 in caso di errore.
 */
int handle_duplicate_request(const char *file_path, char *calculated_hash, server_status_code *status_code) {
    pending_request_t *pending_req = NULL;
    int is_first_request = 0;

    pthread_mutex_lock(&pending_requests_mutex);
    // Cerca se c'è già una richiesta pendente per questo file_path
    pending_request_t *current = pending_requests_head;
    while (current != NULL) {
        if (strcmp(current->file_path, file_path) == 0) {
            pending_req = current;
            break;
        }
        current = current->next;
    }

    if (pending_req == NULL) {
        // Questa è la prima richiesta per questo file_path, inizializza una nuova entry
        pending_req = (pending_request_t *)malloc(sizeof(pending_request_t));
        if (pending_req == NULL) {
            perror("Failed to allocate pending request entry");
            fflush(stderr);
            pthread_mutex_unlock(&pending_requests_mutex);
            return -1;
        }
        pending_req->file_path = strdup(file_path);
        if (pending_req->file_path == NULL) {
            perror("Failed to duplicate file_path for pending request");
            fflush(stderr);
            free(pending_req);
            pthread_mutex_unlock(&pending_requests_mutex);
            return -1;
        }
        pthread_cond_init(&pending_req->cond, NULL);
        pthread_mutex_init(&pending_req->mutex, NULL);
        pending_req->result_hash = NULL;
        pending_req->status = -1; // Stato iniziale sconosciuto
        pending_req->ref_count = 1; // Un riferimento: il thread corrente
        pending_req->next = pending_requests_head;
        pending_requests_head = pending_req;
        is_first_request = 1;
        printf("Nuova richiesta in corso per '%s'.\n", file_path);
        fflush(stdout);
    } else {
        // Richiesta duplicata, incrementa il contatore e attendi
        pending_req->ref_count++;
        printf("Richiesta duplicata per '%s'. Attesa del risultato.\n", file_path);
        fflush(stdout);
    }
    pthread_mutex_unlock(&pending_requests_mutex);

    // Se è la prima richiesta, calcola l'hash
    if (is_first_request) {
        uint8_t hash_binary[SHA256_HASH_SIZE];
        char local_hash_hex_string[HASH_HEX_LENGTH];
        server_status_code local_status;

        if (digest_file_sha256(file_path, hash_binary) == 0) {
            hash_to_hex_string(hash_binary, local_hash_hex_string);
            local_status = STATUS_OK;
        } else {
            local_status = STATUS_FILE_NOT_FOUND;
        }

        pthread_mutex_lock(&pending_req->mutex);
        if (local_status == STATUS_OK) {
             pending_req->result_hash = strdup(local_hash_hex_string);
        } else {
             // In caso di errore, potremmo memorizzare un messaggio di errore o NULL
             pending_req->result_hash = NULL; 
        }
        pending_req->status = local_status;
        pthread_cond_broadcast(&pending_req->cond); // Sveglia tutti i thread in attesa
        pthread_mutex_unlock(&pending_req->mutex);

        // Copia il risultato nel buffer di output
        if (local_status == STATUS_OK) {
            strcpy(calculated_hash, local_hash_hex_string);
        } else {
            strcpy(calculated_hash, "ERROR_CALC"); 
        }
        *status_code = local_status;
    } else {
        // Se non è la prima richiesta, attendi il risultato
        pthread_mutex_lock(&pending_req->mutex);
        while (pending_req->result_hash == NULL && pending_req->status == -1) {
            pthread_cond_wait(&pending_req->cond, &pending_req->mutex);
        }
        if (pending_req->status == STATUS_OK && pending_req->result_hash != NULL) {
            strcpy(calculated_hash, pending_req->result_hash);
        } else {
            strcpy(calculated_hash, "ERROR_PENDING_FAIL"); // Errore durante l'attesa del duplicato
        }
        *status_code = pending_req->status;
        pthread_mutex_unlock(&pending_req->mutex);
    }

    // Decrementa il contatore di riferimenti e pulisci se nessuno sta più aspettando
    pthread_mutex_lock(&pending_requests_mutex);
    pending_req->ref_count--;
    if (pending_req->ref_count == 0) {
        // Rimuovi l'elemento dalla lista e pulisci le risorse
        pending_request_t *temp = pending_requests_head;
        pending_request_t *prev = NULL;
        while (temp != NULL && temp != pending_req) {
            prev = temp;
            temp = temp->next;
        }
        if (temp != NULL) {
            if (prev == NULL) { // È la testa
                pending_requests_head = temp->next;
            } else {
                prev->next = temp->next;
            }
        }
        printf("Pulizia richiesta in corso per '%s'.\n", pending_req->file_path);
        fflush(stdout);
        free(pending_req->file_path);
        free(pending_req->result_hash);
        pthread_cond_destroy(&pending_req->cond);
        pthread_mutex_destroy(&pending_req->mutex);
        free(pending_req);
    }
    pthread_mutex_unlock(&pending_requests_mutex);

    return 0;
}


/**
 * @brief Funzione eseguita dai thread worker del pool.
 *
 * I worker prelevano le richieste dalla coda, le elaborano (usando cache e gestione duplicati)
 * e inviano la risposta al client.
 *
 * @param arg Non usato (NULL).
 * @return NULL.
 */
void *worker_thread_function(void *arg) {
    while (1) {
        work_item_t *item = dequeue_request(); // Attende e preleva una richiesta
        if (item == NULL) {
            continue;
        }

        client_request_t current_request = item->request;
        free(item);

        char hash_hex_string[HASH_HEX_LENGTH];
        server_status_code status = STATUS_OK;
        int client_fifo_fd = -1;

        printf("Worker %lu: Elaborando '%s' (dim: %lld).\n", pthread_self(),
               current_request.file_path, current_request.file_size);
        fflush(stdout);

        // 1. Controlla la cache
        char *cached_hash = get_from_cache(current_request.file_path);
        if (cached_hash != NULL) {
            strcpy(hash_hex_string, cached_hash);
            status = STATUS_OK;
            free(cached_hash);
            printf("Worker %lu: Trovato in cache per '%s': %s\n", pthread_self(),
                   current_request.file_path, hash_hex_string);
            fflush(stdout);
        } else {
            // 2. Se non in cache, gestisci richieste duplicate o calcola
            if (handle_duplicate_request(current_request.file_path, hash_hex_string, &status) == 0) {
                 if (status == STATUS_OK) {
                     add_to_cache(current_request.file_path, hash_hex_string);
                 }
            } else {
                fprintf(stderr, "Worker %lu: Errore critico in handle_duplicate_request per '%s'.\n",
                        pthread_self(), current_request.file_path);
                fflush(stderr);
                status = STATUS_GENERIC_ERROR;
                strcpy(hash_hex_string, "ERROR_SERVER");
            }
        }

        // 3. Apri la FIFO di risposta del client
        client_fifo_fd = open(current_request.client_response_fifo_path, O_WRONLY);
        if (client_fifo_fd == -1) {
            perror("Worker: Errore nell'apertura della FIFO di risposta del client");
            fflush(stderr);
            continue;
        }

        // 4. Invia lo stato e il risultato al client
        if (write(client_fifo_fd, &status, sizeof(status)) < 0) {
            perror("Worker: Errore nell'invio dello stato al client via FIFO");
            fflush(stderr);
        } else {
            if (status == STATUS_OK) {
                if (write(client_fifo_fd, hash_hex_string, strlen(hash_hex_string) + 1) < 0) {
                    perror("Worker: Errore nell'invio dell'hash al client via FIFO");
                    fflush(stderr);
                }
            } else {
                const char *error_msg;
                if (status == STATUS_FILE_NOT_FOUND) {
                    error_msg = "ERRORE: File non trovato o non leggibile sul server.";
                } else if (status == STATUS_INVALID_REQUEST) {
                    error_msg = "ERRORE: Richiesta non valida.";
                } else {
                    error_msg = "ERRORE: Errore generico del server.";
                }
                if (write(client_fifo_fd, error_msg, strlen(error_msg) + 1) < 0) {
                    perror("Worker: Errore nell'invio del messaggio di errore al client via FIFO");
                    fflush(stderr);
                }
            }
        }

        close(client_fifo_fd);
        printf("Worker %lu: Richiesta per '%s' completata. FIFO chiusa.\n", pthread_self(), current_request.file_path);
        fflush(stdout);
    }
    return NULL;
}

int main() {
    int server_request_fifo_fd;
    char request_message_buffer[MAX_REQUEST_MESSAGE_LENGTH];

    // Inizializza i thread worker
    pthread_t worker_tids[NUM_WORKER_THREADS];
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        if (pthread_create(&worker_tids[i], NULL, worker_thread_function, NULL) != 0) {
            perror("Errore nella creazione del thread worker");
            fflush(stderr);
            exit(EXIT_FAILURE);
        }
        pthread_detach(worker_tids[i]); // I thread si gestiranno autonomamente
    }
    printf("Avviati %d thread worker.\n", NUM_WORKER_THREADS);
    fflush(stdout);

    // Crea la FIFO principale del server per le richieste
    if (mkfifo(SERVER_REQUEST_FIFO, 0666) == -1) {
        if (errno != EEXIST) {
            perror("Errore nella creazione della FIFO del server");
            fflush(stderr);
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "La FIFO del server '%s' esiste già\n", SERVER_REQUEST_FIFO);
        fflush(stderr);
    }

    // Apre la FIFO del server in lettura
    server_request_fifo_fd = open(SERVER_REQUEST_FIFO, O_RDONLY);
    if (server_request_fifo_fd == -1) {
        perror("Errore nell'apertura della FIFO del server per la lettura");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    printf("Server avviato. In attesa di richieste sulla FIFO '%s'...\n", SERVER_REQUEST_FIFO);
    fflush(stdout);

    // Mantiene un descrittore di scrittura aperto per evitare EOF prematuri sulla FIFO di lettura
    int dummy_fd = open(SERVER_REQUEST_FIFO, O_WRONLY | O_NONBLOCK);
    if (dummy_fd == -1) {
        perror("Attenzione: Impossibile aprire dummy FD per la FIFO del server.");
        fflush(stderr);
    }

    // Loop principale di lettura delle richieste
    while (1) {
        memset(request_message_buffer, 0, sizeof(request_message_buffer));
        ssize_t bytes_read = read(server_request_fifo_fd, request_message_buffer, MAX_REQUEST_MESSAGE_LENGTH - 1);
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                printf("Tutti i mittenti della FIFO hanno chiuso. Riapertura della FIFO...\n");
                fflush(stdout);
                close(server_request_fifo_fd);
                server_request_fifo_fd = open(SERVER_REQUEST_FIFO, O_RDONLY);
                 if (server_request_fifo_fd == -1) {
                    perror("Errore riapertura FIFO del server");
                    fflush(stderr);
                    break;
                 }
                continue;
            } else {
                perror("Errore nella lettura dalla FIFO del server");
                fflush(stderr);
                break;
            }
        }
        request_message_buffer[bytes_read] = '\0';

        printf("Ricevuta richiesta RAW: '%s'\n", request_message_buffer);
        fflush(stdout);

        // Parsing del messaggio: percorso_file | dimensione_file | percorso_fifo_risposta
        char *file_path_token = strtok(request_message_buffer, MESSAGE_DELIMITER);
        char *file_size_token = strtok(NULL, MESSAGE_DELIMITER);
        char *client_fifo_path_token = strtok(NULL, MESSAGE_DELIMITER);

        if (file_path_token == NULL || file_size_token == NULL || client_fifo_path_token == NULL) {
            fprintf(stderr, "Errore: Messaggio di richiesta non valido: '%s'\n", request_message_buffer);
            fflush(stderr);
            continue;
        }
        
        client_request_t new_request;
        strncpy(new_request.file_path, file_path_token, MAX_PATH_LENGTH - 1);
        new_request.file_path[MAX_PATH_LENGTH - 1] = '\0';

        new_request.file_size = atoll(file_size_token);

        strncpy(new_request.client_response_fifo_path, client_fifo_path_token, MAX_FIFO_PATH_LENGTH - 1);
        new_request.client_response_fifo_path[MAX_FIFO_PATH_LENGTH - 1] = '\0';

        // Aggiungi la richiesta alla coda di lavoro
        enqueue_request(new_request);
        printf("Richiesta accodata per file: '%s' (dim: %lld).\n", new_request.file_path, new_request.file_size);
        fflush(stdout);
    }

    // Pulizia
    if (dummy_fd != -1) close(dummy_fd);
    close(server_request_fifo_fd);
    unlink(SERVER_REQUEST_FIFO);

    return 0;
}