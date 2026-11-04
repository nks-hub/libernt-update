/* Builds without the C runtime: only ntdll calls, no memset/memcpy from the CRT. */
#include "bootplan.h"
#include "regplan.h"

#define LUUP_FileBasicInformation ((FILE_INFORMATION_CLASS)4)
#define LUUP_FileStandardInformation ((FILE_INFORMATION_CLASS)5)
#define LUUP_FileRenameInformation ((FILE_INFORMATION_CLASS)10)
#define LUUP_FileDispositionInformation ((FILE_INFORMATION_CLASS)13)
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003AL)
#endif
#define LUUP_STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define LUUP_STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define LUUP_STATUS_BAD_PLAN ((NTSTATUS)0xC000007BL) /* STATUS_INVALID_IMAGE_FORMAT */
#define LUUP_MAX_PLAN (64u << 20)

typedef struct {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
} LUUP_RENAME_INFO;

typedef struct {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    ULONG FileAttributes;
} LUUP_BASIC_INFO;

typedef struct {
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending, Directory;
} LUUP_STANDARD_INFO;

NTSYSAPI NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
NTSYSAPI NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, const void*, ULONG, PLARGE_INTEGER, PULONG);
NTSYSAPI NTSTATUS NTAPI NtFlushBuffersFile(HANDLE, PIO_STATUS_BLOCK);
NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
NTSYSAPI NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
NTSYSAPI NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG);
NTSYSAPI NTSTATUS NTAPI NtQueryAttributesFile(POBJECT_ATTRIBUTES, PVOID);

static void Fill(void* p, unsigned char v, SIZE_T n) {
    volatile unsigned char* b = (volatile unsigned char*)p;
    while (n--) *b++ = v;
}

static void Copy(void* d, const void* s, SIZE_T n) {
    volatile unsigned char* a = (volatile unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    while (n--) *a++ = *b++;
}

static PVOID Alloc(SIZE_T size) {
    PVOID p = NULL;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1, &p, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return NULL;
    return p;
}

PVOID LuupAlloc(SIZE_T size) { return Alloc(size); }

void LuupFree(PVOID p) {
    SIZE_T size = 0;
    if (p) NtFreeVirtualMemory((HANDLE)-1, &p, &size, MEM_RELEASE);
}

static void InitPath(UNICODE_STRING* us, OBJECT_ATTRIBUTES* oa, PCWSTR path) {
    RtlInitUnicodeString(us, path);
    InitializeObjectAttributes(oa, us, OBJ_CASE_INSENSITIVE, NULL, NULL);
}

NTSTATUS LuupReadFile(PCWSTR ntPath, PVOID* data, ULONG* size) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    LUUP_STANDARD_INFO si;
    NTSTATUS st;
    PVOID buf;

    *data = NULL;
    *size = 0;
    InitPath(&us, &oa, ntPath);
    st = NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) return st;
    st = NtQueryInformationFile(h, &iosb, &si, sizeof(si), LUUP_FileStandardInformation);
    if (NT_SUCCESS(st) && (si.EndOfFile.HighPart || si.EndOfFile.LowPart > LUUP_MAX_PLAN)) st = LUUP_STATUS_BAD_PLAN;
    if (NT_SUCCESS(st)) {
        buf = Alloc(si.EndOfFile.LowPart + sizeof(WCHAR));
        if (!buf) {
            st = LUUP_STATUS_NO_MEMORY;
        } else {
            st = si.EndOfFile.LowPart ? NtReadFile(h, NULL, NULL, NULL, &iosb, buf, si.EndOfFile.LowPart, NULL, NULL) : STATUS_SUCCESS;
            if (NT_SUCCESS(st)) {
                *data = buf;
                *size = si.EndOfFile.LowPart;
            } else {
                LuupFree(buf);
            }
        }
    }
    NtClose(h);
    return st;
}

NTSTATUS LuupWriteFile(PCWSTR ntPath, const void* data, ULONG size) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;

    InitPath(&us, &oa, ntPath);
    st = NtCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) return st;
    st = size ? NtWriteFile(h, NULL, NULL, NULL, &iosb, data, size, NULL, NULL) : STATUS_SUCCESS;
    if (NT_SUCCESS(st)) st = NtFlushBuffersFile(h, &iosb);
    NtClose(h);
    return st;
}

BOOLEAN LuupFileExists(PCWSTR ntPath) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    LUUP_BASIC_INFO bi;
    InitPath(&us, &oa, ntPath);
    return NT_SUCCESS(NtQueryAttributesFile(&oa, &bi));
}

static NTSTATUS ClearReadOnly(PCWSTR ntPath) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    LUUP_BASIC_INFO bi;
    NTSTATUS st;

    InitPath(&us, &oa, ntPath);
    st = NtOpenFile(&h, FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(st)) return st;
    Fill(&bi, 0, sizeof(bi));
    bi.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    st = NtSetInformationFile(h, &iosb, &bi, sizeof(bi), LUUP_FileBasicInformation);
    NtClose(h);
    return st;
}

NTSTATUS LuupDeleteFile(PCWSTR ntPath) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    BOOLEAN del = TRUE;
    NTSTATUS st;

    ClearReadOnly(ntPath);
    InitPath(&us, &oa, ntPath);
    st = NtOpenFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_OBJECT_PATH_NOT_FOUND) return STATUS_SUCCESS;
    if (!NT_SUCCESS(st)) return st;
    st = NtSetInformationFile(h, &iosb, &del, sizeof(del), LUUP_FileDispositionInformation);
    NtClose(h);
    return st;
}

static NTSTATUS LuupCopyFile(PCWSTR from, PCWSTR to) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE source = NULL, target = NULL;
    LUUP_STANDARD_INFO si;
    LUUP_BASIC_INFO bi;
    NTSTATUS st;
    PVOID buf = NULL;
    ULONGLONG remaining;

    InitPath(&us, &oa, from);
    st = NtOpenFile(&source, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) return st;
    st = NtQueryInformationFile(source, &iosb, &si, sizeof(si), LUUP_FileStandardInformation);
    if (NT_SUCCESS(st)) st = NtQueryInformationFile(source, &iosb, &bi, sizeof(bi), LUUP_FileBasicInformation);
    if (!NT_SUCCESS(st)) goto out;

    ClearReadOnly(to);
    InitPath(&us, &oa, to);
    st = NtCreateFile(&target, GENERIC_WRITE | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL, bi.FileAttributes,
                      0, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) goto out;
    buf = Alloc(64u * 1024u);
    if (!buf) {
        st = LUUP_STATUS_NO_MEMORY;
        goto out;
    }
    remaining = (ULONGLONG)si.EndOfFile.QuadPart;
    while (remaining) {
        ULONG chunk = remaining > 64u * 1024u ? 64u * 1024u : (ULONG)remaining;
        st = NtReadFile(source, NULL, NULL, NULL, &iosb, buf, chunk, NULL, NULL);
        if (!NT_SUCCESS(st) || iosb.Information != chunk) {
            if (NT_SUCCESS(st)) st = LUUP_STATUS_BAD_PLAN;
            break;
        }
        st = NtWriteFile(target, NULL, NULL, NULL, &iosb, buf, chunk, NULL, NULL);
        if (!NT_SUCCESS(st) || iosb.Information != chunk) {
            if (NT_SUCCESS(st)) st = LUUP_STATUS_BAD_PLAN;
            break;
        }
        remaining -= chunk;
    }
    if (NT_SUCCESS(st)) st = NtFlushBuffersFile(target, &iosb);
    if (NT_SUCCESS(st)) st = NtSetInformationFile(target, &iosb, &bi, sizeof(bi), LUUP_FileBasicInformation);
out:
    LuupFree(buf);
    if (target) NtClose(target);
    if (source) NtClose(source);
    return st;
}

static ULONG Len(PCWSTR s) {
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

static BOOLEAN SameContents(PCWSTR a, PCWSTR b) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE left = NULL, right = NULL;
    LUUP_STANDARD_INFO first, second;
    PVOID memory = NULL;
    BOOLEAN equal = FALSE;
    ULONGLONG remaining;
    NTSTATUS st;
    InitPath(&us, &oa, a);
    st = NtOpenFile(&left, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) goto out;
    InitPath(&us, &oa, b);
    st = NtOpenFile(&right, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) goto out;
    if (!NT_SUCCESS(NtQueryInformationFile(left, &iosb, &first, sizeof(first), LUUP_FileStandardInformation)) ||
        !NT_SUCCESS(NtQueryInformationFile(right, &iosb, &second, sizeof(second), LUUP_FileStandardInformation)) ||
        first.EndOfFile.QuadPart != second.EndOfFile.QuadPart) goto out;
    memory = Alloc(128u * 1024u);
    if (!memory) goto out;
    remaining = (ULONGLONG)first.EndOfFile.QuadPart;
    equal = TRUE;
    while (remaining && equal) {
        ULONG size = remaining > 65536 ? 65536 : (ULONG)remaining, i;
        if (!NT_SUCCESS(NtReadFile(left, NULL, NULL, NULL, &iosb, memory, size, NULL, NULL)) || iosb.Information != size ||
            !NT_SUCCESS(NtReadFile(right, NULL, NULL, NULL, &iosb, (UCHAR*)memory + 65536, size, NULL, NULL)) || iosb.Information != size) {
            equal = FALSE;
            break;
        }
        for (i = 0; i < size; i++) if (((UCHAR*)memory)[i] != ((UCHAR*)memory)[65536 + i]) { equal = FALSE; break; }
        remaining -= size;
    }
out:
    if (left) NtClose(left);
    if (right) NtClose(right);
    LuupFree(memory);
    return equal;
}

static BOOLEAN ExpectedContents(PCWSTR target, PCWSTR expected) {
    return expected[0] == L'-' && !expected[1] ? !LuupFileExists(target) : SameContents(target, expected);
}

static NTSTATUS GuardedReplace(PCWSTR source, PCWSTR target, PCWSTR expected) {
    PWSTR temporary;
    ULONG length;
    NTSTATUS st;
    if (SameContents(source, target)) return STATUS_SUCCESS;
    if (!ExpectedContents(target, expected)) return STATUS_OBJECT_NAME_COLLISION;
    length = Len(target);
    temporary = (PWSTR)Alloc((length + 16) * sizeof(WCHAR));
    if (!temporary) return LUUP_STATUS_NO_MEMORY;
    Copy(temporary, target, length * sizeof(WCHAR));
    Copy(temporary + length, L".lntu-new", 10 * sizeof(WCHAR));
    st = LuupCopyFile(source, temporary);
    if (NT_SUCCESS(st)) st = LuupRenameFile(temporary, target, TRUE);
    if (!NT_SUCCESS(st)) LuupDeleteFile(temporary);
    LuupFree(temporary);
    return st;
}

static NTSTATUS RenameOnce(PCWSTR from, PCWSTR to, BOOLEAN replace) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;
    ULONG nameBytes = Len(to) * sizeof(WCHAR);
    ULONG infoSize = (ULONG)FIELD_OFFSET(LUUP_RENAME_INFO, FileName) + nameBytes;
    LUUP_RENAME_INFO* info;

    InitPath(&us, &oa, from);
    st = NtOpenFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(st)) return st;
    info = (LUUP_RENAME_INFO*)Alloc(infoSize);
    if (!info) {
        NtClose(h);
        return LUUP_STATUS_NO_MEMORY;
    }
    info->ReplaceIfExists = replace;
    info->RootDirectory = NULL;
    info->FileNameLength = nameBytes;
    Copy(info->FileName, to, nameBytes);
    st = NtSetInformationFile(h, &iosb, info, infoSize, LUUP_FileRenameInformation);
    LuupFree(info);
    NtClose(h);
    return st;
}

NTSTATUS LuupRenameFile(PCWSTR from, PCWSTR to, BOOLEAN replace) {
    NTSTATUS st = RenameOnce(from, to, replace);
    /* A read-only target refuses to be replaced: same recovery as smss for PendingFileRenameOperations. */
    if (replace && (st == STATUS_ACCESS_DENIED || st == STATUS_OBJECT_NAME_COLLISION) && NT_SUCCESS(ClearReadOnly(to)))
        st = RenameOnce(from, to, replace);
    return st;
}

/* Splits the next line off *cursor in place; returns NULL at the end. */
static PWSTR NextLine(PWSTR* cursor, PWSTR end) {
    PWSTR start = *cursor, p = start;
    if (start >= end) return NULL;
    while (p < end && *p != L'\n') p++;
    *cursor = p < end ? p + 1 : end;
    if (p > start && p[-1] == L'\r') p--;
    *p = 0;
    return start;
}

static PWSTR NextField(PWSTR* cursor) {
    PWSTR start = *cursor, p = start;
    if (!start) return NULL;
    while (*p && *p != L'\t') p++;
    if (*p) {
        *p = 0;
        *cursor = p + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

#define LUUP_LOG_CHARS (128u * 1024u)

typedef struct {
    PWSTR buf;
    ULONG len;
} LUUP_LOG;

static void LogStr(LUUP_LOG* log, PCWSTR s) {
    if (!log->buf) return;
    while (*s && log->len + 1 < LUUP_LOG_CHARS) log->buf[log->len++] = *s++;
}

static void LogHex(LUUP_LOG* log, ULONG v, int base) {
    WCHAR tmp[12];
    int n = 0;
    do {
        tmp[n++] = L"0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n && log->buf && log->len + 1 < LUUP_LOG_CHARS) log->buf[log->len++] = tmp[--n];
}

NTSTATUS LuupApplyPlan(PCWSTR planNtPath, LUUP_RESULT* result, PCWSTR logNtPath) {
    PVOID data;
    ULONG size, lineNo = 0;
    PWSTR text, end, line;
    NTSTATUS st;
    LUUP_LOG log = {NULL, 0};

    Fill(result, 0, sizeof(*result));
    if (logNtPath) {
        log.buf = (PWSTR)Alloc(LUUP_LOG_CHARS * sizeof(WCHAR));
        if (log.buf) log.buf[log.len++] = 0xFEFF;
    }
    st = LuupReadFile(planNtPath, &data, &size);
    if (!NT_SUCCESS(st)) {
        LuupFree(log.buf);
        return st;
    }
    text = (PWSTR)data;
    end = text + size / sizeof(WCHAR);
    if (text < end && *text == 0xFEFF) text++;
    line = NextLine(&text, end);
    lineNo = 1;
    if (!line || line[0] != L'L' || line[1] != L'U' || line[2] != L'U' || line[3] != L'P') {
        LuupFree(data);
        LuupFree(log.buf);
        return LUUP_STATUS_BAD_PLAN;
    }
    while ((line = NextLine(&text, end)) != NULL) {
        PWSTR cur = line, op, a, b;
        lineNo++;
        if (!*line) continue;
        op = NextField(&cur);
        a = NextField(&cur);
        b = NextField(&cur);
        if (op[0] == L'M' && !op[1] && a && b) st = LuupRenameFile(a, b, TRUE);
        else if (op[0] == L'G' && !op[1] && a && b && !cur) st = LuupApplyRegistry(a, b);
        else if (op[0] == L'C' && !op[1] && a && b) st = LuupCopyFile(a, b);
        else if (op[0] == L'R' && !op[1] && a && b) {
            PWSTR expected = NextField(&cur);
            st = expected ? GuardedReplace(a, b, expected) : LUUP_STATUS_INVALID_PARAMETER;
        }
        else if (op[0] == L'X' && !op[1] && a && b)
            st = !LuupFileExists(a) || ExpectedContents(a, b) ? LuupDeleteFile(a) : STATUS_OBJECT_NAME_COLLISION;
        else if (op[0] == L'D' && !op[1] && a) st = LuupDeleteFile(a);
        else st = LUUP_STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(st)) {
            result->Done++;
        } else {
            if (!result->Failed) {
                result->FirstFailedLine = lineNo;
                result->FirstFailure = st;
            }
            result->Failed++;
            LogStr(&log, L"line ");
            LogHex(&log, lineNo, 10);
            LogStr(&log, L" status ");
            LogHex(&log, (ULONG)st, 16);
            LogStr(&log, L": ");
            LogStr(&log, op);
            LogStr(&log, L" ");
            LogStr(&log, a ? a : L"");
            if (b) {
                LogStr(&log, L" -> ");
                LogStr(&log, b);
            }
            LogStr(&log, L"\n");
        }
    }
    LuupFree(data);
    if (log.buf) {
        LuupWriteFile(logNtPath, log.buf, log.len * sizeof(WCHAR));
        LuupFree(log.buf);
    }
    return STATUS_SUCCESS;
}

#define LUUP_MAX_PATH 1024

static PWSTR BootFile(PCWSTR dir, PCWSTR name) {
    ULONG a = Len(dir), b = Len(name);
    PWSTR p = (PWSTR)Alloc((a + b + 8) * sizeof(WCHAR));
    if (!p) return NULL;
    Copy(p, dir, a * sizeof(WCHAR));
    Copy(p + a, L"\\boot\\", 6 * sizeof(WCHAR));
    Copy(p + a + 6, name, (b + 1) * sizeof(WCHAR));
    return p;
}

static char* PutNum(char* p, ULONG v, int base) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static char* PutStr(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

static void WriteResult(PCWSTR path, const char* what, const LUUP_RESULT* r, NTSTATUS planStatus) {
    char buf[160], *p = buf;
    p = PutStr(p, what);
    p = PutStr(p, " done ");
    p = PutNum(p, r->Done, 10);
    p = PutStr(p, " failed ");
    p = PutNum(p, r->Failed, 10);
    p = PutStr(p, " line ");
    p = PutNum(p, r->FirstFailedLine, 10);
    p = PutStr(p, " status ");
    p = PutNum(p, (ULONG)(r->Failed ? r->FirstFailure : planStatus), 16);
    p = PutStr(p, "\n");
    LuupWriteFile(path, buf, (ULONG)(p - buf));
}

static ULONG ReadCount(PCWSTR path) {
    PVOID data;
    ULONG size, n = 0, i;
    if (!NT_SUCCESS(LuupReadFile(path, &data, &size))) return 0;
    for (i = 0; i < size && ((char*)data)[i] >= '0' && ((char*)data)[i] <= '9'; i++) n = n * 10 + (((char*)data)[i] - '0');
    LuupFree(data);
    return n;
}

static NTSTATUS WriteCount(PCWSTR path, ULONG n) {
    char buf[12];
    char* p = PutNum(buf, n, 10);
    return LuupWriteFile(path, buf, (ULONG)(p - buf));
}

NTSTATUS LuupBoot(PCWSTR dataDirNt, BOOLEAN forceRollback, void (*say)(PCWSTR text)) {
    PWSTR plan = BootFile(dataDirNt, L"plan.txt"), planDone = BootFile(dataDirNt, L"plan.done");
    PWSTR rb = BootFile(dataDirNt, L"rollback.txt"), rbDone = BootFile(dataDirNt, L"rollback.done");
    PWSTR trial = BootFile(dataDirNt, L"trial"), result = BootFile(dataDirNt, L"result.txt");
    PWSTR applying = BootFile(dataDirNt, L"applying");
    PWSTR committed = BootFile(dataDirNt, L"committed");
    PWSTR marker = BootFile(dataDirNt, L"rolledback"), cancelled = BootFile(dataDirNt, L"plan.cancelled");
    PWSTR planLog = BootFile(dataDirNt, L"plan.log"), rbLog = BootFile(dataDirNt, L"rollback.log");
    LUUP_RESULT r;
    NTSTATUS st = STATUS_SUCCESS;
    BOOLEAN rollback = FALSE;

    if (!plan || !planDone || !rb || !rbDone || !trial || !result || !marker || !cancelled || !applying || !committed) {
        st = LUUP_STATUS_NO_MEMORY;
        goto out;
    }
    if (LuupFileExists(committed)) goto out;
    if (LuupFileExists(applying)) {
        rollback = TRUE;
        goto restore;
    }
    if (LuupFileExists(plan)) {
        if (forceRollback) {
            /* Recovery entry chosen before the update was applied: nothing to undo, just drop it. */
            LuupRenameFile(plan, cancelled, TRUE);
            LuupDeleteFile(rb);
            LuupWriteFile(marker, "cancelled\n", 10);
            goto out;
        }
        if (say) say(L"LiberNT Update: installing the update...\n");
        st = LuupWriteFile(applying, "applying\n", 9);
        if (!NT_SUCCESS(st)) goto out;
        st = LuupApplyPlan(plan, &r, planLog);
        WriteResult(result, "plan", &r, st);
        if (!NT_SUCCESS(st) || r.Failed) {
            rollback = TRUE;
            goto restore;
        }
        st = LuupRenameFile(plan, planDone, TRUE);
        if (!NT_SUCCESS(st)) goto out;
        st = WriteCount(trial, 1);
        if (!NT_SUCCESS(st)) goto out;
        LuupDeleteFile(applying);
        goto out;
    }
    if (LuupFileExists(trial)) {
        ULONG n = ReadCount(trial) + 1;
        rollback = forceRollback || n >= 3;
        if (!rollback) {
            st = WriteCount(trial, n);
            if (!NT_SUCCESS(st)) goto out;
        }
    } else {
        rollback = forceRollback;
    }
restore:
    if (rollback && LuupFileExists(rb)) {
        if (say) say(L"LiberNT Update: restoring the previous version...\n");
        st = LuupApplyPlan(rb, &r, rbLog);
        WriteResult(result, "rollback", &r, st);
        if (NT_SUCCESS(st) && !r.Failed) {
            st = LuupWriteFile(marker, "rolledback\n", 11);
            if (!NT_SUCCESS(st)) goto out;
            LuupRenameFile(rb, rbDone, TRUE);
            LuupDeleteFile(trial);
            LuupDeleteFile(plan);
            LuupDeleteFile(applying);
        } else if (NT_SUCCESS(st)) {
            st = r.FirstFailure;
        }
    }
out:
    LuupFree(plan); LuupFree(planDone); LuupFree(rb); LuupFree(rbDone);
    LuupFree(trial); LuupFree(result); LuupFree(marker); LuupFree(cancelled);
    LuupFree(planLog); LuupFree(rbLog);
    LuupFree(applying);
    LuupFree(committed);
    return st;
}
