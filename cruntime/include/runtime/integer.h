#pragma once
#ifndef __INTEGER_H__
#define __INTEGER_H__
/*---------------------------------------------------------------------------
  Copyright 2020 Daan Leijen, Microsoft Corporation.

  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the file "license.txt" at the root of this distribution.
---------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------------------------------
Integers are always boxed: and either a pointer to a `bigint_t` (with lowest bit == 0), 
or a boxed (small) int (with lowest bit == 1).

The boxed small int is restricted in size to SMALLINT_BITS such that we can do
efficient arithmetic on the boxed representation of a small int directly where
`boxed(n) == 4*n + 1`. The `smallint_t` size is chosen to allow efficient overflow detection.
By using `4*n + 1` we always have the lowest two bits of a pointer as `00` 
while those of a smallint_t are always `01`.

This way we can do more efficient basic arithmetic where we can for example directly add 
and test afterwards if we actually added two small integers (and not two pointers)
and whether there was an overflow. For example,

    intptr_t integer_add(intptr_t x, intptr_t y) {
      intptr_t z;
      if (unlikely(__builtin_add_overflow(x,y,&z) || (z&2)==0)) return integer_add_generic(x,y);
      return (z^3);  // or `z - 1`
    }

Here we test afterwards if we actually added 2 integers and not two pointers.
If we add, the last 2 bits are:

     x + y = z
    00  00  00    ptr + ptr
    00  01  01    ptr + int
    01  00  01    int + ptr
    01  01  10    int + int

so the test `(z&0x02) == 0` checks if we added 2 integers.
Finally, we subtract 1 (== ^3 in this case) to normalize the integer again. 
With gcc on x86-64 we get:

integer_add(long x, long y)
        mov     rax, rdi   // move  `x` to eax
        add     rax, rsi   // add `y`
        jo      .L7        // on overflow goto slow
        and     rax, 2     // check bit 1 of the result
        je      .L7        // if it is zero, goto slow
        xor     rax, 3     // normalize back to an integer (clear bit 1, set bit 0)
        ret
.L7:
        jmp     integer_add_generic(long, long)

However, not all compilers have a `__builtin_add_overflow`, and it is not always 
compiled well either. For example, the Intel C compiler generates:

integer_add(long x, long y)
        mov       rax, rdi                                      #22.20
        xor       edx, edx                                      #22.20
        add       rax, rsi                                      #22.20
        seto      dl                                            #22.20
        test      dl, dl                                        #56.8
        jne       ..B2.4        # Prob 9%                       #56.8
        test      rax, 2                                        #56.41
        je        ..B2.4        # Prob 29%                      #56.45
        xor       rax, 3                                        #56.58
        ret                                                     #56.58
..B2.4:                         # Preds ..B2.2 ..B2.1
        jmp       integer_add_generic(long, long)


However, we can also test in portable way, and do it with just a single test!
We can do that by limiting smallint_t to a half-word. 
We then use a full-word add and see if the sign-extended lower half-word 
equals the full-word (and thus didn't overflow). 
This also allows us to combine that test with testing
if we added two small integers, (where bit 1 must ==1 after an addition):

    intptr_t integer_add(intptr_t x, intptr_t y) {
      intptr_t z = x + y;
      if (likely(z == (int32_t)(z|2))) return (z^3);
                                  else return integer_add_generic(x,y);
    }

Now we have just one test that test both for overflow, as well as for the
small integers. This gives with clang (and gcc/msvc/icc) on x86-64:

integer_add(long x, long y)
        lea     rax, [rdi+rsi]        // add into rax
        movsxd  rcx, eax              // sign extend lower 32-bits to rcx
        or      rcx, 2                // set bit 1 to 1
        cmp     rax, rcx              // rax == rcx ?
        jne     .L28                  // if not, we have an overflow or added a pointer
        xor     rax, 3                // clear bit 1, set bit 0
        ret
.L28:
        jmp     integer_add_generic

on RISC-V with gcc we get:

integer_add(long x, long y)
        add     a4,a0,a1       // add into a4
        ori     a5,a4,2        // a5 = a4|2
        sext.w  a5,a5          // sign extend
        bne     a5,a4,.L30     // a5 == a4 ? if not, overflow or pointer add
        xori    a0,a5,3        // clear bit 1, set bit 0
        ret
.L30:
        tail    integer_add_generic


on ARM-v8 with gcc; using __builtin_add_overflow, we have:

        add     x2, x0, x1    // x2 = x0 + x1
        eon     x3, x0, x1    // x3 = ~(x0^x1)
        eor     x4, x2, x1    // x4 = x2^x1
        tst     x4, x3        // x4 & x3
        bmi     .L6           // branch if negative
        tbz     x2, 1, .L6    // test bit 1 == 0
        eor     x0, x3, 3     // x0 = x3^3
        ret
.L6:
        b       add_slow(long, long)

and in the portable way:

        add     x3, x0, x1   // x3 = x0 + x1
        orr     w2, w3, 2    // w2 = w3|2
        sxtw    x2, w2       // sign extend w2 to x2
        cmp     x2, x3       // x2 == x3?
        bne     .L32         // if not, goto slow
        eor     x0, x2, 3    // x0 = x2^3
        ret
.L32:
        b       add_slow(long, long)
        
So, overall, the portable way seems to always be better with a single test
but can only use a half-word for small integers. We make it a define so we can 
measure the impact on specific platforms.
--------------------------------------------------------------------------------------------------*/

#if defined(__GNUC__)
#define USE_BUILTIN_OVF (1)
#endif

#ifndef USE_BUILTIN_OVF
#define USE_BUILTIN_OVF (0)       // default to portable overflow detection
#endif

#if USE_BUILTIN_OVF
typedef intptr_t smallint_t
#define SMALLINT_BITS  (INTPTR_BITS)
#elif INTPTR_SIZE==8
typedef int32_t smallint_t;
#define SMALLINT_BITS  (32)
#elif INTPTR_SIZE==4
typedef int16_t smallint_t;
#define SMALLINT_BITS  (16)
#else
# error "platform must be 32 or 64 bits."
#endif

#define SMALLINT_MAX  ((intptr_t)(((uintptr_t)INTPTR_MAX >> (INTPTR_BITS - SMALLINT_BITS)) >> 2))  // use unsigned shift to avoid UB
#define SMALLINT_MIN  (-SMALLINT_MAX - 1)

static inline intx_t smallint_from_integer(integer_t i) {  // use for known small ints
  assert_internal(is_value(i) && (i.box&0x03)==0x01);
  return sar(unbox_int(i), 1);
}

static inline integer_t integer_from_small(intptr_t i) {   // use for known small int constants (at most 14 bits)
  assert_internal(i >= SMALLINT_MIN && i <= SMALLINT_MAX);
  return box_int((i<<1));
}

static inline bool is_integer(integer_t i) {
  return ((is_value(i) && smallint_from_integer(i) >= SMALLINT_MIN && smallint_from_integer(i) <= SMALLINT_MAX) 
         || (is_ptr(i) && block_tag(unbox_ptr(i)) == TAG_BIGINT));
}

static inline bool is_bigint(integer_t i) {
  assert_internal(is_integer(i));
  return _is_ptr_fast(i);
}

static inline bool is_smallint(integer_t i) {
  assert_internal(is_integer(i));
  return _is_value_fast(i);
}

static inline bool are_smallints(integer_t i, integer_t j) {
  assert_internal(is_integer(i) && is_integer(j));
  return ((i.box&j.box)&1)!=0;
}



#define integer_zero     (integer_from_small(0))
#define integer_one      (integer_from_small(1))
#define integer_min_one  (integer_from_small(-1))

/*---------------------------------------------------------------------------------
  Generic operations on integers
-----------------------------------------------------------------------------------*/

static inline box_t     box_integer_t(integer_t i) { return i; }
static inline integer_t unbox_integer_t(box_t b)   { return b; }

static inline integer_t dup_integer_t(integer_t x)    { return dup_box_t(x); }
static inline void      drop_integer_t(integer_t x, context_t* ctx) { drop_box_t(x, ctx); }

decl_export integer_t  integer_parse(const char* num, context_t* ctx);
decl_export integer_t  integer_from_str(const char* num, context_t* ctx); // for known correct string number
decl_export noinline integer_t  integer_from_big(intx_t i, context_t* ctx);         // for possibly large i
decl_export noinline integer_t  integer_from_big64(int64_t i, context_t* ctx);     // for possibly large i
decl_export noinline integer_t  integer_from_bigu64(uint64_t i, context_t* ctx);   // for possibly large i
decl_export noinline integer_t  integer_from_double(double d, context_t* ctx);     // round d and convert to integer (0 for NaN/Inf)

decl_export noinline int32_t    integer_clamp32_generic(integer_t i, context_t* ctx);
decl_export noinline int64_t    integer_clamp64_generic(integer_t i, context_t* ctx);
decl_export noinline double     integer_as_double_generic(integer_t i, context_t* ctx);

decl_export noinline integer_t  integer_add_generic(integer_t x, integer_t y, context_t* ctx);
decl_export noinline integer_t  integer_sub_generic(integer_t x, integer_t y, context_t* ctx);
decl_export noinline integer_t  integer_mul_generic(integer_t x, integer_t y, context_t* ctx);
decl_export noinline integer_t  integer_div_generic(integer_t x, integer_t y, context_t* ctx);
decl_export noinline integer_t  integer_mod_generic(integer_t x, integer_t y, context_t* ctx);
decl_export noinline integer_t  integer_div_mod_generic(integer_t x, integer_t y, integer_t* mod, context_t* ctx);

decl_export noinline int        integer_cmp_generic(integer_t x, integer_t y, context_t* ctx);
decl_export noinline integer_t  integer_neg_generic(integer_t x, context_t* ctx);
decl_export noinline integer_t  integer_sqr_generic(integer_t x, context_t* ctx);
decl_export noinline integer_t  integer_pow(integer_t x, integer_t p, context_t* ctx);

decl_export noinline bool       integer_is_even_generic(integer_t x, context_t* ctx);
decl_export noinline int        integer_signum_generic(integer_t x, context_t* ctx);

decl_export noinline integer_t  integer_ctz(integer_t x, context_t* ctx);           // count trailing zero digits
decl_export noinline integer_t  integer_count_digits(integer_t x, context_t* ctx);  // count decimal digits
decl_export noinline integer_t  integer_mul_pow10(integer_t x, integer_t p, context_t* ctx);  // x*(10^p)
decl_export noinline integer_t  integer_div_pow10(integer_t x, integer_t p, context_t* ctx);  // x/(10^p)

decl_export noinline void       integer_fprint(FILE* f, integer_t x, context_t* ctx);
decl_export noinline void       integer_print(integer_t x, context_t* ctx);


/*---------------------------------------------------------------------------------
  Inlined operation to allow for fast operation on small integers
-----------------------------------------------------------------------------------*/

static inline integer_t integer_from_int(intx_t i, context_t* ctx) {
  return (likely(i >= SMALLINT_MIN && i <= SMALLINT_MAX) ? integer_from_small(i) : integer_from_big(i,ctx));
}

static inline integer_t integer_from_int32(int32_t i, context_t* ctx) {
  return (likely(i >= SMALLINT_MIN && i <= SMALLINT_MAX) ? integer_from_small(i) : integer_from_big(i, ctx));
}

static inline integer_t integer_from_int64(int64_t i, context_t* ctx) {
  return (likely(i >= SMALLINT_MIN && i <= SMALLINT_MAX) ? integer_from_small((intptr_t)i) : integer_from_big64(i, ctx));
}

static inline integer_t integer_from_uint64(uint64_t i, context_t* ctx) {
  return (likely(i <= SMALLINT_MAX) ? integer_from_small((intptr_t)i) : integer_from_bigu64(i, ctx));
}

#if (INTX_SIZE<=4)
static inline integer_t integer_from_uintx_t(uintx_t i, context_t* ctx) {
  return (i <= INT32_MAX ? integer_from_int((intx_t)i,ctx) : integer_from_uint64(i,ctx));
}
#else
static inline integer_t integer_from_uintx_t(uintx_t i, context_t* ctx) {
  return integer_from_uint64(i, ctx);
}
#endif

static inline integer_t integer_from_size_t(size_t i, context_t* ctx) {
  return integer_from_uintx_t(i, ctx);
}

static inline integer_t integer_from_intptr_t(intptr_t i, context_t* ctx) {
  return integer_from_int(i, ctx);
}

#if defined(__GNUC__) && (SMALLINT_BITS >= 32)  

// Use the overflow detecting primitives

static inline bool smallint_add_ovf(intptr_t h, intptr_t y, intptr_t* r) {
  smallint_t i;
  const bool ovf = __builtin_add_overflow((smallint_t)h, (smallint_t)y, &i);
  *r = (intptr_t)i; // sign extend
  return ovf;
}

static inline bool smallint_sub_ovf(intptr_t h, intptr_t y, intptr_t* r) {
  smallint_t i;
  const bool ovf = __builtin_sub_overflow((smallint_t)h, (smallint_t)y, &i);
  *r = (intptr_t)i; // sign extend
  return ovf;
}

static inline bool smallint_mul_ovf(intptr_t h, intptr_t y, intptr_t* r) {
  smallint_t i;
  const bool ovf = __builtin_mul_overflow((smallint_t)h, (smallint_t)y, &i);
  *r = (intptr_t)i; // sign extend
  return ovf;
}

#else 
// Generic overflow detection, still quite good.

static inline bool smallint_add_ovf(intptr_t x, intptr_t y, intptr_t* r) {
  intptr_t z = x + y; // do a full add
  *r = z;
  uintptr_t s = (uintptr_t)(sar(z, SMALLINT_BITS - 1)); // cast to unsigned to avoid UB for s+1 on overflow
  return (s + 1 > 1); // are the top bits not 0 or -1 ?
}

static inline bool smallint_sub_ovf(intptr_t x, intptr_t y, intptr_t* r) {
  intptr_t z = x - y; // do a full sub
  *r = z;
  uintptr_t s = (uintptr_t)(sar(z, SMALLINT_BITS - 1)); // cast to unsigned to avoid UB for s+1 on overflow
  return (s + 1 > 1); // are the top bits not 0 or -1 ?
}

static inline bool smallint_mul_ovf(intptr_t x, intptr_t y, intptr_t* r) {
  intptr_t z = x * y; // do a full multiply (as smallint is at most half the int_t size)
  *r = z;
  uintptr_t s = (uintptr_t)(sar(z, SMALLINT_BITS - 1)); // cast to unsigned to avoid UB for s+1 on overflow
  return (s + 1 > 1); // are the top bits not 0 or -1 ?
}

#endif


/* Fast addition on small integers. Since `boxed(n) = n*4 + 1`, we can add as:
      boxed(n) + (boxed(m) - 1)
      = (n*4 + 1) + ((m*4 + 1) - 1)
      = n*4 + m*4 + 1
      = 4*(n + m) + 1
      = boxed(n+m)
    (we use (^ 1) instead of (- 1) to reduce register stalls (since we know the bottom bits of `y` are 01)
*/
static inline integer_t integer_add_small(integer_t x, integer_t y, context_t* ctx) {
  assert_internal(are_smallints(x, y));
  intptr_t i;
  if (likely(!smallint_add_ovf(box_as_intptr(x), box_as_intptr(y)^1, &i))) {
    integer_t z = box_from_intptr(i); 
    assert_internal(is_smallint(z));
    return z;
  }
  return integer_add_generic(x, y, ctx);
}


/*
static inline integer_t integer_add(integer_t x, integer_t y) {
  if (likely(are_smallints(x, y))) return integer_add_small(x, y);
  return integer_add_generic(x, y);
}
*/

/* We further optimize addition on integers, if we add directly, our lowest 2 bits are :
    x + y = z
    00  00  00    ptr + ptr
    00  01  01    ptr + int
    01  00  01    int + ptr
    01  01  10    int + int
  and we can detect afterwards if it was correct to assume these were smallint's
*/
static inline integer_t integer_add(integer_t x, integer_t y, context_t* ctx) {
  assert_internal(is_integer(x) && is_integer(y));
  intptr_t i;
  if (likely(!smallint_add_ovf(box_as_intptr(x), box_as_intptr(y), &i) && (i&2)!=0)) {
    integer_t z = box_from_intptr(i^3);  // == i - 1
    assert_internal(is_smallint(z));
    return z;
  }
  return integer_add_generic(x, y, ctx);
}


/* Fast subtract on small integers. Since `boxed(n) = n*4 + 1`, we can subtract as:
      boxed(n) - (boxed(m) - 1)
      = (n*4 + 1) - ((m*4 + 1) - 1)
      = n*4 + 1 - m*4
      = (n - m)*4 + 1
      = boxed(n-m)
*/
static inline integer_t integer_sub_small(integer_t x, integer_t y, context_t* ctx) {
  assert_internal(are_smallints(x, y));
  intptr_t i;
  if (likely(!smallint_sub_ovf(box_as_intptr(x), box_as_intptr(y)^1, &i))) {
    integer_t z = box_from_intptr(i);
    assert_internal(is_smallint(z));
    return z;
  }
  return integer_sub_generic(x, y, ctx);
}

static inline integer_t integer_sub(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return integer_sub_small(x, y, ctx);
  return integer_sub_generic(x, y, ctx);
}

/* Fast multiply on small integers. Since `boxed(n) = n*4 + 1`, we can multiply as:
    (boxed(n)/2) * (boxed(m)/2) + 1
    = (n*4+1)/2 * (m*4+1)/2 + 1
    = (n*2) * (m*2) + 1
    = (n*m*4) + 1
    = boxed(n*m)
*/
static inline integer_t integer_mul_small(integer_t x, integer_t y, context_t* ctx) {
  assert_internal(are_smallints(x, y));
  intptr_t i = sar(box_as_intptr(x), 1);
  intptr_t j = sar(box_as_intptr(y), 1);
  intptr_t k;
  if (likely(!smallint_mul_ovf(i, j, &k))) {
    integer_t z = box_from_intptr(k|1);
    assert_internal(is_smallint(z));
    return z;
  }
  return integer_mul_generic(x, y, ctx);
}

static inline integer_t integer_mul(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return integer_mul_small(x, y, ctx);
  return integer_mul_generic(x, y, ctx);
}

/* Fast divide on small integers. Since `boxed(n) = n*4 + 1`, we can divide as:
    4*((boxed(n)/2)/((boxed(m)/2) + 1
    = 4*((n*2)/(m*2)) + 1
    = 4*(n/m) + 1
    = boxed(n/m)
*/
static inline integer_t integer_div_small(integer_t x, integer_t y) {
  assert_internal(are_smallints(x, y));
  intptr_t i = sar(box_as_intptr(x), 1);
  intptr_t j = sar(box_as_intptr(y), 1);
  return box_from_uintptr(shr(i/j, UX(2))|1);
}

/* Fast modulus on small integers. Since `boxed(n) = n*4 + 1`, we can divide as:
    2*((boxed(n)/2)/((boxed(m)/2) + 1
    = 2*((n*2)%(m*2)) + 1
    = 2*2*(n%m) + 1
    = boxed(n%m)
*/
static inline integer_t integer_mod_small(integer_t x, integer_t y) {
  assert_internal(are_smallints(x, y));
  intptr_t i = sar(box_as_intptr(x), 1);
  intptr_t j = sar(box_as_intptr(y), 1);
  return box_from_uintptr(shr(i%j, UX(1))|1);
}

static inline integer_t integer_div_mod_small(integer_t x, integer_t y, integer_t* mod) {
  assert_internal(are_smallints(x, y)); assert_internal(mod!=NULL);
  intptr_t i = sar(box_as_intptr(x), 1);
  intptr_t j = sar(box_as_intptr(y), 1);
  *mod = box_from_intptr(shr(i%j, 1)|1);
  return box_from_intptr(shr(i/j, 2)|1);
}

static inline integer_t integer_div(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return integer_div_small(x, y);
  return integer_div_generic(x, y, ctx);
}

static inline integer_t integer_mod(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return integer_mod_small(x, y);
  return integer_mod_generic(x, y, ctx);
}

static inline integer_t integer_div_mod(integer_t x, integer_t y, integer_t* mod, context_t* ctx) {
  assert_internal(mod!=NULL);
  if (likely(are_smallints(x, y))) return integer_div_mod_small(x, y, mod);
  return integer_div_mod_generic(x, y, mod, ctx);
}

static inline int32_t integer_clamp32(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (int32_t)smallint_from_integer(x);
  return integer_clamp32_generic(x, ctx);
}

static inline int64_t integer_clamp64(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (int64_t)smallint_from_integer(x);
  return integer_clamp64_generic(x, ctx);
}

static inline intx_t integer_clamp(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return smallint_from_integer(x);
#if INTX_SIZE <= 4
  return integer_clamp32_generic(x, ctx);
#else
  return integer_clamp64_generic(x, ctx);
#endif
}


static inline double integer_as_double(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (double)smallint_from_integer(x);
  return integer_as_double_generic(x, ctx);
}

static inline integer_t integer_sqr(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return integer_mul_small(x,x,ctx);
  return integer_sqr_generic(x,ctx);
}

static inline integer_t integer_neg_small(integer_t x, context_t* ctx) {
  assert_internal(is_smallint(x));
  return integer_sub_small(integer_zero, x, ctx);   // negation can overflow
}

static inline integer_t integer_neg(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return integer_neg_small(x,ctx);
  return integer_neg_generic(x,ctx);
}

static inline integer_t integer_abs(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (box_as_intptr(x) < box_as_intptr(integer_zero) ? integer_neg_small(x,ctx) : x);
  return (integer_signum_generic(x, ctx) < 0 ? integer_neg_generic(x, ctx) : x);
}

static inline integer_t integer_dec(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return integer_sub_small(x,integer_one,ctx);
  return integer_sub_generic(x, integer_one,ctx);
}

static inline integer_t integer_inc(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return integer_add_small(x, integer_one,ctx);
  return integer_add_generic(x, integer_one,ctx);
}

static inline int integer_cmp(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) == box_as_intptr(y) ? 0 : (box_as_intptr(x) > box_as_intptr(y) ? 1 : -1));
  return integer_cmp_generic(x, y, ctx);
}

static inline bool integer_lt(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) < box_as_intptr(y));
  return (integer_cmp(x, y, ctx) == -1);
}

static inline bool integer_lte(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) <= box_as_intptr(y));
  return (integer_cmp(x, y, ctx) <= 0);
}

static inline bool integer_gt(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) > box_as_intptr(y));
  return (integer_cmp(x, y, ctx) == 1);
}

static inline bool integer_gte(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) >= box_as_intptr(y));
  return (integer_cmp(x, y, ctx) >= 0);
}

static inline bool integer_eq(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) == box_as_intptr(y));
  return (integer_cmp(x, y, ctx) == 0);
}

static inline bool integer_neq(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x) != box_as_intptr(y));
  return (integer_cmp(x, y, ctx) != 0);
}

static inline bool integer_is_zero(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (box_as_intptr(x) == box_as_intptr(integer_zero));
  drop_integer_t(x,ctx);
  return false;
}

static inline bool integer_is_one(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (box_as_intptr(x) == box_as_intptr(integer_one));
  drop_integer_t(x,ctx);
  return false;
}

static inline bool integer_is_minus_one(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (box_as_intptr(x) == box_as_intptr(integer_min_one));
  drop_integer_t(x,ctx);
  return false;
}

static inline bool integer_is_even(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return ((box_as_intptr(x)&0x04)==0);
  return integer_is_even_generic(x,ctx);
}

static inline bool integer_is_odd(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return ((box_as_intptr(x)&0x04)!=0);
  return !integer_is_even_generic(x,ctx);
}

static inline int integer_signum(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return ((box_as_intptr(x)>1)-(box_as_intptr(x)<0));
  return integer_signum_generic(x,ctx);
}

static inline bool integer_is_neg(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (box_as_intptr(x)<0);
  return (integer_signum_generic(x,ctx) < 0);
}

static inline bool integer_is_pos(integer_t x, context_t* ctx) {
  if (likely(is_smallint(x))) return (box_as_intptr(x)>1);
  return (integer_signum_generic(x,ctx) > 0);
}

static inline integer_t integer_max(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x)>=box_as_intptr(y) ? x : y);
  dup_integer_t(x); dup_integer_t(y);
  if (integer_gte(x,y,ctx)) {
    drop_integer_t(y,ctx); return x;
  }
  else {
    drop_integer_t(x,ctx); return y;
  }
}

static inline integer_t integer_min(integer_t x, integer_t y, context_t* ctx) {
  if (likely(are_smallints(x, y))) return (box_as_intptr(x)<=box_as_intptr(y) ? x : y);
  dup_integer_t(x); dup_integer_t(y);
  if (integer_lte(x, y, ctx)) {
    drop_integer_t(y, ctx); return x;
  }
  else {
    drop_integer_t(x, ctx); return y;
  }
}



#endif // include guard