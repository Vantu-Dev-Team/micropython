/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2019 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/binary.h"
#include "py/objarray.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"

#if MICROPY_VFS

void mp_vfs_blockdev_init(mp_vfs_blockdev_t *self, mp_obj_t bdev) {
    self->spare = NULL;
    self->spare_size = 0;
    mp_load_method(bdev, MP_QSTR_readblocks, self->readblocks);
    mp_load_method_maybe(bdev, MP_QSTR_writeblocks, self->writeblocks);
    mp_load_method_maybe(bdev, MP_QSTR_ioctl, self->u.ioctl);
    if (self->u.ioctl[0] != MP_OBJ_NULL) {
        // Device supports new block protocol, so indicate it
        self->flags |= MP_BLOCKDEV_FLAG_HAVE_IOCTL;
    } else {
        // No ioctl method, so assume the device uses the old block protocol
        mp_load_method_maybe(bdev, MP_QSTR_sync, self->u.old.sync);
        mp_load_method(bdev, MP_QSTR_count, self->u.old.count);
    }
}

static bool mp_vfs_blockdev_is_native_fun(mp_obj_t fun) {
    const mp_obj_type_t *type = mp_obj_get_type(fun);
    return type == &mp_type_fun_builtin_0 || type == &mp_type_fun_builtin_1
           || type == &mp_type_fun_builtin_2 || type == &mp_type_fun_builtin_3
           || type == &mp_type_fun_builtin_var;
}

// Helper function to minimise code size of read/write functions
// note the n_args argument is moved to the end for further code size reduction (args keep same position in caller and callee).
static int mp_vfs_blockdev_call_rw(mp_vfs_blockdev_t *self, mp_obj_t *args, size_t block_num, size_t block_off, size_t len, void *buf, bool is_read, size_t n_args) {
    // buf is filesystem-driver storage that is only valid during this call.
    // Native C methods get a zero-copy view. A Python method may keep its
    // argument, so it always gets a view of GC-owned memory: a fresh copy,
    // or the reusable spare when the heap cannot allocate (e.g. locked in a
    // finaliser). Without either the operation fails with ENOMEM.
    #if MICROPY_PY_BUILTINS_MEMORYVIEW
    mp_obj_array_t ar = {{&mp_type_memoryview}, 'B' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW, 0, len, buf};
    #else
    mp_obj_array_t ar = {{&mp_type_bytearray}, BYTEARRAY_TYPECODE, 0, len, buf};
    #endif
    mp_obj_array_t *view = &ar;
    if (!mp_vfs_blockdev_is_native_fun(args[0])) {
        void *copy = m_malloc_maybe(len ? len : 1);
        view = copy == NULL ? NULL : m_new_obj_maybe(mp_obj_array_t);
        if (view != NULL) {
            *view = ar;
            view->items = copy;
            if (len >= self->spare_size) {
                self->spare = view;
                self->spare_size = len;
            }
        } else if (self->spare != NULL && self->spare_size >= len) {
            view = self->spare;
            view->len = len;
        } else {
            return -MP_ENOMEM;
        }
        memcpy(view->items, buf, len);
    }
    args[2] = MP_OBJ_NEW_SMALL_INT(block_num);
    args[3] = MP_OBJ_FROM_PTR(view);
    args[4] = MP_OBJ_NEW_SMALL_INT(block_off); // ignored for n_args == 2
    mp_obj_t ret = mp_call_method_n_kw(n_args, 0, args);
    bool copy_back = is_read && view != &ar;

    int i = 0;
    if (ret != mp_const_none) {
        // Block device functions are expected to return 0 on success
        // and negative integer on errors. Check for positive integer
        // results as some callers (i.e. littlefs) will produce corrupt
        // results from these. Conversion may run __int__, which can still
        // write the buffer: copy back afterwards, also when it raises, as
        // the zero-copy path would expose those bytes to the driver.
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            i = mp_obj_get_int(ret);
            nlr_pop();
        } else {
            if (copy_back) {
                memcpy(buf, view->items, len);
            }
            nlr_jump(nlr.ret_val);
        }
        i = i > 0 ? (-MP_EINVAL) : i;
    }
    if (copy_back) {
        memcpy(buf, view->items, len);
    }
    return i;
}

int mp_vfs_blockdev_read(mp_vfs_blockdev_t *self, size_t block_num, size_t num_blocks, uint8_t *buf) {
    #if MICROPY_VFS_BLOCKDEV_NATIVE
    if (self->flags & MP_BLOCKDEV_FLAG_NATIVE) {
        mp_vfs_blockdev_native_readblocks f = (void *)(uintptr_t)self->readblocks[2];
        return f(buf, block_num, num_blocks);
    }
    #endif

    return mp_vfs_blockdev_call_rw(self, self->readblocks, block_num, 0, num_blocks * self->block_size, buf, true, 2);
}

int mp_vfs_blockdev_read_ext(mp_vfs_blockdev_t *self, size_t block_num, size_t block_off, size_t len, uint8_t *buf) {
    return mp_vfs_blockdev_call_rw(self, self->readblocks, block_num, block_off, len, buf, true, 3);
}

int mp_vfs_blockdev_write(mp_vfs_blockdev_t *self, size_t block_num, size_t num_blocks, const uint8_t *buf) {
    if (self->writeblocks[0] == MP_OBJ_NULL) {
        // read-only block device
        return -MP_EROFS;
    }

    #if MICROPY_VFS_BLOCKDEV_NATIVE
    if (self->flags & MP_BLOCKDEV_FLAG_NATIVE) {
        mp_vfs_blockdev_native_writeblocks f = (void *)(uintptr_t)self->writeblocks[2];
        return f(buf, block_num, num_blocks);
    }
    #endif

    return mp_vfs_blockdev_call_rw(self, self->writeblocks, block_num, 0, num_blocks * self->block_size, (void *)buf, false, 2);
}

int mp_vfs_blockdev_write_ext(mp_vfs_blockdev_t *self, size_t block_num, size_t block_off, size_t len, const uint8_t *buf) {
    if (self->writeblocks[0] == MP_OBJ_NULL) {
        // read-only block device
        return -MP_EROFS;
    }
    return mp_vfs_blockdev_call_rw(self, self->writeblocks, block_num, block_off, len, (void *)buf, false, 3);
}

mp_obj_t mp_vfs_blockdev_ioctl(mp_vfs_blockdev_t *self, uintptr_t cmd, uintptr_t arg) {
    if (self->flags & MP_BLOCKDEV_FLAG_HAVE_IOCTL) {
        // New protocol with ioctl
        self->u.ioctl[2] = MP_OBJ_NEW_SMALL_INT(cmd);
        self->u.ioctl[3] = MP_OBJ_NEW_SMALL_INT(arg);
        return mp_call_method_n_kw(2, 0, self->u.ioctl);
    } else {
        // Old protocol with sync and count
        switch (cmd) {
            case MP_BLOCKDEV_IOCTL_SYNC:
                if (self->u.old.sync[0] != MP_OBJ_NULL) {
                    mp_call_method_n_kw(0, 0, self->u.old.sync);
                }
                break;

            case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
                return mp_call_method_n_kw(0, 0, self->u.old.count);

            case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
                // Old protocol has fixed sector size of 512 bytes
                break;

            case MP_BLOCKDEV_IOCTL_INIT:
                // Old protocol doesn't have init
                break;
        }
        return mp_const_none;
    }
}

#endif // MICROPY_VFS
