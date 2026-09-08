#include "edit.h"

#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HISTORY_MAX 50

static char history[HISTORY_MAX][LINE_SIZE];
static int hist_count;
static int raw_ok;

void editor_init(void)
{
    struct termios t;
    raw_ok = tcgetattr(STDIN_FILENO, &t) == 0;
}

static void emit(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void history_add(const char *line)
{
    if (!*line)
        return;
    if (hist_count && strcmp(history[hist_count - 1], line) == 0)
        return;
    if (hist_count == HISTORY_MAX) {
        memmove(history[0], history[1], sizeof(history[0]) * (HISTORY_MAX - 1));
        hist_count--;
    }
    snprintf(history[hist_count++], LINE_SIZE, "%s", line);
}

static void redraw(const char *prompt, const char *line, int len, int pos)
{
    char buf[LINE_SIZE + 64];
    snprintf(buf, sizeof(buf), "\r\x1b[K%s", prompt);
    emit(buf);
    write(STDOUT_FILENO, line, (size_t)len);
    int back = len - pos;
    if (back > 0) {
        snprintf(buf, sizeof(buf), "\x1b[%dD", back);
        emit(buf);
    }
}

/* Le uma sequencia depois do ESC. Devolve a letra final ou 0. */
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
        if (c == '~')
            return code == '3' ? 'x' : 0;
        return c;
    }
    return 0;
}

int editor_readline(const char *prompt, char *line, int size)
{
    struct termios saved, raw;
    int have_raw = raw_ok && tcgetattr(STDIN_FILENO, &saved) == 0;
    if (have_raw) {
        raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
            have_raw = 0;
    }
    if (!have_raw) {
        emit(prompt);
        if (!fgets(line, size, stdin))
            return -1;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        history_add(line);
        return (int)strlen(line);
    }

    int len = 0, pos = 0, hist = hist_count;
    char saved_line[LINE_SIZE] = "";
    line[0] = '\0';
    emit(prompt);

    for (;;) {
        char c;
        long n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            return -1;
        }

        if (c == '\n' || c == '\r') {
            emit("\r\n");
            break;
        }
        if (c == 0x04) {
            if (len == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &saved);
                emit("\r\n");
                return -1;
            }
            continue;
        }
        if (c == 0x0C) {
            emit("\x1b[2J\x1b[H");
            redraw(prompt, line, len, pos);
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                memmove(line + pos - 1, line + pos, (size_t)(len - pos));
                len--;
                pos--;
                line[len] = '\0';
                redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == 0x1B) {
            char code = read_escape();
            if (code == 'D' && pos > 0) {
                pos--;
                emit("\x1b[1D");
            } else if (code == 'C' && pos < len) {
                pos++;
                emit("\x1b[1C");
            } else if (code == 'H') {
                pos = 0;
                redraw(prompt, line, len, pos);
            } else if (code == 'F') {
                pos = len;
                redraw(prompt, line, len, pos);
            } else if (code == 'x') {
                if (pos < len) {
                    memmove(line + pos, line + pos + 1, (size_t)(len - pos - 1));
                    len--;
                    line[len] = '\0';
                    redraw(prompt, line, len, pos);
                }
            } else if (code == 'A' || code == 'B') {
                if (hist == hist_count && code == 'A')
                    snprintf(saved_line, sizeof(saved_line), "%s", line);
                if (code == 'A' && hist > 0)
                    hist--;
                else if (code == 'B' && hist < hist_count)
                    hist++;
                const char *src = hist < hist_count ? history[hist] : saved_line;
                snprintf(line, (size_t)size, "%s", src);
                len = (int)strlen(line);
                pos = len;
                redraw(prompt, line, len, pos);
            }
            continue;
        }
        if ((unsigned char)c < 0x20 || len >= size - 1)
            continue;

        memmove(line + pos + 1, line + pos, (size_t)(len - pos));
        line[pos] = c;
        len++;
        pos++;
        line[len] = '\0';
        if (pos == len)
            write(STDOUT_FILENO, &c, 1);
        else
            redraw(prompt, line, len, pos);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    line[len] = '\0';
    history_add(line);
    return len;
}

int editor_history_count(void)
{
    return hist_count;
}

const char *editor_history_at(int i)
{
    return i >= 0 && i < hist_count ? history[i] : "";
}
