/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MINMAX_H
#define _LINUX_MINMAX_H

#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/const.h>
#include <linux/types.h>

/*
 * min()/max()/clamp() macros must accomplish three things:
 *
 * - Avoid multiple evaluations of the arguments (so side-effects like
 *   "x++" happen only once) when non-constant.
 * - Retain result as a constant expressions when called with only
 *   constant expressions (to avoid tripping VLA warnings in stack
 *   allocation usage).
 * - Perform signed v unsigned type-checking (to generate compile
 *   errors instead of nasty runtime surprises).
 * - Unsigned char/short are always promoted to signed int and can be
 *   compared against signed or unsigned arguments.
 * - Unsigned arguments can be compared against non-negative signed constants.
 * - Comparison of a signed argument against an unsigned constant fails
 *   even if the constant is below __INT_MAX__ and could be cast to int.
 */
#define __typecheck(x, y) \
	(!!(sizeof((typeof(x) *)1 == (typeof(y) *)1)))

/* is_signed_type() isn't a constexpr for pointer types */
#define __is_signed(x) 								\
	__builtin_choose_expr(__is_constexpr(is_signed_type(typeof(x))),	\
		is_signed_type(typeof(x)), 0)

/* True for a non-negative signed int constant */
#define __is_noneg_int(x)	\
	(__builtin_choose_expr(__is_constexpr(x) && __is_signed(x), x, -1) >= 0)

#define __types_ok(x, y) 					\
	(__is_signed(x) == __is_signed(y) ||			\
		__is_signed((x) + 0) == __is_signed((y) + 0) ||	\
		__is_noneg_int(x) || __is_noneg_int(y))

#define __cmp_op_min <
#define __cmp_op_max >

#define __cmp(op, x, y)	((x) __cmp_op_##op (y) ? (x) : (y))

#define __cmp_once(op, x, y, unique_x, unique_y) ({	\
	typeof(x) unique_x = (x);			\
	typeof(y) unique_y = (y);			\
	static_assert(__types_ok(x, y),			\
		#op "(" #x ", " #y ") signedness error, fix types or consider u" #op "() before " #op "_t()"); \
	__cmp(op, unique_x, unique_y); })

#define __careful_cmp(op, x, y)					\
	__builtin_choose_expr(__is_constexpr((x) - (y)),	\
		__cmp(op, x, y),				\
		__cmp_once(op, x, y, __UNIQUE_ID(__x), __UNIQUE_ID(__y)))

#define __clamp(val, lo, hi)	\
	((val) >= (hi) ? (hi) : ((val) <= (lo) ? (lo) : (val)))

#define __clamp_once(val, lo, hi, unique_val, unique_lo, unique_hi) ({		\
	typeof(val) unique_val = (val);						\
	typeof(lo) unique_lo = (lo);						\
	typeof(hi) unique_hi = (hi);						\
	static_assert(__builtin_choose_expr(__is_constexpr((lo) > (hi)), 	\
			(lo) <= (hi), true),					\
		"clamp() low limit " #lo " greater than high limit " #hi);	\
	static_assert(__types_ok(val, lo), "clamp() 'lo' signedness error");	\
	static_assert(__types_ok(val, hi), "clamp() 'hi' signedness error");	\
	__clamp(unique_val, unique_lo, unique_hi); })

#define __careful_clamp(val, lo, hi) ({					\
	__builtin_choose_expr(__is_constexpr((val) - (lo) + (hi)),	\
		__clamp(val, lo, hi),					\
		__clamp_once(val, lo, hi, __UNIQUE_ID(__val),		\
			     __UNIQUE_ID(__lo), __UNIQUE_ID(__hi))); })

/**
 * min - return minimum of two values of the same or compatible types
 * @x: first value
 * @y: second value
 */
#define min(x, y)	__careful_cmp(min, x, y)

/**
 * max - return maximum of two values of the same or compatible types
 * @x: first value
 * @y: second value
 */
#define max(x, y)	__careful_cmp(max, x, y)

/**
 * umin - return minimum of two non-negative values
 *   Signed types are zero extended to match a larger unsigned type.
 * @x: first value
 * @y: second value
 */
#define umin(x, y)	\
	__careful_cmp(min, (x) + 0u + 0ul + 0ull, (y) + 0u + 0ul + 0ull)

/**
 * umax - return maximum of two non-negative values
 * @x: first value
 * @y: second value
 */
#define umax(x, y)	\
	__careful_cmp(max, (x) + 0u + 0ul + 0ull, (y) + 0u + 0ul + 0ull)

/**
 * min3 - return minimum of three values
 * @x: first value
 * @y: second value
 * @z: third value
 */
#define min3(x, y, z) min((typeof(x))min(x, y), z)

/**
 * max3 - return maximum of three values
 * @x: first value
 * @y: second value
 * @z: third value
 */
#define max3(x, y, z) max((typeof(x))max(x, y), z)

/**
 * min_not_zero - return the minimum that is _not_ zero, unless both are zero
 * @x: value1
 * @y: value2
 */
#define min_not_zero(x, y) ({			\
	typeof(x) __x = (x);			\
	typeof(y) __y = (y);			\
	__x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })

/**
 * clamp - return a value clamped to a given range with strict typechecking
 * @val: current value
 * @lo: lowest allowable value
 * @hi: highest allowable value
 *
 * This macro does strict typechecking of @lo/@hi to make sure they are of the
 * same type as @val.  See the unnecessary pointer comparisons.
 */
#define clamp(val, lo, hi) __careful_clamp(val, lo, hi)

/*
 * ..and if you can't take the strict
 * types, you can specify one yourself.
 *
 * Or not use min/max/clamp at all, of course.
 */

/**
 * min_t - return minimum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 */
#define min_t(type, x, y)	__careful_cmp(min, (type)(x), (type)(y))

/**
 * max_t - return maximum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 */
#define max_t(type, x, y)	__careful_cmp(max, (type)(x), (type)(y))

/*
 * Do not check the array parameter using __must_be_array().
 * In the following legit use-case where the "array" passed is a simple pointer,
 * __must_be_array() will return a failure.
 * --- 8< ---
 * int *buff
 * ...
 * min = min_array(buff, nb_items);
 * --- 8< ---
 *
 * The first typeof(&(array)[0]) is needed in order to support arrays of both
 * 'int *buff' and 'int buff[N]' types.
 *
 * The array can be an array of const items.
 * typeof() keeps the const qualifier. Use __unqual_scalar_typeof() in order
 * to discard the const qualifier for the __element variable.
 */
#define __minmax_array(op, array, len) ({				\
	typeof(&(array)[0]) __array = (array);				\
	typeof(len) __len = (len);					\
	__unqual_scalar_typeof(__array[0]) __element = __array[--__len];\
	while (__len--)							\
		__element = op(__element, __array[__len]);		\
	__element; })

/**
 * min_array - return minimum of values present in an array
 * @array: array
 * @len: array length
 *
 * Note that @len must not be zero (empty array).
 */
#define min_array(array, len) __minmax_array(min, array, len)

/**
 * max_array - return maximum of values present in an array
 * @array: array
 * @len: array length
 *
 * Note that @len must not be zero (empty array).
 */
#define max_array(array, len) __minmax_array(max, array, len)

/**
 * clamp_t - return a value clamped to a given range using a given type
 * @type: the type of variable to use
 * @val: current value
 * @lo: minimum allowable value
 * @hi: maximum allowable value
 *
 * This macro does no typechecking and uses temporary variables of type
 * @type to make all the comparisons.
 */
#define clamp_t(type, val, lo, hi) __careful_clamp((type)(val), (type)(lo), (type)(hi))

/**
 * clamp_val - return a value clamped to a given range using val's type
 * @val: current value
 * @lo: minimum allowable value
 * @hi: maximum allowable value
 *
 * This macro does no typechecking and uses temporary variables of whatever
 * type the input argument @val is.  This is useful when @val is an unsigned
 * type and @lo and @hi are literals that will otherwise be assigned a signed
 * integer type.
 */
#define clamp_val(val, lo, hi) clamp_t(typeof(val), val, lo, hi)

static inline bool in_range64(u64 val, u64 start, u64 len)
{
	return (val - start) < len;
}

static inline bool in_range32(u32 val, u32 start, u32 len)
{
	return (val - start) < len;
}

/**
 * in_range - Determine if a value lies within a range.
 * @val: Value to test.
 * @start: First value in range.
 * @len: Number of values in range.
 *
 * This is more efficient than "if (start <= val && val < (start + len))".
 * It also gives a different answer if @start + @len overflows the size of
 * the type by a sufficient amount to encompass @val.  Decide for yourself
 * which behaviour you want, or prove that start + len never overflow.
 * Do not blindly replace one form with the other.
 */
#define in_range(val, start, len)					\
	((sizeof(start) | sizeof(len) | sizeof(val)) <= sizeof(u32) ?	\
		in_range32(val, start, len) : in_range64(val, start, len))

/**
 * swap - swap values of @a and @b
 * @a: first value
 * @b: second value
 */
#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#endif	/* _LINUX_MINMAX_H */
