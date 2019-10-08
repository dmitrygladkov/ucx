/**
 * Copyright (C) Mellanox Technologies Ltd. 2018.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include <common/test.h>
#include <common/mem_buffer.h>

#include <ucs/sys/sys.h>
#include <ucs/memory/memtype_cache.h>
#include <ucm/api/ucm.h>

extern "C" {
#include <ucm/event/event.h>
}

class test_memtype_cache : public ucs::test_with_param<ucs_memory_type_t> {
protected:
    test_memtype_cache() : m_memtype_cache(NULL) {
    }

    virtual void init() {
        ucs::test_with_param<ucs_memory_type_t>::init();
        ucs_status_t status = ucs_memtype_cache_create(&m_memtype_cache);
        ASSERT_UCS_OK(status);

        m_type = GetParam();
    }

    virtual void cleanup() {
        ucs_memtype_cache_destroy(m_memtype_cache);
        ucs::test_with_param<ucs_memory_type_t>::cleanup();
    }

    void test_lookup_found(void *ptr, size_t size,
                           ucs_memory_type_t expected_type = m_type) const {
        if (!size) {
            return;
        }

        ucs_memory_type_t mem_type;
        ucs_status_t status = ucs_memtype_cache_lookup(m_memtype_cache, ptr,
                                                       size, &mem_type);
        EXPECT_UCS_OK(status);
        EXPECT_EQ(expected_type, mem_type) << "ptr=" << ptr << " size=" << size;
    }

    void test_lookup_notfound(void *ptr, size_t size) const {
        if (!size) {
            return;
        }

        ucs_memory_type_t mem_type;
        ucs_status_t status = ucs_memtype_cache_lookup(m_memtype_cache, ptr,
                                                       size, &mem_type);
        /* memory type should be not-found or unknown */
        EXPECT_TRUE((status == UCS_ERR_NO_ELEM) ||
                    ((status == UCS_OK) && (mem_type == UCS_MEMORY_TYPE_LAST)))
              << "ptr=" << ptr << " size=" << size << ": "
              << ucs_status_string(status)
              << " memtype=" << mem_buffer::mem_type_name(mem_type);
    }

    void test_ptr_found(void *ptr, size_t size,
                        ucs_memory_type_t expected_type) const {
        if (expected_type != UCS_MEMORY_TYPE_HOST) {
            test_lookup_found(ptr, size, expected_type);
            test_lookup_found(ptr, size / 2, expected_type);
            test_lookup_found(ptr, 1, expected_type);
            test_lookup_found(UCS_PTR_BYTE_OFFSET(ptr, size - 1),
                              1, expected_type);
            test_lookup_found(ptr, 0, expected_type);
        }
    }

    void test_region_found(mem_buffer &b, size_t size) const {
        test_ptr_found(b.ptr(), size, b.mem_type());
    }

    void test_region_not_found(mem_buffer &b, size_t size) const {
        test_ptr_not_found(b.ptr(), size);
    }

    void test_ptr_not_found(void *ptr, size_t size) const {
        /* memtype cache is page-aligned, so need to step
         * by page size to make something not found */        
        test_lookup_notfound(ptr, size + ucs_get_page_size());
        test_lookup_notfound(UCS_PTR_BYTE_OFFSET(ptr, size + ucs_get_page_size()), 1);
    }

    void test_ptr_released(void *ptr, size_t size,
                           /* the flag indicates that the first/last page
                            * maybe shared between two regions */
                           bool page_maybe_shared = 0) const {
        if (!page_maybe_shared) {
            test_lookup_notfound(ptr, size);
            test_lookup_notfound(ptr, 1);
        } else {
            test_lookup_notfound(ptr, size - ucs_min(ucs_get_page_size(), size));
            test_lookup_notfound(ptr, 1 + ucs_get_page_size());
        }
    }

    mem_buffer* allocate_mem_buffer(size_t size, ucs_memory_type_t mem_type,
                                    std::vector<mem_buffer*> *allocated_buffers = NULL,
                                    bool test_not_found = true) const {
        mem_buffer *buf = new mem_buffer(size, GetParam());

        if (allocated_buffers != NULL) {
            allocated_buffers->push_back(buf);
        }

        test_region_found(*buf, buf->size());

        if (test_not_found) {
            test_region_not_found(*buf, buf->size());
        }

        return buf;
    }

    void release_mem_buffer(mem_buffer *buf,
                            std::vector<std::pair<void*, size_t> > *released_ptrs,
                            std::vector<mem_buffer*> *allocated_buffers = NULL) const {
        released_ptrs->push_back(std::make_pair(buf->ptr(),
                                                buf->size()));

        delete buf;

        if (allocated_buffers != NULL) {
            allocated_buffers->pop_back();
        }
    }

    void test_ptrs_released(std::vector<std::pair<void*, size_t> > *released_ptrs) const {
        while (!released_ptrs->empty()) {
            void *ptr   = released_ptrs->back().first;
            size_t size = released_ptrs->back().second;

            test_ptr_released(ptr, size);
            test_ptr_not_found(ptr, size);

            released_ptrs->pop_back();
        }
    }

private:
    ucs_memtype_cache_t      *m_memtype_cache;
    static ucs_memory_type_t m_type;
};

ucs_memory_type_t test_memtype_cache::m_type = UCS_MEMORY_TYPE_LAST;

UCS_TEST_P(test_memtype_cache, basic) {
    const size_t size = 64;
    void *ptr;

    {
        mem_buffer b(size, GetParam());

        test_region_found(b, size);
        test_region_not_found(b, size);

        ptr = b.ptr();
    }

    /* buffer is released */
    test_ptr_released(ptr, size);
    test_ptr_not_found(ptr, size);
}

UCS_TEST_P(test_memtype_cache, shared_page_regions) {
    const std::vector<ucs_memory_type_t> supported_mem_types =
        mem_buffer::supported_mem_types();
    const size_t size = 1000000;
    void *buf2_ptr;

    for (std::vector<ucs_memory_type_t>::const_iterator iter =
             supported_mem_types.begin();
         iter != supported_mem_types.end(); ++iter) {
        std::vector<std::pair<void*, size_t> > released_ptrs;

        /* Create the second buffer that possibly will share its
         * first page with the last page of the first buffer
         *
         *                         < shared page >
         *                            ||    ||
         *                            \/    ||
         *        +----------------------+  ||
         * buf1:  |    |    |    |    |  |  \/
         *        +----------------------+----------------------+
         *                        buf2:  |  |    |    |    |    |
         *                               +----------------------+
         */
        mem_buffer *buf1 = allocate_mem_buffer(size, GetParam());
        mem_buffer *buf2 = allocate_mem_buffer(size, *iter);

        test_region_found(*buf1, size);

        buf2_ptr = buf2->ptr();
        release_mem_buffer(buf2, &released_ptrs);

        /* buffer `buf2` is released, but need to consider
         * a shared page with buffer `buf1` */
        test_ptr_released(buf2_ptr, size, 1);

        test_region_found(*buf1, size);

        release_mem_buffer(buf1, &released_ptrs);

        /* buffer `buf1` and `buf2` are released */
        test_ptrs_released(&released_ptrs);
    }
}

UCS_TEST_P(test_memtype_cache, different_mem_types) {
    const size_t step = 64;
    const std::vector<ucs_memory_type_t> supported_mem_types =
        mem_buffer::supported_mem_types();

    /* The test tries to allocate two buffers with different memory types */
    for (std::vector<ucs_memory_type_t>::const_iterator iter =
             supported_mem_types.begin();
         iter != supported_mem_types.end(); ++iter) {
        /* 1. Allocate the same amount of memory for buffers */
        {
            std::vector<std::pair<void*, size_t> > released_ptrs;

            for (size_t i = 1; i < ucs_get_page_size(); i += step) {
                mem_buffer *buf1 = allocate_mem_buffer(i, GetParam());
                mem_buffer *buf2 = allocate_mem_buffer(i, *iter);

                release_mem_buffer(buf2, &released_ptrs);
                release_mem_buffer(buf1, &released_ptrs);
            }

            test_ptrs_released(&released_ptrs);
        }

        /* 2. Allocate the same amount of memory for buffers
         *    and keep them allocated until the end of the testing */
        {
            std::vector<mem_buffer*> allocated_buffers;
            std::vector<std::pair<void*, size_t> > released_ptrs;

            for (size_t i = 1; i < ucs_get_page_size(); i += step) {
                allocate_mem_buffer(i, GetParam(), &allocated_buffers, 0);
                allocate_mem_buffer(i, *iter, &allocated_buffers, 0);
            }

            /* release allocated buffers */
            while (!allocated_buffers.empty()) {
                release_mem_buffer(allocated_buffers.back(),
                                   &released_ptrs, &allocated_buffers);
            }

            test_ptrs_released(&released_ptrs);
        }

        /* 3. Allocate different amount of memory for buffers */
        {
            std::vector<std::pair<void*, size_t> > released_ptrs;

            for (size_t i = 1; i < ucs_get_page_size(); i += step) {
                mem_buffer *buf1 = allocate_mem_buffer(i, GetParam());

                for (size_t j = 1; j < ucs_get_page_size(); j += step) {
                    mem_buffer *buf2 = allocate_mem_buffer(j, *iter);
                    release_mem_buffer(buf2, &released_ptrs);
                }

                release_mem_buffer(buf1, &released_ptrs);
            }
        }

        /* 4. Allocate different amount of memory for buffers
         *    and keep them allocated until the end of testing */
        {
            std::vector<mem_buffer*> allocated_buffers;
            std::vector<std::pair<void*, size_t> > released_ptrs;

            for (size_t i = 1; i < ucs_get_page_size(); i += step) {
                allocate_mem_buffer(i, GetParam(), &allocated_buffers, 0);

                for (size_t j = 1; j < ucs_get_page_size(); j += step) {
                    allocate_mem_buffer(j, *iter, &allocated_buffers, 0);
                }
            }

            /* release allocated buffers */
            while (!allocated_buffers.empty()) {
                release_mem_buffer(allocated_buffers.back(),
                                   &released_ptrs, &allocated_buffers);
            }

            test_ptrs_released(&released_ptrs);
        }
    }
}

UCS_TEST_P(test_memtype_cache, different_mem_types_use_same_page) {
    const std::vector<ucs_memory_type_t> supported_mem_types =
        mem_buffer::supported_mem_types();

    for (std::vector<ucs_memory_type_t>::const_iterator iter =
             supported_mem_types.begin();
         iter != supported_mem_types.end(); ++iter) {
        ucm_event_t event1 = {
            .mem_type = {
                .address  = (void*)ucs_get_page_size(),
                .size     = (size_t)1.5 * ucs_get_page_size(),
                .mem_type = GetParam(),
            }
        };
        ucm_event_t event2 = {
            .mem_type = {
                .address  = UCS_PTR_BYTE_OFFSET(event1.mem_type.address,
                                                event1.mem_type.size),
                .size     = (size_t)1.5 * ucs_get_page_size(),
                .mem_type = *iter
            }
        };

        ucm_event_enter();
        /* simulate allocation of first buffer */
        ucm_event_dispatch(UCM_EVENT_MEM_TYPE_ALLOC, &event1);

        /* check first buffer */
        test_ptr_found(event1.mem_type.address, event1.mem_type.size,
                       event1.mem_type.mem_type);
        test_ptr_not_found(event1.mem_type.address, event1.mem_type.size);

        /* simulate allocation of second buffer */
        ucm_event_dispatch(UCM_EVENT_MEM_TYPE_ALLOC, &event2);

        /* check second buffer */
        test_ptr_found(event2.mem_type.address, event2.mem_type.size,
                       event2.mem_type.mem_type);
        test_ptr_not_found(event2.mem_type.address, event2.mem_type.size);

        /* check first buffer */
        test_ptr_found(event1.mem_type.address, event1.mem_type.size,
                       event1.mem_type.mem_type);
        ucm_event_leave();

        ucm_event_enter();
        /* simulate destroying of second buffer */
        ucm_event_dispatch(UCM_EVENT_MEM_TYPE_FREE, &event1);
        /* simulate destroying of first buffer */
        ucm_event_dispatch(UCM_EVENT_MEM_TYPE_FREE, &event2);
        ucm_event_leave();

        /* first buffer is released */
        test_ptr_released(event1.mem_type.address, event1.mem_type.size);
        test_ptr_not_found(event1.mem_type.address, event1.mem_type.size);

        /* second buffer is released */
        test_ptr_released(event2.mem_type.address, event2.mem_type.size);
        test_ptr_not_found(event2.mem_type.address, event2.mem_type.size);
    }
}

INSTANTIATE_TEST_CASE_P(mem_type, test_memtype_cache,
                        ::testing::ValuesIn(mem_buffer::supported_mem_types()));
