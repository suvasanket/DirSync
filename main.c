#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <copyfile.h>
#include <removefile.h>
#include <errno.h>

// mkpath_np is a non-portable (macOS) function equivalent to 'mkdir -p'
int mkpath_np(const char *path, mode_t mode);
int asprintf(char **strp, const char *fmt, ...);

char *src_path = NULL;
char *dst_path = NULL;
int verbose_f = 0;
int keep_f = 0;

// Global flag indicating if destination is currently writable/mounted
volatile int is_dest_ready = 0;

#define green_str(str)  "\x1b[32m" str "\x1b[0m"
#define red_str(str)    "\x1b[31m" str "\x1b[0m"
#define yellow_str(str) "\x1b[33m" str "\x1b[0m"
#define blue_str(str)   "\x1b[34m" str "\x1b[0m"

/* -------------------------------------------------------------------------- */
/*                                File Operations                             */
/* -------------------------------------------------------------------------- */

int remove_entry(const char *target) {
    if (access(target, F_OK) != 0) return 0;

    removefile_state_t state = removefile_state_alloc();
    int ret = removefile(target, state, REMOVEFILE_RECURSIVE);

    if (ret < 0) {
        if (verbose_f) perror("Remove failed");
        removefile_state_free(state);
        return 1;
    }

    if (verbose_f) fprintf(stdout, "%s %s\n", red_str("[-]"), target);
    removefile_state_free(state);
    return 0;
}

int copy_entry(const char *src, const char *dst) {
    copyfile_state_t state = copyfile_state_alloc();
    int ret = copyfile(src, dst, state, COPYFILE_ALL | COPYFILE_NOFOLLOW);

    if (ret < 0) {
        // Ignore if src disappeared during processing
        if (access(src, F_OK) != 0) {
            copyfile_state_free(state);
            return 0;
        }
        if (verbose_f) perror("Copy failed");
        copyfile_state_free(state);
        return 1;
    }

    if (verbose_f) fprintf(stdout, "%s %s\n", green_str("[+]"), dst);
    copyfile_state_free(state);
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                            Connection Monitor                              */
/* -------------------------------------------------------------------------- */

/*
   Checks if destination exists.
   If not, tries to create it (mkdir -p).
   If that fails (e.g. volume not mounted), sets ready=0.
   */
void check_destination_availability() {
    struct stat sb;
    int exists = (stat(dst_path, &sb) == 0 && S_ISDIR(sb.st_mode));

    if (exists) {
        if (!is_dest_ready) {
            if (verbose_f) fprintf(stdout, "%s Destination connected: %s\n", blue_str("♦"), dst_path);
            is_dest_ready = 1;
        }
    } else {
        // Try to create it
        if (mkpath_np(dst_path, 0755) == 0) {
            if (!is_dest_ready) {
                if (verbose_f) fprintf(stdout, "%s Destination created: %s\n", blue_str("♦"), dst_path);
                is_dest_ready = 1;
            }
        } else {
            // Creation failed (Volume missing, permission error, etc)
            if (is_dest_ready) {
                if (verbose_f) fprintf(stderr, "%s Destination lost (Waiting for mount...): %s\n", yellow_str("⚠"), dst_path);
                is_dest_ready = 0;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                               Event Handler                                */
/* -------------------------------------------------------------------------- */

void callback_fn(
        ConstFSEventStreamRef streamRef,
        void *clientCallBackInfo,
        size_t numEvents,
        void *eventPaths,
        const FSEventStreamEventFlags eventFlags[],
        const FSEventStreamEventId eventIds[])
{
    // If destination is missing (unmounted), we drop events.
    // (Queuing events during downtime is complex; dropping is safer for this scope)
    if (!is_dest_ready) return;

    char **entries = (char **)eventPaths;

    for(size_t i = 0; i < numEvents; i++){
        char *src_full = entries[i];

        // Basic sanity checks
        if (strlen(src_full) < strlen(src_path)) continue;
        char *rel_path = src_full + strlen(src_path);
        if (strstr(rel_path, ".DS_Store")) continue;

        char *dst_full;
        if (asprintf(&dst_full, "%s%s", dst_path, rel_path) == -1) continue;

        struct stat sb;
        if (lstat(src_full, &sb) == 0) {
            // Source Exists -> Copy
            copy_entry(src_full, dst_full);
        } else {
            // Source Removed
            if (keep_f) {
                if (verbose_f) fprintf(stdout, "%s %s\n", yellow_str("[SKIP DEL]"), dst_full);
            } else {
                remove_entry(dst_full);
            }
        }
        free(dst_full);
    }
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
/*                                   Main                                     */
/* -------------------------------------------------------------------------- */

int fs_watch(const char *src) {
    CFStringRef myPath = CFStringCreateWithCString(kCFAllocatorDefault, src, kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&myPath, 1, NULL);
    FSEventStreamContext context = {0, NULL, NULL, NULL, NULL};

    FSEventStreamRef stream = FSEventStreamCreate(
            NULL, &callback_fn, &context, pathsToWatch,
            kFSEventStreamEventIdSinceNow,
            0.3, // 0.3s latency
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
            );

    dispatch_queue_t myQueue = dispatch_queue_create("com.rocky.dirsync.events", NULL);
    FSEventStreamSetDispatchQueue(stream, myQueue);

    if (!FSEventStreamStart(stream)) {
        if (verbose_f) fprintf(stderr, "FSWatch: Failed to start watch\n");
        return 1;
    }

    // --- Heartbeat Timer for Destination Monitoring ---
    dispatch_queue_t monitorQueue = dispatch_queue_create("com.rocky.dirsync.monitor", NULL);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, monitorQueue);

    // Fire every 2 seconds
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 2 * NSEC_PER_SEC, 1 * NSEC_PER_SEC);

    dispatch_source_set_event_handler(timer, ^{
            check_destination_availability();
            });
    dispatch_resume(timer);
    // --------------------------------------------------

    if (verbose_f) printf("♦ Service Started.\n");

    // Initial Check
    check_destination_availability();

    CFRelease(pathsToWatch);
    CFRelease(myPath);
    return 0;
}

void set_path(char **to, const char *from) {
    size_t len = strlen(from);
    size_t copy_len = len;
    if (len > 1 && from[len - 1] == '/') copy_len--; // Trim trailing slash
    *to = malloc(copy_len + 1);
    if (!*to) exit(1);
    strncpy(*to, from, copy_len);
    (*to)[copy_len] = '\0';
}

int main(int argc, char **argv) {
    if (argc < 2) {
        if (verbose_f) fprintf(stderr, "Usage: %s -s <src> -d <dst> [-v] [-k]\n", argv[0]);
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
        if (verbose_f) fprintf(stderr, "Error: Source and Destination paths required.\n");
        return EXIT_FAILURE;
    }

    // Validate Source immediately (Source must exist to watch it)
    struct stat sb;
    if (stat(src_path, &sb) == -1) {
        perror("Error accessing Source");
        return EXIT_FAILURE;
    }

    printf("Source: %s\nDest:   %s\n", src_path, dst_path);
    if (keep_f) printf("Mode:   %s\n", yellow_str("KEEP (Safe Mode)"));

    // Note: We do NOT validate destination here.
    // The timer in fs_watch will handle creation/waiting.

    if (fs_watch(src_path)) return EXIT_FAILURE;

    dispatch_main();
    return 0;
}
