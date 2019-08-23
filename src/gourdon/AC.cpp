///
/// @file  AC_libdivide.cpp
/// @brief Implementation of the A + C formulas in Xavier Gourdon's
///        prime counting algorithm. In this version the memory usage
///        has been reduced from O(x^(1/2)) to O(z) by segmenting
///        the pi[x] lookup table. In each segment we process the
///        leaves that satisfy: low <= x / (prime * m) < high.
///
///        The A & C formulas roughly correspond to the easy special
///        leaves in the Deleglise-Rivat algorithm. Since both
///        formulas use a very similar segmented algorithm that goes
///        up to x^(1/2) it makes sense to merge the A & C formulas
///        hence reducing the runtime complexity by a factor of
///        O(x^(1/2) * ln ln x^(1/2)) and avoiding initializing some
///        data structures twice. Merging the A & C formulas also
///        improves scaling on systems with many CPU cores.
///
/// Copyright (C) 2019 Kim Walisch, <kim.walisch@gmail.com>
///
/// This file is distributed under the BSD License. See the COPYING
/// file in the top level directory.
///

#include <PiTable.hpp>
#include <SegmentedPiTable.hpp>
#include <primecount-internal.hpp>
#include <fast_div.hpp>
#include <generate.hpp>
#include <int128_t.hpp>
#include <min.hpp>
#include <imath.hpp>
#include <print.hpp>
#include <S2Status.hpp>

#include <stdint.h>
#include <vector>

using namespace std;
using namespace primecount;

namespace {

/// Compute the A formula
template <typename T, typename Primes>
T A(T x,
    int64_t y,
    int64_t b,
    int64_t max_a_prime,
    T x_div_low,
    T x_div_high,
    Primes& primes,
    PiTable& pi,
    SegmentedPiTable& segmentedPi)
{
  int64_t prime = primes[b];
  T xp = x / prime;
  T sum = 0;

  int64_t min_2nd_prime = min(x_div_high / prime, max_a_prime);
  int64_t i = pi[min_2nd_prime] + 1;
  i = max(i, b + 1);
  int64_t max_2nd_prime = min(x_div_low / prime, isqrt(xp));
  int64_t max_i = pi[max_2nd_prime];

  // x / (p * q) >= y
  for (; i <= max_i; i++)
  {
    int64_t xpq = fast_div64(xp, primes[i]);
    if (xpq < y)
      break;
    sum += segmentedPi[xpq];
  }

  // x / (p * q) < y
  for (; i <= max_i; i++)
  {
    int64_t xpq = fast_div64(xp, primes[i]);
    sum += segmentedPi[xpq] * 2;
  }

  return sum;
}

/// Used to compute the 1st part of the C formula.
/// Recursively iterate over the square free numbers coprime
/// to the first b primes. This algorithm is described in
/// section 2.2 of the paper: Douglas Staple, "The Combinatorial
/// Algorithm For Computing pi(x)", arXiv:1503.01839, 6 March
/// 2015.
///
template <int MU, typename T, typename Primes>
T C1(T xp,
     int64_t b,
     int64_t i,
     int64_t pi_y,
     int64_t m,
     int64_t min_m,
     int64_t max_m,
     Primes& primes,
     PiTable& pi)
{
  T sum = 0;

  for (i++; i <= pi_y; i++)
  {
    // Calculate next m
    T m128 = (T) m * primes[i];
    if (m128 > (T) max_m)
      return sum;

    int64_t m64 = (int64_t) m128;
    if (m64 > min_m) {
      int64_t xpm = fast_div64(xp, m64);
      sum += MU * (pi[xpm] - b + 2);
    }

    sum += C1<-MU>(xp, b, i, pi_y, m64, min_m, max_m, primes, pi);
  }

  return sum;
}

/// Compute the 2nd part of the C formula
template <typename T, typename Primes>
T C2(T x,
     int64_t y,
     int64_t b,
     T x_div_low,
     T x_div_high,
     Primes& primes,
     PiTable& pi,
     SegmentedPiTable& segmentedPi)
{
  int64_t prime = primes[b];
  T xp = x / prime;
  T sum = 0;

  int64_t max_m = min3(x_div_low / prime, xp / prime, y);
  T min_m128 = max3(x_div_high / prime, x / ipow<T>(prime, 3), prime);
  int64_t min_m = min(min_m128, max_m);

  int64_t i = pi[max_m];
  int64_t pi_min_m = pi[min_m];
  int64_t min_clustered = (int64_t) isqrt(xp);
  min_clustered = in_between(min_m, min_clustered, max_m);
  int64_t pi_min_clustered = pi[min_clustered];

  // Find all clustered easy leaves where
  // successive leaves are identical.
  // n = primes[b] * primes[i]
  // Which satisfy: n > z && primes[i] <= y
  while (i > pi_min_clustered)
  {
    int64_t xpq = fast_div64(xp, primes[i]);
    int64_t phi_xpq = segmentedPi[xpq] - b + 2;
    int64_t xpq2 = fast_div64(xp, primes[b + phi_xpq - 1]);
    int64_t i2 = segmentedPi[xpq2];
    sum += phi_xpq * (i - i2);
    i = i2;
  }

  // Find all sparse easy leaves where
  // successive leaves are different.
  // n = primes[b] * primes[i]
  // Which satisfy: n > z && primes[i] <= y
  for (; i > pi_min_m; i--)
  {
    int64_t xpq = fast_div64(xp, primes[i]);
    sum += segmentedPi[xpq] - b + 2;
  }

  return sum;
}

/// Compute A + C
template <typename T, typename Primes>
T AC_OpenMP(T x,
            int64_t y,
            int64_t z,
            int64_t k,
            int64_t x_star,
            int64_t max_a_prime,
            Primes& primes,
            int threads)
{
  T sum = 0;
  int64_t x13 = iroot<3>(x);
  int64_t thread_threshold = 1000;
  threads = ideal_num_threads(threads, x13, thread_threshold);

  S2Status status(x);
  PiTable pi(max(z, max_a_prime));
  SegmentedPiTable segmentedPi(isqrt(x), z, threads);

  int64_t pi_y = pi[y];
  int64_t pi_sqrtz = pi[isqrt(z)];
  int64_t pi_x_star = pi[x_star];
  int64_t pi_x13 = pi[x13];
  int64_t pi_root3_xy = pi[iroot<3>(x / y)];
  int64_t pi_root3_xz = pi[iroot<3>(x / z)];
  int64_t min_b = max(k, pi_root3_xz);

  // This computes the 1st part of the C formula.
  // Find all special leaves of type:
  // x / (primes[b] * m) <= z.
  // m may be a prime <= y or a square free number <= z who
  // is coprime to the first b primes and whose whose
  // largest prime factor <= y.
  #pragma omp parallel for schedule(dynamic) num_threads(threads) reduction(-: sum)
  for (int64_t b = min_b + 1; b <= pi_sqrtz; b++)
  {
    int64_t prime = primes[b];
    T xp = x / prime;
    int64_t max_m = min(xp / prime, z);
    T min_m128 = max(x / ipow<T>(prime, 3), z / prime);
    int64_t min_m = min(min_m128, max_m);

    sum -= C1<-1>(xp, b, b, pi_y, 1, min_m, max_m, primes, pi);

    if (is_print())
      status.print(b, pi_x13);
  }

  // This computes A and the 2nd part of the C formula.
  // Find all special leaves of type:
  // x / (primes[b] * primes[i]) <= x^(1/2)
  // with z^(1/2) < primes[b] <= x^(1/3).
  // Since we need to lookup PrimePi[n] values for n <= x^(1/2)
  // we use a segmented PrimePi[n] table of size z (~O(x^1/3))
  // in order to reduce the memory usage.
  for (; !segmentedPi.finished(); segmentedPi.next())
  {
    // Current segment [low, high[
    int64_t low = segmentedPi.low();
    int64_t high = segmentedPi.high();
    low = max(low, 1);
    T x_div_low = x / low;
    T x_div_high = x / high;

    int64_t min_prime1 = min(isqrt(low), primes[pi_x_star]);
    int64_t min_prime2 = min(x_div_high / y, primes[pi_x_star]);
    min_b = max3(k, pi_sqrtz, pi_root3_xy);
    min_b = max(min_b, pi[min_prime1]);
    min_b = max(min_b, pi[min_prime2]);

    // x / (primes[i] * primes[i+1]) >= low
    // primes[i] * primes[i+1] <= x / low
    // primes[i] < sqrt(x / low)
    // primes[i+1] <= || >= sqrt(x / low)
    int64_t sqrt_low = min(isqrt(x_div_low), x13);
    int64_t max_b = pi[sqrt_low];
    if (max_b + 1 < (int64_t) primes.size() &&
        primes[max_b] * (T) primes[max_b + 1] > x_div_low)
      max_b -= 1;

    min_b = min(min_b, pi_x_star + 1);
    max_b = max(max_b, pi_x_star);

    // C2 formula: pi[sqrt(z)] < b <= pi[x_star]
    // A  formula: pi[x_star] < b <= pi[x13]
    #pragma omp parallel for schedule(dynamic) num_threads(threads) reduction(+: sum)
    for (int64_t b = min_b + 1; b <= max_b; b++)
    {
      if (b <= pi_x_star)
        sum += C2(x, y, b, x_div_low, x_div_high, primes, pi, segmentedPi);
      else
        sum += A(x, y, b, max_a_prime, x_div_low, x_div_high, primes, pi, segmentedPi);

      if (is_print())
        status.print(b, pi_x13);
    }
  }

  return sum;
}

} // namespace

namespace primecount {

int64_t AC(int64_t x,
           int64_t y,
           int64_t z,
           int64_t k,
           int threads)
{
  print("");
  print("=== AC(x, y) ===");
  print(x, y, z, k, threads);

  double time = get_time();
  int64_t x_star = get_x_star_gourdon(x, y);
  int64_t max_c_prime = y;
  int64_t max_a_prime = (int64_t) isqrt(x / x_star);
  int64_t max_prime = max(max_a_prime, max_c_prime);
  auto primes = generate_primes<int32_t>(max_prime);

  int64_t ac = AC_OpenMP((intfast64_t) x, y, z, k, x_star, max_a_prime, primes, threads);

  print("A + C", ac, time);
  return ac;
}

#ifdef HAVE_INT128_T

int128_t AC(int128_t x,
            int64_t y,
            int64_t z,
            int64_t k,
            int threads)
{
  print("");
  print("=== AC(x, y) ===");
  print(x, y, z, k, threads);

  double time = get_time();
  int64_t x_star = get_x_star_gourdon(x, y);
  int64_t max_c_prime = y;
  int64_t max_a_prime = (int64_t) isqrt(x / x_star);
  int64_t max_prime = max(max_a_prime, max_c_prime);
  int128_t ac;

  // uses less memory
  if (max_prime <= numeric_limits<uint32_t>::max())
  {
    auto primes = generate_primes<uint32_t>(max_prime);
    ac = AC_OpenMP((intfast128_t) x, y, z, k, x_star, max_a_prime, primes, threads);
  }
  else
  {
    auto primes = generate_primes<int64_t>(max_prime);
    ac = AC_OpenMP((intfast128_t) x, y, z, k, x_star, max_a_prime,  primes, threads);
  }

  print("A + C", ac, time);
  return ac;
}

#endif

} // namespace
