// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/array/builder_dict.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/status.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/hashing.h"
#include "arrow/util/logging.h"
#include "arrow/util/string_view.h"
#include "arrow/visitor_inline.h"

namespace arrow {
namespace internal {

template <typename T, typename Enable = void>
struct DictionaryTraits {
  using MemoTableType = void;
};

}  // namespace internal

template <typename T, typename Out = void>
using enable_if_memoize = enable_if_t<
    !std::is_same<typename internal::DictionaryTraits<T>::MemoTableType, void>::value,
    Out>;

template <typename T, typename Out = void>
using enable_if_no_memoize = enable_if_t<
    std::is_same<typename internal::DictionaryTraits<T>::MemoTableType, void>::value,
    Out>;

namespace internal {

template <>
struct DictionaryTraits<BooleanType> {
  using T = BooleanType;
  using MemoTableType = typename HashTraits<T>::MemoTableType;

  static Status GetDictionaryArrayData(MemoryPool* pool,
                                       const std::shared_ptr<DataType>& type,
                                       const MemoTableType& memo_table,
                                       int64_t start_offset,
                                       std::shared_ptr<ArrayData>* out) {
    if (start_offset < 0) {
      return Status::Invalid("invalid start_offset ", start_offset);
    }

    BooleanBuilder builder(pool);
    const auto& bool_values = memo_table.values();
    const auto null_index = memo_table.GetNull();

    // Will iterate up to 3 times.
    for (int64_t i = start_offset; i < memo_table.size(); i++) {
      RETURN_NOT_OK(i == null_index ? builder.AppendNull()
                                    : builder.Append(bool_values[i]));
    }

    return builder.FinishInternal(out);
  }
};  // namespace internal

template <typename T>
struct DictionaryTraits<T, enable_if_has_c_type<T>> {
  using c_type = typename T::c_type;
  using MemoTableType = typename HashTraits<T>::MemoTableType;

  static Status GetDictionaryArrayData(MemoryPool* pool,
                                       const std::shared_ptr<DataType>& type,
                                       const MemoTableType& memo_table,
                                       int64_t start_offset,
                                       std::shared_ptr<ArrayData>* out) {
    std::shared_ptr<Buffer> dict_buffer;
    auto dict_length = static_cast<int64_t>(memo_table.size()) - start_offset;
    // This makes a copy, but we assume a dictionary array is usually small
    // compared to the size of the dictionary-using array.
    // (also, copying the dictionary values is cheap compared to the cost
    //  of building the memo table)
    RETURN_NOT_OK(
        AllocateBuffer(pool, TypeTraits<T>::bytes_required(dict_length), &dict_buffer));
    memo_table.CopyValues(static_cast<int32_t>(start_offset),
                          reinterpret_cast<c_type*>(dict_buffer->mutable_data()));

    int64_t null_count = 0;
    std::shared_ptr<Buffer> null_bitmap = nullptr;
    RETURN_NOT_OK(
        ComputeNullBitmap(pool, memo_table, start_offset, &null_count, &null_bitmap));

    *out = ArrayData::Make(type, dict_length, {null_bitmap, dict_buffer}, null_count);
    return Status::OK();
  }
};

template <typename T>
struct DictionaryTraits<T, enable_if_base_binary<T>> {
  using MemoTableType = typename HashTraits<T>::MemoTableType;

  static Status GetDictionaryArrayData(MemoryPool* pool,
                                       const std::shared_ptr<DataType>& type,
                                       const MemoTableType& memo_table,
                                       int64_t start_offset,
                                       std::shared_ptr<ArrayData>* out) {
    using offset_type = typename T::offset_type;
    std::shared_ptr<Buffer> dict_offsets;
    std::shared_ptr<Buffer> dict_data;

    // Create the offsets buffer
    auto dict_length = static_cast<int64_t>(memo_table.size() - start_offset);
    if (dict_length > 0) {
      RETURN_NOT_OK(
          AllocateBuffer(pool, sizeof(offset_type) * (dict_length + 1), &dict_offsets));
      auto raw_offsets = reinterpret_cast<offset_type*>(dict_offsets->mutable_data());
      memo_table.CopyOffsets(static_cast<int32_t>(start_offset), raw_offsets);
    }

    // Create the data buffer
    auto values_size = memo_table.values_size();
    if (values_size > 0) {
      RETURN_NOT_OK(AllocateBuffer(pool, values_size, &dict_data));
      memo_table.CopyValues(static_cast<int32_t>(start_offset), dict_data->size(),
                            dict_data->mutable_data());
    }

    int64_t null_count = 0;
    std::shared_ptr<Buffer> null_bitmap = nullptr;
    RETURN_NOT_OK(
        ComputeNullBitmap(pool, memo_table, start_offset, &null_count, &null_bitmap));

    *out = ArrayData::Make(type, dict_length, {null_bitmap, dict_offsets, dict_data},
                           null_count);

    return Status::OK();
  }
};

template <typename T>
struct DictionaryTraits<T, enable_if_fixed_size_binary<T>> {
  using MemoTableType = typename HashTraits<T>::MemoTableType;

  static Status GetDictionaryArrayData(MemoryPool* pool,
                                       const std::shared_ptr<DataType>& type,
                                       const MemoTableType& memo_table,
                                       int64_t start_offset,
                                       std::shared_ptr<ArrayData>* out) {
    const T& concrete_type = internal::checked_cast<const T&>(*type);
    std::shared_ptr<Buffer> dict_data;

    // Create the data buffer
    auto dict_length = static_cast<int64_t>(memo_table.size() - start_offset);
    auto width_length = concrete_type.byte_width();
    auto data_length = dict_length * width_length;
    RETURN_NOT_OK(AllocateBuffer(pool, data_length, &dict_data));
    auto data = dict_data->mutable_data();

    memo_table.CopyFixedWidthValues(static_cast<int32_t>(start_offset), width_length,
                                    data_length, data);

    int64_t null_count = 0;
    std::shared_ptr<Buffer> null_bitmap = nullptr;
    RETURN_NOT_OK(
        ComputeNullBitmap(pool, memo_table, start_offset, &null_count, &null_bitmap));

    *out = ArrayData::Make(type, dict_length, {null_bitmap, dict_data}, null_count);
    return Status::OK();
  }
};

template <typename T>
struct DictionaryCTraits {
  using ArrowType = typename CTypeTraits<T>::ArrowType;
  using MemoTableType = typename DictionaryTraits<ArrowType>::MemoTableType;
};

template <>
struct DictionaryCTraits<util::string_view> {
  using MemoTableType = DictionaryTraits<BinaryType>::MemoTableType;
};

}  // namespace internal
}  // namespace arrow
