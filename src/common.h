/* common.h - zajednicke definicije */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#define APP_NAME    "PS5 Comic Reader"
#define APP_VERSION "0.1"

/* Payload nema stdout na ekranu; elfldr preusmerava stderr na svoj socket,
 * pa je fprintf(stderr) najpouzdaniji kanal za debug tokom razvoja. */
#define LOG(fmt, ...)  fprintf(stderr, "[cr] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "[cr][err] " fmt "\n", ##__VA_ARGS__)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Poredjenje imena fajlova tako da "page2.jpg" dodje pre "page10.jpg".
 * Bez ovoga se stranice stripa citaju u pogresnom redosledu. */
int natural_cmp(const char *a, const char *b);

/* Vraca pokazivac na ekstenziju (bez tacke) ili "" ako je nema. */
const char *path_ext(const char *path);

/* Vraca pokazivac na ime fajla unutar putanje. */
const char *path_base(const char *path);

#endif /* COMMON_H */
