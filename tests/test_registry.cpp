/**
 * @file test_registry.cpp
 *
 * Unit tests for the registry API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

// Mock method for testing
static gcomp_status_t mock_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  (void)registry;
  (void)options;
  (void)encoder_out;
  return GCOMP_ERR_UNSUPPORTED; // Mock doesn't actually create encoders
}

static gcomp_status_t mock_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  (void)registry;
  (void)options;
  (void)decoder_out;
  return GCOMP_ERR_UNSUPPORTED; // Mock doesn't actually create decoders
}

static void mock_destroy_encoder(gcomp_encoder_t * encoder) {
  (void)encoder;
}

static void mock_destroy_decoder(gcomp_decoder_t * decoder) {
  (void)decoder;
}

// Create a mock method for testing
static gcomp_method_t create_mock_method(
    const char * name, gcomp_capabilities_t caps) {
  gcomp_method_t method = {};
  method.abi_version = 1;
  method.size = sizeof(gcomp_method_t);
  method.name = name;
  method.capabilities = caps;
  method.create_encoder = mock_create_encoder;
  method.create_decoder = mock_create_decoder;
  method.destroy_encoder = mock_destroy_encoder;
  method.destroy_decoder = mock_destroy_decoder;
  return method;
}

class RegistryTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a fresh registry for each test
    gcomp_registry_create(nullptr, &registry_);
  }

  void TearDown() override {
    if (registry_ != nullptr) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

// Test gcomp_registry_default()
TEST(RegistryDefaultTest, ReturnsNonNull) {
  gcomp_registry_t * reg = gcomp_registry_default();
  EXPECT_NE(reg, nullptr);
}

TEST(RegistryDefaultTest, SingletonBehavior) {
  gcomp_registry_t * reg1 = gcomp_registry_default();
  gcomp_registry_t * reg2 = gcomp_registry_default();
  EXPECT_EQ(reg1, reg2); // Should return same pointer
}

TEST(RegistryDefaultTest, CannotDestroy) {
  gcomp_registry_t * reg = gcomp_registry_default();
  // Attempting to destroy default registry should not crash
  // (implementation may ignore or handle gracefully)
  gcomp_registry_destroy(reg);
  // Verify it still works after attempted destroy
  gcomp_registry_t * reg2 = gcomp_registry_default();
  EXPECT_NE(reg2, nullptr);
}

// Test gcomp_registry_create()
TEST_F(RegistryTest, CreateSuccess) {
  gcomp_registry_t * reg = nullptr;
  gcomp_status_t status = gcomp_registry_create(nullptr, &reg);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(reg, nullptr);
  gcomp_registry_destroy(reg);
}

TEST_F(RegistryTest, CreateNullPointer) {
  gcomp_status_t status = gcomp_registry_create(nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_registry_destroy()
TEST_F(RegistryTest, DestroyNullPointer) {
  // Should not crash
  gcomp_registry_destroy(nullptr);
}

TEST_F(RegistryTest, DestroyCleanup) {
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_NE(reg, nullptr);

  // Register a method
  gcomp_method_t method = create_mock_method("test", GCOMP_CAP_ENCODE);
  ASSERT_EQ(gcomp_registry_register(reg, &method), GCOMP_OK);

  // Destroy should clean up
  gcomp_registry_destroy(reg);
  // If we get here without crashing, cleanup worked
}

// Test gcomp_registry_register()
TEST_F(RegistryTest, RegisterMethod) {
  gcomp_method_t method = create_mock_method("test_method", GCOMP_CAP_ENCODE);

  EXPECT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  // Verify it can be found
  const gcomp_method_t * found = gcomp_registry_find(registry_, "test_method");
  EXPECT_NE(found, nullptr);
  EXPECT_STREQ(found->name, "test_method");
  EXPECT_EQ(found->capabilities, GCOMP_CAP_ENCODE);
}

TEST_F(RegistryTest, RegisterNullPointer) {
  gcomp_method_t method = create_mock_method("test", GCOMP_CAP_ENCODE);

  EXPECT_EQ(gcomp_registry_register(nullptr, &method), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_registry_register(registry_, nullptr), GCOMP_ERR_INVALID_ARG);
}

TEST_F(RegistryTest, RegisterInvalidMethod) {
  // Method with NULL name
  gcomp_method_t method = create_mock_method("test", GCOMP_CAP_ENCODE);
  method.name = nullptr;

  EXPECT_NE(gcomp_registry_register(registry_, &method), GCOMP_OK);
}

TEST_F(RegistryTest, RegisterDuplicate) {
  gcomp_method_t method1 = create_mock_method("test", GCOMP_CAP_ENCODE);
  gcomp_method_t method2 = create_mock_method("test", GCOMP_CAP_DECODE);

  EXPECT_EQ(gcomp_registry_register(registry_, &method1), GCOMP_OK);

  // Registering again should either be idempotent or error appropriately
  // For now, test that it doesn't crash
  gcomp_status_t status = gcomp_registry_register(registry_, &method2);
  // Implementation may overwrite or return error - both are acceptable
  EXPECT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_INVALID_ARG);
}

TEST_F(RegistryTest, RegisterEmptyName) {
  gcomp_method_t method = create_mock_method("", GCOMP_CAP_ENCODE);

  // Empty name might be invalid
  gcomp_status_t status = gcomp_registry_register(registry_, &method);
  // Implementation may accept or reject - test doesn't crash
  (void)status;
}

TEST_F(RegistryTest, RegisterLongName) {
  std::string long_name(1000, 'a');
  gcomp_method_t method =
      create_mock_method(long_name.c_str(), GCOMP_CAP_ENCODE);

  EXPECT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  const gcomp_method_t * found =
      gcomp_registry_find(registry_, long_name.c_str());
  EXPECT_NE(found, nullptr);
  EXPECT_STREQ(found->name, long_name.c_str());
}

// Test gcomp_registry_find()
TEST_F(RegistryTest, FindRegisteredMethod) {
  gcomp_method_t method = create_mock_method("test_method", GCOMP_CAP_ENCODE);
  ASSERT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  const gcomp_method_t * found = gcomp_registry_find(registry_, "test_method");
  EXPECT_NE(found, nullptr);
  EXPECT_STREQ(found->name, "test_method");
  EXPECT_EQ(found->capabilities, GCOMP_CAP_ENCODE);
}

TEST_F(RegistryTest, FindNonExistent) {
  const gcomp_method_t * found = gcomp_registry_find(registry_, "nonexistent");
  EXPECT_EQ(found, nullptr);
}

TEST_F(RegistryTest, FindNullPointer) {
  EXPECT_EQ(gcomp_registry_find(nullptr, "test"), nullptr);
  EXPECT_EQ(gcomp_registry_find(registry_, nullptr), nullptr);
}

TEST_F(RegistryTest, FindCaseSensitive) {
  gcomp_method_t method = create_mock_method("TestMethod", GCOMP_CAP_ENCODE);
  ASSERT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  // Should be case-sensitive
  const gcomp_method_t * found1 = gcomp_registry_find(registry_, "TestMethod");
  EXPECT_NE(found1, nullptr);

  const gcomp_method_t * found2 = gcomp_registry_find(registry_, "testmethod");
  EXPECT_EQ(found2, nullptr); // Different case
}

// Test multiple methods
TEST_F(RegistryTest, MultipleMethods) {
  gcomp_method_t method1 = create_mock_method("method1", GCOMP_CAP_ENCODE);
  gcomp_method_t method2 = create_mock_method("method2", GCOMP_CAP_DECODE);
  gcomp_method_t method3 = create_mock_method("method3",
      static_cast<gcomp_capabilities_t>(GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE));

  EXPECT_EQ(gcomp_registry_register(registry_, &method1), GCOMP_OK);
  EXPECT_EQ(gcomp_registry_register(registry_, &method2), GCOMP_OK);
  EXPECT_EQ(gcomp_registry_register(registry_, &method3), GCOMP_OK);

  const gcomp_method_t * found1 = gcomp_registry_find(registry_, "method1");
  EXPECT_NE(found1, nullptr);
  EXPECT_STREQ(found1->name, "method1");
  EXPECT_EQ(found1->capabilities, GCOMP_CAP_ENCODE);

  const gcomp_method_t * found2 = gcomp_registry_find(registry_, "method2");
  EXPECT_NE(found2, nullptr);
  EXPECT_STREQ(found2->name, "method2");
  EXPECT_EQ(found2->capabilities, GCOMP_CAP_DECODE);

  const gcomp_method_t * found3 = gcomp_registry_find(registry_, "method3");
  EXPECT_NE(found3, nullptr);
  EXPECT_STREQ(found3->name, "method3");
  EXPECT_EQ(found3->capabilities,
      static_cast<gcomp_capabilities_t>(GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE));
}

// Test memory cleanup
TEST_F(RegistryTest, MemoryCleanupManyMethods) {
  // Register many methods
  for (int i = 0; i < 100; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "method_%d", i);
    gcomp_method_t method = create_mock_method(name, GCOMP_CAP_ENCODE);
    EXPECT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);
  }

  // Verify all can be found
  for (int i = 0; i < 100; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "method_%d", i);
    const gcomp_method_t * found = gcomp_registry_find(registry_, name);
    EXPECT_NE(found, nullptr);
  }

  // Destroy should clean up all memory
  gcomp_registry_destroy(registry_);
  registry_ = nullptr;
  // If we get here without crashing, cleanup worked
}

// Test default registry with methods
TEST(RegistryDefaultTest, RegisterAndFind) {
  gcomp_registry_t * reg = gcomp_registry_default();

  gcomp_method_t method = create_mock_method("default_test", GCOMP_CAP_ENCODE);
  EXPECT_EQ(gcomp_registry_register(reg, &method), GCOMP_OK);

  const gcomp_method_t * found = gcomp_registry_find(reg, "default_test");
  EXPECT_NE(found, nullptr);
  EXPECT_STREQ(found->name, "default_test");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
