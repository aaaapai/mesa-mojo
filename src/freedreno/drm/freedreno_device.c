/*
 * Copyright © 2012-2018 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util/os_file.h"
#include "util/u_process.h"

#include "freedreno_rd_output.h"

#include "freedreno_drmif.h"
#include "freedreno_drm_perfetto.h"
#include "freedreno_priv.h"

struct fd_device *msm_device_new(int fd, drmVersionPtr version);
#ifdef HAVE_FREEDRENO_VIRTIO
struct fd_device *virtio_device_new(int fd, drmVersionPtr version);
#endif
#ifdef HAVE_FREEDRENO_KGSL
struct fd_device *kgsl_device_new(int fd);
#endif

uint64_t os_page_size = 4096;

struct fd_device *
fd_device_new(int fd)
{
   struct fd_device *dev = NULL;
   drmVersionPtr version = NULL;
   bool use_heap = false;
   bool support_use_heap = true;
   int kgsl_fd = -1;

   os_get_page_size(&os_page_size);

   /* 尝试获取 DRM 版本，失败时不退出，后续会尝试 KGSL */
   version = drmGetVersion(fd);
   if (!version) {
      DEBUG_MSG("drmGetVersion failed: %s (assuming KGSL)", strerror(errno));
   }

#ifdef HAVE_FREEDRENO_VIRTIO
   if (debug_get_bool_option("FD_FORCE_VTEST", false)) {
      DEBUG_MSG("virtio_gpu vtest device");
      dev = virtio_device_new(-1, version);
      if (dev) goto out;
   }
#endif

   if (version && !strcmp(version->name, "msm")) {
      DEBUG_MSG("msm DRM device");
      if (version->version_major != 1) {
         ERROR_MSG("unsupported version: %u.%u.%u", version->version_major,
                   version->version_minor, version->version_patchlevel);
         goto out;
      }
      dev = msm_device_new(fd, version);
      if (dev) goto out;
   }

#ifdef HAVE_FREEDRENO_VIRTIO
   if (version && !strcmp(version->name, "virtio_gpu")) {
      DEBUG_MSG("virtio_gpu DRM device");
      dev = virtio_device_new(fd, version);
      if (dev) {
         use_heap = true;  /* virtio-gpu 支持 heap */
         goto out;
      }
   }
#endif

#if HAVE_FREEDRENO_KGSL
   /* 当 version 为 NULL 或者不是上述 DRM 驱动时，尝试 KGSL */
   {
      /* 检查传入的 fd 是否有效（若无效则重新打开 kgsl 设备） */
      if (fd < 0 || fcntl(fd, F_GETFD) == -1) {
         kgsl_fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
         if (kgsl_fd < 0) {
            ERROR_MSG("Cannot open /dev/kgsl-3d0: %s", strerror(errno));
            goto out;
         }
      } else {
         kgsl_fd = fd;  /* 使用传入的 fd */
      }

      dev = kgsl_device_new(kgsl_fd);
      if (dev) {
         /* 如果是新打开的 fd，让设备自己关闭 */
         if (kgsl_fd != fd)
            dev->closefd = 1;
         support_use_heap = false;  /* KGSL 不支持子分配堆 */
         use_heap = false;
         goto out;
      } else {
         ERROR_MSG("kgsl_device_new failed");
         if (kgsl_fd != fd)
            close(kgsl_fd);
      }
   }
#endif

   /* 所有尝试均失败 */
   if (!dev) {
      INFO_MSG("unsupported device: %s", version ? version->name : "unknown");
      goto out;
   }

out:
   if (version)
      drmFreeVersion(version);

   if (!dev)
      return NULL;

   fd_drm_perfetto_init();

   fd_rd_dump_env_init();
   fd_rd_output_init(&dev->rd, util_get_process_name());

   p_atomic_set(&dev->refcnt, 1);
   dev->fd = fd;

   if (kgsl_fd != -1 && kgsl_fd != fd && dev->fd == fd) {
      dev->fd = kgsl_fd;
   }

   dev->handle_table =
      _mesa_hash_table_create(NULL, _mesa_hash_u32, _mesa_key_u32_equal);
   dev->name_table =
      _mesa_hash_table_create(NULL, _mesa_hash_u32, _mesa_key_u32_equal);
   fd_bo_cache_init(&dev->bo_cache, false, "bo");
   fd_bo_cache_init(&dev->ring_cache, true, "ring");

   list_inithead(&dev->deferred_submits);
   simple_mtx_init(&dev->submit_lock, mtx_plain);
   simple_mtx_init(&dev->suballoc_lock, mtx_plain);

   if (!use_heap) {
      struct fd_pipe *pipe = fd_pipe_new(dev, FD_PIPE_3D);
      if (!pipe)
         goto fail;

      /* Userspace fences don't appear to be reliable enough (missing some
       * cache flushes?) on older gens, so limit sub-alloc heaps to a6xx+
       * for now:
       */
      use_heap = fd_dev_gen(&pipe->dev_id) >= 6;
      fd_pipe_del(pipe);
   }

   if (support_use_heap && use_heap) {
      dev->ring_heap = fd_bo_heap_new(dev, RING_FLAGS);
      dev->default_heap = fd_bo_heap_new(dev, 0);
   }

   return dev;

fail:
   fd_device_del(dev);
   return NULL;
}

/* like fd_device_new() but creates it's own private dup() of the fd
 * which is close()d when the device is finalized.
 */
struct fd_device *
fd_device_new_dup(int fd)
{
   int dup_fd = os_dupfd_cloexec(fd);
   struct fd_device *dev = fd_device_new(dup_fd);
   if (dev)
      dev->closefd = 1;
   else
      close(dup_fd);
   return dev;
}

/* Convenience helper to open the drm device and return new fd_device:
 */
struct fd_device *
fd_device_open(void)
{
   int fd;

   fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
#ifdef HAVE_FREEDRENO_VIRTIO
   if (fd < 0)
      fd = drmOpenWithType("virtio_gpu", NULL, DRM_NODE_RENDER);
#endif
   if (fd < 0)
      return NULL;

   return fd_device_new(fd);
}

struct fd_device *
fd_device_ref(struct fd_device *dev)
{
   ref(&dev->refcnt);
   return dev;
}

void
fd_device_purge(struct fd_device *dev)
{
   fd_bo_cache_cleanup(&dev->bo_cache, 0);
   fd_bo_cache_cleanup(&dev->ring_cache, 0);
}

void
fd_device_del(struct fd_device *dev)
{
   if (!unref(&dev->refcnt))
      return;

   fd_rd_output_fini(&dev->rd);

   assert(list_is_empty(&dev->deferred_submits));
   assert(!dev->deferred_submits_fence);

   if (dev->suballoc_bo)
      fd_bo_del(dev->suballoc_bo);

   if (dev->ring_heap)
      fd_bo_heap_destroy(dev->ring_heap);

   if (dev->default_heap)
      fd_bo_heap_destroy(dev->default_heap);

   fd_bo_cache_cleanup(&dev->bo_cache, 0);
   fd_bo_cache_cleanup(&dev->ring_cache, 0);

   /* Needs to be after bo cache cleanup in case backend has a
    * util_vma_heap that it destroys:
    */
   dev->funcs->destroy(dev);

   _mesa_hash_table_destroy(dev->handle_table, NULL);
   _mesa_hash_table_destroy(dev->name_table, NULL);

   if (fd_device_threaded_submit(dev))
      util_queue_destroy(&dev->submit_queue);

   if (dev->closefd)
      close(dev->fd);

   free(dev);
}

int
fd_device_fd(struct fd_device *dev)
{
   return dev->fd;
}

enum fd_version
fd_device_version(struct fd_device *dev)
{
   return dev->version;
}

void
fd_device_disable_explicit_sync_heuristic(struct fd_device *dev)
{
   dev->disable_explicit_sync_heuristic = true;
}

DEBUG_GET_ONCE_BOOL_OPTION(libgl, "LIBGL_DEBUG", false)

bool
fd_dbg(void)
{
   return debug_get_option_libgl();
}

uint32_t
fd_get_features(struct fd_device *dev)
{
   return dev->features;
}

bool
fd_has_syncobj(struct fd_device *dev)
{
   uint64_t value;
   if (drmGetCap(dev->fd, DRM_CAP_SYNCOBJ, &value))
      return false;
   return value && dev->version >= FD_VERSION_FENCE_FD;
}
