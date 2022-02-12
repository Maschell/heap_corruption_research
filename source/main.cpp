#include <proc_ui/procui.h>
#include <coreinit/foreground.h>
#include <coreinit/thread.h>
#include <coreinit/cache.h>
#include <coreinit/memexpheap.h>
#include <coreinit/debug.h>
#include <coreinit/filesystem.h>
#include <coreinit/core.h>
#include <cstdlib>
#include <vector>
#include <whb/log_console.h>

#include "logger.h"
#include "utils.h"
#include "CThread.h"

uint32_t heapSize = 100 * 1024 * 1024;

MEMHeapHandle heap = nullptr;

CThread *thread1 = nullptr;
CThread *thread2 = nullptr;
CThread *thread3 = nullptr;

bool stopThreads = false;


void runOnAllCores(CThread::Callback callback, void *callbackArg, int32_t iAttr = 0, int32_t iPriority = 16, int32_t iStackSize = 0x8000) {
    int32_t aff[] = {CThread::eAttributeAffCore2, CThread::eAttributeAffCore1, CThread::eAttributeAffCore0};

    //thread1 = new CThread(iAttr | CThread::eAttributeAffCore0, iPriority, iStackSize, callback, callbackArg);
    //thread1->resumeThread();
    thread2 = new CThread(iAttr | CThread::eAttributeAffCore1, iPriority, iStackSize, callback, callbackArg);
    thread2->resumeThread();
    //thread3 = new CThread(iAttr | CThread::eAttributeAffCore2, iPriority, iStackSize, callback, callbackArg);
    //thread3->resumeThread();

    OSMemoryBarrier();
}

void allocFreeMemoryInLoop(CThread *thread, void *arg) {
    OSReport("[Core %d] Hello from thread\n", OSGetCoreId());
    while (!stopThreads) {
        uint32_t nRandonNumber = 1341;
        auto mem = (uint32_t *) MEMAllocFromExpHeapEx(heap, nRandonNumber, 0x4);
        if (mem) {
            MEMFreeToExpHeap(heap, mem);
        }
    }
}

bool CheckRunning() {
    switch (ProcUIProcessMessages(true)) {
        case PROCUI_STATUS_EXITING: {
            return false;
        }
        case PROCUI_STATUS_RELEASE_FOREGROUND: {
            ProcUIDrawDoneRelease();
            break;
        }
        case PROCUI_STATUS_IN_FOREGROUND: {
            break;
        }
        case PROCUI_STATUS_IN_BACKGROUND:
        default:
            break;
    }
    return true;
}


#define CHECK_HEAP ((int (*)(uint32_t))(0x101C400 + 0x300a8))

void ReadFile(bool roundup) {
    FSCmdBlock cmd;
    FSClient client;

    if (FSAddClient(&client, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) {
        OSFatal("Failed to add client");
    }
    FSInitCmdBlock(&cmd);

    FSFileHandle iFd = -1;

    auto status = FSOpenFile(&client, &cmd, "/vol/external01/test.txt", "r", &iFd, FS_ERROR_FLAG_ALL);
    if (status != FS_STATUS_OK) {
        OSFatal("Failed to open file");
    }

    FSStat stat;
    stat.size = 0;

    void *pBuffer = nullptr;

    status = FSGetStatFile(&client, &cmd, iFd, &stat, FS_ERROR_FLAG_ALL);

    if (status == FS_STATUS_OK && stat.size > 0) {
        pBuffer = MEMAllocFromExpHeapEx(heap, roundup ? ROUNDUP(stat.size, 0x40) : stat.size, 0x40);
        DEBUG_FUNCTION_LINE("Allocated %d bytes: %08X", stat.size, pBuffer);
    } else {
        OSFatal("Failed to stat file\n");
    }

    uint32_t done = 0;
    uint32_t blocksize = 0x4000;

    while (done < stat.size) {
        if (done + blocksize > stat.size) {
            blocksize = stat.size - done;
        }

        // The corruption only happens on the last read of a file.
        if (blocksize < 0x4000) {
            DEBUG_FUNCTION_LINE("Read %d bytes: into %08X", blocksize, ((uint32_t) pBuffer + done));
        }
        int32_t readBytes = FSReadFile(&client, &cmd, (uint8_t *) ((uint32_t) pBuffer + done), 1, blocksize, iFd, 0, FS_ERROR_FLAG_ALL);

        // The corruption only happens on the last read of a file.
        if (blocksize < 0x4000 && !CHECK_HEAP((uint32_t) heap)) {
            OSReport("##############################\n");
            OSReport("MEMORY CORRUPTION. roundup: %s!!!!!\n", roundup ? "true" : "false");
            OSReport("############################\n");
            WHBLogPrintf("##############################");
            WHBLogPrintf("MEMORY CORRUPTION. roundup: %s!!!!!", roundup ? "true" : "false");
            WHBLogPrintf("##############################");
            WHBLogConsoleDraw();
            while (true);
        }
        if (readBytes <= 0) {
            break;
        }
        done += readBytes;
    }

    FSDelClient(&client, FS_ERROR_FLAG_ALL);
    MEMFreeToExpHeap(heap, pBuffer);
}

extern "C" void _SYSLaunchMiiStudio(void *);

int
main(int argc, char **argv) {
    ProcUIInit(OSSavesDone_ReadyToRelease);

    void *heapData = malloc(heapSize);
    if (!heapData) {
        OSFatal("Failed to alloc memory for heap");
    }

    heap = MEMCreateExpHeapEx(heapData, heapSize, MEM_HEAP_FLAG_USE_LOCK);
    OSMemoryBarrier();

    runOnAllCores(allocFreeMemoryInLoop, nullptr);

    WHBLogConsoleInit();
    WHBLogConsoleDraw();

    uint32_t loopEnd = 100;
    // This should work without any problems.
    for (int i = 0; i < 100; i++) {
        WHBLogPrintf("Read file with aligned buffer size. Iteration %d of %d", i, loopEnd);
        WHBLogConsoleDraw();
        ReadFile(true);
    }

    OSSleepTicks(OSSecondsToTicks(1));

    loopEnd = 50;

    // This should corrupt the heap.
    for (int i = 0; i < loopEnd; i++) {
        WHBLogPrintf("Read file with unaligned buffer size. Iteration %d / %d", i, loopEnd);
        WHBLogConsoleDraw();
        ReadFile(false);
    }

    WHBLogPrintf("Wait for threads");
    WHBLogConsoleDraw();
    stopThreads = true;
    OSMemoryBarrier();

    //delete thread1;
    delete thread2;
    //delete thread3;

    _SYSLaunchMiiStudio(nullptr);

    while (CheckRunning()) {
        // wait.
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    MEMDestroyExpHeap(heap);

    free(heapData);

    WHBLogConsoleFree();

    ProcUIShutdown();

    return 0;
}
