/* test_config.c - parser konfiguracije */
#include "config.h"
#include "common.h"
#include <assert.h>
#include <unistd.h>

static const char *SAMPLE =
    "# komentar\n"
    "cache_mb = 2048\n"
    "\n"
    "[source]\n"
    "name = qnap\n"
    "url  = http://<ip-nas>:5000/STRIPOVI/\n"
    "user = PS5\n"
    "pass = tajna\n"
    "\n"
    "[source]\n"
    "name = drugi\n"
    "url = http://1.2.3.4:8080/x/\n"
    "type = autoindex\n"
    "nepoznat_kljuc = svejedno\n";

int main(void)
{
    char tmpl[] = "/tmp/cr_cfgXXXXXX";
    int  fd = mkstemp(tmpl);
    assert(fd >= 0);
    assert(write(fd, SAMPLE, strlen(SAMPLE)) == (ssize_t)strlen(SAMPLE));
    close(fd);

    config_t c;
    assert(config_load(&c, tmpl) == 0);

    assert(c.cache_mb == 2048);
    assert(c.n_srcs == 2);

    assert(!strcmp(c.srcs[0].name, "qnap"));
    assert(!strcmp(c.srcs[0].url,  "http://<ip-nas>:5000/STRIPOVI/"));
    assert(!strcmp(c.srcs[0].user, "PS5"));
    assert(!strcmp(c.srcs[0].pass, "tajna"));
    assert(!strcmp(c.srcs[0].type, "auto"));       /* default */

    assert(!strcmp(c.srcs[1].name, "drugi"));
    assert(!strcmp(c.srcs[1].type, "autoindex"));
    assert(c.srcs[1].user[0] == '\0');             /* nema auth */

    unlink(tmpl);

    /* Nepostojeci fajl nije fatalno - app radi samo s USB-om. */
    config_t c2;
    assert(config_load(&c2, "/nema/ovoga") == -1);
    assert(c2.n_srcs == 0);
    assert(c2.cache_mb == 4096);                   /* default ostaje */

    printf("test_config OK\n");
    return 0;
}
