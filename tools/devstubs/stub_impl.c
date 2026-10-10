/*
 * devstubs/stub_impl.c — 桩实现，仅供主机端**链接检查**使用。
 *
 * 目的：让 T113 工程能在开发机上完成链接，从而发现“声明了但没有定义”
 * （undefined reference）与重复定义（duplicate symbol）这类静态检查
 * 覆盖不到的缺陷。
 *
 * 所有函数只返回“合理但无实际作用”的值，不做任何真实 I/O。
 * 这个文件绝不能进入目标平台构建——交叉编译必须链接真实的
 * libasound 与 libgpiod。
 *
 * 若需要显式排除，可定义 ACCESSTERMINAL_STUB_IMPL 之外的方式；但更直接
 * 的做法是：本文件只被 tools/check_host_compile.sh 的链接步骤引用，
 * CMakeLists.txt / smartpro.pro 都不包含它。
 */
#ifdef ACCESSTERMINAL_HOST_LINK_CHECK

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>
#include <gpiod.h>

/* ========================================================================
 * ALSA 桩实现
 * ===================================================================== */

int snd_pcm_open(snd_pcm_t** pcm, const char* name,
                 snd_pcm_stream_t stream, int mode)
{
    (void)name; (void)stream; (void)mode;
    /* 返回非空句柄，模拟“设备可用”，便于检查调用方是否正确处理成功路径 */
    *pcm = (snd_pcm_t*)calloc(1, 64);
    return *pcm ? 0 : -1;
}

int snd_pcm_close(snd_pcm_t* pcm) { free(pcm); return 0; }

size_t snd_pcm_hw_params_sizeof(void) { return 128; }

int snd_pcm_hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params)
{ (void)pcm; (void)params; return 0; }

int snd_pcm_hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 snd_pcm_access_t access)
{ (void)pcm; (void)params; (void)access; return 0; }

int snd_pcm_hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 snd_pcm_format_t val)
{ (void)pcm; (void)params; (void)val; return 0; }

int snd_pcm_hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                   unsigned int val)
{ (void)pcm; (void)params; (void)val; return 0; }

int snd_pcm_hw_params_set_rate_near(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                    unsigned int* val, int* dir)
{ (void)pcm; (void)params; (void)dir; (void)val; return 0; }

int snd_pcm_hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                               unsigned int val, int dir)
{ (void)pcm; (void)params; (void)val; (void)dir; return 0; }

int snd_pcm_hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params)
{ (void)pcm; (void)params; return 0; }

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t* pcm, const void* buffer,
                                 snd_pcm_uframes_t size)
{ (void)pcm; (void)buffer; return (snd_pcm_sframes_t)size; }

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t* pcm, void* buffer,
                                snd_pcm_uframes_t size)
{
    (void)pcm;
    if (buffer) memset(buffer, 0, (size_t)size);
    return (snd_pcm_sframes_t)size;
}

int snd_pcm_prepare(snd_pcm_t* pcm) { (void)pcm; return 0; }
int snd_pcm_drain(snd_pcm_t* pcm) { (void)pcm; return 0; }
int snd_pcm_drop(snd_pcm_t* pcm) { (void)pcm; return 0; }

const char* snd_strerror(int errnum)
{
    (void)errnum;
    return "stub error";
}

int snd_mixer_open(snd_mixer_t** mixer, int mode)
{
    (void)mode;
    *mixer = (snd_mixer_t*)calloc(1, 64);
    return *mixer ? 0 : -1;
}

int snd_mixer_close(snd_mixer_t* mixer) { free(mixer); return 0; }

int snd_mixer_attach(snd_mixer_t* mixer, const char* name)
{ (void)mixer; (void)name; return 0; }

int snd_mixer_selem_register(snd_mixer_t* mixer,
                             struct snd_mixer_selem_regopt* options,
                             snd_pcm_t** playbackp)
{ (void)mixer; (void)options; (void)playbackp; return 0; }

int snd_mixer_load(snd_mixer_t* mixer) { (void)mixer; return 0; }

snd_mixer_elem_t* snd_mixer_find_selem(snd_mixer_t* mixer,
                                       const snd_mixer_selem_id_t* id)
{
    (void)mixer; (void)id;
    /* 返回非空，走“找到控件”的路径，便于检查返回值处理 */
    return (snd_mixer_elem_t*)calloc(1, 64);
}

size_t snd_mixer_selem_id_sizeof(void) { return 64; }

void snd_mixer_selem_id_set_index(snd_mixer_selem_id_t* obj, unsigned int val)
{ (void)obj; (void)val; }

void snd_mixer_selem_id_set_name(snd_mixer_selem_id_t* obj, const char* val)
{ (void)obj; (void)val; }

int snd_mixer_selem_get_playback_volume_range(snd_mixer_elem_t* elem,
                                              long* min, long* max)
{ (void)elem; if (min) *min = 0; if (max) *max = 100; return 0; }

int snd_mixer_selem_set_playback_volume_all(snd_mixer_elem_t* elem, long value)
{ (void)elem; (void)value; return 0; }

/* ========================================================================
 * libgpiod 桩实现
 * ===================================================================== */

struct gpiod_chip* gpiod_chip_open_by_name(const char* name)
{ (void)name; return (struct gpiod_chip*)calloc(1, 64); }

struct gpiod_chip* gpiod_chip_open(const char* path)
{ (void)path; return (struct gpiod_chip*)calloc(1, 64); }

void gpiod_chip_close(struct gpiod_chip* chip) { free(chip); }

struct gpiod_line* gpiod_chip_get_line(struct gpiod_chip* chip,
                                       unsigned int offset)
{ (void)chip; (void)offset; return (struct gpiod_line*)calloc(1, 64); }

int gpiod_line_request_input(struct gpiod_line* line, const char* consumer)
{ (void)line; (void)consumer; return 0; }

int gpiod_line_request_output(struct gpiod_line* line, const char* consumer,
                              int default_val)
{ (void)line; (void)consumer; (void)default_val; return 0; }

int gpiod_line_get_value(struct gpiod_line* line) { (void)line; return 0; }

int gpiod_line_set_value(struct gpiod_line* line, int value)
{ (void)line; (void)value; return 0; }

void gpiod_line_release(struct gpiod_line* line) { free(line); }

/* ========================================================================
 * POSIX 套接字 / ioctl / mmap 桩实现
 *
 * 这些在 Linux 的 glibc 中由 libc 提供，MinGW 上没有（其等价物是
 * winsock2 的 socket/bind 等，但需要 WSAStartup，且类型语义不同）。
 * 为了在开发机上完成**链接完整性**检查，这里给出最小实现。
 * ===================================================================== */

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>

/* 字节序转换：MinGW 在 winsock2.h 中以宏/内联函数形式提供，与这里声明的
 * 外部函数冲突，故用自定义名字定义后再由同名符号导出。此处直接实现，
 * 语义与 POSIX 一致（主机序 <-> 网络序）。 */
unsigned short htons(unsigned short hostshort)
{
    return (unsigned short)(((hostshort & 0x00FFU) << 8) |
                            ((hostshort & 0xFF00U) >> 8));
}

unsigned short ntohs(unsigned short netshort) { return htons(netshort); }

unsigned int htonl(unsigned int hostlong)
{
    return ((hostlong & 0x000000FFU) << 24) |
           ((hostlong & 0x0000FF00U) << 8)  |
           ((hostlong & 0x00FF0000U) >> 8)  |
           ((hostlong & 0xFF000000U) >> 24);
}

unsigned int ntohl(unsigned int netlong) { return htonl(netlong); }

int socket(int domain, int type, int protocol)
{ (void)domain; (void)type; (void)protocol; return -1; }

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen)
{ (void)sockfd; (void)addr; (void)addrlen; return -1; }

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen)
{ (void)sockfd; (void)addr; (void)addrlen; return -1; }

int setsockopt(int sockfd, int level, int optname, const void* optval,
               socklen_t optlen)
{ (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen; return -1; }

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen)
{ (void)sockfd; (void)buf; (void)flags; (void)dest_addr; (void)addrlen;
  return (ssize_t)len; }

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* addrlen)
{ (void)sockfd; (void)buf; (void)flags; (void)src_addr; (void)addrlen; return -1; }

int inet_pton(int af, const char* src, void* dst)
{ (void)af; (void)src; (void)dst; return 0; }

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size)
{ (void)af; (void)src; (void)dst; (void)size; return dst; }

int ioctl(int fd, unsigned long request, ...)
{ (void)fd; (void)request; return -1; }

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset)
{ (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
  return MAP_FAILED; }

int munmap(void* addr, size_t length)
{ (void)addr; (void)length; return -1; }

int fcntl(int fd, int cmd, ...)
{ (void)fd; (void)cmd; return -1; }

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout)
{ (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
  return 0; }

#endif /* ACCESSTERMINAL_HOST_LINK_CHECK */
