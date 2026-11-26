/* Load the BUILT dsp.so the way chain_midi.c does (dlopen + dlsym of
 * move_midi_fx_init) and drive it for a few bars: proves the artifact, not a
 * recompile of its sources. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

static double g_beat = -1.0;
static double beat(void) { return g_beat; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    midi_fx_init_fn init = (midi_fx_init_fn)dlsym(h, MIDI_FX_INIT_SYMBOL);
    if (!init) { fprintf(stderr, "missing %s\n", MIDI_FX_INIT_SYMBOL); return 1; }
    static host_api_v1_t host;
    host.api_version = 1; host.sample_rate = 44100; host.frames_per_block = 128;
    host.get_beat_position = beat;
    midi_fx_api_v1_t *api = init(&host);
    if (!api || api->api_version != MIDI_FX_API_VERSION) return 1;
    void *inst = api->create_instance("/nonexistent", NULL);
    if (!inst) return 1;

    uint8_t out[MIDI_FX_MAX_OUT_MSGS][3]; int lens[MIDI_FX_MAX_OUT_MSGS];
    int ons = 0, offs = 0;
    for (long f = 0; f < 44100L * 4; f += 128) {           /* 4 s at 120 BPM = 4 loops */
        g_beat = f / 22050.0;
        int n = api->tick(inst, 128, 44100, out, lens, MIDI_FX_MAX_OUT_MSGS);
        for (int i = 0; i < n; i++) *((out[i][0] & 0xF0) == 0x90 ? &ons : &offs) += 1;
        if (f == 44100) { uint8_t pad[3] = { 0x90, 36, 100 }; api->process_midi(inst, pad, 3, out, lens, 16); }
    }
    g_beat = -1.0;
    int n = api->tick(inst, 128, 44100, out, lens, MIDI_FX_MAX_OUT_MSGS);
    for (int i = 0; i < n; i++) *((out[i][0] & 0xF0) == 0x90 ? &ons : &offs) += 1;
    char dbg[1024];
    api->get_param(inst, "debug", dbg, sizeof(dbg));
    api->destroy_instance(inst);
    dlclose(h);
    printf("%s", dbg);
    printf("dlopen smoke: %d note-ons, %d note-offs\n", ons, offs);
    return (ons >= 24 && ons == offs) ? 0 : 1;
}
