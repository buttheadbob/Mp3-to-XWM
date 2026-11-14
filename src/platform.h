// SPDX-License-Identifier: AGPL-3.0-or-later
/* Small OS layer: folders, files and keyboard input. Paths are UTF-8 on every system. */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#define PATH_MAX_LEN 4096

/* Console setup: UTF-8 output where needed, restored when the program ends. */
void  plat_console_init(void);
/* True when input comes from a person at a console, not a pipe, file or NUL. */
bool  plat_interactive(void);
/* Directory that holds the running executable, without a trailing separator. */
int   plat_exe_dir(char *out, size_t size);
bool  plat_dir_exists(const char *path);
int   plat_make_dir(const char *path);
FILE *plat_fopen(const char *path, const char *mode);
int   plat_remove(const char *path);
/* Renames from to to, replacing to if it exists. */
int   plat_rename(const char *from, const char *to);

/* Calls fn for every regular file in dir. Returns the number of files seen, or -1. */
typedef void (*plat_file_fn)(const char *name, void *ctx);
int   plat_list_files(const char *dir, plat_file_fn fn, void *ctx);

/* Waits for one key press and returns it. Returns -1 at end of input. */
int   plat_read_key(void);

/* Joins dir and name. Returns false if the result does not fit in size. */
bool  plat_join(char *out, size_t size, const char *dir, const char *name);

#endif
