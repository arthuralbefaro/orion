#pragma once

#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int auth_read_line(const char *prompt, char *buf, size_t size, int hide);
int auth_check(const char *user, const char *password);
int auth_become(const struct passwd *pw);
void auth_exec_shell(const struct passwd *pw, const char *command, int login);

int login_main(int argc, char **argv);
int su_main(int argc, char **argv);
int passwd_main(int argc, char **argv);
