/* luupboot.exe: native program that smss starts from BootExecute ("luupboot \??\C:\ProgramData\LiberNTUpdate").
 * It runs before smss processes PendingFileRenameOperations and before any Win32 process exists, so every
 * system file can be replaced; see LuupBoot() for what it does on each boot. */
#include "bootplan.h"

NTSYSAPI NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING);
NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE, NTSTATUS);
NTSYSAPI NTSTATUS NTAPI NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSYSAPI NTSTATUS NTAPI NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);

typedef struct {
    ULONG TitleIndex, Type, DataLength;
    UCHAR Data[1];
} LUUP_KEY_VALUE_PARTIAL;

static void Say(PCWSTR text) {
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, text);
    NtDisplayString(&us);
}

static WCHAR Upper(WCHAR c) { return c >= L'a' && c <= L'z' ? (WCHAR)(c - 32) : c; }

/* True when the loader options of this boot contain LNTUROLLBACK (the recovery entry in freeldr.ini). */
static BOOLEAN RecoveryBoot(void) {
    static const WCHAR word[] = L"LNTUROLLBACK";
    UNICODE_STRING key, name;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG len = 0, i, j, chars;
    static UCHAR buf[2048];
    LUUP_KEY_VALUE_PARTIAL* v = (LUUP_KEY_VALUE_PARTIAL*)buf;
    PCWSTR s;
    BOOLEAN found = FALSE;

    RtlInitUnicodeString(&key, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control");
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(NtOpenKey(&h, KEY_QUERY_VALUE, &oa))) return FALSE;
    RtlInitUnicodeString(&name, L"SystemStartOptions");
    if (NT_SUCCESS(NtQueryValueKey(h, &name, 2 /* KeyValuePartialInformation */, buf, sizeof(buf) - sizeof(WCHAR), &len)) &&
        v->Type == REG_SZ) {
        s = (PCWSTR)v->Data;
        chars = v->DataLength / sizeof(WCHAR);
        for (i = 0; !found && i + 12 <= chars; i++) {
            for (j = 0; j < 12 && Upper(s[i + j]) == word[j]; j++) {}
            found = j == 12;
        }
    }
    NtClose(h);
    return found;
}

/* The data folder is the first argument that is an NT path. */
static PWSTR DataDirFromCommandLine(PPEB peb, PWSTR out, ULONG cap) {
    UNICODE_STRING* cl = &peb->ProcessParameters->CommandLine;
    PCWSTR s = cl->Buffer;
    ULONG n = cl->Length / sizeof(WCHAR), i, k = 0;
    for (i = 0; i + 4 <= n; i++)
        if (s[i] == L'\\' && s[i + 1] == L'?' && s[i + 2] == L'?' && s[i + 3] == L'\\') break;
    if (i + 4 > n) return NULL;
    while (i < n && s[i] != L' ' && s[i] != L'"' && k + 1 < cap) out[k++] = s[i++];
    while (k && out[k - 1] == L'\\') k--;
    out[k] = 0;
    return k ? out : NULL;
}

void NTAPI NtProcessStartup(PPEB peb) {
    static WCHAR dir[512];
    NTSTATUS st = (NTSTATUS)0xC000000DL;
    if (DataDirFromCommandLine(peb, dir, 512)) st = LuupBoot(dir, RecoveryBoot(), Say);
    NtTerminateProcess((HANDLE)-1, st);
}
