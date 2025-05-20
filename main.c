#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <termios.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

bool is_printable(char c) {
    return (c >= 32 && c <= 126);
}

char term_buf[32] = {};
void enable_altbuffer(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[?1049h");
    write(0, term_buf, len);
}
void disable_altbuffer(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[?1049l");
    write(0, term_buf, len);
}
void save_term(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[?47h");
    write(0, term_buf, len);
}
void restore_term(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[?47l");
    write(0, term_buf, len);
}
void clear_term(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[2J");
    write(0, term_buf, len);
}
void reset_cursor(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[H");
    write(0, term_buf, len);
}
void set_cursor(int row, int col) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[%d;%dH", row, col);
    write(0, term_buf, len);
}
void erase_line(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[2K");
    write(0, term_buf, len);
}
void set_foreground(int color) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[38;5;%dm", color);
    write(0, term_buf, len);
}
void set_background(int color) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[48;5;%dm", color);
    write(0, term_buf, len);
}
void reset_color(void) {
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[0m");
    write(0, term_buf, len);
}

typedef struct {
    char *name;
    char *data;
    uint64_t size;
} File;

void print_block(File file, uint64_t buffer_size, uint64_t offset) {
    if (((int64_t)file.size - (int64_t)offset) <= 0) {
        printf("no bytes to display!\n");
        return;
    }

    uint64_t rem_file_size = file.size - offset;
    uint64_t read_size = buffer_size;
    if (rem_file_size < buffer_size) {
        read_size = rem_file_size % buffer_size;
    }

    char *buffer = file.data + offset;

    uint64_t chunk_size = 16;
    for (int i = 0; i < (buffer_size / chunk_size); i++) {
        uint64_t sub_idx = i * chunk_size;

        uint8_t *row = (uint8_t *)buffer + sub_idx;
        if (sub_idx >= read_size) {
            break;
        }

        printf("\x1b[38;5;248m%08llx\x1b[0m: ", offset + sub_idx);

        for (int j = 0; j < chunk_size; j++) {
            if (sub_idx + j >= read_size) {
                printf("   ");
                continue;
            }

            printf("%02x ", row[j]);
        }
        printf(" \x1b[38;5;248m");
        for (int j = 0; j < chunk_size; j++) {
            if (sub_idx + j >= read_size) {
                printf("   ");
                continue;
            }

            char ch = row[j];
            if (!is_printable(ch)) {
                ch = '.';
            }
            printf("%c", ch);
        }
        printf("\x1b[0m\n");

        if (i >= read_size / chunk_size) {
            break;
        }
    }

    return;
}



struct termios orig_termios;
void cleanup_term(void) {
    tcsetattr(0, TCSAFLUSH, &orig_termios);
    disable_altbuffer();
}
void init_term(void) {
    enable_altbuffer();

    tcgetattr(0, &orig_termios);
    atexit(cleanup_term);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(0, TCSAFLUSH, &raw);
}

typedef struct {
    uint64_t rows;
    uint64_t cols;
} Window;

bool get_term_size(Window *w) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return false;
    }

    w->rows = ws.ws_row - 1;
    w->cols = ws.ws_col;
    return true;
}

typedef struct {
    File file;
    Window w;
    int x;
    int y;

    uint64_t block_size;
    uint64_t offset;
} ViewState;
ViewState view;

void refresh_screen(void) {
    clear_term();
    reset_cursor();

    set_background(244);
    set_foreground(232);
    erase_line();

    printf(" %s\n", view.file.name);

    reset_color();
    print_block(view.file, (view.w.rows - 1) * 16, view.offset);
    set_cursor(view.w.rows + 1, 1);
}

void handle_sigwinch(int unused) {
    get_term_size(&view.w);
    refresh_screen();
}

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

    init_term();

    view = (ViewState){
        .file = (File){.name = argv[1], .data = file_bytes, .size = file_size},
        .offset = 0,
        .x = 1,
        .y = 1
    };
    get_term_size(&view.w);
    signal(SIGWINCH, handle_sigwinch);

    for (;;) {
        refresh_screen();

        char ch;
        read(0, &ch, 1);

        switch (ch) {
            case 'w': {
                view.offset = MAX((int64_t)(view.offset - 16), 0);
            } break;
            case 's': {
                view.offset = MIN(view.offset + 16, view.file.size);
            } break;
            case 'q': {
                return 1;
            } break;
        }
    }
}
