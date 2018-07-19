#include "rmw_cyclonedds_cpp/serdes.hpp"

cycser::cycser(struct sertopic *topic)
{
    st = ddsi_serstate_new(topic);
    sd = nullptr;
}

cycser::~cycser()
{
    if (sd == nullptr) {
        ddsi_serstate_release(st);
    } else {
        ddsi_serdata_unref(sd);
    }
}

cycser& cycser::ref()
{
    assert(sd != nullptr);
    ddsi_serdata_ref(sd);
    return *this;
}

void cycser::unref()
{
    assert(sd != nullptr);
    ddsi_serdata_unref(sd);
}

cycser& cycser::fix()
{
    assert(sd == nullptr);
    sd = ddsi_serstate_fix(st);
    return *this;
}

struct serdata *cycser::data()
{
    assert(sd != nullptr);
    return sd;
}

cycdeser::cycdeser(const void *data_, size_t size_)
{
    data = static_cast<const char *>(data_);
    lim = size_;
    pos = 0;
}

cycdeser::~cycdeser()
{
}
