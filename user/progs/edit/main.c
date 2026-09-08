#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define MAX_LINES 512
#define LINE_MAX  256

static char lines[MAX_LINES][LINE_MAX];
static int count = 1;
static int cur_row, cur_col, top_row;
static int rows = 24, cols = 80;
static int dirty;
static int full_draw = 1;
static char path[256];
static char status[128];
static struct termios saved;

static void emit(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void load(void)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(status, sizeof(status), "novo arquivo");
        return;
    }
    count = 0;
    char buf[LINE_MAX];
    while (count < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
        snprintf(lines[count++], LINE_MAX, "%s", buf);
    }
    fclose(f);
    if (count == 0)
        count = 1;
    snprintf(status, sizeof(status), "%d linha(s) lidas", count);
    full_draw = 1;
}

static int save(void)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(status, sizeof(status), "erro ao salvar: %s", strerror(errno));
        return -1;
    }
    for (int i = 0; i < count; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
    sync();
    dirty = 0;
    snprintf(status, sizeof(status), "%s salvo (%d linha(s))", path, count);
    return 0;
}

static void draw(void)
{
    char out[LINE_MAX + 64];
    int body = rows - 2;

    if (full_draw) {
        emit("\x1b[H");
        for (int i = 0; i < body; i++) {
            int idx = top_row + i;
            snprintf(out, sizeof(out), "\x1b[K%s\r\n", idx < count ? lines[idx] : "~");
            emit(out);
        }
        full_draw = 0;
    } else {
        snprintf(out, sizeof(out), "\x1b[%d;1H\x1b[K%s", cur_row - top_row + 1, lines[cur_row]);
        emit(out);
    }

    snprintf(out, sizeof(out), "\x1b[%d;1H\x1b[K\x1b[7m %-*s\x1b[0m", body + 1, cols - 2,
             dirty ? "Ctrl+S salva  Ctrl+X sai  [modificado]" : "Ctrl+S salva  Ctrl+X sai");
    emit(out);
    snprintf(out, sizeof(out), "\x1b[%d;1H\x1b[K%s  linha %d/%d col %d  %s", body + 2, path,
             cur_row + 1, count, cur_col + 1, status);
    emit(out);
    snprintf(out, sizeof(out), "\x1b[%d;%dH", cur_row - top_row + 1, cur_col + 1);
    emit(out);
}

static void scroll_fix(void)
{
    int body = rows - 2;
    int before = top_row;
    if (cur_row < top_row)
        top_row = cur_row;
    if (cur_row >= top_row + body)
        top_row = cur_row - body + 1;
    if (top_row < 0)
        top_row = 0;
    if (top_row != before)
        full_draw = 1;
}

static void insert_char(char c)
{
    char *l = lines[cur_row];
    int len = (int)strlen(l);
    if (len >= LINE_MAX - 1)
        return;
    memmove(l + cur_col + 1, l + cur_col, (size_t)(len - cur_col + 1));
    l[cur_col++] = c;
    dirty = 1;
}

static void split_line(void)
{
    if (count >= MAX_LINES)
        return;
    memmove(lines[cur_row + 2], lines[cur_row + 1],
            sizeof(lines[0]) * (size_t)(count - cur_row - 1));
    snprintf(lines[cur_row + 1], LINE_MAX, "%s", lines[cur_row] + cur_col);
    lines[cur_row][cur_col] = '\0';
    count++;
    cur_row++;
    cur_col = 0;
    dirty = 1;
    full_draw = 1;
}

static void join_prev(void)
{
    if (cur_row == 0)
        return;
    int prev = (int)strlen(lines[cur_row - 1]);
    if (prev + (int)strlen(lines[cur_row]) < LINE_MAX - 1)
        strcat(lines[cur_row - 1], lines[cur_row]);
    memmove(lines[cur_row], lines[cur_row + 1],
            sizeof(lines[0]) * (size_t)(count - cur_row - 1));
    count--;
    cur_row--;
    cur_col = prev;
    dirty = 1;
    full_draw = 1;
}

static void backspace(void)
{
    if (cur_col > 0) {
        char *l = lines[cur_row];
        int len = (int)strlen(l);
        memmove(l + cur_col - 1, l + cur_col, (size_t)(len - cur_col + 1));
        cur_col--;
        dirty = 1;
        return;
    }
    join_prev();
}

static void delete_char(void)
{
    char *l = lines[cur_row];
    int len = (int)strlen(l);
    if (cur_col < len) {
        memmove(l + cur_col, l + cur_col + 1, (size_t)(len - cur_col));
        dirty = 1;
    } else if (cur_row + 1 < count) {
        cur_row++;
        cur_col = 0;
        join_prev();
    }
}

static char read_escape(void)
{
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1 || c != '[')
        return 0;
    char code = 0;
    for (int i = 0; i < 8; i++) {
        if (read(STDIN_FILENO, &c, 1) != 1)
            return 0;
        if (c >= '0' && c <= '9') {
            code = c;
            continue;
        }
        if (c == '~') {
            if (code == '3') return 'x';
            if (code == '5') return 'P';
            if (code == '6') return 'N';
            return 0;
        }
        return c;
    }
    return 0;
}

static void clamp(void)
{
    if (cur_row < 0) cur_row = 0;
    if (cur_row >= count) cur_row = count - 1;
    int len = (int)strlen(lines[cur_row]);
    if (cur_col > len) cur_col = len;
    if (cur_col < 0) cur_col = 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: edit <arquivo>\n");
        return 1;
    }
    snprintf(path, sizeof(path), "%s", argv[1]);
    lines[0][0] = '\0';
    load();

    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }

    if (tcgetattr(STDIN_FILENO, &saved) < 0) {
        fprintf(stderr, "edit: precisa de um terminal\n");
        return 1;
    }
    struct termios raw = saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    emit("\x1b[2J");

    for (;;) {
        scroll_fix();
        draw();

        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            break;

        if (c == 0x18) {
            if (dirty) {
                snprintf(status, sizeof(status), "modificado: Ctrl+S salva, Ctrl+X de novo sai");
                dirty = 2;
                scroll_fix();
                draw();
                if (read(STDIN_FILENO, &c, 1) != 1 || c != 0x18)
                    continue;
            }
            break;
        }
        if (c == 0x13) {
            save();
            continue;
        }
        if (c == '\r' || c == '\n') {
            split_line();
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            backspace();
            continue;
        }
        if (c == 0x1B) {
            char code = read_escape();
            switch (code) {
            case 'A': cur_row--; break;
            case 'B': cur_row++; break;
            case 'C': cur_col++; break;
            case 'D': cur_col--; break;
            case 'H': cur_col = 0; break;
            case 'F': cur_col = (int)strlen(lines[cur_row]); break;
            case 'P': cur_row -= rows - 2; break;
            case 'N': cur_row += rows - 2; break;
            case 'x': delete_char(); break;
            default: break;
            }
            clamp();
            continue;
        }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F)
            insert_char(c);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    emit("\x1b[2J\x1b[H");
    printf("edit: %s\n", dirty ? "saiu sem salvar" : "pronto");
    return 0;
}
