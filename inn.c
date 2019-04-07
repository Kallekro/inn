#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define INN_VERSION "0.0.1"

#define TAB_STOP 8
#define QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key {
    BACKSPACE = 127,
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

/* data */
typedef struct erow {
    int size;
    int rsize;
    char* chars;
    char* render;
} erow;

struct editor_config {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow* row;
    int dirty;
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};
struct editor_config ES;

/* forward declarations */
void editor_set_statusmessage(const char *fmt, ...);

/* terminal */
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
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ES.orig_termios) == -1) { die("tcsetattr"); }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &ES.orig_termios) == -1) { die("tcgetattr"); }
    atexit(disable_raw_mode);

    struct termios raw = ES.orig_termios;
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

/* row operations */
int editor_row_cxtorx(erow* row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
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

void editor_insert_row(int at, char* s, size_t len) {
    if (at < 0 || at > ES.numrows) return;

    ES.row = realloc(ES.row, sizeof(erow) * (ES.numrows + 1));
    memmove(&ES.row[at + 1], &ES.row[at], sizeof(erow) * (ES.numrows - at));

    ES.row[at].size = len;
    ES.row[at].chars = malloc(len+1);
    memcpy(ES.row[at].chars, s, len);
    ES.row[at].chars[len] = '\0';

    ES.row[at].rsize = 0;
    ES.row[at].render = NULL;
    update_row(&ES.row[at]);

    ES.numrows++;
    ES.dirty++;
}

void editor_free_row(erow* row) {
    free(row->render);
    free(row->chars);
}

void editor_delete_row(int at) {
    if (at < 0 || at >= ES.numrows) return;
    editor_free_row(&ES.row[at]);
    memmove(&ES.row[at], &ES.row[at + 1], sizeof(erow) * (ES.numrows - at - 1));
    ES.numrows--;
    ES.dirty++;
}

void row_insert_char(erow* row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    update_row(row);
    ES.dirty++;
}

void row_append_string(erow* row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    update_row(row);
    ES.dirty++;
}

void row_delete_char(erow* row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    update_row(row);
    ES.dirty++;
}

/* editor operations */
void editor_insert_char(int c) {
    if (ES.cy == ES.numrows) { editor_insert_row(ES.numrows, "", 0); }
    row_insert_char(&ES.row[ES.cy], ES.cx, c);
    ES.cx++;
}

void editor_insert_newline() {
    if (ES.cx == 0) {
        editor_insert_row(ES.cy, "", 0);
    } else {
        erow* row = &ES.row[ES.cy];
        editor_insert_row(ES.cy + 1, &row->chars[ES.cx], row->size - ES.cx);
        row = &ES.row[ES.cy];
        row->size = ES.cx;
        row->chars[row->size] = '\0';
        update_row(row);
    }
    ES.cy++;
    ES.cx = 0;
}

void editor_delete_char() {
    if (ES.cy == ES.numrows) return;
    if (ES.cx == 0 && ES.cy == 0) return;

    erow* row = &ES.row[ES.cy];
    if (ES.cx > 0) {
        row_delete_char(row, ES.cx - 1);
        ES.cx--;
    } else {
        ES.cx = ES.row[ES.cy - 1].size;
        row_append_string(&ES.row[ES.cy - 1], row->chars, row->size);
        editor_delete_row(ES.cy);
        ES.cy--;
    }
}

/* file i/o */

char* rows_to_string(int* buflen) {
    int total_len = 0;
    int j;
    for (j = 0; j < ES.numrows; j++) {
        total_len += ES.row[j].size + 1;
    }
    *buflen = total_len;

    char* buf = malloc(total_len);
    char* p = buf;
    for (j = 0; j < ES.numrows; j++) {
        memcpy(p, ES.row[j].chars, ES.row[j].size);
        p += ES.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editor_open(char* filename) {
    free(ES.filename);
    ES.filename = strdup(filename);

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
        editor_insert_row(ES.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    ES.dirty = 0;
}

void editor_save() {
    // TODO:
    // * use temporary file
    // * error checking
    if (ES.filename == NULL) return;

    int len;
    char* buf = rows_to_string(&len);

    int fd = open(ES.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                ES.dirty = 0;
                editor_set_statusmessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_statusmessage("save failed. I/O error: %s", strerror(errno));
}

/* append buffer */
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

/* output */
void editor_scroll() {
    ES.rx = 0;
    if (ES.cy < ES.numrows) {
        ES.rx = editor_row_cxtorx(&ES.row[ES.cy], ES.cx);
    }

    if (ES.cy < ES.rowoff) {
        ES.rowoff = ES.cy;
    }
    if (ES.cy >= ES.rowoff + ES.screenrows) {
        ES.rowoff = ES.cy - ES.screenrows + 1;
    }
    if (ES.rx < ES.coloff) {
        ES.coloff = ES.rx;
    }
    if (ES.rx >= ES.coloff + ES.screencols) {
        ES.coloff = ES.rx - ES.screencols + 1;
    }
}

void draw_rows(struct abuf* ab) {
    int y;
    for (y=0; y < ES.screenrows; y++) {
        int filerow = y + ES.rowoff;
        if (filerow >= ES.numrows) {
            if (ES.numrows == 0 && y == ES.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "welcome to inn -- version %s", INN_VERSION);
                if (welcomelen > ES.screencols) { welcomelen = ES.screencols; }

                int padding = (ES.screencols - welcomelen) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                }
                while (padding--) { ab_append(ab, " ", 1); }

                ab_append(ab, welcome, welcomelen);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = ES.row[filerow].rsize - ES.coloff;
            if (len < 0 ) len = 0;
            if (len > ES.screencols) len = ES.screencols;
            ab_append(ab, &ES.row[filerow].render[ES.coloff], len);
        }

        ab_append(ab, "\x1b[K", 3); // clear line right of cursor
        ab_append(ab, "\r\n", 2);
    }
}

void draw_statusbar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                                                ES.filename ? ES.filename : "[NO NAME]",
                                                ES.numrows,
                                                ES.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", ES.cy + 1, ES.numrows);
    if (len > ES.screencols) len = ES.screencols;
    ab_append(ab, status, len);
    while (len < ES.screencols) {
        if (ES.screencols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

void draw_messagebar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);
    ab_append(ab, "\x1b[K", 3);
    int msglen = strlen(ES.statusmsg);
    if (msglen > ES.screencols) msglen = ES.screencols;
    if (msglen && time(NULL) - ES.statusmsg_time < 5) ab_append(ab, ES.statusmsg, msglen);
    ab_append(ab, "\x1b[m", 3);
}

void refresh_screen() {
    editor_scroll();

    struct abuf ab = ABUF_INT;
    ab_append(&ab, "\x1b[?25l", 6); // hide cursor
    ab_append(&ab, "\x1b[H", 3); // reset cursor position
    draw_rows(&ab);
    draw_statusbar(&ab);
    draw_messagebar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ES.cy - ES.rowoff) + 1, (ES.rx - ES.coloff) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6); // show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

void editor_set_statusmessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ES.statusmsg, sizeof(ES.statusmsg), fmt, ap);
  va_end(ap);
  ES.statusmsg_time = time(NULL);
}

/* input */
void move_cursor(int key) {
    erow* row = (ES.cy >= ES.numrows) ? NULL : &ES.row[ES.cy];

    switch (key) {
        case ARROW_UP:
            if (ES.cy != 0) ES.cy--;
            break;
        case ARROW_LEFT:
            if (ES.cx != 0) ES.cx--;
            else if (ES.cy > 0) {
                ES.cy--;
                ES.cx = ES.row[ES.cy].size;
            }
            break;
        case ARROW_DOWN:
            if (ES.cy < ES.numrows) ES.cy++;
            break;
        case ARROW_RIGHT:
            if (row && ES.cx < row->size) ES.cx++;
            else if (row && ES.cx == row->size) {
                ES.cy++;
                ES.cx = 0;
            }
            break;
    }

    row = (ES.cy >= ES.numrows) ? NULL : &ES.row[ES.cy];
    int rowlen = row ? row->size : 0;
    if (ES.cx > rowlen) {
        ES.cx = rowlen;
    }
}

void process_keypress() {
    static int quit_times = QUIT_TIMES;

    int c = read_key();

    switch (c) {
        case '\r':
            editor_insert_newline();
            break;
        case CTRL_KEY('q'):
            if (ES.dirty && quit_times > 0) {
                editor_set_statusmessage("no write since last change - press CTRL-q %d more times to force quit.", quit_times);
                quit_times--;
                return;
            }
            clear_screen();
            exit(0);
            break;
        case CTRL_KEY('s'):
            editor_save();
            break;
        case HOME_KEY:
            ES.cx = 0;
            break;
        case END_KEY:
            if (ES.cy < ES.numrows) ES.cx = ES.row[ES.cy].size;
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) move_cursor(ARROW_RIGHT);
            editor_delete_char();
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    ES.cy = ES.rowoff;
                } else if (c == PAGE_DOWN) {
                    ES.cy = ES.rowoff + ES.screenrows - 1;
                    if (ES.cy > ES.numrows) ES.cy = ES.numrows;
                }

                int times = ES.screenrows;
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
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            editor_insert_char(c);
            break;
    }
    quit_times = QUIT_TIMES;
}

/* init */
void init_editor() {
    ES.cx = 0;
    ES.cy = 0;
    ES.rx = 0;
    ES.rowoff = 0;
    ES.coloff = 0;
    ES.numrows = 0;
    ES.row = NULL;
    ES.dirty = 0;
    ES.filename = NULL;
    ES.statusmsg[0] = '\0';
    ES.statusmsg_time = 0;

    if (get_window_size(&ES.screenrows, &ES.screencols) == -1) { die("get_window_size"); }
    ES.screenrows -= 2;
}

int main(int argc, char* argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_statusmessage("Ctrl-Q to quit");

    while (1) {
        refresh_screen();
        process_keypress();
    }


    return 0;
}