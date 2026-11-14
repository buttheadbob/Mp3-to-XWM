// SPDX-License-Identifier: AGPL-3.0-or-later
/* Mp3 to XWM: converts the MP3 files in ToConvert into .xwm files in Output. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#include "platform.h"
#include "version.h"

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

/* Shows the format choice and waits for S or Q. Returns false to quit. */
static bool ask_start(convert_options *opt)
{
    opt->format = FORMAT_XWM;
    printf("\nOutput format:\n");
    printf("  X = XWM (default): small files at 48 kbit/s, like the game's own music. Best for music.\n");
    printf("      A sound set to loop has a short gap (about 0.08 s) at its end, because the format\n");
    printf("      always decodes whole frames.\n");
    printf("  A = MS ADPCM: loops without a gap, best for looping sounds and effects. Files are about\n");
    printf("      7-8 times larger than XWM. The game uses it for its own loops. Sounds longer than\n");
    printf("      %d minutes are converted but not fitted as loops.\n", CONVERT_LOOP_SECONDS / 60);
    for (;;) {
        if (opt->format == FORMAT_XWM)
            printf("Format: XWM. ");
        else
            printf("Format: MS ADPCM. ");
        printf("Press X or A for the format, S to start or Q to quit.\n");
        fflush(stdout);
        int c;
        do
            c = plat_read_key();
        while (c >= 0 && (c == 0 || !strchr("xXaAsSqQ", c)));
        if (c < 0 || c == 'q' || c == 'Q')
            return false;
        if (c == 's' || c == 'S')
            return true;
        opt->format = c == 'a' || c == 'A' ? FORMAT_ADPCM : FORMAT_XWM;
    }
}

static int convert_all(const char *in_dir, const char *out_dir, const convert_options *opt)
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
        snprintf(out_name, sizeof out_name, "%.*s.xwm", (int)(strlen(name) - 4), name);
        printf("[%d/%d] %s -> %s ... ", i + 1, list.count, name, out_name);
        fflush(stdout);
        if (!plat_join(in_path, sizeof in_path, in_dir, name) || !plat_join(out_path, sizeof out_path, out_dir, out_name)) {
            printf("failed: path too long\n");
            continue;
        }
        convert_result res;
        if (convert_mp3(in_path, out_path, opt, &res, err, sizeof err) == 0) {
            if (opt->format == FORMAT_XWM)
                printf("done (XWM %d kbit/s)\n", CONVERT_XWM_KBPS);
            else if (res.loop_streamed)
                printf("done (MS ADPCM; longer than %d minutes, so it was not fitted as a loop: %ld samples of"
                       " silence end it and it will not loop seamlessly)\n", CONVERT_LOOP_SECONDS / 60,
                       res.loop_change);
            else if (res.loop_padded)
                printf("done (MS ADPCM; too short to fit, so %ld samples of silence end it and it will not loop"
                       " seamlessly)\n", res.loop_change);
            else
                printf("done (MS ADPCM, loops without a gap; length fitted by %+ld"
                       " samples, played at %d Hz so pitch and speed are unchanged)\n",
                       res.loop_change, res.loop_rate);
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

    printf("Mp3 to XWM " MP3TOXWM_VERSION " - converts MP3 files to Space Engineers .xwm audio\n");
    printf("Source: " MP3TOXWM_SOURCE "\n\n");

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

    convert_options opt;
    if (!ask_start(&opt)) {
        printf("Quit.\n");
        return 0;
    }
    return convert_all(in_dir, out_dir, &opt);
}

int main(void)
{
    plat_console_init();
    int rc = run();
    pause_before_exit();
    return rc;
}
