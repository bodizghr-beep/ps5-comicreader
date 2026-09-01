/* config.h - .ps5cr.conf sa USB-a */
#ifndef CONFIG_H
#define CONFIG_H

#include "source.h"

#define CFG_SRC_MAX 8

typedef struct {
    char name[SRC_NAME_MAX];
    char url[LIB_PATH_MAX];
    char type[16];              /* auto | webdav | autoindex */
    char user[64];
    char pass[64];
} cfg_source_t;

typedef struct {
    int          cache_mb;      /* gornja granica kesa; default 4096 */
    cfg_source_t srcs[CFG_SRC_MAX];
    int          n_srcs;
} config_t;

/* 0 = procitano, -1 = nema fajla (c je i tada popunjen defaultima). */
int config_load(config_t *c, const char *path);

/* Trazi .ps5cr.conf po /mnt/usb0..7. 0 ako je nadjen, -1 inace. */
int config_find(char *out, size_t len);

#endif /* CONFIG_H */
