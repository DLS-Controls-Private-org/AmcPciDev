#include <kunit/test.h>
#include "prom_processing.h"
#include "test_assets/test_prom1.c"


static u64 base_to_u64(u16 *base)
{
    return (u64) base[0] | (u64) base[1] << 16 | (u64) base[2] << 32;
}


static void test_load_prom_validation_ok(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1);
    KUNIT_EXPECT_NOT_ERR_OR_NULL(test, context);
    if (!IS_ERR(context))
        release_prom_context(context);
}


static void test_load_prom_validation_fail(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1_corrupted);
    KUNIT_EXPECT_TRUE(test, IS_ERR(context));
}


static void test_load_prom_data(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1);
    KUNIT_EXPECT_EQ(test, 0, memcmp(context->buff, test_prom1,
                    test_prom1_size));
    KUNIT_EXPECT_EQ(test, test_prom1_size, context->data_len);
    KUNIT_EXPECT_EQ(test, test_prom1_nentries, context->nentries);
    release_prom_context(context);
}


static void test_prom_first_entry(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1);
    struct prom_device_entry *dev_entry =
        (struct prom_device_entry *) prom_first_entry(context);
    KUNIT_EXPECT_EQ(test, 0, memcmp(dev_entry->name, "test_dev", 8));
    KUNIT_EXPECT_EQ(test, (u8) PROM_DEVICE_TAG, dev_entry->tag);
    KUNIT_EXPECT_EQ(test, (u8) 9, dev_entry->size);
    release_prom_context(context);
}


static void test_prom_next_entry(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1);
    struct prom_entry *entry = prom_first_entry(context);
    entry = prom_next_entry(entry);
    struct prom_dma_entry *dma_entry = (struct prom_dma_entry *) entry;
    KUNIT_EXPECT_EQ(test, (u8) PROM_DMA_TAG, dma_entry->tag);
    KUNIT_EXPECT_EQ(test, 0, memcmp(dma_entry->name, "memA", 4));
    KUNIT_EXPECT_TRUE(test, base_to_u64(dma_entry->base) == 0);
    KUNIT_EXPECT_EQ(test, (u32) 0x12131415, dma_entry->length);
    KUNIT_EXPECT_EQ(test, (u8) 4, dma_entry->perm);
    entry = prom_next_entry(entry);
    dma_entry = (struct prom_dma_entry *) entry;
    KUNIT_EXPECT_EQ(test, (u8) PROM_DMA_TAG, dma_entry->tag);
    KUNIT_EXPECT_EQ(test, 0, memcmp(dma_entry->name, "memB", 4));
    KUNIT_EXPECT_TRUE(test, base_to_u64(dma_entry->base) == 0xabcd11223344);
    KUNIT_EXPECT_EQ(test, (u32) 0x8912345, dma_entry->length);
    KUNIT_EXPECT_EQ(test, (u8) 2, dma_entry->perm);
    entry = prom_next_entry(entry);
    dma_entry = (struct prom_dma_entry *) entry;
    KUNIT_EXPECT_EQ(test, (u8) PROM_DMA_TAG, dma_entry->tag);
    KUNIT_EXPECT_EQ(test, 0, memcmp(dma_entry->name, "memC", 4));
    KUNIT_EXPECT_TRUE(test,  base_to_u64(dma_entry->base) == 0x42616263);
    KUNIT_EXPECT_EQ(test, (u32) 0x100, dma_entry->length);
    KUNIT_EXPECT_EQ(test, (u8) 6, dma_entry->perm);
    entry = prom_next_entry(entry);
    KUNIT_EXPECT_TRUE(test, entry->tag == PROM_END_TAG);
    release_prom_context(context);
}


static void test_prom_find_entry_available(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1);
    struct prom_dma_entry *dma_entry =
        (struct prom_dma_entry *) prom_find_entry(context, 2);
    KUNIT_EXPECT_EQ(test, (u8) PROM_DMA_TAG, dma_entry->tag);
    KUNIT_EXPECT_EQ(test, 0, memcmp(dma_entry->name, "memB", 4));
    KUNIT_EXPECT_TRUE(test, base_to_u64(dma_entry->base) == 0xabcd11223344);
    KUNIT_EXPECT_EQ(test, (u32) 0x8912345, dma_entry->length);
    KUNIT_EXPECT_EQ(test, (u8) 2, dma_entry->perm);
    dma_entry = (struct prom_dma_entry *) prom_find_entry(context, 3);
    KUNIT_EXPECT_EQ(test, (u8) PROM_DMA_TAG, dma_entry->tag);
    KUNIT_EXPECT_EQ(test, 0, memcmp(dma_entry->name, "memC", 4));
    KUNIT_EXPECT_TRUE(test,  base_to_u64(dma_entry->base) == 0x42616263);
    KUNIT_EXPECT_EQ(test, (u32) 0x100, dma_entry->length);
    KUNIT_EXPECT_EQ(test, (u8) 6, dma_entry->perm);
    release_prom_context(context);
}


static void test_prom_find_entry_missing(struct kunit *test)
{
    struct prom_context *context = load_prom((void *) test_prom1);
    struct prom_entry *entry = prom_find_entry(context, 4);
    KUNIT_EXPECT_PTR_EQ(test, (struct prom_entry *) NULL, entry);
    release_prom_context(context);
}


static struct kunit_case prom_processing_test_cases[] = {
    KUNIT_CASE(test_load_prom_validation_ok),
    KUNIT_CASE(test_load_prom_validation_fail),
    KUNIT_CASE(test_load_prom_data),
    KUNIT_CASE(test_prom_first_entry),
    KUNIT_CASE(test_prom_next_entry),
    KUNIT_CASE(test_prom_find_entry_available),
    KUNIT_CASE(test_prom_find_entry_missing),
    {}
};


static struct kunit_suite prom_processing_test_suite = {
    .name = "prom_processing",
    .test_cases = prom_processing_test_cases,
};


kunit_test_suite(prom_processing_test_suite);

MODULE_LICENSE("GPL");
