//
// Created by Dan Rose on 2019-12-05.
//
#ifndef ROS2_MASTER_CDR_HPP
#define ROS2_MASTER_CDR_HPP

#include <cstddef>
#include <cstdint>
namespace rmw_cyclonedds_cpp
{
enum class EncodingVersion {
  CDR_Legacy,
  CDR1,
  CDR2,
};

class CDREncodingInfo
{
  EncodingVersion m_version;

public:
  explicit CDREncodingInfo(EncodingVersion version) { m_version = version; }

  size_t max_align() const
  {
    switch (m_version) {
      case EncodingVersion::CDR_Legacy:
      case EncodingVersion::CDR1:
        return 8;
      case EncodingVersion::CDR2:
        return 4;
    }
  }

  size_t get_size_of_primitive(ROSIDL_TypeKind tk) const
  {
    /// return 0 if the value type is not primitive
    /// else returns the number of bytes it should serialize to
    switch (tk) {
      case ROSIDL_TypeKind::BOOLEAN:
      case ROSIDL_TypeKind::OCTET:
      case ROSIDL_TypeKind::UINT8:
      case ROSIDL_TypeKind::INT8:
      case ROSIDL_TypeKind::CHAR:
        return 1;
      case ROSIDL_TypeKind::UINT16:
      case ROSIDL_TypeKind::INT16:
      case ROSIDL_TypeKind::WCHAR:
        return 2;
      case ROSIDL_TypeKind::UINT32:
      case ROSIDL_TypeKind::INT32:
      case ROSIDL_TypeKind::FLOAT:
        return 4;
      case ROSIDL_TypeKind::UINT64:
      case ROSIDL_TypeKind::INT64:
      case ROSIDL_TypeKind::DOUBLE:
        return 8;
      case ROSIDL_TypeKind::LONG_DOUBLE:
        return 16;
      default:
        return 0;
    }
  }

  size_t get_align_of_primitive(ROSIDL_TypeKind tk) const
  {
    size_t sizeof_ = get_size_of_primitive(tk);
    return std::min(sizeof_ ,max_align());
  }
};
}

#endif  //ROS2_MASTER_CDR_HPP
