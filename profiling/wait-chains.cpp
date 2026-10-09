#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <wct.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const DWORD process = std::strtoul(argv[1], nullptr, 10);
    const auto session = OpenThreadWaitChainSession(0, nullptr);
    if (!session) { std::printf("session error %lu\n", GetLastError()); return 1; }
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { CloseThreadWaitChainSession(session); return 1; }
    THREADENTRY32 thread{sizeof(thread)};
    if (Thread32First(snapshot, &thread)) do {
        if (thread.th32OwnerProcessID != process) continue;
        WAITCHAIN_NODE_INFO nodes[16]{};
        DWORD count = 16;
        BOOL cycle = FALSE;
        if (!GetThreadWaitChain(session, 0, 0, thread.th32ThreadID, &count, nodes, &cycle)) {
            std::printf("tid=%lu error=%lu\n", thread.th32ThreadID, GetLastError());
            continue;
        }
        std::printf("tid=%lu cycle=%d nodes=%lu", thread.th32ThreadID, cycle, count);
        for (DWORD i = 0; i < count; ++i) {
            const auto& node = nodes[i];
            std::printf(" [type=%d status=%d", node.ObjectType, node.ObjectStatus);
            if (node.ObjectType == WctThreadType) std::printf(" pid=%lu tid=%lu wait=%lu switches=%lu", node.ThreadObject.ProcessId, node.ThreadObject.ThreadId, node.ThreadObject.WaitTime, node.ThreadObject.ContextSwitches);
            std::printf("]");
        }
        std::printf("\n");
    } while (Thread32Next(snapshot, &thread));
    CloseHandle(snapshot);
    CloseThreadWaitChainSession(session);
}
