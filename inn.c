#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define INN_VERSION "0.0.1"

#define TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

typedef struct erow {
    int size;
    int rsize;
    char* chars;
    char* render;
} erow;

struct editor_config {
    int cx, cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow* row;
    struct termios orig_termios;
};
struct editor_config ECONF;

void clear_screen() {
    write(STDIN_FILENO, "\x1b[2J", 4);
    write(STDIN_FILENO, "\x1b[H", 3);
}

void die(const char* s) {
    clear_screen();
    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ECONF.orig_termios) == -1) { die("tcsetattr"); }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &ECONF.orig_termios) == -1) { die("tcgetattr"); }
    atexit(disable_raw_mode);

    struct termios raw = ECONF.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // disable ICRNL=carriage-return/new-line trans, IXON=ctrl-s/ctrl-q flow control
    raw.c_oflag &= ~(OPOST); // disable output processing
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // disable ECHO=echo, ICANON=canonical, IEXTEN=ctrl-v, ISIG=ctrl-z/ctrl-d/ctrl-c signals
    raw.c_cc[VMIN] = 0; // minimum bytes needed for read() to return
    raw.c_cc[VTIME] = 1; // read timeout after 100 milliseconds

    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &raw) == -1) { die("tcsetattr"); }
}

int read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) { die("read"); } // EAGAIN cygwin compatibility
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) { break; }
        if (buf[i] == 'R') { break; }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') { return -1; }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) { return -1; }

    return 0;
}

int get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) { return -1; }
        return get_cursor_position(rows, cols);
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

void update_row(erow* row) {
    int tabs = 0;
    int j;

    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP-1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_append_row(char* s, size_t len) {
    ECONF.row = realloc(ECONF.row, sizeof(erow) * (ECONF.numrows + 1));
    int at = ECONF.numrows;

    ECONF.row[at].size = len;
    ECONF.row[at].chars = malloc(len+1);
    memcpy(ECONF.row[at].chars, s, len);
    ECONF.row[at].chars[len] = '\0';

    ECONF.row[at].rsize = 0;
    ECONF.row[at].render = NULL;
    update_row(&ECONF.row[at]);

    ECONF.numrows++;
}

void editor_open(char* filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r')) {
            linelen--;
        }
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
}

// append buffer
struct abuf {
    char *b;
    int len;
};
#define ABUF_INT {NULL, 0}

void ab_append(struct abuf* ab, const char* s, int len) {
    char* new = realloc(ab->b, ab->len + len);

    if (new == NULL) { return; }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ab_free(struct abuf* ab) {
    free(ab->b);
}

void editor_scroll() {
    if (ECONF.cy < ECONF.rowoff) {
        ECONF.rowoff = ECONF.cy;
    }
    if (ECONF.cy >= ECONF.rowoff + ECONF.screenrows) {
        ECONF.rowoff = ECONF.cy - ECONF.screenrows + 1;
    }
    if (ECONF.cx < ECONF.coloff) {
        ECONF.coloff = ECONF.cx;
    }
    if (ECONF.cx >= ECONF.coloff + ECONF.screencols) {
        ECONF.coloff = ECONF.cx - ECONF.screencols + 1;
    }
}

void draw_rows(struct abuf* ab) {
    int y;
    for (y=0; y < ECONF.screenrows; y++) {
        int filerow = y + ECONF.rowoff;
        if (filerow >= ECONF.numrows) {
            if (ECONF.numrows == 0 && y == ECONF.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "welcome to inn version %s", INN_VERSION);
                if (welcomelen > ECONF.screencols) { welcomelen = ECONF.screencols; }

                int padding = (ECONF.screencols - welcomelen) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                }
                while (padding--) { ab_append(ab, " ", 1); }

                ab_append(ab, welcome, welcomelen);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = ECONF.row[filerow].rsize - ECONF.coloff;
            if (len < 0 ) len = 0;
            if (len > ECONF.screencols) len = ECONF.screencols;
            ab_append(ab, &ECONF.row[filerow].render[ECONF.coloff], len);
        }


        ab_append(ab, "\x1b[K", 3); // clear line right of cursor
        if (y < ECONF.screenrows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

void refresh_screen() {
    editor_scroll();

    struct abuf ab = ABUF_INT;
    ab_append(&ab, "\x1b[?25l", 6); // hide cursor
    ab_append(&ab, "\x1b[H", 3); // reset cursor position
    draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ECONF.cy - ECONF.rowoff) + 1, (ECONF.cx - ECONF.coloff) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6); // show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

void move_cursor(int key) {
    erow* row = (ECONF.cy >= ECONF.numrows) ? NULL : &ECONF.row[ECONF.cy];

    switch (key) {
        case ARROW_UP:
            if (ECONF.cy != 0) ECONF.cy--;
            break;
        case ARROW_LEFT:
            if (ECONF.cx != 0) ECONF.cx--;
            else if (ECONF.cy > 0) {
                ECONF.cy--;
                ECONF.cx = ECONF.row[ECONF.cy].size;
            }
            break;
        case ARROW_DOWN:
            if (ECONF.cy < ECONF.numrows) ECONF.cy++;
            break;
        case ARROW_RIGHT:
            if (row && ECONF.cx < row->size) ECONF.cx++;
            else if (row && ECONF.cx == row->size) {
                ECONF.cy++;
                ECONF.cx = 0;
            }
            break;
    }

    row = (ECONF.cy >= ECONF.numrows) ? NULL : &ECONF.row[ECONF.cy];
    int rowlen = row ? row->size : 0;
    if (ECONF.cx > rowlen) {
        ECONF.cx = rowlen;
    }
}

void process_keypress() {
    int c = read_key();

    switch (c) {
        case CTRL_KEY('q'):
            clear_screen();
            exit(0);
            break;
        case HOME_KEY:
            ECONF.cx = 0;
            break;
        case END_KEY:
            ECONF.cx = ECONF.screencols - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = ECONF.screenrows;
                while (times--) {
                    move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        case ARROW_LEFT:
        case ARROW_RIGHT:
        case ARROW_UP:
        case ARROW_DOWN:
            move_cursor(c);
            break;
    }
}

void init_editor() {
    ECONF.cx = 0;
    ECONF.cy = 0;
    ECONF.rowoff = 0;
    ECONF.coloff = 0;
    ECONF.numrows = 0;
    ECONF.row = NULL;
    if (get_window_size(&ECONF.screenrows, &ECONF.screencols) == -1) { die("get_window_size"); }
}

int main(int argc, char* argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    while (1) {
        refresh_screen();
        process_keypress();
    }


    return 0;
}