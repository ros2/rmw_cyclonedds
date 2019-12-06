//
// Created by Dan Rose on 2019-12-05.
//

#ifndef ROS2_MASTER_CDRCURSOR_HPP
#define ROS2_MASTER_CDRCURSOR_HPP
namespace rmw_cyclonedds_cpp
{
struct CDRCursor
{
  CDRCursor() = default;
  ~CDRCursor() = default;

  // don't want to accidentally copy
  explicit CDRCursor(CDRCursor const &) = delete;
  void operator=(CDRCursor const & x) = delete;

  // virtual functions to be implemented
  // get the cursor's current offset.
  virtual size_t offset() const = 0;
  // advance the cursor.
  virtual void advance(size_t n_bytes) = 0;
  // Copy bytes to the current cursor location (if needed) and advance the cursor
  virtual void put_bytes(const void * data, size_t n_bytes) = 0;
  virtual void get_bytes(void * dest, size_t n_bytes) = 0;

  virtual bool ignores_data() const = 0;
  // Move the logical origin this many places
  virtual void rebase(ptrdiff_t relative_origin) = 0;

  void align(size_t n_bytes)
  {
    assert(n_bytes > 0);
    size_t start_offset = offset();
    if (n_bytes == 1 || start_offset % n_bytes == 0) {
      return;
    }
    advance((-start_offset) % n_bytes);
    assert(offset() - start_offset < n_bytes);
    assert(offset() % n_bytes == 0);
  }
  ptrdiff_t operator-(const CDRCursor & other) const
  {
    return static_cast<ptrdiff_t>(offset()) - static_cast<ptrdiff_t>(other.offset());
  }
};

struct SizeCursor : CDRCursor
{
  SizeCursor() : SizeCursor(0) {}
  explicit SizeCursor(size_t initial_offset) : m_offset(initial_offset) {}
  explicit SizeCursor(CDRCursor & c) : m_offset(c.offset()) {}

  size_t m_offset;
  size_t offset() const final { return m_offset; }
  void advance(size_t n_bytes) final { m_offset += n_bytes; }
  void put_bytes(const void *, size_t n_bytes) final { advance(n_bytes); }
  void get_bytes(void *, size_t n_bytes) final { advance(n_bytes); }
  bool ignores_data() const final { return true; }
  void rebase(ptrdiff_t relative_origin) override
  {
    // we're moving the *origin* so this has to change in the *opposite* direction
    m_offset -= relative_origin;
  }
};

struct DataCursor : public CDRCursor
{
  const void * origin;
  void * position;

  explicit DataCursor(void * position) : origin(position), position(position) {}

  size_t offset() const final { return (const byte *)position - (const byte *)origin; }
  void advance(size_t n_bytes) final { position = byte_offset(position, n_bytes); }
  void put_bytes(const void * bytes, size_t n_bytes) final
  {
    if (n_bytes == 0) {
      return;
    }
    std::memcpy(position, bytes, n_bytes);
    position = byte_offset(position, n_bytes);
  }
  void get_bytes(void * dest, size_t n_bytes) final {
    if (n_bytes == 0) {
      return;
    }
    std::memcpy(dest, position, n_bytes);
    position = byte_offset(position, n_bytes);
  }
  bool ignores_data() const final { return false; }
  void rebase(ptrdiff_t relative_origin) final { origin = byte_offset(origin, relative_origin); }
};
}  // namespace rmw_cyclonedds_cpp
#endif  //ROS2_MASTER_CDRCURSOR_HPP
