#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define MAX_ITEMS 4096
#define LINE_BUFSIZE 512

static void usage(void) {
    fprintf(stderr,
        "usage: idkfs_snapper --image <img> [--store <dir>] <cmd> [desc]\n"
        "commands:\n"
        "  create [desc]   create a snapshot copy of the image\n"
        "  list            show existing snapshots\n"
        "  delete <id>     remove a snapshot\n"
        "  rollback <id>   replace the image with a snapshot\n");
}

static const char *default_store(const char *image) {
    static char buf[4096];
    int n = snprintf(buf, sizeof(buf), "%s.snapshots", image);
    if (n < 0 || n >= (int)sizeof(buf))
        return NULL;
    return buf;
}

static bool ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path, 0755) == 0)
        return true;
    return errno == EEXIST;
}

static int read_next_id(const char *store) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/next_id", store);
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;
    int id = 1;
    if (fscanf(f, "%d", &id) != 1)
        id = 1;
    fclose(f);
    return id;
}

static void write_next_id(const char *store, int id) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/next_id", store);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", id);
    fclose(f);
}

typedef struct {
    int id;
    char timestamp[32];
    char desc[256];
} SnapshotMeta;

static size_t load_metadata(const char *store, SnapshotMeta **out) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/list.txt", store);
    FILE *f = fopen(path, "r");
    if (!f) {
        *out = NULL;
        return 0;
    }
    SnapshotMeta *items = calloc(MAX_ITEMS, sizeof(SnapshotMeta));
    if (!items) { fclose(f); return 0; }
    size_t cnt = 0;
    char line[LINE_BUFSIZE];
    while (fgets(line, sizeof(line), f)) {
        if (cnt >= MAX_ITEMS)
            break;
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        char *sep = strchr(line, '|');
        if (!sep) continue;
        char *sep2 = strchr(sep + 1, '|');
        if (!sep2) continue;
        *sep = '\0';
        *sep2 = '\0';
        int id = atoi(line);
        strncpy(items[cnt].timestamp, sep + 1, sizeof(items[cnt].timestamp) - 1);
        strncpy(items[cnt].desc, sep2 + 1, sizeof(items[cnt].desc) - 1);
        items[cnt].id = id;
        cnt++;
    }
    fclose(f);
    *out = items;
    return cnt;
}

static void save_metadata(const char *store, SnapshotMeta *items, size_t count) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/list.txt", store);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "%d|%s|%s\n", items[i].id, items[i].timestamp, items[i].desc);
    }
    fclose(f);
}

static bool copy_file(const char *src, const char *dst) {
    int infd = open(src, O_RDONLY);
    if (infd < 0) return false;
    int outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) { close(infd); return false; }
    char buf[65536];
    ssize_t n;
    while ((n = read(infd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(outfd, buf + written, n - written);
            if (w < 0) { close(infd); close(outfd); return false; }
            written += w;
        }
    }
    close(infd);
    close(outfd);
    return n == 0;
}

static const char *current_timestamp(char *out, size_t len) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm);
    return out;
}

int main(int argc, char *argv[]) {
    if (argc < 3) { usage(); return 1; }
    const char *image = NULL;
    const char *store = NULL;
    const char *cmd = NULL;
    const char *desc = NULL;
    int id = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image = argv[++i];
        } else if (strcmp(argv[i], "--store") == 0 && i + 1 < argc) {
            store = argv[++i];
        } else if (!cmd) {
            cmd = argv[i];
            if (strcmp(cmd, "create") == 0 && i + 1 < argc)
                desc = argv[++i];
            if ((strcmp(cmd, "delete") == 0 || strcmp(cmd, "rollback") == 0) && i + 1 < argc)
                id = atoi(argv[++i]);
        }
    }

    if (!image || !cmd) {
        usage();
        return 1;
    }

    char store_buf[4096];
    if (!store) store = default_store(image);
    if (!store) {
        fprintf(stderr, "idkfs_snapper: image path too long\n");
        return 1;
    }

    snprintf(store_buf, sizeof(store_buf), "%s", store);
    if (!ensure_dir(store_buf)) {
        fprintf(stderr, "idkfs_snapper: could not create store '%s'\n", store_buf);
        return 1;
    }

    if (strcmp(cmd, "create") == 0) {
        int next_id = read_next_id(store_buf);
        char snapfile[4096];
        snprintf(snapfile, sizeof(snapfile), "%s/%04d.img", store_buf, next_id);
        if (!copy_file(image, snapfile)) {
            fprintf(stderr, "idkfs_snapper: failed to copy image\n");
            return 1;
        }
        SnapshotMeta *items = NULL;
        size_t count = load_metadata(store_buf, &items);
        SnapshotMeta meta = {.id = next_id};
        current_timestamp(meta.timestamp, sizeof(meta.timestamp));
        strncpy(meta.desc, desc ? desc : "", sizeof(meta.desc) - 1);
        size_t new_count = count + 1;
        SnapshotMeta *new_items = realloc(items, new_count * sizeof(SnapshotMeta));
        if (!new_items) {
            free(items);
            fprintf(stderr, "idkfs_snapper: out of memory\n");
            return 1;
        }
        new_items[count] = meta;
        save_metadata(store_buf, new_items, new_count);
        write_next_id(store_buf, next_id + 1);
        printf("created snapshot %04d (%s)\n", next_id, meta.desc);
        free(new_items);
        return 0;
    }

    SnapshotMeta *items = NULL;
    size_t count = load_metadata(store_buf, &items);
    if (strcmp(cmd, "list") == 0) {
        if (count == 0) {
            printf("no snapshots\n");
        } else {
            for (size_t i = 0; i < count; i++)
                printf("%04d  %s  %s\n", items[i].id, items[i].timestamp, items[i].desc);
        }
        free(items);
        return 0;
    }

    if (id < 0) {
        fprintf(stderr, "idkfs_snapper: %s requires an id\n", cmd);
        free(items);
        return 1;
    }

    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (items[i].id == id) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX) {
        fprintf(stderr, "idkfs_snapper: snapshot %04d not found\n", id);
        free(items);
        return 1;
    }

    char snapfile[4096];
    snprintf(snapfile, sizeof(snapfile), "%s/%04d.img", store_buf, id);

    if (strcmp(cmd, "delete") == 0) {
        if (unlink(snapfile) && errno != ENOENT) {
            perror("unlink");
            free(items);
            return 1;
        }
        memmove(&items[idx], &items[idx + 1], (count - idx - 1) * sizeof(SnapshotMeta));
        save_metadata(store_buf, items, count - 1);
        printf("deleted snapshot %04d\n", id);
        free(items);
        return 0;
    }

    if (strcmp(cmd, "rollback") == 0) {
        if (!copy_file(snapfile, image)) {
            fprintf(stderr, "idkfs_snapper: failed to restore image\n");
            free(items);
            return 1;
        }
        printf("rolled back image to snapshot %04d\n", id);
        free(items);
        return 0;
    }

    fprintf(stderr, "idkfs_snapper: unknown command '%s'\n", cmd);
    free(items);
    return 1;
}
