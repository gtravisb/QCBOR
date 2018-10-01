/*==============================================================================
 Copyright 2018 Laurence Lundblade
 
 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:
 
 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 
 (This is the MIT license)
 ==============================================================================*/

//
//  ieee754.c
//  Indefinite
//
//  Created by Laurence Lundblade on 7/23/18.
//  Copyright © 2018 Laurence Lundblade. All rights reserved.
//

#include "ieee754.h"
#include <string.h> // For memcpy()


/*
 This code is written for clarity and verifiability, not for size, on the assumption
 that the optimizer will do a good job. The LLVM optimizer, -Os, does seem to do the
 job and the resulting object code is smaller from combining code for the many different
 cases (normal, subnormal, infinity, zero...) for the conversions.
 
 Dead stripping is also really helpful to get code size down.
 
 This code also works solely using shifts and masks and thus has no dependency on
 any math libraries. It will even work if the CPU doesn't have any floating
 point support.
 
 The memcpy() dependency is only for CopyFloatToUint32() and friends which only
 is needed to avoid type punning when converting the actual float bits to
 an unsigned value so the bit shifts and masks can work.
 */

/*
 The references used to write this code:
 
 - IEEE 754-2008, particularly section 3.6 and 6.2.1
 
 - https://en.wikipedia.org/wiki/IEEE_754 and subordinate pages
 
 - https://stackoverflow.com/questions/19800415/why-does-ieee-754-reserve-so-many-nan-values
 */


// ----- Half Precsion -----------
#define HALF_NUM_SIGNIFICAND_BITS (10)
#define HALF_NUM_EXPONENT_BITS    (5)
#define HALF_NUM_SIGN_BITS        (1)

#define HALF_SIGNIFICAND_SHIFT    (0)
#define HALF_EXPONENT_SHIFT       (HALF_NUM_SIGNIFICAND_BITS)
#define HALF_SIGN_SHIFT           (HALF_NUM_SIGNIFICAND_BITS + HALF_NUM_EXPONENT_BITS)

#define HALF_SIGNIFICAND_MASK     (0x3ff) // The lower 10 bits  // 0x03ff
#define HALF_EXPONENT_MASK        (0x1f << HALF_EXPONENT_SHIFT) // 0x7c00 5 bits of exponent
#define HALF_SIGN_MASK            (0x01 << HALF_SIGN_SHIFT) //  // 0x80001 bit of sign
#define HALF_QUIET_NAN_BIT        (0x01 << (HALF_NUM_SIGNIFICAND_BITS-1)) // 0x0200

/* Biased    Biased    Unbiased   Use
    0x00       0        -15       0 and subnormal
    0x01       1        -14       Smallest normal exponent
    0x1e      30         15       Largest normal exponent
    0x1F      31         16       NaN and Infinity  */
#define HALF_EXPONENT_BIAS        (15)
#define HALF_EXPONENT_MAX         (HALF_EXPONENT_BIAS)    //  15 Unbiased
#define HALF_EXPONENT_MIN         (-HALF_EXPONENT_BIAS+1) // -14 Unbiased
#define HALF_EXPONENT_ZERO        (-HALF_EXPONENT_BIAS)   // -15 Unbiased
#define HALF_EXPONENT_INF_OR_NAN  (HALF_EXPONENT_BIAS+1)  //  16 Unbiased


// ------ Single Precision --------
#define SINGLE_NUM_SIGNIFICAND_BITS (23)
#define SINGLE_NUM_EXPONENT_BITS    (8)
#define SINGLE_NUM_SIGN_BITS        (1)

#define SINGLE_SIGNIFICAND_SHIFT    (0)
#define SINGLE_EXPONENT_SHIFT       (SINGLE_NUM_SIGNIFICAND_BITS)
#define SINGLE_SIGN_SHIFT           (SINGLE_NUM_SIGNIFICAND_BITS + SINGLE_NUM_EXPONENT_BITS)

#define SINGLE_SIGNIFICAND_MASK     (0x7fffffUL) // The lower 23 bits
#define SINGLE_EXPONENT_MASK        (0xffUL << SINGLE_EXPONENT_SHIFT) // 8 bits of exponent
#define SINGLE_SIGN_MASK            (0x01UL << SINGLE_SIGN_SHIFT) // 1 bit of sign
#define SINGLE_QUIET_NAN_BIT        (0x01UL << (SINGLE_NUM_SIGNIFICAND_BITS-1))

/* Biased  Biased   Unbiased  Use
    0x0000     0     -127      0 and subnormal
    0x0001     1     -126      Smallest normal exponent
    0x7f     127        0      1
    0xfe     254      127      Largest normal exponent
    0xff     255      128      NaN and Infinity  */
#define SINGLE_EXPONENT_BIAS        (127)
#define SINGLE_EXPONENT_MAX         (SINGLE_EXPONENT_BIAS)    // 127  unbiased
#define SINGLE_EXPONENT_MIN         (-SINGLE_EXPONENT_BIAS+1) // -126 unbiased
#define SINGLE_EXPONENT_ZERO        (-SINGLE_EXPONENT_BIAS)   // -127 unbiased
#define SINGLE_EXPONENT_INF_OR_NAN  (SINGLE_EXPONENT_BIAS+1)  // 128  unbiased


// --------- Double Precision ----------
#define DOUBLE_NUM_SIGNIFICAND_BITS (52)
#define DOUBLE_NUM_EXPONENT_BITS    (11)
#define DOUBLE_NUM_SIGN_BITS        (1)

#define DOUBLE_SIGNIFICAND_SHIFT    (0)
#define DOUBLE_EXPONENT_SHIFT       (DOUBLE_NUM_SIGNIFICAND_BITS)
#define DOUBLE_SIGN_SHIFT           (DOUBLE_NUM_SIGNIFICAND_BITS + DOUBLE_NUM_EXPONENT_BITS)

#define DOUBLE_SIGNIFICAND_MASK     (0xfffffffffffffULL) // The lower 52 bits
#define DOUBLE_EXPONENT_MASK        (0x7ffULL << DOUBLE_EXPONENT_SHIFT) // 11 bits of exponent
#define DOUBLE_SIGN_MASK            (0x01ULL << DOUBLE_SIGN_SHIFT) // 1 bit of sign
#define DOUBLE_QUIET_NAN_BIT        (0x01ULL << (DOUBLE_NUM_SIGNIFICAND_BITS-1))


/* Biased      Biased   Unbiased  Use
   0x00000000     0     -1023     0 and subnormal
   0x00000001     1     -1022     Smallest normal exponent
   0x000007fe  2046      1023     Largest normal exponent
   0x000007ff  2047      1024     NaN and Infinity  */
#define DOUBLE_EXPONENT_BIAS        (1023)
#define DOUBLE_EXPONENT_MAX         (DOUBLE_EXPONENT_BIAS)    // unbiased
#define DOUBLE_EXPONENT_MIN         (-DOUBLE_EXPONENT_BIAS+1) // unbiased
#define DOUBLE_EXPONENT_ZERO        (-DOUBLE_EXPONENT_BIAS)   // unbiased
#define DOUBLE_EXPONENT_INF_OR_NAN  (DOUBLE_EXPONENT_BIAS+1)  // unbiased



/*
 Convenient functions to avoid type punning, compiler warnings and such
 The optimizer reduces them to a simple assignment.
 This is a crusty corner of C. It shouldn't be this hard.
 
 These are also in UsefulBuf.h under a different name. They are copied
 here because to avoid a dependency on UsefulBuf.h. There is no
 object code size impact because these always optimze down to a
 simple assignment.
 */
static inline uint32_t CopyFloatToUint32(float f)
{
    uint32_t u32;
    memcpy(&u32, &f, sizeof(uint32_t));
    return u32;
}

static inline uint64_t CopyDoubleToUint64(double d)
{
    uint64_t u64;
    memcpy(&u64, &d, sizeof(uint64_t));
    return u64;
}

static inline double CopyUint64ToDouble(uint64_t u64)
{
    double d;
    memcpy(&d, &u64, sizeof(uint64_t));
    return d;
}

static inline float CopyUint32ToFloat(uint32_t u32)
{
    float f;
    memcpy(&f, &u32, sizeof(uint32_t));
    return f;
}



// Public function; see ieee754.h
uint16_t IEEE754_FloatToHalf(float f)
{
    // Pull the three parts out of the single-precision float
    const uint32_t uSingle = CopyFloatToUint32(f);
    const int32_t  nSingleUnbiasedExponent = ((uSingle & SINGLE_EXPONENT_MASK) >> SINGLE_EXPONENT_SHIFT) - SINGLE_EXPONENT_BIAS;
    const uint32_t uSingleSign             =  (uSingle & SINGLE_SIGN_MASK) >> SINGLE_SIGN_SHIFT;
    const uint32_t uSingleSignificand      =   uSingle & SINGLE_SIGNIFICAND_MASK;
    
    
    // Now convert the three parts to half-precision.
    uint16_t uHalfSign, uHalfSignificand, uHalfBiasedExponent;
    if(nSingleUnbiasedExponent == SINGLE_EXPONENT_INF_OR_NAN) {
        // +/- Infinity and NaNs -- single biased exponent is 0xff
        uHalfBiasedExponent = HALF_EXPONENT_INF_OR_NAN + HALF_EXPONENT_BIAS;
        if(!uSingleSignificand) {
            // Infinity
            uHalfSignificand = 0;
        } else {
            // Copy the LBSs of the NaN payload that will fit from the single to the half
            uHalfSignificand = uSingleSignificand & (HALF_SIGNIFICAND_MASK & ~HALF_QUIET_NAN_BIT);
            if(uSingleSignificand & SINGLE_QUIET_NAN_BIT) {
                // It's a qNaN; copy the qNaN bit
                uHalfSignificand |= HALF_QUIET_NAN_BIT;
            } else {
                // It's a sNaN; make sure the significand is not zero so it stays a NaN
                // This is needed because not all significand bits are copied from single
                if(!uHalfSignificand) {
                    // Set the LSB. This is what wikipedia shows for sNAN.
                    uHalfSignificand |= 0x01;
                }
            }
        }
    } else if(nSingleUnbiasedExponent == SINGLE_EXPONENT_ZERO) {
        // 0 or a subnormal number  -- singled biased exponent is 0
        uHalfBiasedExponent = 0;
        uHalfSignificand    = 0; // Any subnormal single will be too small to express as a half precision
    } else if(nSingleUnbiasedExponent > HALF_EXPONENT_MAX) {
        // Exponent is too large to express in half-precision; round up to infinity
        uHalfBiasedExponent = HALF_EXPONENT_INF_OR_NAN + HALF_EXPONENT_BIAS;
        uHalfSignificand    = 0;
    } else if(nSingleUnbiasedExponent < HALF_EXPONENT_MIN) {
        // Exponent is too small to express in half-precision normal; make it a half-precision subnormal
        uHalfBiasedExponent = (uint16_t)(HALF_EXPONENT_ZERO + HALF_EXPONENT_BIAS);
        // Difference between single normal exponent and the base exponent of a half subnormal
        const uint32_t nExpDiff = -(nSingleUnbiasedExponent - HALF_EXPONENT_MIN);
        // Also have to shift the significand by the difference in number of bits between a single and a half significand
        const int32_t nSignificandBitsDiff = SINGLE_NUM_SIGNIFICAND_BITS - HALF_NUM_SIGNIFICAND_BITS;
        // Add in the 1 that is implied in the significand of a normal number; it needs to be present in a subnormal
        const uint32_t uSingleSignificandSubnormal = uSingleSignificand + (0x01L << SINGLE_NUM_SIGNIFICAND_BITS);
        uHalfSignificand = uSingleSignificandSubnormal >> (nExpDiff + nSignificandBitsDiff);
    } else {
        // The normal case
        uHalfBiasedExponent = nSingleUnbiasedExponent + HALF_EXPONENT_BIAS;
        uHalfSignificand    = uSingleSignificand >> (SINGLE_NUM_SIGNIFICAND_BITS - HALF_NUM_SIGNIFICAND_BITS);
    }
    uHalfSign = uSingleSign;

    // Put the 3 values in the right place for a half precision
    const uint16_t uHalfPrecision =  uHalfSignificand |
                                    (uHalfBiasedExponent << HALF_EXPONENT_SHIFT) |
                                    (uHalfSign << HALF_SIGN_SHIFT);
    return uHalfPrecision;
}


// Public function; see ieee754.h
uint16_t IEEE754_DoubleToHalf(double d)
{
    // Pull the three parts out of the double-precision float
    const uint64_t uDouble = CopyDoubleToUint64(d);
    const int64_t  nDoubleUnbiasedExponent = ((uDouble & DOUBLE_EXPONENT_MASK) >> DOUBLE_EXPONENT_SHIFT) - DOUBLE_EXPONENT_BIAS;
    const uint64_t uDoubleSign             =  (uDouble & DOUBLE_SIGN_MASK) >> DOUBLE_SIGN_SHIFT;
    const uint64_t uDoubleSignificand      =   uDouble & DOUBLE_SIGNIFICAND_MASK;

    
    // Now convert the three parts to half-precision.
    uint16_t uHalfSign, uHalfSignificand, uHalfBiasedExponent;
    if(nDoubleUnbiasedExponent == DOUBLE_EXPONENT_INF_OR_NAN) {
        // +/- Infinity and NaNs -- single biased exponent is 0xff
        uHalfBiasedExponent = HALF_EXPONENT_INF_OR_NAN + HALF_EXPONENT_BIAS;
        if(!uDoubleSignificand) {
            // Infinity
            uHalfSignificand = 0;
        } else {
            // Copy the LBSs of the NaN payload that will fit from the double to the half
            uHalfSignificand = uDoubleSignificand & (HALF_SIGNIFICAND_MASK & ~HALF_QUIET_NAN_BIT);
            if(uDoubleSignificand & DOUBLE_QUIET_NAN_BIT) {
                // It's a qNaN; copy the qNaN bit
                uHalfSignificand |= HALF_QUIET_NAN_BIT;
            } else {
                // It's an sNaN; make sure the significand is not zero so it stays a NaN
                // This is needed because not all significand bits are copied from single
                if(!uHalfSignificand) {
                    // Set the LSB. This is what wikipedia shows for sNAN.
                    uHalfSignificand |= 0x01;
                }
            }
        }
    } else if(nDoubleUnbiasedExponent == DOUBLE_EXPONENT_ZERO) {
        // 0 or a subnormal number  -- double biased exponent is 0
        uHalfBiasedExponent = 0;
        uHalfSignificand    = 0; // Any subnormal single will be too small to express as a half precision; TODO, is this really true?
    } else if(nDoubleUnbiasedExponent > HALF_EXPONENT_MAX) {
        // Exponent is too large to express in half-precision; round up to infinity; TODO, is this really true?
        uHalfBiasedExponent = HALF_EXPONENT_INF_OR_NAN + HALF_EXPONENT_BIAS;
        uHalfSignificand    = 0;
    } else if(nDoubleUnbiasedExponent < HALF_EXPONENT_MIN) {
        // Exponent is too small to express in half-precision; round down to zero
        uHalfBiasedExponent = (uint16_t)(HALF_EXPONENT_ZERO + HALF_EXPONENT_BIAS);
        // Difference between double normal exponent and the base exponent of a half subnormal
        const uint64_t nExpDiff = -(nDoubleUnbiasedExponent - HALF_EXPONENT_MIN);
        // Also have to shift the significand by the difference in number of bits between a double and a half significand
        const int64_t nSignificandBitsDiff = DOUBLE_NUM_SIGNIFICAND_BITS - HALF_NUM_SIGNIFICAND_BITS;
        // Add in the 1 that is implied in the significand of a normal number; it needs to be present in a subnormal
        const uint64_t uDoubleSignificandSubnormal = uDoubleSignificand + (0x01L << DOUBLE_NUM_SIGNIFICAND_BITS);
        uHalfSignificand = uDoubleSignificandSubnormal >> (nExpDiff + nSignificandBitsDiff);
    } else {
        // The normal case
        uHalfBiasedExponent = nDoubleUnbiasedExponent + HALF_EXPONENT_BIAS;
        uHalfSignificand    = uDoubleSignificand >> (DOUBLE_NUM_SIGNIFICAND_BITS - HALF_NUM_SIGNIFICAND_BITS);
    }
    uHalfSign = uDoubleSign;
    
    
    // Put the 3 values in the right place for a half precision
    const uint16_t uHalfPrecision =  uHalfSignificand |
                                    (uHalfBiasedExponent << HALF_EXPONENT_SHIFT) |
                                    (uHalfSign << HALF_SIGN_SHIFT);
    return uHalfPrecision;
}


// Public function; see ieee754.h
float IEEE754_HalfToFloat(uint16_t uHalfPrecision)
{
    // Pull out the three parts of the half-precision float
    const uint16_t uHalfSignificand      =   uHalfPrecision & HALF_SIGNIFICAND_MASK;
    const int16_t  nHalfUnBiasedExponent = ((uHalfPrecision & HALF_EXPONENT_MASK) >> HALF_EXPONENT_SHIFT) - HALF_EXPONENT_BIAS;
    const uint16_t uHalfSign             =  (uHalfPrecision & HALF_SIGN_MASK) >> HALF_SIGN_SHIFT;
    
    
    // Make the three parts of the single-precision number
    uint32_t uSingleSignificand, uSingleSign, uSingleBiasedExponent;
    if(nHalfUnBiasedExponent == HALF_EXPONENT_ZERO) {
        // 0 or subnormal
        if(uHalfSignificand) {
            // Subnormal case
            uSingleBiasedExponent = -HALF_EXPONENT_BIAS + SINGLE_EXPONENT_BIAS +1;
            // A half-precision subnormal can always be converted to a normal single-precision float because the ranges line up
            uSingleSignificand = uHalfSignificand;
            // Shift bits from right of the decimal to left, reducing the exponent by 1 each time
            do {
                uSingleSignificand <<= 1;
                uSingleBiasedExponent--;
            } while ((uSingleSignificand & 0x400) == 0);
            uSingleSignificand &= HALF_SIGNIFICAND_MASK;
            uSingleSignificand <<= (SINGLE_NUM_SIGNIFICAND_BITS - HALF_NUM_SIGNIFICAND_BITS);
        } else {
            // Just zero
            uSingleBiasedExponent = SINGLE_EXPONENT_ZERO + SINGLE_EXPONENT_BIAS;
            uSingleSignificand = 0;
        }
    } else if(nHalfUnBiasedExponent == HALF_EXPONENT_INF_OR_NAN) {
        // NaN or Inifinity
        uSingleBiasedExponent = SINGLE_EXPONENT_INF_OR_NAN + SINGLE_EXPONENT_BIAS;
        if(uHalfSignificand) {
            // NaN
            // First preserve the NaN payload from half to single
            uSingleSignificand = uHalfSignificand & ~HALF_QUIET_NAN_BIT;
            if(uHalfSignificand & HALF_QUIET_NAN_BIT) {
                // Next, set qNaN if needed since half qNaN bit is not copied above
                uSingleSignificand |= SINGLE_QUIET_NAN_BIT;
            }
        } else {
            // Infinity
            uSingleSignificand = 0;
        }
    } else {
        // Normal number
        uSingleBiasedExponent = nHalfUnBiasedExponent + SINGLE_EXPONENT_BIAS;
        uSingleSignificand = uHalfSignificand << (SINGLE_NUM_SIGNIFICAND_BITS - HALF_NUM_SIGNIFICAND_BITS);
    }
    uSingleSign = uHalfSign;
    
    
    // Shift the three parts of the single precision into place
    const uint32_t uSinglePrecision = uSingleSignificand |
                                     (uSingleBiasedExponent << SINGLE_EXPONENT_SHIFT) |
                                     (uSingleSign << SINGLE_SIGN_SHIFT);
    
    return CopyUint32ToFloat(uSinglePrecision);
}


/*
 double IEEE754_HalfToDouble(uint16_t uHalfPrecision) is not needed
*/



// Public function; see ieee754.h
IEEE754_union IEEE754_FloatToSmallest(float f)
{
    IEEE754_union result;
    
    // Pull the neeed two parts out of the single-precision float
    const uint32_t uSingle = CopyFloatToUint32(f);
    const int32_t  nSingleExponent    = ((uSingle & SINGLE_EXPONENT_MASK) >> SINGLE_EXPONENT_SHIFT) - SINGLE_EXPONENT_BIAS;
    const uint32_t uSingleSignificand =   uSingle & SINGLE_SIGNIFICAND_MASK;
    
    // Bit mask that is the significand bits that would be lost when converting
    // from single-precision to half-precision
    const uint64_t uDroppedSingleBits = SINGLE_SIGNIFICAND_MASK >> HALF_NUM_SIGNIFICAND_BITS;

    // Optimizer will re organize so there is only one call to IEEE754_FloatToHalf()
    if(uSingle == 0) {
        // Value is 0.0000, not a a subnormal
        result.uTag = IEEE754_UNION_IS_HALF;
        result.u16  = IEEE754_FloatToHalf(f);
    } else if(nSingleExponent == SINGLE_EXPONENT_INF_OR_NAN) {
        // NaN, +/- infinity
        result.uTag = IEEE754_UNION_IS_HALF;
        result.u16  = IEEE754_FloatToHalf(f);
    } else if((nSingleExponent >= HALF_EXPONENT_MIN) && nSingleExponent <= HALF_EXPONENT_MAX && (!(uSingleSignificand & uDroppedSingleBits))) {
        // Normal number in exponent range and precision won't be lost
        result.uTag = IEEE754_UNION_IS_HALF;
        result.u16  = IEEE754_FloatToHalf(f);
    } else {
        // Subnormal, exponent out of range, or precision will be lost
        result.uTag = IEEE754_UNION_IS_SINGLE;
        result.u32  = uSingle;
    }
    
    return result;
}

// Public function; see ieee754.h
IEEE754_union IEEE754_DoubleToSmallestInternal(double d, int bAllowHalfPrecision)
{
    IEEE754_union result;
    
    // Pull the needed two parts out of the double-precision float
    const uint64_t uDouble = CopyDoubleToUint64(d);
    const int64_t  nDoubleExponent     = ((uDouble & DOUBLE_EXPONENT_MASK) >> DOUBLE_EXPONENT_SHIFT) - DOUBLE_EXPONENT_BIAS;
    const uint64_t uDoubleSignificand  =   uDouble & DOUBLE_SIGNIFICAND_MASK;
    
    // Masks to check whether dropped significand bits are zero or not
    const uint64_t uDroppedDoubleBits = DOUBLE_SIGNIFICAND_MASK >> HALF_NUM_SIGNIFICAND_BITS;
    const uint64_t uDroppedSingleBits = DOUBLE_SIGNIFICAND_MASK >> SINGLE_NUM_SIGNIFICAND_BITS;
        
    // The various cases
    if(d == 0.0) { // Take care of positive and negative zero
        // Value is 0.0000, not a a subnormal
        result.uTag = IEEE754_UNION_IS_HALF;
        result.u16  = IEEE754_DoubleToHalf(d);
    } else if(nDoubleExponent == DOUBLE_EXPONENT_INF_OR_NAN) {
        // NaN, +/- infinity
        result.uTag = IEEE754_UNION_IS_HALF;
        result.u16  = IEEE754_DoubleToHalf(d);
    } else if(bAllowHalfPrecision && (nDoubleExponent >= HALF_EXPONENT_MIN) && nDoubleExponent <= HALF_EXPONENT_MAX && (!(uDoubleSignificand & uDroppedDoubleBits))) {
        // Can convert to half without precision loss
        result.uTag = IEEE754_UNION_IS_HALF;
        result.u16  = IEEE754_DoubleToHalf(d);
    } else if((nDoubleExponent >= SINGLE_EXPONENT_MIN) && nDoubleExponent <= SINGLE_EXPONENT_MAX && (!(uDoubleSignificand & uDroppedSingleBits))) {
        // Can convert to single without precision loss
        result.uTag = IEEE754_UNION_IS_SINGLE;
        result.u32  = CopyFloatToUint32((float)d);
    } else {
        // Can't convert without precision loss
        result.uTag = IEEE754_UNION_IS_DOUBLE;
        result.u64  = uDouble;
    }
    
    return result;
}
