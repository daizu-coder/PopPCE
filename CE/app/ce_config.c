/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * File-based settings storage (see ce_config.h). A small in-memory
 * key/int table, loaded from and saved back to a single flat text file -
 * simple line-based "key=value" parsing, no need for anything fancier
 * given the whole table is at most a few dozen small integers.
 */
#include "ce_config.h"
#include "ce_log.h"
#include "ce_app_config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CE_CONFIG_MAX_ENTRIES 48
#define CE_CONFIG_KEY_LEN     32
/* Wide enough for a remembered folder path (ce_fileopen.c's
 * "OpenLastFolderDir", UTF-8-encoded) as well as every plain integer
 * value every other module stores here. */
#define CE_CONFIG_VALUE_LEN   260

typedef struct
{
    char key[CE_CONFIG_KEY_LEN];
    char value[CE_CONFIG_VALUE_LEN]; /* text on disk either way - CeConfigGetInt/SetInt are atoi()/snprintf() wrappers around this */
} CeConfigEntry;

static CeConfigEntry s_entries[CE_CONFIG_MAX_ENTRIES];
static int s_entryCount = 0;

static void GetConfigPath(wchar_t *outPath, size_t outPathCount)
{
    wchar_t exePath[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    slash = wcsrchr(exePath, L'\\');
    if (slash)
        *slash = L'\0';
    _snwprintf(outPath, outPathCount, L"%s\\" CE_APP_CONFIG_FILENAME, exePath);
}

static CeConfigEntry *FindEntry(const char *key)
{
    int i;
    for (i = 0; i < s_entryCount; i++)
        if (strcmp(s_entries[i].key, key) == 0)
            return &s_entries[i];
    return NULL;
}

void CeConfigLoad(void)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char line[CE_CONFIG_KEY_LEN + CE_CONFIG_VALUE_LEN + 4];

    s_entryCount = 0;
    GetConfigPath(path, MAX_PATH);

    f = _wfopen(path, L"r");
    if (!f)
    {
        CeLog("CeConfigLoad: no config file yet, using defaults");
        return;
    }

    while (fgets(line, sizeof(line), f) && s_entryCount < CE_CONFIG_MAX_ENTRIES)
    {
        char *eq = strchr(line, '=');
        char *eol;

        if (!eq)
            continue;
        *eq = '\0';

        eol = strchr(eq + 1, '\r');
        if (eol) *eol = '\0';
        eol = strchr(eq + 1, '\n');
        if (eol) *eol = '\0';

        _snprintf(s_entries[s_entryCount].key, CE_CONFIG_KEY_LEN, "%s", line);
        s_entries[s_entryCount].key[CE_CONFIG_KEY_LEN - 1] = '\0';
        _snprintf(s_entries[s_entryCount].value, CE_CONFIG_VALUE_LEN, "%s", eq + 1);
        s_entries[s_entryCount].value[CE_CONFIG_VALUE_LEN - 1] = '\0';
        s_entryCount++;
    }

    fclose(f);
    CeLog("CeConfigLoad: loaded %d setting(s) from config file", s_entryCount);
}

void CeConfigGetString(const char *key, char *outValue, size_t outValueCount)
{
    CeConfigEntry *e = FindEntry(key);
    if (!e)
        return; /* leave the caller's own default untouched */
    _snprintf(outValue, outValueCount, "%s", e->value);
    outValue[outValueCount - 1] = '\0';
}

void CeConfigSetString(const char *key, const char *value)
{
    CeConfigEntry *e = FindEntry(key);

    if (!e)
    {
        if (s_entryCount >= CE_CONFIG_MAX_ENTRIES)
        {
            CeLog("CeConfigSetString: table full (%d entries), dropping key=%s", CE_CONFIG_MAX_ENTRIES, key);
            return;
        }
        e = &s_entries[s_entryCount++];
        strncpy(e->key, key, CE_CONFIG_KEY_LEN - 1);
        e->key[CE_CONFIG_KEY_LEN - 1] = '\0';
    }

    _snprintf(e->value, CE_CONFIG_VALUE_LEN, "%s", value);
    e->value[CE_CONFIG_VALUE_LEN - 1] = '\0';
}

int CeConfigGetInt(const char *key, int defaultValue)
{
    CeConfigEntry *e = FindEntry(key);
    return e ? atoi(e->value) : defaultValue;
}

void CeConfigSetInt(const char *key, int value)
{
    char buf[16];
    _snprintf(buf, sizeof(buf), "%d", value);
    buf[sizeof(buf) - 1] = '\0';
    CeConfigSetString(key, buf);
}

void CeConfigSave(void)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    int i;

    GetConfigPath(path, MAX_PATH);
    f = _wfopen(path, L"w");
    if (!f)
    {
        CeLog("CeConfigSave: failed to open config file for write");
        return;
    }

    for (i = 0; i < s_entryCount; i++)
        fprintf(f, "%s=%s\n", s_entries[i].key, s_entries[i].value);

    fclose(f);
    CeLog("CeConfigSave: saved %d setting(s) to config file", s_entryCount);
}
