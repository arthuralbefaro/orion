#include "auth.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *name = argc > 0 ? argv[0] : "login";
    const char *slash = strrchr(name, '/');
    if (slash)
        name = slash + 1;
    if (strcmp(name, "su") == 0)
        return su_main(argc, argv);
    if (strcmp(name, "passwd") == 0)
        return passwd_main(argc, argv);
    return login_main(argc, argv);
}
