#pragma once

#define LINE_SIZE 512

void editor_init(void);
int  editor_readline(const char *prompt, char *line, int size);
int  editor_history_count(void);
const char *editor_history_at(int i);
