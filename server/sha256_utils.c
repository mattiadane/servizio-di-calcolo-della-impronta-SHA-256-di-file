#include "sha256_utils.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int digest_file_sha256(const char *filename, uint8_t *hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    char buffer[4096]; 

    int file_fd = open(filename, O_RDONLY);
    if (file_fd == -1) {
        perror("Errore nell'apertura del file");
        return -1;
    }

    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
        SHA256_Update(&ctx, (const unsigned char *)buffer, bytes_read);
    }

    if (bytes_read < 0) {
        perror("Errore nella lettura del file");
        close(file_fd);
        return -1;
    }

    SHA256_Final(hash, &ctx);
    close(file_fd);
    return 0;
}

void hash_to_hex_string(const uint8_t *hash, char *hex_hash) {
    for(int i = 0; i < SHA256_HASH_SIZE; i++) {
        sprintf(hex_hash + (i * 2), "%02x", hash[i]);
    }
    hex_hash[SHA256_HASH_SIZE * 2] = '\0';
}