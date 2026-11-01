#ifndef STUB_SERIAL_LINK_H_
#define STUB_SERIAL_LINK_H_
#include <cstddef>
#include <stdint.h>
namespace stages {
enum SerialLinkDirection { SERIAL_LINK_DIRECTION_LEFT, SERIAL_LINK_DIRECTION_RIGHT };
class SerialLink {
 public:
  void Init(SerialLinkDirection, uint32_t, uint8_t*, size_t) { }
  template<typename T> void Transmit(const T&) { }
  template<typename T> const T* available_rx_buffer() { return NULL; }
};
}
#endif
