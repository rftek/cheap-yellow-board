#include "ld_sounds.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "kit.h"
#include "music_out.h"
#include "sdcard.h"
#include "wav.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* WAVs are scaled to this peak, as the kits' sounds are, and mixed a
   little below them. */
#define WAV_PEAK 0.89f
#define WAV_GAIN 0.75f
#define END_FADE_FRAMES (LD_RATE / 200)
#define BANK_MAGIC "LIVEDRUM 1"
#define BANK_LINE_MAX 200

static const char *TAG = "ld_sounds";

static ld_source_t sources[LD_PADS];
static int16_t *owned[LD_PADS]; /* the pad's copy of a WAV or synth sample */
static char names[LD_PADS][LD_NAME_MAX + 1];
static ld_synth_source_t synth_source;

void ld_sounds_set_synth(const ld_synth_source_t *synth)
{
    synth_source = *synth;
}

const ld_synth_source_t *ld_sounds_synth(void)
{
    return &synth_source;
}

const ld_source_t *ld_sounds_source(int pad)
{
    return &sources[pad < 0 ? 0 : pad >= LD_PADS ? LD_PADS - 1 : pad];
}

const char *ld_sounds_name(int pad)
{
    return names[pad < 0 ? 0 : pad >= LD_PADS ? LD_PADS - 1 : pad];
}

/* Hands the pad its new sound, then frees the copy it had, once the audio
   task has let go of it. */
static void install(int pad, const ld_sample_t *sample, int16_t *copy)
{
    ld_set_sample(pad, sample);
    free(owned[pad]);
    owned[pad] = copy;
}

void ld_sounds_clear(int pad)
{
    if (pad < 0 || pad >= LD_PADS) {
        return;
    }
    install(pad, NULL, NULL);
    memset(&sources[pad], 0, sizeof(sources[pad]));
    snprintf(names[pad], sizeof(names[pad]), "---");
}

esp_err_t ld_sounds_assign_kit(int pad, int kit, int track, char *status, size_t status_size)
{
    if (pad < 0 || pad >= LD_PADS || kit < 0 || kit >= kit_count() || track < 0 ||
        track >= DRUM_TRACKS) {
        return ESP_ERR_INVALID_ARG;
    }
    char line[48];
    if (kit_on_card(kit)) {
        esp_err_t result = kit_load(kit, line, sizeof(line));
        if (result != ESP_OK) {
            snprintf(status, status_size, "%s", line);
            return result;
        }
    }
    const drum_sound_t *sound = kit_sound(kit, track);
    ld_sample_t sample = {.pcm = sound->pcm, .frames = sound->frames, .gain = sound->gain};
    install(pad, &sample, NULL);
    sources[pad] = (ld_source_t){.kind = LD_SOURCE_KIT, .kit = (uint8_t)kit, .track = (uint8_t)track};
    snprintf(names[pad], sizeof(names[pad]), "%s %s", sound->label, kit_name(kit));
    snprintf(status, status_size, " Pad %d: %s", pad + 1, names[pad]);
    return ESP_OK;
}

/* The file's name without its folder or .wav. */
static void file_stem(const char *path, char *stem, size_t size)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    size_t length = strlen(name);
    if (length > 4 && strcasecmp(name + length - 4, ".wav") == 0) {
        length -= 4;
    }
    if (length >= size) {
        length = size - 1;
    }
    memcpy(stem, name, length);
    stem[length] = '\0';
}

esp_err_t ld_sounds_assign_wav(int pad, const char *path, char *status, size_t status_size)
{
    if (pad < 0 || pad >= LD_PADS || strlen(path) >= LD_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t *pcm = NULL;
    uint32_t frames = 0;
    esp_err_t result = wav_load(path, LD_SAMPLE_SECONDS, WAV_PEAK, &pcm, &frames);
    char stem[LD_NAME_MAX + 1];
    file_stem(path, stem, sizeof(stem));
    if (result != ESP_OK) {
        const char *why = result == ESP_ERR_NOT_FOUND       ? "can't open"
                          : result == ESP_ERR_INVALID_ARG   ? "not a WAV"
                          : result == ESP_ERR_NOT_SUPPORTED ? "unsupported"
                          : result == ESP_ERR_NO_MEM        ? "no memory"
                                                            : "no audio";
        snprintf(status, status_size, " %s: %s", stem, why);
        return result;
    }
    ld_sample_t sample = {.pcm = pcm, .frames = frames, .gain = WAV_GAIN};
    install(pad, &sample, pcm);
    sources[pad] = (ld_source_t){.kind = LD_SOURCE_WAV};
    snprintf(sources[pad].path, sizeof(sources[pad].path), "%s", path);
    snprintf(names[pad], sizeof(names[pad]), "%s", stem);
    snprintf(status, status_size, " Pad %d: %s %.1fs", pad + 1, stem, (double)frames / LD_RATE);
    return ESP_OK;
}

esp_err_t ld_sounds_assign_synth(int pad, int sample, char *status, size_t status_size)
{
    if (pad < 0 || pad >= LD_PADS || sample < 0 || sample >= synth_source.count) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t frames = 0;
    const int16_t *pcm = synth_source.pcm(sample, &frames);
    if (pcm == NULL || frames == 0) {
        snprintf(status, status_size, " S%d is empty: render one in SYNTH", sample + 1);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t limit = (uint32_t)(LD_SAMPLE_SECONDS * LD_RATE);
    if (frames > limit) {
        frames = limit;
    }
    int16_t *copy = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        snprintf(status, status_size, " No memory for S%d", sample + 1);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, pcm, frames * sizeof(int16_t));
    for (uint32_t i = 0; i < END_FADE_FRAMES && i < frames; ++i) {
        copy[frames - 1 - i] = (int16_t)(copy[frames - 1 - i] * (float)i / END_FADE_FRAMES);
    }
    ld_sample_t s = {.pcm = copy, .frames = frames, .gain = WAV_GAIN};
    install(pad, &s, copy);
    sources[pad] = (ld_source_t){.kind = LD_SOURCE_SYNTH, .synth = (uint8_t)sample};
    snprintf(names[pad], sizeof(names[pad]), "%s", synth_source.name(sample));
    snprintf(status, status_size, " Pad %d: %s", pad + 1, names[pad]);
    return ESP_OK;
}

void ld_sounds_default(void)
{
    static const struct {
        uint8_t kit, track, choke;
    } set[LD_PADS] = {
        {KIT_808, DRUM_KICK, 0},   {KIT_808, DRUM_SNARE, 0},      {KIT_808, DRUM_CLAP, 0},
        {KIT_808, DRUM_CLOSED_HAT, 1}, {KIT_808, DRUM_OPEN_HAT, 1}, {KIT_808, DRUM_TOM, 0},
        {KIT_808, DRUM_RIM, 0},    {KIT_808, DRUM_EXTRA, 0},      {KIT_909, DRUM_KICK, 0},
        {KIT_909, DRUM_SNARE, 0},  {KIT_909, DRUM_CLOSED_HAT, 2}, {KIT_909, DRUM_OPEN_HAT, 2},
        {KIT_TRAP, DRUM_KICK, 0},  {KIT_TRAP, DRUM_SNARE, 0},     {KIT_DNB, DRUM_SNARE, 0},
        {KIT_TRAP, DRUM_EXTRA, 0},
    };
    char status[48];
    for (int pad = 0; pad < LD_PADS; ++pad) {
        ld_sounds_assign_kit(pad, set[pad].kit, set[pad].track, status, sizeof(status));
        ld_pad_settings_t *settings = ld_pad(pad);
        settings->level = LD_LEVEL_DEFAULT;
        settings->pitch = 0;
        settings->choke = set[pad].choke;
    }
}

/* ---- SD card ----------------------------------------------------------------- */

static bool card_ready(void)
{
    return sdcard_mounted() || sdcard_mount() == ESP_OK;
}

static bool is_wav(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && strcasecmp(name + length - 4, ".wav") == 0;
}

static int compare_entries(const void *a, const void *b)
{
    const ld_entry_t *x = a;
    const ld_entry_t *y = b;
    if (x->folder != y->folder) {
        return x->folder ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

int ld_sounds_list(const char *folder, ld_entry_t *entries, int max)
{
    if (!card_ready()) {
        return 0;
    }
    DIR *listing = opendir(folder);
    if (listing == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(listing)) != NULL && count < max) {
        const char *name = entry->d_name;
        bool folder_entry = entry->d_type == DT_DIR;
        if (name[0] == '.' || strlen(name) >= LD_ENTRY_MAX || (!folder_entry && !is_wav(name))) {
            continue;
        }
        snprintf(entries[count].name, LD_ENTRY_MAX, "%s", name);
        entries[count].folder = folder_entry;
        ++count;
    }
    closedir(listing);
    qsort(entries, (size_t)count, sizeof(entries[0]), compare_entries);
    return count;
}

static void bank_path(int bank, char *path, size_t size)
{
    snprintf(path, size, LD_BANK_DIR "/bank%d.txt", bank + 1);
}

bool ld_bank_info(int bank, char *info, size_t info_size)
{
    if (!card_ready()) {
        return false;
    }
    char path[64];
    bank_path(bank, path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        snprintf(info, info_size, "EMPTY");
        return true;
    }
    char line[BANK_LINE_MAX];
    unsigned bpm = 0;
    unsigned bars = 1;
    int hits = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "hit ", 4) == 0) {
            ++hits;
        } else {
            sscanf(line, "bpm %u", &bpm);
            sscanf(line, "bars %u", &bars);
        }
    }
    fclose(file);
    snprintf(info, info_size, "%u BPM %uB %dH", bpm, bars, hits);
    return true;
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    put_u16(p, (uint16_t)value);
    put_u16(p + 2, (uint16_t)(value >> 16));
}

/* Writes 44100 Hz mono 16-bit PCM as a WAV. */
static esp_err_t write_wav(const char *path, const int16_t *pcm, uint32_t frames)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    uint8_t header[44];
    uint32_t data_bytes = frames * sizeof(int16_t);
    memcpy(header, "RIFF", 4);
    put_u32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_u32(header + 16, 16);
    put_u16(header + 20, 1); /* PCM */
    put_u16(header + 22, 1); /* mono */
    put_u32(header + 24, LD_RATE);
    put_u32(header + 28, LD_RATE * sizeof(int16_t));
    put_u16(header + 32, sizeof(int16_t));
    put_u16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(pcm, sizeof(int16_t), frames, file) == frames;
    ok = fclose(file) == 0 && ok;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ld_bank_save(int bank, uint16_t bpm, char *status, size_t status_size)
{
    if (!card_ready()) {
        snprintf(status, status_size, " No SD card");
        return ESP_ERR_NOT_FOUND;
    }
    if (mkdir(LD_BANK_DIR, 0777) != 0 && errno != EEXIST) {
        snprintf(status, status_size, " Can't make " LD_BANK_DIR);
        return ESP_FAIL;
    }
    /* A synth sample is saved as a WAV beside the banks, and the pad then
       plays that file. The file gets a name no other file has, and is
       never written over, since other banks may play it too. */
    for (int pad = 0; pad < LD_PADS; ++pad) {
        if (sources[pad].kind != LD_SOURCE_SYNTH || owned[pad] == NULL) {
            continue;
        }
        char path[LD_PATH_MAX];
        struct stat info;
        int number = 1;
        do {
            snprintf(path, sizeof(path), LD_BANK_DIR "/synth%03d.wav", number++);
        } while (stat(path, &info) == 0 && number < 1000);
        if (write_wav(path, owned[pad], ld_sample(pad)->frames) != ESP_OK) {
            snprintf(status, status_size, " Can't write pad %d's sample", pad + 1);
            return ESP_FAIL;
        }
        sources[pad].kind = LD_SOURCE_WAV;
        snprintf(sources[pad].path, sizeof(sources[pad].path), "%s", path);
    }

    ld_loop_hit_t *loop = heap_caps_malloc(LD_MAX_HITS * sizeof(ld_loop_hit_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (loop == NULL) {
        snprintf(status, status_size, " No memory");
        return ESP_ERR_NO_MEM;
    }
    int bars = 1;
    int count = ld_get_loop(loop, LD_MAX_HITS, &bars);

    char path[64];
    bank_path(bank, path, sizeof(path));
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        free(loop);
        snprintf(status, status_size, " Can't write %s", path + strlen(LD_BANK_DIR) + 1);
        return ESP_FAIL;
    }
    fprintf(file, BANK_MAGIC "\nbpm %u\nbars %d\n", bpm, bars);
    for (int pad = 0; pad < LD_PADS; ++pad) {
        const ld_pad_settings_t *settings = ld_pad(pad);
        fprintf(file, "pad %d %u %d %u ", pad + 1, settings->level, settings->pitch, settings->choke);
        switch (sources[pad].kind) {
        case LD_SOURCE_KIT:
            fprintf(file, "kit %u %s\n", sources[pad].track, kit_name(sources[pad].kit));
            break;
        case LD_SOURCE_WAV:
            fprintf(file, "wav %s\n", sources[pad].path);
            break;
        default:
            fprintf(file, "none\n");
            break;
        }
    }
    for (int h = 0; h < count; ++h) {
        fprintf(file, "hit %u %u %u\n", loop[h].tick, loop[h].pad + 1, loop[h].take);
    }
    free(loop);
    if (fclose(file) != 0) {
        snprintf(status, status_size, " Write failed");
        return ESP_FAIL;
    }
    snprintf(status, status_size, " Saved bank %d: %d hits", bank + 1, count);
    ESP_LOGI(TAG, "Saved %s: %d hits", path, count);
    return ESP_OK;
}

/* The kit with this name, looking on the card again if need be, or -1. */
static int find_kit(const char *name)
{
    for (int pass = 0; pass < 2; ++pass) {
        for (int kit = 0; kit < kit_count(); ++kit) {
            if (strcmp(kit_name(kit), name) == 0) {
                return kit;
            }
        }
        kit_scan(KIT_FOLDER);
    }
    return -1;
}

static void trim(char *line)
{
    size_t length = strlen(line);
    while (length > 0 && isspace((unsigned char)line[length - 1])) {
        line[--length] = '\0';
    }
}

esp_err_t ld_bank_load(int bank, uint16_t *bpm, char *status, size_t status_size)
{
    if (!card_ready()) {
        snprintf(status, status_size, " No SD card");
        return ESP_ERR_NOT_FOUND;
    }
    char path[64];
    bank_path(bank, path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        snprintf(status, status_size, " Bank %d is empty", bank + 1);
        return ESP_ERR_NOT_FOUND;
    }
    char line[BANK_LINE_MAX];
    if (fgets(line, sizeof(line), file) == NULL || strncmp(line, BANK_MAGIC, strlen(BANK_MAGIC)) != 0) {
        fclose(file);
        snprintf(status, status_size, " Bank %d isn't a LiveDrum bank", bank + 1);
        return ESP_ERR_INVALID_ARG;
    }
    ld_loop_hit_t *loop = heap_caps_malloc(LD_MAX_HITS * sizeof(ld_loop_hit_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (loop == NULL) {
        fclose(file);
        snprintf(status, status_size, " No memory");
        return ESP_ERR_NO_MEM;
    }
    int count = 0;
    int bars = 1;
    int missing = 0;
    bool seen[LD_PADS] = {false};
    char note[48];
    while (fgets(line, sizeof(line), file) != NULL) {
        trim(line);
        unsigned value;
        unsigned tick;
        unsigned pad;
        unsigned take;
        unsigned level;
        int pitch;
        unsigned choke;
        int used = 0;
        if (sscanf(line, "bpm %u", &value) == 1) {
            if (value >= MUSIC_TEMPO_MIN && value <= MUSIC_TEMPO_MAX) {
                *bpm = (uint16_t)value;
            }
        } else if (sscanf(line, "bars %u", &value) == 1) {
            bars = (int)value;
        } else if (sscanf(line, "hit %u %u %u", &tick, &pad, &take) == 3) {
            if (count < LD_MAX_HITS && pad >= 1 && pad <= LD_PADS && take < 256) {
                loop[count++] = (ld_loop_hit_t){
                    .tick = (uint16_t)tick, .pad = (uint8_t)(pad - 1), .take = (uint8_t)take};
            }
        } else if (sscanf(line, "pad %u %u %d %u %n", &pad, &level, &pitch, &choke, &used) == 4 &&
                   used > 0 && pad >= 1 && pad <= LD_PADS) {
            int p = (int)pad - 1;
            const char *source = line + used;
            unsigned track;
            int name_at = 0;
            esp_err_t result = ESP_OK;
            if (strncmp(source, "kit ", 4) == 0 &&
                sscanf(source, "kit %u %n", &track, &name_at) == 1 && name_at > 0) {
                int kit = find_kit(source + name_at);
                result = kit >= 0 ? ld_sounds_assign_kit(p, kit, (int)track, note, sizeof(note))
                                  : ESP_ERR_NOT_FOUND;
            } else if (strncmp(source, "wav ", 4) == 0) {
                result = ld_sounds_assign_wav(p, source + 4, note, sizeof(note));
            } else {
                ld_sounds_clear(p);
            }
            if (result != ESP_OK) {
                ld_sounds_clear(p);
                ++missing;
            }
            ld_pad_settings_t *settings = ld_pad(p);
            settings->level = (uint8_t)(level > LD_LEVEL_MAX ? LD_LEVEL_MAX : level);
            settings->pitch = (int8_t)(pitch < LD_PITCH_MIN   ? LD_PITCH_MIN
                                       : pitch > LD_PITCH_MAX ? LD_PITCH_MAX
                                                              : pitch);
            settings->choke = (uint8_t)(choke > LD_CHOKE_GROUPS ? 0 : choke);
            seen[p] = true;
        }
    }
    fclose(file);
    for (int pad = 0; pad < LD_PADS; ++pad) {
        if (!seen[pad]) {
            ld_sounds_clear(pad);
        }
    }
    ld_set_loop(loop, count, bars);
    free(loop);
    if (missing > 0) {
        snprintf(status, status_size, " Bank %d: %d sound%s missing", bank + 1, missing,
                 missing == 1 ? "" : "s");
    } else {
        snprintf(status, status_size, " Loaded bank %d: %d hits", bank + 1, count);
    }
    return ESP_OK;
}
