/**
 * @file test_allocator.cpp
 *
 * Unit tests for allocator plumbing in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

#include <cstdlib>

struct CountCtx {
  size_t mallocs = 0;
  size_t callocs = 0;
  size_t reallocs = 0;
  size_t frees = 0;
};

static void * count_malloc(void * ctx, size_t size) {
  auto * c = static_cast<CountCtx *>(ctx);
  c->mallocs++;
  return std::malloc(size);
}

static void * count_calloc(void * ctx, size_t nitems, size_t size) {
  auto * c = static_cast<CountCtx *>(ctx);
  c->callocs++;
  return std::calloc(nitems, size);
}

static void * count_realloc(void * ctx, void * ptr, size_t size) {
  auto * c = static_cast<CountCtx *>(ctx);
  c->reallocs++;
  return std::realloc(ptr, size);
}

static void count_free(void * ctx, void * ptr) {
  auto * c = static_cast<CountCtx *>(ctx);
  c->frees++;
  std::free(ptr);
}

TEST(Allocator, RegistryUsesProvidedAllocator) {
  CountCtx ctx;
  gcomp_allocator_t alloc = {
      .ctx = &ctx,
      .malloc_fn = count_malloc,
      .calloc_fn = count_calloc,
      .realloc_fn = count_realloc,
      .free_fn = count_free,
  };

  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(&alloc, &reg), GCOMP_OK);
  ASSERT_NE(reg, nullptr);

  gcomp_registry_destroy(reg);

  // At minimum, registry allocation/free should have used the allocator.
  EXPECT_GE(ctx.callocs + ctx.mallocs, 1u);
  EXPECT_GE(ctx.frees, 1u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
