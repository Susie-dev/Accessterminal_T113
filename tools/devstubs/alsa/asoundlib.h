/*
 * asoundlib.h — 仅供主机端（Windows/MinGW）语法与语义检查用的严格桩头文件。
 *
 * 目的：ALSA 只在 Linux 上存在，而 T113 工程大量使用其 C 接口。为了让
 * 开发机也能对工程自身的代码做真实编译验证（发现类型不匹配、参数个数
 * 错误、拼写错误等），这里按 ALSA 官方头文件的签名复刻所用到的子集。
 *
 * 重要：本文件不是 ALSA 的实现，也不能用于产出可运行的程序。
 *       交叉编译时必须使用 Tina SDK 中的真实 <alsa/asoundlib.h>。
 *       若误用本桩，链接期会因缺少 snd_* 实现而失败。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_ASOUNDLIB_H
#define ACCESSTERMINAL_DEVSTUB_ASOUNDLIB_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;
typedef struct _snd_mixer_selem_id snd_mixer_selem_id_t;
struct snd_mixer_selem_regopt;

typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

/* ---- 枚举常量（取值与 ALSA 一致，便于静态断言）---- */
typedef enum {
    SND_PCM_STREAM_PLAYBACK = 0,
    SND_PCM_STREAM_CAPTURE = 1
} snd_pcm_stream_t;

typedef enum {
    SND_PCM_ACCESS_MMAP_INTERLEAVED = 0,
    SND_PCM_ACCESS_MMAP_NONINTERLEAVED = 1,
    SND_PCM_ACCESS_MMAP_COMPLEX = 2,
    SND_PCM_ACCESS_RW_INTERLEAVED = 3
} snd_pcm_access_t;

typedef enum {
    SND_PCM_FORMAT_S16_LE = 2,
    SND_PCM_FORMAT_S16_BE = 3
} snd_pcm_format_t;

#define SND_PCM_NONBLOCK 0x00000001U

/* ---- PCM ---- */
int snd_pcm_open(snd_pcm_t** pcm, const char* name,
                 snd_pcm_stream_t stream, int mode);
int snd_pcm_close(snd_pcm_t* pcm);

size_t snd_pcm_hw_params_sizeof(void);
int snd_pcm_hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);
int snd_pcm_hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 snd_pcm_access_t access);
int snd_pcm_hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 snd_pcm_format_t val);
int snd_pcm_hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                   unsigned int val);
int snd_pcm_hw_params_set_rate_near(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                    unsigned int* val, int* dir);
int snd_pcm_hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                               unsigned int val, int dir);
int snd_pcm_hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t* pcm, const void* buffer,
                                 snd_pcm_uframes_t size);
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t* pcm, void* buffer,
                                snd_pcm_uframes_t size);
int snd_pcm_prepare(snd_pcm_t* pcm);
int snd_pcm_drain(snd_pcm_t* pcm);
int snd_pcm_drop(snd_pcm_t* pcm);

const char* snd_strerror(int errnum);

/* ALSA 的 _alloca 系列是宏；此处给出等价的最小定义。
 * 真实头文件用 __snd_alloca(ptr,type) 展开为 alloca + memset。 */
#define __snd_alloca(ptr, type)                                \
    do {                                                       \
        *(ptr) = (type##_t*)alloca(type##_sizeof());           \
        memset(*(ptr), 0, type##_sizeof());                    \
    } while (0)

#define snd_pcm_hw_params_alloca(ptr) __snd_alloca(ptr, snd_pcm_hw_params)

/* ---- Mixer ---- */
int snd_mixer_open(snd_mixer_t** mixer, int mode);
int snd_mixer_close(snd_mixer_t* mixer);
int snd_mixer_attach(snd_mixer_t* mixer, const char* name);
int snd_mixer_selem_register(snd_mixer_t* mixer,
                             struct snd_mixer_selem_regopt* options,
                             snd_pcm_t** playbackp);
int snd_mixer_load(snd_mixer_t* mixer);
snd_mixer_elem_t* snd_mixer_find_selem(snd_mixer_t* mixer,
                                       const snd_mixer_selem_id_t* id);

size_t snd_mixer_selem_id_sizeof(void);
void snd_mixer_selem_id_set_index(snd_mixer_selem_id_t* obj, unsigned int val);
void snd_mixer_selem_id_set_name(snd_mixer_selem_id_t* obj, const char* val);

#define snd_mixer_selem_id_alloca(ptr) __snd_alloca(ptr, snd_mixer_selem_id)

int snd_mixer_selem_get_playback_volume_range(snd_mixer_elem_t* elem,
                                              long* min, long* max);
int snd_mixer_selem_set_playback_volume_all(snd_mixer_elem_t* elem, long value);

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_ASOUNDLIB_H */
