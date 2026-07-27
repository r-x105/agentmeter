#include "secrets.h"
#include <Preferences.h>
#include <freertos/semphr.h>

#define NVS_NS "agentmeter"

// Small fixed cache: id.suffix -> value. Sized for the compiled-in provider
// set (each provider uses at most 6 keys).
#define MAX_ENTRIES 24

typedef struct {
    char   key[16];   // "openai.rt" — NVS key, <=15 chars + NUL
    String value;
    bool   dirty;     // RAM differs from NVS; secrets_flush() writes it
    bool   erased;    // flush should remove the key instead of writing
} entry_t;

static entry_t          s_entries[MAX_ENTRIES];
static int              s_count = 0;
static SemaphoreHandle_t s_mutex = nullptr;

static void make_key(char* out, size_t n, const char* id, const char* suffix) {
    snprintf(out, n, "%s.%s", id, suffix);
}

static int find_entry(const char* key) {
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_entries[i].key, key) == 0) return i;
    return -1;
}

// Cache a key from NVS if present (called at init, holding no lock yet).
static void preload(Preferences& prefs, const char* key) {
    if (!prefs.isKey(key)) return;
    if (s_count >= MAX_ENTRIES) return;
    strlcpy(s_entries[s_count].key, key, sizeof(s_entries[s_count].key));
    s_entries[s_count].value = prefs.getString(key, "");
    s_count++;
}

void secrets_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    Preferences prefs;
    prefs.begin(NVS_NS, false);

    // One-time migration: a device previously flashed with clawdmeter-wifi
    // has its Anthropic token at clawdmeter/token. Adopt it as anthropic.tk.
    if (!prefs.isKey("anthropic.tk")) {
        Preferences old;
        old.begin("clawdmeter", true);
        String legacy = old.getString("token", "");
        old.end();
        if (legacy.length()) {
            prefs.putString("anthropic.tk", legacy);
            Serial.println("secrets: migrated legacy clawdmeter token -> anthropic.tk");
        }
    }

    // Preload every key the compiled-in providers could own. Unknown ids are
    // impossible here because ids come from the static registry.
    static const char* IDS[]      = { "anthropic", "openai", "openrouter" };
    static const char* SUFFIXES[] = { "tk", "rt", "at", "pr", "ex", "en" };
    for (auto id : IDS) {
        for (auto sfx : SUFFIXES) {
            char key[16];
            make_key(key, sizeof(key), id, sfx);
            preload(prefs, key);
        }
    }
    prefs.end();
    Serial.printf("secrets: %d keys loaded\n", s_count);
}

String secrets_get(const char* id, const char* suffix) {
    char key[16];
    make_key(key, sizeof(key), id, suffix);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int i = find_entry(key);
    String r = (i >= 0) ? s_entries[i].value : String("");
    xSemaphoreGive(s_mutex);
    return r;
}

bool secrets_has(const char* id, const char* suffix) {
    return secrets_get(id, suffix).length() > 0;
}

// Cache-first write: readers (any task) see the value immediately; the NVS
// write happens in secrets_flush() on the main loop. NEVER write NVS here —
// this runs on the poll task, whose PSRAM stack asserts inside flash ops.
void secrets_set(const char* id, const char* suffix, const char* value) {
    char key[16];
    make_key(key, sizeof(key), id, suffix);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int i = find_entry(key);
    if (i < 0 && s_count < MAX_ENTRIES) {
        i = s_count++;
        strlcpy(s_entries[i].key, key, sizeof(s_entries[i].key));
    }
    if (i >= 0) {
        s_entries[i].value  = value;
        s_entries[i].dirty  = true;
        s_entries[i].erased = false;
    }
    xSemaphoreGive(s_mutex);
    Serial.printf("secrets: %s queued\n", key);
}

void secrets_erase(const char* id, const char* suffix) {
    char key[16];
    make_key(key, sizeof(key), id, suffix);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int i = find_entry(key);
    if (i >= 0) {
        s_entries[i].value  = "";
        s_entries[i].dirty  = true;
        s_entries[i].erased = true;
    }
    xSemaphoreGive(s_mutex);
}

// Main loop only (internal stack). Writes every dirty entry to NVS.
void secrets_flush(void) {
    // Cheap unlocked peek — dirty entries are rare (token rotation, config
    // saves); skip opening NVS on the hot path.
    bool any = false;
    for (int i = 0; i < s_count; i++) if (s_entries[i].dirty) { any = true; break; }
    if (!any) return;

    Preferences prefs;
    prefs.begin(NVS_NS, false);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (!s_entries[i].dirty) continue;
        if (s_entries[i].erased) prefs.remove(s_entries[i].key);
        else                     prefs.putString(s_entries[i].key, s_entries[i].value);
        s_entries[i].dirty = false;
        Serial.printf("secrets: %s flushed\n", s_entries[i].key);
    }
    xSemaphoreGive(s_mutex);
    prefs.end();
}

bool secrets_enabled(const char* id) {
    String en = secrets_get(id, "en");
    if (en.length()) return en == "1";
    // Default: enabled when the provider has a primary credential.
    return secrets_has(id, "tk") || secrets_has(id, "rt");
}

void secrets_set_enabled(const char* id, bool on) {
    secrets_set(id, "en", on ? "1" : "0");
}
