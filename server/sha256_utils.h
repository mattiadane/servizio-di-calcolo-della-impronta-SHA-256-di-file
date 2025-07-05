#ifndef SHA256_UTILS_H
#define SHA256_UTILS_H

#include <stdint.h>
#include <openssl/sha.h>

#define SHA256_HASH_SIZE 32

/**
 * @brief Calcola l'hash SHA-256 di un file specificato dal percorso.
 *
 * @param filename Il percorso del file da processare.
 * @param hash Buffer di 32 byte dove verrà memorizzato l'hash binario.
 * @return 0 in caso di successo, -1 in caso di errore (es. file non trovato/leggibile).
 */
int digest_file_sha256(const char *filename, uint8_t *hash);

/**
 * @brief Converte un hash SHA-256 binario in una stringa esadecimale.
 *
 * @param hash L'hash binario (array di 32 byte).
 * @param hex_hash Buffer di almeno 65 byte per la stringa esadecimale (32*2 + 1 per '\0').
 */
void hash_to_hex_string(const uint8_t *hash, char *hex_hash);

#endif 
