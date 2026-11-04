#include "regplan.h"

#define REG_BAD_RECORD ((NTSTATUS)0xC000007BL)
#define REG_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define REG_PATH_NOT_FOUND ((NTSTATUS)0xC000003AL)
#define REG_CONFLICT ((NTSTATUS)0xC0000035L)
#define REG_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define REG_BUFFER_SMALL ((NTSTATUS)0xC0000023L)
#define REG_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)

NTSYSAPI NTSTATUS NTAPI NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSYSAPI NTSTATUS NTAPI NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
NTSYSAPI NTSTATUS NTAPI NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
NTSYSAPI NTSTATUS NTAPI NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
NTSYSAPI NTSTATUS NTAPI NtDeleteValueKey(HANDLE, PUNICODE_STRING);
NTSYSAPI NTSTATUS NTAPI NtDeleteKey(HANDLE);
NTSYSAPI NTSTATUS NTAPI NtQueryKey(HANDLE, ULONG, PVOID, ULONG, PULONG);
NTSYSAPI NTSTATUS NTAPI NtFlushKey(HANDLE);
NTSYSAPI BOOLEAN NTAPI RtlEqualUnicodeString(PCUNICODE_STRING, PCUNICODE_STRING, BOOLEAN);

typedef struct {
    ULONG TitleIndex, Type, DataLength;
    UCHAR Data[1];
} REG_PARTIAL_VALUE;

typedef struct {
    LARGE_INTEGER LastWriteTime;
    ULONG TitleIndex, ClassOffset, ClassLength, SubKeys, MaxNameLen, MaxClassLen, Values, MaxValueNameLen, MaxValueDataLen;
    WCHAR Class[1];
} REG_FULL_KEY;

static PWSTR KeyOf(LUUP_REG_RECORD* record) { return (PWSTR)(record + 1); }
static PWSTR NameOf(LUUP_REG_RECORD* record) { return KeyOf(record) + record->KeyChars + 1; }
static UCHAR* DataOf(LUUP_REG_RECORD* record) { return (UCHAR*)(NameOf(record) + record->NameChars + 1); }

static BOOLEAN EqualBytes(const UCHAR* a, const UCHAR* b, ULONG size) {
    ULONG i;
    for (i = 0; i < size; i++) if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static BOOLEAN EqualNames(PCWSTR a, PCWSTR b) {
    UNICODE_STRING left, right;
    RtlInitUnicodeString(&left, a);
    RtlInitUnicodeString(&right, b);
    return RtlEqualUnicodeString(&left, &right, TRUE);
}

static BOOLEAN ValidRecord(LUUP_REG_RECORD* record, ULONG size) {
    ULONGLONG expected;
    ULONG i;
    PWSTR key, name;
    if (size < sizeof(*record) || record->Magic != LUUP_REG_MAGIC ||
        (record->Kind != LUUP_REG_VALUE && record->Kind != LUUP_REG_KEY) ||
        !record->KeyChars || record->KeyChars > 32760 || record->NameChars > 16383 ||
        record->Present > 1 || record->DataBytes > LUUP_REG_MAX_DATA) return FALSE;
    expected = sizeof(*record) + ((ULONGLONG)record->KeyChars + record->NameChars + 2) * sizeof(WCHAR) + record->DataBytes;
    if (expected != size) return FALSE;
    key = KeyOf(record);
    name = NameOf(record);
    if (key[0] != L'\\' || key[record->KeyChars - 1] == L'\\' || key[record->KeyChars] || name[record->NameChars]) return FALSE;
    for (i = 0; i < record->KeyChars; i++) if (!key[i] || (i && key[i] == L'\\' && key[i - 1] == L'\\')) return FALSE;
    for (i = 0; i < record->NameChars; i++) if (!name[i]) return FALSE;
    if (!record->Present && (record->Type || record->DataBytes)) return FALSE;
    if (record->Kind == LUUP_REG_KEY && (record->NameChars || record->Type || record->DataBytes)) return FALSE;
    if (record->Type > REG_QWORD || record->Type == REG_LINK) return FALSE;
    return TRUE;
}

static NTSTATUS OpenKey(PCWSTR key, ACCESS_MASK access, HANDLE* handle) {
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    RtlInitUnicodeString(&name, key);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtOpenKey(handle, access, &attributes);
}

#ifndef LUUP_REGISTRY_TESTING
static BOOLEAN RegistryReady(void) {
    HANDLE key = NULL;
    UNICODE_STRING name;
    ULONG size = 0;
    UCHAR data[FIELD_OFFSET(REG_PARTIAL_VALUE, Data) + sizeof(ULONG)];
    REG_PARTIAL_VALUE* value = (REG_PARTIAL_VALUE*)data;
    BOOLEAN ready = FALSE;
    if (!NT_SUCCESS(OpenKey(L"\\Registry\\Machine\\HARDWARE\\LiberNTUpdate", KEY_QUERY_VALUE, &key))) return FALSE;
    RtlInitUnicodeString(&name, L"Version");
    if (NT_SUCCESS(NtQueryValueKey(key, &name, 2, data, sizeof(data), &size)) &&
        value->Type == REG_DWORD && value->DataLength == sizeof(ULONG) && *(ULONG*)value->Data == 1) ready = TRUE;
    NtClose(key);
    return ready;
}
#endif

#ifdef LUUP_REGISTRY_TESTING
static ULONG FlushFailures;
void LuupSetRegistryFlushFailures(ULONG count) { FlushFailures = count; }
#endif

static NTSTATUS FlushKey(HANDLE key) {
#ifdef LUUP_REGISTRY_TESTING
    if (FlushFailures) { FlushFailures--; return (NTSTATUS)0xC0000185L; }
#endif
    return NtFlushKey(key);
}

static NTSTATUS FlushParent(PWSTR path, ULONG chars) {
    HANDLE parent = NULL;
    NTSTATUS status;
    WCHAR saved;
    do {
        while (chars && path[chars - 1] != L'\\') chars--;
        if (chars <= 1) return REG_PATH_NOT_FOUND;
        chars--;
        saved = path[chars];
        path[chars] = 0;
        status = OpenKey(path, KEY_QUERY_VALUE, &parent);
        path[chars] = saved;
        if (NT_SUCCESS(status)) {
            status = FlushKey(parent);
            NtClose(parent);
            return status;
        }
    } while (status == REG_NOT_FOUND || status == REG_PATH_NOT_FOUND);
    return status;
}

static NTSTATUS CreateKey(PWSTR path, ULONG chars, ACCESS_MASK access, HANDLE* handle) {
    ULONG parentEnd = chars;
    HANDLE parent = NULL;
    NTSTATUS status;
    WCHAR saved;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    while (parentEnd && path[parentEnd - 1] != L'\\') parentEnd--;
    if (parentEnd <= 1) return REG_PATH_NOT_FOUND;
    parentEnd--;
    saved = path[parentEnd];
    path[parentEnd] = 0;
    status = OpenKey(path, KEY_CREATE_SUB_KEY, &parent);
    path[parentEnd] = saved;
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, path + parentEnd + 1);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, parent, NULL);
    status = NtCreateKey(handle, access, &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    NtClose(parent);
    return status;
}

static NTSTATUS WritableKey(PWSTR path, ULONG chars, ACCESS_MASK access, HANDLE* handle) {
    HANDLE writable = NULL;
    NTSTATUS status;
    if (!*handle) return CreateKey(path, chars, access, handle);
    status = OpenKey(path, access, &writable);
    if (NT_SUCCESS(status)) {
        NtClose(*handle);
        *handle = writable;
    }
    return status;
}

static BOOLEAN SameValue(REG_PARTIAL_VALUE* actual, LUUP_REG_RECORD* expected) {
    if (!actual) return !expected->Present;
    return expected->Present && actual->Type == expected->Type && actual->DataLength == expected->DataBytes &&
           EqualBytes(actual->Data, DataOf(expected), expected->DataBytes);
}

NTSTATUS LuupApplyRegistry(PCWSTR desiredRecordNtPath, PCWSTR expectedRecordNtPath) {
    LUUP_REG_RECORD *desired = NULL, *expected = NULL;
    REG_PARTIAL_VALUE* actual = NULL;
    ULONG desiredSize, expectedSize, needed = 0;
    HANDLE key = NULL;
    NTSTATUS status;
    BOOLEAN exists;
    UNICODE_STRING name;
    REG_FULL_KEY info;

#ifndef LUUP_REGISTRY_TESTING
    if (!RegistryReady()) return (NTSTATUS)0xc0000184L;
#endif

    status = LuupReadFile(desiredRecordNtPath, (PVOID*)&desired, &desiredSize);
    if (!NT_SUCCESS(status)) goto done;
    status = LuupReadFile(expectedRecordNtPath, (PVOID*)&expected, &expectedSize);
    if (!NT_SUCCESS(status)) goto done;
    status = REG_BAD_RECORD;
    if (!ValidRecord(desired, desiredSize) || !ValidRecord(expected, expectedSize) || desired->Kind != expected->Kind ||
        !EqualNames(KeyOf(desired), KeyOf(expected)) || !EqualNames(NameOf(desired), NameOf(expected))) goto done;
    status = OpenKey(KeyOf(desired), KEY_QUERY_VALUE, &key);
    exists = NT_SUCCESS(status);
    if (!exists && status != REG_NOT_FOUND && status != REG_PATH_NOT_FOUND) goto done;

    if (desired->Kind == LUUP_REG_KEY) {
        if (exists == (BOOLEAN)desired->Present) {
            status = key ? FlushKey(key) : FlushParent(KeyOf(desired), desired->KeyChars);
            goto done;
        }
        if (exists != (BOOLEAN)expected->Present) { status = REG_CONFLICT; goto done; }
        if (desired->Present) {
            status = WritableKey(KeyOf(desired), desired->KeyChars, KEY_QUERY_VALUE, &key);
            if (NT_SUCCESS(status)) status = FlushKey(key);
        } else {
            status = WritableKey(KeyOf(desired), desired->KeyChars, KEY_QUERY_VALUE | DELETE, &key);
            if (!NT_SUCCESS(status)) goto done;
            status = NtQueryKey(key, 2, &info, sizeof(info), &needed);
            if (!NT_SUCCESS(status)) goto done;
            if (info.SubKeys || info.Values) { status = REG_CONFLICT; goto done; }
            status = NtDeleteKey(key);
            if (NT_SUCCESS(status)) status = FlushParent(KeyOf(desired), desired->KeyChars);
        }
        goto done;
    }

    RtlInitUnicodeString(&name, NameOf(desired));
    if (exists) {
        status = NtQueryValueKey(key, &name, 2, NULL, 0, &needed);
        if (status != REG_NOT_FOUND) {
            if (status != REG_BUFFER_SMALL && status != REG_BUFFER_OVERFLOW) goto done;
            if (needed > LUUP_REG_MAX_DATA + FIELD_OFFSET(REG_PARTIAL_VALUE, Data)) { status = REG_CONFLICT; goto done; }
            actual = (REG_PARTIAL_VALUE*)LuupAlloc(needed);
            if (!actual) { status = REG_NO_MEMORY; goto done; }
            status = NtQueryValueKey(key, &name, 2, actual, needed, &needed);
            if (!NT_SUCCESS(status)) goto done;
        }
    }
    if (SameValue(actual, desired)) {
        status = key ? FlushKey(key) : FlushParent(KeyOf(desired), desired->KeyChars);
        goto done;
    }
    if (!SameValue(actual, expected)) { status = REG_CONFLICT; goto done; }
    if (!key) { status = REG_PATH_NOT_FOUND; goto done; }
    status = WritableKey(KeyOf(desired), desired->KeyChars, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (!NT_SUCCESS(status)) goto done;
    status = desired->Present ? NtSetValueKey(key, &name, 0, desired->Type, DataOf(desired), desired->DataBytes)
                              : NtDeleteValueKey(key, &name);
    if (NT_SUCCESS(status)) status = FlushKey(key);
done:
    if (key) NtClose(key);
    LuupFree(actual);
    LuupFree(desired);
    LuupFree(expected);
    return status;
}
