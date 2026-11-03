/* Boot-time file replacement shared by luupboot.exe (native, ntdll only) and the client self-test.
 *
 * A plan is a UTF-16LE text file:
 *     LUUP 1
 *     M<TAB>\??\C:\...\new\0.bin<TAB>\??\C:\Windows\explorer.exe     move, replacing the target
 *     D<TAB>\??\C:\Windows\old.dll                                     delete
 * Lines end with LF; CR before LF is ignored. */
#pragma once
#include <windows.h>
#include <winternl.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LUUP_RESULT {
    ULONG Done;
    ULONG Failed;
    ULONG FirstFailedLine;     /* 1-based plan line, 0 when nothing failed */
    NTSTATUS FirstFailure;
} LUUP_RESULT;

/* Applies every line of the plan in order and keeps going after a failed line; every failed line goes to the
 * UTF-16 log file logNtPath (may be NULL). */
NTSTATUS LuupApplyPlan(PCWSTR planNtPath, LUUP_RESULT* result, PCWSTR logNtPath);

/* One boot of luupboot.exe. dataDirNt is the client's data folder as an NT path; its boot\ subfolder holds
 *   plan.txt      update to apply now (renamed to plan.done afterwards, and "trial" is created)
 *   rollback.txt  undo of that update
 *   trial         boots since the plan was applied that the LiberNTUpdate service has not confirmed yet
 *   result.txt    outcome of the last plan or rollback, for the service
 *   rolledback    written when luupboot itself rolled the update back
 * The update is rolled back when forceRollback is set (recovery boot entry) or on the third boot that the
 * service never confirmed. say() shows progress on the boot screen; it may be NULL. */
NTSTATUS LuupBoot(PCWSTR dataDirNt, BOOLEAN forceRollback, void (*say)(PCWSTR text));

/* NtCreateFile/NtReadFile helpers, also used by luupboot.exe for its state files. */
NTSTATUS LuupReadFile(PCWSTR ntPath, PVOID* data, ULONG* size);
NTSTATUS LuupWriteFile(PCWSTR ntPath, const void* data, ULONG size);
NTSTATUS LuupDeleteFile(PCWSTR ntPath);
NTSTATUS LuupRenameFile(PCWSTR fromNtPath, PCWSTR toNtPath, BOOLEAN replace);
BOOLEAN LuupFileExists(PCWSTR ntPath);
void LuupFree(PVOID p);

#ifdef __cplusplus
}
#endif
