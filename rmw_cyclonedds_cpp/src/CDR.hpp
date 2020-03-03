//
// Created by Dan Rose on 2019-12-05.
//
#ifndef ROS2_MASTER_CDR_HPP
#define ROS2_MASTER_CDR_HPP

#include <cstddef>
#include <cstdint>

enum class EncodingVersion {
  CDR_Legacy,
  CDR1,
  CDR2,
};

struct PrimitiveValueType{};

struct SignedIntegralValueType : public PrimitiveValueType
{
  uint8_t num_bits;
};

struct UnsignedIntegralValueType : public PrimitiveValueType
{
  uint8_t num_bits;
};
struct BooleanValueType : public PrimitiveValueType
{
};
struct ByteValueType : public PrimitiveValueType
{
};
struct FloatValueType : public PrimitiveValueType
{
  uint8_t num_bits;
  uint8_t exponent_bits;
  uint8_t fraction_bits;
};
struct CharacterValueType : public PrimitiveValueType
{
  uint8_t num_bits;
};
struct StringValueType  {
  CharacterValueType c();
  uint_least32_t bound();
};
static constexpr size_t get_cdr_sizeof(SignedIntegralValueType v) { return v.num_bits / 8; }
static constexpr size_t get_cdr_sizeof(UnsignedIntegralValueType v) { return v.num_bits / 8; }
static constexpr size_t get_cdr_sizeof(BooleanValueType) { return 1; }
static constexpr size_t get_cdr_sizeof(ByteValueType) { return 1; }
static constexpr size_t get_cdr_sizeof(FloatValueType v) { return v.num_bits / 8; }
static constexpr size_t get_cdr_sizeof(CharacterValueType v) { return v.num_bits / 8; }
static constexpr size_t get_cdr_maxalign(EncodingVersion eversion)
{
  switch (eversion) {
    case EncodingVersion::CDR_Legacy:
    case EncodingVersion::CDR1:
      return 8;
    case EncodingVersion::CDR2:
      return 4;
  }
}
template<typename VT>
static constexpr size_t get_cdr_alignof(EncodingVersion encoding, VT vt){
  return std::min(get_cdr_maxalign(vt), get_cdr_maxalign(encoding));
}

#endif  //ROS2_MASTER_CDR_HPP
