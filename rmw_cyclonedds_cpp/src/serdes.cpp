#include "rmw_cyclonedds_cpp/serdes.hpp"

cycser::cycser (std::vector<unsigned char>& dst_)
  : dst (dst_)
  , off (0)
{
  dst.reserve (4);
  /* FIXME: hard code to little endian ... and ignoring endianness in deser */
  dst.push_back (0);
  dst.push_back (3);
  /* options: */
  dst.push_back (0);
  dst.push_back (0);
}

cycdeser::cycdeser (const void *data_, size_t size_)
  : data (static_cast<const char *> (data_) + 4)
  , pos (0)
  , lim (size_ - 4)
{
}
