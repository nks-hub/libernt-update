#pragma once
#include "bootplan.h"

#define LUUP_REG_MAGIC 0x3152554cUL
#define LUUP_REG_VALUE 1UL
#define LUUP_REG_KEY 2UL
#define LUUP_REG_MAX_DATA (1024UL * 1024UL)

typedef struct LUUP_REG_RECORD {
    ULONG Magic;
    ULONG Kind;
    ULONG KeyChars;
    ULONG NameChars;
    ULONG Present;
    ULONG Type;
    ULONG DataBytes;
} LUUP_REG_RECORD;

/* The header is followed by NUL-terminated UTF-16 key/name strings and DataBytes raw bytes. */

#ifdef __cplusplus
extern "C" {
#endif
NTSTATUS LuupApplyRegistry(PCWSTR desiredRecordNtPath, PCWSTR expectedRecordNtPath);
#ifdef LUUP_REGISTRY_TESTING
void LuupSetRegistryFlushFailures(ULONG count);
#endif
#ifdef __cplusplus
}
#endif
