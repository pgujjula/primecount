///
/// @file  pi_deleglise_rivat_parallel1.cpp
/// @brief Implementation of the Deleglise-Rivat prime counting
///        algorithm. In the Deleglise-Rivat algorithm there are 3
///        additional types of special leaves compared to the
///        Lagarias-Miller-Odlyzko algorithm: trivial special leaves,
///        clustered easy leaves and sparse easy leaves.
///
///        This implementation is based on the paper:
///        Tomás Oliveira e Silva, Computing pi(x): the combinatorial
///        method, Revista do DETUA, vol. 4, no. 6, March 2006,
///        pp. 759-768.
///
/// Copyright (C) 2014 Kim Walisch, <kim.walisch@gmail.com>
///
/// This file is distributed under the BSD License. See the COPYING
/// file in the top level directory.
///

#include <primecount-internal.hpp>
#include <BitSieve.hpp>
#include <generate.hpp>
#include <pmath.hpp>
#include <PhiTiny.hpp>
#include <S1.hpp>
#include <tos_counters.hpp>

#include <stdint.h>
#include <algorithm>
#include <vector>
#include <iostream>

using namespace std;
using namespace primecount;

namespace {

/// Cross-off the multiples of prime in the sieve array.
/// For each element that is unmarked the first time update
/// the special counters tree data structure.
///
template <typename T>
void cross_off(int64_t prime,
               int64_t low,
               int64_t high,
               int64_t& next_multiple,
               BitSieve& sieve,
               T& counters)
{
  int64_t segment_size = sieve.size();
  int64_t k = next_multiple;

  for (; k < high; k += prime * 2)
  {
    if (sieve[k - low])
    {
      sieve.unset(k - low);
      cnt_update(counters, k - low, segment_size);
    }
  }
  next_multiple = k;
}

/// Calculate the contribution of the trivial leaves.
///
int64_t S2_trivial(int64_t x,
                   int64_t y,
                   int64_t z,
                   int64_t c,
                   vector<int32_t>& pi,
                   vector<int32_t>& primes)
{
  int64_t pi_y = pi[y];
  int64_t pi_sqrtz = pi[min(isqrt(z), y)];
  int64_t S2_result = 0;

  // Find all trivial leaves: n = primes[b] * primes[l]
  // which satisfy phi(x / n), b - 1) = 1
  for (int64_t b = max(c, pi_sqrtz + 1); b < pi_y; b++)
  {
    int64_t prime = primes[b];
    S2_result += pi_y - pi[max(x / (prime * prime), prime)];
  }

  return S2_result;
}

/// Calculate the contribution of the trivial leaves, the clustered
/// easy leaves and the sparse easy leaves.
///
int64_t S2_easy(int64_t x,
                int64_t y,
                int64_t z,
                int64_t c,
                vector<int32_t>& pi,
                vector<int32_t>& primes)
{
  int64_t pi_sqrty = pi[isqrt(y)];
  int64_t pi_x13 = pi[iroot<3>(x)];
  int64_t S2_result = 0;

  for (int64_t b = max(c, pi_sqrty) + 1; b <= pi_x13; b++)
  {
    int64_t prime = primes[b];
    int64_t min_trivial_leaf = x / (prime * prime);
    int64_t min_clustered_easy_leaf = isqrt(x / prime);
    int64_t min_sparse_easy_leaf = z / prime;
    int64_t min_hard_leaf = max(y / prime, prime);

    min_sparse_easy_leaf = max(min_sparse_easy_leaf, min_hard_leaf);
    min_clustered_easy_leaf = max(min_clustered_easy_leaf, min_hard_leaf);
    int64_t l = pi[min(min_trivial_leaf, y)];

    // Find all clustered easy leaves:
    // x / n <= y and phi(x / n, b - 1) == phi(x / m, b - 1)
    // where phi(x / n, b - 1) = pi[x / n] - b + 2
    while (primes[l] > min_clustered_easy_leaf)
    {
      int64_t n = prime * primes[l];
      int64_t xn = x / n;
      assert(xn < isquare(primes[b]));
      int64_t phi_xn = pi[xn] - b + 2;
      int64_t m = prime * primes[b + phi_xn - 1];
      int64_t xm = max(x / m, min_clustered_easy_leaf);
      int64_t l2 = pi[xm];
      S2_result += phi_xn * (l - l2);
      l = l2;
    }

    // Find all sparse easy leaves:
    // x / n <= y and phi(x / n, b - 1) = pi[x / n] - b + 2
    for (; primes[l] > min_sparse_easy_leaf; l--)
    {
      int64_t n = prime * primes[l];
      int64_t xn = x / n;
      assert(xn < isquare(primes[b]));
      S2_result += pi[xn] - b + 2;
    }
  }

  return S2_result;
}

/// Calculate the contribution of the special leaves which require
/// a sieve (in order to reduce the memory usage).
///
int64_t S2_sieve(int64_t x,
                 int64_t y,
                 int64_t z,
                 int64_t c,
                 vector<int32_t>& pi,
                 vector<int32_t>& primes,
                 vector<int32_t>& lpf,
                 vector<int32_t>& mu)
{
  int64_t limit = z + 1;
  int64_t segment_size = next_power_of_2(isqrt(limit));
  int64_t pi_sqrty = pi[isqrt(y)];
  int64_t pi_sqrtz = pi[min(isqrt(z), y)];
  int64_t S2_result = 0;

  BitSieve sieve(segment_size);
  vector<int32_t> counters(segment_size);
  vector<int64_t> next(primes.begin(), primes.end());
  vector<int64_t> phi(primes.size(), 0);

  // Segmented sieve of Eratosthenes
  for (int64_t low = 1; low < limit; low += segment_size)
  {
    // Current segment = interval [low, high[
    int64_t high = min(low + segment_size, limit);
    int64_t b = 2;

    sieve.fill(low, high);

    // phi(y, b) nodes with b <= c do not contribute to S2, so we
    // simply sieve out the multiples of the first c primes
    for (; b <= c; b++)
    {
      int64_t k = next[b];
      for (int64_t prime = primes[b]; k < high; k += prime * 2)
        sieve.unset(k - low);
      next[b] = k;
    }

    // Initialize special tree data structure from sieve
    cnt_finit(sieve, counters, segment_size);

    // For c + 1 <= b <= pi_sqrty
    // Find all special leaves: n = primes[b] * m, with mu[m] != 0 and primes[b] < lpf[m]
    // which satisfy: low <= (x / n) < high
    for (; b <= pi_sqrty; b++)
    {
      int64_t prime = primes[b];
      int64_t min_m = max(x / (prime * high), y / prime);
      int64_t max_m = min(x / (prime * low), y);

      if (prime >= max_m)
        goto next_segment;

      for (int64_t m = max_m; m > min_m; m--)
      {
        if (mu[m] != 0 && prime < lpf[m])
        {
          int64_t n = prime * m;
          int64_t count = cnt_query(counters, (x / n) - low);
          int64_t phi_xn = phi[b] + count;
          S2_result -= mu[m] * phi_xn;
        }
      }

      phi[b] += cnt_query(counters, (high - 1) - low);
      cross_off(prime, low, high, next[b], sieve, counters);
    }

    // For pi_sqrty <= b <= pi_sqrtz
    // Find all hard special leaves: n = primes[b] * primes[l]
    // which satisfy: low <= (x / n) < high
    for (; b <= pi_sqrtz; b++)
    {
      int64_t prime = primes[b];
      int64_t l = pi[min3(x / (prime * low), z / prime, y)];
      int64_t min_hard_leaf = max3(x / (prime * high), y / prime, prime);

      if (prime >= primes[l])
        goto next_segment;

      for (; primes[l] > min_hard_leaf; l--)
      {
        int64_t n = prime * primes[l];
        int64_t xn = x / n;
        int64_t count = cnt_query(counters, xn - low);
        int64_t phi_xn = phi[b] + count;
        S2_result += phi_xn;
      }

      phi[b] += cnt_query(counters, (high - 1) - low);
      cross_off(prime, low, high, next[b], sieve, counters);
    }

    next_segment:;
  }

  return S2_result;
}

/// Calculate the contribution of the special leaves.
/// @pre y > 0 && c > 1
///
int64_t S2(int64_t x,
           int64_t y,
           int64_t z,
           int64_t c,
           vector<int32_t>& primes,
           vector<int32_t>& lpf,
           vector<int32_t>& mu)
{
  vector<int32_t> pi = generate_pi(y);
  int64_t S2_total = 0;

  S2_total += S2_trivial(x, y, z, c, pi, primes);
  S2_total += S2_easy(x, y, z, c, pi, primes);
  S2_total += S2_sieve(x, y, z, c, pi, primes, lpf, mu);

  return S2_total;
}

/// alpha is a tuning factor which should grow like (log(x))^3
/// for the Deleglise-Rivat prime counting algorithm.
///
double compute_alpha(int64_t x)
{
  double d = (double) x;
  double alpha = log(d) * log(d) * log(d) / 1500;
  return in_between(1, alpha, iroot<6>(x));
}

} // namespace

namespace primecount {

/// Calculate the number of primes below x using the
/// Deleglise-Rivat algorithm.
/// Run time: O(x^(2/3) / (log x)^2) operations, O(x^(1/3) * (log x)^3) space.
///
int64_t pi_deleglise_rivat1(int64_t x)
{
  if (x < 2)
    return 0;

  double alpha = compute_alpha(x);
  int64_t y = (int64_t) (alpha * iroot<3>(x));
  int64_t z = x / y;
  int64_t p2 = P2(x, y, 1);

  vector<int32_t> mu = generate_moebius(y);
  vector<int32_t> lpf = generate_least_prime_factors(y);
  vector<int32_t> primes = generate_primes(y);    

  int64_t pi_y = primes.size() - 1;
  int64_t c = min(pi_y, PhiTiny::max_a());
  int64_t s1 = S1(x, y, c, primes[c], lpf , mu, 1);
  int64_t s2 = S2(x, y, z, c, primes, lpf , mu);
  int64_t phi = s1 + s2;
  int64_t sum = phi + pi_y - 1 - p2;

  return sum;
}

} // namespace primecount
