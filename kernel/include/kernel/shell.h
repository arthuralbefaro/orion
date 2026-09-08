/*
 * shell.h - mini shell interativo do kernel (debug/diagnóstico).
 */
#pragma once

void shell_init(void);

/* Entrega um caractere digitado (teclado ou serial) ao shell. */
void shell_feed(char c);
