// NVS in a file: every entry (namespace, key, type, bytes) in memory, written out whole after each change. Opening a
// namespace that holds nothing read-only fails with NOT_FOUND, and lengths behave as on the board, since the
// firmware's code leans on both.
#include <pthread.h>
#include <sys/stat.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "sim_port.h"

typedef struct { char ns[16], key[16]; nvs_type_t type; uint32_t len; uint8_t *data; } entry_t;

static entry_t *ents;
static int n_ents, cap_ents;
static char handles[64][16];                          // handle (index + 1) -> its namespace
static int n_handles;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool loaded;

static void save(void)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", sim_nvs_path());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite("BNVS1", 1, 5, f);
    for (int i = 0; i < n_ents; i++) {
        entry_t *e = &ents[i];
        uint8_t t = e->type;
        fwrite(e->ns, 1, 16, f);
        fwrite(e->key, 1, 16, f);
        fwrite(&t, 1, 1, f);
        fwrite(&e->len, 4, 1, f);
        fwrite(e->data, 1, e->len, f);
    }
    fclose(f);
    rename(tmp, sim_nvs_path());
}

static entry_t *add(const char *ns, const char *key)
{
    if (n_ents == cap_ents) {
        cap_ents = cap_ents ? cap_ents * 2 : 64;
        ents = realloc(ents, cap_ents * sizeof *ents);
    }
    entry_t *e = &ents[n_ents++];
    memset(e, 0, sizeof *e);
    strlcpy(e->ns, ns, sizeof e->ns);
    strlcpy(e->key, key, sizeof e->key);
    return e;
}

static void load(void)
{
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(sim_nvs_path(), "rb");
    char magic[5];
    if (!f) return;
    if (fread(magic, 1, 5, f) == 5 && !memcmp(magic, "BNVS1", 5)) {
        char ns[16], key[16];
        uint8_t t;
        uint32_t len;
        while (fread(ns, 1, 16, f) == 16 && fread(key, 1, 16, f) == 16 && fread(&t, 1, 1, f) == 1 && fread(&len, 4, 1, f) == 1) {
            ns[15] = key[15] = 0;
            entry_t *e = add(ns, key);
            e->type = t;
            e->len = len;
            e->data = malloc(len ? len : 1);
            if (fread(e->data, 1, len, f) != len) { n_ents--; break; }
        }
    }
    fclose(f);
}

static entry_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < n_ents; i++)
        if (!strcmp(ents[i].ns, ns) && !strcmp(ents[i].key, key)) return &ents[i];
    return NULL;
}

static const char *ns_of(nvs_handle_t h) { return h >= 1 && h <= (nvs_handle_t)n_handles ? handles[h - 1] : NULL; }

esp_err_t nvs_flash_init(void)
{
    pthread_mutex_lock(&lock);
    load();
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n_ents; i++) free(ents[i].data);
    n_ents = 0;
    save();
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

esp_err_t nvs_flash_deinit(void) { return ESP_OK; }

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    pthread_mutex_lock(&lock);
    load();
    bool any = false;
    for (int i = 0; i < n_ents && !any; i++) any = !strcmp(ents[i].ns, ns);
    esp_err_t r = ESP_OK;
    if (mode == NVS_READONLY && !any) r = ESP_ERR_NVS_NOT_FOUND;   // (as on the board: nothing saved there yet)
    else {
        int k = -1;
        for (int i = 0; i < n_handles; i++) if (!strcmp(handles[i], ns)) k = i;
        if (k < 0 && n_handles < 64) { k = n_handles++; strlcpy(handles[k], ns, 16); }
        *out = k + 1;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

void nvs_close(nvs_handle_t h) {}
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }

static esp_err_t put(nvs_handle_t h, const char *key, nvs_type_t type, const void *v, size_t len)
{
    pthread_mutex_lock(&lock);
    const char *ns = ns_of(h);
    if (!ns) { pthread_mutex_unlock(&lock); return ESP_ERR_INVALID_ARG; }
    entry_t *e = find(ns, key);
    if (!e) e = add(ns, key);
    free(e->data);
    e->type = type;
    e->len = (uint32_t)len;
    e->data = malloc(len ? len : 1);
    memcpy(e->data, v, len);
    save();
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

// Copy out an entry of this type: out NULL asks just the length; too small a buffer is an error (as on the board)
static esp_err_t get(nvs_handle_t h, const char *key, nvs_type_t type, void *out, size_t *len, bool exact)
{
    pthread_mutex_lock(&lock);
    const char *ns = ns_of(h);
    entry_t *e = ns ? find(ns, key) : NULL;
    esp_err_t r = ESP_OK;
    if (!e || e->type != type) r = ESP_ERR_NVS_NOT_FOUND;
    else if (exact) memcpy(out, e->data, e->len);
    else if (!out) *len = e->len;
    else if (*len < e->len) { *len = e->len; r = ESP_ERR_NVS_INVALID_LENGTH; }
    else { memcpy(out, e->data, e->len); *len = e->len; }
    pthread_mutex_unlock(&lock);
    return r;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { return put(h, key, NVS_TYPE_STR, v, strlen(v) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) { return get(h, key, NVS_TYPE_STR, out, len, false); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len) { return put(h, key, NVS_TYPE_BLOB, v, len); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) { return get(h, key, NVS_TYPE_BLOB, out, len, false); }

#define INT_ACCESSORS(T, n, TYPE) \
    esp_err_t nvs_set_##n(nvs_handle_t h, const char *key, T v) { return put(h, key, TYPE, &v, sizeof v); } \
    esp_err_t nvs_get_##n(nvs_handle_t h, const char *key, T *out) { return get(h, key, TYPE, out, NULL, true); }
INT_ACCESSORS(uint8_t, u8, NVS_TYPE_U8)
INT_ACCESSORS(int8_t, i8, NVS_TYPE_I8)
INT_ACCESSORS(uint16_t, u16, NVS_TYPE_U16)
INT_ACCESSORS(int16_t, i16, NVS_TYPE_I16)
INT_ACCESSORS(uint32_t, u32, NVS_TYPE_U32)
INT_ACCESSORS(int32_t, i32, NVS_TYPE_I32)
INT_ACCESSORS(uint64_t, u64, NVS_TYPE_U64)
INT_ACCESSORS(int64_t, i64, NVS_TYPE_I64)

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    pthread_mutex_lock(&lock);
    const char *ns = ns_of(h);
    entry_t *e = ns ? find(ns, key) : NULL;
    if (e) {
        free(e->data);
        *e = ents[--n_ents];
        save();
    }
    pthread_mutex_unlock(&lock);
    return e ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    pthread_mutex_lock(&lock);
    const char *ns = ns_of(h);
    for (int i = 0; ns && i < n_ents;) {
        if (strcmp(ents[i].ns, ns)) { i++; continue; }
        free(ents[i].data);
        ents[i] = ents[--n_ents];
    }
    save();
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

// ---- walking the entries (a snapshot, taken when the walk starts)
struct sim_nvs_it { nvs_entry_info_t *list; int n, at; };

esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *out)
{
    pthread_mutex_lock(&lock);
    load();
    struct sim_nvs_it *it = calloc(1, sizeof *it);
    it->list = calloc(n_ents ? n_ents : 1, sizeof *it->list);
    for (int i = 0; i < n_ents; i++) {
        if ((ns && strcmp(ents[i].ns, ns)) || (type != NVS_TYPE_ANY && ents[i].type != type)) continue;
        nvs_entry_info_t *x = &it->list[it->n++];
        strlcpy(x->namespace_name, ents[i].ns, sizeof x->namespace_name);
        strlcpy(x->key, ents[i].key, sizeof x->key);
        x->type = ents[i].type;
    }
    pthread_mutex_unlock(&lock);
    if (!it->n) { nvs_release_iterator(it); *out = NULL; return ESP_ERR_NVS_NOT_FOUND; }
    *out = it;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    if (!*it) return ESP_ERR_INVALID_ARG;
    if (++(*it)->at >= (*it)->n) { nvs_release_iterator(*it); *it = NULL; return ESP_ERR_NVS_NOT_FOUND; }
    return ESP_OK;
}

esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *out)
{
    if (!it) return ESP_ERR_INVALID_ARG;
    *out = it->list[it->at];
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t it)
{
    if (!it) return;
    free(it->list);
    free(it);
}

bool sim_nvs_exists(void)
{
    struct stat st;
    return stat(sim_nvs_path(), &st) == 0;
}
