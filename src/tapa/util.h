#ifndef TAPA_UTIL_H_
#define TAPA_UTIL_H_

#include <climits>

#include <iostream>

#include "tapa/stream.h"

namespace tapa {

constexpr int join = 0;
constexpr int detach = -1;

namespace internal {

template <typename T, int width = T::width>
inline constexpr int widthof(int) {
  return T::width;
}

template <typename T>
inline constexpr int widthof(short) {
  return sizeof(T) * CHAR_BIT;
}

}  // namespace internal

/// Queries width (in bits) of the type.
///
/// @tparam T Type to be queried.
/// @return   @c T::width if it exists, `sizeof(T) * CHAR_BIT` otherwise.
template <typename T>
inline constexpr int widthof() {
  return internal::widthof<T>(0);
}

/// Queries width (in bits) of the object.
///
/// @note         Unlike @c sizeof, the argument expression is evaluated (though
///               unused).
/// @tparam T     Type of @c object.
/// @param object Object to be queried.
/// @return       @c T::width if it exists, `sizeof(T) * CHAR_BIT` otherwise.
template <typename T>
inline constexpr int widthof(T object) {
  return internal::widthof<T>(0);
}

template <uint64_t N>
inline constexpr uint64_t round_up_div(uint64_t i) {
  return ((i - 1) / N + 1);
}

template <uint64_t N>
inline constexpr uint64_t round_up(uint64_t i) {
  return ((i - 1) / N + 1) * N;
}

/// Obtain a value of type @c To by reinterpreting the object representation of
/// @c from.
///
/// @note       This function is slightly different from C++20 @c std::bit_cast.
/// @tparam To  Target type.
/// @param from Source object.
template <typename To, typename From>
inline typename std::enable_if<sizeof(To) == sizeof(From), To>::type  //
bit_cast(From from) noexcept {
#pragma HLS inline
  To to;
  std::memcpy(&to, &from, sizeof(To));
  return to;
}

template <typename T>
T reg(T x) {
#pragma HLS inline off
#pragma HLS pipeline II = 1
#pragma HLS latency min = 1 max = 1
  return x;
}

template <typename Addr, typename Payload>
struct packet {
  Addr addr;
  Payload payload;
};

template <typename Addr, typename Payload>
inline std::ostream& operator<<(std::ostream& os,
                                const packet<Addr, Payload>& obj) {
  return os << "{addr: " << obj.addr << ", payload: " << obj.payload << "}";
}

}  // namespace tapa

#define TAPA_WHILE_NOT_EOT(fifo)                                \
  for (bool tapa_##fifo##_valid;                                \
       !fifo.eot(tapa_##fifo##_valid) || !tapa_##fifo##_valid;) \
  _Pragma("HLS pipeline II = 1") if (tapa_##fifo##_valid)

#define TAPA_WHILE_NEITHER_EOT(fifo1, fifo2)                          \
  for (bool tapa_##fifo1##_valid, tapa_##fifo2##_valid;               \
       (!fifo1.eot(tapa_##fifo1##_valid) || !tapa_##fifo1##_valid) && \
       (!fifo2.eot(tapa_##fifo2##_valid) || !tapa_##fifo2##_valid);)  \
  _Pragma("HLS pipeline II = 1") if (tapa_##fifo1##_valid &&          \
                                     tapa_##fifo2##_valid)

#define TAPA_WHILE_NONE_EOT(fifo1, fifo2, fifo3)                              \
  for (bool tapa_##fifo1##_valid, tapa_##fifo2##_valid, tapa_##fifo3##_valid; \
       (!fifo1.eot(tapa_##fifo1##_valid) || !tapa_##fifo1##_valid) &&         \
       (!fifo2.eot(tapa_##fifo2##_valid) || !tapa_##fifo2##_valid) &&         \
       (!fifo3.eot(tapa_##fifo3##_valid) || !tapa_##fifo3##_valid);)          \
  _Pragma("HLS pipeline II = 1") if (tapa_##fifo1##_valid &&                  \
                                     tapa_##fifo2##_valid &&                  \
                                     tapa_##fifo3##_valid)

#endif  // TAPA_UTIL_H_
