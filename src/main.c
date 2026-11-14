// SPDX-License-Identifier: AGPL-3.0-or-later
/* Mp3 to XWM: converts the MP3 files in ToConvert into .xwm files in Output. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#include "platform.h"

static constexpr char IN_DIR[]  = "ToConvert";
static constexpr char OUT_DIR[] = "Output";

typedef struct {
    char **names;
    int    count, cap;
    bool   out_of_memory;
} name_list;

static bool is_mp3(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && tolower((unsigned char)name[n - 3]) == 'm'
        && tolower((unsigned char)name[n - 2]) == 'p' && name[n - 1] == '3';
}

static void collect_mp3(const char *name, void *ctx)
{
    name_list *list = ctx;
    if (!is_mp3(name) || list->out_of_memory)
        return;
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 16;
        char **grown = realloc(list->names, (size_t)cap * sizeof *grown);
        if (!grown) {
            list->out_of_memory = true;
            return;
        }
        list->names = grown;
        list->cap = cap;
    }
    size_t len = strlen(name) + 1;
    char *copy = malloc(len);
    if (!copy) {
        list->out_of_memory = true;
        return;
    }
    memcpy(copy, name, len);
    list->names[list->count++] = copy;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

/* True when two .mp3 names differ only in the case of the extension, so give the same .xwm name. */
static bool same_output(const char *a, const char *b)
{
    size_t n = strlen(a);
    return n == strlen(b) && !strncmp(a, b, n - 4);
}

typedef struct {
    const char *dir;
    int         kept;
} output_scan;

/* Deletes temporary files an interrupted run left in the output folder; counts the rest. */
static void tidy_output(const char *name, void *ctx)
{
    output_scan *scan = ctx;
    if (ends_with(name, ".xwm.part") || ends_with(name, ".xwm.data.part")) {
        char path[PATH_MAX_LEN];
        if (plat_join(path, sizeof path, scan->dir, name))
            plat_remove(path);
        return;
    }
    scan->kept++;
}

/* Returns 1 if the folder was created, 0 if it already existed, -1 on failure. */
static int ensure_dir(const char *path, const char *label)
{
    if (plat_dir_exists(path))
        return 0;
    if (plat_make_dir(path) != 0) {
        printf("Could not create the %s folder: %s\n", label, path);
        return -1;
    }
    printf("Created the %s folder: %s\n", label, path);
    return 1;
}

/* Shows the quality choice and waits for S or Q. Returns the chosen rate, or 0 to quit. */
static int ask_start(void)
{
    int choice = 0;
    printf("\nQuality for stereo files (mono files always use 48 kbit/s):\n");
    printf("  1 = 48 kbit/s, the game's own music (default)\n");
    printf("  2 = 96 kbit/s\n");
    printf("  3 = 192 kbit/s\n");
    for (;;) {
        printf("Quality: %d kbit/s. Press 1, 2 or 3 to change it, S to start or Q to quit.\n",
               CONVERT_RATES[choice]);
        fflush(stdout);
        int c;
        do
            c = plat_read_key();
        while (c >= 0 && (c == 0 || !strchr("123sSqQ", c)));
        if (c < 0 || c == 'q' || c == 'Q')
            return 0;
        if (c == 's' || c == 'S')
            return CONVERT_RATES[choice];
        choice = c - '1';
    }
}

static int convert_all(const char *in_dir, const char *out_dir, int kbps)
{
    name_list list = {0};
    int rc = 1;
    if (plat_list_files(in_dir, collect_mp3, &list) < 0) {
        printf("Could not read the %s folder: %s\n", IN_DIR, in_dir);
        goto done;
    }
    if (list.out_of_memory) {
        printf("Out of memory while listing the %s folder.\n", IN_DIR);
        goto done;
    }
    if (list.count == 0) {
        printf("No .mp3 files found in the %s folder.\n", IN_DIR);
        goto done;
    }
    qsort(list.names, (size_t)list.count, sizeof *list.names, compare_names);

    int ok = 0;
    for (int i = 0; i < list.count; i++) {
        const char *name = list.names[i];
        /* a.mp3 and a.MP3 would both write a.xwm: convert the first only */
        int clash = -1;
        for (int j = 0; j < i && clash < 0; j++)
            if (same_output(list.names[j], name))
                clash = j;
        if (clash >= 0) {
            printf("[%d/%d] %s skipped: %s already writes the same .xwm name\n", i + 1, list.count, name,
                   list.names[clash]);
            continue;
        }
        char in_path[PATH_MAX_LEN], out_path[PATH_MAX_LEN], out_name[PATH_MAX_LEN];
        char err[256] = "";
        int used = 0;
        snprintf(out_name, sizeof out_name, "%.*s.xwm", (int)(strlen(name) - 4), name);
        printf("[%d/%d] %s -> %s ... ", i + 1, list.count, name, out_name);
        fflush(stdout);
        if (!plat_join(in_path, sizeof in_path, in_dir, name) || !plat_join(out_path, sizeof out_path, out_dir, out_name)) {
            printf("failed: path too long\n");
            continue;
        }
        if (convert_mp3_to_xwm(in_path, out_path, kbps, &used, err, sizeof err) == 0) {
            printf("done (%d kbit/s)\n", used);
            ok++;
        } else {
            printf("failed: %s\n", err);
        }
    }
    printf("\nConverted %d of %d file(s). They are in the %s folder.\n", ok, list.count, OUT_DIR);
    rc = ok == list.count ? 0 : 1;

done:
    for (int i = 0; i < list.count; i++)
        free(list.names[i]);
    free(list.names);
    return rc;
}

/* Keeps the window open when the program was started from Explorer or a desktop. */
static void pause_before_exit(void)
{
    if (!plat_interactive())
        return;
    printf("\nPress any key to close.\n");
    fflush(stdout);
    plat_read_key();
}

static int run(void)
{
    char base[PATH_MAX_LEN], in_dir[PATH_MAX_LEN], out_dir[PATH_MAX_LEN];
    if (plat_exe_dir(base, sizeof base) != 0) {
        printf("Could not find the program's folder.\n");
        return 1;
    }
    if (!plat_join(in_dir, sizeof in_dir, base, IN_DIR) || !plat_join(out_dir, sizeof out_dir, base, OUT_DIR)) {
        printf("The program's folder path is too long: %s\n", base);
        return 1;
    }

    printf("Mp3 to XWM - converts MP3 files to Space Engineers .xwm audio\n\n");

    int made_in = ensure_dir(in_dir, IN_DIR);
    int made_out = ensure_dir(out_dir, OUT_DIR);
    if (made_in < 0 || made_out < 0)
        return 1;

    output_scan scan = {out_dir, 0};
    plat_list_files(out_dir, tidy_output, &scan);
    if (made_in || made_out) {
        printf("\nPlace the .mp3 files to convert into the %s folder.\n", IN_DIR);
        printf("All converted files will be in the %s folder.\n", OUT_DIR);
    } else {
        if (scan.kept > 0)
            printf("The %s folder already holds %d file(s). Files with the same name will be overwritten.\n",
                   OUT_DIR, scan.kept);
        printf("MP3 files in the %s folder will be converted into the %s folder.\n", IN_DIR, OUT_DIR);
    }

    int kbps = ask_start();
    if (!kbps) {
        printf("Quit.\n");
        return 0;
    }
    return convert_all(in_dir, out_dir, kbps);
}

int main(void)
{
    plat_console_init();
    int rc = run();
    pause_before_exit();
    return rc;
}
