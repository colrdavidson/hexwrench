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

#define ARR_EXPAND(arr) do {                                                   \
    if (((arr)->len + 1) > (arr)->cap) {                                       \
        (arr)->cap = MAX((arr)->cap * 2, 8);                                   \
        (arr)->data = realloc((arr)->data, sizeof(*(arr)->data) * (arr)->cap); \
    }                                                                          \
    (arr)->len += 1;                                                           \
} while (0);

#define ARR_APPEND(arr, val) do {                                              \
    if (((arr)->len + 1) > (arr)->cap) {                                       \
        (arr)->cap = MAX((arr)->cap * 2, 8);                                   \
        (arr)->data = realloc((arr)->data, sizeof(*(arr)->data) * (arr)->cap); \
    }                                                                          \
                                                                               \
    (arr)->data[(arr)->len] = (val);                                           \
    (arr)->len += 1;                                                           \
} while (0);

#define ARR_INSERT(arr, val, off) do {                                                                    \
    if ((off) == (arr)->len) {                                                                            \
        ARR_APPEND(arr, val);                                                                             \
    } else {                                                                                              \
        ARR_EXPAND(arr);                                                                                  \
        memmove(&(arr)->data[(off)+1], &(arr)->data[(off)], sizeof(*(arr)->data) * ((arr)->len - (off))); \
        (arr)->data[(off)] = (val);                                                                       \
    }                                                                                                     \
} while (0);

#define ARR_DELETE(arr, off) do {                                                                     \
    memmove(&(arr)->data[(off)], &(arr)->data[(off)+1], sizeof(*(arr)->data) * ((arr)->len - (off))); \
    (arr)->len -= 1;                                                                                  \
} while (0);

typedef struct {
    char *name;
    uint8_t *data;
    uint64_t size;
} File;

typedef struct {
    uint8_t *data;
    uint64_t len;

    bool patch;
} Block;

typedef struct {
    Block *data;
    uint64_t len;
    uint64_t cap;
} BlockArr;

typedef struct {
    uint64_t rows;
    uint64_t cols;
} Window;

typedef struct {
    File file;
    Window w;
    int x;
    int y;

    uint64_t view_size;
    uint64_t offset;

    BlockArr blocks;
} ViewState;

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

void print_view(uint8_t *buffer, uint64_t total_size, uint64_t buffer_size, uint64_t offset) {
    if (((int64_t)total_size - (int64_t)offset) <= 0) {
        printf("no bytes to display!\n");
        return;
    }

    uint64_t rem_file_size = total_size - offset;
    uint64_t read_size = buffer_size;
    if (rem_file_size < buffer_size) {
        read_size = rem_file_size % buffer_size;
    }

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
            uint64_t byte_idx = offset + sub_idx + j;

            uint8_t ch = row[j];
            printf("%02x ", ch);
        }
        printf(" \x1b[38;5;248m");
        for (int j = 0; j < chunk_size; j++) {
            if (sub_idx + j >= read_size) {
                printf("   ");
                continue;
            }
            uint64_t byte_idx = offset + sub_idx + j;

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


bool get_term_size(Window *w) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return false;
    }

    w->rows = ws.ws_row - 1;
    w->cols = ws.ws_col;
    return true;
}

ViewState view;
void refresh_screen(void) {
    clear_term();
    reset_cursor();

    set_background(244);
    set_foreground(232);
    erase_line();

    printf("%s -- %llu bytes\n", view.file.name, view.file.size);

    reset_color();
    //print_view(&view, (view.w.rows - 1) * 16, view.offset);
    set_cursor(view.w.rows + 1, 1);
}

void handle_sigwinch(int unused) {
    get_term_size(&view.w);
    refresh_screen();
}

Block new_block(uint8_t *data, uint64_t len) {
    return (Block){.data = data, .len = len, .patch = true};
}

void print_block(Block *b) {
    printf("Block %p %llu %s\n", b->data, b->len, b->patch ? "(patched)" : "");
}

void print_blocks(BlockArr *blocks) {
    uint64_t accum_offset = 0;
    for (int i = 0; i < blocks->len; i++) {
        Block *b = &blocks->data[i];
        printf("Block %p | off: %llu len: %llu %s\n", b->data, accum_offset, b->len, b->patch ? "(patched)" : "");
        accum_offset += b->len;
    }
}

uint64_t get_total_size(ViewState *view) {
    uint64_t accum_offset = 0;
    for (int i = 0; i < view->blocks.len; i++) {
        accum_offset += view->blocks.data[i].len;
    }
    return accum_offset;
}

void delete_data(ViewState *view, uint64_t offset, uint64_t len) {
    if (len == 0) {
        return;
    }

    uint64_t d_head = offset;
    uint64_t d_tail = offset + len;

    uint64_t accum_deleted = 0;
    uint64_t accum_offset = 0;
    for (int i = 0; i < view->blocks.len; i++) {
        Block *b = &view->blocks.data[i];

        uint64_t b_head = accum_offset;
        uint64_t b_tail = accum_offset + b->len;

        accum_offset += b->len;

        // Skip any block that ends before our delete
        if (d_head > b_tail) {
            continue;
        }

        // exit, we've seen all the blocks we need to edit
        if (b_head >= d_tail) {
            break;
        }

        // if we completely cover a block, just delete it
        if (d_head <= b_head && d_tail >= b_tail && b->len <= len) {
            accum_deleted += b->len;
            b->len = 0;

        // If we overlap a block's start
        } else if (b_head >= d_head && d_tail <= b_tail) {

            uint64_t delete_leftovers = len - accum_deleted;
            b->data += delete_leftovers;
            b->len -= delete_leftovers;
            accum_deleted += delete_leftovers;

        // If we overlap a block's end
        } else if (d_head > b_head && d_tail > b_tail && d_head < b_tail) {

            uint64_t delete_coverage = b_tail - d_head;
            b->len -= delete_coverage;
            accum_deleted += delete_coverage;
        }

        if (accum_deleted == len) {
            break;
        }
    }
}

void insert_data(ViewState *view, uint64_t offset, Block block) {
    if (block.len == 0) {
        return;
    }

    Block *new_b   = &block;
    Block *start_b = &view->blocks.data[0];
    Block *end_b   = &view->blocks.data[view->blocks.len-1];

    uint64_t accum_offset = get_total_size(view);

    uint64_t new_b_head   = offset;
    uint64_t new_b_tail   = offset + new_b->len;

    uint64_t start_b_head = 0;
    uint64_t start_b_tail = start_b->len;

    uint64_t end_b_head   = accum_offset - end_b->len;
    uint64_t end_b_tail   = accum_offset;

    // If we need to insert a new block at the end of our data
    if (new_b_head >= end_b_tail) {
        ARR_APPEND(&view->blocks, block);

        return;

    // If we just need to push the existing block forward a bit
    } else if (new_b_head <= start_b_head) {
        Block post_block = new_block(start_b->data, start_b->len);

        Block old_b = *start_b;
        *start_b = block;

        ARR_INSERT(&view->blocks, post_block, 1);

        uint64_t new_total_len = block.len + post_block.len;
        if (new_total_len - post_block.len != new_b->len) {
            printf("invalid insert!\n");
            exit(1);
        }

        return;

    // If we need to do a before/after block split
    } else {
        uint64_t accum_offset = 0;
        for (int i = 0; i < view->blocks.len; i++) {
            Block *b = &view->blocks.data[i];

            uint64_t b_head  = accum_offset;
            accum_offset += b->len;

            // Skip any block that ends before our block
            if (new_b_head > accum_offset) {
                continue;
            }

            Block pre_block  = new_block(
                b->data,
                new_b_head - b_head
            );
            Block mid_block  = *new_b;
            Block post_block = new_block(
                b->data + pre_block.len,
                b->len - pre_block.len
            );

            Block old_b = *b;
            *b = pre_block;

            ARR_INSERT(&view->blocks, mid_block, i+1);
            ARR_INSERT(&view->blocks, post_block, i+2);

            uint64_t new_total_len = pre_block.len + mid_block.len + post_block.len;
            if (new_total_len - old_b.len != new_b->len) {
                printf("invalid insert!\n");
                exit(1);
            }

            return;
        }
    }
}

bool get_data(ViewState *view, uint64_t offset, uint8_t *buffer, uint64_t len) {
    uint64_t accum_offset = 0;

    for (int i = 0; i < view->blocks.len; i++) {
        Block *b = &view->blocks.data[i];

        uint64_t view_start  = offset;
        uint64_t view_end    = offset + len;
        uint64_t block_start = accum_offset;
        uint64_t block_end   = accum_offset + b->len;
        accum_offset += b->len;

        // If the block is before the view range
        if (block_end < view_start) {
            continue;
        }

        // If there's range overlap
        if (view_start <= block_end && block_start <= view_end) {

            uint64_t start_offset = MAX((int64_t)block_start - (int64_t)view_start, 0);
            uint64_t bytes_to_grab = MIN(view_end, block_end);

            memcpy(buffer + start_offset, b->data, bytes_to_grab);
        }

        // If the block is past the view, or crosses the view end
        if (block_start > view_end || block_end > view_end) {
            return true;
        }
    }

    return true;
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

    uint8_t *file_bytes = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_bytes == MAP_FAILED) {
        printf("Failed to map file %s\n", argv[1]);
        return 1;
    }

    //init_term();

    view = (ViewState){
        .file = (File){.name = argv[1], .data = file_bytes, .size = file_size},
        .x = 1,
        .y = 1,
    };
    ARR_APPEND(&view.blocks, ((Block){.data = view.file.data, .len = view.file.size}));


    uint64_t view_offset = 0;
    uint64_t buffer_len = 50;
    uint8_t *buffer = calloc(1, buffer_len);

    insert_data(&view, 0, new_block((uint8_t *)"<3 ", 3));
    insert_data(&view, 0, new_block((uint8_t *)":) ", 3));
    delete_data(&view, 1, 6);

    get_data(&view, view_offset, buffer, buffer_len);
    print_view(buffer, get_total_size(&view), buffer_len, view_offset);
/*

    get_term_size(&view.w);
    signal(SIGWINCH, handle_sigwinch);

    bool insert_mode = false;
    for (;;) {
        refresh_screen();
        char ch;

    read_char:
        read(0, &ch, 1);

        uint64_t max_offset = (uint64_t)(MAX(0, (int64_t)(view.file.size - (view.file.size % 16)) - (int64_t)((view.w.rows - 2) * 16)));

        if (!insert_mode) {
            switch (ch) {
                case 'g': {
                    view.offset = 0;
                } break;
                case 'G': {
                    view.offset = max_offset;
                } break;
                case 'k': {
                    view.offset = (uint64_t)MAX((int64_t)(view.offset - 16), 0);
                } break;
                case 'j': {
                    view.offset = MIN(view.offset + 16, max_offset);
                } break;
                case 'q': {
                    return 1;
                } break;
                default: {
                    goto read_char;
                } break;
            }
        }
    }
*/
}
