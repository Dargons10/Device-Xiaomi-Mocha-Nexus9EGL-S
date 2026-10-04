/* SPDX-License-Identifier: Apache-2.0
 * Mocha FM bench diagnostic: record raw PCM, or verify the capture route
 * against a known stereo tone sent through AIF1. No Android playback loop.
 * Stop FMRadio before running. Mixer changes are restored on normal exit.
 */
#include <tinyalsa/asoundlib.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t running = 1;
static void stop_probe(int sig) { (void)sig; running = 0; }
struct setting { const char *name, *value; struct mixer_ctl *ctl; int old[2]; unsigned int n; };
static struct setting route[] = {
    {"IF1 ADC Mux", "IF1_ADC2", NULL, {0}, 0},
    {"IF1 ADC2 IN1 Mux", "IF1_ADC2_IN", NULL, {0}, 0},
    {"IF1 ADC2 IN Mux", "IF_ADC2", NULL, {0}, 0},
    {"Mono ADC L1 Mux", "Mono DAC MIXL", NULL, {0}, 0},
    {"Mono ADC R1 Mux", "Mono DAC MIXR", NULL, {0}, 0},
    {"Mono ADC MIXL ADC1 Switch", "1", NULL, {0}, 0},
    {"Mono ADC MIXR ADC1 Switch", "1", NULL, {0}, 0},
    {"Mono ADC MIXL ADC2 Switch", "0", NULL, {0}, 0},
    {"Mono ADC MIXR ADC2 Switch", "0", NULL, {0}, 0},
    {"AD MONOL ASRC Switch", "clk_sysy_div_out", NULL, {0}, 0},
    {"AD MONOR ASRC Switch", "clk_sysy_div_out", NULL, {0}, 0},
    {"DAC1 L Mux", "IF4 DAC", NULL, {0}, 0},
    {"DAC1 R Mux", "IF4 DAC", NULL, {0}, 0},
    {"DA STO ASRC Switch", "clk_sysy_div_out", NULL, {0}, 0},
    {"DAC1 MIXL DAC1 Switch", "1", NULL, {0}, 0},
    {"DAC1 MIXR DAC1 Switch", "1", NULL, {0}, 0},
    {"DAC1 Playback Volume", "175", NULL, {0}, 0},
    {"Mono ADC Capture Volume", "47", NULL, {0}, 0},
    {"Stereo DAC MIXL DAC L1 Switch", "0", NULL, {0}, 0},
    {"Stereo DAC MIXR DAC R1 Switch", "0", NULL, {0}, 0},
    {"Mono DAC MIXL DAC L1 Switch", "1", NULL, {0}, 0},
    {"Mono DAC MIXR DAC R1 Switch", "1", NULL, {0}, 0},
    {"Mono DAC MIXL DAC L2 Switch", "0", NULL, {0}, 0},
    {"Mono DAC MIXR DAC R2 Switch", "0", NULL, {0}, 0},
    {"FM Switch", "1", NULL, {0}, 0},
};
static struct pcm_config config = {
    .channels = 2, .rate = 48000, .period_size = 512, .period_count = 4,
    .format = PCM_FORMAT_S16_LE, .start_threshold = 0,
};
static void *tone(void *arg)
{
    struct pcm *out = arg;
    int16_t data[1024];
    unsigned int frame = 0;
    while (running) {
        for (unsigned int i = 0; i < 512; ++i, ++frame) {
            data[2*i] = 3000 * sin(2 * M_PI * 997 * frame / 48000.0);
            data[2*i+1] = 3000 * sin(2 * M_PI * 1511 * frame / 48000.0);
        }
        if (pcm_writei(out, data, 512) < 0) { fprintf(stderr, "tone: %s\n", pcm_get_error(out)); running = 0; }
    }
    return NULL;
}
static int control(int fd, unsigned int id, int value)
{
    struct v4l2_control c = {.id = id, .value = value};
    int ret = ioctl(fd, VIDIOC_S_CTRL, &c);
    if (ret < 0) perror("FM control");
    return ret;
}
static int sysfs_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror(path); return -1; }
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    if (n != (ssize_t)strlen(value)) { perror(path); return -1; }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 5 || (strcmp(argv[1], "reference") && strcmp(argv[1], "fm"))) {
        fprintf(stderr, "Usage: %s reference|fm output.s16le frequency_kHz seconds\n"
            "Optional bench overrides: MOCHA_FM_PINS=PCM-master|PCM-slave,\n"
            "MOCHA_FM_AUDIO_CTRL=0x2c, MOCHA_FM_I2S_FORMAT=8000 (hex).\n"
            "Restart device after overrides to restore codec defaults.\n", argv[0]);
        return 2;
    }
    const int reference = !strcmp(argv[1], "reference");
    if (atoi(argv[3]) < 87500 || atoi(argv[3]) > 108000 ||
            atoi(argv[4]) < 1 || atoi(argv[4]) > 120) return 2;
    int status = 1, fd = -1, started = 0;
    struct mixer *mixer = NULL;
    struct pcm *in = NULL, *out = NULL;
    FILE *file = NULL;
    pthread_t thread;
    signal(SIGINT, stop_probe); signal(SIGTERM, stop_probe);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!reference) {
        fd = open("/dev/radio0", O_RDONLY | O_NONBLOCK);
        if (fd < 0) { perror("radio0"); goto done; }
        struct v4l2_frequency f = {.type = V4L2_TUNER_RADIO, .frequency = atoi(argv[3]) * 16};
        if (ioctl(fd, VIDIOC_S_FREQUENCY, &f) < 0) { perror("tune"); goto done; }
        if (control(fd, V4L2_CID_AUDIO_VOLUME, 255) < 0 || control(fd, V4L2_CID_AUDIO_MUTE, 0) < 0) goto done;
        struct v4l2_tuner t = {0};
        if (ioctl(fd, VIDIOC_G_TUNER, &t) < 0) { perror("tuner"); goto done; }
        printf("FM requested=%d kHz signal=%u subchannels=0x%x\n", atoi(argv[3]), t.signal, t.rxsubchans);
        if (ioctl(fd, VIDIOC_G_FREQUENCY, &f) < 0) { perror("frequency readback"); goto done; }
        printf("FM readback=%u kHz\n", f.frequency/16);
        struct v4l2_control c = {.id = V4L2_CID_AUDIO_VOLUME};
        if (ioctl(fd, VIDIOC_G_CTRL, &c) < 0) { perror("volume readback"); goto done; }
        printf("FM volume=%d audmode=%u\n", c.value, t.audmode);
        if (getenv("MOCHA_FM_MONO")) {
            t.rangelow = t.rangehigh = 0; t.audmode = V4L2_TUNER_MODE_MONO;
            if (ioctl(fd, VIDIOC_S_TUNER, &t) < 0) { perror("force mono"); goto done; }
        }
        const char *pins = getenv("MOCHA_FM_PINS");
        if (pins && sysfs_write("/sys/class/video4linux/radio0/fmrx_fm_audio_pins", pins)) goto done;
        const char *ctrl = getenv("MOCHA_FM_AUDIO_CTRL");
        if (ctrl && sysfs_write("/sys/class/video4linux/radio0/fmrx_audio_ctrl", ctrl)) goto done;
        const char *fmt = getenv("MOCHA_FM_I2S_FORMAT");
        if (fmt) {
            char *end; unsigned long value = strtoul(fmt, &end, 16);
            if (*end || (value & ~0x8083UL)) { fprintf(stderr, "Invalid I2S format\n"); goto done; }
            char command[32]; snprintf(command, sizeof(command), "6f %04lx", value);
            if (sysfs_write("/sys/devices/platform/tegra12-i2c.0/i2c-0/0-001c/codec_reg", command)) goto done;
        }
    }
    mixer = mixer_open(1);
    if (!mixer) { perror("mixer"); goto done; }
    for (unsigned int i = 0; i < sizeof(route)/sizeof(route[0]); ++i) {
        struct setting *s = &route[i];
        s->ctl = mixer_get_ctl_by_name(mixer, s->name);
        if (!s->ctl || (s->n = mixer_ctl_get_num_values(s->ctl)) > 2) { fprintf(stderr, "Missing/unsupported control: %s\n", s->name); goto done; }
        for (unsigned int j = 0; j < s->n; ++j) s->old[j] = mixer_ctl_get_value(s->ctl, j);
        const char *value = s->value;
        if (reference && (!strcmp(s->name, "DAC1 L Mux") || !strcmp(s->name, "DAC1 R Mux"))) value = "IF1 DAC";
        if (!reference && getenv("MOCHA_FM_TRACK") && !strcmp(s->name, "DA STO ASRC Switch")) value = "clk_i2s4_track";
        int ret = 0;
        if (mixer_ctl_get_type(s->ctl) == MIXER_CTL_TYPE_ENUM) ret = mixer_ctl_set_enum_by_string(s->ctl, value);
        else for (unsigned int j = 0; j < s->n; ++j) ret |= mixer_ctl_set_value(s->ctl, j, atoi(value));
        if (ret < 0) { fprintf(stderr, "Failed setting %s=%s\n", s->name, value); goto done; }
    }
    in = pcm_open(1, 0, PCM_IN, &config);
    if (!in || !pcm_is_ready(in)) { fprintf(stderr, "capture: %s\n", pcm_get_error(in)); goto done; }
    if (reference) {
        out = pcm_open(1, 0, PCM_OUT, &config);
        if (!out || !pcm_is_ready(out)) { fprintf(stderr, "playback: %s\n", pcm_get_error(out)); goto done; }
        if (pthread_create(&thread, NULL, tone, out)) goto done;
        started = 1;
    }
    file = fopen(argv[2], "wb");
    if (!file) { perror("output"); goto done; }
    int16_t data[1024];
    double sum[2] = {0}; int peak[2] = {0}; unsigned int frames = 0;
    unsigned int limit = atoi(argv[4]) * config.rate;
    while (running && frames < limit) {
        int n = pcm_readi(in, data, 512);
        if (n < 0) { fprintf(stderr, "read: %s\n", pcm_get_error(in)); goto done; }
        if (fwrite(data, sizeof(int16_t)*2, n, file) != (unsigned int)n) { perror("write"); goto done; }
        for (int j = 0; j < n*2; ++j) {
            int v = abs(data[j]); if (v > peak[j&1]) peak[j&1] = v;
            sum[j&1] += (double)data[j]*data[j];
        }
        frames += n;
    }
    if (frames) printf("Captured frames=%u peak=%d,%d rms_dBFS=%.2f,%.2f\n", frames, peak[0], peak[1], 10*log10(sum[0]/frames/(32768.0*32768.0)), 10*log10(sum[1]/frames/(32768.0*32768.0)));
    status = 0;
done:
    running = 0;
    if (started) pthread_join(thread, NULL);
    if (file) fclose(file);
    if (out) pcm_close(out);
    if (in) pcm_close(in);
    for (int i = (int)(sizeof(route)/sizeof(route[0]))-1; i >= 0; --i)
        if (route[i].ctl) for (unsigned int j = 0; j < route[i].n; ++j) mixer_ctl_set_value(route[i].ctl, j, route[i].old[j]);
    if (mixer) mixer_close(mixer);
    if (fd >= 0) close(fd);
    return status;
}
