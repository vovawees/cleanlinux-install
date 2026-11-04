/* SPDX-License-Identifier: MIT */
/* cleanlinux-install: shared helpers */
#ifndef CLI_UTIL_H
#define CLI_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

extern const char *prog; /* command name for messages */

void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* ":: text" on stdout */
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* "   text" on stdout */
void warnx(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warn_errno(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void die_errno(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* a growable vector of owned strings, NULL-terminated */
typedef struct {
	char **v;
	size_t n, cap;
} strv;
#define STRV_INIT {NULL, 0, 0}
void strv_push(strv *s, const char *str); /* copies */
void strv_take(strv *s, char *str);       /* takes ownership */
void strv_split(strv *s, const char *str); /* words separated by blanks */
bool strv_contains(const strv *s, const char *str);
void strv_free(strv *s);
char **strv_argv(strv *s);

/* a growable string */
typedef struct {
	char *s;
	size_t n, cap;
} sbuf;
#define SBUF_INIT {NULL, 0, 0}
void sb_add(sbuf *b, const char *s);
void sb_addn(sbuf *b, const char *s, size_t n);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sb_take(sbuf *b); /* the string (never NULL); the buffer is empty again */

char *read_file(const char *path, size_t *len); /* NUL-terminated; NULL + errno on error */
char *read_line(const char *path);              /* first line without the newline, trimmed */
int write_file(const char *path, const void *data, size_t len, mode_t mode); /* atomic replace */
int mkdir_p(const char *path, mode_t mode);
bool is_dir(const char *path);
bool is_file(const char *path);
bool exists(const char *path);
bool is_mountpoint(const char *path);
char *trim(char *s);
bool str_prefix(const char *s, const char *prefix);
bool str_suffix(const char *s, const char *suffix);
void random_bytes(void *buf, size_t n);

/* run a program (found in PATH) and wait: its exit status, 128+signal, or -1 when it could not
 * run; INPUT, when not NULL, is written to its standard input */
int run(char *const argv[], const char *input);
int runl(const char *input, const char *prog_name, ...);
/* the standard output of a program (stdin and stderr: /dev/null); NULL unless it exits with 0 */
char *run_output(char *const argv[]); /* NULL-terminated arguments */
/* the program in PATH, or NULL */
char *find_program(const char *name);
/* "a b 'c d'": a command line for messages */
char *argv_text(char *const argv[]);

#endif
