/* rr_config.c - Persistent overlay settings.
 *
 * Settings live in %APPDATA%\RobRun\RobRun.ini. A RobRun.ini beside the
 * executable takes precedence, which makes portable use possible without a
 * second configuration format. Writes are atomic: a temporary file is
 * flushed and then renamed over the destination.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "menu_bridge.h"

#define RR_CONFIG_DIR  L"RobRun"
#define RR_CONFIG_FILE L"RobRun.ini"

typedef struct {
    const char* key;
    size_t offset;
    int minimum, maximum;
} CFG_INT;

#define CFG_FIELD(field, initial, minimum, maximum) \
    { #field, offsetof(BR_Config, field), minimum, maximum },
static const CFG_INT g_cfg_ints[] = { BR_SETTINGS(CFG_FIELD) };
#undef CFG_FIELD

static BR_Config g_config;
static volatile LONG g_config_ready = 0;

static void config_defaults(BR_Config* config) {
    memset(config, 0, sizeof(*config));
    config->magic = BR_MAGIC;
    config->version = BR_VERSION;
#define BR_DEFAULT(field, initial, minimum, maximum) config->field = initial;
    BR_SETTINGS(BR_DEFAULT)
#undef BR_DEFAULT
    config->menu_open = 0;

    static const unsigned char palette[BR_COLOR_COUNT][3] = {
        { 86,168,255 }, { 255, 86, 86 }, { 120,255,140 }, { 86,255,246 },
        { 158,158,158 }, { 255,255,255 }, { 255,255,255 }, { 255,255,255 },
        { 255,255,255 }, { 255,255,255 }, { 255,255,255 }, { 120,255,140 },
        { 255,176, 66 }, { 139, 92,246 }, { 220,220,220 },
    };
    memcpy(config->color, palette, sizeof(palette));
}

BR_Config* br_get(void) {
    if (InterlockedCompareExchange(&g_config_ready, 1, 0) == 0)
        config_defaults(&g_config);
    return &g_config;
}

int br_config_set(BR_Config* config, const char* key, int value) {
    if (!config || !key) return 0;
    for (size_t i = 0; i < sizeof(g_cfg_ints) / sizeof(g_cfg_ints[0]); ++i) {
        const CFG_INT* field = &g_cfg_ints[i];
        if (strcmp(key, field->key) != 0) continue;
        if (value < field->minimum) value = field->minimum;
        if (value > field->maximum) value = field->maximum;
        *(int*)((char*)config + field->offset) = value;
        return 1;
    }
    return 0;
}

static int exe_directory(wchar_t* out, size_t capacity) {
    DWORD length = GetModuleFileNameW(NULL, out, (DWORD)capacity);
    if (!length || length >= capacity) return 0;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return 0;
    slash[1] = 0;
    return 1;
}

static int join_path(wchar_t* out, size_t capacity, const wchar_t* directory,
                     const wchar_t* name) {
    int written = swprintf_s(out, capacity, L"%ls%ls", directory, name);
    return written >= 0;
}

static int config_path(wchar_t* path, size_t capacity) {
    wchar_t directory[MAX_PATH];
    wchar_t appdata[MAX_PATH];

    /* Portable configuration wins when the user placed one beside the
     * executable. */
    if (exe_directory(directory, _countof(directory))) {
        if (join_path(path, capacity, directory, RR_CONFIG_FILE) &&
            GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }

    DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, _countof(appdata));
    if (length && length < _countof(appdata)) {
        if (!join_path(directory, _countof(directory), appdata, L"\\" RR_CONFIG_DIR))
            return 0;
        CreateDirectoryW(directory, NULL);
        if (join_path(path, capacity, directory, L"\\" RR_CONFIG_FILE))
            return 1;
    }

    /* Last resort: keep the settings beside the executable. */
    if (exe_directory(directory, _countof(directory)) &&
        join_path(path, capacity, directory, RR_CONFIG_FILE))
        return 1;
    return 0;
}

static int config_save_at(const wchar_t* path, const BR_Config* config) {
    wchar_t temp[MAX_PATH + 32];
    if (swprintf_s(temp, _countof(temp), L"%ls.%lu.tmp", path, GetCurrentProcessId()) < 0)
        return 0;

    FILE* file = _wfopen(temp, L"w");
    if (!file) return 0;

    fprintf(file, "# RobRun settings\n");
    for (size_t i = 0; i < sizeof(g_cfg_ints) / sizeof(g_cfg_ints[0]); ++i) {
        const CFG_INT* field = &g_cfg_ints[i];
        int value = *(const int*)((const char*)config + field->offset);
        fprintf(file, "%s=%d\n", field->key, value);
    }
    for (int i = 0; i < BR_COLOR_COUNT; ++i) {
        fprintf(file, "color%d=%u,%u,%u,0\n", i,
                (unsigned)config->color[i].rgb[0],
                (unsigned)config->color[i].rgb[1],
                (unsigned)config->color[i].rgb[2]);
    }

    int ok = !ferror(file);
    if (fclose(file) != 0) ok = 0;
    if (ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 1;
    DeleteFileW(temp);
    return 0;
}

static int config_load_at(const wchar_t* path, BR_Config* config) {
    FILE* file = _wfopen(path, L"r");
    if (!file) return 0;

    BR_Config loaded = *config;
    int legacy_visible = -1, legacy_head = -1;
    int have_los = 0, have_bone = 0;
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        char* newline = strpbrk(line, "\r\n");
        if (newline) *newline = 0;
        if (!line[0] || line[0] == '#') continue;

        char* value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;

        if (!strncmp(line, "color", 5)) {
            char* end;
            long index = strtol(line + 5, &end, 10);
            int red, green, blue;
            if (end != line + 5 && !*end && index >= 0 && index < BR_COLOR_COUNT &&
                sscanf(value, "%d,%d,%d", &red, &green, &blue) == 3 &&
                (unsigned)red <= 255 && (unsigned)green <= 255 && (unsigned)blue <= 255) {
                loaded.color[index].rgb[0] = (unsigned char)red;
                loaded.color[index].rgb[1] = (unsigned char)green;
                loaded.color[index].rgb[2] = (unsigned char)blue;
            }
            continue;
        }

        char* end;
        long number = strtol(value, &end, 10);
        if (end == value || *end) continue;

        /* Settings renamed after the first public build. */
        if (!strcmp(line, "aa_visible") && (number == 0 || number == 1))
            legacy_visible = (int)number;
        else if (!strcmp(line, "aa_head") && (number == 0 || number == 1))
            legacy_head = (int)number;
        else if (br_config_set(&loaded, line, (int)number)) {
            if (!strcmp(line, "aa_los_on")) have_los = 1;
            if (!strcmp(line, "aa_bone")) have_bone = 1;
        }
    }

    int ok = !ferror(file);
    fclose(file);
    if (!ok) return 0;

    if (!have_los && legacy_visible >= 0) loaded.aa_los_on = legacy_visible;
    if (!have_bone && legacy_head >= 0) loaded.aa_bone = legacy_head ? 0 : 1;
#define BR_APPLY(field, initial, minimum, maximum) config->field = loaded.field;
    BR_SETTINGS(BR_APPLY)
#undef BR_APPLY
    memcpy(config->color, loaded.color, sizeof(config->color));
    return 1;
}

int br_config_save(void) {
    wchar_t path[MAX_PATH];
    return config_path(path, _countof(path)) && config_save_at(path, br_get());
}

int br_config_load(void) {
    wchar_t path[MAX_PATH];
    return config_path(path, _countof(path)) && config_load_at(path, br_get());
}
