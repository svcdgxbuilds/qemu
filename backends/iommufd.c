/*
 * iommufd container backend
 *
 * Copyright (C) 2022 Intel Corporation.
 * Copyright Red Hat, Inc. 2022
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *          Eric Auger <eric.auger@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "sysemu/iommufd.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "qemu/error-report.h"
#include "monitor/monitor.h"
#include "trace.h"
#include <sys/ioctl.h>
#include <linux/iommufd.h>

static void iommufd_backend_init(Object *obj)
{
    IOMMUFDBackend *be = IOMMUFD_BACKEND(obj);

    be->fd = -1;
    be->users = 0;
    be->owned = true;
    qemu_mutex_init(&be->lock);
}

static void iommufd_backend_finalize(Object *obj)
{
    IOMMUFDBackend *be = IOMMUFD_BACKEND(obj);

    if (be->owned) {
        close(be->fd);
        be->fd = -1;
    }
}

static void iommufd_backend_set_fd(Object *obj, const char *str, Error **errp)
{
    IOMMUFDBackend *be = IOMMUFD_BACKEND(obj);
    int fd = -1;

    fd = monitor_fd_param(monitor_cur(), str, errp);
    if (fd == -1) {
        error_prepend(errp, "Could not parse remote object fd %s:", str);
        return;
    }
    qemu_mutex_lock(&be->lock);
    be->fd = fd;
    be->owned = false;
    qemu_mutex_unlock(&be->lock);
    trace_iommu_backend_set_fd(be->fd);
}

static void iommufd_backend_class_init(ObjectClass *oc, void *data)
{
    object_class_property_add_str(oc, "fd", NULL, iommufd_backend_set_fd);
}

int iommufd_backend_connect(IOMMUFDBackend *be, Error **errp)
{
    int fd, ret = 0;

    qemu_mutex_lock(&be->lock);
    if (be->users == UINT32_MAX) {
        error_setg(errp, "too many connections");
        ret = -E2BIG;
        goto out;
    }
    if (be->owned && !be->users) {
        fd = qemu_open_old("/dev/iommu", O_RDWR);
        if (fd < 0) {
            error_setg_errno(errp, errno, "/dev/iommu opening failed");
            ret = fd;
            goto out;
        }
        be->fd = fd;
    }
    be->users++;
out:
    trace_iommufd_backend_connect(be->fd, be->owned,
                                  be->users, ret);
    qemu_mutex_unlock(&be->lock);
    return ret;
}

void iommufd_backend_disconnect(IOMMUFDBackend *be)
{
    qemu_mutex_lock(&be->lock);
    if (!be->users) {
        goto out;
    }
    be->users--;
    if (!be->users && be->owned) {
        close(be->fd);
        be->fd = -1;
    }
out:
    trace_iommufd_backend_disconnect(be->fd, be->users);
    qemu_mutex_unlock(&be->lock);
}

static int iommufd_backend_alloc_ioas(int fd, uint32_t *ioas)
{
    int ret;
    struct iommu_ioas_alloc alloc_data  = {
        .size = sizeof(alloc_data),
        .flags = 0,
    };

    ret = ioctl(fd, IOMMU_IOAS_ALLOC, &alloc_data);
    if (ret) {
        error_report("Failed to allocate ioas %m");
    }

    *ioas = alloc_data.out_ioas_id;
    trace_iommufd_backend_alloc_ioas(fd, *ioas, ret);

    return ret;
}

void iommufd_backend_free_id(int fd, uint32_t id)
{
    int ret;
    struct iommu_destroy des = {
        .size = sizeof(des),
        .id = id,
    };

    ret = ioctl(fd, IOMMU_DESTROY, &des);
    trace_iommufd_backend_free_id(fd, id, ret);
    if (ret) {
        error_report("Failed to free id: %u %m", id);
    }
}

int iommufd_backend_get_ioas(IOMMUFDBackend *be, uint32_t *ioas_id)
{
    int ret;

    ret = iommufd_backend_alloc_ioas(be->fd, ioas_id);
    trace_iommufd_backend_get_ioas(be->fd, *ioas_id, ret);
    return ret;
}

void iommufd_backend_put_ioas(IOMMUFDBackend *be, uint32_t ioas)
{
    trace_iommufd_backend_put_ioas(be->fd, ioas);
    iommufd_backend_free_id(be->fd, ioas);
}

int iommufd_backend_unmap_dma(IOMMUFDBackend *be, uint32_t ioas,
                              hwaddr iova, ram_addr_t size)
{
    int ret;
    struct iommu_ioas_unmap unmap = {
        .size = sizeof(unmap),
        .ioas_id = ioas,
        .iova = iova,
        .length = size,
    };

    ret = ioctl(be->fd, IOMMU_IOAS_UNMAP, &unmap);
    trace_iommufd_backend_unmap_dma(be->fd, ioas, iova, size, ret);
    if (ret) {
        error_report("IOMMU_IOAS_UNMAP failed: %s", strerror(errno));
    }
    return !ret ? 0 : -errno;
}

int iommufd_backend_map_dma(IOMMUFDBackend *be, uint32_t ioas, hwaddr iova,
                            ram_addr_t size, void *vaddr, bool readonly)
{
    int ret;
    struct iommu_ioas_map map = {
        .size = sizeof(map),
        .flags = IOMMU_IOAS_MAP_READABLE |
                 IOMMU_IOAS_MAP_FIXED_IOVA,
        .ioas_id = ioas,
        .__reserved = 0,
        .user_va = (int64_t)vaddr,
        .iova = iova,
        .length = size,
    };

    if (!readonly) {
        map.flags |= IOMMU_IOAS_MAP_WRITEABLE;
    }

    ret = ioctl(be->fd, IOMMU_IOAS_MAP, &map);
    trace_iommufd_backend_map_dma(be->fd, ioas, iova, size,
                                  vaddr, readonly, ret);
    if (ret) {
        error_report("IOMMU_IOAS_MAP failed: %s", strerror(errno));
    }
    return !ret ? 0 : -errno;
}

int iommufd_backend_copy_dma(IOMMUFDBackend *be, uint32_t src_ioas,
                             uint32_t dst_ioas, hwaddr iova,
                             ram_addr_t size, bool readonly)
{
    int ret;
    struct iommu_ioas_copy copy = {
        .size = sizeof(copy),
        .flags = IOMMU_IOAS_MAP_READABLE |
                 IOMMU_IOAS_MAP_FIXED_IOVA,
        .dst_ioas_id = dst_ioas,
        .src_ioas_id = src_ioas,
        .length = size,
        .dst_iova = iova,
        .src_iova = iova,
    };

    if (!readonly) {
        copy.flags |= IOMMU_IOAS_MAP_WRITEABLE;
    }

    ret = ioctl(be->fd, IOMMU_IOAS_COPY, &copy);
    trace_iommufd_backend_copy_dma(be->fd, src_ioas, dst_ioas,
                                   iova, size, readonly, ret);
    if (ret) {
        error_report("IOMMU_IOAS_COPY failed: %s", strerror(errno));
    }
    return !ret ? 0 : -errno;
}

int iommufd_backend_alloc_hwpt(int iommufd, uint32_t flags, uint32_t dev_id,
                               uint32_t hwpt_type, uint32_t parent,
                               uint32_t data_type, void *data,
                               uint32_t data_len, uint32_t *out_hwpt)
{
    int ret;
    struct iommu_alloc_hwpt alloc_hwpt = {
        .size = sizeof(struct iommu_alloc_hwpt),
        .flags = flags,
        .dev_id = dev_id,
        .hwpt_type = hwpt_type,
        .parent_id = parent,
        .data_type = data_type,
        .data_len = data_len,
        .reserved = 0,
        .data_uptr = (uint64_t)data,
    };

    ret = ioctl(iommufd, IOMMU_ALLOC_HWPT, &alloc_hwpt);
    trace_iommufd_backend_alloc_hwpt(iommufd, flags, dev_id, hwpt_type, parent,
                                     data_type, (uint64_t)data, ret);
    if (ret) {
        error_report("IOMMU_ALLOC_HWPT (%d type) failed: %s",
                     hwpt_type, strerror(errno));
    } else {
        *out_hwpt = alloc_hwpt.out_hwpt_id;
    }
    return !ret ? 0 : -errno;
}

int iommufd_backend_add_hwpt_event(int iommufd, uint32_t dev_id, uint32_t hwpt,
                                   int eventfd, int *out_fd)
{
    int ret;
    struct iommu_add_hwpt_event add_event = {
        .size = sizeof(struct iommu_add_hwpt_event),
        .flags = 0,
        .type = IOMMU_HWPT_EVENT_FAULT,
        .dev_id = dev_id,
        .hwpt_id = hwpt,
        .eventfd = eventfd,
    };

    ret = ioctl(iommufd, IOMMU_ADD_HWPT_EVENT, &add_event);
    trace_iommufd_backend_add_hwpt_event(iommufd, add_event.type, dev_id, hwpt,
                                         eventfd, ret);
    if (ret) {
        error_report("IOMMU_ADD_HWPT_EVENT failed: %s", strerror(errno));
    } else {
        *out_fd = add_event.out_fd;
    }
    return !ret ? 0 : -errno;
}

int iommufd_backend_alloc_pasid(int iommufd, uint32_t min, uint32_t max,
                        bool identical, uint32_t *pasid)
{
    int ret;
    uint32_t upasid = *pasid;
    struct iommu_alloc_pasid alloc = {
        .size = sizeof(alloc),
        .flags = identical ? IOMMU_ALLOC_PASID_IDENTICAL : 0,
        .range.min = min,
        .range.max = max,
        .pasid = upasid,
    };

    ret = ioctl(iommufd, IOMMU_ALLOC_PASID, &alloc);
    if (ret) {
        error_report("IOMMU_ALLOC_PASID failed: %s", strerror(errno));
    } else {
        *pasid = alloc.pasid;
    }
    trace_iommufd_backend_alloc_pasid(iommufd, min, max,
                              identical, upasid, *pasid, ret);
    return !ret ? 0 : -errno;
}

int iommufd_backend_free_pasid(int iommufd, uint32_t pasid)
{
    int ret;
    struct iommu_free_pasid free = {
        .size = sizeof(free),
        .flags = 0,
        .pasid = pasid,
    };

    ret = ioctl(iommufd, IOMMU_FREE_PASID, &free);
    if (ret) {
        error_report("IOMMU_FREE_PASID failed: %s", strerror(errno));
    }
    trace_iommufd_backend_free_pasid(iommufd, pasid, ret);
    return !ret ? 0 : -errno;
}

int iommufd_backend_invalidate_cache(int iommufd, uint32_t hwpt_id,
                             struct iommu_cache_invalidate_info *info)
{
    int ret;
    struct iommu_hwpt_invalidate_s1_cache cache = {
        .size = sizeof(cache),
        .flags = 0,
        .hwpt_id = hwpt_id,
        .info = *info,
    };

    ret = ioctl(iommufd, IOMMU_HWPT_INVAL_S1_CACHE, &cache);
    if (ret) {
        error_report("IOMMU_HWPT_INVAL_S1_CACHE failed: %s", strerror(errno));
    }
    trace_iommufd_backend_invalidate_cache(iommufd, hwpt_id, ret);
    return !ret ? 0 : -errno;
}

int iommufd_backend_page_response(int iommufd, uint32_t hwpt_id,
                          uint32_t dev_id, struct iommu_page_response *resp)
{
    int ret;
    struct iommu_hwpt_page_response page = {
        .size = sizeof(page),
        .flags = 0,
        .hwpt_id = hwpt_id,
        .dev_id = dev_id,
        .resp = *resp,
    };

    ret = ioctl(iommufd, IOMMU_PAGE_RESPONSE, &page);
    if (ret) {
        error_report("IOMMU_PAGE_RESPONSE failed: %s", strerror(errno));
    }
    trace_iommufd_backend_page_response(iommufd, hwpt_id, dev_id, ret);
    return !ret ? 0 : -errno;
}

static const TypeInfo iommufd_backend_info = {
    .name = TYPE_IOMMUFD_BACKEND,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(IOMMUFDBackend),
    .instance_init = iommufd_backend_init,
    .instance_finalize = iommufd_backend_finalize,
    .class_size = sizeof(IOMMUFDBackendClass),
    .class_init = iommufd_backend_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void register_types(void)
{
    type_register_static(&iommufd_backend_info);
}

type_init(register_types);
