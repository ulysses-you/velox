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

#include <string>

#include "velox/expression/FunctionSignature.h"
#include "velox/functions/lib/SimpleNumericAggregate.h"
#include "velox/functions/prestosql/aggregates/SingleValueAccumulator.h"

using namespace facebook::velox::aggregate;

namespace facebook::velox::functions::sparksql::aggregates {

namespace {

/// FirstLastAggregate returns the first or last value of |expr| for a group of
/// rows. If |ignoreNull| is true, returns only non-null values.
///
/// The function is non-deterministic because its results depends on the order
/// of the rows which may be non-deterministic after a shuffle.
template <bool numeric, typename TData>
class FirstLastAggregateBase
    : public SimpleNumericAggregate<TData, TData, TData> {
  using BaseAggregate = SimpleNumericAggregate<TData, TData, TData>;

 protected:
  using TAccumulator = std::conditional_t<
      numeric,
      std::optional<TData>,
      std::optional<SingleValueAccumulator>>;

 public:
  explicit FirstLastAggregateBase(TypePtr resultType)
      : BaseAggregate(std::move(resultType)) {}

  int32_t accumulatorFixedWidthSize() const override {
    return sizeof(TAccumulator);
  }

  int32_t accumulatorAlignmentSize() const override {
    return 1;
  }

  void initializeNewGroups(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    exec::Aggregate::setAllNulls(groups, indices);

    for (auto i : indices) {
      new (groups[i] + exec::Aggregate::offset_) TAccumulator();
    }
  }

  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    if constexpr (numeric) {
      BaseAggregate::doExtractValues(
          groups, numGroups, result, [&](char* group) {
            auto accumulator = exec::Aggregate::value<TAccumulator>(group);
            return accumulator->value();
          });
    } else {
      VELOX_CHECK(result);
      (*result)->resize(numGroups);

      auto* rawNulls = exec::Aggregate::getRawNulls(result->get());

      for (auto i = 0; i < numGroups; ++i) {
        char* group = groups[i];
        if (exec::Aggregate::isNull(group)) {
          (*result)->setNull(i, true);
        } else {
          exec::Aggregate::clearNull(rawNulls, i);
          auto accumulator = exec::Aggregate::value<TAccumulator>(group);
          accumulator->value().read(*result, i);
        }
      }
    }
  }

  void extractAccumulators(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    auto rowVector = (*result)->as<RowVector>();
    VELOX_CHECK_EQ(
        rowVector->childrenSize(),
        2,
        "intermediate results must have 2 children");

    auto ignoreNullVector = rowVector->childAt(1)->asFlatVector<bool>();
    rowVector->resize(numGroups);
    ignoreNullVector->resize(numGroups);

    extractValues(groups, numGroups, &(rowVector->childAt(0)));
  }

  void destroy(folly::Range<char**> groups) override {
    if constexpr (!numeric) {
      for (auto group : groups) {
        auto accumulator = exec::Aggregate::value<TAccumulator>(group);
        if (accumulator->has_value()) {
          accumulator->value().destroy(exec::Aggregate::allocator_);
        }
      }
    }
  }
};

template <>
inline int32_t
FirstLastAggregateBase<true, UnscaledLongDecimal>::accumulatorAlignmentSize()
    const {
  return static_cast<int32_t>(sizeof(UnscaledLongDecimal));
}

template <bool ignoreNull, typename TData, bool numeric>
class FirstAggregate : public FirstLastAggregateBase<numeric, TData> {
 public:
  explicit FirstAggregate(TypePtr resultType)
      : FirstLastAggregateBase<numeric, TData>(std::move(resultType)) {}

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);

    rows.applyToSelected([&](vector_size_t i) {
      updateValue(decoded.index(i), groups[i], decoded.base());
    });
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);
    auto rowVector = dynamic_cast<const RowVector*>(decoded.base());
    VELOX_CHECK_NOT_NULL(rowVector);
    VELOX_CHECK_EQ(
        rowVector->childrenSize(),
        2,
        "intermediate results must have 2 children");

    auto valueVector = rowVector->childAt(0);

    rows.applyToSelected([&](vector_size_t i) {
      updateValue(decoded.index(i), groups[i], valueVector.get());
    });
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);

    rows.testSelected([&](vector_size_t i) {
      return updateValue(decoded.index(i), group, decoded.base());
    });
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);
    auto rowVector = dynamic_cast<const RowVector*>(decoded.base());
    VELOX_CHECK_NOT_NULL(rowVector);
    VELOX_CHECK_EQ(
        rowVector->childrenSize(),
        2,
        "intermediate results must have 2 children");

    auto valueVector = rowVector->childAt(0);
    rows.testSelected([&](vector_size_t i) {
      return updateValue(decoded.index(i), group, valueVector.get());
    });
  }

 private:
  using TAccumulator =
      typename FirstLastAggregateBase<numeric, TData>::TAccumulator;

  // If we found a valid value, set to accumulator, then skip remaining rows in
  // group.
  bool updateValue(vector_size_t i, char* group, const BaseVector* vector) {
    auto accumulator = exec::Aggregate::value<TAccumulator>(group);
    if (accumulator->has_value()) {
      return false;
    }

    if constexpr (!numeric) {
      return updateNonNumeric(i, group, vector);
    } else {
      if (!vector->isNullAt(i)) {
        exec::Aggregate::clearNull(group);
        auto value = vector->as<SimpleVector<TData>>()->valueAt(i);
        *accumulator = value;
        return false;
      }

      if constexpr (ignoreNull) {
        return true;
      } else {
        *accumulator = TData();
        return false;
      }
    }
  }

  bool
  updateNonNumeric(vector_size_t i, char* group, const BaseVector* vector) {
    auto accumulator = exec::Aggregate::value<TAccumulator>(group);

    if (!vector->isNullAt(i)) {
      exec::Aggregate::clearNull(group);
      *accumulator = SingleValueAccumulator();
      accumulator->value().write(vector, i, exec::Aggregate::allocator_);
      return false;
    }

    if constexpr (ignoreNull) {
      return true;
    } else {
      *accumulator = SingleValueAccumulator();
      return false;
    }
  }
};

template <bool ignoreNull, typename TData, bool numeric>
class LastAggregate : public FirstLastAggregateBase<numeric, TData> {
 public:
  explicit LastAggregate(TypePtr resultType)
      : FirstLastAggregateBase<numeric, TData>(std::move(resultType)) {}

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);

    rows.applyToSelected([&](vector_size_t i) {
      updateValue(decoded.index(i), groups[i], decoded.base());
    });
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);
    auto rowVector = dynamic_cast<const RowVector*>(decoded.base());
    VELOX_CHECK_NOT_NULL(rowVector);
    VELOX_CHECK_EQ(
        rowVector->childrenSize(),
        2,
        "intermediate results must have 2 children");

    auto valueVector = rowVector->childAt(0);

    rows.applyToSelected([&](vector_size_t i) {
      updateValue(decoded.index(i), groups[i], valueVector.get());
    });
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);

    rows.applyToSelected([&](vector_size_t i) {
      updateValue(decoded.index(i), group, decoded.base());
    });
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /* mayPushdown */) override {
    DecodedVector decoded(*args[0], rows);
    auto rowVector = dynamic_cast<const RowVector*>(decoded.base());
    VELOX_CHECK_NOT_NULL(rowVector);
    VELOX_CHECK_EQ(
        rowVector->childrenSize(),
        2,
        "intermediate results must have 2 children");

    auto valueVector = rowVector->childAt(0);
    rows.applyToSelected([&](vector_size_t i) {
      updateValue(decoded.index(i), group, valueVector.get());
    });
  }

 private:
  using TAccumulator =
      typename FirstLastAggregateBase<numeric, TData>::TAccumulator;

  void updateValue(vector_size_t i, char* group, const BaseVector* vector) {
    if constexpr (!numeric) {
      return updateNonNumeric(i, group, vector);
    } else {
      auto accumulator = exec::Aggregate::value<TAccumulator>(group);

      if (!vector->isNullAt(i)) {
        exec::Aggregate::clearNull(group);
        *accumulator = vector->as<SimpleVector<TData>>()->valueAt(i);
        return;
      }

      if constexpr (!ignoreNull) {
        exec::Aggregate::setNull(group);
        *accumulator = TData();
      }
    }
  }

  void
  updateNonNumeric(vector_size_t i, char* group, const BaseVector* vector) {
    auto accumulator = exec::Aggregate::value<TAccumulator>(group);

    if (!vector->isNullAt(i)) {
      exec::Aggregate::clearNull(group);
      *accumulator = SingleValueAccumulator();
      accumulator->value().write(vector, i, exec::Aggregate::allocator_);
      return;
    }

    if constexpr (!ignoreNull) {
      exec::Aggregate::setNull(group);
      *accumulator = SingleValueAccumulator();
    }
  }
};

} // namespace

template <template <bool B1, typename T, bool B2> class TClass, bool ignoreNull>
bool registerFirstLast(const std::string& name) {
  std::vector<std::shared_ptr<exec::AggregateFunctionSignature>> signatures = {
      exec::AggregateFunctionSignatureBuilder()
          .typeVariable("T")
          .argumentType("T")
          .intermediateType("row(T, boolean)")
          .returnType("T")
          .build()};

  signatures.push_back(
      exec::AggregateFunctionSignatureBuilder()
          .integerVariable("a_precision")
          .integerVariable("a_scale")
          .argumentType("DECIMAL(a_precision, a_scale)")
          .intermediateType("row(DECIMAL(a_precision, a_scale), boolean)")
          .returnType("DECIMAL(a_precision, a_scale)")
          .build());

  return exec::registerAggregateFunction(
      name,
      std::move(signatures),
      [name](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType) -> std::unique_ptr<exec::Aggregate> {
        VELOX_CHECK_EQ(argTypes.size(), 1, "{} takes only 1 arguments", name);
        const auto& inputType = argTypes[0];
        TypeKind dataKind = exec::isRawInput(step)
            ? inputType->kind()
            : inputType->childAt(0)->kind();
        switch (dataKind) {
          case TypeKind::BOOLEAN:
            return std::make_unique<TClass<ignoreNull, bool, true>>(resultType);
          case TypeKind::TINYINT:
            return std::make_unique<TClass<ignoreNull, int8_t, true>>(
                resultType);
          case TypeKind::SMALLINT:
            return std::make_unique<TClass<ignoreNull, int16_t, true>>(
                resultType);
          case TypeKind::INTEGER:
            return std::make_unique<TClass<ignoreNull, int32_t, true>>(
                resultType);
          case TypeKind::BIGINT:
            return std::make_unique<TClass<ignoreNull, int64_t, true>>(
                resultType);
          case TypeKind::REAL:
            return std::make_unique<TClass<ignoreNull, float, true>>(
                resultType);
          case TypeKind::DOUBLE:
            return std::make_unique<TClass<ignoreNull, double, true>>(
                resultType);
          case TypeKind::TIMESTAMP:
            return std::make_unique<TClass<ignoreNull, Timestamp, true>>(
                resultType);
          case TypeKind::DATE:
            return std::make_unique<TClass<ignoreNull, Date, true>>(resultType);
          case TypeKind::SHORT_DECIMAL:
            return std::make_unique<
                TClass<ignoreNull, UnscaledShortDecimal, true>>(resultType);
          case TypeKind::LONG_DECIMAL:
            return std::make_unique<
                TClass<ignoreNull, UnscaledLongDecimal, true>>(resultType);
          case TypeKind::VARCHAR:
          case TypeKind::ARRAY:
          case TypeKind::MAP:
            return std::make_unique<TClass<ignoreNull, ComplexType, false>>(
                resultType);
          default:
            VELOX_FAIL(
                "Unknown input type for {} aggregation {}",
                name,
                inputType->toString());
        }
      },
      true);
}

void registerFirstLastAggregates(const std::string& prefix) {
  registerFirstLast<FirstAggregate, false>(prefix + "first");
  registerFirstLast<FirstAggregate, true>(prefix + "first_ignore_null");
  registerFirstLast<LastAggregate, false>(prefix + "last");
  registerFirstLast<LastAggregate, true>(prefix + "last_ignore_null");
}

} // namespace facebook::velox::functions::sparksql::aggregates
