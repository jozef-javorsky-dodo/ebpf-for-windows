// Copyright (c) eBPF for Windows contributors
// SPDX-License-Identifier: MIT

#include "ebpf_epoch.h"
#include "ebpf_hash_table.h"
#include "ebpf_random.h"

#include <intrin.h>

// Buckets contain an array of pointers to value and keys.
// Buckets are immutable once inserted in to the hash-table and replaced when
// modified.

// Layout is:
// ebpf_hash_table_t.buckets->ebpf_hash_bucket_header_t.entries->data
// Keys are stored contiguously in ebpf_hash_bucket_header_t for fast
// searching, data is stored separately to prevent read-copy-update semantics
// from causing loss of updates.

/**
 * @brief Each bucket entry contains a pointer to the value, the key, and a pointer to pre-allocated memory that can be
 * used to replace the current bucket with a bucket one entry smaller.
 */
typedef struct _ebpf_hash_bucket_entry
{
    uint8_t* data;
    struct _ebpf_hash_bucket_header* backup_bucket;
    uint8_t key[1];
} ebpf_hash_bucket_entry_t;

/**
 * @brief Header for each bucket. The header contains the number of entries in the bucket and an array of bucket
 * entries.
 */
typedef struct _ebpf_hash_bucket_header
{
    size_t count;
    _Field_size_(count) ebpf_hash_bucket_entry_t entries[1];
} ebpf_hash_bucket_header_t;

/**
 * @brief This structure contains the pointer to the bucket and a lock to synchronize replacing the bucket.
 */
typedef struct _ebpf_hash_bucket_header_and_lock
{
    ebpf_hash_bucket_header_t* header;
    ebpf_lock_t lock;
} ebpf_hash_bucket_header_and_lock_t;

/**
 * @brief The ebpf_hash_table_t structure represents a hash table. It contains an array of pointers to buckets and a
 * a per bucket lock.
 */
struct _ebpf_hash_table
{
    size_t bucket_count;      // Count of buckets.
    size_t bucket_count_mask; // Mask to use to get bucket index from hash.
    volatile size_t
        entry_count; // Count of entries in the hash table. Only valid if max_entry_count != EBPF_HASH_TABLE_NO_LIMIT.
    size_t max_entry_count;         // Maximum number of entries allowed or EBPF_HASH_TABLE_NO_LIMIT if no maximum.
    uint32_t seed;                  // Seed used for hashing.
    size_t key_size;                // Size of key.
    size_t value_size;              // Size of value.
    size_t supplemental_value_size; // Size of supplemental value.
    void* (*allocate)(size_t size); // Function to allocate memory.
    void (*free)(void* memory);     // Function to free memory.
    void (*extract)(
        _In_ const uint8_t* value,
        _Outptr_ const uint8_t** data,
        _Out_ size_t* num); // Function to extract bytes to hash from key.

    void* notification_context; //< Context to pass to notification functions.
    ebpf_hash_table_notification_function notification_callback;
    _Field_size_(bucket_count) ebpf_hash_bucket_header_and_lock_t buckets[1]; // Pointer to array of buckets.
};

typedef enum _ebpf_hash_bucket_operation
{
    EBPF_HASH_BUCKET_OPERATION_INSERT_OR_UPDATE, // Insert or update a key-value pair.
    EBPF_HASH_BUCKET_OPERATION_INSERT,           // Insert a key-value pair. Fails if key already exists.
    EBPF_HASH_BUCKET_OPERATION_UPDATE,           // Update a key-value pair. Fails if key does not exist.
    EBPF_HASH_BUCKET_OPERATION_DELETE,           // Delete a key-value pair. Fails if key does not exist.
} ebpf_hash_bucket_operation_t;

/**
 * @brief Perform a rotate left on a value.
 *
 * @param[in] value Value to be rotated.
 * @param[in] count Count of bits to rotate.
 * @return Rotated value.
 */
static inline unsigned long
_ebpf_rol(uint32_t value, size_t count)
{
    return (value << count) | (value >> (32 - count));
}

// Ported from https://github.com/aappleby/smhasher
// Quote from https://github.com/aappleby/smhasher/blob/61a0530f28277f2e850bfc39600ce61d02b518de/src/MurmurHash3.cpp#L2
// "MurmurHash3 was written by Austin Appleby, and is placed in the public domain."

/**
 * @brief An implementation of the murmur3_32 hash function. This is a high
 * performance non-cryptographic hash function.
 *
 * @param[in] key Pointer to key to hash.
 * @param[in] length Length of key to hash.
 * @param[in] seed Seed to randomize hash.
 * @return Hash of key.
 */
static unsigned long
_ebpf_murmur3_32(_In_reads_((length_in_bits + 7) / 8) const uint8_t* key, size_t length_in_bits, uint32_t seed)
{
    uint32_t c1 = 0xcc9e2d51;
    uint32_t c2 = 0x1b873593;
    uint32_t r1 = 15;
    uint32_t r2 = 13;
    uint32_t m = 5;
    uint32_t n = 0xe6546b64;
    uint32_t hash = seed;
    uint32_t length_in_bytes = ((uint32_t)length_in_bits / 8);
    uint32_t remaining_bits = length_in_bits % 8;

    for (size_t index = 0; (length_in_bytes - index) > 3; index += 4) {
        uint32_t k = *(uint32_t*)(key + index);
        k *= c1;
        k = _ebpf_rol(k, r1);
        k *= c2;

        hash ^= k;
        hash = _ebpf_rol(hash, r2);
        hash *= m;
        hash += n;
    }
    unsigned long remainder = 0;
    for (size_t index = length_in_bytes & (~3); index < length_in_bytes; index++) {
        remainder <<= 8;
        remainder |= key[index];
    }
    if (remaining_bits) {
        uint8_t bits = key[length_in_bytes];
        bits >>= (8 - remaining_bits);
        remainder <<= 8;
        remainder |= bits;
    }

    remainder *= c1;
    remainder = _ebpf_rol(remainder, r1);
    remainder *= c2;

    hash ^= remainder;
    hash ^= (uint32_t)length_in_bytes;
    hash *= 0x85ebca6b;
    hash ^= (hash >> r2);
    hash *= 0xc2b2ae35;
    hash ^= (hash >> 16);
    return hash;
}
#if defined(_M_X64)
static unsigned long
_ebpf_compute_crc32(_In_reads_(length_in_bytes) const uint8_t* key, size_t length_in_bytes, uint32_t seed)
{
    // First process 8 bytes at a time.
    uint32_t crc = seed;
    uint8_t* start = (uint8_t*)key;
    uint8_t* end = start + length_in_bytes;

    while ((end - start) >= 8) {
        crc = (uint32_t)_mm_crc32_u64(crc, *(uint64_t*)start);
        start += 8;
    }

    // Process 4 bytes at a time.
    while ((end - start) >= 4) {
        crc = _mm_crc32_u32(crc, *(uint32_t*)start);
        start += 4;
    }

    // Process remaining bytes.
    while ((end - start) > 0) {
        crc = _mm_crc32_u8(crc, *start);
        start++;
    }
    return crc;
}
#endif

/**
 * @brief Compare keys assuming they are integers.
 *
 * @param[in] hash_table Hash table the keys belong to.
 * @param[in] key_a First key.
 * @param[in] key_b Second key.
 *
 * @return -1 if key_a < key_b, 0 if key_a == key_b, 1 if key_a > key_b.
 */
static __forceinline int
_ebpf_hash_table_compare_integer_keys(
    _In_ const ebpf_hash_table_t* hash_table, _In_ const uint8_t* key_a, _In_ const uint8_t* key_b)
{
    uint64_t lhs;
    uint64_t rhs;
    switch (hash_table->key_size) {
    case 1:
        lhs = *key_a;
        rhs = *key_b;
        break;
    case 2:
        lhs = *(uint16_t*)key_a;
        rhs = *(uint16_t*)key_b;
        break;
    case 4:
        lhs = *(uint32_t*)key_a;
        rhs = *(uint32_t*)key_b;
        break;
    case 8:
        lhs = *(uint64_t*)key_a;
        rhs = *(uint64_t*)key_b;
        break;
    default:
        ebpf_assert(false);
        return 0;
    }
    return (lhs < rhs) ? -1 : (lhs > rhs) ? 1 : 0;
}

/**
 * @brief Compare keys that require extraction and compare as byte arrays.
 *
 * @param[in] hash_table Hash table the keys belong to.
 * @param[in] key_a First key.
 * @param[in] key_b Second key.
 *
 * @return -1 if key_a < key_b, 0 if key_a == key_b, 1 if key_a > key_b.
 */
static int
_ebpf_hash_table_compare_extracted_keys(
    _In_ const ebpf_hash_table_t* hash_table, _In_ const uint8_t* key_a, _In_ const uint8_t* key_b)
{
    size_t length_a_in_bits;
    size_t length_b_in_bits;
    const uint8_t* data_a;
    const uint8_t* data_b;
    uint8_t remainder_a;
    uint8_t remainder_b;

    hash_table->extract(key_a, &data_a, &length_a_in_bits);
    hash_table->extract(key_b, &data_b, &length_b_in_bits);

    if (length_a_in_bits < length_b_in_bits) {
        return -1;
    }
    if (length_a_in_bits > length_b_in_bits) {
        return 1;
    }

    int cmp_result = memcmp(data_a, data_b, length_a_in_bits / 8);

    // No match or length ends on a byte boundary.
    if (cmp_result != 0 || length_a_in_bits % 8 == 0) {
        return cmp_result;
    }
    // Check remaining high-order bits.
    remainder_a = data_a[length_a_in_bits / 8];
    remainder_b = data_b[length_b_in_bits / 8];
    remainder_a >>= 8 - (length_a_in_bits % 8);
    remainder_b >>= 8 - (length_b_in_bits % 8);
    if (remainder_a < remainder_b) {
        return -1;
    } else if (remainder_a > remainder_b) {
        return 1;
    } else {
        return 0;
    }
}

/**
 * @brief Wrapper to select the best comparison method for keys based on key size and extract function.
 *
 * @param[in] hash_table Hash table the keys belong to.
 * @param[in] key_a First key.
 * @param[in] key_b Second key.
 * @retval -1 if key_a < key_b
 * @retval 0 if key_a == key_b
 * @retval 1 if key_a > key_b
 */
static __forceinline int
_ebpf_hash_table_compare(_In_ const ebpf_hash_table_t* hash_table, _In_ const uint8_t* key_a, _In_ const uint8_t* key_b)
{
    if (hash_table->extract) {
        // If key is not integer and extract function is provided, use it to compare.
        return _ebpf_hash_table_compare_extracted_keys(hash_table, key_a, key_b);
    } else if ((hash_table->key_size & (hash_table->key_size - 1)) == 0 && hash_table->key_size <= 8) {
        // If key size is a power of 2 and less than or equal to 64 bits, compare as integers.
        return _ebpf_hash_table_compare_integer_keys(hash_table, key_a, key_b);
    } else {
        // Otherwise, compare as byte arrays.
        return memcmp(key_a, key_b, hash_table->key_size);
    }
}

/**
 * @brief Given a potentially non-comparable key value, extract the key and
 * compute the hash and convert it to a bucket index.
 *
 * @param[in] hash_table Hash table the keys belong to.
 * @param[in] key Key to hash.
 * @return Bucket index.
 */
static uint32_t
_ebpf_hash_table_compute_bucket_index(_In_ const ebpf_hash_table_t* hash_table, _In_ const uint8_t* key)
{
    if (!hash_table->extract) {
#if defined(_M_X64)
        if (ebpf_processor_supports_sse42) {
            return _ebpf_compute_crc32(key, hash_table->key_size, hash_table->seed) & hash_table->bucket_count_mask;
        } else {
            return _ebpf_murmur3_32(key, hash_table->key_size * 8, hash_table->seed) & hash_table->bucket_count_mask;
        }
#else
        return _ebpf_murmur3_32(key, hash_table->key_size * 8, hash_table->seed) & hash_table->bucket_count_mask;
#endif
    } else {
        uint8_t* data;
        size_t length;
        hash_table->extract(key, &data, &length);
        return _ebpf_murmur3_32(data, length, hash_table->seed) & hash_table->bucket_count_mask;
    }
}

/**
 * @brief Given a pointer to a bucket, compute the offset of a bucket entry.
 *
 * @param [in] key_size Size of key.
 * @param [in] bucket Pointer to start of the bucket.
 * @param [in] index Index into the bucket.
 * @return Pointer to the ebpf_hash_bucket_entry_t.
 */
static ebpf_hash_bucket_entry_t*
_ebpf_hash_table_bucket_entry(size_t key_size, _In_ const ebpf_hash_bucket_header_t* bucket, size_t index)
{
    uint8_t* offset = (uint8_t*)bucket->entries;
    size_t entry_size = EBPF_OFFSET_OF(ebpf_hash_bucket_entry_t, key) + key_size;

    return (ebpf_hash_bucket_entry_t*)(offset + (size_t)index * entry_size);
}

/**
 * @brief Helper function to ensure correct memory ordering when reading a bucket from the hash table.
 *
 * @param[in] hash_table Pointer to the hash table.
 * @param[in] bucket_index Index of the bucket to read.
 * @return Pointer to the bucket or NULL if the bucket is empty.
 */
static inline ebpf_hash_bucket_header_t*
_ebpf_hash_table_get_bucket(_In_ const ebpf_hash_table_t* hash_table, size_t bucket_index)
{
    return (ebpf_hash_bucket_header_t*)ReadSizeTAcquire((ULONG_PTR*)&(hash_table->buckets[bucket_index].header));
}

/**
 * @brief Helper function to ensure correct memory ordering when writing a bucket to the hash table.
 *
 * @param[in] hash_table Pointer to the hash table.
 * @param[in] bucket_index Index of the bucket to write.
 * @param[in] bucket Bucket pointer to write.
 */
static inline void
_ebpf_hash_table_set_bucket(
    _In_ ebpf_hash_table_t* hash_table, size_t bucket_index, _In_opt_ ebpf_hash_bucket_header_t* bucket)
{
    WriteSizeTRelease((ULONG_PTR*)&(hash_table->buckets[bucket_index].header), (ULONG_PTR)bucket);
}

/**
 * @brief Build a replacement bucket with the given entry inserted at the end.
 * Caller must free the old bucket.
 * Caller must ensure that the entry is not already in the bucket.
 *
 * @param[in] hash_table The hash table.
 * @param[in] old_bucket The immutable bucket to copy.
 * @param[in] key The key to insert.
 * @param[in, out] data The copy of the value to insert. On success the new_bucket owns this memory.
 * @param[out] new_bucket The new bucket with the entry inserted. On success the caller owns this memory.
 * @retval EBPF_SUCCESS The operation was successful.
 * @retval EBPF_NO_MEMORY Unable to allocate resources for this operation.
 */
static ebpf_result_t
_ebpf_hash_table_bucket_insert(
    _Inout_ ebpf_hash_table_t* hash_table,
    _In_opt_ const ebpf_hash_bucket_header_t* old_bucket,
    _In_ const uint8_t* key,
    _Inout_opt_ uint8_t* data,
    _Outptr_ ebpf_hash_bucket_header_t** new_bucket)
{
    ebpf_result_t result;
    size_t entry_size = EBPF_OFFSET_OF(ebpf_hash_bucket_entry_t, key) + hash_table->key_size;
    size_t old_bucket_size = old_bucket ? entry_size * old_bucket->count + sizeof(ebpf_hash_bucket_header_t) : 0;
    size_t new_bucket_size =
        entry_size * ((old_bucket ? old_bucket->count : 0) + 1) + sizeof(ebpf_hash_bucket_header_t);
    ebpf_hash_bucket_header_t* local_new_bucket = NULL;
    ebpf_hash_bucket_header_t* backup_bucket = NULL;

    if (hash_table->max_entry_count != EBPF_HASH_TABLE_NO_LIMIT) {
        size_t new_entry_count = ebpf_interlocked_increment_int64_no_fence((volatile int64_t*)&hash_table->entry_count);
        if (new_entry_count > hash_table->max_entry_count) {
            result = EBPF_OUT_OF_SPACE;
            goto Done;
        }
    }

    // Allocate new bucket.
    local_new_bucket = hash_table->allocate(new_bucket_size);
    if (!local_new_bucket) {
        result = EBPF_NO_MEMORY;
        goto Done;
    }

    // Allocate a new backup bucket.
    if (old_bucket_size) {
        backup_bucket = hash_table->allocate(old_bucket_size);
        if (!backup_bucket) {
            result = EBPF_NO_MEMORY;
            goto Done;
        }
        backup_bucket->count = old_bucket->count;
    }

    // Copy old bucket into new bucket.
    memcpy(local_new_bucket, old_bucket, old_bucket_size);

    // Append new key, data, and backup bucket.
    ebpf_hash_bucket_entry_t* entry =
        _ebpf_hash_table_bucket_entry(hash_table->key_size, local_new_bucket, local_new_bucket->count);

    entry->backup_bucket = backup_bucket;
    backup_bucket = NULL;
    entry->data = data;
    memcpy(entry->key, key, hash_table->key_size);
    local_new_bucket->count++;

    *new_bucket = local_new_bucket;
    local_new_bucket = NULL;

    result = EBPF_SUCCESS;

Done:
    hash_table->free(local_new_bucket);
    hash_table->free(backup_bucket);

    if (hash_table->max_entry_count != EBPF_HASH_TABLE_NO_LIMIT) {
        if (result != EBPF_SUCCESS) {
            ebpf_interlocked_decrement_int64_no_fence((volatile int64_t*)&hash_table->entry_count);
        }
    }

    return result;
}

/**
 * @brief Build a replacement bucket with the given entry removed.
 * Caller must free the old bucket.
 * Memory from the last entry's backup_bucket is used to build the new bucket.
 * Caller must ensure that the entry is in the bucket.
 * Operation can't fail.
 *
 * @param[in] hash_table The hash table.
 * @param[in] old_bucket The immutable old bucket to copy.
 * @param[in] key_index Location of the key to remove.
 * @param[out] new_bucket The new bucket with the entry removed. On success the caller owns this memory.
 */
static void
_ebpf_hash_table_bucket_delete(
    _Inout_ ebpf_hash_table_t* hash_table,
    _In_ const ebpf_hash_bucket_header_t* old_bucket,
    size_t key_index,
    _Outptr_result_maybenull_ ebpf_hash_bucket_header_t** new_bucket)
{
    ebpf_hash_bucket_header_t* backup_bucket =
        _ebpf_hash_table_bucket_entry(hash_table->key_size, old_bucket, old_bucket->count - 1)->backup_bucket;

    // Delete the bucket if removing last entry.
    if (old_bucket->count == 1) {
        ebpf_assert(backup_bucket == NULL);
        *new_bucket = backup_bucket;
        goto Done;
    }
    ebpf_assert(backup_bucket->count == old_bucket->count - 1);

    // Reset bucket entry count.
    backup_bucket->count = 0;

    // Copy key and value from each entry into the backup bucket.
    for (size_t index = 0; index < old_bucket->count; index++) {
        if (index == key_index) {
            continue;
        }
        const ebpf_hash_bucket_entry_t* old_entry =
            _ebpf_hash_table_bucket_entry(hash_table->key_size, old_bucket, index);
        ebpf_hash_bucket_entry_t* new_entry =
            _ebpf_hash_table_bucket_entry(hash_table->key_size, backup_bucket, backup_bucket->count);

        new_entry->data = old_entry->data;
        memcpy(new_entry->key, old_entry->key, hash_table->key_size);
        backup_bucket->count++;
    }

    // Copy each entries backup bucket into the backup bucket.
    for (size_t index = 0; index < old_bucket->count - 1; index++) {
        ebpf_hash_bucket_entry_t* old_entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, old_bucket, index);
        ebpf_hash_bucket_entry_t* new_entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, backup_bucket, index);

        new_entry->backup_bucket = old_entry->backup_bucket;
        // Bucket at index N > 0 should have a backup bucket of size N - 1.
        ebpf_assert(index == 0 || new_entry->backup_bucket);
        ebpf_assert(index == 0 || new_entry->backup_bucket->count == index);
    }

    *new_bucket = backup_bucket;

Done:
    if (hash_table->max_entry_count != EBPF_HASH_TABLE_NO_LIMIT) {
        ebpf_interlocked_decrement_int64_no_fence((volatile int64_t*)&hash_table->entry_count);
    }

    return;
}

/**
 * @brief Build a new bucket with the given entry updated.
 * Caller must free the old bucket.
 * Caller must ensure that the entry is in the bucket.
 *
 * @param[in] hash_table Hash table.
 * @param[in] old_bucket The immutable old bucket to copy.
 * @param[in] key_index The location of the key to update.
 * @param[in, out] data A copy of the data to update.
 * @param[out] new_bucket The new bucket with the entry updated. On success the caller owns this memory.
 *
 * @retval EBPF_SUCCESS The operation was successful.
 * @retval EBPF_NO_MEMORY Unable to allocate memory for the new bucket.
 */
static ebpf_result_t
_ebpf_hash_table_bucket_update(
    _Inout_ ebpf_hash_table_t* hash_table,
    _In_ const ebpf_hash_bucket_header_t* old_bucket,
    size_t key_index,
    _Inout_opt_ uint8_t* data,
    _Outptr_ ebpf_hash_bucket_header_t** new_bucket)
{
    ebpf_result_t result;
    size_t entry_size = EBPF_OFFSET_OF(ebpf_hash_bucket_entry_t, key) + hash_table->key_size;
    size_t old_bucket_size = entry_size * old_bucket->count + sizeof(ebpf_hash_bucket_header_t);
    ebpf_hash_bucket_header_t* local_new_bucket = NULL;

    // Allocate new bucket.
    local_new_bucket = hash_table->allocate(old_bucket_size);
    if (!local_new_bucket) {
        result = EBPF_NO_MEMORY;
        goto Done;
    }

    // Copy old bucket into new bucket.
    memcpy(local_new_bucket, old_bucket, old_bucket_size);

    ebpf_hash_bucket_entry_t* entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, local_new_bucket, key_index);

    entry->data = data;

    *new_bucket = local_new_bucket;
    local_new_bucket = NULL;
    result = EBPF_SUCCESS;

Done:
    hash_table->free(local_new_bucket);
    return result;
}

/**
 * @brief Perform an atomic replacement of a bucket in the hash table.
 * Operations include insert, update and delete of elements.
 *
 * @param[in] hash_table Hash table to update.
 * @param[in] key Key to operate on.
 * @param[in] value Value to be inserted or NULL.
 * @param[in] operation Operation to perform.
 * @retval EBPF_SUCCESS The operation succeeded.
 * @retval EBPF_KEY_NOT_FOUND The specified key is not present in the bucket.
 * @retval EBPF_NO_MEMORY Insufficient memory to construct new bucket or value.
 * @retval EBPF_OUT_OF_SPACE Maximum number of entries reached.
 */
static ebpf_result_t
_ebpf_hash_table_replace_bucket(
    _Inout_ ebpf_hash_table_t* hash_table,
    _In_ const uint8_t* key,
    _In_opt_ const uint8_t* value,
    ebpf_hash_bucket_operation_t operation)
{
    ebpf_result_t result = EBPF_SUCCESS;
    size_t index;
    uint32_t bucket_index;
    uint8_t* old_data = NULL;
    uint8_t* new_data = NULL;
    ebpf_hash_bucket_header_t* old_bucket = NULL;
    ebpf_hash_bucket_header_t* new_bucket = NULL;

    bucket_index = _ebpf_hash_table_compute_bucket_index(hash_table, key);

    // Lock the bucket.
    ebpf_lock_state_t state = ebpf_lock_lock(&hash_table->buckets[bucket_index].lock);

    // Make a copy of the value to insert.
    if (operation != EBPF_HASH_BUCKET_OPERATION_DELETE) {
        new_data = hash_table->allocate(hash_table->value_size + hash_table->supplemental_value_size);
        if (!new_data) {
            result = EBPF_NO_MEMORY;
            goto Done;
        }
        // If the value is NULL, then the caller wants to insert a zeroed value.
        if (value) {
            memcpy(new_data, value, hash_table->value_size);
        } else {
            memset(new_data, 0, hash_table->value_size + hash_table->supplemental_value_size);
        }
        if (hash_table->notification_callback) {
            hash_table->notification_callback(
                hash_table->notification_context, EBPF_HASH_TABLE_NOTIFICATION_TYPE_ALLOCATE, key, new_data);
        }
    }

    // Find the old bucket.
    old_bucket = _ebpf_hash_table_get_bucket(hash_table, bucket_index);
    size_t old_bucket_count = old_bucket ? old_bucket->count : 0;

    // Find the entry in the bucket, if any.
    for (index = 0; index < old_bucket_count; index++) {
        ebpf_hash_bucket_entry_t* entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, old_bucket, index);
        if (_ebpf_hash_table_compare(hash_table, key, entry->key) == 0) {
            old_data = entry->data;
            break;
        }
    }

    switch (operation) {
    case EBPF_HASH_BUCKET_OPERATION_INSERT_OR_UPDATE:
        if (index == old_bucket_count) {
            result = _ebpf_hash_table_bucket_insert(hash_table, old_bucket, key, new_data, &new_bucket);
        } else {
            result = _ebpf_hash_table_bucket_update(hash_table, old_bucket, index, new_data, &new_bucket);
        }
        break;
    case EBPF_HASH_BUCKET_OPERATION_INSERT:
        if (index != old_bucket_count) {
            result = EBPF_OBJECT_ALREADY_EXISTS;
        } else {
            result = _ebpf_hash_table_bucket_insert(hash_table, old_bucket, key, new_data, &new_bucket);
        }
        break;
    case EBPF_HASH_BUCKET_OPERATION_UPDATE:
        if (index == old_bucket_count) {
            result = EBPF_KEY_NOT_FOUND;
        } else {
            result = _ebpf_hash_table_bucket_update(hash_table, old_bucket, index, new_data, &new_bucket);
        }
        break;
    case EBPF_HASH_BUCKET_OPERATION_DELETE:
        if (index == old_bucket_count) {
            result = EBPF_KEY_NOT_FOUND;
        } else {
            _ebpf_hash_table_bucket_delete(hash_table, old_bucket, index, &new_bucket);
        }
        break;
    default:
        result = EBPF_INVALID_ARGUMENT;
        break;
    }

    if (result != EBPF_SUCCESS) {
        old_bucket = NULL;
        old_data = NULL;
        goto Done;
    }

    // If a value was inserted and deleted, the count of values in the hash table did not change.

    // Update the bucket in the hash table.
    // From this point on the new bucket is immutable.
    _ebpf_hash_table_set_bucket(hash_table, bucket_index, new_bucket);
    new_data = NULL;
    new_bucket = NULL;

Done:
    ebpf_lock_unlock(&hash_table->buckets[bucket_index].lock, state);

    if (hash_table->notification_callback) {
        if (new_data) {
            hash_table->notification_callback(
                hash_table->notification_context, EBPF_HASH_TABLE_NOTIFICATION_TYPE_FREE, key, new_data);
        }
        if (old_data) {
            hash_table->notification_callback(
                hash_table->notification_context, EBPF_HASH_TABLE_NOTIFICATION_TYPE_FREE, key, old_data);
        }
    }

    // Free new_data if any. This occurs if the insert failed.
    hash_table->free(new_data);
    // Free old_data if any. This occurs if a delete or update succeeded.
    hash_table->free(old_data);
    // The new bucket should always be inserted into the hash table.
    ebpf_assert(new_bucket == NULL);
    // Free the old bucket if any. This occurs if a insert, delete, or update succeeded.
    hash_table->free(old_bucket);
    return result;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_create(_Out_ ebpf_hash_table_t** hash_table, _In_ const ebpf_hash_table_creation_options_t* options)
{
    ebpf_result_t retval;
    ebpf_hash_table_t* table = NULL;
    size_t table_size = 0;
    // Select default values for the hash table.
    size_t bucket_count =
        options->minimum_bucket_count ? options->minimum_bucket_count : EBPF_HASH_TABLE_DEFAULT_BUCKET_COUNT;
    void* (*allocate)(size_t size) = options->allocate ? options->allocate : ebpf_epoch_allocate;
    void (*free)(void* memory) = options->free ? options->free : ebpf_epoch_free;

    // Increase bucket_count to next power of 2.
    unsigned long msb_index;
    _BitScanReverse64(&msb_index, bucket_count);

    if (bucket_count != (1ull << msb_index)) {
        bucket_count = 1ull << (msb_index + 1ull);
    }

    retval = ebpf_safe_size_t_multiply(sizeof(ebpf_hash_bucket_header_and_lock_t), bucket_count, &table_size);
    if (retval != EBPF_SUCCESS) {
        goto Done;
    }
    retval = ebpf_safe_size_t_add(table_size, EBPF_OFFSET_OF(ebpf_hash_table_t, buckets), &table_size);
    if (retval != EBPF_SUCCESS) {
        goto Done;
    }

    table = allocate(table_size);
    if (table == NULL) {
        retval = EBPF_NO_MEMORY;
        goto Done;
    }

    table->key_size = options->key_size;
    table->value_size = options->value_size;
    table->allocate = allocate;
    table->free = free;
    table->bucket_count = bucket_count;
    table->bucket_count_mask = bucket_count - 1;
    table->entry_count = 0;
    table->seed = ebpf_random_uint32();
    table->extract = options->extract_function;
#if defined(NDEBUG)
    table->max_entry_count = options->max_entries;
#else
    // If debug mode, treat EBPF_HASH_TABLE_NO_LIMIT as -1 to ensure that entries are counted.
    table->max_entry_count = options->max_entries == EBPF_HASH_TABLE_NO_LIMIT ? -1 : options->max_entries;
#endif

    table->supplemental_value_size = options->supplemental_value_size;
    table->notification_context = options->notification_context;
    table->notification_callback = options->notification_callback;

    *hash_table = table;
    retval = EBPF_SUCCESS;
Done:
    return retval;
}

void
ebpf_hash_table_destroy(_In_opt_ _Post_ptr_invalid_ ebpf_hash_table_t* hash_table)
{
    size_t index;
    if (!hash_table) {
        return;
    }

    for (index = 0; index < hash_table->bucket_count; index++) {
        ebpf_hash_bucket_header_t* bucket = (ebpf_hash_bucket_header_t*)hash_table->buckets[index].header;
        if (bucket) {
            size_t inner_index;
            for (inner_index = 0; inner_index < bucket->count; inner_index++) {
                ebpf_hash_bucket_entry_t* entry =
                    _ebpf_hash_table_bucket_entry(hash_table->key_size, bucket, inner_index);
                hash_table->free(entry->data);
                hash_table->free(entry->backup_bucket);
            }
            hash_table->free(bucket);
            hash_table->buckets[index].header = NULL;
        }
    }
    hash_table->free(hash_table);
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_find(_In_ const ebpf_hash_table_t* hash_table, _In_ const uint8_t* key, _Outptr_ uint8_t** value)
{
    ebpf_result_t retval;
    uint32_t bucket_index;
    uint8_t* data = NULL;
    size_t index;
    ebpf_hash_bucket_header_t* bucket;

    if (!hash_table || !key) {
        retval = EBPF_INVALID_ARGUMENT;
        goto Done;
    }

    bucket_index = _ebpf_hash_table_compute_bucket_index(hash_table, key);
    bucket = _ebpf_hash_table_get_bucket(hash_table, bucket_index);
    if (!bucket) {
        retval = EBPF_KEY_NOT_FOUND;
        goto Done;
    }

    for (index = 0; index < bucket->count; index++) {
        ebpf_hash_bucket_entry_t* entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, bucket, index);
        if (_ebpf_hash_table_compare(hash_table, key, entry->key) == 0) {
            data = entry->data;
            break;
        }
    }

    if (!data) {
        retval = EBPF_KEY_NOT_FOUND;
        goto Done;
    }

    PrefetchForWrite(data);

    *value = data;
    if (hash_table->notification_callback) {
        hash_table->notification_callback(
            hash_table->notification_context, EBPF_HASH_TABLE_NOTIFICATION_TYPE_USE, key, data);
    }
    retval = EBPF_SUCCESS;
Done:
    return retval;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_update(
    _Inout_ ebpf_hash_table_t* hash_table,
    _In_ const uint8_t* key,
    _In_opt_ const uint8_t* value,
    ebpf_hash_table_operations_t operation)
{
    ebpf_result_t retval;
    ebpf_hash_bucket_operation_t bucket_operation;

    if (!hash_table || !key) {
        retval = EBPF_INVALID_ARGUMENT;
        goto Done;
    }

    switch (operation) {
    case EBPF_HASH_TABLE_OPERATION_ANY:
        bucket_operation = EBPF_HASH_BUCKET_OPERATION_INSERT_OR_UPDATE;
        break;
    case EBPF_HASH_TABLE_OPERATION_INSERT:
        bucket_operation = EBPF_HASH_BUCKET_OPERATION_INSERT;
        break;
    case EBPF_HASH_TABLE_OPERATION_REPLACE:
        bucket_operation = EBPF_HASH_BUCKET_OPERATION_UPDATE;
        break;
    default:
        retval = EBPF_INVALID_ARGUMENT;
        goto Done;
    }

    retval = _ebpf_hash_table_replace_bucket(hash_table, key, value, bucket_operation);
Done:
    return retval;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_delete(_Inout_ ebpf_hash_table_t* hash_table, _In_ const uint8_t* key)
{
    ebpf_result_t retval;

    if (!hash_table || !key) {
        retval = EBPF_INVALID_ARGUMENT;
        goto Done;
    }

    retval = _ebpf_hash_table_replace_bucket(hash_table, key, NULL, EBPF_HASH_BUCKET_OPERATION_DELETE);

Done:
    return retval;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_next_key_pointer_and_value(
    _In_ const ebpf_hash_table_t* hash_table,
    _In_opt_ const uint8_t* previous_key,
    _Outptr_ uint8_t** next_key_pointer,
    _Outptr_opt_ uint8_t** value)
{
    ebpf_result_t result = EBPF_SUCCESS;
    uint32_t starting_bucket_index;
    ebpf_hash_bucket_entry_t* next_entry = NULL;
    size_t bucket_index;
    size_t data_index;
    bool found_entry = false;

    if (!hash_table || !next_key_pointer) {
        result = EBPF_INVALID_ARGUMENT;
        goto Done;
    }

    starting_bucket_index =
        (previous_key != NULL) ? _ebpf_hash_table_compute_bucket_index(hash_table, previous_key) : 0;

    for (bucket_index = starting_bucket_index; bucket_index < hash_table->bucket_count; bucket_index++) {
        ebpf_hash_bucket_header_t* bucket = _ebpf_hash_table_get_bucket(hash_table, bucket_index);
        // Skip empty buckets.
        if (!bucket) {
            continue;
        }

        // Pick first entry if no previous key.
        if (!previous_key) {
            next_entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, bucket, 0);
            break;
        }

        for (data_index = 0; data_index < bucket->count; data_index++) {
            ebpf_hash_bucket_entry_t* entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, bucket, data_index);
            if (!entry) {
                result = EBPF_INVALID_ARGUMENT;
                goto Done;
            }
            // Do we have the previous key?
            if (found_entry) {
                // Yes, then this is the next key.
                next_entry = entry;
                break;
            }

            // Is this the previous key?
            if (_ebpf_hash_table_compare(hash_table, previous_key, entry->key) == 0) {
                // Yes, record its location.
                found_entry = true;
            }
        }

        if (next_entry) {
            break;
        }
    }

    // If we were given a previous key, and the searched key was not found in the hash table, we return
    // EBPF_KEY_NOT_FOUND, so that the caller can detect that the key is missing, and return the first key (as per
    // 'bpf_map_get_next_key' specs).
    if (!found_entry && previous_key != NULL) {
        result = EBPF_KEY_NOT_FOUND;
        goto Done;
    }

    if (!next_entry) {
        result = EBPF_NO_MORE_KEYS;
        goto Done;
    }

    result = EBPF_SUCCESS;

    if (value) {
        *value = next_entry->data;
    }

    *next_key_pointer = next_entry->key;

Done:

    return result;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_next_key_and_value(
    _In_ const ebpf_hash_table_t* hash_table,
    _In_opt_ const uint8_t* previous_key,
    _Out_ uint8_t* next_key,
    _Inout_opt_ uint8_t** next_value)
{
    uint8_t* next_key_pointer;
    ebpf_result_t result =
        ebpf_hash_table_next_key_pointer_and_value(hash_table, previous_key, &next_key_pointer, next_value);
    if (result == EBPF_SUCCESS) {
        memcpy(next_key, next_key_pointer, hash_table->key_size);
    }
    return result;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_next_key(
    _In_ const ebpf_hash_table_t* hash_table, _In_opt_ const uint8_t* previous_key, _Out_ uint8_t* next_key)
{
    return ebpf_hash_table_next_key_and_value(hash_table, previous_key, next_key, NULL);
}

size_t
ebpf_hash_table_key_count(_In_ const ebpf_hash_table_t* hash_table)
{
    // If max_entry_count is being enforced, return the current count.
    if (hash_table->max_entry_count != EBPF_HASH_TABLE_NO_LIMIT) {
        return hash_table->entry_count;
    } else {
        // Otherwise, count the keys in the hash table.
        size_t count = 0;
        for (size_t i = 0; i < hash_table->bucket_count; i++) {
            ebpf_hash_bucket_header_t* bucket = hash_table->buckets[i].header;
            if (bucket) {
                count += bucket->count;
            }
        }
        return count;
    }
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_iterate(
    _In_ const ebpf_hash_table_t* hash_table,
    _Inout_ size_t* bucket,
    _Inout_ size_t* count,
    _Out_writes_(*count) const uint8_t** keys,
    _Out_writes_(*count) const uint8_t** values)
{
    size_t bucket_index = *bucket;
    size_t index = 0;
    size_t remaining_space = *count;
    size_t next_bucket_count = 0;
    if (bucket_index >= hash_table->bucket_count) {
        return EBPF_NO_MORE_KEYS;
    }

    while (remaining_space > 0) {
        if (bucket_index >= hash_table->bucket_count) {
            break;
        }
        ebpf_hash_bucket_header_t* bucket_header = _ebpf_hash_table_get_bucket(hash_table, bucket_index);
        // Check if the bucket is empty.
        if (!bucket_header) {
            bucket_index++;
            continue;
        }
        // Check if the next bucket will fit in the remaining space.
        next_bucket_count = bucket_header->count;
        if (remaining_space < next_bucket_count) {
            break;
        }
        // Copy the keys and values.
        for (size_t i = 0; i < next_bucket_count; i++) {
            ebpf_hash_bucket_entry_t* entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, bucket_header, i);
            if (!entry) {
                return EBPF_INVALID_ARGUMENT;
            }
            keys[index] = entry->key;
            values[index] = entry->data;
            index++;
            remaining_space--;
        }
        bucket_index++;
    }

    // If the bucket_index did not change, then there wasn't enough space to copy the next bucket.
    if (*bucket == bucket_index) {
        *count = next_bucket_count;
        return EBPF_INSUFFICIENT_BUFFER;
    }

    *bucket = bucket_index;
    *count = index;
    return EBPF_SUCCESS;
}

_Must_inspect_result_ ebpf_result_t
ebpf_hash_table_next_key_and_value_sorted(
    _In_ const ebpf_hash_table_t* hash_table,
    _In_opt_ const uint8_t* previous_key,
    _In_ int (*compare)(_In_ const uint8_t* key1, _In_ const uint8_t* key2),
    _In_opt_ void* filter_context,
    _In_ bool (*filter)(_In_opt_ void* filter_context, _In_ const uint8_t* key, _In_ const uint8_t* value),
    _Out_ uint8_t* next_key,
    _Inout_opt_ uint8_t** next_value)
{
    uint8_t* next_key_pointer = NULL;
    uint8_t* next_value_pointer = NULL;
    for (size_t bucket_index = 0; bucket_index < hash_table->bucket_count; bucket_index++) {
        ebpf_hash_bucket_header_t* bucket_header = _ebpf_hash_table_get_bucket(hash_table, bucket_index);
        if (!bucket_header) {
            continue;
        }
        for (size_t i = 0; i < bucket_header->count; i++) {
            ebpf_hash_bucket_entry_t* entry = _ebpf_hash_table_bucket_entry(hash_table->key_size, bucket_header, i);
            if (!entry) {
                return EBPF_INVALID_ARGUMENT;
            }
            if (previous_key == NULL || compare(previous_key, entry->key) < 0) {
                if (next_key_pointer == NULL || compare(next_key_pointer, entry->key) > 0) {
                    if (filter(filter_context, entry->key, entry->data)) {
                        next_key_pointer = entry->key;
                        next_value_pointer = entry->data;
                    }
                }
            }
        }
    }
    if (next_key_pointer == NULL) {
        return EBPF_NO_MORE_KEYS;
    }

    memcpy(next_key, next_key_pointer, hash_table->key_size);
    if (next_value) {
        *next_value = next_value_pointer;
    }

    return EBPF_SUCCESS;
}
