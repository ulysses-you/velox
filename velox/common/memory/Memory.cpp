/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/common/memory/Memory.h"

DECLARE_int32(velox_memory_num_shared_leaf_pools);

namespace facebook::velox::memory {
namespace {
static constexpr std::string_view kDefaultRootName{"__default_root__"};
static constexpr std::string_view kDefaultLeafName("__default_leaf__");

std::mutex& instanceMutex() {
  static std::mutex kMutex;
  return kMutex;
}

// Must be called while holding a lock over instanceMutex().
std::unique_ptr<MemoryManager>& instance() {
  static std::unique_ptr<MemoryManager> kInstance;
  return kInstance;
}
} // namespace

MemoryManager::MemoryManager(const MemoryManagerOptions& options)
    : capacity_{options.capacity},
      allocator_{options.allocator->shared_from_this()},
      poolInitCapacity_(options.memoryPoolInitCapacity),
      // TODO: consider to reserve a small amount of memory to compensate for
      //  the unreclaimable cache memory which are pinned by query accesses if
      //  enabled.
      arbitrator_(MemoryArbitrator::create(
          {.kind = options.arbitratorKind,
           .capacity = std::min(options.queryMemoryCapacity, options.capacity),
           .memoryPoolTransferCapacity = options.memoryPoolTransferCapacity,
           .memoryReclaimWaitMs = options.memoryReclaimWaitMs,
           .arbitrationStateCheckCb = options.arbitrationStateCheckCb})),
      alignment_(std::max(MemoryAllocator::kMinAlignment, options.alignment)),
      checkUsageLeak_(options.checkUsageLeak),
      debugEnabled_(options.debugEnabled),
      coreOnAllocationFailureEnabled_(options.coreOnAllocationFailureEnabled),
      poolDestructionCb_([&](MemoryPool* pool) { dropPool(pool); }),
      poolGrowCb_([&](MemoryPool* pool, uint64_t targetBytes) {
        return growPool(pool, targetBytes);
      }),
      defaultRoot_{std::make_shared<MemoryPoolImpl>(
          this,
          std::string(kDefaultRootName),
          MemoryPool::Kind::kAggregate,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          // NOTE: the default root memory pool has no capacity limit, and it is
          // used for system usage in production such as disk spilling.
          MemoryPool::Options{
              .alignment = alignment_,
              .maxCapacity = kMaxMemory,
              .trackUsage = options.trackDefaultUsage,
              .debugEnabled = options.debugEnabled,
              .coreOnAllocationFailureEnabled =
                  options.coreOnAllocationFailureEnabled})} {
  VELOX_CHECK_NOT_NULL(allocator_);
  VELOX_CHECK_NOT_NULL(arbitrator_);
  VELOX_CHECK_EQ(
      allocator_->capacity(),
      capacity_,
      "MemoryAllocator capacity {} must be the same as MemoryManager capacity {}.",
      allocator_->capacity(),
      capacity_);
  VELOX_USER_CHECK_GE(capacity_, 0);
  MemoryAllocator::alignmentCheck(0, alignment_);
  defaultRoot_->grow(defaultRoot_->maxCapacity());
  const size_t numSharedPools =
      std::max(1, FLAGS_velox_memory_num_shared_leaf_pools);
  sharedLeafPools_.reserve(numSharedPools);
  for (size_t i = 0; i < numSharedPools; ++i) {
    sharedLeafPools_.emplace_back(
        addLeafPool(fmt::format("default_shared_leaf_pool_{}", i)));
  }
}

MemoryManager::~MemoryManager() {
  if (checkUsageLeak_) {
    VELOX_CHECK_EQ(
        pools_.size(),
        0,
        "There are unexpected alive memory pools allocated by user on memory manager destruction:\n{}",
        toString(true));
  }
}

// static
MemoryManager& MemoryManager::deprecatedGetInstance(
    const MemoryManagerOptions& options) {
  std::lock_guard<std::mutex> l(instanceMutex());
  auto& instanceRef = instance();
  if (instanceRef == nullptr) {
    instanceRef = std::make_unique<MemoryManager>(options);
  }
  return *instanceRef;
}

// static
void MemoryManager::initialize(const MemoryManagerOptions& options) {
  std::lock_guard<std::mutex> l(instanceMutex());
  auto& instanceRef = instance();
  VELOX_CHECK_NULL(
      instanceRef,
      "The memory manager has already been set: {}",
      instanceRef->toString());
  instanceRef = std::make_unique<MemoryManager>(options);
}

// static.
MemoryManager* MemoryManager::getInstance() {
  std::lock_guard<std::mutex> l(instanceMutex());
  auto& instanceRef = instance();
  VELOX_CHECK_NOT_NULL(instanceRef, "The memory manager is not set");
  return instanceRef.get();
}

// static.
MemoryManager& MemoryManager::testingSetInstance(
    const MemoryManagerOptions& options) {
  std::lock_guard<std::mutex> l(instanceMutex());
  auto& instanceRef = instance();
  instanceRef = std::make_unique<MemoryManager>(options);
  return *instanceRef;
}

int64_t MemoryManager::capacity() const {
  return capacity_;
}

uint16_t MemoryManager::alignment() const {
  return alignment_;
}

std::shared_ptr<MemoryPool> MemoryManager::addRootPool(
    const std::string& name,
    int64_t capacity,
    std::unique_ptr<MemoryReclaimer> reclaimer) {
  std::string poolName = name;
  if (poolName.empty()) {
    static std::atomic<int64_t> poolId{0};
    poolName = fmt::format("default_root_{}", poolId++);
  }

  MemoryPool::Options options;
  options.alignment = alignment_;
  options.maxCapacity = capacity;
  options.trackUsage = true;
  options.debugEnabled = debugEnabled_;
  options.coreOnAllocationFailureEnabled = coreOnAllocationFailureEnabled_;

  folly::SharedMutex::WriteHolder guard{mutex_};
  if (pools_.find(poolName) != pools_.end()) {
    VELOX_FAIL("Duplicate root pool name found: {}", poolName);
  }
  auto pool = std::make_shared<MemoryPoolImpl>(
      this,
      poolName,
      MemoryPool::Kind::kAggregate,
      nullptr,
      std::move(reclaimer),
      poolGrowCb_,
      poolDestructionCb_,
      options);
  pools_.emplace(poolName, pool);
  VELOX_CHECK_EQ(pool->capacity(), 0);
  arbitrator_->growCapacity(
      pool.get(), std::min<uint64_t>(poolInitCapacity_, capacity));
  return pool;
}

std::shared_ptr<MemoryPool> MemoryManager::addLeafPool(
    const std::string& name,
    bool threadSafe) {
  std::string poolName = name;
  if (poolName.empty()) {
    static std::atomic<int64_t> poolId{0};
    poolName = fmt::format("default_leaf_{}", poolId++);
  }
  return defaultRoot_->addLeafChild(poolName, threadSafe, nullptr);
}

bool MemoryManager::growPool(MemoryPool* pool, uint64_t incrementBytes) {
  VELOX_CHECK_NOT_NULL(pool);
  VELOX_CHECK_NE(pool->capacity(), kMaxMemory);
  return arbitrator_->growCapacity(pool, getAlivePools(), incrementBytes);
}

uint64_t MemoryManager::shrinkPools(uint64_t targetBytes) {
  return arbitrator_->shrinkCapacity(getAlivePools(), targetBytes);
}

void MemoryManager::dropPool(MemoryPool* pool) {
  VELOX_CHECK_NOT_NULL(pool);
  folly::SharedMutex::WriteHolder guard{mutex_};
  auto it = pools_.find(pool->name());
  if (it == pools_.end()) {
    VELOX_FAIL("The dropped memory pool {} not found", pool->name());
  }
  pools_.erase(it);
  VELOX_DCHECK_EQ(pool->currentBytes(), 0);
  arbitrator_->shrinkCapacity(pool, 0);
}

MemoryPool& MemoryManager::deprecatedSharedLeafPool() {
  const auto idx = std::hash<std::thread::id>{}(std::this_thread::get_id());
  folly::SharedMutex::ReadHolder guard{mutex_};
  return *sharedLeafPools_.at(idx % sharedLeafPools_.size());
}

int64_t MemoryManager::getTotalBytes() const {
  return allocator_->totalUsedBytes();
}

size_t MemoryManager::numPools() const {
  size_t numPools = defaultRoot_->getChildCount();
  VELOX_CHECK_GE(numPools, 0);
  {
    folly::SharedMutex::ReadHolder guard{mutex_};
    numPools += pools_.size() - sharedLeafPools_.size();
  }
  return numPools;
}

MemoryAllocator& MemoryManager::allocator() {
  return *allocator_;
}

MemoryArbitrator* MemoryManager::arbitrator() {
  return arbitrator_.get();
}

std::string MemoryManager::toString(bool detail) const {
  std::stringstream out;
  out << "Memory Manager[capacity "
      << (capacity_ == kMaxMemory ? "UNLIMITED" : succinctBytes(capacity_))
      << " alignment " << succinctBytes(alignment_) << " usedBytes "
      << succinctBytes(getTotalBytes()) << " number of pools " << numPools()
      << "\n";
  out << "List of root pools:\n";
  if (detail) {
    out << defaultRoot_->treeMemoryUsage();
  } else {
    out << "\t" << defaultRoot_->name() << "\n";
  }
  std::vector<std::shared_ptr<MemoryPool>> pools = getAlivePools();
  for (const auto& pool : pools) {
    if (detail) {
      out << pool->treeMemoryUsage();
    } else {
      out << "\t" << pool->name() << "\n";
    }
  }
  out << allocator_->toString() << "\n";
  out << arbitrator_->toString();
  out << "]";
  return out.str();
}

std::vector<std::shared_ptr<MemoryPool>> MemoryManager::getAlivePools() const {
  std::vector<std::shared_ptr<MemoryPool>> pools;
  folly::SharedMutex::ReadHolder guard{mutex_};
  pools.reserve(pools_.size());
  for (const auto& entry : pools_) {
    auto pool = entry.second.lock();
    if (pool != nullptr) {
      pools.push_back(std::move(pool));
    }
  }
  return pools;
}

void initializeMemoryManager(const MemoryManagerOptions& options) {
  MemoryManager::initialize(options);
}

MemoryManager* memoryManager() {
  return MemoryManager::getInstance();
}

MemoryManager& deprecatedDefaultMemoryManager() {
  return MemoryManager::deprecatedGetInstance();
}

std::shared_ptr<MemoryPool> deprecatedAddDefaultLeafMemoryPool(
    const std::string& name,
    bool threadSafe) {
  auto& memoryManager = deprecatedDefaultMemoryManager();
  return memoryManager.addLeafPool(name, threadSafe);
}

MemoryPool& deprecatedSharedLeafPool() {
  return deprecatedDefaultMemoryManager().deprecatedSharedLeafPool();
}

memory::MemoryPool* spillMemoryPool() {
  static auto pool =
      memory::MemoryManager::getInstance()->addLeafPool("_sys.spilling");
  return pool.get();
}

bool isSpillMemoryPool(memory::MemoryPool* pool) {
  return pool == spillMemoryPool();
}
} // namespace facebook::velox::memory
