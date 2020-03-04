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
#include "Deserialization.hpp"

#include "CDR.hpp"
#include "CDRCursor.hpp"
namespace rmw_cyclonedds_cpp
{
class CDRReader : public AbstractCDRReader
{
protected:
  CDREncodingInfo m_cdr;

public:
  explicit CDRReader(std::unique_ptr<StructValueType> value_type)
  : m_cdr{EncodingVersion::CDR_Legacy}
  {
    std::ignore = (value_type);  //todo
  }

  void deserialize_top_level(
    void * destination_object, const void * data, const StructValueType * ts);

  uint32_t read_u32(CDRReadCursor * data_cursor)
  {
    uint32_t result;
    data_cursor->align(m_cdr.get_align_of_primitive(ROSIDL_TypeKind::UINT32));
    // todo: handle endianness if needed
    data_cursor->get_bytes(&result, m_cdr.get_size_of_primitive(ROSIDL_TypeKind::UINT32));
    return result;
  }

  void deserialize(
    void * destination_object, const PrimitiveValueType & vt, CDRReadCursor * data_cursor)
  {
    data_cursor->align(m_cdr.get_align_of_primitive(vt.type_kind()));
    // todo: handle endianness if needed
    data_cursor->get_bytes(destination_object, m_cdr.get_size_of_primitive(vt.type_kind()));
  }

  void deserialize(
    void * destination_object, const U8StringValueType & vt, CDRReadCursor * data_cursor)
  {
    size_t size = read_u32(data_cursor);
    vt.assign(destination_object, static_cast<const char *>(data_cursor->position), size);
  }

  void deserialize(
    void * destination_object, const U16StringValueType & vt, CDRReadCursor * data_cursor)
  {
    size_t size = read_u32(data_cursor);
    // todo: handle endianness if needed
    vt.assign(destination_object, static_cast<const uint16_t *>(data_cursor->position), size);
  }

  void deserialize(void * destination_object, const ArrayValueType & vt, CDRReadCursor * data_cursor)
  {
    for (size_t i=0; i<vt.array_size(); i++) {
      void * dest = byte_offset(destination_object,i*vt.element_value_type()->sizeof_type());
      deserialize(dest, vt.element_value_type(), data_cursor);
    }
  }

  void deserialize(
    void * destination_object, const SpanSequenceValueType & vt, CDRReadCursor * data_cursor)
  {
    size_t size = read_u32(data_cursor);
    vt.resize(destination_object, size);
    void * sequence_contents = vt.sequence_contents(destination_object);
    for (size_t i=0; i<size; i++) {
      void * dest = byte_offset(sequence_contents, i*vt.element_value_type()->sizeof_type());
      deserialize(dest, vt.element_value_type(), data_cursor);
    }
  }

  void deserialize(
    void * destination_object, const StructValueType & vt, CDRReadCursor * data_cursor)
  {
    for (size_t i = 0; i < vt.n_members(); i++) {
      auto member = vt.get_member(i);
      auto member_object = byte_offset(destination_object, member->member_offset);
      deserialize(member_object, member->value_type, data_cursor);
    }
  }

  void deserialize(void * destination_object, const AnyValueType * vt, CDRReadCursor * data_cursor)
  {
    switch (vt->e_value_type()) {
      case EValueType::PrimitiveValueType:
        deserialize(destination_object, *static_cast<const PrimitiveValueType *>(vt), data_cursor);
        break;
      case EValueType::U8StringValueType:
        deserialize(destination_object, *static_cast<const U8StringValueType *>(vt), data_cursor);
        break;
      case EValueType::U16StringValueType:
        deserialize(destination_object, *static_cast<const U16StringValueType *>(vt), data_cursor);
        break;
      case EValueType::StructValueType:
        deserialize(destination_object, *static_cast<const StructValueType *>(vt), data_cursor);
        break;
      case EValueType::ArrayValueType:
        deserialize(destination_object, *static_cast<const ArrayValueType *>(vt), data_cursor);
        break;
      case EValueType::SpanSequenceValueType:
        deserialize(
          destination_object, *static_cast<const SpanSequenceValueType *>(vt), data_cursor);
        break;
      case EValueType::BoolVectorValueType:
        // todo
        throw std::exception();
        break;
    }
  }
};
}  // namespace rmw_cyclonedds_cpp