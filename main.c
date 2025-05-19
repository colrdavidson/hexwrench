#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Expected %s <name of file>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY, 0);
    if (fd < 0) {
        printf("Failed to open %s\n", argv[1]);
        return 1;
    }

    struct stat info;
    if (fstat(fd, &info)) {
        printf("Failed to get file info for %s\n", argv[1]);
        return 1;
    }
    uint64_t file_size = info.st_size;

    char *file_bytes = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_bytes == MAP_FAILED) {
        printf("Failed to map file %s\n", argv[1]);
        return 1;
    }

    uint64_t block_size = 64;
    uint64_t offset = 0x0;

    char *buffer = malloc(block_size);

    uint64_t rem_file_size = file_size - offset;
    uint64_t read_size = block_size;
    if (rem_file_size < block_size) {
        read_size = rem_file_size % block_size;
    }
    uint64_t zero_size = block_size - read_size;

    memcpy(buffer, file_bytes + offset, read_size);
    memset(buffer + read_size, 0, zero_size);

    uint64_t chunk_size = 16;
    for (int i = 0; i < (block_size / chunk_size); i++) {
        uint64_t sub_idx = i * chunk_size;

        uint8_t *row = (uint8_t *)buffer + sub_idx;
        if (sub_idx >= read_size) {
            break;
        }

        printf("%08llx: ", offset + sub_idx);
        for (int j = 0; j < chunk_size; j++) {
            if (sub_idx + j >= read_size) {
                printf("   ");
                continue;
            }

            printf("%02x ", row[j]);
        }
        printf("\t");
        for (int j = 0; j < chunk_size; j++) {
            if (sub_idx + j >= read_size) {
                printf("   ");
                continue;
            }

            char ch = row[j];
            if (ch == '\n' || ch == '\t') {
                ch = '.';
            }
            printf("%c", ch);
        }
        printf("\n");

        if (i >= read_size / chunk_size) {
            break;
        }
    }
}
