#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 44100
#define DEFAULT_OCTAVE 4
#define DEFAULT_LEN 4
#define DEFAULT_TEMPO 120
#define DEFAULT_VOLUME 8

// MSX musical articulation style (M command)
typedef enum {
    STYLE_NORMAL = 0,     // MN: 7/8 duration sound, 1/8 duration silence
    STYLE_BACKGROUND = 1, // MB / MS: 3/4 duration sound, 1/4 duration silence (detached/staccato)
    STYLE_LEGATO = 2      // ML: 8/8 duration sound, continuous without pause
} MusicStyle;

// Persistent state for each of the 3 PSG sound channels
typedef struct {
    int octave;        // 0 to 8 (default 4)
    int default_len;   // 1 to 64 (default 4)
    int tempo;         // 32 to 255 (default 120)
    int volume;        // 0 to 15 (default 8)
    MusicStyle style;  // STYLE_NORMAL, STYLE_BACKGROUND, STYLE_LEGATO
    double phase;      // Continuous phase accumulator for smooth wave transitions
    double filter_val; // Low-pass filter state to emulate MSX analog audio filter
} ChannelState;

// Dynamic audio buffer for 16-bit PCM samples
typedef struct {
    int16_t *data;
    size_t size;
    size_t capacity;
} AudioBuffer;

// Logarithmic DAC attenuation table for General Instrument AY-3-8910 (MSX PSG)
// Levels 0 to 15 normalized to 1.0 maximum
static const double psg_dac_table[16] = {
    0.000000, 0.009766, 0.014234, 0.020752,
    0.030273, 0.044189, 0.064514, 0.094177,
    0.137451, 0.200684, 0.292969, 0.427734,
    0.624512, 0.749023, 0.866025, 1.000000
};

// MSX analog RC low-pass filter coefficient (~8 kHz cutoff at 44.1 kHz)
// Smooths sharp square wave transitions and recreates authentic warm chiptune tone
static const double LPF_ALPHA = 0.68;

// Initialize dynamic audio buffer
void init_buffer(AudioBuffer *ab) {
    ab->capacity = SAMPLE_RATE * 5;
    ab->size = 0;
    ab->data = malloc(ab->capacity * sizeof(int16_t));
}

// Free dynamic audio buffer
void free_buffer(AudioBuffer *ab) {
    if (ab->data) {
        free(ab->data);
        ab->data = NULL;
    }
    ab->size = 0;
    ab->capacity = 0;
}

// Reset a single channel state to MSX power-on / BEEP defaults
void reset_channel_state(ChannelState *cs) {
    cs->octave = DEFAULT_OCTAVE;
    cs->default_len = DEFAULT_LEN;
    cs->tempo = DEFAULT_TEMPO;
    cs->volume = DEFAULT_VOLUME;
    cs->style = STYLE_NORMAL;
    cs->phase = 0.0;
    cs->filter_val = 0.0;
}

// Determine path to persistent state file
static const char *get_state_file_path(char *buf, size_t len) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && strlen(xdg) > 0) {
        snprintf(buf, len, "%s/msx_play_state.txt", xdg);
    } else {
        const char *tmp = getenv("TMPDIR");
        if (!tmp) tmp = "/tmp";
        snprintf(buf, len, "%s/msx_play_state_%u.txt", tmp, (unsigned int)getuid());
    }
    return buf;
}

// Load persisted state for the 3 channels
int load_state(ChannelState states[3]) {
    char path[512];
    get_state_file_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        for (int i = 0; i < 3; i++) reset_channel_state(&states[i]);
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        int ch, oct, len, tempo, vol, style;
        if (fscanf(f, "CH %d: O=%d L=%d T=%d V=%d M=%d\n", &ch, &oct, &len, &tempo, &vol, &style) == 6) {
            states[i].octave = (oct >= 0 && oct <= 8) ? oct : DEFAULT_OCTAVE;
            states[i].default_len = (len >= 1 && len <= 64) ? len : DEFAULT_LEN;
            states[i].tempo = (tempo >= 32 && tempo <= 255) ? tempo : DEFAULT_TEMPO;
            states[i].volume = (vol >= 0 && vol <= 15) ? vol : DEFAULT_VOLUME;
            states[i].style = (style >= 0 && style <= 2) ? (MusicStyle)style : STYLE_NORMAL;
            states[i].phase = 0.0;
            states[i].filter_val = 0.0;
        } else {
            reset_channel_state(&states[i]);
        }
    }
    fclose(f);
    return 1;
}

// Save persisted state for the 3 channels
int save_state(const ChannelState states[3]) {
    char path[512];
    get_state_file_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    for (int i = 0; i < 3; i++) {
        fprintf(f, "CH %d: O=%d L=%d T=%d V=%d M=%d\n",
                i + 1, states[i].octave, states[i].default_len,
                states[i].tempo, states[i].volume, (int)states[i].style);
    }
    fclose(f);
    return 1;
}

// Display current settings of all 3 channels
void print_status(const ChannelState states[3]) {
    printf("\n--- MSX PLAY: Current Channel Settings ---\n");
    for (int i = 0; i < 3; i++) {
        const char *m_desc = "MN (Normal 7/8)";
        if (states[i].style == STYLE_BACKGROUND) m_desc = "MB (Background/Staccato 3/4)";
        else if (states[i].style == STYLE_LEGATO) m_desc = "ML (Legato 8/8)";

        printf("Channel %d: Octave=%d, Length=1/%d, Tempo=%d BPM, Volume=%d/15, Style=%s\n",
               i + 1, states[i].octave, states[i].default_len,
               states[i].tempo, states[i].volume, m_desc);
    }
    printf("------------------------------------------\n\n");
}

// Append synthesized tone samples using PSG 50% square wave (with analog filter) or sine wave
void append_samples(AudioBuffer *ab, ChannelState *cs, double freq, double duration, int use_sine) {
    size_t num_samples = (size_t)(duration * SAMPLE_RATE);
    if (num_samples == 0) return;

    if (ab->size + num_samples > ab->capacity) {
        ab->capacity = (ab->size + num_samples) * 2 + 1024;
        ab->data = realloc(ab->data, ab->capacity * sizeof(int16_t));
    }

    double vol_factor = psg_dac_table[cs->volume];
    double phase_step = freq / SAMPLE_RATE;

    for (size_t i = 0; i < num_samples; i++) {
        double raw;
        if (use_sine) {
            raw = sin(2.0 * M_PI * cs->phase);
        } else {
            // 50% duty cycle square wave characteristic of AY-3-8910 tone generator
            raw = (cs->phase < 0.5) ? 1.0 : -1.0;
        }

        cs->phase += phase_step;
        if (cs->phase >= 1.0) {
            cs->phase -= floor(cs->phase);
        }

        // Low-pass RC filter to emulate MSX audio hardware output filter
        cs->filter_val += LPF_ALPHA * (raw - cs->filter_val);

        // Amplitude factor ~10000 per channel so 3 mixed channels fit within int16_t (32767)
        int32_t val = (int32_t)(cs->filter_val * vol_factor * 10000.0);
        ab->data[ab->size + i] = (int16_t)val;
    }
    ab->size += num_samples;
}

// Append silence samples to buffer (for rests and articulation spacing)
void append_silence(AudioBuffer *ab, ChannelState *cs, double duration) {
    size_t num_samples = (size_t)(duration * SAMPLE_RATE);
    if (num_samples == 0) return;

    if (ab->size + num_samples > ab->capacity) {
        ab->capacity = (ab->size + num_samples) * 2 + 1024;
        ab->data = realloc(ab->data, ab->capacity * sizeof(int16_t));
    }

    for (size_t i = 0; i < num_samples; i++) {
        // Smooth filter decay to zero to prevent DC offset clicks
        cs->filter_val += LPF_ALPHA * (0.0 - cs->filter_val);
        ab->data[ab->size + i] = 0;
    }
    ab->size += num_samples;
}

// Semitone offset for musical note names
static int note_base_semitone(char note) {
    switch(toupper(note)) {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
        default:  return 0;
    }
}

// Calculate frequency from note letter, accidental, and octave
// Octave 4 C (C4) is MIDI note 60 (261.63 Hz). A4 is MIDI note 69 (440.0 Hz).
double note_to_freq(char note, int semitone, int octave) {
    int base = note_base_semitone(note);
    int midi = base + semitone + (octave + 1) * 12;
    return 440.0 * pow(2.0, (midi - 69.0) / 12.0);
}

// Calculate frequency for direct note number N (N0 = rest, N1 to N95)
// N1 = C1 (MIDI 24, ~32.7 Hz). N37 = C4 (MIDI 60). N46 = A4 (440.0 Hz).
double direct_note_to_freq(int n) {
    if (n <= 0) return 0.0;
    int midi = 23 + n;
    return 440.0 * pow(2.0, (midi - 69.0) / 12.0);
}

// Parse MSX BASIC MML commands for one channel and synthesize into audio buffer
void parse_channel(const char *str, AudioBuffer *ab, ChannelState *cs, int use_sine) {
    int i = 0;
    while (str[i] != '\0') {
        char c = toupper(str[i]);

        if (isspace(c)) {
            i++;
            continue;
        }

        // Octave: O [0 to 8]
        if (c == 'O') {
            i++;
            int oct = 0;
            if (isdigit(str[i])) {
                oct = str[i] - '0';
                i++;
                if (oct > 8) oct = 8;
                cs->octave = oct;
            }
        }
        // Relative octave shifts: < (down) and > (up)
        else if (c == '<') {
            if (cs->octave > 0) cs->octave--;
            i++;
        }
        else if (c == '>') {
            if (cs->octave < 8) cs->octave++;
            i++;
        }
        // Default note length: L [1 to 64]
        else if (c == 'L') {
            i++;
            int len = 0;
            while (isdigit(str[i])) {
                len = len * 10 + (str[i] - '0');
                i++;
            }
            if (len >= 1 && len <= 64) {
                cs->default_len = len;
            }
        }
        // Tempo: T [32 to 255]
        else if (c == 'T') {
            i++;
            int t = 0;
            while (isdigit(str[i])) {
                t = t * 10 + (str[i] - '0');
                i++;
            }
            if (t >= 32 && t <= 255) {
                cs->tempo = t;
            }
        }
        // Volume: V [0 to 15]
        else if (c == 'V') {
            i++;
            int v = 0;
            while (isdigit(str[i])) {
                v = v * 10 + (str[i] - '0');
                i++;
            }
            if (v >= 0 && v <= 15) {
                cs->volume = v;
            }
        }
        // Music style / articulation: M [N / B / L] (also accepts S for staccato)
        else if (c == 'M') {
            i++;
            char sub = toupper(str[i]);
            if (sub == 'N') {
                cs->style = STYLE_NORMAL;
                i++;
            } else if (sub == 'B' || sub == 'S') {
                cs->style = STYLE_BACKGROUND;
                i++;
            } else if (sub == 'L') {
                cs->style = STYLE_LEGATO;
                i++;
            }
        }
        // Rest: R [optional length] [optional dots]
        else if (c == 'R') {
            i++;
            int rest_len = cs->default_len;
            if (isdigit(str[i])) {
                int len = 0;
                while (isdigit(str[i])) {
                    len = len * 10 + (str[i] - '0');
                    i++;
                }
                if (len >= 1 && len <= 64) rest_len = len;
            }

            int dots = 0;
            while (str[i] == '.') {
                dots++;
                i++;
            }

            double base_dur = (240.0 / (double)cs->tempo) / (double)rest_len;
            double dot_factor = 1.0;
            double dot_add = 0.5;
            for (int d = 0; d < dots; d++) {
                dot_factor += dot_add;
                dot_add *= 0.5;
            }
            double total_dur = base_dur * dot_factor;
            append_silence(ab, cs, total_dur);
        }
        // Direct note number: N [0 to 95] (N0 = rest)
        else if (c == 'N') {
            i++;
            int note_num = 0;
            while (isdigit(str[i])) {
                note_num = note_num * 10 + (str[i] - '0');
                i++;
            }

            int dots = 0;
            while (str[i] == '.') {
                dots++;
                i++;
            }

            double base_dur = (240.0 / (double)cs->tempo) / (double)cs->default_len;
            double dot_factor = 1.0;
            double dot_add = 0.5;
            for (int d = 0; d < dots; d++) {
                dot_factor += dot_add;
                dot_add *= 0.5;
            }
            double total_dur = base_dur * dot_factor;

            if (note_num == 0) {
                // N0 represents a rest
                append_silence(ab, cs, total_dur);
            } else {
                double freq = direct_note_to_freq(note_num);
                double sound_dur = 0.0;
                double rest_dur = 0.0;

                if (cs->volume == 0 || freq <= 0.0) {
                    rest_dur = total_dur;
                } else {
                    switch (cs->style) {
                        case STYLE_NORMAL:
                            sound_dur = total_dur * 7.0 / 8.0;
                            rest_dur = total_dur * 1.0 / 8.0;
                            break;
                        case STYLE_BACKGROUND:
                            sound_dur = total_dur * 6.0 / 8.0;
                            rest_dur = total_dur * 2.0 / 8.0;
                            break;
                        case STYLE_LEGATO:
                            sound_dur = total_dur;
                            rest_dur = 0.0;
                            break;
                    }
                }

                if (sound_dur > 0.0) append_samples(ab, cs, freq, sound_dur, use_sine);
                if (rest_dur > 0.0) append_silence(ab, cs, rest_dur);
            }
        }
        // Notes A through G
        else if (c >= 'A' && c <= 'G') {
            char note = c;
            i++;

            // Accidental: + / # (sharp), - (flat)
            int semitone = 0;
            if (str[i] == '+' || str[i] == '#') {
                semitone = 1;
                i++;
            } else if (str[i] == '-') {
                semitone = -1;
                i++;
            }

            // Optional note-specific length
            int note_len = cs->default_len;
            if (isdigit(str[i])) {
                int len = 0;
                while (isdigit(str[i])) {
                    len = len * 10 + (str[i] - '0');
                    i++;
                }
                if (len >= 1 && len <= 64) note_len = len;
            }

            // Prolongation dots (.)
            int dots = 0;
            while (str[i] == '.') {
                dots++;
                i++;
            }

            double freq = note_to_freq(note, semitone, cs->octave);
            double base_dur = (240.0 / (double)cs->tempo) / (double)note_len;
            double dot_factor = 1.0;
            double dot_add = 0.5;
            for (int d = 0; d < dots; d++) {
                dot_factor += dot_add;
                dot_add *= 0.5;
            }
            double total_dur = base_dur * dot_factor;

            // Divide duration according to music style (Normal, Background, Legato)
            double sound_dur = 0.0;
            double rest_dur = 0.0;

            if (cs->volume == 0 || freq <= 0.0) {
                rest_dur = total_dur;
            } else {
                switch (cs->style) {
                    case STYLE_NORMAL:
                        sound_dur = total_dur * 7.0 / 8.0;
                        rest_dur = total_dur * 1.0 / 8.0;
                        break;
                    case STYLE_BACKGROUND:
                        sound_dur = total_dur * 6.0 / 8.0;
                        rest_dur = total_dur * 2.0 / 8.0;
                        break;
                    case STYLE_LEGATO:
                        sound_dur = total_dur;
                        rest_dur = 0.0;
                        break;
                }
            }

            if (sound_dur > 0.0) append_samples(ab, cs, freq, sound_dur, use_sine);
            if (rest_dur > 0.0) append_silence(ab, cs, rest_dur);
        } else {
            i++;
        }
    }
}

// Write standard 16-bit mono PCM WAV file header
void write_wav_header(FILE *f, uint32_t data_size) {
    uint32_t total_size = data_size + 36;
    uint16_t channels = 1;
    uint32_t sample_rate = SAMPLE_RATE;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    fwrite("RIFF", 1, 4, f);
    fwrite(&total_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub_chunk1_size = 16;
    fwrite(&sub_chunk1_size, 4, 1, f);
    uint16_t audio_format = 1; // PCM
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

// Play audio buffer through available system player or save to designated file
void play_buffer(const int16_t *data, size_t num_samples, const char *output_file) {
    char temp_filename[256];
    const char *wav_path = output_file;

    if (!wav_path) {
        snprintf(temp_filename, sizeof(temp_filename), "/tmp/msx_play_%u.wav", (unsigned int)getpid());
        wav_path = temp_filename;
    }

    FILE *f = fopen(wav_path, "wb");
    if (!f) {
        perror("Error creating WAV file");
        return;
    }

    uint32_t data_size = (uint32_t)(num_samples * sizeof(int16_t));
    write_wav_header(f, data_size);
    fwrite(data, sizeof(int16_t), num_samples, f);
    fclose(f);

    if (!output_file) {
        char command[2048];
        snprintf(command, sizeof(command),
                 "aplay -q \"%s\" 2>/dev/null || paplay \"%s\" 2>/dev/null || pw-play \"%s\" 2>/dev/null || play -q \"%s\" 2>/dev/null",
                 wav_path, wav_path, wav_path, wav_path);
        system(command);
        remove(wav_path);
    } else {
        printf("Audio successfully saved to: %s\n", output_file);
    }
}

// Sound the classic MSX BIOS system BEEP (1200 Hz, ~150 ms) and reset channel attributes
void trigger_system_beep(ChannelState states[3]) {
    printf("[MSX BEEP] Resetting attributes to defaults (O4, L4, T120, V8, MN)...\n");

    for (int i = 0; i < 3; i++) {
        reset_channel_state(&states[i]);
    }
    save_state(states);

    // Generate 1200 Hz square wave tone for 0.15s at maximum volume
    ChannelState beep_state;
    reset_channel_state(&beep_state);
    beep_state.volume = 15;

    AudioBuffer ab;
    init_buffer(&ab);
    append_samples(&ab, &beep_state, 1200.0, 0.15, 0);
    append_silence(&ab, &beep_state, 0.05);

    play_buffer(ab.data, ab.size, NULL);
    free_buffer(&ab);
}

// Process and mix up to 3 sound channels simultaneously
void execute_channels(int num_channels, const char *channel_strs[], ChannelState states[3], int use_sine, const char *output_file) {
    if (num_channels > 3) num_channels = 3;

    AudioBuffer buffers[3];
    for (int i = 0; i < num_channels; i++) {
        init_buffer(&buffers[i]);
        printf("Processing Channel %d: %s\n", i + 1, channel_strs[i]);
        parse_channel(channel_strs[i], &buffers[i], &states[i], use_sine);
    }

    size_t max_size = 0;
    for (int i = 0; i < num_channels; i++) {
        if (buffers[i].size > max_size) {
            max_size = buffers[i].size;
        }
    }

    if (max_size == 0) {
        printf("No playable notes found.\n");
        for (int i = 0; i < num_channels; i++) free_buffer(&buffers[i]);
        return;
    }

    // Mix channels with soft saturation / limiting
    int16_t *mixed = calloc(max_size, sizeof(int16_t));
    for (size_t s = 0; s < max_size; s++) {
        int32_t sum = 0;
        for (int c = 0; c < num_channels; c++) {
            if (s < buffers[c].size) {
                sum += buffers[c].data[s];
            }
        }
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        mixed[s] = (int16_t)sum;
    }

    // Persist updated channel settings
    save_state(states);

    printf("Playing MSX PSG mix...\n");
    play_buffer(mixed, max_size, output_file);

    for (int i = 0; i < num_channels; i++) {
        free_buffer(&buffers[i]);
    }
    free(mixed);
}

// Interactive REPL mimicking MSX BASIC command line
void interactive_repl(ChannelState states[3], int use_sine) {
    printf("========================================================\n");
    printf("     MSX PLAY - MSX BASIC PSG Synthesizer (AY-3-8910)   \n");
    printf("========================================================\n");
    printf("Available commands:\n");
    printf("  PLAY \"ch1\" [, \"ch2\", \"ch3\"]  - Play up to 3 channels\n");
    printf("  \"ch1\" [, \"ch2\"]              - Shorthand notation\n");
    printf("  BEEP                          - Sound system beep and reset settings\n");
    printf("  STATUS                        - Display current channel settings\n");
    printf("  RESET                         - Reset settings without sound\n");
    printf("  HELP                          - Show MML syntax reference\n");
    printf("  QUIT / EXIT                   - Exit interactive mode\n");
    printf("--------------------------------------------------------\n");
    printf("Note: Settings (O, L, T, V, M) persist between executions.\n\n");

    char line[1024];
    while (1) {
        printf("MSX> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        // Strip trailing newline characters
        size_t slen = strlen(line);
        while (slen > 0 && (line[slen - 1] == '\n' || line[slen - 1] == '\r')) {
            line[--slen] = '\0';
        }

        // Skip leading whitespace
        char *p = line;
        while (isspace(*p)) p++;
        if (*p == '\0') continue;

        if (strcasecmp(p, "QUIT") == 0 || strcasecmp(p, "EXIT") == 0) {
            break;
        }
        if (strcasecmp(p, "BEEP") == 0) {
            trigger_system_beep(states);
            continue;
        }
        if (strcasecmp(p, "RESET") == 0) {
            for (int i = 0; i < 3; i++) reset_channel_state(&states[i]);
            save_state(states);
            printf("Channel settings reset to defaults.\n");
            continue;
        }
        if (strcasecmp(p, "STATUS") == 0) {
            print_status(states);
            continue;
        }
        if (strcasecmp(p, "HELP") == 0) {
            printf("\nMSX MML Subcommands:\n");
            printf("  O [0-8]    : Octave (Default: 4)\n");
            printf("  < / >      : Down / Up one octave\n");
            printf("  A-G        : Notes (optional +/# for sharp, - for flat)\n");
            printf("  N [0-95]   : Direct note number (N0 = rest, N1 = C1, N37 = C4, N46 = A4)\n");
            printf("  R [1-64]   : Rest / silence\n");
            printf("  L [1-64]   : Default note length (1=whole, 4=quarter, etc.; default: 4)\n");
            printf("  .          : Dot (increases duration by 50%%)\n");
            printf("  T [32-255] : Tempo (quarter notes per minute; default: 120)\n");
            printf("  V [0-15]   : Volume level (0=silent, 15=max; default: 8)\n");
            printf("  M [N/B/L]  : Music style (MN: Normal 7/8, MB: Background/Staccato 3/4, ML: Legato)\n\n");
            continue;
        }

        // If prefixed by PLAY, skip keyword
        if (strncasecmp(p, "PLAY", 4) == 0 && (isspace(p[4]) || p[4] == '"' || p[4] == '\0')) {
            p += 4;
            while (isspace(*p)) p++;
        }

        // Extract up to 3 comma-separated channel strings
        char channels[3][512];
        int num_ch = 0;

        while (*p != '\0' && num_ch < 3) {
            while (isspace(*p) || *p == ',') p++;
            if (*p == '\0') break;

            if (*p == '"') {
                p++;
                int idx = 0;
                while (*p != '\0' && *p != '"' && idx < 511) {
                    channels[num_ch][idx++] = *p++;
                }
                channels[num_ch][idx] = '\0';
                if (*p == '"') p++;
            } else {
                int idx = 0;
                while (*p != '\0' && *p != ',' && idx < 511) {
                    channels[num_ch][idx++] = *p++;
                }
                channels[num_ch][idx] = '\0';
            }
            num_ch++;
        }

        if (num_ch > 0) {
            const char *ch_ptrs[3];
            for (int i = 0; i < num_ch; i++) ch_ptrs[i] = channels[i];
            execute_channels(num_ch, ch_ptrs, states, use_sine, NULL);
        }
    }
}

// Print command-line usage and help
void print_help(const char *prog) {
    printf("Usage: %s [options] [\"Channel 1\"] [\"Channel 2\"] [\"Channel 3\"]\n\n", prog);
    printf("Options:\n");
    printf("  -i, --interactive    Launch interactive MSX PLAY console (REPL)\n");
    printf("  -b, --beep           Sound MSX system BEEP and reset channel attributes to defaults\n");
    printf("  -r, --reset          Reset channel attributes to defaults without sounding beep\n");
    printf("  -s, --status         Display current persisted settings for all channels\n");
    printf("  -o <file.wav>        Save output to a WAV file instead of playing\n");
    printf("  --clean              Ignore persisted settings and start from defaults\n");
    printf("  --sine               Use legacy sine wave synthesis instead of MSX PSG square wave\n");
    printf("  -h, --help           Display this help guide\n\n");
    printf("Embedded MML Commands:\n");
    printf("  O [0-8]    : Sets octave (Default: 4)\n");
    printf("  < / >      : Decrements / increments octave\n");
    printf("  A-G        : Notes (+/# for sharp, - for flat, optional length and dots)\n");
    printf("  N [0-95]   : Direct note number (N0 = rest, N1 to N95)\n");
    printf("  R [1-64]   : Rest (optional length and dots)\n");
    printf("  L [1-64]   : Sets default note length (1=whole, 4=quarter; default: 4)\n");
    printf("  T [32-255] : Sets tempo in quarter notes per minute (Default: 120)\n");
    printf("  V [0-15]   : Sets volume level (Default: 8, AY-3-8910 DAC logarithmic curve)\n");
    printf("  M [N/B/L]  : Sets music style (MN: Normal 7/8, MB: Background/Staccato 3/4, ML: Legato)\n\n");
    printf("Examples:\n");
    printf("  %s \"T140 O4 V15 MN C4 D4 E4 F4 G2\" \"O3 V12 C4 E4 G2\"\n", prog);
    printf("  %s \"ML C D E F G A B > C\"\n", prog);
    printf("  %s \"N37 N39 N41 N42 N44 N46 N48 N49\"\n", prog);
    printf("  %s --beep\n", prog);
}

#define MAX_CHANNEL_CHARS 65536

// Load channels from a .mus or text file
int load_mus_file(const char *filename, char storage[3][MAX_CHANNEL_CHARS], const char *channel_ptrs[3]) {
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    for (int c = 0; c < 3; c++) storage[c][0] = '\0';

    int count = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t nread;

    while ((nread = getline(&line, &line_cap, f)) != -1) {
        size_t slen = (size_t)nread;
        while (slen > 0 && (line[slen - 1] == '\n' || line[slen - 1] == '\r')) {
            line[--slen] = '\0';
        }

        char *p = line;
        while (isspace(*p)) p++;
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        // Check for PLAY command
        if (strncasecmp(p, "PLAY", 4) == 0 && (isspace(p[4]) || p[4] == '"' || p[4] == '\0')) {
            p += 4;
            int ch_idx = 0;
            while (*p != '\0' && ch_idx < 3) {
                while (isspace(*p) || *p == ',') p++;
                if (*p == '\0') break;
                if (*p == '"') {
                    p++;
                    int idx = 0;
                    while (*p != '\0' && *p != '"' && idx < MAX_CHANNEL_CHARS - 1) storage[ch_idx][idx++] = *p++;
                    storage[ch_idx][idx] = '\0';
                    if (*p == '"') p++;
                } else {
                    int idx = 0;
                    while (*p != '\0' && *p != ',' && idx < MAX_CHANNEL_CHARS - 1) storage[ch_idx][idx++] = *p++;
                    storage[ch_idx][idx] = '\0';
                }
                channel_ptrs[ch_idx] = storage[ch_idx];
                ch_idx++;
            }
            count = ch_idx;
            break;
        }

        // Check for explicit channel prefix: CH1:, CH2:, CH3:, CHANNEL 1:, etc.
        int target_ch = -1;
        if (strncasecmp(p, "CH1:", 4) == 0 || strncasecmp(p, "CH 1:", 5) == 0) {
            target_ch = 0;
            p = strchr(p, ':') + 1;
        } else if (strncasecmp(p, "CH2:", 4) == 0 || strncasecmp(p, "CH 2:", 5) == 0) {
            target_ch = 1;
            p = strchr(p, ':') + 1;
        } else if (strncasecmp(p, "CH3:", 4) == 0 || strncasecmp(p, "CH 3:", 5) == 0) {
            target_ch = 2;
            p = strchr(p, ':') + 1;
        }

        if (target_ch >= 0) {
            while (isspace(*p)) p++;
            size_t cur_len = strlen(storage[target_ch]);
            if (cur_len > 0 && cur_len < MAX_CHANNEL_CHARS - 2) {
                strncat(storage[target_ch], " ", MAX_CHANNEL_CHARS - cur_len - 1);
            }
            strncat(storage[target_ch], p, MAX_CHANNEL_CHARS - strlen(storage[target_ch]) - 1);
            if (target_ch + 1 > count) count = target_ch + 1;
            channel_ptrs[target_ch] = storage[target_ch];
        } else {
            if (count < 3) {
                snprintf(storage[count], MAX_CHANNEL_CHARS, "%s", p);
                channel_ptrs[count] = storage[count];
                count++;
            }
        }
    }

    if (line) free(line);
    fclose(f);
    for (int i = 0; i < count; i++) {
        channel_ptrs[i] = storage[i];
    }
    return count;
}

int main(int argc, char *argv[]) {
    ChannelState states[3];
    load_state(states);

    int use_sine = 0;
    const char *output_file = NULL;
    int interactive = 0;
    int clean = 0;

    static char file_storage[3][MAX_CHANNEL_CHARS];
    const char *channel_args[3];
    int num_channels = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--beep") == 0) {
            trigger_system_beep(states);
            return 0;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reset") == 0) {
            for (int c = 0; c < 3; c++) reset_channel_state(&states[c]);
            save_state(states);
            printf("Channel settings reset to defaults.\n");
            return 0;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--status") == 0) {
            print_status(states);
            return 0;
        } else if (strcmp(argv[i], "--clean") == 0) {
            clean = 1;
            for (int c = 0; c < 3; c++) reset_channel_state(&states[c]);
        } else if (strcmp(argv[i], "--sine") == 0) {
            use_sine = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "Error: -o requires an output filename.\n");
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s. Use -h for help.\n", argv[i]);
            return 1;
        } else {
            // Check if argument is a .mus file or readable file
            if (strstr(argv[i], ".mus") || access(argv[i], R_OK) == 0) {
                int loaded = load_mus_file(argv[i], file_storage, channel_args);
                if (loaded > 0) {
                    num_channels = loaded;
                    continue;
                }
            }
            if (num_channels < 3) {
                channel_args[num_channels++] = argv[i];
            }
        }
    }

    if (clean) {
        for (int c = 0; c < 3; c++) reset_channel_state(&states[c]);
    }

    if (interactive || num_channels == 0) {
        interactive_repl(states, use_sine);
        return 0;
    }

    execute_channels(num_channels, channel_args, states, use_sine, output_file);
    return 0;
}
