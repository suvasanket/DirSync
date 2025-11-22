#include <stdio.h>
#include <string.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <copyfile.h>

#define SRC "/Users/suvasanketrout/dev/dir_sync/test/"
#define DST "/Users/suvasanketrout/dev/dir_sync/test_copy/"

int copy_entry(const char *src, const char *dst) {
    copyfile_state_t state = copyfile_state_alloc();

    int ret = copyfile(src, dst, state, COPYFILE_ALL);

    if (ret < 0) {
        perror("Error: Copy failed");
        copyfile_state_free(state);
        return 1;
    }
    return 0;
}

void callback_fn(
        ConstFSEventStreamRef streamRef,
        void *clientCallBackInfo,
        size_t numEvents,
        void *eventPaths,
        const FSEventStreamEventFlags eventFlags[],
        const FSEventStreamEventId eventIds[])
{
    // act
    char **entries = eventPaths;
    for(size_t i = 0; i < numEvents; ++i){
        char *src = entries[i];
        char *src_entry = src + strlen(SRC);
        char dst[sizeof(DST) + strlen(src_entry)];
        snprintf(dst, sizeof dst, DST"%s", src_entry);

        if (copy_entry(src, dst)) continue;
    }


    fflush(stdout);
}

int fs_watch() {
    CFStringRef myPath = CFSTR(SRC);

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
            kFSEventStreamCreateFlagFileEvents
            );

    dispatch_queue_t myQueue = dispatch_queue_create("DirSync", NULL);

    FSEventStreamSetDispatchQueue(stream, myQueue);

    if (!FSEventStreamStart(stream)) {
        perror("FSWatch: Failed to start watch\n");
        return 1;
    }

    printf("Started watching: "SRC"\n");
    dispatch_main();
}

int main() {
    /* Path Check */
    struct stat sb;
    if (stat(SRC, &sb) == -1) { perror(SRC); return -1; }
    if (stat(DST, &sb) == -1) { perror(DST); return -1; }

    if (fs_watch()) return -1;

    return 0;
}
