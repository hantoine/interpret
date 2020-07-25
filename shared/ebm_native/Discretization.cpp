// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

// TODO: use noexcept throughout our codebase (exception extern "C" functions) !  The compiler can optimize functions better if it knows there are no exceptions
// TODO: review all the C++ library calls, including things like std::abs and verify that none of them throw exceptions, otherwise use the C versions that provide this guarantee
// TODO: after we've found our splits, generate the best interpretable cut points, then move 1% backwards and forwards to pick the cut points with the lowest numbers of digits that are closest to the original cut points.  Moving 1% either way should be acceptable.  Make it a parameter that can be used from internal python code but we shouldn't export this to the end user since it has limited usefullness

#include <stddef.h> // size_t, ptrdiff_t
#include <limits> // std::numeric_limits
#include <algorithm> // std::sort
#include <cmath> // std::round
#include <vector> // std::vector (used in std::priority_queue)
#include <queue> // std::priority_queue
#include <stdio.h> // snprintf
#include <set> // std::set
#include <string.h> // strchr, memmove

#include "ebm_native.h"
#include "EbmInternal.h"
#include "Logging.h" // EBM_ASSERT & LOG
#include "RandomStream.h"

// Some general definitions:
//  - unsplittable range - a long contiguous series of feature values after sorting that have the same value, 
//    and are therefore not separable by binning.  In order for us to consider the range unsplittable, the number of
//    identical values in the range needs to be longer than the average number of values in a bin.  Example: if
//    we are given 15 bins max, and we have 150 values, then an unsplittable range needs to be 10 values at minimum
//  - SplittingRange - a contiguous series of values after sorting that we can attempt to find SplitPoints within
//    because there are no long series of unsplittable values within the SplittingRange.
//  - SplitPoint - the places where we split one bin to annother
//  - cutPoint - the value we assign to a SplitPoint that separates one bin from annother.  Example:
//    if we had the values [1, 2, 3, 4] and one SplitPoint, a reasonable cutPoint would be 2.5.
//  - cut range - the values between two SplitPoint

constexpr IntEbmType k_randomSeed = 42424242;
constexpr size_t k_SplitExploreDistance = 20;

constexpr unsigned int k_MiddleSplittingRange = 0x0;
constexpr unsigned int k_FirstSplittingRange = 0x1;
constexpr unsigned int k_LastSplittingRange = 0x2;

struct NeighbourJump final {

   NeighbourJump() = default; // preserve our POD status
   ~NeighbourJump() = default; // preserve our POD status
   void * operator new(std::size_t) = delete; // we only use malloc/free in this library
   void operator delete (void *) = delete; // we only use malloc/free in this library

   size_t      m_iStartCur;
   size_t      m_iStartNext;
};
static_assert(std::is_standard_layout<NeighbourJump>::value,
   "We use the struct hack in several places, so disallow non-standard_layout types in general");
static_assert(std::is_trivial<NeighbourJump>::value,
   "We use memcpy in several places, so disallow non-trivial types in general");
static_assert(std::is_pod<NeighbourJump>::value,
   "We use a lot of C constructs, so disallow non-POD types in general");

struct SplittingRange final {

   // we divide the space into long segments of unsplittable equal values separated by spaces where we can put
   // splits, which we call SplittingRanges.  SplittingRanges can have zero or more items.  If they have zero
   // splittable items, then the SplittingRange is just there to separate two unsplittable ranges on both sides.
   // The first and last SplittingRanges are special in that they can either have a long range of unsplittable
   // values on the tail end, or not.  If they have a tail consisting of a long range of unsplitable values, then
   // we'll definetly want to have a split point within the tail SplittingRange, but if there is no unsplitable
   // range on the tail end, then having splits within that range is more optional.
   // 
   // If the first few or last few values are unequal, and followed by an unsplittable range, then
   // we put the unequal values into the unsplittable range IF there are not enough of them to create a split based
   // on our countSamplesPerBinMin value.
   // Example: If countSamplesPerBinMin == 3 and the avg bin size is 5, and the list is 
   // (1, 2, 3, 3, 3, 3, 3 | 4, 5, 6 | 7, 7, 7, 7, 7, 8, 9) -> then the only splittable range is (4, 5, 6)

   SplittingRange() = default; // preserve our POD status
   ~SplittingRange() = default; // preserve our POD status
   void * operator new(std::size_t) = delete; // we only use malloc/free in this library
   void operator delete (void *) = delete; // we only use malloc/free in this library

   // this can be zero if we're sandwitched between two unsplittable ranges, eg: 0, 0, 0, <SplittingRange here> 1, 1, 1
   size_t         m_cSplittableValues;
   FloatEbmType * m_pSplittableValuesFirst;

   size_t         m_cUnsplittableLowValues;
   size_t         m_cUnsplittableHighValues;

   size_t         m_cUnsplittableEitherSideValuesMax;
   size_t         m_cUnsplittableEitherSideValuesMin;

   size_t         m_uniqueRandom;

   size_t         m_cSplitsAssigned;

   FloatEbmType   m_avgSplittableRangeWidthAfterAddingOneSplit;

   unsigned int   m_flags;
};
static_assert(std::is_standard_layout<SplittingRange>::value,
   "We use the struct hack in several places, so disallow non-standard_layout types in general");
static_assert(std::is_trivial<SplittingRange>::value,
   "We use memcpy in several places, so disallow non-trivial types in general");
static_assert(std::is_pod<SplittingRange>::value,
   "We use a lot of C constructs, so disallow non-POD types in general");

constexpr ptrdiff_t k_splitValue = std::numeric_limits<ptrdiff_t>::min();
constexpr FloatEbmType k_noSplitPriority = std::numeric_limits<FloatEbmType>::lowest();
struct SplitPoint final {
   SplitPoint() = default; // preserve our POD status
   ~SplitPoint() = default; // preserve our POD status
   void * operator new(std::size_t) = delete; // we only use malloc/free in this library
   void operator delete (void *) = delete; // we only use malloc/free in this library

   // TODO: using our doubly linked list, we can move them from one place to
   //       another place far away.  We can create a second priority queue that measures how much adding
   //       a split into the open range surrounding a non-materialized aspirational cut.  We can then
   //       then either check when we're clearing cuts what the best range to insert an orphaned cut would be
   //       or we can maintain another priority queue of best cuts to remove and then keep shifting items from
   //       the best to remove to the best to add queue at each step

   SplitPoint *   m_pPrev;
   SplitPoint *   m_pNext;

   // m_cSplitMoveThis is a valid number until we split it.  After splitting we don't 
   // need it's value and we set it to k_splitValue
   ptrdiff_t      m_cSplitMoveThis;

   FloatEbmType   m_iValAspirationalFloat;
   size_t         m_iVal;

   FloatEbmType   m_priority;
   size_t         m_uniqueRandom;

   INLINE_ALWAYS void SetSplit() noexcept {
      m_cSplitMoveThis = k_splitValue;
   }
   INLINE_ALWAYS bool IsSplit() noexcept {
      return k_splitValue == m_cSplitMoveThis;
   }
};
static_assert(std::is_standard_layout<SplitPoint>::value,
   "We use the struct hack in several places, so disallow non-standard_layout types in general");
static_assert(std::is_trivial<SplitPoint>::value,
   "We use memcpy in several places, so disallow non-trivial types in general");
static_assert(std::is_pod<SplitPoint>::value,
   "We use a lot of C constructs, so disallow non-POD types in general");

class CompareSplitPoint final {
public:
   // TODO : check how efficient this is.  Is there a faster way to to this
   INLINE_ALWAYS bool operator() (const SplitPoint * const & lhs, const SplitPoint * const & rhs) const noexcept {
      if(UNLIKELY(lhs->m_priority == rhs->m_priority)) {
         return UNPREDICTABLE(lhs->m_uniqueRandom < rhs->m_uniqueRandom);
      } else {
         return UNPREDICTABLE(lhs->m_priority < rhs->m_priority);
      }
   }
};

INLINE_ALWAYS size_t CalculateRangesMaximizeMin(
   const FloatEbmType iVal, 
   const FloatEbmType cVals, 
   const size_t cRanges
) noexcept {
   // our goal is to, as much as possible, avoid having small ranges at the end.  We don't care as much
   // about having long ranges so much as small range since small ranges allow the boosting algorithm to overfit
   // more easily.  This function takes a 

   EBM_ASSERT(2 <= cRanges); // we require there to be at least one range on the left and one range on the right
   EBM_ASSERT(0 <= iVal);
   EBM_ASSERT(iVal <= cVals);
   // provided FloatEbmType is a double, this shouldn't be able to overflow even if we're on a 128 bit computer
   // if FloatEbmType was a float we might be in trouble for extrememly large ranges and iVal values
   //
   // even with numeric instability, we shouldn't end up with a terrible result here since we only get numeric
   // issues if the number of ranges is huge, and we clip on both the low and high ranges below to handle issues
   // where rounding pushes us a bit over the numeric limits
   const size_t cRangesPlusOne = cRanges + size_t { 1 };
   size_t cLeft = static_cast<size_t>(static_cast<FloatEbmType>(cRangesPlusOne) * iVal / cVals);
   cLeft = std::max(size_t { 1 }, cLeft); // don't allow zero ranges on the low side
   cLeft = std::min(cLeft, cRanges - 1); // don't allow zero ranges on the high side

#ifndef NDEBUG
   
   FloatEbmType avg = std::min(iVal / cLeft, (cVals - iVal) / (cRanges - cLeft));
   if(2 <= cLeft) {
      const size_t denominator = cRanges - cLeft + 1;
      FloatEbmType avgOther = std::min(iVal / (cLeft - 1), (cVals - iVal) / denominator);
      EBM_ASSERT(avgOther <= avg);
   }

   if(2 <= cRanges - cLeft) {
      const size_t denominator = cLeft + 1;
      FloatEbmType avgOther = std::min(iVal / denominator, (cVals - iVal) / (cRanges - cLeft - 1));
      EBM_ASSERT(avgOther <= avg);
   }

#endif

   return cLeft;
}

constexpr static char g_pPrintfForRoundTrip[] = "%+.*" FloatEbmTypePrintf;
constexpr static char g_pPrintfLongInt[] = "%d";


//static FloatEbmType FindClean1eFloat(
//   const int cCharsFloatPrint,
//   char * const pStr,
//   const FloatEbmType low, 
//   const FloatEbmType high, 
//   FloatEbmType val
//) noexcept {
//   // we know that we are very close to 1e[something].  For positive exponents, we have a whole number,
//   // which for smaller values is guaranteed to be exact, but for decimal numbers they will all be inexact
//   // we could therefore be either "+9.99999999999999999e+299" or "+1.00000000000000000e+300"
//   // we just need to check that the number starts with a 1 to be sure that we're the latter
//
//   constexpr int cMantissaTextDigits = std::numeric_limits<FloatEbmType>::max_digits10;
//   unsigned int cIterationsRemaining = 100;
//   do {
//      if(high <= val) {
//         // oh no.  how did this happen.  Oh well, just return the high value, which is guaranteed 
//         // to split low and high
//         break;
//      }
//      const int cCharsWithoutNullTerminator = snprintf(
//         pStr,
//         cCharsFloatPrint,
//         g_pPrintfForRoundTrip,
//         cMantissaTextDigits,
//         val
//      );
//      if(cCharsFloatPrint <= cCharsWithoutNullTerminator) {
//         break;
//      }
//      if(0 == cCharsWithoutNullTerminator) {
//         // check this before trying to access the 2nd item in the array
//         break;
//      }
//      if('1' == pStr[1]) {
//         // do one last check to verify for sure that we're above val in the end!
//         val = low < val ? val : high;
//         return val;
//      }
//
//      val = std::nextafter(val, std::numeric_limits<FloatEbmType>::max());
//      --cIterationsRemaining;
//   } while(0 != cIterationsRemaining);
//   return high;
//}



//static FloatEbmType LoopingMean(const FloatEbmType low, const FloatEbmType high) noexcept {
//   EBM_ASSERT(low < high); // if two numbers were equal, we wouldn't put a cut point between them
//
//   // nan values represent missing, and are filtered out from our data prior to discretization
//   EBM_ASSERT(!std::isnan(low));
//   EBM_ASSERT(!std::isnan(high));
//
//   // -infinity is converted to min_float and +infinity is converted to max_float in our data prior to discretization
//   EBM_ASSERT(!std::isinf(low));
//   EBM_ASSERT(!std::isinf(high));
//
//   FloatEbmType prev;
//   // if low is zero, it could be a huge gulph towards high if we start from the smallest numbers, so start with
//   // high and go downwards.  Also, returning high is valid since we use lower bound inclusive binning
//   FloatEbmType cur = high;
//   constexpr unsigned int cIterationsRemainingMax = 1000000;
//   unsigned int cIterationsRemaining = cIterationsRemainingMax;
//   do {
//      prev = cur;
//      cur = std::nextafter(cur, low);
//      EBM_ASSERT(cur < high);
//      if(cur <= low) {
//         unsigned int cIterationsRemaining = (cIterationsRemainingMax - cIterationsRemainingMax) >> 2;
//         cur = high;
//         do {
//            cur = std::nextafter(cur, low);
//            EBM_ASSERT(cur < high);
//            if(cur <= low) {
//               // this really should not happen
//               return high;
//            }
//            --cIterationsRemaining;
//         } while(0 != cIterationsRemaining);
//      }
//      --cIterationsRemaining;
//   } while(0 != cIterationsRemaining);
//}

static FloatEbmType ArithmeticMean(const FloatEbmType low, const FloatEbmType high) noexcept {
   // nan values represent missing, and are filtered out from our data prior to discretization
   EBM_ASSERT(!std::isnan(low));
   EBM_ASSERT(!std::isnan(high));

   // -infinity is converted to min_float and +infinity is converted to max_float in our data prior to discretization
   EBM_ASSERT(!std::isinf(low));
   EBM_ASSERT(!std::isinf(high));

   EBM_ASSERT(low < high); // if two numbers were equal, we wouldn't put a cut point between them

   static_assert(std::numeric_limits<FloatEbmType>::is_iec559,
      "IEEE 754 gives us certain guarantees for floating point results that we use below");

   // this multiplication before addition format avoid overflows/underflows at the cost of a little more work.
   // IEEE 754 guarantees that 0.5 is representable as 2^(-1), so it has an exact representation.
   // IEEE 754 guarantees that division and addition give exactly rounded results.  Since we're multiplying by 0.5,
   // the internal representation will have the same mantissa, but will decrement the exponent, unless it underflows
   // to zero, or we have a subnormal number, which also works for reasons described below.
   // Fundamentally, the average can be equal to low if high is one epsilon tick above low.  If low is zero and 
   // high is the smallest number, then both numbers divided by two are zero and the average is zero.  
   // If low is the smallest number and high is one tick above that, low will go to zero on the division, but 
   // high will become the smallest number since it uses powers of two, so the avg is again the 
   // low value in this case.
   FloatEbmType avg = low * FloatEbmType { 0.5 } + high * FloatEbmType { 0.5 };

   EBM_ASSERT(!std::isnan(avg)); // in no reasonable implementation should this result in NaN
   
   // in theory, EBM_ASSERT(!std::isinf(avg)); should be ok, but there are bad IEEE 754 implementations that might
   // do the addition before the multiplication, which could result in overflow if done that way.

   // these should be correct in IEEE 754, even with floating point inexactness, due to "correct rounding"
   // in theory, EBM_ASSERT(low <= avg); AND EBM_ASSERT(avg < high); should be ok, but there are bad IEEE 754 
   // implementations that might correctly implement "correct rounding", like the Intel x87 instructions

   // if our result is equal to low, then high should be guaranteed to be the next highest floating point number
   // in theory, EBM_ASSERT(low < avg || low == avg && std::nextafter(low, high) == high); should be ok, but
   // this depends on "correct rounding" which isn't true of all compilers

   if(UNLIKELY(avg <= low)) {
      // This check is required to handle the case where high is one epsilon higher than low, which means the average 
      // could be low (the average could also be higher than low, but we don't need to handle that)
      // In that case, our only option is to make our cut equal to high, since we use lower bound inclusive semantics
      //
      // this check has the added benefit that if we have a compiler/platform that isn't truely IEEE 754 compliant,
      // which is sadly often the case due to double rounding and other issues, then we'd return high, 
      // which is a legal value for us to cut on, and if we have values this close, it's appropriate to just return
      // high instead of doing a more exhaustive examination
      //
      // this check has the added advantage of checking for -infinity
      avg = high;
   }
   if(UNLIKELY(UNLIKELY(high < avg))) {
      // because so many compilers claim to be IEEE 754, but are not, we have this fallback to prevent us
      // from crashing due to unexpected outputs.  high is a legal value to return since we use lower bound 
      // inclusivity.  I don't see how, even in a bad compiler/platform, we'd get a NaN result I'm not including it
      // here.  Some non-compliant platforms might get to +-infinity if they do the addition first then multiply
      // so that's one possibility to be wary about
      //
      // this check has the added advantage of checking for +infinity
      avg = high;
   }
   return avg;
}

static FloatEbmType GeometricMeanSameSign(const FloatEbmType low, const FloatEbmType high) noexcept {
   // nan values represent missing, and are filtered out from our data prior to discretization
   EBM_ASSERT(!std::isnan(low));
   EBM_ASSERT(!std::isnan(high));

   // -infinity is converted to min_float and +infinity is converted to max_float in our data prior to discretization
   EBM_ASSERT(!std::isinf(low));
   EBM_ASSERT(!std::isinf(high));

   EBM_ASSERT(low < high);

   // geometric mean requires the same sign, which we filter our before calling this function
   EBM_ASSERT(low < FloatEbmType { 0 } && high < FloatEbmType { 0 } || 
      FloatEbmType { 0 } <= low && FloatEbmType { 0 } <= high);

   // If both low and high are zero or positive, and high is higher than low, high must be greater than zero.  
   // If both are negative high can't be zero either
   EBM_ASSERT(FloatEbmType { 0 } != high);

   FloatEbmType result;
   if(PREDICTABLE(FloatEbmType { 0 } == low)) {
      // the geometric mean involving a zero would be zero, but that's not really helpful, and since our low
      // value needs to be the zero, we can't return zero since we use lower bound inclusivity, so we need to return
      // something else.  Using the arithmetic mean in this case seems appropriate, and it seems like it would
      // be an "interpretable" result by the user.  To get this we just divide high by 2, since low is zero.

      result = high * FloatEbmType { 0.5 };
      EBM_ASSERT(!std::isnan(result));
      EBM_ASSERT(!std::isinf(result)); // even in a pathalogic processor I don't see how this would get +-infinity
      // high was checked as positive above for 0 == low, and it's hard to see how even a bad floating point
      // implementation would make this negative.
      EBM_ASSERT(FloatEbmType { 0 } <= result);
      if(result <= FloatEbmType { 0 }) {
         // if high is very small, underflow is possible.  In that case high must be very close to zero, so
         // we can just return high.  We can't return zero since with lower bound inclusivity that would put zero
         // in the wrong bin
         result = high;
      }
   } else {
      result = low * high;
      EBM_ASSERT(!std::isnan(result)); // even a pathological implementation shouln't return NaN for this
      EBM_ASSERT(FloatEbmType { 0 } <= result); // even a pathological implementation shouln't return a negative number

      // comparing to max is a good way to check for +infinity without using infinity, which can be problematic on
      // some compilers with some compiler settings.  Using <= helps avoid optimization away because the compiler
      // might assume that nothing is larger than max if it thinks there's no +infinity.  If we reach exactly max, then
      // no harm in computing via exp and log
      if(UNLIKELY(std::numeric_limits<FloatEbmType>::max() <= result)) {
         // if we overflow, which is certainly possible with multiplication, use an exponential approach

         // if low was zero, then the multiplication should not have overflowed to infinity even in a pathalogical implementation
         EBM_ASSERT(FloatEbmType { 0 } < low);

         // in a reasonable world, with both low and high being non-zero, non-nan, non-infinity, and both having been 
         // made to be positive values before calling log, log should return a non-overflowing or non-underflowing 
         // value since all floating point values from -min to +max for floats give us reasonable log values.  
         // Since our logs should average to a number that is between them, the exp value should result in a value 
         // between them in almost all cases, so it shouldn't overflow or underflow either.  BUT, with floating
         // point jitter, we might get any of these scenarios, but this is a real corner case that we can presume
         // is very very very rare.

         // there is no way that the result of log is going to overflow, so we add before multiplying by 0.5 
         // since multiplication is more expensive.

         if(PREDICTABLE(low < FloatEbmType { 0 })) {
            result = -std::exp((std::log(-low) + std::log(-high)) * FloatEbmType { 0.5 });
         } else {
            result = std::exp((std::log(low) + std::log(high)) * FloatEbmType { 0.5 });
         }

         // IEEE 754 doesn't give us a lot of guarantees about log and exp.  They don't have have "correct rounding"
         // guarantees, unlike basic operators, so we could obtain results outside of our bounds, or perhaps
         // even overflow or underflow in a way that would lead to infinities.  I can't think of a way to get NaN
         // but who knows what's happening inside log, which would get NaN for zero and in a bad implementation
         // perhaps might return that for subnormal floats.
         //
         // If our result is not between low and high, then low and high should be very close and we can use the
         // arithmatic mean.  In the spirit of not trusting log and exp, we'll check for bad outputs and 
         // switch to arithmatic mean.  In the case that we have nan or +-infinity, we aren't guaranteed that
         // low and high are close, so we can't really use an approach were we move small epsilon values in 
         // our floats, so the artithmetic mean is really our only viable falllback in that case.
         //
         // Even in the fully compliant IEEE 754 case, result could be equal to low, so we do need to handle that
         // since we can't return the low value given we use lower bound inclusivity for cut points

         // checking the bounds also checks for +-infinity
         if(std::isnan(result) || result <= low || high < result) {
            result = ArithmeticMean(low, high);
         }
      } else {
         // multiplying two positive numbers or two negative numbers should never be negative, 
         // even in a pathalogical floating point implemenation
         EBM_ASSERT(FloatEbmType { 0 } <= result);
         result = std::sqrt(result);
         // even a pathological implementation shouln't return NaN for this
         EBM_ASSERT(!std::isnan(result));
         // no positive number should generate an infinity for sqrt in a reasonable implementation
         EBM_ASSERT(!std::isinf(result));

         if(PREDICTABLE(low < FloatEbmType { 0 })) {
            EBM_ASSERT(high < FloatEbmType { 0 });
            // geometic mean of two negative numbers should be negative
            result = -result;
         }

         // floating point jitter might have put us outside our bounds, but if that were to happen we'd be required
         // to have very very close low and high results.  In that case we can just use the arithmetic mean.
         if(UNLIKELY(UNLIKELY(result <= low) || UNLIKELY(high < result))) {
            result = ArithmeticMean(low, high);
         }
      }
   }
   return result;
}

// VERIFIED
INLINE_ALWAYS constexpr static int CountBase10CharactersAbs(int n) noexcept {
   // this works for negative numbers too
   return int { 0 } == n / int { 10 } ? int { 1 } : int { 1 } + CountBase10CharactersAbs(n / int { 10 });
}

// NOTE: according to the C++ documentation, std::numeric_limits<FloatEbmType>::max_digits10 - 1 digits 
// are required after the period, so for a double, the values would be 
// 17 == std::numeric_limits<FloatEbmType>::max_digits10, 
// and printf format specifier "%.16e", and sample output "3.1415926535897932"
constexpr int cDigitsAfterPeriod = std::numeric_limits<FloatEbmType>::max_digits10 - 1;

// Unfortunately, min_exponent10 doesn't seem to include subnormal numbers, so although it's the true
// minimum exponent in terms of the floating point exponential representation, it isn't the true minimum exponent 
// when considering numbers converted into text.  To counter this, we add 1 extra digit.  For double numbers
// the largest exponent (308), smallest exponent for normal (-307), and the smallest exponent for subnormal (-324) 
// all have 3 digits, but in the more general scenario we might go from N to N+1 digits, but I think
// it's really unlikely to go from N to N+2, since in the simplest case that would be a factor of 10 in the 
// exponential term (if the low number was almost N and the high number was just a bit above N+2), and 
// subnormal numbers shouldn't increase the exponent by that much ever.
constexpr int cExponentMaxTextDigits = CountBase10CharactersAbs(std::numeric_limits<FloatEbmType>::max_exponent10);
constexpr int cExponentMinTextDigits = CountBase10CharactersAbs(std::numeric_limits<FloatEbmType>::min_exponent10) + 1;
constexpr int cExponentTextDigits =
cExponentMaxTextDigits < cExponentMinTextDigits ? cExponentMinTextDigits : cExponentMaxTextDigits;

// example: "+9.1234567890123456e+300" (this is when 16 == cDigitsAfterPeriod, the value for doubles)
// 3 characters for "+9."
// cDigitsAfterPeriod characters for the mantissa text
// 2 characters for "e+"
// cExponentTextDigits characters for the exponent text
// 1 characters for null terminator
constexpr int iExp = 3 + cDigitsAfterPeriod;
constexpr int cCharsFloatPrint = iExp + 2 + cExponentTextDigits + 1;

static bool FloatToString(const FloatEbmType val, char * const str) noexcept {
   // NOTE: str must be a buffer with cCharsFloatPrint characters available 
   //       (cCharsFloatPrint includes a character for the null terminator)

   // the C++ standard is pretty good about harmonizing the "e" format.  There is some openess to what happens
   // in the exponent (2 or 3 digits with or without the leading sign character, etc).  If there is ever any
   // implementation observed that differs, this function should convert all formats to a common standard
   // we use string manipulation to find interpretable cut points, so we need all strings to have a common format

   // snprintf says to use the buffer size for the "n" term, but in alternate unicode versions it says # of characters
   // with the null terminator as one of the characters, so a string of 5 characters plus a null terminator would be 6.
   // For char strings, the number of bytes and the number of characters is the same.  I use number of characters for 
   // future-proofing the n term to unicode versions, so n-1 characters other than the null terminator can fill 
   // the buffer.  According to the docs, snprintf returns the number of characters that would have been written MINUS 
   // the null terminator.

   const int cCharsWithoutNullTerminator = snprintf(
      str,
      cCharsFloatPrint,
      g_pPrintfForRoundTrip,
      cDigitsAfterPeriod,
      val
   );
   if(cCharsWithoutNullTerminator <= iExp || cCharsFloatPrint <= cCharsWithoutNullTerminator) {
      // cCharsWithoutNullTerminator <= iExp checks for both negative values returned and strings that are too short
      return true;
   }
   char ch;
   ch = str[0];
   if('+' != ch && '-' != ch) {
      return true;
   }
   ch = str[1];
   if(ch < '0' || '9' < ch) {
      return true;
   }
   ch = str[2];
   if('.' != ch) {
      return true;
   }
   char * pch = &str[3];
   char * pE = &str[iExp];
   do {
      ch = *pch;
      if(ch < '0' || '9' < ch) {
         return true;
      }
      ++pch;
   } while(pch != pE);
   ch = *pch;
   if('e' != ch && 'E' != ch) {
      return true;
   }

   // use strtol instead of atol incase we have a bad input.  atol has undefined behavior if the
   // number isn't representable as an int.  strtol returns a 0 with bad inputs, or LONG_MAX, or LONG_MIN, 
   // on overflow or underflow.  The C++ standard makes clear though that on error strtol sets endptr
   // equal to str, so we can use that

   ++pch;
   char * endptr = pch;
   strtol(pch, &endptr, 10);
   if(endptr == pch) {
      return true;
   }
   return false;
}

INLINE_RELEASE static long GetExponent(const char * str) noexcept {
   str = &str[iExp + 1];
   // we previously checked that this converted to a long in FloatToString
   return strtol(str, nullptr, 10);
}

INLINE_ALWAYS static int IntToString(const int val, char * const str, const int index) noexcept {
   // snprintf says to use the buffer size for the "n" term, but in alternate unicode versions it says # of characters
   // with the null terminator as one of the characters, so a string of 5 characters plus a null terminator would be 6.
   // For char strings, the number of bytes and the number of characters is the same.  I use number of characters for 
   // future-proofing the n term to unicode versions, so n-1 characters other than the null terminator can fill 
   // the buffer.  According to the docs, snprintf returns the number of characters that would have been written MINUS 
   // the null terminator.

   int cCharsWithoutNullTerminator = snprintf(
      &str[index],
      cCharsFloatPrint - index,
      g_pPrintfLongInt,
      val
   );
   cCharsWithoutNullTerminator = UNLIKELY(cCharsFloatPrint - index <= cCharsWithoutNullTerminator) ? -1 :
      cCharsWithoutNullTerminator;
   return cCharsWithoutNullTerminator;
}

INLINE_ALWAYS static FloatEbmType StringToFloat(const char * const str) noexcept {
   // we only convert str values that we've verified to conform, OR chopped versions of these which we know to be legal
   // If the chopped representations underflow (possible on chopping to lower) or 
   // overflow (possible when we increment from the lower chopped value), then strtod gives 
   // us enough information to convert these

   static_assert(std::is_same<FloatEbmType, double>::value,
      "FloatEbmType must be double, otherwise use something other than strtod");

   // the documentation says that if we have an underflow or overflow, strtod returns us +-HUGE_VAL, which is
   // +-infinity for at least some implementations.  We can't really take a ratio from those numbers, so convert
   // this to the lowest and max values

   FloatEbmType val = strtod(str, nullptr);

   // this is a check for -infinity/-HUGE_VAL, without the -infinity value since some compilers make that illegal
   // even so far as to make isinf always FALSE with some compiler flags
   // include the equals case so that the compiler is less likely to optimize that out
   val = val <= std::numeric_limits<FloatEbmType>::lowest() ? std::numeric_limits<FloatEbmType>::lowest() : val;
   // this is a check for +infinity/HUGE_VAL, without the +infinity value since some compilers make that illegal
   // even so far as to make isinf always FALSE with some compiler flags
   // include the equals case so that the compiler is less likely to optimize that out
   val = std::numeric_limits<FloatEbmType>::max() <= val ? std::numeric_limits<FloatEbmType>::max() : val;

   return val;
}

static FloatEbmType StringToFloatWithFixup(const char * const str, int iIdenticalCharsRequired) noexcept {
   char strRehydrate[cCharsFloatPrint];
   FloatEbmType ret = StringToFloat(str);
   if(FloatToString(ret, strRehydrate)) {
      return ret;
   }

   if(0 == memcmp(str, strRehydrate, iIdenticalCharsRequired)) {
      return ret;
   }

   // according to the C++ docs, nextafter won't exceed the to parameter, so we don't have to worry about this
   // generating infinities
   ret = std::nextafter(ret, '-' == str[0] ? std::numeric_limits<FloatEbmType>::lowest() :
      std::numeric_limits<FloatEbmType>::max());

   return ret;
}




static void StringToFloatChopped(
   const char * const pStr,
   int iTruncateMantissaTextDigitsAfter,
   FloatEbmType & lowChop,
   FloatEbmType & highChop
) noexcept {
   EBM_ASSERT(nullptr != pStr);
   // don't pass us a non-truncated string, since we should handle anything that gets to that level differently
   EBM_ASSERT(iTruncateMantissaTextDigitsAfter <= cDigitsAfterPeriod);

   char strTruncated[cCharsFloatPrint];

   iTruncateMantissaTextDigitsAfter = 0 < iTruncateMantissaTextDigitsAfter ?
      1 + iTruncateMantissaTextDigitsAfter : iTruncateMantissaTextDigitsAfter;

   iTruncateMantissaTextDigitsAfter += 2; // add one for the sign character and one for the first character

   memcpy(strTruncated, pStr, iTruncateMantissaTextDigitsAfter);
   strcpy(&strTruncated[iTruncateMantissaTextDigitsAfter], &pStr[iExp]);

   if('-' == pStr[0]) {
      highChop = StringToFloatWithFixup(strTruncated, iTruncateMantissaTextDigitsAfter);
   } else {
      lowChop = StringToFloatWithFixup(strTruncated, iTruncateMantissaTextDigitsAfter);
   }

   char * pIncrement = &strTruncated[iTruncateMantissaTextDigitsAfter - 1];
   char ch;
   if(2 == iTruncateMantissaTextDigitsAfter) {
      goto start_at_top;
   }
   while(true) {
      ch = *pIncrement;
      if('.' == ch) {
         --pIncrement;
      start_at_top:;
         ch = *pIncrement;
         if('9' == ch) {
            // oh, great.  now we need to increment our exponential
            *pIncrement = '1';
            *(pIncrement + 1) = 'e';
            int exponent = GetExponent(pStr) + 1;
            IntToString(exponent, strTruncated, 3);
         } else {
            *pIncrement = ch + 1;
         }
         break;
      }
      if('9' == ch) {
         *pIncrement = '0';
      } else {
         *pIncrement = ch + 1;
         break;
      }
      --pIncrement;
   }
   if('-' == pStr[0]) {
      lowChop = StringToFloatWithFixup(strTruncated, iTruncateMantissaTextDigitsAfter);
   } else {
      highChop = StringToFloatWithFixup(strTruncated, iTruncateMantissaTextDigitsAfter);
   }
   return;
}

INLINE_RELEASE static FloatEbmType GetInterpretableCutPointFloat(
   const FloatEbmType low, 
   const FloatEbmType high
) noexcept {
   // TODO : add logs here when we find a condition we didn't think was possible, but that occurs

   // nan values represent missing, and are filtered out from our data prior to discretization
   EBM_ASSERT(!std::isnan(low));
   EBM_ASSERT(!std::isnan(high));

   // -infinity is converted to min_float and +infinity is converted to max_float in our data prior to discretization
   EBM_ASSERT(!std::isinf(low));
   EBM_ASSERT(!std::isinf(high));

   EBM_ASSERT(low < high); // if two numbers were equal, we wouldn't put a cut point between them

   // if our numbers pass the asserts above, all combinations of low and high values can get a legal cut point, 
   // since we can always return the high value given that our binning is lower bound inclusive

   if(low < FloatEbmType { 0 } && FloatEbmType { 0 } <= high) {
      // if low is negative and high is zero or positive, a natural cut point is zero.  Also, this solves the issue
      // that we can't take the geometric mean of mixed positive/negative numbers.  This works since we use 
      // lower bound inclusivity, so a cut point of 0 will include the number 0 in the upper bin.  Normally we try 
      // to avoid putting a cut directly on one of the numbers, but in the case of zero it seems appropriate.
      return FloatEbmType { 0 };
   }

   char strLow[cCharsFloatPrint];
   char strHigh[cCharsFloatPrint];
   char strAvg[cCharsFloatPrint];

   if(FloatToString(low, strLow)) {
      return high;
   }
   if(FloatToString(high, strHigh)) {
      return high;
   }
   int lowExp = GetExponent(strLow);
   int highExp = GetExponent(strHigh);

   EBM_ASSERT(lowExp <= highExp);

   FloatEbmType avg;
   if(lowExp + 2 <= highExp) {
      avg = GeometricMeanSameSign(low, high);
   } else {
      avg = ArithmeticMean(low, high);
   }
   EBM_ASSERT(!std::isnan(avg));
   EBM_ASSERT(!std::isinf(avg));
   EBM_ASSERT(low < avg);
   EBM_ASSERT(avg <= high);

   if(FloatToString(avg, strAvg)) {
      return high;
   }

   FloatEbmType lowChop;
   FloatEbmType highChop;

   if(lowExp + 2 <= highExp) {
      EBM_ASSERT(low < avg);
      EBM_ASSERT(avg < high);

      StringToFloatChopped(strAvg, 0, lowChop, highChop);

      // TODO : handle low == 0.  We probalby want to invert these divisions, or change them to multiplications
      const FloatEbmType highRatio = high / lowChop;
      const FloatEbmType lowRatio = highChop / low;

      if(highRatio < lowRatio) {
         return highChop;
      } else {
         return lowChop;
      }
   } else {
      for(int i = 0; i < cDigitsAfterPeriod; ++i) {
         FloatEbmType lowLow;
         FloatEbmType lowHigh;
         FloatEbmType avgLow;
         FloatEbmType avgHigh;
         FloatEbmType highLow;
         FloatEbmType highHigh;

         StringToFloatChopped(strLow, i, lowLow, lowHigh);
         StringToFloatChopped(strAvg, i, avgLow, avgHigh);
         StringToFloatChopped(strHigh, i, highLow, highHigh);

         if(lowHigh < avgLow && avgLow < highLow && low < avgLow && avgLow <= high) {
            // avgLow is a possibility
            if(lowHigh < avgHigh && avgHigh < highLow && low < avgHigh && avgHigh <= high) {
               // avgHigh is a possibility
               FloatEbmType lowDistance = high - avgLow;
               FloatEbmType highDistance = avgHigh - low;
               if(highDistance < lowDistance) {
                  return avgHigh;
               }
            }
            return avgLow;
         } else {
            if(lowHigh < avgHigh && avgHigh < highLow && low < avgHigh && avgHigh <= high) {
               // avgHigh is a possibility
               return avgHigh;
            }
         }
      }

      // this was already checked to be valid
      return avg;
   }
}

INLINE_RELEASE static void IronSplits() noexcept {
   // - TODO: POST-HEALING
   //   - after fitting these, we might want to jigger the final results.  We would do this by finding the smallest 
   //     section and trying to expand it either way.  Each side we'd push it only enough to make things better.
   //     If we find that we can make a push that improves things, then we take that.  We'd need a priority queue to 
   //     indicate the smallest sections
   //

   // TODO: here we should try to even out our final result in case there are large scale differences in size
   //       that we can address by pushing our existing cuts arround by small amounts

   // TODO: one option would be to try pushing inwards from the outer regions.  Take a window size that we think 
   //       is good and try pushing inwards simultaneously from both sides such that no window is smaller than that
   //       size and keep examining the result for best squared error fit while it's happening.  We might end up
   //       squeezing smaller sized ranges to the center, which might actually be good since overfitting is probably
   //       happening more on the edges

   // TODO: we might try making a sliding window of 5 cuts.  Delete the 5 cuts in between two boundaries and try
   //       5! * 2^5 (examine all orderings of cuts and all left/right choices).  Move from both edges simultaneously
   //       to the center and repeat several times.  This has the advantage that all examinations will have their
   //       endpoints fixed while being examined, but the end points will themselves be examined as the window
   //       moves along

}

constexpr unsigned int k_cNeighbourExploreDistanceMax = 5;
typedef int signed_neighbour_type;
constexpr size_t k_illegalIndex = std::numeric_limits<size_t>::max();

INLINE_RELEASE static void CalculatePriority(
   const FloatEbmType iValLowerFloat,
   const FloatEbmType iValHigherFloat,
   SplitPoint * const pSplitCur
) noexcept {
   EBM_ASSERT(!pSplitCur->IsSplit());

   // if the m_iVal value was set to k_illegalIndex, then there are no legal splits, 
   // so leave it with the most terrible possible priority
   if(k_illegalIndex == pSplitCur->m_iVal) {
      pSplitCur->m_priority = k_noSplitPriority;
      return;
   }

   EBM_ASSERT(iValLowerFloat <= pSplitCur->m_iVal);
   EBM_ASSERT(iValLowerFloat < pSplitCur->m_iValAspirationalFloat);

   const FloatEbmType lowerPriority = std::abs(FloatEbmType { 1 } -
      ((pSplitCur->m_iVal - iValLowerFloat) / (pSplitCur->m_iValAspirationalFloat - iValLowerFloat)));

   EBM_ASSERT(pSplitCur->m_iVal <= iValHigherFloat);
   EBM_ASSERT(pSplitCur->m_iValAspirationalFloat < iValHigherFloat);

   const FloatEbmType higherPriority = std::abs(FloatEbmType { 1 } -
      ((iValHigherFloat - pSplitCur->m_iVal) / (iValHigherFloat - pSplitCur->m_iValAspirationalFloat)));

   const FloatEbmType priority = std::max(lowerPriority, higherPriority);

   pSplitCur->m_priority = priority;
}

//WARNING_PUSH
//WARNING_DISABLE_SIZE_PROMOTION_AFTER_OPERATOR
//INLINE_RELEASE static FloatEbmType ScoreOneNeighbourhoodSide(
//   signed_neighbour_type choices,
//
//   const size_t cMinimumSamplesPerBin,
//   const size_t iValuesStart,
//   const size_t cSplittableItems,
//   const NeighbourJump * const aNeighbourJumps,
//
//   const ptrdiff_t direction,
//
//   const size_t cRanges,
//   const size_t iVal,
//   const FloatEbmType iValBoundary,
//
//   const FloatEbmType idealWidth
//) noexcept {
//   // this function's purpose is really just to decide if we should move lower or higher for any given cut
//   // The best choice will allow us to place cuts at equidistant spots until the point in the horizon that we're
//   // willing to examine.  As such, we want to drop our potential cuts down at equal intervals and not chain the
//   // decisions by putting down the first cut then putting the next one based on the first.  If we chained them
//   // then we wouldn't really be able to compare the chain where we select the shortest option each time against
//   // the longest, since the longer one will be elongated and might fall on a distant long stretch that puts the outer
//   // edge of our nearby boundary farther than the boundary on the option with the shortest always chosen.
//
//   EBM_ASSERT(0 <= choices);
//
//   EBM_ASSERT(1 <= cMinimumSamplesPerBin);
//   EBM_ASSERT(2 * cMinimumSamplesPerBin <= cSplittableItems);
//   EBM_ASSERT(nullptr != aNeighbourJumps);
//
//   EBM_ASSERT(ptrdiff_t { -1 } == direction || ptrdiff_t { 1 } == direction);
//
//   EBM_ASSERT(1 <= cRanges);
//
//   EBM_ASSERT(ptrdiff_t { -1 } == direction && iValBoundary <= static_cast<FloatEbmType>(iVal) || 
//      ptrdiff_t { 1 } == direction && static_cast<FloatEbmType>(iVal) <= iValBoundary);
//
//   EBM_ASSERT(0 < idealWidth);
//
//   size_t iPrev = iVal;
//
//   const FloatEbmType step = iValBoundary - iVal;
//   FloatEbmType badness = 0;
//   for(unsigned int i = 1; i <= k_cNeighbourExploreDistanceMax; ++i) {
//      const FloatEbmType iAspirationCutFloat = iVal + static_cast<FloatEbmType>(i) * step;
//      ptrdiff_t iCut = static_cast<ptrdiff_t>(iAspirationCutFloat);
//      iCut = std::max(ptrdiff_t { 0 }, iCut);
//      iCut = std::min(iCut, static_cast<ptrdiff_t>(cSplittableItems - size_t { 1 }));
//
//      const NeighbourJump * const pNeighbourJump = &aNeighbourJumps[iValuesStart + static_cast<size_t>(iCut)];
//      EBM_ASSERT(iValuesStart <= pNeighbourJump->m_iStartCur);
//      EBM_ASSERT(iValuesStart < pNeighbourJump->m_iStartNext);
//      EBM_ASSERT(pNeighbourJump->m_iStartCur < pNeighbourJump->m_iStartNext);
//      EBM_ASSERT(pNeighbourJump->m_iStartNext <= iValuesStart + cSplittableItems);
//      EBM_ASSERT(pNeighbourJump->m_iStartCur < iValuesStart + cSplittableItems);
//
//      size_t iCur = *(signed_neighbour_type { 0 } == UNPREDICTABLE(choices & signed_neighbour_type { 1 }) ?
//         &pNeighbourJump->m_iStartCur : &pNeighbourJump->m_iStartNext);
//
//      // because we try both sides of each range, it's possible that our previous index chose the lower values
//      // and our current one chooses the higher value, and we then travel "backwards" 
//      ptrdiff_t diffPrev;
//      if(PREDICTABLE(direction < 0)) {
//         diffPrev = iPrev - iCur;
//      } else {
//         diffPrev = iCur - iPrev;
//      }
//
//      FloatEbmType diffFromIdeal;
//      if(UNLIKELY(diffPrev < static_cast<ptrdiff_t>(cMinimumSamplesPerBin))) {
//         if(PREDICTABLE(0 < diffPrev)) {
//            iPrev = iCur;
//         }
//
//         // strongly penalize not being able to make a range by making the penalty equal to complete elimination
//         // this pentaly is chosen to be so large that even if all the rest of our cuts lead to zero length ranges
//         // the loss of this single range would exceed that
//         diffFromIdeal = idealWidth * (k_cNeighbourExploreDistanceMax + static_cast<unsigned int>(1));
//      } else {
//         EBM_ASSERT(0 < diffPrev);
//         iPrev = iCur;
//
//         diffFromIdeal = idealWidth - static_cast<FloatEbmType>(diffPrev);
//         // only penalize being too small
//         diffFromIdeal = diffFromIdeal < FloatEbmType { 0 } ? FloatEbmType { 0 } : diffFromIdeal;
//      }
//
//      // square the error so that we penalize being far away more than being in the range
//      diffFromIdeal *= diffFromIdeal;
//
//      badness += diffFromIdeal;
//      EBM_ASSERT(FloatEbmType { 0 } <= badness);
//
//      choices >>= 1; // this operation should be non-implementation defined since 0 <= choices
//   }
//
//   FloatEbmType remainingDistance;
//   if(PREDICTABLE(direction < 0)) {
//      // if we have a long-ish range of equal values, it can push our iPrev past our boundary
//      remainingDistance = static_cast<FloatEbmType>(iPrev) - iValBoundary;
//   } else {
//      // if we have a long-ish range of equal values, it can push our iPrev past our boundary
//      remainingDistance = iValBoundary - static_cast<FloatEbmType>(iPrev);
//   }
//
//   if(PREDICTABLE(k_cNeighbourExploreDistanceMax < cRanges)) {
//      const size_t cRangesRemaining = cRanges - k_cNeighbourExploreDistanceMax;
//      const FloatEbmType remainingDistancePerRange = remainingDistance / static_cast<FloatEbmType>(cRangesRemaining);
//
//      FloatEbmType diffFromIdeal;
//      diffFromIdeal = idealWidth - remainingDistancePerRange;
//      // only penalize being too small
//      diffFromIdeal = diffFromIdeal < FloatEbmType { 0 } ? FloatEbmType { 0 } : diffFromIdeal;
//
//      // square the error so that we penalize being far away more than being in the range
//      diffFromIdeal *= diffFromIdeal;
//
//      badness += diffFromIdeal * cRangesRemaining;
//   }
//
//   return badness;
//}
//WARNING_POP

static void BuildNeighbourhoodPlan(
   const size_t cMinimumSamplesPerBin,
   const size_t iValuesStart,
   const size_t cSplittableItems,
   const NeighbourJump * const aNeighbourJumps,

   const size_t cRangesLow,
   const size_t iValLow,
   const FloatEbmType iValAspirationalLowFloat,

   const size_t cRangesHigh,
   const size_t iValHigh,
   const FloatEbmType iValAspirationalHighFloat,

   // NOTE: m_iValAspirationalFloat is the only value that we can count on from pSplitCur to reflect our current
   // situation.  All other fields are being overwritten as we nuke them, or they were uninitialized
   SplitPoint * const pSplitCur
) noexcept {
   EBM_ASSERT(1 <= cMinimumSamplesPerBin);
   EBM_ASSERT(2 * cMinimumSamplesPerBin <= cSplittableItems);
   EBM_ASSERT(nullptr != aNeighbourJumps);
   
   EBM_ASSERT(1 <= cRangesLow);
   EBM_ASSERT(1 <= cRangesHigh);

   EBM_ASSERT(k_illegalIndex == iValLow || 
      std::abs(iValAspirationalLowFloat - static_cast<FloatEbmType>(iValLow)) < FloatEbmType { 0.001 });
   EBM_ASSERT(k_illegalIndex == iValHigh || 
      std::abs(iValAspirationalHighFloat - static_cast<FloatEbmType>(iValHigh)) < FloatEbmType { 0.001 });

   EBM_ASSERT(nullptr != pSplitCur);

   EBM_ASSERT(FloatEbmType { 0 } <= pSplitCur->m_iValAspirationalFloat); // I suppose it could be zero if we had huge numbers and we rounded down
   EBM_ASSERT(pSplitCur->m_iValAspirationalFloat <= 
      static_cast<FloatEbmType>(cSplittableItems) + FloatEbmType { 0.001 });

   // TODO TODO TODO:
   // implement this alternate algorithm:
   // - IDEA: instead of exploring the combinatorial space, maybe instead we take a measured step to the left (based
   //         on the remaining space, then choose either the left or right direction (based on the bits) and eliminate
   //         dead end choices that lead to less than the minimum sizes, then re-calculate the remaining space, and make
   //         the next sequential hop.  This is non-combinatorial, can still use our bits, but we'll end up with different
   //         sized windows.  We convert the end to 
   // - the problem with our existing algorithm of choosing the points independnetly is that the neighbouring ranges 
   //   might need to be fairly different sizes.  For sample say there is a long run at the start and smaller 
   //   ones at the ends.  
   // - we should think of the problem as choosing the next 5 splits on the left and right for each of the lower and
   //   higher choices.  We should consider our window of the larger N, so let's say 50 items, but we're going to
   //   restrict our next few choices to the next 5 items in any order.  For each of those items we're allowed to
   //   choose either the higher or lower choices.
   // - if we allow our next 5 items to be chosen in any order, then we have 5! choices, and we have 2 sides to each
   //   choice so we have 2^5 side options.  Our last item is special in that we don't really know if we'll choose
   //   the higher or lower choice due to constraints on the other side.  In fact, we might not choose either of them
   //   due to constraints on the other side.  Perhaps we want to game a few options like dividing the region up into
   //   10 different ending points (they could be all over the place if there isn't a long range of values there)
   //   and taking the geometric mean which would ensure us the best average choice depending on how those values are
   //   chosen.  I like the idea of spraying the end point with various choices to see what the totality of options are
   // - for the points beyond our end, if we don't hit a hard split boundary, we should divide them up by the remaining
   //   split points and use whatever scoring metric we're using for the other, but using floatig aspirational cuts
   // - our metric probably shouldn't just consider penalizing small ranges otherwise our algorithm will just select
   //   for all ranges that are big.  We should probably do something like, scale positive and negative distances
   //   by some factor first to weight the smaller ones more, then square them, and use that
   // - we might consider taking the second or third best score of all the permutations on the left or right because 
   //   we are not guaranteed to get the best one.  We are guaranteed to get our split if we're selected from the
   //   priority queue, but our ultimate cut plan may not occur as we had envisioned, so variety is important, and
   //   recognizing that if one side has many options, but the other side has much fewer options, we may want to choose
   //   the side with more options even if the best options are better than the other side.  We aim here for safety
   //   instead of the best result



   // TODO TODO TODO TODO TODO TODO TODO TODO 
   //!!!!
   //Inside BuildNeighbourhoodPlan, it's possible to have a super-improbable condition (but maybe with constructed bad data)
   //that we try and find where our low and high choices are, and one or BOTH are outside our visibility window.This would only happen
   //if someone started with big ranges, but then squeezed the ranges close to the minimum and made the possible ranges larger than 50 minimum distances
   //If one boundary is outside our visibility window, then we shouldn't consider it
   //If both boundaries are outside our visibility window, then we're in trouble, since we don't have a place to materialize out cut
   //Even if we pre - scan the range once we've taken out a cut, we move our window lower and higher and it's possible that a cut on one of the sides
   //will get into a scenario where it has no legal cuts(perhaps the left cut doesn't have the minimum and the right is outside of the visibility window)
   //So, we need to handle this somehow.
   //Further complicating this is, if we self - remove ourselves, we screw up the lower and higher counts of our parent since a hole opened up
   //So, we should probably not deleted ourselves.
   //Perhaps we can set our priority to the most negative number so that we aren't selected ever. and our algorithm ends when we pull off a cut point with that number since it means all remaining cuts are impossible
   //but we set the priority in a later function, so we need to somehow signal that we have no cuts, and then set the priority afterwards ? ?
   //Even if we have no legal cuts because our lower bound doesn't meet the minimum and our upper bound is outside the visibility window, we might in the future become cutable, so we shouldn't delete ourselves
   //If we initiazlie the priority to a value (zero presumably), we can set it to -max_value to signal that there are no legal cuts and then we won't call the prioritize function when that is observed


   // if the splits to the immediate lower and higher positions are both materialized AND if we have a long range
   // in the middle that only allows our lower and higher splits to be less than cMinimumSamplesPerBin items
   // away from the boundaries, then no splits will be possible and this function is allowed to delete pSplitCur
   // from consideration (returning true so that the SplitPoint isn't inserted into the priority queue).
   // We never delete a SplitPoint though outside of this condition though since if there are 2 split points within
   // a range, then neither is guranteed to be the true split since the other one could be selected and our split
   // point moved.  Deleting/moving Split points is a kind of materialization, so don't do that.

   // It's tempting to materialize our cut if both of our neighbours are materialized, since our boundaries won't
   // change.  In the future though we might someday move counts of ranges arround, and perhaps a split point will
   // be moved into our range before we make our actual cut.  We should probably actually give a priority of zero
   // to any SplitPoint that has materialized split points to either side so that it doesn't get materialized until
   // the end



   // TODO: we could take an alternate approach here and look at N lower and N higher points based on our ideal 
   //       division width, and get the square distance between the ideal cut points and their nearest real 
   //       cuttable points.  Try this out!

   // TODO: we could run a pre-step in our caller that would, at the time the bounds are determined, jump from
   //       both the lower and upper bounds by the minimum distances and then assign a count to each SplitPoint
   //       that indicates how many ranges there are lower and higher for each of the lower and higher cut choices
   //       this could be done once per entire large window, so each SplitPoint wouldn't need to do this.
   //       At the minimum this 

   // TODO: another idea would be to slide a window through all potential cut points and measure how well we can
   //       go from a cut on any particular point to any series of cuts on either side.  If one place has a cut
   //       cut point that meshes well with others, but nearby points are much worse, we probably want to lock that
   //       option down.  I strikes me that this method would work really well for the interior regions that are
   //       far from the edges.  Perhaps we want to mix this method with our other top down method so that we choose
   //       one cut top down, then one cut bottom up, etc.  Many possible options here

   // TODO: no matter what, I think we'll need to have a way to re-balance the cut points from one open region to
   //       annother, since otherwise with lumpy ranges we'll end up with too many somewhere and too few in other
   //       places



   EBM_ASSERT(FloatEbmType { 0 } <= pSplitCur->m_iValAspirationalFloat);
   size_t iValAspirationalCur = static_cast<size_t>(pSplitCur->m_iValAspirationalFloat);
   if(UNLIKELY(cSplittableItems <= iValAspirationalCur)) {
      // handle the very very unlikely situation where m_iAspirationalFloat rounds up to 
      // cSplittableItems due to floating point issues
      iValAspirationalCur = cSplittableItems - 1;
   }

   const NeighbourJump * const pNeighbourJump = &aNeighbourJumps[iValuesStart + iValAspirationalCur];
   EBM_ASSERT(iValuesStart <= pNeighbourJump->m_iStartCur);
   EBM_ASSERT(iValuesStart < pNeighbourJump->m_iStartNext);
   EBM_ASSERT(pNeighbourJump->m_iStartCur < pNeighbourJump->m_iStartNext);
   EBM_ASSERT(pNeighbourJump->m_iStartNext <= iValuesStart + cSplittableItems);
   EBM_ASSERT(pNeighbourJump->m_iStartCur < iValuesStart + cSplittableItems);

   const size_t iValLowChoice = pNeighbourJump->m_iStartCur - iValuesStart;
   const size_t iValHighChoice = pNeighbourJump->m_iStartNext - iValuesStart;
   EBM_ASSERT(iValLowChoice <= iValAspirationalCur);
   EBM_ASSERT(iValAspirationalCur <= iValHighChoice);

   FloatEbmType totalDistance;
   FloatEbmType distanceLowFloat;
   bool bCanSplitLow;
   FloatEbmType distanceHighFloat;
   bool bCanSplitHigh;

   // this can underflow, but that's legal for unsigned numbers.  We check for underflow below
   const size_t lowMinusMin = iValLowChoice - cMinimumSamplesPerBin;

   if(k_illegalIndex == iValLow) {
      totalDistance = iValAspirationalHighFloat - iValAspirationalLowFloat;
      distanceLowFloat = static_cast<FloatEbmType>(iValLowChoice) - iValAspirationalLowFloat;
      distanceHighFloat = static_cast<FloatEbmType>(iValHighChoice) - iValAspirationalLowFloat;
      bCanSplitLow = cMinimumSamplesPerBin <= iValLowChoice && 
         iValAspirationalLowFloat <= static_cast<FloatEbmType>(lowMinusMin);
      if(k_illegalIndex == iValHigh) {
         // this can overflow, but that's legal for unsigned numbers.  We check for overflow below
         const size_t highPlusMin = cMinimumSamplesPerBin + iValHighChoice;

         // if the add of cMinimumSamplesPerBin iValHighChoice is an overflow, then the sum is not representable
         // inside memory, and thus we can conclude we're too close to the upper boundary, which must be
         // representable within memory as cSplittableItems items
         bCanSplitHigh = !IsAddError(cMinimumSamplesPerBin, iValHighChoice) &&
            static_cast<FloatEbmType>(highPlusMin) <= iValAspirationalHighFloat;
      } else {
         bCanSplitHigh = iValHighChoice <= iValHigh && cMinimumSamplesPerBin <= iValHigh - iValHighChoice;
      }
   } else {
      distanceLowFloat = static_cast<FloatEbmType>(iValLowChoice - iValLow);
      distanceHighFloat = static_cast<FloatEbmType>(iValHighChoice - iValLow);
      EBM_ASSERT(iValLow <= iValLowChoice); // our boundary is materialized, so we can't be lower
      bCanSplitLow = cMinimumSamplesPerBin <= iValLowChoice && iValLow <= lowMinusMin;
      if(k_illegalIndex == iValHigh) {
         totalDistance = iValAspirationalHighFloat - iValAspirationalLowFloat;
         // this can overflow, but that's legal for unsigned numbers.  We check for overflow below
         const size_t highPlusMin = cMinimumSamplesPerBin + iValHighChoice;
         bCanSplitHigh = !IsAddError(cMinimumSamplesPerBin, iValHighChoice) &&
            static_cast<FloatEbmType>(highPlusMin) <= iValAspirationalHighFloat;
      } else {
         // reduce floating point noise when we have have exact distances
         totalDistance = static_cast<FloatEbmType>(iValHigh - iValLow);
         bCanSplitHigh = iValHighChoice <= iValHigh && cMinimumSamplesPerBin <= iValHigh - iValHighChoice;
      }
   }

   const size_t cRanges = cRangesLow + cRangesHigh;

   constexpr FloatEbmType k_badScore = std::numeric_limits<FloatEbmType>::lowest();

   // TODO: this simple metric just determins which side leads to a better average length.  There are other
   //       algorithms we should use to calculate which side would be better.  Use one of those algorithms, but for
   //       now this simple metric will be fine

   ptrdiff_t transferRangesLow = 0;
   FloatEbmType scoreLow = k_badScore;
   if(bCanSplitLow) {
      const size_t cRangesLowLow = CalculateRangesMaximizeMin(distanceLowFloat, totalDistance, cRanges);
      EBM_ASSERT(1 <= cRangesLowLow);
      EBM_ASSERT(cRangesLowLow < cRanges);
      const size_t cRangesLowHigh = cRanges - cRangesLowLow;
      EBM_ASSERT(1 <= cRangesLowHigh);

      const FloatEbmType avgLengthLow = distanceLowFloat / cRangesLowLow;
      const FloatEbmType avgLengthHigh = (totalDistance - distanceLowFloat) / cRangesLowHigh;

      scoreLow = std::min(avgLengthLow, avgLengthHigh);
      transferRangesLow = static_cast<ptrdiff_t>(cRangesLowLow) - static_cast<ptrdiff_t>(cRangesLow);

      // TODO: for now this simple metric to choose which direction we go should suffice, but in the future 
      //       use a more complicated method to measure things like how badly one side or the other meshes with
      //       nearby ranges

      // TODO: we should also examine how splits work on both sides if we add or subtract some ranges.  We can
      //       start with the zero move from our new count (which might already be moved but it's the optimal new size)
      //       and then examine 2-3 chanes on either end or keep going if things keep improving, or maybe we want
      //       to limit ourselves to just 1 move per split so that we don't get too far away from evenly dividing them
   }

   ptrdiff_t transferRangesHigh = 0;
   FloatEbmType scoreHigh = k_badScore;
   if(bCanSplitHigh) {
      const size_t cRangesHighLow = CalculateRangesMaximizeMin(distanceHighFloat, totalDistance, cRanges);
      EBM_ASSERT(1 <= cRangesHighLow);
      EBM_ASSERT(cRangesHighLow < cRanges);
      const size_t cRangesHighHigh = cRanges - cRangesHighLow;
      EBM_ASSERT(1 <= cRangesHighHigh);

      const FloatEbmType avgLengthLow = distanceHighFloat / cRangesHighLow;
      const FloatEbmType avgLengthHigh = (totalDistance - distanceHighFloat) / cRangesHighHigh;

      scoreHigh = std::min(avgLengthLow, avgLengthHigh);
      transferRangesHigh = static_cast<ptrdiff_t>(cRangesHighLow) - static_cast<ptrdiff_t>(cRangesLow);

      // TODO: for now this simple metric to choose which direction we go should suffice, but in the future 
      //       use a more complicated method to measure things like how badly one side or the other meshes with
      //       nearby ranges
   }

   if(scoreLow < scoreHigh) {
      pSplitCur->m_iVal = iValHighChoice;
      pSplitCur->m_cSplitMoveThis = transferRangesHigh;
   } else {
      if(k_badScore == scoreHigh && k_badScore == scoreLow) {
         pSplitCur->m_iVal = k_illegalIndex;
         pSplitCur->m_cSplitMoveThis = 0; // set this to indicate that we aren't split
      } else {
         pSplitCur->m_iVal = iValLowChoice;
         pSplitCur->m_cSplitMoveThis = transferRangesLow;
      }
   }
   EBM_ASSERT(!pSplitCur->IsSplit());
}

static size_t SplitSegment(
   std::set<SplitPoint *, CompareSplitPoint> * const pBestSplitPoints,

   const size_t cMinimumSamplesPerBin,

   const size_t iValuesStart,
   const size_t cSplittableItems,
   const NeighbourJump * const aNeighbourJumps
) noexcept {
   EBM_ASSERT(nullptr != pBestSplitPoints);

   EBM_ASSERT(1 <= cMinimumSamplesPerBin);
   // we need to be able to put down at least one split not at the edges
   EBM_ASSERT(2 <= cSplittableItems / cMinimumSamplesPerBin);
   EBM_ASSERT(nullptr != aNeighbourJumps);

   // TODO: someday, for performance, it might make sense to use a non-allocating tree, like:
   //       https://github.com/attractivechaos/klib/blob/master/kavl.h

   // TODO: try to use integer math for indexes instead of floating point numbers which introduce inexactness, and they break down above 2^52 where
   //   // individual integers can no longer be represented by a double
   //   // use integer math and integer based fractions (this is also slighly faster)

   // this function assumes that there will either be splits on either sides of the ranges OR that we're 
   // at the end of a range.  We don't handle the special case of there only being 1 split in a range that needs
   // to be chosen.  That's a special case handled elsewhere

   try {
      while(!pBestSplitPoints->empty()) {
         // We've located our desired split points previously.  Sometimes those desired split points
         // are placed in the bulk of a long run of identical values and we have to decide if we'll be putting
         // the split at the start or the end of those long run of identical values.
         //
         // Before this function in the call stack, we do some expensive exploration of the hardest split point placement
         // decisions that we need to make.  We do a full exploration of both the lower and higher placements of the long 
         // runs, but that grows at O(2^N), so we need to limit this full exploration to just a few choices
         //
         // This function being lower in the stack needs to decide whether to place the split at the lower or higher
         // position without the benefit of an in-depth exploration of both options across all possible other cut points 
         // (local exploration is still ok though).  We need to choose one side and live with that decision, so we 
         // look at all our potential splits, and we greedily pick out the split that is 
         // really nice on one side, but really bad on the other, and we keep greedily picking splits this way until they 
         // are all selected.  We use a priority queue to efficiently find the most important split at any given time.
         // Now we have an O(N * log(N)) algorithm in principal, but it's still a bit worse than that.
         //
         // After we decide whether to put the split at the start or end of a run, we're actualizing the location of 
         // the split and we'll be changing the size of the runs to our left and right since they'll either have
         // actualized or desired split points, or the immutable ends as neighbours.  We'd prefer to spread out the
         // movement between our desired and actual split points into all our potential neighbours instead of to the 
         // immediately bordering ranges.  Ideally, we'd like to spread out and re-calculate all other split points 
         // until we reach the immovable boundaries of an already decided split, or the ends, after we've decided 
         // on each split. So, if we had 255 splits, we'd choose one, then re-calculate the split points of the remaining 
         // 254, but that is clearly bad computationally, since then our algorithm would be O(N^2 * log(N)).  For low 
         // numbers like 255 it might be fine, but our user could choose much larger numbers of splits, and then 
         // it would become intractable.
         //
         // Instead of re-calculating all remaining 255 split points though, maybe we can instead choose a window 
         // of influence.  So, if our influence window was set to 50 split points, then even if we had to move one 
         // split point by a large amount of almost a complete split range, we'd only impact the neighboring 50 ranges 
         // by 2% (1/50).
         //
         // After we choose whether to go to the start or the end, we then choose an anchor point 50 to the 
         // left and another one 50 to the right, unless we hit a materialized split point, or the end, which we can't 
         // move.  All the 50 items to the left and right either grow a bit smaller, or a bit bigger anchored to 
         // the influence region ends.
         //
         // After calculating the new sizes of the ranges and the new desired split points, we can then remove
         // the 50-ish items on both sides from our priority queue, which in fact needs to be a tree so that we can
         // remove items that aren't just the lowest value, and we can re-add them to the tree with their new
         // recalculated priority score.
         //
         // But the scores of the desired split points outside of our window have changed slightly too!  
         // The 50th, 51st, 52nd, etc, items to the left and the right weren't moved, but they can still "see" split 
         // points that are within our influence window of desired split points that we changed, so their priorty 
         // scores need to change.  Once we get to twice the window size though, the items beyond that can't be 
         // affected, so we only need to update items within a four time range of our window size, 
         // two on the left and two on the right.
         //
         // Initially we place all our desired split points equidistant from each other within a splitting range.  
         // After one split has been actualized though, we find that there are different distances between desired 
         // split point, which is a consequence of our choosing a tractable algorithm that uses windows of influence. 
         // Let's say our next split point that we extract from the priority queue was at the previous influence boundary
         // split point.  In this case, the ranges to the left and right are sized differently.  Let's say that our influence
         // windows for this new split point are 50 in both directions.  We have two options for choosing where to
         // actualize our split point.  We could equally divide up the space between our influence window boundaries
         // such that our actualized split point is as close to the center as possible, BUT then we'd be radically
         // departing from the priority that we inserted ourselves into the priority queue with.  We also couldn't
         // have inserted ourselves into the priority queue with equidistant ranges on our sides, since then we'd need
         // to cascade the range lengths all the way to the ends, thus giving up the non N^2 nature of of our window
         // of influence algorithm.  I think we need to proportionally move our desired split point to the actual split
         // point such that we don't radically change the location beyond the edges of the range that we fall inside
         // if we alllowed more radical departures in location then our priority queue would loose meaning.
         // 
         // By using proportional adjustment we keep our proposed split point within the range that it fell under 
         // when we generated a priority score, since ottherwise we'd randomize the scores too much and be pulling 
         // out bad choices.  We therefore
         // need to keep the proportions of the ranges relative to their sizes at any given time.  If we re-computed
         // where the split point should be based on a new 50 item window, then it could very well be placed
         // well outside of the range that it was originally suposed to be inside.  It seems like getting that far out
         // of the priority queue score would be bad, so we preseve the relative lengths of the ranges and always
         // find ourselves falling into the range we were considering.
         //
         // If we have our window set to larger than the number of splits, then we'll effectively be re-doing all
         // the splits, which might be ok for small N.  In that case all the splits would always have the same width
         // In our modified world, we get divergence over time, but since we're limiiting our change to a small
         // percentage, we shouldn't get too far out of whack.  Also, we'll quickly put down actualized splitting points such
         // that afer a few we'll probably find ourselves close to a previous split point and we'll proceed by
         // updating all the priority scores exactly since we'll hit the already decided splits before the 
         // influence window length
         //
         // Our priority score will probably depend on the smallest range in our range window, but then we'd need to
         // maintain a sliding window that knows the smallest range within it.  This requries a tree to hold
         // the lengths, and we'd maintain the window as we move, so if we were moving to the left, we'd add one
         // item to the window from the left, and remove the item to the right.  But if the item to the right is
         // the lowest value, we'd need to scan the remaining items, unless we use a tree to keep track.
         // It's probably reasonably effective though to just use the average smallest range that we calculate from 
         // the right side minus the left divided by the number of ranges.  It isn't perfect, but it shouldn't get too
         // bad, and the complexity is lower so it would allow us to do our calculations faster, and therefore allows
         // more exploratory forays in our caller above before we hit time limits.

         // initially, we have pre-calculated which direction each split should go, and we've calculated how many
         // cut points should move between our right and left sides, and we also previously calculated a priority for
         // making decisions. When we pull one potential split point off the queue, we need to nuke all our decisions
         // within the 50 item window on both sides (or until we hit an imovable boundary) and then we need to
         // recalculate for each split which way it should go and what it's priority is
         //
         // At this point we're re-doing our cuts within the 50 item cut window and we need to decide two things:
         //   1) calculate the direction we'd go for each new cut point, and how many cuts we'd move from our right 
         //      and left to the other side
         //   2) Calculate the priority of making the decions
         //
         // For each range we can go either left or right and we need to choose.  We should try and make decions
         // based on our local neighbourhood, so what we can do is start by assuming we go left, and then chop
         // up the spaces to our left boundary equally.  We can then know our desired step distance so we can go and
         // examine what our close neighbours look like within a smaller window.  We can try going left and right for each
         // of our close neighbours and see if we'll need to make hard decisions.  We can try out the combinations by
         // using a 32 bit number (provided we're exploring less than 2^32 options) and then let each bit represent
         // going left or right for a position at the index.  We can then increment the number and re-simulate various
         // nearby options until we find the one that has the best outcome (the one with the biggest smallest range with
         // the least dropage of cuts).  This represents a possible/reasonable outcome.  We know our neighbours will
         // do the same.  Even though they won't end up with the same cut points they'll at least have an available reasonable
         // choice if we make one available to them.
         //
         // Ok, so each cut point we've examined our neighbours and selected a right/left decison that we can live with
         // for ourselves.  Each cut point does this independently.  We can then maybe do an analysis to see if our
         // ideas for the neighbours match up with theirs and do some jiggering if the outcome within a window is bad
         //
         // Ok, so now we've computed our aspirational cut points, and decided where we'd go for each cut point if we
         // were forced to select a cut point now.  We now need to calculate the PRIORITY for all our cut points
         // 
         // Initially we have a lot of options when deciding cuts.  As time goes on, we get less options.  We want our
         // first cut to minimize the danger that later cuts will force us into a bad position.  

         // When calculating the pririty of a cut point a couple of things come to mind:
         //   - if we have a large open space of many un-materialized cuts, an aspiration cut in the middle that falls 
         //     into a big range is not a threat yet since we can deftly avoid it by changing by small amounts the
         //     aspirational cuts on both ends, BUT if someone puts down a ham-fisted cut right down next to it then
         //     we'll have a problem
         //   - even if the cuts in the center are good, a cut in the center next to a large range of equal values could
         //     create a problem for us easily
         //   - our cut materializer needs to be smart and examine the local space before it finalizes a cut, so that
         //     we avoid the largest risks around putting down ham-fisted cuts.  So, with this combination we can relax
         //     and not worry about the asipirational cuts in the middle of a large un-materialized section, whether
         //     they fall onto a currently bad cut or not
         //   - the asipirational cuts near the boundaries of materialized cuts are the most problematic, especially
         //     if they currently happen to be hard decision cuts.  We probably want to make the hard decisions early
         //     when we have the most flexibility to address them
         //   - so, our algorithm will tend to first materialize the cuts near existing boundaries and move inwards as
         //     spaces that were previously not problems become more constrained
         //   - we might or might not want to include results from our decisions about where we'll put cuts.  For
         //     instance, let's say a potential cut point has one good option and one terrible option.  We may want
         //     to materialize the good option so that a neighbour cut doesn't force us to take the terrible option
         //     But this issue is reduced if before we materialize cuts we do an exploration of of the local space to
         //     avoid hurting neighbours.
         //   - in general, because cuts tend to disrupt many other aspirational cuts, we should probably weigh the exact
         //     cut plan less and concentrate of making the most disruptive cuts first.  We might find that our carefully
         //     crafted plan for future cuts is irrelevant and we no longer even make cuts on ranges that we thought
         //     were important previously.
         //   - by choosing the most disruptive cuts first, we'll probably get to a point quickly were most of our
         //     remaining potential cuts are non-conrovertial.  All the hard decisions will be made early and we'll be
         //     left with the cuts that jiggle the remaining cuts less.
         //
         // If we do a full local exploration of where we're going to do our cuts for any single cut, then we can
         // do a better job at calculating the priority, since we'll know how many cuts will be moved from right to left
         //
         // When doing a local exploration, examine going right left on each N segments to each side (2^N explorations)
         // and then re-calculate the average cut point length on the remaining open space beyond the last cut
         //
         // When we're making a cut decision, we recalculate the aspirational position and intended direction of N cuts
         // within a window, but we make NO changes to aspiration cuts OR intended direction outside of that window
         // since those changes might change the priority, and we want to limit the number of cuts we modify
         // we still need to travel 2 * N cuts from our position.  The first N are ones we change, and the next N can
         // see the changes that we made within our window

         // If we push aspirational cuts from our left to right, we don't change the window bounds when that happens
         // because if we did, then there would be no bounds on where we can 100% guarantee that no changes will affect
         // outside regions
         //
         // Ok, so our last step should be to re-calcluate all the priorities.  We do this since we might have a
         // non-linear step in between where we harmonize through a non-sinlge-individual cut process where cuts will
         // happen.  Our non-single-individual cut process should happen after a single pass where we place the cuts
         // without detailed information about where are neighbours want to cut (although we can examine distances in that
         // round
         //
         // I think we pretty much need to pre-calcluate which direction we're going to split the split point BEFORE
         // calculating the priority, since we can't otherwise decide whether to use the best or worst case left/right
         // decision.  If you have a hard to decide small range of values where a cut fall in the middle near one of the
         // tails, then we shouldn't choose the inwards decision for priority calculation if that leaves us with too small
         // a range for putting a split based on our minimum length, so I think this means we need to calculate the
         // splits that we would acualize before calculating priority
         //
         // If for our priority score, we wanted to first materialize the items with the highest tidal disruption, then
         // we need to know which direction we'll be going from an aspirational split point.  We can choose the best
         // case or worst case side but at the time we're computing it we have the same amount of info that we'll have
         // in the future if we decide to split one of them, so we can compute which direction we'll prefer at this
         // point and we can use either the high cost minus the low cost if we think we want to first materialize the
         // options that don't screw us, or if we want to first work on the splits with the highest tidal disruption
         // after we've carefullly decided which way we'll split
         // 
         // So, our process is:
         //    1) Pull a high priority item from the queue (which has a pre-calculated direction to split and all other
         //       splitting decisions already calculated beforehand)
         //    1) execute our pre-determined cut placement AND move cuts from one side to the other if called for in our pre-plan
         //    2) re-calculate the aspiration cut points and for each of those do a first pass combination exploration
         //       to choose the best materialied cut point based on just ourselves
         //    3) Re-pass through our semi-materialized cuts points and jiggle them as necessary against their neighbours
         //       since the "view of the world" is different for each cut point and they don't match perfectly even if
         //       they are often close.
         //    4) Pass from the center to the outer-outer boundary (twice the boundary distance), and remove cuts from
         //       our priority queue, then calculate our new priority which is based on the squared change in all
         //       aspirational cut point (either real or just assuming equal splitting after the cut)
         //       And re-add them with our newly calculated priority, which can examine any cuts within
         //       the N item window at any point (but won't change them)

         auto iterator = pBestSplitPoints->begin();
         SplitPoint * const pSplitBest = *iterator;

         EBM_ASSERT(nullptr != pSplitBest->m_pPrev);
         EBM_ASSERT(nullptr != pSplitBest->m_pNext);

         if(k_noSplitPriority == pSplitBest->m_priority) {
            // k_noSplitPriority means there are no legal splits, and also that all the remaining items in the queue
            // are also unsplittable, so exit
            break;
         }

         EBM_ASSERT(!pSplitBest->IsSplit());

         // we can't move past our outer boundaries
         EBM_ASSERT(-ptrdiff_t { k_SplitExploreDistance } < pSplitBest->m_cSplitMoveThis &&
            pSplitBest->m_cSplitMoveThis < ptrdiff_t { k_SplitExploreDistance });

         // TODO: 
         //   We can also write a pre - checker that finds the maximum possible cuts between two materialized bounds and removes cuts that won't work.. this
         //   is useful in that we might more quickly / earlier find impossible cuts that we can prune and move to other locations
         //   TO do this, when we pull a cut point from the priority queue, we check if our left and right boundaries are materialized
         //   and if they are we find how many cuts we are allowed at maximum and we prune any that can't happen
         //   This probably won't be too useful in the general sense, but it will be important to handle the final condition
         //   where we have enough room on both sides to make cuts in theory but we have a long-ish range that 
         //   is just big enough that it doesn't leave enough room to make cuts at cMinimumSamplesPerBin apart


         // find our visibility window region
         SplitPoint * pSplitLowBoundary = pSplitBest;
         size_t cLowRangesBoundary = k_SplitExploreDistance;
         ptrdiff_t cSplitMoveThisLowLow;
         do {
            pSplitLowBoundary = pSplitLowBoundary->m_pPrev;
            cSplitMoveThisLowLow = pSplitLowBoundary->m_cSplitMoveThis;
            --cLowRangesBoundary;
         } while(0 != cLowRangesBoundary && k_splitValue != cSplitMoveThisLowLow);
         cLowRangesBoundary = k_SplitExploreDistance - cLowRangesBoundary;
         EBM_ASSERT(1 <= cLowRangesBoundary);
         EBM_ASSERT(cLowRangesBoundary <= k_SplitExploreDistance);
         EBM_ASSERT(-pSplitBest->m_cSplitMoveThis < static_cast<ptrdiff_t>(cLowRangesBoundary));

         SplitPoint * pSplitHighBoundary = pSplitBest;
         size_t cHighRangesBoundary = k_SplitExploreDistance;
         ptrdiff_t cSplitMoveThisHighHigh;
         do {
            pSplitHighBoundary = pSplitHighBoundary->m_pNext;
            cSplitMoveThisHighHigh = pSplitHighBoundary->m_cSplitMoveThis;
            --cHighRangesBoundary;
         } while(0 != cHighRangesBoundary && k_splitValue != cSplitMoveThisHighHigh);
         cHighRangesBoundary = k_SplitExploreDistance - cHighRangesBoundary;
         EBM_ASSERT(1 <= cHighRangesBoundary);
         EBM_ASSERT(cHighRangesBoundary <= k_SplitExploreDistance);
         EBM_ASSERT(pSplitBest->m_cSplitMoveThis < static_cast<ptrdiff_t>(cHighRangesBoundary));

         // we're allowed to move splits from our low to high side before splitting, so let's find our new home
         ptrdiff_t cSplitMoveThis = pSplitBest->m_cSplitMoveThis;
         const size_t iVal = pSplitBest->m_iVal;

         SplitPoint * pSplitCur = pSplitBest;

         SplitPoint * pSplitLowLowWindow = pSplitLowBoundary;
         size_t cLowLowRangesWindow = cLowRangesBoundary;

         SplitPoint * pSplitHighHighWindow = pSplitHighBoundary;
         size_t cHighHighRangesWindow = cHighRangesBoundary;

         cLowRangesBoundary += cSplitMoveThis;
         cHighRangesBoundary -= cSplitMoveThis;

         EBM_ASSERT(1 <= cLowRangesBoundary);
         EBM_ASSERT(1 <= cHighRangesBoundary);

         if(0 != cSplitMoveThis) {
            if(cSplitMoveThis < 0) {
               do {
                  pSplitCur = pSplitCur->m_pPrev;
                  EBM_ASSERT(!pSplitCur->IsSplit());

                  if(k_splitValue != cSplitMoveThisLowLow) {
                     pSplitLowLowWindow = pSplitLowLowWindow->m_pPrev;
                     cSplitMoveThisLowLow = pSplitLowLowWindow->m_cSplitMoveThis;
                  } else {
                     // we've hit a split boundary which we can't move, so we get closer to it
                     EBM_ASSERT(2 <= cLowLowRangesWindow);
                     --cLowLowRangesWindow;
                  }
                  EBM_ASSERT((k_splitValue == cSplitMoveThisLowLow) == pSplitLowLowWindow->IsSplit());

                  // TODO: since the movement of pSplitHighHighWindow is dependent on hitting a maximum, we should
                  // be able to calculate the required movement, and then loop it without all this checking and
                  // conditional increments.
                  if(cHighHighRangesWindow == k_SplitExploreDistance) {
                     pSplitHighHighWindow = pSplitHighHighWindow->m_pPrev;
                     EBM_ASSERT(!pSplitHighHighWindow->IsSplit());
                  } else {
                     EBM_ASSERT(pSplitHighHighWindow->IsSplit());
                     // we've escape the length that we need for our window, so we're in the void
                     ++cHighHighRangesWindow;
                  }

                  ++cSplitMoveThis;
               } while(0 != cSplitMoveThis);
               cSplitMoveThisHighHigh = pSplitHighHighWindow->m_cSplitMoveThis;
               EBM_ASSERT((k_splitValue == cSplitMoveThisLowLow) == pSplitLowLowWindow->IsSplit());
            } else {
               do {
                  pSplitCur = pSplitCur->m_pNext;
                  // TODO: since the movement of pSplitLowLowWindow is dependent on hitting a maximum, we should
                  // be able to calculate the required movement, and then loop it without all this checking and
                  // conditional increments.
                  if(cLowLowRangesWindow == k_SplitExploreDistance) {
                     pSplitLowLowWindow = pSplitLowLowWindow->m_pNext;
                     EBM_ASSERT(!pSplitLowLowWindow->IsSplit());
                  } else {
                     EBM_ASSERT(pSplitLowLowWindow->IsSplit());
                     // we've escape the length that we need for our window, so we're in the void
                     ++cLowLowRangesWindow;
                  }
                  if(k_splitValue != cSplitMoveThisHighHigh) {
                     pSplitHighHighWindow = pSplitHighHighWindow->m_pNext;
                     cSplitMoveThisHighHigh = pSplitHighHighWindow->m_cSplitMoveThis;
                  } else {
                     // we've hit a split boundary which we can't move, so we get closer to it
                     EBM_ASSERT(2 <= cHighHighRangesWindow);
                     --cHighHighRangesWindow;
                  }
                  EBM_ASSERT((k_splitValue == cSplitMoveThisHighHigh) == pSplitHighHighWindow->IsSplit());

                  --cSplitMoveThis;
               } while(0 != cSplitMoveThis);
               cSplitMoveThisLowLow = pSplitLowLowWindow->m_cSplitMoveThis;
               EBM_ASSERT((k_splitValue == cSplitMoveThisHighHigh) == pSplitHighHighWindow->IsSplit());
            }
         }

         EBM_ASSERT(1 <= cLowLowRangesWindow);
         EBM_ASSERT(1 <= cHighHighRangesWindow);

         EBM_ASSERT(pSplitLowBoundary < pSplitCur);
         EBM_ASSERT(pSplitCur < pSplitHighBoundary);

         EBM_ASSERT(pSplitLowLowWindow < pSplitCur);
         EBM_ASSERT(pSplitCur < pSplitHighHighWindow);

         EBM_ASSERT(!pSplitCur->IsSplit());

         pSplitCur->SetSplit();

         // TODO: after we set m_iVal, do we really still need to keep m_iValAspirationalFloat??
         pSplitCur->m_iValAspirationalFloat = static_cast<FloatEbmType>(iVal);
         pSplitCur->m_iVal = iVal;


         // TODO : improve this if our boundary is a size_t
         FloatEbmType stepPoint = pSplitLowBoundary->m_iValAspirationalFloat;
         FloatEbmType stepLength = (static_cast<FloatEbmType>(iVal) - stepPoint) / 
            static_cast<FloatEbmType>(cLowRangesBoundary);

         SplitPoint * pSplitAspirational = pSplitCur;
         while(0 != --cLowRangesBoundary) {
            pSplitAspirational = pSplitAspirational->m_pPrev;
            const FloatEbmType iValAspirationalFloat = stepPoint + 
               stepLength * static_cast<FloatEbmType>(cLowRangesBoundary);
            pSplitAspirational->m_iValAspirationalFloat = iValAspirationalFloat;
         }

         // TODO : improve this if our boundary is a size_t
         stepPoint = static_cast<FloatEbmType>(iVal);
         stepLength = (pSplitHighBoundary->m_iValAspirationalFloat - stepPoint) /
            static_cast<FloatEbmType>(cHighRangesBoundary);

         pSplitAspirational = pSplitHighBoundary;
         while(0 != --cHighRangesBoundary) {
            pSplitAspirational = pSplitAspirational->m_pPrev;
            const FloatEbmType iValAspirationalFloat = stepPoint + 
               stepLength * static_cast<FloatEbmType>(cHighRangesBoundary);
            pSplitAspirational->m_iValAspirationalFloat = iValAspirationalFloat;
         }

         SplitPoint * pSplitLowLowNeighbourhoodWindow = pSplitLowLowWindow;
         SplitPoint * pSplitLowHighNeighbourhoodWindow = pSplitCur;
         size_t cLowHighRangesNeighbourhoodWindow = 0;

         size_t iValLowLow = k_splitValue == cSplitMoveThisLowLow ? pSplitLowLowNeighbourhoodWindow->m_iVal : k_illegalIndex;
         size_t iValLowHigh = iVal;

         SplitPoint * pSplitLowNeighbourhoodCur = pSplitCur;

         while(true) {
            if(PREDICTABLE(k_illegalIndex == iValLowLow)) {
               EBM_ASSERT(!pSplitLowLowNeighbourhoodWindow->IsSplit());
               pSplitLowLowNeighbourhoodWindow = pSplitLowLowNeighbourhoodWindow->m_pPrev;
               if(UNLIKELY(pSplitLowLowNeighbourhoodWindow->IsSplit())) {
                  iValLowLow = pSplitLowLowNeighbourhoodWindow->m_iVal;
               }
            } else {
               EBM_ASSERT(pSplitLowLowNeighbourhoodWindow->IsSplit());
               --cLowLowRangesWindow;
               if(UNLIKELY(0 == cLowLowRangesWindow)) {
                  break;
               }
            }

            if(PREDICTABLE(k_SplitExploreDistance == cLowHighRangesNeighbourhoodWindow)) {
               pSplitLowHighNeighbourhoodWindow = pSplitLowHighNeighbourhoodWindow->m_pPrev;
               EBM_ASSERT(!pSplitLowHighNeighbourhoodWindow->IsSplit());
               iValLowHigh = k_illegalIndex;
               if(UNLIKELY(pSplitLowHighNeighbourhoodWindow == pSplitLowBoundary)) {
                  break;
               }
            } else {
               EBM_ASSERT(pSplitLowHighNeighbourhoodWindow->IsSplit());
               EBM_ASSERT(k_illegalIndex != iValLowHigh);
               ++cLowHighRangesNeighbourhoodWindow;
            }

            pSplitLowNeighbourhoodCur = pSplitLowNeighbourhoodCur->m_pPrev;
            EBM_ASSERT(!pSplitLowNeighbourhoodCur->IsSplit()); // we should have exited on 0 == cSplitLowerLower beforehand

            BuildNeighbourhoodPlan(
               cMinimumSamplesPerBin,
               iValuesStart,
               cSplittableItems,
               aNeighbourJumps,

               cLowLowRangesWindow,
               iValLowLow,
               pSplitLowLowNeighbourhoodWindow->m_iValAspirationalFloat,

               cLowHighRangesNeighbourhoodWindow,
               iValLowHigh,
               pSplitLowHighNeighbourhoodWindow->m_iValAspirationalFloat,

               pSplitLowNeighbourhoodCur
            );
         }

         SplitPoint * pSplitHighHighNeighbourhoodWindow = pSplitHighHighWindow;
         SplitPoint * pSplitHighLowNeighbourhoodWindow = pSplitCur;
         size_t cHighLowRangesNeighbourhoodWindow = 0;

         size_t iValHighHigh = k_splitValue == cSplitMoveThisHighHigh ? pSplitHighHighNeighbourhoodWindow->m_iVal : k_illegalIndex;
         size_t iValHighLow = iVal;

         SplitPoint * pSplitHighNeighbourhoodCur = pSplitCur;

         while(true) {
            if(PREDICTABLE(k_illegalIndex == iValHighHigh)) {
               EBM_ASSERT(!pSplitHighHighNeighbourhoodWindow->IsSplit());
               pSplitHighHighNeighbourhoodWindow = pSplitHighHighNeighbourhoodWindow->m_pNext;
               if(UNLIKELY(pSplitHighHighNeighbourhoodWindow->IsSplit())) {
                  iValHighHigh = pSplitHighHighNeighbourhoodWindow->m_iVal;
               }
            } else {
               EBM_ASSERT(pSplitHighHighNeighbourhoodWindow->IsSplit());
               --cHighHighRangesWindow;
               if(UNLIKELY(0 == cHighHighRangesWindow)) {
                  break;
               }
            }

            if(PREDICTABLE(k_SplitExploreDistance == cHighLowRangesNeighbourhoodWindow)) {
               pSplitHighLowNeighbourhoodWindow = pSplitHighLowNeighbourhoodWindow->m_pNext;
               EBM_ASSERT(!pSplitHighLowNeighbourhoodWindow->IsSplit());
               iValHighLow = k_illegalIndex;
               if(UNLIKELY(pSplitHighLowNeighbourhoodWindow == pSplitHighBoundary)) {
                  break;
               }
            } else {
               EBM_ASSERT(pSplitHighLowNeighbourhoodWindow->IsSplit());
               EBM_ASSERT(k_illegalIndex != iValHighLow);
               ++cHighLowRangesNeighbourhoodWindow;
            }

            pSplitHighNeighbourhoodCur = pSplitHighNeighbourhoodCur->m_pNext;
            EBM_ASSERT(!pSplitHighNeighbourhoodCur->IsSplit());

            BuildNeighbourhoodPlan(
               cMinimumSamplesPerBin,
               iValuesStart,
               cSplittableItems,
               aNeighbourJumps,

               cHighLowRangesNeighbourhoodWindow,
               iValHighLow,
               pSplitHighLowNeighbourhoodWindow->m_iValAspirationalFloat,

               cHighHighRangesWindow,
               iValHighHigh,
               pSplitHighHighNeighbourhoodWindow->m_iValAspirationalFloat,

               pSplitHighNeighbourhoodCur
            );
         }

         EBM_ASSERT(pBestSplitPoints->end() != pBestSplitPoints->find(pSplitCur));
         pBestSplitPoints->erase(pSplitCur);

         SplitPoint * pSplitLowLowPriorityWindow = pSplitLowLowWindow;
         SplitPoint * pSplitLowHighPriorityWindow = pSplitCur;
         size_t cLowHighRangesPriorityWindow = 0;
         SplitPoint * pSplitLowPriorityCur = pSplitCur;

         while(true) {
            if(PREDICTABLE(k_SplitExploreDistance == cLowHighRangesPriorityWindow)) {
               pSplitLowHighPriorityWindow = pSplitLowHighPriorityWindow->m_pPrev;
               EBM_ASSERT(!pSplitLowHighPriorityWindow->IsSplit());
               if(UNLIKELY(pSplitLowHighPriorityWindow == pSplitLowBoundary)) {
                  break;
               }
            } else {
               EBM_ASSERT(pSplitLowHighPriorityWindow->IsSplit());
               ++cLowHighRangesPriorityWindow;
            }

            pSplitLowPriorityCur = pSplitLowPriorityCur->m_pPrev;
            if(PREDICTABLE(k_splitValue != cSplitMoveThisLowLow)) {
               EBM_ASSERT(!pSplitLowLowPriorityWindow->IsSplit());
               pSplitLowLowPriorityWindow = pSplitLowLowPriorityWindow->m_pPrev;
               cSplitMoveThisLowLow = pSplitLowLowPriorityWindow->m_cSplitMoveThis;
            } else {
               EBM_ASSERT(pSplitLowLowPriorityWindow->IsSplit());
               if(UNLIKELY(pSplitLowPriorityCur == pSplitLowLowPriorityWindow)) {
                  EBM_ASSERT(pSplitLowPriorityCur->IsSplit());
                  break;
               }
            }
            EBM_ASSERT(!pSplitLowPriorityCur->IsSplit());
            EBM_ASSERT(pBestSplitPoints->end() != pBestSplitPoints->find(pSplitLowPriorityCur));
            pBestSplitPoints->erase(pSplitLowPriorityCur);

            CalculatePriority(
               pSplitLowLowPriorityWindow->m_iValAspirationalFloat,
               pSplitLowHighPriorityWindow->m_iValAspirationalFloat,
               pSplitLowPriorityCur
            );

            EBM_ASSERT(!pSplitLowPriorityCur->IsSplit());
            pBestSplitPoints->insert(pSplitLowPriorityCur);
         }

         SplitPoint * pSplitHighHighPriorityWindow = pSplitHighHighWindow;
         SplitPoint * pSplitHighLowPriorityWindow = pSplitCur;
         size_t cHighLowRangesPriorityWindow = 0;
         SplitPoint * pSplitHighPriorityCur = pSplitCur;

         while(true) {
            if(PREDICTABLE(k_SplitExploreDistance == cHighLowRangesPriorityWindow)) {
               pSplitHighLowPriorityWindow = pSplitHighLowPriorityWindow->m_pNext;
               EBM_ASSERT(!pSplitHighLowPriorityWindow->IsSplit());
               if(UNLIKELY(pSplitHighLowPriorityWindow == pSplitHighBoundary)) {
                  break;
               }
            } else {
               EBM_ASSERT(pSplitHighLowPriorityWindow->IsSplit());
               ++cHighLowRangesPriorityWindow;
            }

            pSplitHighPriorityCur = pSplitHighPriorityCur->m_pNext;
            if(PREDICTABLE(k_splitValue != cSplitMoveThisHighHigh)) {
               EBM_ASSERT(!pSplitHighHighPriorityWindow->IsSplit());
               pSplitHighHighPriorityWindow = pSplitHighHighPriorityWindow->m_pNext;
               cSplitMoveThisHighHigh = pSplitHighHighPriorityWindow->m_cSplitMoveThis;
            } else {
               EBM_ASSERT(pSplitHighHighPriorityWindow->IsSplit());
               if(UNLIKELY(pSplitHighPriorityCur == pSplitHighHighPriorityWindow)) {
                  EBM_ASSERT(pSplitHighPriorityCur->IsSplit());
                  break;
               }
            }
            EBM_ASSERT(!pSplitHighPriorityCur->IsSplit());
            EBM_ASSERT(pBestSplitPoints->end() != pBestSplitPoints->find(pSplitHighPriorityCur));
            pBestSplitPoints->erase(pSplitHighPriorityCur);

            CalculatePriority(
               pSplitHighLowPriorityWindow->m_iValAspirationalFloat,
               pSplitHighHighPriorityWindow->m_iValAspirationalFloat,
               pSplitHighPriorityCur
            );

            EBM_ASSERT(!pSplitHighPriorityCur->IsSplit());
            pBestSplitPoints->insert(pSplitHighPriorityCur);
         }
      }
   } catch(...) {
      // TODO : HANDLE THIS
      LOG_0(TraceLevelWarning, "WARNING SplitSegment exception");
      exit(1); // for now take this draconian step
   }


   // TODO : we have an optional phase here were we try and reduce the tension between neighbours and improve
   // the tentative plans of each m_iVal


   // TODO: after we have our final m_iVal for each potential split given all our information, we then find
   // the split that needs to screw everything up the most and start with that one since it's the most constrained
   // and we want to handle it while we have the most flexibility
   // 
   // there are a lot of metrics we might use.  Two ideas:
   //   1) Look at how bad the best solution for any particular split is.. if it's bad it's probably because the
   //      alternatives were worse
   //   2) Look at how bad the worst solution for any particular split is.. we don't want to be forced to take the
   //      worst
   //   3) * take the aspirational split, take the best matrialized split, calculate what percentage we need to
   //      stretch from either boundary (the low boundary and the high boundary).  Take the one that has the highest
   //      percentage stretch
   //
   // I like #3, because after we choose each split everything (within the windows) get re-shuffed.  We might not
   // even fall on some of the problematic ranges anymore.  Choosing the splits with the highest "tension" causes
   // us to decide the longest ranges that are the closest to one of our existing imovable boundaries thus
   // we're nailing down the ones that'll cause the most movement first while we have the most room, and it also
   // captures the idea that these are bad ones that need to be selected.  It'll tend to try deciding splits
   // near our existing edge boundaries first instead of the ones in the center.  This is good since the ones at
   // the boundaries are more critical.  As we materialize cuts we'll get closer to the center and those will start
   // to want attention



   IronSplits();

   return 0;
}

static size_t TreeSearchSplitSegment(
   std::set<SplitPoint *, CompareSplitPoint> * pBestSplitPoints,

   const size_t cMinimumSamplesPerBin,

   const size_t iValuesStart,
   const size_t cSplittableItems,
   const NeighbourJump * const aNeighbourJumps,

   const size_t cRanges,
   // for efficiency we include space for the end point cuts even if they don't exist
   SplitPoint * const aSplitsWithENDPOINTS
) noexcept {
   try {
      EBM_ASSERT(nullptr != pBestSplitPoints);
      EBM_ASSERT(pBestSplitPoints->empty());

      EBM_ASSERT(1 <= cMinimumSamplesPerBin);
      EBM_ASSERT(nullptr != aNeighbourJumps);

      EBM_ASSERT(2 <= cRanges);
      EBM_ASSERT(cRanges <= cSplittableItems / cMinimumSamplesPerBin);
      EBM_ASSERT(nullptr != aSplitsWithENDPOINTS);

      // - TODO: EXPLORING BOTH SIDES
      //   - first strategy is to divide the region into floating point divisions, and find the single worst split where going both left or right is bad (using floating point distance)
      //   - then go left and go right, re - divide the entire set base on the left choice and the right choice
      //   - but this grows at 2 ^ N, so we need annother one that makes a decision without going down two paths.It needs to check the left and right and make a decion on the spot
      //
      //   - we can design an algorithm that divides into 255 and chooses the worst one and then does a complete fit on either direction.Best fit is recorded
      //     then we re-do all 254 other cuts on BOTH sides.We can only do a set number of these, so after 8 levels we'd have 256 attempts.  That might be acceptable
      //   - the algorithm that we have below plays it safe since it needs to live with it's decions.  This more spectlative algorithm above can be more
      //     risky since it plays both directions a bad play won't undermine it.  As such, we should try and chose the worst decion without regard to position
      //     so in other words, try to choose the range that we have a drop point in in the middle where we need to move the most to get away from the 
      //     best drops.  We can also try going left, going right, OR not choosing.  Don't traverse down the NO choice path, so we add 50% load, but we don't grow at 3^N, and we'll 
      //     also explore the no choice at the root option
      //

      //constexpr size_t k_SplitExploreDepth = 8;
      //constexpr size_t k_SplitExplorations = size_t { 1 } << k_SplitExploreDepth;

      SplitPoint * pSplitCur = &aSplitsWithENDPOINTS[0];
      SplitPoint * pSplitNext = &aSplitsWithENDPOINTS[1];

      pSplitCur->m_pNext = pSplitNext;
      pSplitCur->SetSplit();
      pSplitCur->m_iValAspirationalFloat = FloatEbmType { 0 };
      pSplitCur->m_iVal = 0;

      const FloatEbmType stepInit = static_cast<FloatEbmType>(cSplittableItems) / static_cast<FloatEbmType>(cRanges);
      for(size_t iNeighbours = 1; iNeighbours < cRanges; ++iNeighbours) {
         pSplitNext->m_pPrev = pSplitCur;
         pSplitCur = pSplitNext;
         ++pSplitNext;
         pSplitCur->m_pNext = pSplitNext;

         const size_t iLowBound = iNeighbours <= k_SplitExploreDistance ? 0 : iNeighbours - k_SplitExploreDistance;
         size_t iHighBound = iNeighbours + k_SplitExploreDistance;
         iHighBound = cRanges < iHighBound ? cRanges : iHighBound;

         EBM_ASSERT(iLowBound < iNeighbours);
         EBM_ASSERT(iNeighbours < iHighBound);

         const FloatEbmType iLowValFloat = stepInit * iLowBound;
         const FloatEbmType iValAspirationalCurFloat = stepInit * iNeighbours;
         const FloatEbmType iHighValFloat = stepInit * iHighBound;

         pSplitCur->m_iValAspirationalFloat = iValAspirationalCurFloat;

         BuildNeighbourhoodPlan(
            cMinimumSamplesPerBin,
            iValuesStart,
            cSplittableItems,
            aNeighbourJumps,
            iNeighbours - iLowBound,
            size_t { 0 } == iLowBound ? size_t { 0 } : k_illegalIndex,
            iLowValFloat,
            iHighBound - iNeighbours,
            cRanges == iHighBound ? cSplittableItems : k_illegalIndex,
            iHighValFloat,
            pSplitCur
         );
      }

      pSplitNext->m_pPrev = pSplitCur;
      pSplitNext->m_pNext = nullptr;
      pSplitNext->SetSplit();
      pSplitNext->m_iValAspirationalFloat = static_cast<FloatEbmType>(cSplittableItems);
      pSplitNext->m_iVal = cSplittableItems;

      size_t iPriority = 1;
      // we might write code above that removes SplitPoints, which if it were true could mean no legal splits
      for(SplitPoint * pSplit = aSplitsWithENDPOINTS[0].m_pNext ; pSplitNext != pSplit ; pSplit = pSplit->m_pNext) {
         const size_t iLowBound = iPriority <= k_SplitExploreDistance ? 0 : iPriority - k_SplitExploreDistance;
         size_t iHighBound = iPriority + k_SplitExploreDistance;
         iHighBound = cRanges < iHighBound ? cRanges : iHighBound;

         EBM_ASSERT(iLowBound < iPriority);
         EBM_ASSERT(iPriority < iHighBound);

         const FloatEbmType iLowValFloat = stepInit * iLowBound;
         const FloatEbmType iHighValFloat = stepInit * iHighBound;

         CalculatePriority(
            iLowValFloat,
            iHighValFloat,
            pSplit
         );
         EBM_ASSERT(!pSplit->IsSplit());
         pBestSplitPoints->insert(pSplit);
         ++iPriority;
      }
   } catch(...) {
      // TODO: handle this!
      LOG_0(TraceLevelWarning, "WARNING TreeSearchSplitSegment exception");
      exit(1); // for now take this draconian step
   }

   return SplitSegment(
      pBestSplitPoints,
      cMinimumSamplesPerBin,
      iValuesStart,
      cSplittableItems,
      aNeighbourJumps
   );
}

INLINE_RELEASE static size_t TradeSplitSegment(
   std::set<SplitPoint *, CompareSplitPoint> * pBestSplitPoints,

   const size_t cMinimumSamplesPerBin,

   const size_t iValuesStart,
   const size_t cSplittableItems,
   const NeighbourJump * const aNeighbourJumps,

   const size_t cRanges,
   // for efficiency we include space for the end point cuts even if they don't exist
   SplitPoint * const aSplitsWithENDPOINTS
) noexcept {
   // - TODO: ABOVE THE COUNT FIXING
   //   - we can examine what it would look like to have 1 more cut and 1 less cut that our original choice
   //   - then we can try and sort the best to worst subtraction and addition, and then try and swap the best subtraction with the best addition and repeat
   //   - calculate the maximum number of splits based on the minimum bunch size.  we should be able to do this by
   //     doing a single pass where we make every range the minimum
   //   - then we can loop from our current cuts to the maximum and stop when we hit the maximum (perahps there are long 
   //     ranges that prevent good spits)
   //   - if we want to get a specific number of cuts, we can ask for that many, but we might get less back as a result
   //     (never more).  We can try to increment the number of items that we ask for and see if we end up with the right
   //     number.  It might be bad though if we continually do +1 because it might be intractable if there are a lot
   //     of splits.  Perahps we want to use a binary algorithm where we do +1, +2, +4, +8, and if we exceed then
   //     do binary descent between 4 and 8 until we get our exact number.

   return TreeSearchSplitSegment(pBestSplitPoints, cMinimumSamplesPerBin, iValuesStart, cSplittableItems, aNeighbourJumps,
      cRanges, aSplitsWithENDPOINTS);
}

static bool StuffSplitsIntoSplittingRanges(
   const size_t cSplittingRanges,
   SplittingRange * const aSplittingRange,
   const size_t cMinimumSamplesPerBin,
   size_t cRemainingSplits
) noexcept {
   EBM_ASSERT(1 <= cSplittingRanges);
   EBM_ASSERT(nullptr != aSplittingRange);
   EBM_ASSERT(1 <= cMinimumSamplesPerBin);

   // generally, having small bins with insufficient data is more dangerous for overfitting
   // than the lost opportunity from not cutting big bins down.  So, what we want to avoid is having
   // small bins.  So, create a heap and insert the average bin size AFTER we would add a new cut
   // don't insert any SplittingRanges that cannot legally be cut (so, it's guaranteed to only have
   // cuttable items).  Now pick off the SplittingRange that has the largest average AFTER adding a cut
   // and then add the cut, re-calculate the new average, and re-insert into the heap.  Continue
   // until there are no items in the heap because they've all exhausted the possibilities of cuts
   // OR until we run out of cuts to dole out.

   class CompareSplittingRange final {
   public:
      INLINE_ALWAYS bool operator() (
         const SplittingRange * const & lhs,
         const SplittingRange * const & rhs
      ) const noexcept {
         return lhs->m_avgSplittableRangeWidthAfterAddingOneSplit == rhs->m_avgSplittableRangeWidthAfterAddingOneSplit ?
            (lhs->m_uniqueRandom < rhs->m_uniqueRandom) :
            (lhs->m_avgSplittableRangeWidthAfterAddingOneSplit < rhs->m_avgSplittableRangeWidthAfterAddingOneSplit);
      }
   };

   if(LIKELY(0 != cRemainingSplits)) {
      try {
         std::priority_queue<SplittingRange *, std::vector<SplittingRange *>, CompareSplittingRange> queue;

         SplittingRange * pSplittingRangeInit = aSplittingRange;
         const SplittingRange * const pSplittingRangeEnd = aSplittingRange + cSplittingRanges;
         do {
            // let's say that our SplittingRange had 2 splits already, and that it was sanwitched in between two
            // long unsplittable sections.  In this configuration, we would have 1 range spanning between the 
            // lower and upper boundary splits.  Our priority queue wants to know what we'd look like IF we chose 
            // this SplittingRange to stuff a split into.  If that were to happen, then we'd have 2 ranges.  So,   
            // if we had 2 splits, we'd have 2 ranges.  This is why we don't increment the cSplitsAssigned value here.
            size_t newProposedRanges = pSplittingRangeInit->m_cSplitsAssigned;
            if(0 == pSplittingRangeInit->m_cUnsplittableEitherSideValuesMin) {
               // our first and last SplittingRanges can either have a long range of equal items on their tail ends
               // or nothing.  If there is a long range of equal items, then we'll be placing one cut at the tail
               // end, otherwise we have an implicit cut there and we don't need to use one of our cuts.  It's
               // like getting a free cut, so increase the number of ranges by one if we don't need one cut at the tail
               // side

               ++newProposedRanges;

               if(0 == pSplittingRangeInit->m_cUnsplittableEitherSideValuesMax) {
                  // if there's a max of zero unsplittable values on our sides, then we're the only range AND 
                  // we don't have to put cuts on either of our boundaries, so add 1 more to our ranges since
                  // the cuts that we do have will be interior

                  EBM_ASSERT(1 == cSplittingRanges);

                  ++newProposedRanges;
               }
            }
            
            const size_t cSplittableItems = pSplittingRangeInit->m_cSplittableValues;
            // use more exact integer math here
            if(cMinimumSamplesPerBin <= cSplittableItems / newProposedRanges) {
               const FloatEbmType avgRangeWidthAfterAddingOneSplit =
                  static_cast<FloatEbmType>(cSplittableItems) / static_cast<FloatEbmType>(newProposedRanges);

               pSplittingRangeInit->m_avgSplittableRangeWidthAfterAddingOneSplit = avgRangeWidthAfterAddingOneSplit;
               queue.push(pSplittingRangeInit);
            }

            ++pSplittingRangeInit;
         } while(pSplittingRangeEnd != pSplittingRangeInit);

         // the queue can initially be empty if all the ranges are too short to make them cMinimumSamplesPerBin
         while(!queue.empty()) {
            SplittingRange * pSplittingRangeAdd = queue.top();
            queue.pop();

            // let's say that our SplittingRange had 2 splits already, and that it was sanwitched in between two
            // long unsplittable sections.  In this configuration, we would have 1 range spanning between the 
            // lower and upper boundary splits.  Since we were chosen from the priority queue, we should now have
            // 2 ranges.  But we want to insert ourselves back into the priority queue with our number of ranges plus
            // one so that the queue can figure out the best splitting Range AFTER a new cut is added, so add 1.
            size_t newProposedRanges = pSplittingRangeAdd->m_cSplitsAssigned + 1;
            pSplittingRangeAdd->m_cSplitsAssigned = newProposedRanges;

            --cRemainingSplits;
            if(0 == cRemainingSplits) {
               break;
            }

            if(0 == pSplittingRangeAdd->m_cUnsplittableEitherSideValuesMin) {
               // our first and last SplittingRanges can either have a long range of equal items on their tail ends
               // or nothing.  If there is a long range of equal items, then we'll be placing one cut at the tail
               // end, otherwise we have an implicit cut there and we don't need to use one of our cuts.  It's
               // like getting a free cut, so increase the number of ranges by one if we don't need one cut at the tail
               // side

               ++newProposedRanges;

               if(0 == pSplittingRangeAdd->m_cUnsplittableEitherSideValuesMax) {
                  // if there's a max of zero unsplittable values on our sides, then we're the only range AND 
                  // we don't have to put cuts on either of our boundaries, so add 1 more to our ranges since
                  // the cuts that we do have will be interior

                  EBM_ASSERT(1 == cSplittingRanges);

                  ++newProposedRanges;
               }
            }

            const size_t cSplittableItems = pSplittingRangeAdd->m_cSplittableValues;
            // use more exact integer math here
            if(cMinimumSamplesPerBin <= cSplittableItems / newProposedRanges) {
               const FloatEbmType avgRangeWidthAfterAddingOneSplit =
                  static_cast<FloatEbmType>(cSplittableItems) / static_cast<FloatEbmType>(newProposedRanges);

               pSplittingRangeAdd->m_avgSplittableRangeWidthAfterAddingOneSplit = avgRangeWidthAfterAddingOneSplit;
               queue.push(pSplittingRangeAdd);
            }
         }
      } catch(...) {
         LOG_0(TraceLevelWarning, "WARNING StuffSplitsIntoSplittingRanges exception");
         return true;
      }
   }
   return false;
}

INLINE_RELEASE static size_t FillSplittingRangeRemaining(
   const size_t cSplittingRanges,
   SplittingRange * const aSplittingRange
) noexcept {
   EBM_ASSERT(1 <= cSplittingRanges);
   EBM_ASSERT(nullptr != aSplittingRange);

   SplittingRange * pSplittingRange = aSplittingRange;
   const SplittingRange * const pSplittingRangeEnd = pSplittingRange + cSplittingRanges;
   do {
      const size_t cUnsplittablePriorItems = pSplittingRange->m_cUnsplittableLowValues;
      const size_t cUnsplittableSubsequentItems = pSplittingRange->m_cUnsplittableHighValues;

      pSplittingRange->m_cUnsplittableEitherSideValuesMax = std::max(cUnsplittablePriorItems, cUnsplittableSubsequentItems);
      pSplittingRange->m_cUnsplittableEitherSideValuesMin = std::min(cUnsplittablePriorItems, cUnsplittableSubsequentItems);

      pSplittingRange->m_flags = k_MiddleSplittingRange;
      pSplittingRange->m_cSplitsAssigned = 1;

      ++pSplittingRange;
   } while(pSplittingRangeEnd != pSplittingRange);

   size_t cConsumedSplittingRanges = cSplittingRanges;
   if(1 == cSplittingRanges) {
      aSplittingRange[0].m_flags = k_FirstSplittingRange | k_LastSplittingRange;
      // might as well assign a split to the only SplittingRange.  We'll be stuffing it as full as it can get soon
      EBM_ASSERT(1 == aSplittingRange[0].m_cSplitsAssigned);
   } else {
      aSplittingRange[0].m_flags = k_FirstSplittingRange;
      if(0 == aSplittingRange[0].m_cUnsplittableLowValues) {
         aSplittingRange[0].m_cSplitsAssigned = 0;
         --cConsumedSplittingRanges;
      }

      --pSplittingRange; // go back to the last one
      pSplittingRange->m_flags = k_LastSplittingRange;
      if(0 == pSplittingRange->m_cUnsplittableHighValues) {
         pSplittingRange->m_cSplitsAssigned = 0;
         --cConsumedSplittingRanges;
      }
   }
   return cConsumedSplittingRanges;
}

INLINE_RELEASE static void FillSplittingRangeNeighbours(
   const size_t cSamples,
   FloatEbmType * const aSingleFeatureValues,
   const size_t cSplittingRanges,
   SplittingRange * const aSplittingRange
) noexcept {
   EBM_ASSERT(1 <= cSamples);
   EBM_ASSERT(nullptr != aSingleFeatureValues);
   EBM_ASSERT(1 <= cSplittingRanges);
   EBM_ASSERT(nullptr != aSplittingRange);

   SplittingRange * pSplittingRange = aSplittingRange;
   size_t cUnsplittablePriorItems = pSplittingRange->m_pSplittableValuesFirst - aSingleFeatureValues;
   const FloatEbmType * const aSingleFeatureValuesEnd = aSingleFeatureValues + cSamples;
   if(1 != cSplittingRanges) {
      const SplittingRange * const pSplittingRangeLast = pSplittingRange + cSplittingRanges - 1; // exit without doing the last one
      do {
         const size_t cUnsplittableSubsequentItems =
            (pSplittingRange + 1)->m_pSplittableValuesFirst - pSplittingRange->m_pSplittableValuesFirst - pSplittingRange->m_cSplittableValues;

         pSplittingRange->m_cUnsplittableLowValues = cUnsplittablePriorItems;
         pSplittingRange->m_cUnsplittableHighValues = cUnsplittableSubsequentItems;

         cUnsplittablePriorItems = cUnsplittableSubsequentItems;
         ++pSplittingRange;
      } while(pSplittingRangeLast != pSplittingRange);
   }
   const size_t cUnsplittableSubsequentItems =
      aSingleFeatureValuesEnd - pSplittingRange->m_pSplittableValuesFirst - pSplittingRange->m_cSplittableValues;

   pSplittingRange->m_cUnsplittableLowValues = cUnsplittablePriorItems;
   pSplittingRange->m_cUnsplittableHighValues = cUnsplittableSubsequentItems;
}

INLINE_RELEASE static void FillSplittingRangeBasics(
   const size_t cSamples,
   FloatEbmType * const aSingleFeatureValues,
   const size_t avgLength,
   const size_t cMinimumSamplesPerBin,
   const size_t cSplittingRanges,
   SplittingRange * const aSplittingRange
) noexcept {
   EBM_ASSERT(1 <= cSamples);
   EBM_ASSERT(nullptr != aSingleFeatureValues);
   EBM_ASSERT(1 <= avgLength);
   EBM_ASSERT(1 <= cMinimumSamplesPerBin);
   EBM_ASSERT(1 <= cSplittingRanges);
   EBM_ASSERT(nullptr != aSplittingRange);

   FloatEbmType rangeValue = *aSingleFeatureValues;
   FloatEbmType * pSplittableValuesStart = aSingleFeatureValues;
   const FloatEbmType * pStartEqualRange = aSingleFeatureValues;
   FloatEbmType * pScan = aSingleFeatureValues + 1;
   const FloatEbmType * const pValuesEnd = aSingleFeatureValues + cSamples;

   SplittingRange * pSplittingRange = aSplittingRange;
   while(pValuesEnd != pScan) {
      const FloatEbmType val = *pScan;
      if(val != rangeValue) {
         size_t cEqualRangeItems = pScan - pStartEqualRange;
         if(avgLength <= cEqualRangeItems) {
            if(aSingleFeatureValues != pSplittableValuesStart || cMinimumSamplesPerBin <= static_cast<size_t>(pStartEqualRange - pSplittableValuesStart)) {
               EBM_ASSERT(pSplittingRange < aSplittingRange + cSplittingRanges);
               pSplittingRange->m_pSplittableValuesFirst = pSplittableValuesStart;
               pSplittingRange->m_cSplittableValues = pStartEqualRange - pSplittableValuesStart;
               ++pSplittingRange;
            }
            pSplittableValuesStart = pScan;
         }
         rangeValue = val;
         pStartEqualRange = pScan;
      }
      ++pScan;
   }
   if(pSplittingRange != aSplittingRange + cSplittingRanges) {
      // we're not done, so we have one more to go.. this last one
      EBM_ASSERT(pSplittingRange == aSplittingRange + cSplittingRanges - 1);
      EBM_ASSERT(pSplittableValuesStart < pValuesEnd);
      pSplittingRange->m_pSplittableValuesFirst = pSplittableValuesStart;
      EBM_ASSERT(pStartEqualRange < pValuesEnd);
      const size_t cEqualRangeItems = pValuesEnd - pStartEqualRange;
      const FloatEbmType * const pSplittableRangeEnd = avgLength <= cEqualRangeItems ? pStartEqualRange : pValuesEnd;
      pSplittingRange->m_cSplittableValues = pSplittableRangeEnd - pSplittableValuesStart;
   }
}

INLINE_RELEASE static void FillSplittingRangeRandom(
   RandomStream * const pRandomStream,
   const size_t cSplittingRanges,
   SplittingRange * const aSplittingRange
) noexcept {
   EBM_ASSERT(1 <= cSplittingRanges);
   EBM_ASSERT(nullptr != aSplittingRange);

   size_t index = 0;
   SplittingRange * pSplittingRange = aSplittingRange;
   const SplittingRange * const pSplittingRangeEnd = pSplittingRange + cSplittingRanges;
   do {
      pSplittingRange->m_uniqueRandom = index;
      ++index;
      ++pSplittingRange;
   } while(pSplittingRangeEnd != pSplittingRange);

   // the last index doesn't need to be swapped, since there is nothing to swap it with
   const size_t cVisitSplittingRanges = cSplittingRanges - 1;
   for(size_t i = 0; LIKELY(i < cVisitSplittingRanges); ++i) {
      const size_t cPossibleSwapLocations = cSplittingRanges - i;
      EBM_ASSERT(1 <= cPossibleSwapLocations);
      // for randomness, we need to be able to swap with ourselves, so iSwap can be 0 
      // and in that case we'll swap with ourselves
      const size_t iSwap = pRandomStream->Next(cPossibleSwapLocations);
      const size_t uniqueRandomTmp = aSplittingRange[i].m_uniqueRandom;
      aSplittingRange[i].m_uniqueRandom = aSplittingRange[i + iSwap].m_uniqueRandom;
      aSplittingRange[i + iSwap].m_uniqueRandom = uniqueRandomTmp;
   }
}

INLINE_RELEASE static void FillSplittingRangePointers(
   const size_t cSplittingRanges,
   SplittingRange ** const apSplittingRange,
   SplittingRange * const aSplittingRange
) noexcept {
   EBM_ASSERT(1 <= cSplittingRanges);
   EBM_ASSERT(nullptr != apSplittingRange);
   EBM_ASSERT(nullptr != aSplittingRange);

   SplittingRange ** ppSplittingRange = apSplittingRange;
   const SplittingRange * const * const apSplittingRangeEnd = apSplittingRange + cSplittingRanges;
   SplittingRange * pSplittingRange = aSplittingRange;
   do {
      *ppSplittingRange = pSplittingRange;
      ++pSplittingRange;
      ++ppSplittingRange;
   } while(apSplittingRangeEnd != ppSplittingRange);
}

INLINE_RELEASE static void FillSplitPointRandom(
   RandomStream * const pRandomStream,
   const size_t cSplitPoints,
   SplitPoint * const aSplitPoints
) noexcept {
   EBM_ASSERT(1 <= cSplitPoints); // 1 can happen if there is only one splitting range
   EBM_ASSERT(nullptr != aSplitPoints);

   size_t index = 0;
   SplitPoint * pSplitPoint = aSplitPoints;
   const SplitPoint * const pSplitPointEnd = pSplitPoint + cSplitPoints;
   do {
      pSplitPoint->m_uniqueRandom = index;
      ++index;
      ++pSplitPoint;
   } while(pSplitPointEnd != pSplitPoint);

   // the last index doesn't need to be swapped, since there is nothing to swap it with
   const size_t cVisitSplitPoints = cSplitPoints - 1;
   for(size_t i = 0; LIKELY(i < cVisitSplitPoints); ++i) {
      const size_t cPossibleSwapLocations = cSplitPoints - i;
      EBM_ASSERT(1 <= cPossibleSwapLocations);
      // for randomness, we need to be able to swap with ourselves, so iSwap can be 0 
      // and in that case we'll swap with ourselves
      const size_t iSwap = pRandomStream->Next(cPossibleSwapLocations);
      const size_t uniqueRandomTmp = aSplitPoints[i].m_uniqueRandom;
      aSplitPoints[i].m_uniqueRandom = aSplitPoints[i + iSwap].m_uniqueRandom;
      aSplitPoints[i + iSwap].m_uniqueRandom = uniqueRandomTmp;
   }
}

INLINE_RELEASE static NeighbourJump * ConstructJumps(
   const size_t cSamples, 
   const FloatEbmType * const aValues
) noexcept {
   // TODO test this
   EBM_ASSERT(0 < cSamples);
   EBM_ASSERT(nullptr != aValues);

   NeighbourJump * const aNeighbourJump = EbmMalloc<NeighbourJump>(cSamples);
   if(nullptr == aNeighbourJump) {
      return nullptr;
   }

   FloatEbmType valNext = aValues[0];
   const FloatEbmType * pValue = aValues;
   const FloatEbmType * const pValueEnd = aValues + cSamples;

   size_t iStartCur = 0;
   NeighbourJump * pNeighbourJump = aNeighbourJump;
   do {
      const FloatEbmType valCur = valNext;
      do {
         ++pValue;
         if(UNLIKELY(pValueEnd == pValue)) {
            break;
         }
         valNext = *pValue;
      } while(PREDICTABLE(valNext == valCur));

      const size_t iStartNext = pValue - aValues;
      const size_t cItems = iStartNext - iStartCur;

      const NeighbourJump * const pNeighbourJumpEnd = pNeighbourJump + cItems;
      do {
         pNeighbourJump->m_iStartCur = iStartCur;
         pNeighbourJump->m_iStartNext = iStartNext;
         ++pNeighbourJump;
      } while(pNeighbourJumpEnd != pNeighbourJump);

      iStartCur = iStartNext;
   } while(LIKELY(pValueEnd != pValue));

   return aNeighbourJump;
}

INLINE_RELEASE static size_t CountSplittingRanges(
   const size_t cSamples,
   const FloatEbmType * const aSingleFeatureValues,
   const size_t avgLength,
   const size_t cMinimumSamplesPerBin
) noexcept {
   EBM_ASSERT(1 <= cSamples);
   EBM_ASSERT(nullptr != aSingleFeatureValues);
   EBM_ASSERT(1 <= avgLength);
   EBM_ASSERT(1 <= cMinimumSamplesPerBin);

   if(cSamples < (cMinimumSamplesPerBin << 1)) {
      // we can't make any cuts if we have less than 2 * cMinimumSamplesPerBin samples, 
      // since we need at least cMinimumSamplesPerBin samples on either side of the cut point
      return 0;
   }
   FloatEbmType rangeValue = *aSingleFeatureValues;
   const FloatEbmType * pSplittableValuesStart = aSingleFeatureValues;
   const FloatEbmType * pStartEqualRange = aSingleFeatureValues;
   const FloatEbmType * pScan = aSingleFeatureValues + 1;
   const FloatEbmType * const pValuesEnd = aSingleFeatureValues + cSamples;
   size_t cSplittingRanges = 0;
   while(pValuesEnd != pScan) {
      const FloatEbmType val = *pScan;
      if(val != rangeValue) {
         size_t cEqualRangeItems = pScan - pStartEqualRange;
         if(avgLength <= cEqualRangeItems) {
            if(aSingleFeatureValues != pSplittableValuesStart || cMinimumSamplesPerBin <= static_cast<size_t>(pStartEqualRange - pSplittableValuesStart)) {
               ++cSplittingRanges;
            }
            pSplittableValuesStart = pScan;
         }
         rangeValue = val;
         pStartEqualRange = pScan;
      }
      ++pScan;
   }
   if(aSingleFeatureValues == pSplittableValuesStart) {
      EBM_ASSERT(0 == cSplittingRanges);

      // we're still on the first splitting range.  We need to make sure that there is at least one possible cut
      // if we require 3 items for a cut, a problematic range like 0 1 3 3 4 5 could look ok, but we can't cut it in the middle!
      const FloatEbmType * pCheckForSplitPoint = aSingleFeatureValues + cMinimumSamplesPerBin;
      EBM_ASSERT(pCheckForSplitPoint <= pValuesEnd);
      const FloatEbmType * pCheckForSplitPointLast = pValuesEnd - cMinimumSamplesPerBin;
      EBM_ASSERT(aSingleFeatureValues <= pCheckForSplitPointLast);
      EBM_ASSERT(aSingleFeatureValues < pCheckForSplitPoint);
      FloatEbmType checkValue = *(pCheckForSplitPoint - 1);
      while(pCheckForSplitPoint <= pCheckForSplitPointLast) {
         if(checkValue != *pCheckForSplitPoint) {
            return 1;
         }
         ++pCheckForSplitPoint;
      }
      // there's no possible place to split, so return
      return 0;
   } else {
      const size_t cItemsLast = static_cast<size_t>(pValuesEnd - pSplittableValuesStart);
      if(cMinimumSamplesPerBin <= cItemsLast) {
         ++cSplittingRanges;
      }
      return cSplittingRanges;
   }
}

INLINE_RELEASE static size_t GetAvgLength(
   const size_t cSamples, 
   const size_t cMaximumBins, 
   const size_t cMinimumSamplesPerBin
) noexcept {
   EBM_ASSERT(size_t { 1 } <= cSamples);
   EBM_ASSERT(size_t { 2 } <= cMaximumBins); // if there is just one bin, then you can't have splits, so we exit earlier
   EBM_ASSERT(size_t { 1 } <= cMinimumSamplesPerBin);

   // SplittingRanges are ranges of numbers that we have the guaranteed option of making at least one split within.
   // if there is only one SplittingRange, then we have no choice other than make cuts within the one SplittingRange that we're given
   // if there are multiple SplittingRanges, then every SplittingRanges borders at least one long range of equal values which are unsplittable.
   // cuts are a limited resource, so we want to spend them wisely.  If we have N cuts to give out, we'll first want to ensure that we get a cut
   // within each possible SplittingRange, since these things always border long ranges of unsplittable values.
   //
   // BUT, what happens if we have N SplittingRange, but only N-1 cuts to give out.  In that case we would have to make difficult decisions about where
   // to put the cuts
   //
   // To avoid the bad scenario of having to figure out which SplittingRange won't get a cut, we instead ensure that we can never have more SplittingRanges
   // than we have cuts.  This way every SplittingRanges is guaranteed to have at least 1 cut.
   // 
   // If our avgLength is the ceiling of cSamples / cMaximumBins, then we get this guarantee
   // but std::ceil works on floating point numbers, and it is inexact, especially if cSamples is above the point where floating point numbers can't
   // represent all integer values anymore (above 2^52)
   // so, instead of taking the std::ceil, we take the floor instead by just converting it to size_t, then we increment the avgLength until we
   // get our guarantee using integer math.  This gives us a true guarantee that we'll have sufficient cuts to give each SplittingRange at least one cut

   // Example of a bad situation if we took the rounded average of cSamples / cMaximumBins:
   // 20 == cSamples, 9 == cMaximumBins (so 8 cuts).  20 / 9 = 2.22222222222.  std::round(2.222222222) = 2.  So avgLength would be 2 if we rounded 20 / 9
   // but if our data is:
   // 0,0|1,1|2,2|3,3|4,4|5,5|6,6|7,7|8,8|9,9
   // then we get 9 SplittingRanges, but we only have 8 cuts to distribute.  And then we get to somehow choose which SplittingRange gets 0 cuts.
   // a better choice would have been to make avgLength 3 instead, so the ceiling.  Then we'd be guaranteed to have 8 or less SplittingRanges

   // our algorithm has the option of not putting cut points in the first and last SplittingRanges, since they could be cMinimumSamplesPerBin long
   // and have a long set of equal values only on one side, which means that a cut there isn't absolutely required.  We still need to take the ceiling
   // for the avgLength though since it's possible to create arbitrarily high number of missing bins.  We have a test that creates 3 missing bins, thereby
   // testing for the case that we don't give the first and last SplittingRanges an initial cut.  In this case, we're still missing a cut for one of the
   // long ranges that we can't fullfil.

   size_t avgLength = static_cast<size_t>(static_cast<FloatEbmType>(cSamples) / static_cast<FloatEbmType>(cMaximumBins));
   avgLength = UNPREDICTABLE(avgLength < cMinimumSamplesPerBin) ? cMinimumSamplesPerBin : avgLength;
   while(true) {
      if(UNLIKELY(IsMultiplyError(avgLength, cMaximumBins))) {
         // cSamples isn't an overflow (we checked when we entered), so if we've reached an overflow in the multiplication, 
         // then our multiplication result must be larger than cSamples, even though we can't perform it, so we're good
         break;
      }
      if(PREDICTABLE(cSamples <= avgLength * cMaximumBins)) {
         break;
      }
      ++avgLength;
   }
   return avgLength;
}

INLINE_RELEASE static size_t PossiblyRemoveBinForMissing(
   const bool bMissing, 
   const IntEbmType countBinsMax
) noexcept {
   EBM_ASSERT(IntEbmType { 2 } <= countBinsMax);
   size_t cMaximumBins = static_cast<size_t>(countBinsMax);
   if(PREDICTABLE(bMissing)) {
      // if there is a missing value, then we use 0 for the missing value bin, and bump up all other values by 1.  This creates a semi-problem
      // if the number of bins was specified as a power of two like 256, because we now have 257 possible values, and instead of consuming 8
      // bits per value, we're consuming 9.  If we're told to have a maximum of a power of two bins though, in most cases it won't hurt to
      // have one less bin so that we consume less data.  Our countBinsMax is just a maximum afterall, so we can choose to have less bins.
      // BUT, if the user requests 8 bins or less, then don't reduce the number of bins since then we'll be changing the bin size significantly

      size_t cBits = (~size_t { 0 }) ^ ((~size_t { 0 }) >> 1);
      do {
         // if cMaximumBins is a power of two equal to or greater than 16, then reduce the number of bins (it's a maximum after all) to one less so that
         // it's more compressible.  If we have 256 bins, we really want 255 bins and 0 to be the missing value, using 256 values and 1 byte of storage
         // some powers of two aren't compressible, like 2^34, which needs to fit into a 64 bit storage, but we don't want to take a dependency
         // on the size of the storage system, which is system dependent, so we just exclude all powers of two
         if(UNLIKELY(cBits == cMaximumBins)) {
            --cMaximumBins;
            break;
         }
         cBits >>= 1;
         // don't allow shrinkage below 16 bins (8 is the first power of two below 16).  By the time we reach 8 bins, we don't want to reduce this
         // by a complete bin.  We can just use an extra bit for the missing bin
         // if we had shrunk down to 7 bits for non-missing, we would have been able to fit in 21 items per data item instead of 16 for 64 bit systems
      } while(UNLIKELY(0x8 != cBits));
   }
   return cMaximumBins;
}

INLINE_RELEASE static size_t RemoveMissingValuesAndReplaceInfinities(const size_t cSamples, FloatEbmType * const aValues) noexcept {
   // we really don't want to have cut points that are either -infinity or +infinity because these values are 
   // problematic for serialization, cross language compatibility, human understantability, graphing, etc.
   // In some cases though, +-infinity might carry some information that we do want to capture.  In almost all
   // cases though we can put a cut point between -infinity and the smallest value or +infinity and the largest
   // value.  One corner case is if our data has both max_float and +infinity values.  Our binning uses
   // lower inclusive bounds, so a cut value of max_float will include both max_float and +infinity, so if
   // our algorithm decides to put a cut there we'd be in trouble.  We don't want to make the cut +infinity
   // since that violates our no infinity cut point rule above.  A good compromise is to turn +infinity
   // into max_float.  If we do it here, our cutting algorithm won't need to deal with the odd case of indicating
   // a cut and removing it later.  In theory we could separate -infinity and min_float, since a cut value of
   // min_float would separate the two, but we convert -infinity to min_float here for symetry with the positive
   // case and for simplicity.

   FloatEbmType * pCopyFrom = aValues;
   const FloatEbmType * const pValuesEnd = aValues + cSamples;
   do {
      FloatEbmType val = *pCopyFrom;
      if(UNLIKELY(std::isnan(val))) {
         FloatEbmType * pCopyTo = pCopyFrom;
         goto skip_val;
         do {
            val = *pCopyFrom;
            if(PREDICTABLE(!std::isnan(val))) {
               if(UNLIKELY(std::numeric_limits<FloatEbmType>::max() <= val)) {
                  val = std::numeric_limits<FloatEbmType>::max();
               } else if(UNLIKELY(val <= PREDICTABLE(std::numeric_limits<FloatEbmType>::lowest()))) {
                  val = std::numeric_limits<FloatEbmType>::lowest();
               }
               *pCopyTo = val;
               ++pCopyTo;
            }
         skip_val:
            ++pCopyFrom;
         } while(LIKELY(pValuesEnd != pCopyFrom));
         const size_t cSamplesWithoutMissing = pCopyTo - aValues;
         EBM_ASSERT(cSamplesWithoutMissing < cSamples);
         return cSamplesWithoutMissing;
      }
      if(UNLIKELY(std::numeric_limits<FloatEbmType>::max() <= val)) {
         *pCopyFrom = std::numeric_limits<FloatEbmType>::max();
      } else if(UNLIKELY(val <= PREDICTABLE(std::numeric_limits<FloatEbmType>::lowest()))) {
         *pCopyFrom = std::numeric_limits<FloatEbmType>::lowest();
      }
      ++pCopyFrom;
   } while(LIKELY(pValuesEnd != pCopyFrom));
   return cSamples;
}

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION GenerateQuantileCutPoints(
   IntEbmType countSamples,
   FloatEbmType * featureValues,
   IntEbmType countBinsMax,
   IntEbmType countSamplesPerBinMin,
   IntEbmType * countCutPointsReturn,
   FloatEbmType * cutPointsLowerBoundInclusiveReturn,
   IntEbmType * isMissingPresentReturn,
   FloatEbmType * minValueReturn,
   FloatEbmType * maxValueReturn
) {
   EBM_ASSERT(0 <= countSamples);
   EBM_ASSERT(0 == countSamples || nullptr != featureValues);
   EBM_ASSERT(0 <= countBinsMax);
   EBM_ASSERT(0 == countSamples || 0 < countBinsMax); // countBinsMax can only be zero if there are no samples, because otherwise you need a bin
   EBM_ASSERT(0 <= countSamplesPerBinMin);
   EBM_ASSERT(nullptr != countCutPointsReturn);
   EBM_ASSERT(0 == countSamples || countBinsMax <= 1 || nullptr != cutPointsLowerBoundInclusiveReturn);
   EBM_ASSERT(nullptr != isMissingPresentReturn);
   EBM_ASSERT(nullptr != minValueReturn);
   EBM_ASSERT(nullptr != maxValueReturn);

   // TODO: 
   //   - we shouldn't use randomness unless impossible to do otherwise.  choosing the split points isn't that critical to have
   //       variability for.  We can do things like hashing the data, etc to choose random values, and we should REALLY
   //       try to not use randomness, instead using things like index position, etc for that
   //       One option would be to hash the value in a cell and use the hash.  it will be randomly distributed in direction!
   //   - we can't be 100% invariant to the direction the data is presented to us for binning, but we can be 99.99999% sure
   //     by doing a combination of:
   //       1) rounding, when not falling on the center value of a split
   //       2) if we fall on a center value, then prefer the inward direction
   //       3) if we happen to be at the index in the exact center, we can first use neighbouring splits and continue
   //          all the way to both tail ends
   //       4) if all the splits are the same from the center, we can use the values given to us to choose a direction
   //       5) if all of these things fail, we can use a random number

   LOG_N(TraceLevelInfo, "Entered GenerateQuantileCutPoints: countSamples=%" IntEbmTypePrintf 
      ", featureValues=%p, countBinsMax=%" IntEbmTypePrintf ", countSamplesPerBinMin=%" IntEbmTypePrintf 
      ", countCutPointsReturn=%p, cutPointsLowerBoundInclusiveReturn=%p, isMissingPresentReturn=%p, minValueReturn=%p, maxValueReturn=%p", 
      countSamples, 
      static_cast<void *>(featureValues), 
      countBinsMax, 
      countSamplesPerBinMin, 
      static_cast<void *>(countCutPointsReturn),
      static_cast<void *>(cutPointsLowerBoundInclusiveReturn),
      static_cast<void *>(isMissingPresentReturn),
      static_cast<void *>(minValueReturn),
      static_cast<void *>(maxValueReturn)
   );

   if(!IsNumberConvertable<size_t, IntEbmType>(countSamples)) {
      LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints !IsNumberConvertable<size_t, IntEbmType>(countSamples)");
      return 1;
   }

   if(!IsNumberConvertable<size_t, IntEbmType>(countBinsMax)) {
      LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints !IsNumberConvertable<size_t, IntEbmType>(countBinsMax)");
      return 1;
   }

   if(!IsNumberConvertable<size_t, IntEbmType>(countSamplesPerBinMin)) {
      LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints !IsNumberConvertable<size_t, IntEbmType>(countSamplesPerBinMin)");
      return 1;
   }

   const size_t cSamplesIncludingMissingValues = static_cast<size_t>(countSamples);

   if(0 == cSamplesIncludingMissingValues) {
      *countCutPointsReturn = 0;
      *isMissingPresentReturn = EBM_FALSE;
      *minValueReturn = 0;
      *maxValueReturn = 0;
   } else {
      const size_t cSamples = RemoveMissingValuesAndReplaceInfinities(
         cSamplesIncludingMissingValues, 
         featureValues
      );

      const bool bMissing = cSamplesIncludingMissingValues != cSamples;
      *isMissingPresentReturn = bMissing ? EBM_TRUE : EBM_FALSE;

      if(0 == cSamples) {
         *countCutPointsReturn = 0;
         *minValueReturn = 0;
         *maxValueReturn = 0;
      } else {
         FloatEbmType * const pValuesEnd = featureValues + cSamples;
         std::sort(featureValues, pValuesEnd);
         *minValueReturn = featureValues[0];
         *maxValueReturn = pValuesEnd[-1];
         if(countBinsMax <= 1) {
            // if there is only 1 bin, then there can be no cut points, and no point doing any more work here
            *countCutPointsReturn = 0;
         } else {
            const size_t cMinimumSamplesPerBin =
               countSamplesPerBinMin <= IntEbmType { 0 } ? size_t { 1 } : static_cast<size_t>(countSamplesPerBinMin);
            const size_t cMaximumBins = PossiblyRemoveBinForMissing(bMissing, countBinsMax);
            EBM_ASSERT(2 <= cMaximumBins); // if we had just one bin then there would be no cuts and we should have exited above
            const size_t avgLength = GetAvgLength(cSamples, cMaximumBins, cMinimumSamplesPerBin);
            EBM_ASSERT(1 <= avgLength);
            const size_t cSplittingRanges = CountSplittingRanges(cSamples, featureValues, avgLength, cMinimumSamplesPerBin);
            // we GUARANTEE that each SplittingRange can have at least one cut by choosing an avgLength sufficiently long to ensure this property
            EBM_ASSERT(cSplittingRanges < cMaximumBins);
            if(0 == cSplittingRanges) {
               *countCutPointsReturn = 0;
            } else {
               NeighbourJump * const aNeighbourJumps = ConstructJumps(cSamples, featureValues);
               if(nullptr == aNeighbourJumps) {
                  goto exit_error;
               }

               // TODO: limit cMaximumBins to a reasonable number based on the number of samples.
               //       if the user passes us the maximum size_t number, we shouldn't try and allocate
               //       that much memory

               // sometimes cut points will move between SplittingRanges, so we won't know an accurate
               // number of cut points, but we can be sure that we won't exceed the total number of cut points
               // so allocate the same number each time.  Hopefully we'll get back the same memory range each time
               // to avoid memory fragmentation.
               const size_t cSplitPointsMax = cMaximumBins - 1;

               // TODO: review if we still require these extra split point endpoints or not
               const size_t cSplitPointsWithEndpointsMax = cSplitPointsMax + 2; // include storage for the end points
               SplitPoint * const aSplitPoints = EbmMalloc<SplitPoint>(cSplitPointsWithEndpointsMax);

               if(nullptr == aSplitPoints) {
                  free(aNeighbourJumps);
                  goto exit_error;
               }

               RandomStream randomStream;
               randomStream.Initialize(k_randomSeed);

               // do this just once and reuse the random numbers
               FillSplitPointRandom(&randomStream, cSplitPointsWithEndpointsMax, aSplitPoints);

               const size_t cBytesCombined = sizeof(SplittingRange) + sizeof(SplittingRange *);
               if(IsMultiplyError(cSplittingRanges, cBytesCombined)) {
                  free(aSplitPoints);
                  free(aNeighbourJumps);
                  goto exit_error;
               }
               // use the same memory allocation for both the Junction items and the pointers to the junctions that we'll use for sorting
               SplittingRange ** const apSplittingRange = static_cast<SplittingRange **>(EbmMalloc<void>(cSplittingRanges * cBytesCombined));
               if(nullptr == apSplittingRange) {
                  free(aSplitPoints);
                  free(aNeighbourJumps);
                  goto exit_error;
               }
               SplittingRange * const aSplittingRange = reinterpret_cast<SplittingRange *>(apSplittingRange + cSplittingRanges);

               FillSplittingRangePointers(cSplittingRanges, apSplittingRange, aSplittingRange);
               FillSplittingRangeRandom(&randomStream, cSplittingRanges, aSplittingRange);

               FillSplittingRangeBasics(cSamples, featureValues, avgLength, cMinimumSamplesPerBin, cSplittingRanges, aSplittingRange);
               FillSplittingRangeNeighbours(cSamples, featureValues, cSplittingRanges, aSplittingRange);

               const size_t cUsedSplits = FillSplittingRangeRemaining(cSplittingRanges, aSplittingRange);

               const size_t cCutsRemaining = cMaximumBins - 1 - cUsedSplits;

               if(StuffSplitsIntoSplittingRanges(
                  cSplittingRanges,
                  aSplittingRange,
                  cMinimumSamplesPerBin,
                  cCutsRemaining
               )) {
                  free(apSplittingRange);
                  free(aSplitPoints);
                  free(aNeighbourJumps);
                  goto exit_error;
               }

               FloatEbmType * pCutPointsLowerBoundInclusive = cutPointsLowerBoundInclusiveReturn;
               for(size_t i = 0; i < cSplittingRanges; ++i) {
                  SplittingRange * const pSplittingRange = &aSplittingRange[i];
                  size_t cSplits = pSplittingRange->m_cSplitsAssigned;
                  if(0 == pSplittingRange->m_cUnsplittableEitherSideValuesMin) {
                     // our first and last SplittingRanges can either have a long range of equal items on their tail ends
                     // or nothing.  If there is a long range of equal items, then we'll be placing one cut at the tail
                     // end, otherwise we have an implicit cut there and we don't need to use one of our cuts.  It's
                     // like getting a free cut, so increase the number of splits by one if we don't need one cut at the tail
                     // side

                     ++cSplits;
                     if(0 == pSplittingRange->m_cUnsplittableEitherSideValuesMax) {
                        // if there's just one range and there are no long ranges on either end, then one split will create
                        // two ranges, so add 1 more.

                        EBM_ASSERT(1 == cSplittingRanges);

                        ++cSplits;
                     }
                  }
                  // we have splits on our ends (or we've accounted for that by adding theoretical splits), so 
                  // if we had 3 cuts, we'd have 1 cut on each end, and 1 cut in the center, and 2 ranges, so..
                  EBM_ASSERT(1 <= cSplits);
                  const size_t cRanges = cSplits - 1;
                  EBM_ASSERT(0 <= cRanges);
                  if(2 <= cRanges) {
                     // we have splits on our ends, and at least one split in our center, so we have to make decisions
                     try {
                        std::set<SplitPoint *, CompareSplitPoint> bestSplitPoints;

#ifdef NEVER
                        // TODO : in the future fill this priority queue with the average length within our
                        //        visibility window AFTER a new split would be added.  We calculate this value per
                        //        SplitPoint and we do it at the same time we're calculating the split priority, which
                        //        is good since we'll already have the visibility windows calculated and all that.
                        //        One wrinkle is that we want to be able to insert a split into a range that no longer
                        //        has any internal splits.  So for instance if we had a range from 50 to 100 with
                        //        materialized splits on both 50 and 100, and no allocated splits between them, in
                        //        the future if splits become plentiful, then we want to create a new split between
                        //        those materialized splits.  I believe the best way to handle this is to check
                        //        when materializing a split if both our lower and higher split points are aspirational
                        //        or materialized.  If they are both materialized, then insert our new materialized
                        //        split into the open space priority queue AND the split to the left (which represents)
                        //        the lower range.  Of if that's too complicated then take the maximum min from both
                        //        our sides and insert ourselves with that.  We can always examine the left and right
                        //        on extraction to determine which side we should go.
                        //        Inisde CalculateRangesMaximizeMin, we might notice that one of our sides doesn't
                        //        work very well with a certain number of splits.  We should speculatively move
                        //        one of our splits from that side to a new set of ranges (encoded as SplitPoints)
                        //        We still do the low/high split number optimization with our left and right windows
                        //        when planning since it's more efficient 
                        //       
                        std::set<SplitPoint *, CompareSplitPoint> fillTheVoids;
#endif // NEVER

                        // TODO : don't ignore the return value of TradeSplitSegment
                        TradeSplitSegment(
                           &bestSplitPoints,
                           cMinimumSamplesPerBin,
                           pSplittingRange->m_pSplittableValuesFirst - featureValues,
                           pSplittingRange->m_cSplittableValues,
                           aNeighbourJumps,
                           cRanges,
                           // for efficiency we include space for the end point cuts even if they don't exist
                           aSplitPoints
                        );

                        const FloatEbmType * const pSplittableValuesStart = pSplittingRange->m_pSplittableValuesFirst;

                        if(0 != pSplittingRange->m_cUnsplittableLowValues) {
                           // if it's zero then it's an implicit cut and we shouldn't put one there, 
                           // otherwise put in the cut
                           const FloatEbmType * const pCut = pSplittableValuesStart;
                           EBM_ASSERT(featureValues < pCut);
                           EBM_ASSERT(pCut < featureValues + countSamples);
                           const FloatEbmType cut = GetInterpretableCutPointFloat(*(pCut - 1), *pCut);
                           *pCutPointsLowerBoundInclusive = cut;
                           ++pCutPointsLowerBoundInclusive;
                        }

                        const SplitPoint * pSplitPoints = aSplitPoints->m_pNext;
                        while(true) {
                           const SplitPoint * const pNext = pSplitPoints->m_pNext;
                           if(nullptr == pNext) {
                              break;
                           }

                           const size_t iVal = pSplitPoints->m_iVal;
                           if(k_illegalIndex != iVal) {
                              const FloatEbmType * const pCut = pSplittableValuesStart + iVal;
                              EBM_ASSERT(featureValues < pCut);
                              EBM_ASSERT(pCut < featureValues + countSamples);
                              const FloatEbmType cut = GetInterpretableCutPointFloat(*(pCut - 1), *pCut);
                              *pCutPointsLowerBoundInclusive = cut;
                              ++pCutPointsLowerBoundInclusive;
                           }

                           pSplitPoints = pNext;
                        }

                        if(0 != pSplittingRange->m_cUnsplittableHighValues) {
                           // if it's zero then it's an implicit cut and we shouldn't put one there, 
                           // otherwise put in the cut
                           const FloatEbmType * const pCut = 
                              pSplittableValuesStart + pSplittingRange->m_cSplittableValues;
                           EBM_ASSERT(featureValues < pCut);
                           EBM_ASSERT(pCut < featureValues + countSamples);
                           const FloatEbmType cut = GetInterpretableCutPointFloat(*(pCut - 1), *pCut);
                           *pCutPointsLowerBoundInclusive = cut;
                           ++pCutPointsLowerBoundInclusive;
                        }

                     } catch(...) {
                        LOG_0(TraceLevelWarning, "WARNING GenerateQuantileCutPoints exception");
                        free(apSplittingRange);
                        free(aSplitPoints);
                        free(aNeighbourJumps);
                        goto exit_error;
                     }
                  } else if(1 == cRanges) {
                     // we have splits on both our ends (either explicit or implicit), so
                     // we don't have to make any hard decisions, but we do have to be careful of the scenarios
                     // where some of our cuts are implicit

                     if(0 != pSplittingRange->m_cUnsplittableLowValues) {
                        // if it's zero then it's an implicit cut and we shouldn't put one there, 
                        // otherwise put in the cut
                        const FloatEbmType * const pCut = pSplittingRange->m_pSplittableValuesFirst;
                        EBM_ASSERT(featureValues < pCut);
                        EBM_ASSERT(pCut < featureValues + countSamples);
                        const FloatEbmType cut = GetInterpretableCutPointFloat(*(pCut - 1), *pCut);
                        *pCutPointsLowerBoundInclusive = cut;
                        ++pCutPointsLowerBoundInclusive;
                     }
                     if(0 != pSplittingRange->m_cUnsplittableHighValues) {
                        // if it's zero then it's an implicit cut and we shouldn't put one there, 
                        // otherwise put in the cut
                        const FloatEbmType * const pCut = 
                           pSplittingRange->m_pSplittableValuesFirst + pSplittingRange->m_cSplittableValues;
                        EBM_ASSERT(featureValues < pCut);
                        EBM_ASSERT(pCut < featureValues + countSamples);
                        const FloatEbmType cut = GetInterpretableCutPointFloat(*(pCut - 1), *pCut);
                        *pCutPointsLowerBoundInclusive = cut;
                        ++pCutPointsLowerBoundInclusive;
                     }
                  } else {
                     EBM_ASSERT(0 == cRanges);
                     // we have only 1 split to place, and no splits on our boundaries, so we need to figure out
                     // where in our range to place it, taking into consideration that we might have neighbours on our
                     // sides that could be large

                     // if we had implicit cuts on both ends and zero assigned cuts, we'd have 1 range and would
                     // be handled above
                     EBM_ASSERT(0 != pSplittingRange->m_cUnsplittableEitherSideValuesMax);

                     // if one side or the other was an implicit split, then we have zero splits left after
                     // the implicit split is accounted for, so do nothing
                     if(0 != pSplittingRange->m_cUnsplittableEitherSideValuesMin) {
                        // even though we could reduce our squared error length more, it probably makes sense to 
                        // include a little bit of our available numbers on one long range and the other, so let's put
                        // the cut in the middle and only make the low/high decision to settle long-ish ranges
                        // in the center

                        const FloatEbmType * pCut = pSplittingRange->m_pSplittableValuesFirst;
                        const size_t cSplittableItems = pSplittingRange->m_cSplittableValues;
                        if(0 != cSplittableItems) {
                           // if m_cSplittableItems then we get bumped into the neighbour jumps object for our
                           // unsplittable range above, and the next value is way far away from our current splitting
                           // range

                           pCut += cSplittableItems >> 1;

                           const NeighbourJump * const pNeighbourJump =
                              &aNeighbourJumps[pCut - featureValues];

                           const size_t cDistanceLow = pNeighbourJump->m_iStartCur - (pSplittingRange->m_pSplittableValuesFirst - featureValues);
                           const size_t cDistanceHigh = pSplittingRange->m_pSplittableValuesFirst + 
                              cSplittableItems - featureValues - pNeighbourJump->m_iStartNext;

                           if(cDistanceHigh < cDistanceLow) {
                              pCut = featureValues + pNeighbourJump->m_iStartCur;
                           } else if(cDistanceLow < cDistanceHigh) {
                              pCut = featureValues + pNeighbourJump->m_iStartNext;
                           } else {
                              EBM_ASSERT(cDistanceHigh == cDistanceLow);
                              // TODO: for now be lazy and just use the low one, but eventually try and get symetry from order
                              // inversion we should next try and use the distance to the outer splitting range border
                              // then if that's still equal to the full array border, and if that's still equal
                              // then pick it randomly
                              pCut = featureValues + pNeighbourJump->m_iStartCur;
                           }
                        }
                        const FloatEbmType cut = GetInterpretableCutPointFloat(*(pCut - 1), *pCut);
                        *pCutPointsLowerBoundInclusive = cut;
                        ++pCutPointsLowerBoundInclusive;
                     }
                  }
               }

               *countCutPointsReturn = pCutPointsLowerBoundInclusive - cutPointsLowerBoundInclusiveReturn;

               free(apSplittingRange); // both the junctions and the pointers to the junctions are in the same memory allocation

               // first let's tackle the short ranges between big ranges (or at the tails) where we know there will be a split to separate the big ranges to either
               // side, but the short range isn't big enough to split.  In otherwords, there are less than cMinimumSamplesPerBin items
               // we start with the biggest long ranges and essentially try to push whatever mass there is away from them and continue down the list

               free(aSplitPoints);
               free(aNeighbourJumps);
            }
         }
      }
   }
   LOG_N(TraceLevelInfo, "Exited GenerateQuantileCutPoints countCutPoints=%" IntEbmTypePrintf ", isMissing=%" IntEbmTypePrintf,
      *countCutPointsReturn,
      *isMissingPresentReturn
   );
   return 0;

exit_error:;
   LOG_N(TraceLevelWarning, "WARNING GenerateQuantileCutPoints returned %" IntEbmTypePrintf, 1);
   return 1;
}

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION GenerateImprovedEqualWidthCutPoints(
   IntEbmType countSamples,
   FloatEbmType * featureValues,
   IntEbmType countBinsMax,
   IntEbmType * countCutPointsReturn,
   FloatEbmType * cutPointsLowerBoundInclusiveReturn,
   IntEbmType * isMissingPresentReturn,
   FloatEbmType * minValueReturn,
   FloatEbmType * maxValueReturn
) {
   UNUSED(countSamples);
   UNUSED(featureValues);
   UNUSED(countBinsMax);
   UNUSED(countCutPointsReturn);
   UNUSED(cutPointsLowerBoundInclusiveReturn);
   UNUSED(isMissingPresentReturn);
   UNUSED(minValueReturn);
   UNUSED(maxValueReturn);

   // TODO: IMPLEMENT

   return 0;
}

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION GenerateEqualWidthCutPoints(
   IntEbmType countSamples,
   FloatEbmType * featureValues,
   IntEbmType countBinsMax,
   IntEbmType * countCutPointsReturn,
   FloatEbmType * cutPointsLowerBoundInclusiveReturn,
   IntEbmType * isMissingPresentReturn,
   FloatEbmType * minValueReturn,
   FloatEbmType * maxValueReturn
) {
   UNUSED(countSamples);
   UNUSED(featureValues);
   UNUSED(countBinsMax);
   UNUSED(countCutPointsReturn);
   UNUSED(cutPointsLowerBoundInclusiveReturn);
   UNUSED(isMissingPresentReturn);
   UNUSED(minValueReturn);
   UNUSED(maxValueReturn);

   // TODO: IMPLEMENT

   return 0;
}

EBM_NATIVE_IMPORT_EXPORT_BODY void EBM_NATIVE_CALLING_CONVENTION Discretize(
   IntEbmType countSamples,
   const FloatEbmType * featureValues,
   IntEbmType countCutPoints,
   const FloatEbmType * cutPointsLowerBoundInclusive,
   IntEbmType * discretizedReturn
) {
   EBM_ASSERT(0 <= countSamples);
   EBM_ASSERT((IsNumberConvertable<size_t, IntEbmType>(countSamples))); // this needs to point to real memory, otherwise it's invalid
   EBM_ASSERT(0 == countSamples || nullptr != featureValues);
   EBM_ASSERT(0 <= countCutPoints);
   EBM_ASSERT((IsNumberConvertable<size_t, IntEbmType>(countCutPoints))); // this needs to point to real memory, otherwise it's invalid
   EBM_ASSERT(0 == countSamples || 0 == countCutPoints || nullptr != cutPointsLowerBoundInclusive);
   EBM_ASSERT(0 == countSamples || nullptr != discretizedReturn);

   if(IntEbmType { 0 } < countSamples) {
      const size_t cCutPoints = static_cast<size_t>(countCutPoints);
#ifndef NDEBUG
      for(size_t iDebug = 1; iDebug < cCutPoints; ++iDebug) {
         EBM_ASSERT(cutPointsLowerBoundInclusive[iDebug - 1] < cutPointsLowerBoundInclusive[iDebug]);
      }
# endif // NDEBUG
      const size_t cSamples = static_cast<size_t>(countSamples);
      const FloatEbmType * pValue = featureValues;
      const FloatEbmType * const pValueEnd = featureValues + cSamples;
      IntEbmType * pDiscretized = discretizedReturn;

      if(size_t { 0 } == cCutPoints) {
         do {
            const FloatEbmType val = *pValue;
            const IntEbmType result = UNPREDICTABLE(std::isnan(val)) ? IntEbmType { 1 } : IntEbmType { 0 };
            *pDiscretized = result;
            ++pDiscretized;
            ++pValue;
         } while(LIKELY(pValueEnd != pValue));
      } else {
         const ptrdiff_t missingVal = static_cast<ptrdiff_t>(cCutPoints + size_t { 1 });
         const ptrdiff_t highStart = static_cast<ptrdiff_t>(cCutPoints - size_t { 1 });
         do {
            const FloatEbmType val = *pValue;
            ptrdiff_t middle = missingVal;
            if(!std::isnan(val)) {
               ptrdiff_t high = highStart;
               ptrdiff_t low = 0;
               FloatEbmType midVal;
               do {
                  middle = (low + high) >> 1;
                  EBM_ASSERT(ptrdiff_t { 0 } <= middle && static_cast<size_t>(middle) < cCutPoints);
                  midVal = cutPointsLowerBoundInclusive[static_cast<size_t>(middle)];
                  high = UNPREDICTABLE(midVal <= val) ? high : middle - ptrdiff_t { 1 };
                  low = UNPREDICTABLE(midVal <= val) ? middle + ptrdiff_t { 1 } : low;
               } while(LIKELY(low <= high));
               middle = UNPREDICTABLE(midVal <= val) ? middle + ptrdiff_t { 1 } : middle;
               EBM_ASSERT(ptrdiff_t { 0 } <= middle && middle <= static_cast<ptrdiff_t>(cCutPoints));
            }
            *pDiscretized = static_cast<IntEbmType>(middle);
            ++pDiscretized;
            ++pValue;
         } while(LIKELY(pValueEnd != pValue));
      }
   }
}
