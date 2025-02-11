/*
 * Copyright (c) 2009-2023, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef UPB_PROTOS_REPEATED_FIELD_H_
#define UPB_PROTOS_REPEATED_FIELD_H_

#include <cstddef>
#include <iterator>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"
#include "protos/protos.h"
#include "protos/protos_traits.h"
#include "protos/repeated_field_iterator.h"
#include "upb/base/string_view.h"
#include "upb/collections/array.h"
#include "upb/collections/array_internal.h"
#include "upb/mem/arena.h"
#include "upb/message/copy.h"
#include "upb/mini_table/types.h"

// Must be last:
#include "upb/port/def.inc"

namespace protos {

namespace internal {

// Shared implementation of repeated fields for absl::string_view and
// message types for mutable and immutable variants.
//
// Immutable (const accessor), constructs this class with a nullptr upb_Array*
// when the underlying array in the message is empty.
//
// Mutable accessors on the other hand, will allocate a new empty non-null
// upb_Array* for the message when the RepeatedFieldProxy is constructed.
template <class T>
class RepeatedFieldProxyBase {
  using Array = add_const_if_T_is_const<T, upb_Array>;

 public:
  explicit RepeatedFieldProxyBase(Array* arr) : arr_(arr) {}

  size_t size() const { return arr_ != nullptr ? upb_Array_Size(arr_) : 0; }

  bool empty() const { return size() == 0; }

 protected:
  // Returns upb_Array string member.
  inline absl::string_view GetString(size_t n) const;

  // Returns upb_Array message member.
  inline upb_Message* GetMessage(size_t n) const;

  Array* arr_;
};

template <class T>
inline absl::string_view RepeatedFieldProxyBase<T>::GetString(size_t n) const {
  upb_MessageValue message_value = upb_Array_Get(arr_, n);
  return absl::string_view(message_value.str_val.data,
                           message_value.str_val.size);
}

template <class T>
upb_Message* RepeatedFieldProxyBase<T>::GetMessage(size_t n) const {
  auto** messages =
      static_cast<upb_Message**>(upb_Array_MutableDataPtr(this->arr_));
  return messages[n];
}

template <class T>
class RepeatedFieldProxyMutableBase : public RepeatedFieldProxyBase<T> {
 public:
  RepeatedFieldProxyMutableBase(upb_Array* arr, upb_Arena* arena)
      : RepeatedFieldProxyBase<T>(arr), arena_(arena) {}

  void clear() { upb_Array_Resize(this->arr_, 0, arena_); }

 protected:
  upb_Arena* arena_;
};

// RepeatedField proxy for repeated messages.
template <class T>
class RepeatedFieldProxy
    : public std::conditional_t<std::is_const_v<T>, RepeatedFieldProxyBase<T>,
                                RepeatedFieldProxyMutableBase<T>> {
  static_assert(!std::is_same_v<T, absl::string_view>, "");
  static_assert(!std::is_same_v<T, const absl::string_view>, "");
  static_assert(!std::is_arithmetic_v<T>, "");
  static constexpr bool kIsConst = std::is_const_v<T>;

 public:
  explicit RepeatedFieldProxy(const upb_Array* arr)
      : RepeatedFieldProxyBase<T>(arr) {}
  RepeatedFieldProxy(upb_Array* arr, upb_Arena* arena)
      : RepeatedFieldProxyMutableBase<T>(arr, arena) {}
  // Constructor used by ::protos::Ptr.
  RepeatedFieldProxy(const RepeatedFieldProxy&) = default;

  // T::CProxy [] operator specialization.
  typename T::CProxy operator[](size_t n) const {
    upb_MessageValue message_value = upb_Array_Get(this->arr_, n);
    return ::protos::internal::CreateMessage<typename std::remove_const_t<T>>(
        (upb_Message*)message_value.msg_val);
  }

  // TODO(b:/280069986) : Audit/Finalize based on Iterator Design.
  // T::Proxy [] operator specialization.
  template <int&... DeductionBlocker, bool b = !kIsConst,
            typename = std::enable_if_t<b>>
  typename T::Proxy operator[](size_t n) {
    return ::protos::internal::CreateMessageProxy<T>(this->GetMessage(n),
                                                     this->arena_);
  }

  // Mutable message reference specialization.
  template <int&... DeductionBlocker, bool b = !kIsConst,
            typename = std::enable_if_t<b>>
  void push_back(const T& t) {
    upb_MessageValue message_value;
    message_value.msg_val =
        upb_Message_DeepClone(GetInternalMsg(t), T::minitable(), this->arena_);
    upb_Array_Append(this->arr_, message_value, this->arena_);
  }

  // Mutable message add using move.
  template <int&... DeductionBlocker, bool b = !kIsConst,
            typename = std::enable_if_t<b>>
  void push_back(T&& msg) {
    upb_MessageValue message_value;
    message_value.msg_val = GetInternalMsg(msg);
    upb_Arena_Fuse(GetArena(msg), this->arena_);
    upb_Array_Append(this->arr_, message_value, this->arena_);
    T moved_msg = std::move(msg);
  }

 private:
  friend class ::protos::Ptr<T>;
};

// RepeatedField proxy for repeated strings.
template <class T>
class RepeatedFieldStringProxy
    : public std::conditional_t<std::is_const_v<T>, RepeatedFieldProxyBase<T>,
                                RepeatedFieldProxyMutableBase<T>> {
  static_assert(std::is_same_v<T, absl::string_view> ||
                    std::is_same_v<T, const absl::string_view>,
                "");
  static constexpr bool kIsConst = std::is_const_v<T>;

 public:
  // Immutable constructor.
  explicit RepeatedFieldStringProxy(const upb_Array* arr)
      : RepeatedFieldProxyBase<T>(arr) {}
  // Mutable constructor.
  RepeatedFieldStringProxy(upb_Array* arr, upb_Arena* arena)
      : RepeatedFieldProxyMutableBase<T>(arr, arena) {}
  // Constructor used by ::protos::Ptr.
  RepeatedFieldStringProxy(const RepeatedFieldStringProxy&) = default;

  T operator[](size_t n) const { return this->GetString(n); }

  template <int&... DeductionBlocker, bool b = !kIsConst,
            typename = std::enable_if_t<b>>
  void push_back(T t) {
    upb_MessageValue message_value;
    // Copy string to arena.
    UPB_ASSERT(this->arena_);
    char* data = (char*)upb_Arena_Malloc(this->arena_, t.size());
    UPB_ASSERT(data);
    memcpy(data, t.data(), t.size());
    message_value.str_val = upb_StringView_FromDataAndSize(data, t.size());
    upb_Array_Append(this->arr_, message_value, this->arena_);
  }
};

// RepeatedField proxy for repeated scalar types.
template <typename T>
class RepeatedFieldScalarProxy
    : public std::conditional_t<std::is_const_v<T>, RepeatedFieldProxyBase<T>,
                                RepeatedFieldProxyMutableBase<T>> {
  static_assert(std::is_arithmetic_v<T>, "");
  static constexpr bool kIsConst = std::is_const_v<T>;

 public:
  explicit RepeatedFieldScalarProxy(const upb_Array* arr)
      : RepeatedFieldProxyBase<T>(arr) {}
  RepeatedFieldScalarProxy(upb_Array* arr, upb_Arena* arena)
      : RepeatedFieldProxyMutableBase<T>(arr, arena) {}
  // Constructor used by ::protos::Ptr.
  RepeatedFieldScalarProxy(const RepeatedFieldScalarProxy&) = default;

  T operator[](size_t n) const {
    upb_MessageValue message_value = upb_Array_Get(this->arr_, n);
    typename std::remove_const_t<T> val;
    memcpy(&val, &message_value, sizeof(T));
    return val;
  }

  template <int&... DeductionBlocker, bool b = !kIsConst,
            typename = std::enable_if_t<b>>
  void push_back(T t) {
    upb_MessageValue message_value;
    memcpy(&message_value, &t, sizeof(T));
    upb_Array_Append(this->arr_, message_value, this->arena_);
  }

  // Iterator support.
  using iterator = internal::RepeatedScalarIterator<T>;

  iterator begin() const { return iterator(unsafe_array()); }
  iterator cbegin() const { return begin(); }
  iterator end() const { return iterator(unsafe_array() + this->size()); }
  iterator cend() const { return end(); }

  // Reverse iterator support.
  using const_reverse_iterator = std::reverse_iterator<iterator>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }

 private:
  T* unsafe_array() const {
    if (kIsConst) {
      const void* unsafe_ptr = ::upb_Array_DataPtr(this->arr_);
      return static_cast<T*>(const_cast<void*>(unsafe_ptr));
    }
    if (!kIsConst) {
      void* unsafe_ptr =
          ::upb_Array_MutableDataPtr(const_cast<upb_Array*>(this->arr_));
      return static_cast<T*>(unsafe_ptr);
    }
  }
};

}  // namespace internal

template <typename T>
class RepeatedField {
  static constexpr bool kIsString = std::is_same_v<T, absl::string_view>;
  static constexpr bool kIsScalar = std::is_arithmetic_v<T>;

 public:
  using Proxy = std::conditional_t<
      kIsScalar, internal::RepeatedFieldScalarProxy<T>,
      std::conditional_t<kIsString, internal::RepeatedFieldStringProxy<T>,
                         internal::RepeatedFieldProxy<T>>>;
  using CProxy = std::conditional_t<
      kIsScalar, internal::RepeatedFieldScalarProxy<const T>,
      std::conditional_t<kIsString, internal::RepeatedFieldStringProxy<const T>,
                         internal::RepeatedFieldProxy<const T>>>;
  // TODO(b/286451125): T supports incomplete type from fwd.h forwarding headers
  // We would like to reference T::CProxy. Validate forwarding header design.
  using ValueProxy = std::conditional_t<
      kIsScalar, T,
      std::conditional_t<kIsString, absl::string_view, ::protos::Ptr<T>>>;
  using ValueCProxy = std::conditional_t<
      kIsScalar, const T,
      std::conditional_t<kIsString, absl::string_view, ::protos::Ptr<const T>>>;
  using Access = std::conditional_t<
      kIsScalar, internal::RepeatedFieldScalarProxy<T>,
      std::conditional_t<kIsString, internal::RepeatedFieldStringProxy<T>,
                         internal::RepeatedFieldProxy<T>>>;
};

}  // namespace protos

#include "upb/port/undef.inc"

#endif  // UPB_PROTOS_REPEATED_FIELD_H_
