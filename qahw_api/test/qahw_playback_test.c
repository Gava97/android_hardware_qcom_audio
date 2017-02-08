/*
 * Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2015 The Android Open Source Project *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Test app to play audio at the HAL layer */

#include <getopt.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include "qahw_api.h"
#include "qahw_defs.h"
#include "qahw_effect_api.h"
#include "qahw_effect_test.h"

#define nullptr NULL

#define LATENCY_NODE "/sys/kernel/debug/audio_out_latency_measurement_node"
#define LATENCY_NODE_INIT_STR "1"

#define AFE_PROXY_SAMPLING_RATE 48000
#define AFE_PROXY_CHANNEL_COUNT 2

#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

#define FORMAT_PCM 1
#define WAV_HEADER_LENGTH_MAX 46

static thread_data_t *ethread_data = NULL;
static cmd_data_t cmd_data;

struct wav_header {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t riff_fmt;
    uint32_t fmt_id;
    uint32_t fmt_sz;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_id;
    uint32_t data_sz;
};

struct audio_config_params {
    qahw_module_handle_t *qahw_mod_handle;
    audio_io_handle_t handle;
    audio_devices_t input_device;
    audio_config_t config;
    audio_input_flags_t flags;
    const char* kStreamName ;
    audio_source_t kInputSource;
    char *file_name;
    volatile bool thread_exit;
};

struct proxy_data {
    struct audio_config_params acp;
    struct wav_header hdr;
};
FILE * log_file = NULL;
volatile bool stop_playback = false;
const char *log_filename = NULL;
float vol_level = 0.01;
pthread_t proxy_thread;

enum {
    FILE_WAV = 1,
    FILE_MP3,
    FILE_AAC,
    FILE_AAC_ADTS,
    FILE_FLAC,
    FILE_ALAC,
    FILE_VORBIS,
    FILE_WMA,
    FILE_AC3,
    FILE_AAC_LATM,
    FILE_EAC3,
    FILE_EAC3_JOC,
    FILE_DTS,
    FILE_MP2,
    FILE_APTX
};

typedef enum {
    AAC_LC = 1,
    AAC_HE_V1,
    AAC_HE_V2
} aac_format_type_t;

typedef enum {
    WMA = 1,
    WMA_PRO,
    WMA_LOSSLESS
} wma_format_type_t;

static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t drain_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drain_cond = PTHREAD_COND_INITIALIZER;

#define FLAC_KVPAIR "music_offload_avg_bit_rate=%d;" \
                    "music_offload_flac_max_blk_size=%d;" \
                    "music_offload_flac_max_frame_size=%d;" \
                    "music_offload_flac_min_blk_size=%d;" \
                    "music_offload_flac_min_frame_size=%d;" \
                    "music_offload_sample_rate=%d;"

#define ALAC_KVPAIR "music_offload_alac_avg_bit_rate=%d;" \
                    "music_offload_alac_bit_depth=%d;" \
                    "music_offload_alac_channel_layout_tag=%d;" \
                    "music_offload_alac_compatible_version=%d;" \
                    "music_offload_alac_frame_length=%d;" \
                    "music_offload_alac_kb=%d;" \
                    "music_offload_alac_max_frame_bytes=%d;" \
                    "music_offload_alac_max_run=%d;" \
                    "music_offload_alac_mb=%d;" \
                    "music_offload_alac_num_channels=%d;" \
                    "music_offload_alac_pb=%d;" \
                    "music_offload_alac_sampling_rate=%d;" \
                    "music_offload_avg_bit_rate=%d;" \
                    "music_offload_sample_rate=%d;"

#define VORBIS_KVPAIR "music_offload_avg_bit_rate=%d;" \
                      "music_offload_sample_rate=%d;" \
                      "music_offload_vorbis_bitstream_fmt=%d;"

#define WMA_KVPAIR "music_offload_avg_bit_rate=%d;" \
                   "music_offload_sample_rate=%d;" \
                   "music_offload_wma_bit_per_sample=%d;" \
                   "music_offload_wma_block_align=%d;" \
                   "music_offload_wma_channel_mask=%d;" \
                   "music_offload_wma_encode_option=%d;" \
                   "music_offload_wma_encode_option1=%d;" \
                   "music_offload_wma_encode_option2=%d;" \
                   "music_offload_wma_format_tag=%d;"

void stop_signal_handler(int signal __unused)
{
   stop_playback = true;
}

void read_kvpair(char *kvpair, char* kvpair_values, int filetype)
{
    char *kvpair_type = NULL;
    char *token = NULL;
    int value = 0;
    int len = 0;
    int size = 0;

    switch (filetype) {
    case FILE_FLAC:
        kvpair_type = FLAC_KVPAIR;
        break;
    case FILE_ALAC:
        kvpair_type = ALAC_KVPAIR;
        break;
    case FILE_VORBIS:
        kvpair_type = VORBIS_KVPAIR;
        break;
    case FILE_WMA:
        kvpair_type = WMA_KVPAIR;
        break;
    default:
        break;
    }

    if (kvpair_type) {
        token = strtok(kvpair_values, ",");
        while (token) {
            len = strcspn(kvpair_type, "=");
            size = len + strlen(token) + 2;
            value = atoi(token);
            snprintf(kvpair, size, kvpair_type, value);
            kvpair += size - 1;
            kvpair_type += len + 3;
            token = strtok(NULL, ",");
        }
    }
}

int async_callback(qahw_stream_callback_event_t event, void *param __unused,
                  void *cookie __unused)
{
    switch (event) {
    case QAHW_STREAM_CBK_EVENT_WRITE_READY:
        fprintf(log_file, "QAHW_STREAM_CBK_EVENT_DRAIN_READY\n");
        pthread_mutex_lock(&write_lock);
        pthread_cond_signal(&write_cond);
        pthread_mutex_unlock(&write_lock);
        break;
    case QAHW_STREAM_CBK_EVENT_DRAIN_READY:
        fprintf(log_file, "QAHW_STREAM_CBK_EVENT_DRAIN_READY\n");
        pthread_mutex_lock(&drain_lock);
        pthread_cond_signal(&drain_cond);
        pthread_mutex_unlock(&drain_lock);
    default:
        break;
    }
    return 0;
}

void *proxy_read (void* data)
{
    struct proxy_data* params = (struct proxy_data*) data;
    qahw_module_handle_t *qahw_mod_handle = params->acp.qahw_mod_handle;
    qahw_in_buffer_t in_buf;
    char *buffer;
    int rc = 0;
    int bytes_to_read, bytes_written = 0;
    FILE *fp = NULL;
    qahw_stream_handle_t* in_handle = nullptr;

    rc = qahw_open_input_stream(qahw_mod_handle, params->acp.handle,
              params->acp.input_device, &params->acp.config, &in_handle,
              params->acp.flags, params->acp.kStreamName, params->acp.kInputSource);
    if (rc) {
        fprintf(log_file, "Could not open input stream %d \n",rc);
        fprintf(stderr, "Could not open input stream %d \n",rc);
        pthread_exit(0);
     }

    if (in_handle != NULL) {
        bytes_to_read = qahw_in_get_buffer_size(in_handle);
        buffer = (char *) calloc(1, bytes_to_read);
        if (buffer == NULL) {
            fprintf(log_file, "calloc failed!!\n");
            fprintf(stderr, "calloc failed!!\n");
            pthread_exit(0);
        }

        if ((fp = fopen(params->acp.file_name,"w"))== NULL) {
            fprintf(log_file, "Cannot open file to dump proxy data\n");
            fprintf(stderr, "Cannot open file to dump proxy data\n");
            pthread_exit(0);
        }
        else {
          params->hdr.num_channels = audio_channel_count_from_in_mask(params->acp.config.channel_mask);
          params->hdr.sample_rate = params->acp.config.sample_rate;
          params->hdr.byte_rate = params->hdr.sample_rate * params->hdr.num_channels * 2;
          params->hdr.block_align = params->hdr.num_channels * 2;
          params->hdr.bits_per_sample = 16;
          fwrite(&params->hdr, 1, sizeof(params->hdr), fp);
        }
        memset(&in_buf,0, sizeof(qahw_in_buffer_t));
        in_buf.buffer = buffer;
        in_buf.bytes = bytes_to_read;

        while (!(params->acp.thread_exit)) {
            rc = qahw_in_read(in_handle, &in_buf);
            if (rc > 0) {
                bytes_written += fwrite((char *)(in_buf.buffer), sizeof(char), (int)in_buf.bytes, fp);
            }
        }
        params->hdr.data_sz = bytes_written;
        params->hdr.riff_sz = bytes_written + 36; //sizeof(hdr) - sizeof(riff_id) - sizeof(riff_sz)
        fseek(fp, 0L , SEEK_SET);
        fwrite(&params->hdr, 1, sizeof(params->hdr), fp);
        fclose(fp);
        rc = qahw_in_standby(in_handle);
        if (rc) {
            fprintf(log_file, "in standby failed %d \n", rc);
            fprintf(stderr, "in standby failed %d \n", rc);
        }
        rc = qahw_close_input_stream(in_handle);
        if (rc) {
            fprintf(log_file, "could not close input stream %d \n", rc);
            fprintf(stderr, "could not close input stream %d \n", rc);
        }
        fprintf(log_file, "pcm data saved to file %s", params->acp.file_name);
    }
    return 0;
}

int write_to_hal(qahw_stream_handle_t* out_handle, char *data,
              size_t bytes)
{
    ssize_t ret;
    pthread_mutex_lock(&write_lock);
    qahw_out_buffer_t out_buf;

    memset(&out_buf,0, sizeof(qahw_out_buffer_t));
    out_buf.buffer = data;
    out_buf.bytes = bytes;

    ret = qahw_out_write(out_handle, &out_buf);
    if (ret < 0 || ret == bytes) {
        fprintf(log_file, "Writing data to hal failed or full write %zd, %zd\n",
            ret, bytes);
    } else if (ret != bytes) {
        fprintf(log_file, "ret %zd, bytes %zd\n", ret, bytes);
        fprintf(log_file, "Waiting for event write ready\n");
        pthread_cond_wait(&write_cond, &write_lock);
        fprintf(log_file, "out of wait for event write ready\n");
    }

    pthread_mutex_unlock(&write_lock);
    return ret;
}


/* Play audio from a WAV file.
 *
 * Parameters:
 *  out_stream: A pointer to the output audio stream.
 *  in_file: A pointer to a SNDFILE object.
 *  config: A pointer to struct that contains audio configuration data.
 *
 * Returns: An int which has a non-negative number on success.
 */

int play_file(qahw_stream_handle_t* out_handle, FILE* in_file,
              bool is_offload) {
    int rc = 0;
    int offset = 0;
    size_t bytes_wanted = 0;
    size_t write_length = 0;
    size_t bytes_remaining = 0;
    size_t bytes_written = 0;
    size_t bytes_read = 0;
    char  *data = NULL;
    bool exit = false;

    if (is_offload) {
        fprintf(log_file, "Set callback for offload stream\n");
        qahw_out_set_callback(out_handle, async_callback, NULL);
    }

    rc = qahw_out_set_volume(out_handle, vol_level, vol_level);
    if (rc < 0)
        fprintf(log_file, "unable to set volume");

    bytes_wanted = qahw_out_get_buffer_size(out_handle);
    data = (char *) malloc (bytes_wanted);
    if (data == NULL) {
        fprintf(log_file, "calloc failed!!\n");
        return -ENOMEM;
    }

    while (!exit && !stop_playback) {
        if (!bytes_remaining) {
            bytes_read = fread(data, 1, bytes_wanted, in_file);
            fprintf(log_file, "fread from file %zd\n", bytes_read);
            if (bytes_read <= 0) {
                if (feof(in_file)) {
                    fprintf(log_file, "End of file\n");
                    if (is_offload) {
                        pthread_mutex_lock(&drain_lock);
                        qahw_out_drain(out_handle, QAHW_DRAIN_ALL);
                        pthread_cond_wait(&drain_cond, &drain_lock);
                        fprintf(log_file, "Out of compress drain\n");
                        fprintf(stdout, "Playback completed sucessfully\n");
                        pthread_mutex_unlock(&drain_lock);
                    }
                } else {
                    fprintf(log_file, "Error in fread --%d\n", ferror(in_file));
                    fprintf(stderr, "Error in fread --%d\n", ferror(in_file));
                }
                exit = true;
                continue;
            }
            bytes_remaining = write_length = bytes_read;
        }

        offset = write_length - bytes_remaining;
        fprintf(log_file, "bytes_remaining %zd, offset %d, write length %zd\n",
                bytes_remaining, offset, write_length);
        bytes_written = write_to_hal(out_handle, data+offset, bytes_remaining);
        bytes_remaining -= bytes_written;
        fprintf(log_file, "bytes_written %zd, bytes_remaining %zd\n",
                bytes_written, bytes_remaining);
    }

    return rc;
}

bool is_valid_aac_format_type(aac_format_type_t format_type)
{
    bool valid_format_type = false;

    switch (format_type) {
    case AAC_LC:
    case AAC_HE_V1:
    case AAC_HE_V2:
        valid_format_type = true;
        break;
    default:
        break;
    }
    return valid_format_type;
}

/*
 * Obtain aac format (refer audio.h) for format type entered.
 */

audio_format_t get_aac_format(int filetype, aac_format_type_t format_type)
{
    audio_format_t aac_format = AUDIO_FORMAT_AAC_ADTS_LC; /* default aac frmt*/

    if (filetype == FILE_AAC_ADTS) {
        switch (format_type) {
        case AAC_LC:
            aac_format = AUDIO_FORMAT_AAC_ADTS_LC;
            break;
        case AAC_HE_V1:
            aac_format = AUDIO_FORMAT_AAC_ADTS_HE_V1;
            break;
        case AAC_HE_V2:
            aac_format = AUDIO_FORMAT_AAC_ADTS_HE_V2;
            break;
        default:
            break;
        }
    } else if (filetype == FILE_AAC) {
        switch (format_type) {
        case AAC_LC:
            aac_format = AUDIO_FORMAT_AAC_LC;
            break;
        case AAC_HE_V1:
            aac_format = AUDIO_FORMAT_AAC_HE_V1;
            break;
        case AAC_HE_V2:
            aac_format = AUDIO_FORMAT_AAC_HE_V2;
            break;
        default:
            break;
        }
    } else if (filetype == FILE_AAC_LATM) {
        switch (format_type) {
        case AAC_LC:
            aac_format = AUDIO_FORMAT_AAC_LATM_LC;
            break;
        case AAC_HE_V1:
            aac_format = AUDIO_FORMAT_AAC_LATM_HE_V1;
            break;
        case AAC_HE_V2:
            aac_format = AUDIO_FORMAT_AAC_LATM_HE_V2;
            break;
        default:
            break;
        }
    } else {
        fprintf(log_file, "Invalid filetype provided %d\n", filetype);
        fprintf(stderr, "Invalid filetype provided %d\n", filetype);
    }

    fprintf(log_file, "aac format %d\n", aac_format);
    return aac_format;
}

int measure_kpi_values(qahw_stream_handle_t* out_handle, bool is_offload) {
    int rc = 0;
    int offset = 0;
    size_t bytes_wanted = 0;
    size_t write_length = 0;
    size_t bytes_remaining = 0;
    size_t bytes_written = 0;
    char  *data = NULL;
    int ret = 0, count = 0;
    struct timespec ts_cold, ts_cont;
    uint64_t tcold, tcont, scold = 0, uscold = 0, scont = 0, uscont = 0;

    if (is_offload) {
        fprintf(log_file, "Set callback for offload stream\n");
        qahw_out_set_callback(out_handle, async_callback, NULL);
    }

    FILE *fd_latency_node = fopen(LATENCY_NODE, "r+");
    if (fd_latency_node) {
        ret = fwrite(LATENCY_NODE_INIT_STR, sizeof(LATENCY_NODE_INIT_STR), 1, fd_latency_node);
        if (ret<1)
            fprintf(log_file, "error(%d) writing to debug node!", ret);
        fflush(fd_latency_node);
    } else {
        fprintf(log_file, "debug node(%s) open failed!", LATENCY_NODE);
        return -1;
    }

    bytes_wanted = qahw_out_get_buffer_size(out_handle);
    data = (char *) calloc (1, bytes_wanted);
    if (data == NULL) {
        fprintf(log_file, "calloc failed!!\n");
        return -ENOMEM;
    }

    while (count < 64) {
        if (!bytes_remaining) {
            bytes_remaining = write_length = bytes_wanted;
        }
        if (count == 0) {
            ret = clock_gettime(CLOCK_REALTIME, &ts_cold);
            if (ret) {
                fprintf(log_file, "error(%d) fetching start time for cold latency", ret);
                return -1;
            }
        } else if (count == 16) {
            int *d = (int *)data;
            d[0] = 0x01010000;
            ret = clock_gettime(CLOCK_REALTIME, &ts_cont);
            if (ret) {
                fprintf(log_file, "error(%d) fetching start time for continuous latency", ret);
                return -1;
            }
        }

        offset = write_length - bytes_remaining;
        bytes_written = write_to_hal(out_handle, data+offset, bytes_remaining);
        bytes_remaining -= bytes_written;
        fprintf(log_file, "bytes_written %zd, bytes_remaining %zd\n",
                bytes_written, bytes_remaining);

        if (count == 16) {
            int *i = (int *)data;
            i[0] = 0x00000000;
        }
        count++;
    }

    char latency_buf[200] = {0};
    fread((void *) latency_buf, 100, 1, fd_latency_node);
    fclose(fd_latency_node);
    sscanf(latency_buf, " %llu,%llu,%*llu,%*llu,%llu,%llu", &scold, &uscold, &scont, &uscont);
    tcold = scold*1000 - ts_cold.tv_sec*1000 + uscold/1000 - ts_cold.tv_nsec/1000000;
    tcont = scont*1000 - ts_cont.tv_sec*1000 + uscont/1000 - ts_cont.tv_nsec/1000000;
    fprintf(log_file, "\n values from debug node %s\n", latency_buf);
    fprintf(log_file, " cold latency %llums, continuous latency %llums,\n", tcold, tcont);
    fprintf(log_file, " **Note: please add DSP Pipe/PP latency numbers to this, for final latency values\n");
    return rc;
}

void parse_aptx_dec_bt_addr(char *value, struct qahw_aptx_dec_param *aptx_cfg)
{
    int ba[6];
    char *str, *tok;
    uint32_t addr[3];
    int i = 0;

    tok = strtok_r(value, ":", &str);
    while (tok != NULL) {
        ba[i] = strtol(tok, NULL, 16);
        i++;
        tok = strtok_r(NULL, ":", &str);
    }
    addr[0] = (ba[0] << 8) | ba[1];
    addr[1] = ba[2];
    addr[2] = (ba[3] << 16) | (ba[4] << 8) | ba[5];

    aptx_cfg->bt_addr.nap = addr[0];
    aptx_cfg->bt_addr.uap = addr[1];
    aptx_cfg->bt_addr.lap = addr[2];
}

void usage() {
    printf(" \n Command \n");
    printf(" \n hal_play_test -f file-path <options>   - Plays audio file from the path provided\n");
    printf(" \n Options\n");
    printf(" -f  --file-path <file path>               - file path to be used for playback.\n");
    printf("                                             file path must be provided unless -K(--kpi) is used\n\n");
    printf(" -r  --sample-rate <sampling rate>         - Required for Non-WAV streams\n");
    printf("                                             For AAC-HE pls specify half the sample rate\n\n");
    printf(" -c  --channel count <channels>            - Required for Non-WAV streams\n\n");
    printf(" -b  --bitwidth <bitwidth>                 - Give either 16 or 24.Default value is 16.\n\n");
    printf(" -v  --volume <float volume level>         - Volume level float value between 0.0 - 1.0.\n");
    printf(" -d  --device <decimal value>              - see system/media/audio/include/system/audio.h for device values\n");
    printf("                                             Optional Argument and Default value is 2, i.e Speaker\n\n");
    printf(" -t  --file-type <file type>               - 1:WAV 2:MP3 3:AAC 4:AAC_ADTS 5:FLAC\n");
    printf("                                             6:ALAC 7:VORBIS 8:WMA 10:AAC_LATM \n");
    printf("                                             Required for non WAV formats\n\n");
    printf(" -a  --aac-type <aac type>                 - Required for AAC streams\n");
    printf("                                             1: LC 2: HE_V1 3: HE_V2\n\n");
    printf(" -w  --wma-type <wma type>                 - Required for WMA clips.Default vlaue is 1\n");
    printf("                                             1: WMA 2: WMAPRO 3:WMA_LOSSLESS \n\n");
    printf(" -k  --kvpairs <values>                    - Metadata information of clip\n");
    printf("                                             See Example for more info\n\n");
    printf(" -l  --log-file <ABSOLUTE FILEPATH>        - File path for debug msg, to print\n");
    printf("                                             on console use stdout or 1 \n\n");
    printf(" -D  --dump-file <ABSOLUTE FILEPATH>       - File path to dump pcm data from proxy\n");
    printf(" -F  --flags <int value for output flags>  - Output flag to be used\n\n");
    printf(" -k  --kpi-mode                            - Required for Latency KPI measurement\n");
    printf("                                             file path is not used here as file playback is not done in this mode\n");
    printf("                                             file path and other file specific options would be ignored in this mode.\n\n");
    printf(" -e  --effect-type <effect type>           - Effect used for test\n");
    printf("                                             0:bassboost 1:virtualizer 2:equalizer 3:visualizer(NA) 4:reverb 5:audiosphere others:null");
    printf(" -A  --bt-addr <bt device addr>            - Required to set bt device adress for aptx decoder\n");
    printf(" \n Examples \n");
    printf(" hal_play_test -f /etc/Anukoledenadu.wav     -> plays Wav stream with default params\n\n");
    printf(" hal_play_test -f /etc/MateRani.mp3 -t 2 -d 2 -v 0.01 -r 44100 -c 2 \n");
    printf("                                          -> plays MP3 stream(-t = 2) on speaker device(-d = 2)\n");
    printf("                                          -> 2 channels and 44100 sample rate\n\n");
    printf(" hal_play_test -f /etc/v1-CBR-32kHz-stereo-40kbps.mp3 -t 2 -d 128 -v 0.01 -r 32000 -c 2 -D /data/proxy_dump.wav\n");
    printf("                                          -> plays MP3 stream(-t = 2) on BT device(-d = 128)\n");
    printf("                                          -> 2 channels and 32000 sample rate\n");
    printf("                                          -> dumps pcm data to file at /data/proxy_dump.wav\n\n");
    printf(" hal_play_test -f /etc/AACLC-71-48000Hz-384000bps.aac  -t 4 -d 2 -v 0.05 -r 48000 -c 2 -a 1 \n");
    printf("                                          -> plays AAC-ADTS stream(-t = 4) on speaker device(-d = 2)\n");
    printf("                                          -> AAC format type is LC(-a = 1)\n");
    printf("                                          -> 2 channels and 48000 sample rate\n\n");
    printf(" hal_play_test -f /etc/AACHE-adts-stereo-32000KHz-128000Kbps.aac  -t 4 -d 2 -v 0.05 -r 16000 -c 2 -a 3 \n");
    printf("                                          -> plays AAC-ADTS stream(-t = 4) on speaker device(-d = 2)\n");
    printf("                                          -> AAC format type is HE V2(-a = 3)\n");
    printf("                                          -> 2 channels and 16000 sample rate\n");
    printf("                                          -> note that the sample rate is half the actual sample rate\n\n");
    printf(" hal_play_test -f /etc/2.0_16bit_48khz.m4a -k 1536000,16,0,0,4096,14,16388,0,10,2,40,48000,1536000,48000 -t 6 -r 48000 -c 2 -v 0.5 \n");
    printf("                                          -> Play alac clip (-t = 6)\n");
    printf("                                          -> kvpair(-k) values represent media-info of clip & values should be in below mentioned sequence\n");
    printf("                                          ->alac_avg_bit_rate,alac_bit_depth,alac_channel_layout,alac_compatible_version,\n");
    printf("                                          ->alac_frame_length,alac_kb,alac_max_frame_bytes,alac_max_run,alac_mb,\n");
    printf("                                          ->alac_num_channels,alac_pb,alac_sampling_rate,avg_bit_rate,sample_rate\n\n");
    printf(" hal_play_test -f /etc/DIL CHAHTA HAI.flac -k 0,4096,13740,4096,14 -t 5 -r 48000 -c 2 -v 0.5 \n");
    printf("                                          -> Play flac clip (-t = 5)\n");
    printf("                                          -> kvpair(-k) values represent media-info of clip & values should be in below mentioned sequence\n");
    printf("                                          ->avg_bit_rate,flac_max_blk_size,flac_max_frame_size\n");
    printf("                                          ->flac_min_blk_size,flac_min_frame_size,sample_rate\n");
    printf(" hal_play_test -f /etc/vorbis.mka -k 500000,48000,1 -t 7 -r 48000 -c 2 -v 0.5 \n");
    printf("                                          -> Play vorbis clip (-t = 7)\n");
    printf("                                          -> kvpair(-k) values represent media-info of clip & values should be in below mentioned sequence\n");
    printf("                                          ->avg_bit_rate,sample_rate,vorbis_bitstream_fmt\n");
    printf(" hal_play_test -f /etc/file.wma -k 192000,48000,16,8192,3,15,0,0,353 -t 8 -w 1 -r 48000 -c 2 -v 0.5 \n");
    printf("                                          -> Play wma clip (-t = 8)\n");
    printf("                                          -> kvpair(-k) values represent media-info of clip & values should be in below mentioned sequence\n");
    printf("                                          ->avg_bit_rate,sample_rate,wma_bit_per_sample,wma_block_align\n");
    printf("                                          ->wma_channel_mask,wma_encode_option,wma_format_tag\n");
    printf(" hal_play_test -f /etc/03_Kuch_Khaas_BE.btaptx -t 9 -d 2 -v 0.2 -r 44100 -c 2 -A 00:02:5b:00:ff:03 \n");
    printf("                                          -> Play aptx clip (-t = 9)\n");
    printf("                                          -> 2 channels and 44100 sample rate\n");
    printf("                                          -> BT addr: bt_addr=00:02:5b:00:ff:03\n");
    printf(" hal_play_test -K -F 4                    -> Measure latency KPIs for low latency output\n\n");
    printf(" hal_play_test -f /etc//Moto_320kbps.mp3 -t 2 -d 2 -v 0.1 -r 44100 -c 2 -e 2\n");
    printf("                                          -> plays MP3 stream(-t = 2) on speaker device(-d = 2)\n");
    printf("                                          -> 2 channels and 44100 sample rate\n\n");
    printf("                                          -> sound effect equalizer enabled\n\n");
}

static int get_wav_header_length (FILE* file_stream)
{
    int subchunk_size = 0, wav_header_len = 0;

    fseek(file_stream, 16, SEEK_SET);
    if(fread(&subchunk_size, 4, 1, file_stream) != 1) {
        fprintf(stdout, "Unable to read subchunk:\n");
        exit (1);
    }
    if(subchunk_size < 16) {
        fprintf(stdout, "This is not a valid wav file \n");
    } else {
          switch (subchunk_size) {
          case 16:
              fprintf(stdout, "44-byte wav header \n");
              wav_header_len = 44;
              break;
          case 18:
              fprintf(stdout, "46-byte wav header \n");
              wav_header_len = 46;
              break;
          default:
              fprintf(stdout, "Header contains extra data and is larger than 46 bytes: subchunk_size=%d \n", subchunk_size);
              wav_header_len = subchunk_size;
              break;
          }
    }
    return wav_header_len;
}


int main(int argc, char* argv[]) {

    FILE *file_stream = NULL;
    char header[WAV_HEADER_LENGTH_MAX] = {0};
    char* filename = nullptr;
    qahw_module_handle_t *qahw_mod_handle = NULL;
    const char *mod_name = "audio.primary";
    qahw_stream_handle_t* out_handle = nullptr;
    int rc = 0, wav_header_len = 0;
    char *kvpair_values = NULL;
    char kvpair[1000] = {0};
    struct proxy_data proxy_params;
    char *ba = NULL;
    qahw_param_payload payload;
    qahw_param_id param_id;
    struct qahw_aptx_dec_param aptx_params;
    /*
     * Default values
     */
    bool kpi_mode = false;
    bool flags_set = false;
    bool proxy_thread_active = false;
    int filetype = FILE_WAV;
    int sample_rate = 44100;
    int channels = 2;
    int bitwidth = 16;
    aac_format_type_t format_type = AAC_LC;
    wma_format_type_t wma_format_type = WMA;
    log_file = stdout;
    audio_devices_t output_device = AUDIO_DEVICE_OUT_SPEAKER;
    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
    proxy_params.acp.file_name = "/data/pcm_dump.wav";

    struct option long_options[] = {
        /* These options set a flag. */
        {"file-path",     required_argument,    0, 'f'},
        {"device",        required_argument,    0, 'd'},
        {"sample-rate",   required_argument,    0, 'r'},
        {"channels",      required_argument,    0, 'c'},
        {"bitwidth",      required_argument,    0, 'b'},
        {"volume",        required_argument,    0, 'v'},
        {"log-file",      required_argument,    0, 'l'},
        {"dump-file",     required_argument,    0, 'D'},
        {"file-type",     required_argument,    0, 't'},
        {"aac-type",      required_argument,    0, 'a'},
        {"wma-type",      required_argument,    0, 'w'},
        {"kvpairs",       required_argument,    0, 'k'},
        {"flags",         required_argument,    0, 'F'},
        {"kpi-mode",      no_argument,          0, 'K'},
        {"effect-path",   required_argument,    0, 'e'},
        {"bt-addr",       required_argument,    0, 'A'},
        {"help",          no_argument,          0, 'h'},
        {0, 0, 0, 0}
    };

    int opt = 0;
    int option_index = 0;
    int effect_index = -1;
    thread_func_t ethread_func = NULL;
    proxy_params.hdr.riff_id = ID_RIFF;
    proxy_params.hdr.riff_sz = 0;
    proxy_params.hdr.riff_fmt = ID_WAVE;
    proxy_params.hdr.fmt_id = ID_FMT;
    proxy_params.hdr.fmt_sz = 16;
    proxy_params.hdr.audio_format = FORMAT_PCM;
    proxy_params.hdr.num_channels = channels;
    proxy_params.hdr.sample_rate = sample_rate;
    proxy_params.hdr.byte_rate = proxy_params.hdr.sample_rate * proxy_params.hdr.num_channels * 2;
    proxy_params.hdr.block_align = proxy_params.hdr.num_channels * 2;
    proxy_params.hdr.bits_per_sample = 16;
    proxy_params.hdr.data_id = ID_DATA;
    proxy_params.hdr.data_sz = 0;
    while ((opt = getopt_long(argc,
                              argv,
                              "-f:r:c:b:d:v:l:t:a:w:k:D:KF:e:A:h",
                              long_options,
                              &option_index)) != -1) {
            switch (opt) {
            case 'f':
                filename = optarg;
                break;
            case 'r':
                sample_rate = atoi(optarg);
                break;
            case 'c':
                channels = atoi(optarg);
                break;
            case 'b':
                bitwidth = atoi(optarg);
                break;
            case 'd':
                output_device = atoll(optarg);
                break;
            case 'v':
                vol_level = atof(optarg);
                break;
            case 'l':
                log_filename = optarg;
                if (strcasecmp(log_filename, "stdout") &&
                    strcasecmp(log_filename, "1") &&
                    (log_file = fopen(log_filename,"wb")) == NULL) {
                    fprintf(stderr, "Cannot open log file %s\n", log_filename);
                    /* continue to log to std out. */
                    log_file = stdout;
                }

                break;
            case 't':
                filetype = atoi(optarg);
                break;
            case 'a':
                format_type = atoi(optarg);
                break;
            case 'w':
                wma_format_type = atoi(optarg);
                break;
            case 'k':
                kvpair_values = optarg;
                break;
            case 'D':
                proxy_params.acp.file_name = optarg;
                break;
            case 'K':
                kpi_mode = true;
                break;
            case 'F':
                flags = atoll(optarg);
                flags_set = true;
                break;
            case 'e':
                effect_index = atoi(optarg);
                if (effect_index < 0 || effect_index >= EFFECT_MAX) {
                    fprintf(stderr, "Invalid effect type %d\n", effect_index);
                    effect_index = -1;
                } else if (effect_index == 3) {
                    // visualizer is a special effect that is not perceivable by hearing
                    // hence, add as an exception in test app.
                    fprintf(stdout, "visualizer effect testing is not available\n");
                    effect_index = -1;
                } else {
                    ethread_func = effect_thread_funcs[effect_index];
                }
                break;
            case 'A':
                ba = optarg;
                break;
            case 'h':
                usage();
                return 0;
                break;
         }
    }

    if (!kpi_mode) {
        if (filename == nullptr) {
            fprintf(stderr, "File name is must for non kpi-mode\n");
            usage();
            goto EXIT;
        }
        if ((file_stream = fopen(filename, "r"))== NULL) {
            fprintf(stderr, "Cannot Open Audio File %s\n", filename);
            goto EXIT;
        }
        fprintf(stdout, "Playing:%s\n", filename);
        fprintf(stdout, "File Type:%d\n", filetype);
    }

    fprintf(stdout, "Sample Rate:%d\n", sample_rate);
    fprintf(stdout, "Channels:%d\n", channels);
    fprintf(stdout, "Bitwidth:%d\n", bitwidth);
    fprintf(stdout, "Log file:%s\n", log_filename);
    fprintf(stdout, "Volume level:%f\n", vol_level);
    fprintf(stdout, "Output Device:%d\n", output_device);
    fprintf(stdout, "Format Type:%d\n", format_type);
    fprintf(stdout, "kvpair values:%s\n", kvpair_values);
    fprintf(stdout, "Output Flags:%d\n", flags);

    /*
     * Set to a high number so it doesn't interfere with existing stream handles
     */
    audio_io_handle_t handle = 0x999;

    if (output_device & AUDIO_DEVICE_OUT_ALL_A2DP)
        fprintf(stdout, "Saving pcm data to file: %s\n", proxy_params.acp.file_name);

    if (effect_index != -1)
        fprintf(stdout, "Effect type:%s\n", effect_str[effect_index]);

    fprintf(stdout, "Starting audio hal tests.\n");

    qahw_mod_handle = qahw_load_module(mod_name);

    /*
     * set device connection state for HDMI.
     */
     if (output_device == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
         char param[100] = {0};
         snprintf(param, sizeof(param), "%s=%d", "connect", AUDIO_DEVICE_OUT_AUX_DIGITAL);
         qahw_set_parameters(qahw_mod_handle, param);
     }

    audio_config_t config;
    memset(&config, 0, sizeof(audio_config_t));

    if (kpi_mode) {
        config.offload_info.format = AUDIO_FORMAT_PCM_16_BIT;
    } else {

        if (!flags_set) {
            flags = AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD|AUDIO_OUTPUT_FLAG_NON_BLOCKING;
            flags |= AUDIO_OUTPUT_FLAG_DIRECT;
        }

        switch (filetype) {
        case FILE_WAV:
            /*
             * Read the wave header
             */
            if((wav_header_len = get_wav_header_length(file_stream)) <= 0) {
                fprintf(stdout, "wav header length is invalid:%d\n", wav_header_len);
                exit(1);
            }
            fseek(file_stream, 0, SEEK_SET);
            rc = fread (header, wav_header_len, 1, file_stream);
            if (rc != 1) {
               fprintf(stdout, "Error .Fread failed\n");
               exit(0);
            }
            if (strncmp (header, "RIFF", 4) && strncmp (header+8, "WAVE", 4)) {
               fprintf(stdout, "Not a wave format\n");
               exit (1);
            }
            memcpy (&channels, &header[22], 2);
            memcpy (&sample_rate, &header[24], 4);
            memcpy (&bitwidth, &header[34], 2);
            if (bitwidth == 32)
                config.offload_info.format = AUDIO_FORMAT_PCM_32_BIT;
            else if (bitwidth == 24)
                config.offload_info.format = AUDIO_FORMAT_PCM_24_BIT_PACKED;
            else
                config.offload_info.format = AUDIO_FORMAT_PCM_16_BIT;
            if (!flags_set)
                flags = AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
            break;

        case FILE_MP3:
            config.offload_info.format = AUDIO_FORMAT_MP3;
            break;

        case FILE_AAC:
        case FILE_AAC_ADTS:
        case FILE_AAC_LATM:
            if (!is_valid_aac_format_type(format_type)) {
                fprintf(log_file, "Invalid format type for AAC %d\n", format_type);
                goto EXIT;
            }
            config.offload_info.format = get_aac_format(filetype, format_type);
            break;
        case FILE_FLAC:
            config.offload_info.format = AUDIO_FORMAT_FLAC;
            break;
        case FILE_ALAC:
            config.offload_info.format = AUDIO_FORMAT_ALAC;
            break;
        case FILE_VORBIS:
            config.offload_info.format = AUDIO_FORMAT_VORBIS;
            break;
        case FILE_WMA:
            if (wma_format_type == WMA)
               config.offload_info.format = AUDIO_FORMAT_WMA;
            else
               config.offload_info.format = AUDIO_FORMAT_WMA_PRO;
            break;
        case FILE_MP2:
            config.offload_info.format = AUDIO_FORMAT_MP2;
            break;
        case FILE_AC3:
            config.offload_info.format = AUDIO_FORMAT_AC3;
            break;
        case FILE_EAC3:
        case FILE_EAC3_JOC:
            config.offload_info.format = AUDIO_FORMAT_E_AC3;
            break;
        case FILE_DTS:
            config.offload_info.format = AUDIO_FORMAT_DTS;
            break;
        case FILE_APTX:
            config.offload_info.format = AUDIO_FORMAT_APTX;
            break;
        default:
           fprintf(stderr, "Does not support given filetype\n");
           usage();
           return 0;
        }
    }
    config.channel_mask = audio_channel_out_mask_from_count(channels);
    config.sample_rate = sample_rate;
    config.format = config.offload_info.format;
    config.offload_info.channel_mask = config.channel_mask;
    config.offload_info.sample_rate = sample_rate;
    config.offload_info.bit_width = bitwidth;
    config.offload_info.version = AUDIO_OFFLOAD_INFO_VERSION_CURRENT;
    config.offload_info.size = sizeof(audio_offload_info_t);

    fprintf(log_file, "Now playing to output_device=%d sample_rate=%d \n"
        , output_device, config.offload_info.sample_rate);
    const char* stream_name = "output_stream";

    if (output_device & AUDIO_DEVICE_OUT_ALL_A2DP) {
        output_device = AUDIO_DEVICE_OUT_PROXY;
    }
    if (filetype == FILE_APTX) {
        if (ba != NULL) {
            parse_aptx_dec_bt_addr(ba, &aptx_params);
            payload = (qahw_param_payload)aptx_params;
            param_id = QAHW_PARAM_APTX_DEC;
            fprintf(log_file, "Send BT addr nap %d, uap %d lap %d to HAL\n", aptx_params.bt_addr.nap,
                        aptx_params.bt_addr.uap, aptx_params.bt_addr.lap);
            rc = qahw_set_param_data(qahw_mod_handle, param_id, &payload);
            if (rc != 0)
                 printf("Error.Failed Set BT addr\n");
        } else {
            fprintf(log_file, "BT addr is NULL, Need valid BT addr for aptx file playback to work\n");
            fprintf(stderr, "BT addr is NULL, Need valid BT addr for aptx file playback to work\n");
            goto EXIT;
        }
    }

    fprintf(log_file, "calling open_out_put_stream:\n");
    rc = qahw_open_output_stream(qahw_mod_handle,
                                 handle,
                                 output_device,
                                 flags,
                                 &config,
                                 &out_handle,
                                 stream_name);
    fprintf(log_file, "open output stream is sucess:%d  out_handhle %p\n"
        , rc, out_handle);
    if (rc) {
        fprintf(stdout, "could not open output stream %d \n", rc);
        goto EXIT;
    }

    if (kpi_mode) {
        measure_kpi_values(out_handle, (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD));
        goto EXIT;
    }

    switch(filetype) {
    case FILE_WMA:
    case FILE_VORBIS:
    case FILE_ALAC:
    case FILE_FLAC:
        if (!kvpair_values) {
           fprintf (log_file, "Error!!No metadata for the Clip\n");
           fprintf (stdout, "Error!!No metadata for the Clip\n");
           goto EXIT;
        }
        read_kvpair(kvpair, kvpair_values, filetype);
        qahw_out_set_parameters(out_handle, kvpair);
        break;
    default:
        break;
    }

    if (output_device & AUDIO_DEVICE_OUT_PROXY) {
        proxy_params.acp.qahw_mod_handle = qahw_mod_handle;
        proxy_params.acp.handle = 0x998;
        proxy_params.acp.input_device = AUDIO_DEVICE_IN_PROXY;
        proxy_params.acp.flags = AUDIO_INPUT_FLAG_NONE;
        proxy_params.acp.config.channel_mask = audio_channel_in_mask_from_count(AFE_PROXY_CHANNEL_COUNT);
        proxy_params.acp.config.sample_rate = AFE_PROXY_SAMPLING_RATE;
        proxy_params.acp.config.format = AUDIO_FORMAT_PCM_16_BIT;
        proxy_params.acp.kStreamName = "input_stream";
        proxy_params.acp.kInputSource = AUDIO_SOURCE_UNPROCESSED;
        proxy_params.acp.thread_exit = false;
        fprintf(log_file, "create thread to read data from proxy \n");
        rc = pthread_create(&proxy_thread, NULL, proxy_read, (void *)&proxy_params);
        if (!rc)
            proxy_thread_active = true;
    }

    // create effect thread, use thread_data to transfer command
    if (ethread_func &&
            (flags & (AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD|AUDIO_OUTPUT_FLAG_DIRECT))) {
        ethread_data = create_effect_thread(effect_index, ethread_func);

        // create effect command thread
        cmd_data.exit = false;
        cmd_data.fx_data_ptr = &ethread_data;
        pthread_attr_init(&cmd_data.attr);
        pthread_attr_setdetachstate(&cmd_data.attr, PTHREAD_CREATE_JOINABLE);
        rc = pthread_create(&cmd_data.cmd_thread, &cmd_data.attr,
                &command_thread_func, &cmd_data);
        if (rc < 0) {
            fprintf(stderr, "Could not create effect command thread!\n");
            goto EXIT;
        }
    }

    if (ethread_data) {
        // load effect module
        notify_effect_command(ethread_data, EFFECT_LOAD_LIB, -1, 0, NULL);

        // get effect desc
        notify_effect_command(ethread_data, EFFECT_GET_DESC, -1, 0, NULL);

        // create effect
        ethread_data->io_handle = handle;
        notify_effect_command(ethread_data, EFFECT_CREATE, -1, 0, NULL);

        // broadcast device info
        notify_effect_command(ethread_data, EFFECT_CMD, QAHW_EFFECT_CMD_SET_DEVICE, sizeof(audio_devices_t), &output_device);

        // enable effect
        notify_effect_command(ethread_data, EFFECT_CMD, QAHW_EFFECT_CMD_ENABLE, 0, NULL);
    }

    /* Register the SIGINT to close the App properly */
    if (signal(SIGINT, stop_signal_handler) == SIG_ERR)
        fprintf(log_file, "Failed to register SIGINT:%d\n",errno);

    play_file(out_handle,
              file_stream,
             (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD));

    if (ethread_data) {
        // disable effect
        notify_effect_command(ethread_data, EFFECT_CMD, QAHW_EFFECT_CMD_DISABLE, 0, NULL);

        // release effect
        notify_effect_command(ethread_data, EFFECT_RELEASE, -1, 0, NULL);

        // unload effect module
        notify_effect_command(ethread_data, EFFECT_UNLOAD_LIB, -1, 0, NULL);

        // destroy effect thread
        destroy_effect_thread(ethread_data);

        free(ethread_data);
        ethread_data = NULL;

        // destory effect command thread
        cmd_data.exit = true;
        usleep(100000);  // give a chance for thread to exit gracefully
        rc = pthread_cancel(cmd_data.cmd_thread);
        if (rc != 0) {
            fprintf(stderr, "Fail to cancel thread!\n");
        }
        rc = pthread_join(cmd_data.cmd_thread, NULL);
        if (rc < 0) {
            fprintf(stderr, "Fail to join effect command thread!\n");
        }
    }

    if (proxy_thread_active) {
       /*
        * DSP gives drain ack for last buffer which will close proxy thread before
        * app reads last buffer. So add sleep before exiting proxy thread to read
        * last buffer of data. This is not a calculated value.
        */
        usleep(500000);
        proxy_params.acp.thread_exit = true;
        fprintf(log_file, "wait for proxy thread exit\n");
    }

EXIT:

    if (proxy_thread_active)
        pthread_join(proxy_thread, NULL);

    if (out_handle != nullptr) {
        rc = qahw_out_standby(out_handle);
        if (rc) {
            fprintf(stdout, "out standby failed %d \n", rc);
        }

        rc = qahw_close_output_stream(out_handle);
        if (rc) {
            fprintf(stdout, "could not close output stream %d \n", rc);
        }
    }

    /*
     * reset device connection state for HDMI.
     */
     if (output_device == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
         char param[100] = {0};
         snprintf(param, sizeof(param), "%s=%d", "disconnect", AUDIO_DEVICE_OUT_AUX_DIGITAL);
         qahw_set_parameters(qahw_mod_handle, param);
     }

    rc = qahw_unload_module(qahw_mod_handle);
    if (rc) {
        fprintf(stdout, "could not unload hal  %d \n", rc);
        return -1;
    }

    if ((log_file != stdout) && (log_file != nullptr))
        fclose(log_file);

    if (file_stream != nullptr)
        fclose(file_stream);

    fprintf(stdout, "\nBYE BYE\n");
    return 0;
}
