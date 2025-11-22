#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <copyfile.h>
#include <removefile.h>

// Define asprintf for clean string allocation
int asprintf(char **strp, const char *fmt, ...);

char *src_path = NULL;
char *dst_path = NULL;
int verbose_f = 0;
int keep_f = 0;

#define green_str(str) "\x1b[32m" str "\x1b[0m"
#define red_str(str)   "\x1b[31m" str "\x1b[0m"
#define yellow_str(str) "\x1b[33m" str "\x1b[0m"

/* entry remove function */
int remove_entry(const char *target) {
    if (access(target, F_OK) != 0) return 0;

    removefile_state_t state = removefile_state_alloc();
    int ret = removefile(target, state, REMOVEFILE_RECURSIVE);

    if (ret < 0) {
        perror("Error: Remove action failed");
        removefile_state_free(state);
        return 1;
    }

    if (verbose_f) fprintf(stdout, "%s %s\n", red_str("[-]"), target);
    removefile_state_free(state);
    return 0;
}

/* entry copy function */
int copy_entry(const char *src, const char *dst) {
    copyfile_state_t state = copyfile_state_alloc();

    // COPYFILE_ALL preserves metadata
    // COPYFILE_NOFOLLOW ensures we don't resolve symlinks (we copy the link itself)
    int ret = copyfile(src, dst, state, COPYFILE_ALL | COPYFILE_NOFOLLOW);

    if (ret < 0) {
        if (access(src, F_OK) != 0) {
            copyfile_state_free(state);
            return 0;
        }
        perror("Error: Copy action failed");
        copyfile_state_free(state);
        return 1;
    }

    if (verbose_f) fprintf(stdout, "%s %s\n", green_str("[+]"), dst);
    copyfile_state_free(state);
    return 0;
}

/* callback function */
void callback_fn(
        ConstFSEventStreamRef streamRef,
        void *clientCallBackInfo,
        size_t numEvents,
        void *eventPaths,
        const FSEventStreamEventFlags eventFlags[],
        const FSEventStreamEventId eventIds[])
{
    char **entries = (char **)eventPaths;

    for(size_t i = 0; i < numEvents; i++){
        char *src_full = entries[i];

        if (strlen(src_full) < strlen(src_path)) continue;

        char *rel_path = src_full + strlen(src_path);

        if (strstr(rel_path, ".DS_Store")) continue;

        char *dst_full;
        if (asprintf(&dst_full, "%s%s", dst_path, rel_path) == -1) {
            perror("OOM");
            continue;
        }

        struct stat sb;
        if (lstat(src_full, &sb) == 0) {
            // Source exists: Copy/Update
            copy_entry(src_full, dst_full);
        } else {
            // Source does not exist
            if (keep_f) {
                // Keep flag is ON: Do not delete
                if (verbose_f) fprintf(stdout, "%s %s (Source deleted)\n", yellow_str("[SKIP DELETE]"), dst_full);
            } else {
                // Keep flag is OFF: Delete
                remove_entry(dst_full);
            }
        }

        free(dst_full);
    }
    fflush(stdout);
}

/* watch function */
int fs_watch(const char *src) {
    CFStringRef myPath = CFStringCreateWithCString(kCFAllocatorDefault, src, kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&myPath, 1, NULL);
    FSEventStreamContext context = {0, NULL, NULL, NULL, NULL};

    FSEventStreamRef stream = FSEventStreamCreate(
            NULL,
            &callback_fn,
            &context,
            pathsToWatch,
            kFSEventStreamEventIdSinceNow,
            0.3,
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
            );

    dispatch_queue_t myQueue = dispatch_queue_create("com.rocky.dirsync", NULL);
    FSEventStreamSetDispatchQueue(stream, myQueue);

    if (!FSEventStreamStart(stream)) {
        fprintf(stderr, "FSWatch: Failed to start watch\n");
        return 1;
    }

    if (verbose_f) printf("♦ Watching: %s\n♦ Syncing to: %s\n", src, dst_path);
    if (keep_f && verbose_f) printf("♦ Mode: %s\n", yellow_str("KEEP (No deletions at destination)"));

    CFRelease(pathsToWatch);
    CFRelease(myPath);

    return 0;
}

/* Helper: safe copy */
void set_path(char **to, const char *from) {
    size_t len = strlen(from);
    size_t copy_len = len;
    if (len > 1 && from[len - 1] == '/') {
        copy_len--;
    }

    *to = malloc(copy_len + 1);
    if (!*to) { perror("malloc"); exit(1); }

    strncpy(*to, from, copy_len);
    (*to)[copy_len] = '\0';
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s -s <src> -d <dst> [-v] [-k]\n", argv[0]);
        fprintf(stderr, "  -k : Keep files in destination even if removed from source.\n");
        fprintf(stderr, "  -v : verbose mode.\n");
        return EXIT_FAILURE;
    }

    int opt;
    while ((opt = getopt(argc, argv, "vs:d:k")) != -1) {
        switch (opt) {
            case 'v': verbose_f = 1; break;
            case 'k': keep_f = 1; break;
            case 's': set_path(&src_path, optarg); break;
            case 'd': set_path(&dst_path, optarg); break;
            default: return EXIT_FAILURE;
        }
    }

    if (!src_path || !dst_path) {
        fprintf(stderr, "Error: Source (-s) and Destination (-d) are required.\n");
        return EXIT_FAILURE;
    }

    struct stat sb;
    if (stat(src_path, &sb) == -1) { perror(src_path); return -1; }
    if (!S_ISDIR(sb.st_mode)) { fprintf(stderr, "Error: Source must be a directory\n"); return -1; }

    if (stat(dst_path, &sb) == -1) { perror(dst_path); return -1; }
    if (!S_ISDIR(sb.st_mode)) { fprintf(stderr, "Error: Destination must be a directory\n"); return -1; }

    if (verbose_f) printf(green_str("✔ Paths Verified\n"));

    if (fs_watch(src_path)) return -1;

    dispatch_main();

    return 0;
}
