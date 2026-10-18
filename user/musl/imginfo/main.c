#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#pragma clang diagnostic pop

static const char *format_of(const char *path)
{
    unsigned char m[4] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f)
        return "?";
    size_t n = fread(m, 1, sizeof(m), f);
    fclose(f);
    if (n >= 4 && m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G')
        return "PNG";
    if (n >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF)
        return "JPEG";
    return "?";
}

static const char *color_name(const unsigned char *p)
{
    int r = p[0], g = p[1], b = p[2];
    if (r > 150 && g > 150 && b < 100)
        return "amarelo";
    if (r > g + 60 && r > b + 60)
        return "vermelho";
    if (g > r + 60 && g > b + 40)
        return "verde";
    if (b > r + 60 && b > g + 40)
        return "azul";
    return "outro";
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: imginfo <imagem>...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int w, h, n;
        unsigned char *px = stbi_load(argv[i], &w, &h, &n, 3);
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        if (!px) {
            printf("%s: nao decodificou (%s)\n", name, stbi_failure_reason());
            bad++;
            continue;
        }
        unsigned long long sum[3] = { 0, 0, 0 };
        for (long k = 0; k < (long)w * h; k++)
            for (int c = 0; c < 3; c++)
                sum[c] += px[k * 3 + c];
        unsigned long long count = (unsigned long long)w * (unsigned long long)h;
        printf("%s: %s %dx%d, %d canais, media %02llx%02llx%02llx\n", name, format_of(argv[i]), w, h, n,
               sum[0] / count, sum[1] / count, sum[2] / count);
        int qx[2] = { w / 4, 3 * w / 4 }, qy[2] = { h / 4, 3 * h / 4 };
        printf("%s: quadrantes %s %s %s %s\n", name, color_name(px + (qy[0] * w + qx[0]) * 3),
               color_name(px + (qy[0] * w + qx[1]) * 3), color_name(px + (qy[1] * w + qx[0]) * 3),
               color_name(px + (qy[1] * w + qx[1]) * 3));
        stbi_image_free(px);
    }
    return bad ? 1 : 0;
}
