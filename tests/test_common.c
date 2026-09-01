/* test_common.c - pomocne funkcije nad putanjama */
#include "common.h"
#include <assert.h>

int main(void)
{
    assert(is_url("http://<ip-nas>:5000/a.cbr") == 1);
    assert(is_url("HTTP://VELIKA.SLOVA/a.cbr") == 1);
    assert(is_url("/mnt/usb0/a.cbr") == 0);
    assert(is_url("") == 0);
    assert(is_url("http:/") == 0);

    char b[64];

    url_decode(b, sizeof b, "Stripoteka%2041-50.cbr");
    assert(!strcmp(b, "Stripoteka 41-50.cbr"));

    /* iz stvarnog PROPFIND odgovora: zarez i zagrada u imenu */
    url_decode(b, sizeof b, "a%2Cb%29c");
    assert(!strcmp(b, "a,b)c"));

    url_decode(b, sizeof b, "bez-escape");
    assert(!strcmp(b, "bez-escape"));

    /* nepotpun escape se ne smije progutati */
    url_decode(b, sizeof b, "krnj%2");
    assert(!strcmp(b, "krnj%2"));

    /* ne smije prepisati preko bafera */
    char small[5];
    url_decode(small, sizeof small, "abcdefgh");
    assert(strlen(small) == 4);

    printf("test_common OK\n");
    return 0;
}
