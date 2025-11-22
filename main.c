#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <copyfile.h>
#include <removefile.h>

char *src_path, *dst_path;
int verbose_f = 0;

#define green_str(str) "\x1b[32m" str "\x1b[0m"

/* entry remove function */
int remove_entry(const char *target) {
    removefile_state_t state = removefile_state_alloc();

    int ret = removefile(target, state, REMOVEFILE_RECURSIVE);

    if (ret < 0) {
        perror("Error: Remove action failed");
        removefile_state_free(state);
        return 1;
    }

    if (verbose_f) fprintf(stdout, "Removed: %s\n", target);
    removefile_state_free(state);
    return 0;
}

/* entry copy function */
int copy_entry(const char *src, const char *dst) {
    copyfile_state_t state = copyfile_state_alloc();

    int ret = copyfile(src, dst, state, COPYFILE_ALL);

    if (ret < 0) {
        perror("Error: Copy action failed");
        copyfile_state_free(state);
        return 1;
    }

    if (verbose_f) fprintf(stdout, "Copied to: %s\n", dst);
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
    // act
    char **entries = (char **)eventPaths;

    for(size_t i = 0; i < numEvents; i++){
        char *src = entries[i];
        char *src_entry = src + strlen(src_path);
        char dst[strlen(dst_path) + strlen(src_entry) + 1];
        snprintf(dst, sizeof dst, "%s%s", dst_path, src_entry);

        int marked_i;
        if ((eventFlags[i] & kFSEventStreamEventFlagItemRemoved) ||
                (eventFlags[i] & kFSEventStreamEventFlagItemRenamed) && marked_i+1 != i) {
            marked_i = i;
            if (access(dst, F_OK) == 0) {
                if (remove_entry(dst)) continue;
            }
        } else {
            if (copy_entry(src, dst)) continue;
        }
    }

    fflush(stdout);
}

/* watch function */
int fs_watch(const char *src) {
    CFStringRef myPath = CFStringCreateWithCString(
            kCFAllocatorDefault,
            src,
            kCFStringEncodingUTF8);

    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&myPath, 1, NULL);

    FSEventStreamContext context = {0, NULL, NULL, NULL, NULL};

    FSEventStreamRef stream;
    stream = FSEventStreamCreate(
            NULL,
            &callback_fn,
            &context,
            pathsToWatch,
            kFSEventStreamEventIdSinceNow,
            1.0,
            kFSEventStreamCreateFlagFileEvents);

    dispatch_queue_t myQueue = dispatch_queue_create("com.rocky.dirsync", NULL);

    FSEventStreamSetDispatchQueue(stream, myQueue);

    if (!FSEventStreamStart(stream)) {
        perror("FSWatch: Failed to start watch\n");
        return 1;
    }

    if (verbose_f) printf("♦ Setup complete for: %s\n", src);

    CFRelease(pathsToWatch);
    CFRelease(myPath);

    return 0;
}

/* Helper: safe copy */
int copy_path(char **to, const char *from) {
    if (!to || !from) return 1;
    size_t len = strlen(from);
    int need_slash = (len == 0) ? 0 : (from[len - 1] != '/');
    int n = snprintf(NULL, 0, "%s%s", from, need_slash ? "/" : "");
    if (n < 0) return 1;
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) return 1;
    if (snprintf(tmp, (size_t)n + 1, "%s%s", from, need_slash ? "/" : "") < 0) {
        free(tmp);
        return 1;
    }
    *to = tmp;
    return 0;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "vs:d:")) != -1) {
        switch (opt) {
            case 'v':
                verbose_f = 1; break;
            case 's':
                if (optarg) copy_path(&src_path, optarg);
                break;
            case 'd':
                if (optarg) copy_path(&dst_path, optarg);
                break;
            default:
                return EXIT_FAILURE;
        }
    }


    /* Path Check */
    struct stat sb;
    if (stat(src_path, &sb) == -1) { fprintf(stderr, "%s", src_path); return -1; }
    if (stat(dst_path, &sb) == -1) { fprintf(stderr, "%s", dst_path); return -1; }

    if (verbose_f) printf(green_str("✔ FilePath Exist\n"));

    /* path watch */
    if (fs_watch(src_path)) return -1;
    dispatch_main(); //block

    return 0;
}
