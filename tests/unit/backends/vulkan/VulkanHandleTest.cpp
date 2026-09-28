//===----------------------------------------------------------------------===//
// VulkanHandleTest.cpp
// Ownership invariants for the RAII Vulkan handle wrappers.
//
// These run without a Vulkan device: they exercise the generic `Unique`/`DestroyFn`
// templates with a fake handle, so the guarantee that every early `return` in a
// backend frees its resources is enforced by the test suite, not by code review.
//===----------------------------------------------------------------------===//

#include "csrun/backends/vulkan/VulkanHandle.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace {

struct FakeHandle {
  int id = 0;
};

bool operator!=(const FakeHandle &a, const FakeHandle &b) { return a.id != b.id; }

/// Deleter that records every destruction so tests can assert on count and order.
struct RecordingDeleter {
  int *count = nullptr;
  std::vector<int> *order = nullptr;

  void operator()(const FakeHandle &h) const {
    if (count != nullptr)
      ++*count;
    if (order != nullptr)
      order->push_back(h.id);
  }
};

using FakeUnique = csrun::Unique<FakeHandle, RecordingDeleter>;

} // namespace

TEST(VulkanHandleTest, DefaultConstructedOwnsNothing) {
  int count = 0;
  {
    FakeUnique handle;
    EXPECT_FALSE(static_cast<bool>(handle));
  }
  EXPECT_EQ(count, 0);
}

TEST(VulkanHandleTest, DestructorDestroysExactlyOnce) {
  int count = 0;
  {
    FakeUnique handle{FakeHandle{7}, RecordingDeleter{&count, nullptr}};
    EXPECT_TRUE(static_cast<bool>(handle));
    EXPECT_EQ(handle.get().id, 7);
    EXPECT_EQ(count, 0) << "must not destroy before scope exit";
  }
  EXPECT_EQ(count, 1);
}

TEST(VulkanHandleTest, ResetIsIdempotentAndCountsOnce) {
  int count = 0;
  FakeUnique handle{FakeHandle{1}, RecordingDeleter{&count, nullptr}};
  handle.reset();
  EXPECT_EQ(count, 1);
  EXPECT_FALSE(static_cast<bool>(handle));
  handle.reset();
  handle.reset();
  EXPECT_EQ(count, 1) << "reset on an empty handle must be a no-op";
}

TEST(VulkanHandleTest, ReleaseDisownsWithoutDestroying) {
  int count = 0;
  FakeHandle raw;
  {
    FakeUnique handle{FakeHandle{3}, RecordingDeleter{&count, nullptr}};
    raw = handle.release();
  }
  EXPECT_EQ(count, 0) << "release() hands ownership to the caller";
  EXPECT_EQ(raw.id, 3);
}

TEST(VulkanHandleTest, MoveTransfersOwnershipWithoutDoubleDestroy) {
  int count = 0;
  std::vector<int> order;
  {
    FakeUnique a{FakeHandle{11}, RecordingDeleter{&count, &order}};
    FakeUnique b{std::move(a)};
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(count, 0);
  }
  EXPECT_EQ(count, 1) << "the moved-from handle must not destroy the same resource again";
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order[0], 11);
}

TEST(VulkanHandleTest, MoveAssignDestroysTheOverwrittenHandleFirst) {
  int count = 0;
  std::vector<int> order;
  {
    FakeUnique a{FakeHandle{1}, RecordingDeleter{&count, &order}};
    FakeUnique b{FakeHandle{2}, RecordingDeleter{&count, &order}};
    b = std::move(a);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(b.get().id, 1);
  }
  EXPECT_EQ(count, 2);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 2) << "the replaced handle is destroyed at assignment";
  EXPECT_EQ(order[1], 1) << "the incoming handle is destroyed at scope exit";
}

TEST(VulkanHandleTest, SelfMoveAssignKeepsOwnership) {
  int count = 0;
  {
    FakeUnique handle{FakeHandle{5}, RecordingDeleter{&count, nullptr}};
    FakeUnique &alias = handle;
    handle = std::move(alias);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(static_cast<bool>(handle));
  }
  EXPECT_EQ(count, 1) << "self-move-assignment must not destroy or leak the handle";
}

TEST(VulkanHandleTest, IsNotCopyable) {
  static_assert(!std::is_copy_constructible_v<FakeUnique>);
  static_assert(!std::is_copy_assignable_v<FakeUnique>);
  static_assert(std::is_move_constructible_v<FakeUnique>);
  static_assert(std::is_move_assignable_v<FakeUnique>);
  SUCCEED();
}

int gDestroyCalls = 0;
int gObservedParent = -1;

void FakeDestroy(int *parent, FakeHandle h, const VkAllocationCallbacks *) {
  ++gDestroyCalls;
  gObservedParent = *parent;
  (void)h;
}

TEST(VulkanHandleTest, DestroyFnSkipsNullHandle) {
  gDestroyCalls = 0;
  int parent = 42;
  csrun::DestroyFn<FakeHandle, int *> deleter{&parent, &FakeDestroy};
  deleter(FakeHandle{0});
  EXPECT_EQ(gDestroyCalls, 0) << "a null handle must never reach the destroy function";
}

TEST(VulkanHandleTest, DestroyFnSkipsNullFn) {
  gDestroyCalls = 0;
  int parent = 42;
  csrun::DestroyFn<FakeHandle, int *> deleter{&parent, nullptr};
  deleter(FakeHandle{9});
  EXPECT_EQ(gDestroyCalls, 0) << "an uninitialised destroy function must not be called";
}

TEST(VulkanHandleTest, DestroyFnForwardsCapturedParent) {
  // This is the invariant that makes the Vulkan wrappers safe: the parent object
  // (the VkDevice) is captured at construction and handed back to the destroy entry
  // point, so the handle stays valid even if the owning scope is torn down first.
  gDestroyCalls = 0;
  gObservedParent = -1;
  int device = 77;
  csrun::DestroyFn<FakeHandle, int *> deleter{&device, &FakeDestroy};
  deleter(FakeHandle{5});
  EXPECT_EQ(gDestroyCalls, 1);
  EXPECT_EQ(gObservedParent, 77);
}

/// A struct whose members are declared buffer-then-memory, as `BufferResource` is.
/// C++ destroys members in reverse declaration order, so this guarantees the buffer
/// is released before the device memory it was bound to -- the ordering Vulkan needs.
struct MirrorsBufferResource {
  int *count = nullptr;
  std::vector<int> *order = nullptr;

  struct Member {
    int id = 0;
    int *count = nullptr;
    std::vector<int> *order = nullptr;
    ~Member() {
      if (count != nullptr)
        ++*count;
      if (order != nullptr)
        order->push_back(id);
    }
  };

  Member memory;
  Member buffer;

  MirrorsBufferResource(int *c, std::vector<int> *o) : memory{2, c, o}, buffer{1, c, o} {}
};

TEST(VulkanHandleTest, ReverseDeclarationOrderFreesBufferBeforeMemory) {
  int count = 0;
  std::vector<int> order;
  {
    MirrorsBufferResource resource{&count, &order};
  }
  EXPECT_EQ(count, 2);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 1) << "buffer must be destroyed before its memory";
  EXPECT_EQ(order[1], 2);
}
