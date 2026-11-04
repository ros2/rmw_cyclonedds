// Copyright 2019 Rover Robotics via Dan Rose
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "TypeSupport2.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rcutils/error_handling.h"
#include "rosidl_buffer/buffer.hpp"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"

namespace rmw_cyclonedds_cpp
{
const PrimitiveValueType primitive_value_type_boolean =
  PrimitiveValueType(ROSIDL_TypeKind::BOOLEAN);

// install_buffer_impl's write-side lambdas take care not to
// `impl.release()` and rewrap the raw pointer in
// `std::unique_ptr<rosidl::BufferImplBase<uint8_t>>` (default_delete),
// which would discard the custom deleter that BufferBackend::
// from_descriptor_with_endpoint() attaches -- rosidl_buffer_backend/
// buffer_backend.hpp's own doc comment says that deleter's whole purpose
// is "correct destruction across the plugin boundary". rosidl::Buffer<T>'s
// constructor only accepts a unique_ptr<BufferImplBase<T>> with the
// default deleter, so the original deleter cannot be handed to it
// directly; this adapter keeps the descriptor-provided unique_ptr (and its
// real deleter) alive as a member and forwards every BufferImplBase call
// to the wrapped object, so destroying the adapter (which Buffer<T> does
// via its own default_delete) invokes the backend's own deleter rather
// than a bare `delete`.
class DeleterPreservingBufferImpl final : public rosidl::BufferImplBase<uint8_t>
{
public:
  explicit DeleterPreservingBufferImpl(std::unique_ptr<void, void (*)(void *)> owned)
  : owned_(std::move(owned)),
    real_(static_cast<rosidl::BufferImplBase<uint8_t> *>(owned_.get()))
  {}

  std::string get_backend_type() const override {return real_->get_backend_type();}
  size_t size() const override {return real_->size();}
  std::unique_ptr<rosidl::BufferImplBase<uint8_t>> to_cpu() const override
  {
    return real_->to_cpu();
  }
  std::unique_ptr<rosidl::BufferImplBase<uint8_t>> clone() const override
  {
    return real_->clone();
  }
  const void * descriptor() const override {return real_->descriptor();}

private:
  std::unique_ptr<void, void (*)(void *)> owned_;
  rosidl::BufferImplBase<uint8_t> * real_;
};

class ROSIDLC_StructValueType : public StructValueType
{
  const rosidl_typesupport_introspection_c__MessageMembers * impl;
  std::vector<Member> m_members;
  std::vector<std::unique_ptr<const AnyValueType>> m_inner_value_types;

  template<typename ConstructedType, typename ... Args>
  ConstructedType * make_value_type(Args && ... args)
  {
    auto unique_ptr = std::make_unique<ConstructedType>(std::forward<Args>(args)...);
    auto ptr = unique_ptr.get();
    m_inner_value_types.push_back(std::move(unique_ptr));
    return ptr;
  }

public:
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_C;
  explicit ROSIDLC_StructValueType(const rosidl_typesupport_introspection_c__MessageMembers * impl);
  size_t sizeof_struct() const override {return impl->size_of_;}
  size_t cdrsizeof_struct() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_struct() const override {throw std::logic_error("not implemented");}
  TypeGenerator type_generator() const override {return gen;}
  size_t n_members() const override {return impl->member_count_;}
  const Member * get_member(size_t index) const override {return &m_members.at(index);}
};

class ROSIDLCPP_StructValueType : public StructValueType
{
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl;
  std::vector<Member> m_members;
  std::vector<std::unique_ptr<const AnyValueType>> m_inner_value_types;
  template<typename ConstructedType, typename ... Args>
  ConstructedType * make_value_type(Args && ... args)
  {
    auto unique_ptr = std::make_unique<ConstructedType>(std::forward<Args>(args)...);
    auto ptr = unique_ptr.get();
    m_inner_value_types.push_back(std::move(unique_ptr));
    return ptr;
  }

public:
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_Cpp;
  explicit ROSIDLCPP_StructValueType(
    const rosidl_typesupport_introspection_cpp::MessageMembers * impl);
  size_t sizeof_struct() const override {return impl->size_of_;}
  size_t cdrsizeof_struct() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_struct() const override {throw std::logic_error("not implemented");}
  TypeGenerator type_generator() const override {return gen;}
  size_t n_members() const override {return impl->member_count_;}
  const Member * get_member(size_t index) const final {return &m_members.at(index);}
};

std::unique_ptr<StructValueType> make_message_value_type(const rosidl_message_type_support_t * mts)
{
  if (auto ts_c =
    get_message_typesupport_handle(
      mts,
      TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::get_identifier()))
  {
    auto members = static_cast<const MetaMessage<TypeGenerator::ROSIDL_C> *>(ts_c->data);
    return std::make_unique<ROSIDLC_StructValueType>(members);
  } else {
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();

    if (auto ts_cpp =
      get_message_typesupport_handle(
        mts,
        TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::get_identifier()))
    {
      auto members = static_cast<const MetaMessage<TypeGenerator::ROSIDL_Cpp> *>(ts_cpp->data);
      return std::make_unique<ROSIDLCPP_StructValueType>(members);
    } else {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();

      throw std::runtime_error(
              std::string("Type support not from this implementation.  Got:\n") +
              "    " + prev_error_string.str + "\n" +
              "    " + error_string.str + "\n" +
              "while fetching it");
    }
  }
}

std::pair<std::unique_ptr<StructValueType>, std::unique_ptr<StructValueType>>
make_request_response_value_types(const rosidl_service_type_support_t * svc_ts)
{
  if (auto tsc =
    get_service_typesupport_handle(
      svc_ts,
      TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::get_identifier()))
  {
    auto typed =
      static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::MetaService *>(tsc->data);
    return {
      std::make_unique<ROSIDLC_StructValueType>(typed->request_members_),
      std::make_unique<ROSIDLC_StructValueType>(typed->response_members_)
    };
  } else {
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();

    if (auto tscpp =
      get_service_typesupport_handle(
        svc_ts,
        TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::get_identifier()))
    {
      auto typed =
        static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::MetaService *>(tscpp->data);
      return {
        std::make_unique<ROSIDLCPP_StructValueType>(typed->request_members_),
        std::make_unique<ROSIDLCPP_StructValueType>(typed->response_members_)
      };
    } else {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();

      throw std::runtime_error(
              std::string("Service type support not from this implementation.  Got:\n") +
              "    " + prev_error_string.str + "\n" +
              "    " + error_string.str + "\n" +
              "while fetching it");
    }
  }
}

ROSIDLC_StructValueType::ROSIDLC_StructValueType(
  const rosidl_typesupport_introspection_c__MessageMembers * impl)
: impl{impl}, m_members{}, m_inner_value_types{}
{
  bool has_keys = false;
  bool is_self_contained = true;
  for (size_t index = 0; index < impl->member_count_; index++) {
    auto member_impl = impl->members_[index];

    const AnyValueType * element_value_type;
    switch (ROSIDL_TypeKind(member_impl.type_id_)) {
      case ROSIDL_TypeKind::MESSAGE:
        m_inner_value_types.push_back(make_message_value_type(member_impl.members_));
        element_value_type = m_inner_value_types.back().get();
        break;
      case ROSIDL_TypeKind::STRING:
        element_value_type = make_value_type<ROSIDLC_StringValueType>(UINT32_MAX - 1);
        break;
      case ROSIDL_TypeKind::WSTRING:
        element_value_type = make_value_type<ROSIDLC_WStringValueType>(UINT32_MAX / 2);
        break;
      default:
        element_value_type =
          make_value_type<PrimitiveValueType>(ROSIDL_TypeKind(member_impl.type_id_));
        break;
    }

    const AnyValueType * member_value_type;
    if (member_impl.is_array_ && member_impl.array_size_ > UINT32_MAX) {
      throw std::length_error("arrays > UINT32_MAX not supported");
    }
    uint32_t bound = UINT32_MAX;
    if (member_impl.is_array_ && member_impl.array_size_ != 0 && member_impl.is_upper_bound_) {
      bound = static_cast<uint32_t>(member_impl.array_size_);
    }
    if (!member_impl.is_array_) {
      member_value_type = element_value_type;
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      member_value_type = make_value_type<ArrayValueType>(
        element_value_type, member_impl.array_size_);
    } else if (member_impl.size_function) {
      member_value_type = make_value_type<CallbackSpanSequenceValueType>(
        element_value_type,
        bound,
        member_impl.size_function,
        member_impl.get_const_function,
        member_impl.get_function,
        [member_impl](void * p, size_t s){
          if (!member_impl.resize_function(p, s)) {throw std::bad_alloc();}
        },
        member_impl.is_rosidl_buffer_,
        member_impl.is_rosidl_buffer_ ?
        std::function<const void * (const void *)>(
          [](const void * p) -> const void * {
            // The generated rosidl_runtime_c__..._Sequence struct's `data`
            // field IS an rosidl::Buffer<uint8_t>* value (not a raw array
            // pointer) whenever is_rosidl_buffer_ is true -- see
            // rosidl_runtime_c/primitives_sequence.h. Reading it directly
            // needs no new accessor on the C side; get_impl() (public,
            // non-throwing) normalizes it to the same BufferImplBase<T>*
            // contract the C++ introspection path's get_buffer_impl_
            // function already returns, so Serialization.cpp can treat
            // both uniformly.
            struct BufferBackedSequenceView {void * data; size_t size; size_t capacity;};
            auto * buf = static_cast<rosidl::Buffer<uint8_t> *>(
              static_cast<const BufferBackedSequenceView *>(p)->data);
            return buf->get_impl();
          }) :
        std::function<const void * (const void *)>(nullptr),
        member_impl.is_rosidl_buffer_ ?
        std::function<void(void *, std::unique_ptr<void, void (*)(void *)>)>(
          [](void * p, std::unique_ptr<void, void (*)(void *)> impl) {
            // Mirrors the getter lambda above exactly -- same `data`-field
            // convention, other direction. `impl`'s own deleter is
            // preserved via DeleterPreservingBufferImpl (see its
            // definition above) rather than dropped, since
            // from_descriptor_with_endpoint()'s deleter is not guaranteed
            // to be a plain `delete` for every backend.
            //
            // The move-assignment below is a post-construction backend
            // replacement, which rosidl_buffer/buffer.hpp's own
            // constructor doc says has "no post-construction setter,
            // which avoids race conditions with concurrent reads" --
            // that guarantee is about Buffer<T>'s own public surface
            // (there is no `set_impl()`), not something this call site
            // can restore; `operator=` is itself public API on Buffer<T>
            // and this line does not add a new hole. It is safe HERE
            // specifically because it runs inside rmw_take_int(), before
            // the message is handed to the caller -- no other thread can
            // hold a reference to `buf` yet.
            struct BufferBackedSequenceView {void * data; size_t size; size_t capacity;};
            auto * buf = static_cast<rosidl::Buffer<uint8_t> *>(
              static_cast<BufferBackedSequenceView *>(p)->data);
            *buf = rosidl::Buffer<uint8_t>(
              std::make_unique<DeleterPreservingBufferImpl>(std::move(impl)));
          }) :
        std::function<void(void *, std::unique_ptr<void, void (*)(void *)>)>(nullptr));
    } else {
      member_value_type = make_value_type<ROSIDLC_SpanSequenceValueType>(
        element_value_type,
        bound,
        member_impl.resize_function);
    }
    if (member_impl.is_key_) {
      has_keys = true;
    }
    if (!member_value_type->is_self_contained()) {
      is_self_contained = false;
    }
    m_members.push_back(
      Member{
        member_impl.name_,
        member_value_type,
        member_impl.offset_,
        member_impl.is_key_
      });
  }
  m_has_keys = has_keys;
  m_is_self_contained = is_self_contained;
}

ROSIDLCPP_StructValueType::ROSIDLCPP_StructValueType(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl)
: impl(impl)
{
  bool has_keys = false;
  bool is_self_contained = true;
  for (size_t index = 0; index < impl->member_count_; index++) {
    auto member_impl = impl->members_[index];

    const AnyValueType * element_value_type;
    switch (ROSIDL_TypeKind(member_impl.type_id_)) {
      case ROSIDL_TypeKind::MESSAGE:
        m_inner_value_types.push_back(make_message_value_type(member_impl.members_));
        element_value_type = m_inner_value_types.back().get();
        break;
      case ROSIDL_TypeKind::STRING:
        element_value_type = make_value_type<ROSIDLCPP_StringValueType>(UINT32_MAX - 1);
        break;
      case ROSIDL_TypeKind::WSTRING:
        element_value_type = make_value_type<ROSIDLCPP_U16StringValueType>(UINT32_MAX / 2);
        break;
      default:
        element_value_type =
          make_value_type<PrimitiveValueType>(ROSIDL_TypeKind(member_impl.type_id_));
        break;
    }

    const AnyValueType * member_value_type;
    if (member_impl.is_array_ && member_impl.array_size_ > UINT32_MAX) {
      throw std::length_error("arrays > UINT32_MAX not supported");
    }
    uint32_t bound = UINT32_MAX;
    if (member_impl.is_array_ && member_impl.array_size_ != 0 && member_impl.is_upper_bound_) {
      bound = static_cast<uint32_t>(member_impl.array_size_);
    }
    if (!member_impl.is_array_) {
      member_value_type = element_value_type;
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      member_value_type = make_value_type<ArrayValueType>(
        element_value_type, member_impl.array_size_);
    } else if (ROSIDL_TypeKind(member_impl.type_id_) == ROSIDL_TypeKind::BOOLEAN) {
      member_value_type =
        make_value_type<BoolVectorValueType>(
        bound);
    } else {
      member_value_type = make_value_type<CallbackSpanSequenceValueType>(
        element_value_type, bound,
        member_impl.size_function, member_impl.get_const_function, member_impl.get_function,
        member_impl.resize_function,
        member_impl.is_rosidl_buffer_,
        std::function<const void * (const void *)>(member_impl.get_buffer_impl_function),
        member_impl.is_rosidl_buffer_ ?
        std::function<void(void *, std::unique_ptr<void, void (*)(void *)>)>(
          [](void * untyped_member, std::unique_ptr<void, void (*)(void *)> impl) {
            // The write-side mirror of get_buffer_impl_function__*,
            // generated per-member in msg__type_support.cpp.em -- but
            // this direction needs no per-member codegen at all, because
            // the operation it performs (reinterpret_cast the member's
            // own address to rosidl::Buffer<uint8_t>*, then replace its
            // backend) does not depend on which message or member this
            // is, unlike the getter, which the .em template also could
            // have written this way. `untyped_member` is the exact same
            // pointer get_buffer_impl_function__*(untyped_member)
            // receives -- the address of the rosidl::Buffer<uint8_t>
            // member itself, per that generated function's own body
            // (`reinterpret_cast<const rosidl::Buffer<uint8_t> *>
            // (untyped_member)`). See the C-introspection lambda above
            // for why `impl`'s own deleter is preserved via
            // DeleterPreservingBufferImpl rather than dropped, and for
            // why the move-assignment below is safe despite Buffer<T>'s
            // constructor doc describing "no post-construction setter" --
            // same reasoning, same rmw_take_int() call site.
            auto * buf = reinterpret_cast<rosidl::Buffer<uint8_t> *>(untyped_member);
            *buf = rosidl::Buffer<uint8_t>(
              std::make_unique<DeleterPreservingBufferImpl>(std::move(impl)));
          }) :
        std::function<void(void *, std::unique_ptr<void, void (*)(void *)>)>(nullptr));
    }
    if (member_impl.is_key_) {
      has_keys = true;
    }
    if (!member_value_type->is_self_contained()) {
      is_self_contained = false;
    }
    m_members.push_back(
      Member {
        member_impl.name_,
        member_value_type,
        member_impl.offset_,
        member_impl.is_key_
      });
  }
  m_has_keys = has_keys;
  m_is_self_contained = is_self_contained;
}
}  // namespace rmw_cyclonedds_cpp
