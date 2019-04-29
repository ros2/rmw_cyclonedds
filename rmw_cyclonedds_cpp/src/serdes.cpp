#include "rmw_cyclonedds_cpp/serdes.hpp"

cycser::cycser (std::vector<unsigned char>& dst_)
  : dst (dst_)
  , off (0)
{
  dst.reserve (4);
  unsigned char *magic = dst.data ();
  /* FIXME: hard code to little endian ... and ignoring endianness in deser */
  magic[0] = 0;
  magic[1] = 3;
  /* options: */
  magic[2] = 0;
  magic[3] = 0;
}

cycdeser::cycdeser (const void *data_, size_t size_)
  : data (static_cast<const char *> (data_) + 4)
  , pos (0)
  , lim (size_ - 4)
{
}
