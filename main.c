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

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define LOG2(...) fprintf(stderr, __VA_ARGS__)
#undef LOG
#define LOG(...)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BLOCK_LEN 1024

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
    uint8_t *data;
    int64_t len;
} Str;

Str stralloc(const char *str) {
    int len = strlen(str);
    uint8_t *tmp = malloc(len + 1);
    strncpy((char *)tmp, str, len + 1);
    return (Str){.data = tmp, .len = len};
}

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
    bool updated;

    uint8_t *buffer; 
    uint64_t buffer_len;
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
    int len = snprintf(term_buf, sizeof(term_buf), "\x1b[%d;%dH", col, row);
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

void print_view(uint8_t *buffer, uint64_t buffer_size, uint64_t total_size, uint64_t offset) {
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

bool has_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    return a_end >= b_start && a_start <= b_end;
}

Block new_block(uint8_t *data, uint64_t len, bool patch) {
    return (Block){.data = data, .len = len, .patch = patch};
}

Block new_block_from_str(const char *str) {
    Str s = stralloc(str);
    return (Block){.data = s.data, .len = s.len, .patch = false};
}

void print_block(Block *b) {
    LOG("Block %p %llu %s\n", b->data, b->len, b->patch ? "(patched)" : "");
}
void print_block_w_offset(Block *b, uint64_t offset) {
    LOG("Block %p %llx -> %llx %s\n", b->data, offset, offset + b->len, b->patch ? "(patched)" : "");
}

void print_blocks(BlockArr *blocks) {
    uint64_t accum_offset = 0;
    for (int i = 0; i < blocks->len; i++) {
        Block *b = &blocks->data[i];
        LOG("Block %p | off: %llu len: %llu %s\n", b->data, accum_offset, b->len, b->patch ? "(patched)" : "");
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

void generate_patchblock(ViewState *view, int block_idx, uint64_t inner_offset) {
    Block *b = &view->blocks.data[block_idx];

    uint64_t rounded_offset = inner_offset - (inner_offset % BLOCK_LEN);
    uint64_t prev_b_len = BLOCK_LEN * (inner_offset / BLOCK_LEN);
    uint64_t seg_len = b->len - rounded_offset;

    uint64_t block_len = MIN(seg_len, BLOCK_LEN);

    uint8_t *bbuffer = malloc(block_len);
    memcpy(bbuffer, b->data + rounded_offset, block_len);
    Block patch_block = new_block(bbuffer, block_len, true);

    LOG("generating patchblock\n");
    LOG("inner: %llu, rounded: %llu | seg_len: %llu, block_len: %llu\n", inner_offset, rounded_offset, seg_len, block_len);
    print_block_w_offset(&patch_block, rounded_offset);

    if (b->len < BLOCK_LEN) {
        LOG("Converting the whole block to a patch block\n");
        *b = patch_block;
        return;
    }

    // Do a head-insert
    if (rounded_offset == 0) {
        LOG("head patch\n");
        LOG("pre-patch blocks\n");
        print_blocks(&view->blocks);

        Block old_b = *b;
        *b = patch_block;
        Block next_b = new_block(old_b.data + patch_block.len, old_b.len - patch_block.len, false);
        ARR_INSERT(&view->blocks, next_b, block_idx+1);

        LOG("post-patch blocks\n");
        print_blocks(&view->blocks);

        return;
    } else {
        LOG("middle patch\n");
        //print_blocks(&view->blocks);

        Block old_b = *b;
        Block prev_b = new_block(old_b.data, prev_b_len, false);
        Block next_b = new_block(old_b.data + rounded_offset + patch_block.len, old_b.len - prev_b.len - patch_block.len, false);

        LOG("block to split, splitting at %llu\n", rounded_offset);
        print_block(&old_b);

        LOG("pieces\n");
        print_block(&prev_b);
        print_block(&patch_block);
        print_block(&next_b);

        *b = prev_b;
        ARR_INSERT(&view->blocks, patch_block, block_idx+1);
        ARR_INSERT(&view->blocks, next_b,      block_idx+2);

        LOG("middle insert done\n");
        //print_blocks(&view->blocks);
        return;
    }
}

void generate_patchruns(ViewState *view, uint64_t offset, uint64_t len) {
    if (len == 0) {
        return;
    }

    uint64_t d_head = offset;
    uint64_t d_tail = offset + len;

    uint64_t accum_offset = 0;
    for (int i = 0; i < view->blocks.len; i++) {
        Block *b = &view->blocks.data[i];

        // skip over tombstones
        if (b->len == 0) {
            continue;
        }

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

        uint64_t inner_offset = (uint64_t)MAX((int64_t)d_head - (int64_t)b_head, (int64_t)b_head);
        if (!b->patch) {
            generate_patchblock(view, i, inner_offset);
        }
    }
}

void delete_data(ViewState *view, uint64_t offset, uint64_t len) {
    if (len == 0) {
        return;
    }

    LOG("deleting from %llx -> %llx\n", offset, offset+len);
    generate_patchruns(view, offset, len);

    uint64_t d_head = offset;
    uint64_t d_tail = offset + len;

    uint64_t accum_deleted = 0;
    uint64_t accum_offset = 0;
    for (int i = 0; i < view->blocks.len; i++) {
        Block *b = &view->blocks.data[i];

        // skip over tombstones
        if (b->len == 0) {
            continue;
        }

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

        uint64_t inner_offset = (uint64_t)MAX((int64_t)d_head - (int64_t)b_head, (int64_t)b_head);

        // if we completely cover a block, just delete it
        if (d_head <= b_head && d_tail >= b_tail && b->len <= len) {
            LOG("deleting full block\n");
            accum_deleted += b->len;
            b->len = 0;

        // If we overlap a block's start
        } else if (b_head >= d_head && d_tail <= b_tail) {
            LOG("deleting block start\n");
            print_block_w_offset(b, accum_offset);

            uint64_t delete_leftovers = len - accum_deleted;
            b->data += delete_leftovers;
            b->len -= delete_leftovers;
            accum_deleted += delete_leftovers;

        // If we overlap a block's end
        } else if (d_head > b_head && d_tail > b_tail && d_head < b_tail) {
            LOG("deleting block end\n");

            uint64_t delete_coverage = b_tail - d_head;
            b->len -= delete_coverage;
            accum_deleted += delete_coverage;

        // Middle-deletes
        } else if (d_head >= b_head && d_tail <= b_tail) {
            LOG("deleting block middle\n");
            memmove(b->data + inner_offset, b->data + inner_offset + len, b->len - len);
            b->len -= len;
            accum_deleted += len;
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
        Block post_block = *start_b;
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
                new_b_head - b_head,
                b->patch
            );
            Block mid_block  = *new_b;
            Block post_block = new_block(
                b->data + pre_block.len,
                b->len - pre_block.len,
                b->patch
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
    uint64_t accum_len = 0;

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

            uint64_t start_offset = MAX((int64_t)view_start - (int64_t)block_start, 0);
            uint64_t bytes_to_grab = MIN(len, b->len);

            //printf("buffer[0x%llx] | writing %llu bytes, from block[%llx]\n", accum_len, bytes_to_grab, start_offset);
            memcpy(buffer + accum_len, b->data + start_offset, bytes_to_grab);
            accum_len += bytes_to_grab;
        }

        // If the block is past the view, or crosses the view end
        if (block_start > view_end || block_end > view_end) {
            return true;
        }
    }

    return true;
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
void update_buffer_size(void) {
    uint64_t req_len = (view.w.rows - 1) * 16;
    if (view.buffer_len != req_len) {
        view.buffer_len = req_len;
        view.buffer = realloc(view.buffer, view.buffer_len);
        view.updated = true;
    }
}

void refresh_screen(void) {
    if (view.updated) {
        clear_term();
    }

    reset_cursor();
    if (view.updated) {
        set_background(244);
        set_foreground(232);
        erase_line();

        printf("%s -- %llu bytes\n", view.file.name, view.file.size);

        reset_color();
        update_buffer_size();

        get_data(&view, view.offset, view.buffer, view.buffer_len);
        print_view(view.buffer, view.buffer_len, get_total_size(&view), view.offset);
    }

    int cluster_adj = view.x / 2;
    int inner_adj = view.x % 2;
    int cur_x = 11 + (cluster_adj * 3) + inner_adj;

    set_cursor(cur_x, view.y + 2);
    view.updated = false;
}

void handle_sigwinch(int unused) {
    get_term_size(&view.w);
    view.updated = true;
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

    uint8_t *file_bytes = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_bytes == MAP_FAILED) {
        printf("Failed to map file %s\n", argv[1]);
        return 1;
    }

    init_term();

    view = (ViewState){
        .file = (File){.name = argv[1], .data = file_bytes, .size = file_size},
        .x = 0,
        .y = 0,
        .buffer_len = 0,
        .buffer = NULL,
        .updated = true
    };
    ARR_APPEND(&view.blocks, new_block(view.file.data, view.file.size, false));
    update_buffer_size();

    //insert_data(&view, 0, new_block_from_str("<3 "));
    //insert_data(&view, 0, new_block_from_str(":) "));
    //delete_data(&view, 1, 7);

    get_term_size(&view.w);
    signal(SIGWINCH, handle_sigwinch);

    bool insert_mode = false;
    for (;;) {
        refresh_screen();
        char ch;

    read_char:
        read(0, &ch, 1);

        int max_rows = view.w.rows - 2;
        uint64_t max_offset = (uint64_t)(MAX(0, (int64_t)(view.file.size - (view.file.size % 16)) - (int64_t)(max_rows * 16)));
        int max_cols = 32;

        int cursor_idx = ((view.y * max_cols) + view.x) / 2;

        if (!insert_mode) {
            switch (ch) {
                case 'q': {
                    return 1;
                } break;

                // actions
                case 'i': {
                    insert_data(&view, cursor_idx, new_block_from_str("i"));
                    view.updated = true;
                } break;
                case 'x': {
                    delete_data(&view, cursor_idx, 1);
                    view.updated = true;
                } break;

                // motions
                case 'g': {
                    view.y = 0;
                    uint64_t new_offset = 0;
                    if (view.offset != new_offset) {
                        view.offset = new_offset;
                        view.updated = true;
                    }
                } break;
                case 'G': {
                    view.y = max_rows;
                    uint64_t new_offset = max_offset;
                    if (view.offset != new_offset) {
                        view.offset = new_offset;
                        view.updated = true;
                    }
                } break;
                case 'h': {
                    view.x = MAX(view.x - 1, 0);
                } break;
                case 'l': {
                    view.x = MIN(view.x + 1, max_cols - 1);
                } break;
                case 'k': {
                    view.y = MAX(view.y - 1, 0);
                    if (view.y - 1 < 0) {
                        uint64_t new_offset = (uint64_t)MAX((int64_t)(view.offset - 16), 0);
                        if (view.offset != new_offset) {
                            view.offset = new_offset;
                            view.updated = true;
                        }
                    }
                } break;
                case 'j': {
                    view.y = MIN(view.y + 1, max_rows);
                    if (view.y + 1 > max_rows) {
                        uint64_t new_offset = MIN(view.offset + 16, max_offset);
                        if (view.offset != new_offset) {
                            view.offset = new_offset;
                            view.updated = true;
                        }
                    }
                } break;

                // no clue, try again
                default: {
                    goto read_char;
                } break;
            }
        }
    }
}
