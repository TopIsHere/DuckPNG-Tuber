/*
 * Duck PNGTuber for OBS Studio
 *
 * Target: OBS Studio 32.0.x
 * License: GPL-2.0-or-later
 *
 * This plugin has no network code, no WebSocket code, no browser code,
 * no telemetry, and does not open the microphone itself.
 *
 * It watches an existing OBS audio source (for example:
 * "Voicemod Virtual Microphone") and switches between two PNG images.
 */

#include <obs-module.h>
#include <obs.h>
#include <obs-source.h>
#include <obs-data.h>
#include <obs-properties.h>

#include <graphics/graphics.h>
#include <graphics/image-file.h>

#include <util/bmem.h>
#include <util/platform.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("duck-pngtuber", "en-US")

#define SOURCE_ID "duck_pngtuber_source"

#define S_IDLE_PATH   "idle_path"
#define S_TALK_PATH   "talk_path"
#define S_AUDIO_NAME  "audio_source"
#define S_THRESHOLD   "threshold"
#define S_RELEASE_MS  "release_ms"
#define S_SCALE       "scale"

struct duck_pngtuber {
    obs_source_t *source;

    pthread_mutex_t mutex;

    char *idle_path;
    char *talk_path;
    char *audio_name;

    float threshold;
    int release_ms;
    float scale;

    gs_image_file_t idle_image;
    gs_image_file_t talk_image;

    bool idle_dirty;
    bool talk_dirty;
    bool idle_ready;
    bool talk_ready;

    uint32_t width;
    uint32_t height;

    obs_source_t *audio_source;

    float level;
    uint64_t last_above_threshold_ns;
    bool talking;
};

static char *dup_string(const char *s)
{
    if (!s)
        return bstrdup("");
    return bstrdup(s);
}

static void replace_string(char **dst, const char *src)
{
    char *copy = dup_string(src);
    if (*dst)
        bfree(*dst);
    *dst = copy;
}

static float audio_level_from_data(obs_source_t *source, const struct audio_data *audio)
{
    if (!audio || !audio->frames)
        return 0.0f;

    /*
     * OBS source capture callbacks normally receive float audio.
     * Handle both interleaved and planar data:
     *   data[0] only  -> interleaved
     *   data[0..N-1]  -> planar
     */
    size_t planes = 0;
    for (size_t i = 0; i < MAX_AV_PLANES; ++i) {
        if (audio->data[i])
            ++planes;
    }

    if (!planes)
        return 0.0f;

    int channels = 2;
    if (source) {
        const enum speaker_layout speakers =
            obs_source_get_speaker_layout(source);
        switch (speakers) {
        case SPEAKERS_MONO:    channels = 1; break;
        case SPEAKERS_STEREO:  channels = 2; break;
        case SPEAKERS_2POINT1: channels = 3; break;
        case SPEAKERS_4POINT0: channels = 4; break;
        case SPEAKERS_4POINT1: channels = 5; break;
        case SPEAKERS_5POINT1: channels = 6; break;
        case SPEAKERS_7POINT1: channels = 8; break;
        default:               channels = 2; break;
        }
    }

    float peak = 0.0f;

    if (planes == 1) {
        const float *samples = (const float *)audio->data[0];
        const size_t count = (size_t)audio->frames * (size_t)channels;

        for (size_t i = 0; i < count; ++i) {
            const float v = fabsf(samples[i]);
            if (v > peak)
                peak = v;
        }
    } else {
        for (size_t p = 0; p < planes; ++p) {
            const float *samples = (const float *)audio->data[p];
            for (uint32_t i = 0; i < audio->frames; ++i) {
                const float v = fabsf(samples[i]);
                if (v > peak)
                    peak = v;
            }
        }
    }

    return peak;
}

static void audio_callback(void *param, obs_source_t *source,
                           const struct audio_data *audio_data, bool muted)
{
    struct duck_pngtuber *d = param;

    if (muted)
        return;

    const float level = audio_level_from_data(source, audio_data);
    const uint64_t now = os_gettime_ns();

    pthread_mutex_lock(&d->mutex);

    d->level = level;

    if (level >= d->threshold) {
        d->last_above_threshold_ns = now;
        d->talking = true;
    }

    pthread_mutex_unlock(&d->mutex);
}

static void detach_audio_source(struct duck_pngtuber *d)
{
    if (!d->audio_source)
        return;

    obs_source_remove_audio_capture_callback(
        d->audio_source, audio_callback, d);

    obs_source_release(d->audio_source);
    d->audio_source = NULL;
}

static void attach_audio_source(struct duck_pngtuber *d)
{
    detach_audio_source(d);

    if (!d->audio_name || !*d->audio_name)
        return;

    obs_source_t *source = obs_get_source_by_name(d->audio_name);
    if (!source)
        return;

    obs_source_add_audio_capture_callback(source, audio_callback, d);
    d->audio_source = source;
}

static void load_images_if_needed(struct duck_pngtuber *d)
{
    pthread_mutex_lock(&d->mutex);

    const bool idle_dirty = d->idle_dirty;
    const bool talk_dirty = d->talk_dirty;

    d->idle_dirty = false;
    d->talk_dirty = false;

    const char *idle_path = d->idle_path ? d->idle_path : "";
    const char *talk_path = d->talk_path ? d->talk_path : "";

    if (idle_dirty) {
        if (d->idle_ready)
            gs_image_file_free(&d->idle_image);

        d->idle_ready = false;

        if (*idle_path) {
            gs_image_file_init(&d->idle_image, idle_path);
            if (d->idle_image.loaded) {
                gs_image_file_init_texture(&d->idle_image);
                d->idle_ready = true;
            }
        }
    }

    if (talk_dirty) {
        if (d->talk_ready)
            gs_image_file_free(&d->talk_image);

        d->talk_ready = false;

        if (*talk_path) {
            gs_image_file_init(&d->talk_image, talk_path);
            if (d->talk_image.loaded) {
                gs_image_file_init_texture(&d->talk_image);
                d->talk_ready = true;
            }
        }
    }

    if (d->idle_ready) {
        d->width = d->idle_image.cx;
        d->height = d->idle_image.cy;
    } else if (d->talk_ready) {
        d->width = d->talk_image.cx;
        d->height = d->talk_image.cy;
    }

    pthread_mutex_unlock(&d->mutex);
}

static void duck_video_tick(void *data, float seconds)
{
    struct duck_pngtuber *d = data;
    UNUSED_PARAMETER(seconds);

    obs_enter_graphics();
    load_images_if_needed(d);
    obs_leave_graphics();

    const uint64_t now = os_gettime_ns();

    pthread_mutex_lock(&d->mutex);

    if (d->talking) {
        const uint64_t delay_ns =
            (uint64_t)(d->release_ms > 0 ? d->release_ms : 0) * 1000000ULL;

        if (now > d->last_above_threshold_ns &&
            now - d->last_above_threshold_ns >= delay_ns) {
            d->talking = false;
        }
    }

    pthread_mutex_unlock(&d->mutex);
}

static void duck_video_render(void *data, gs_effect_t *effect)
{
    struct duck_pngtuber *d = data;

    pthread_mutex_lock(&d->mutex);

    const bool talking = d->talking;
    const float scale = d->scale;

    gs_image_file_t *image =
        talking && d->talk_ready ? &d->talk_image :
        d->idle_ready ? &d->idle_image :
        d->talk_ready ? &d->talk_image : NULL;

    if (!image || !image->texture) {
        pthread_mutex_unlock(&d->mutex);
        UNUSED_PARAMETER(effect);
        return;
    }

    gs_texture_t *texture = image->texture;
    const uint32_t width = image->cx;
    const uint32_t height = image->cy;

    /*
     * The texture is owned by the image file and remains alive while
     * this source is rendering. Keep the mutex for the draw call.
     */
    gs_eparam_t *image_param =
        gs_effect_get_param_by_name(effect, "image");

    if (image_param)
        gs_effect_set_texture(image_param, texture);

    while (gs_effect_loop(effect, "Draw")) {
        gs_draw_sprite(
            texture, 0,
            (uint32_t)fmaxf(1.0f, width * scale),
            (uint32_t)fmaxf(1.0f, height * scale));
    }

    pthread_mutex_unlock(&d->mutex);
}

static uint32_t duck_get_width(void *data)
{
    struct duck_pngtuber *d = data;

    pthread_mutex_lock(&d->mutex);
    const uint32_t width = d->width;
    const float scale = d->scale;
    pthread_mutex_unlock(&d->mutex);

    return (uint32_t)fmaxf(1.0f, width * scale);
}

static uint32_t duck_get_height(void *data)
{
    struct duck_pngtuber *d = data;

    pthread_mutex_lock(&d->mutex);
    const uint32_t height = d->height;
    const float scale = d->scale;
    pthread_mutex_unlock(&d->mutex);

    return (uint32_t)fmaxf(1.0f, height * scale);
}

static void duck_defaults(obs_data_t *settings)
{
    char *idle = obs_module_file("idle.png");
    char *talk = obs_module_file("talking.png");

    obs_data_set_default_string(settings, S_IDLE_PATH, idle ? idle : "");
    obs_data_set_default_string(settings, S_TALK_PATH, talk ? talk : "");
    obs_data_set_default_string(settings, S_AUDIO_NAME,
                                "Voicemod Virtual Microphone");
    obs_data_set_default_double(settings, S_THRESHOLD, 0.025);
    obs_data_set_default_int(settings, S_RELEASE_MS, 120);
    obs_data_set_default_double(settings, S_SCALE, 1.0);

    if (idle)
        bfree(idle);
    if (talk)
        bfree(talk);
}

static obs_properties_t *duck_properties(void *data)
{
    UNUSED_PARAMETER(data);

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(
        props, S_IDLE_PATH, "Idle PNG",
        OBS_PATH_FILE, "PNG files (*.png);;All files (*.*)", NULL);

    obs_properties_add_path(
        props, S_TALK_PATH, "Talking PNG",
        OBS_PATH_FILE, "PNG files (*.png);;All files (*.*)", NULL);

    obs_properties_add_text(
        props, S_AUDIO_NAME, "OBS audio source",
        OBS_TEXT_DEFAULT);

    obs_properties_add_float_slider(
        props, S_THRESHOLD, "Talking threshold",
        0.001, 1.0, 0.001);

    obs_properties_add_int_slider(
        props, S_RELEASE_MS, "Mouth release delay (ms)",
        0, 1000, 10);

    obs_properties_add_float_slider(
        props, S_SCALE, "Scale",
        0.05, 3.0, 0.01);

    return props;
}

static void duck_update(void *data, obs_data_t *settings)
{
    struct duck_pngtuber *d = data;

    const char *idle = obs_data_get_string(settings, S_IDLE_PATH);
    const char *talk = obs_data_get_string(settings, S_TALK_PATH);
    const char *audio = obs_data_get_string(settings, S_AUDIO_NAME);

    pthread_mutex_lock(&d->mutex);

    const bool idle_changed =
        !d->idle_path || strcmp(d->idle_path, idle ? idle : "") != 0;
    const bool talk_changed =
        !d->talk_path || strcmp(d->talk_path, talk ? talk : "") != 0;

    replace_string(&d->idle_path, idle);
    replace_string(&d->talk_path, talk);
    replace_string(&d->audio_name, audio);

    d->threshold = (float)obs_data_get_double(settings, S_THRESHOLD);
    d->release_ms = (int)obs_data_get_int(settings, S_RELEASE_MS);
    d->scale = (float)obs_data_get_double(settings, S_SCALE);

    if (d->scale <= 0.0f)
        d->scale = 1.0f;

    d->idle_dirty |= idle_changed;
    d->talk_dirty |= talk_changed;

    pthread_mutex_unlock(&d->mutex);

    /*
     * The selected OBS audio source is looked up by name. This means the
     * plugin never opens the microphone itself; Voicemod stays responsible
     * for the microphone/voice-changing part.
     */
    attach_audio_source(d);
}

static void *duck_create(obs_data_t *settings, obs_source_t *source)
{
    struct duck_pngtuber *d = bzalloc(sizeof(*d));

    d->source = source;
    pthread_mutex_init(&d->mutex, NULL);

    d->idle_path = NULL;
    d->talk_path = NULL;
    d->audio_name = NULL;

    d->threshold = 0.025f;
    d->release_ms = 120;
    d->scale = 1.0f;

    d->idle_dirty = true;
    d->talk_dirty = true;
    d->width = 1405;
    d->height = 1119;

    duck_update(d, settings);

    return d;
}

static void duck_destroy(void *data)
{
    struct duck_pngtuber *d = data;

    detach_audio_source(d);

    obs_enter_graphics();

    if (d->idle_ready)
        gs_image_file_free(&d->idle_image);
    if (d->talk_ready)
        gs_image_file_free(&d->talk_image);

    obs_leave_graphics();

    if (d->idle_path)
        bfree(d->idle_path);
    if (d->talk_path)
        bfree(d->talk_path);
    if (d->audio_name)
        bfree(d->audio_name);

    pthread_mutex_destroy(&d->mutex);
    bfree(d);
}

static const char *duck_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Duck PNGTuber";
}

static struct obs_source_info duck_source_info = {
    .id = SOURCE_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = duck_get_name,
    .create = duck_create,
    .destroy = duck_destroy,
    .update = duck_update,
    .get_defaults = duck_defaults,
    .get_properties = duck_properties,
    .video_tick = duck_video_tick,
    .video_render = duck_video_render,
    .get_width = duck_get_width,
    .get_height = duck_get_height,
};

bool obs_module_load(void)
{
    obs_register_source(&duck_source_info);
    return true;
}

void obs_module_unload(void)
{
}
