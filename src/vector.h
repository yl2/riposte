
#ifndef _RIPOSTE_VECTOR_H
#define _RIPOSTE_VECTOR_H

#include "value.h"

template< class Op, int64_t N, bool Multiple = (((N)%(4)) == 0) >
struct Map1 {
	static void eval(Thread& thread, typename Op::A::Element const* a, typename Op::R::Element* r) {
		for(int64_t i = 0; i < N; ++i) r[i] = Op::eval(thread, a[i]);
	}
};

template< class Op, int64_t N, bool Multiple = (((N)%(4)) == 0) >
struct Map2VV {
	static void eval(Thread& thread, typename Op::A::Element const* a, typename Op::B::Element const* b, typename Op::R::Element* r) {
		for(int64_t i = 0; i < N; ++i) r[i] = Op::eval(thread, a[i], b[i]);
	}
};

template< class Op, int64_t N, bool Multiple = (((N)%(4)) == 0) >
struct Map2SV {
	static void eval(Thread& thread, typename Op::A::Element const a, typename Op::B::Element const* b, typename Op::R::Element* r) {
		for(int64_t i = 0; i < N; ++i) r[i] = Op::eval(thread, a, b[i]);
	}
};

template< class Op, int64_t N, bool Multiple = (((N)%(4)) == 0) >
struct Map2VS {
	static void eval(Thread& thread, typename Op::A::Element const* a, typename Op::B::Element const b, typename Op::R::Element* r) {
		for(int64_t i = 0; i < N; ++i) r[i] = Op::eval(thread, a[i], b);
	}
};

template< class Op, int64_t N >
struct FoldLeftT {
	static typename Op::R eval(Thread& thread, typename Op::A::Element const* a, typename Op::R::Element r) {
		for(int64_t i = 0; i < N; ++i) r = Op::eval(thread, r, a[i]);
		return r;
	}
};

template< class Op, int64_t N >
struct ScanLeftT {
	static typename Op::R eval(Thread& thread, typename Op::A::Element const* a, typename Op::R::Element b, typename Op::R::Element* r) {
		for(int64_t i = 0; i < N; ++i) r[i] = b = Op::eval(thread, b, a[i]);
		return b;
	}
};

template< class Op >
struct Zip1 {
	static void eval(Thread& thread, typename Op::A const& a, Value& out)
	{
		if(a.isScalar()) {
			Op::Scalar(thread, a[0], out);
		}
		else {
			typename Op::R r(a.length);
			typename Op::R::Element* re = r.v();
			typename Op::A::Element const* ae = a.v();
			int64_t length = a.length;
			int64_t i = 0;
			for(; i < length-3; i+=4) Map1<Op,4>::eval(thread, ae+i, re+i);
			for(; i < length; i++) Map1<Op,1>::eval(thread, ae+i, re+i);
			out = (Value&)r;
		}
	}
};

template< class Op >
struct Zip2 {
	static void eval(Thread& thread, typename Op::A const& a, typename Op::B const& b, Value& out)
	{
		if(a.isScalar() && b.isScalar()) {
			Op::Scalar(thread, a[0], b[0], out);
		}
		else if(b.isScalar()) {
			typename Op::R r(a.length);
			typename Op::R::Element* re = r.v();
			typename Op::A::Element const* ae = a.v();
			typename Op::B::Element be = b[0];
			int64_t length = a.length;
			int64_t i = 0;
			for(; i < length-3; i+=4) Map2VS<Op,4>::eval(thread, ae+i, be, re+i);
			for(; i < length; i++) Map2VS<Op,1>::eval(thread, ae+i, be, re+i);
			out = (Value&)r;
		}
		else if(a.isScalar()) {
			typename Op::R r(b.length);
			typename Op::R::Element* re = r.v();
			typename Op::A::Element ae = a[0];
			typename Op::B::Element const* be = b.v();
			int64_t length = b.length;
			int64_t i = 0;
			for(; i < length-3; i+=4) Map2SV<Op,4>::eval(thread, ae, be+i, re+i);
			for(; i < length; i++) Map2SV<Op,1>::eval(thread, ae, be+i, re+i);
			out = (Value&)r;
		}
		else if(a.length == b.length) {
			typename Op::R r(a.length);
			typename Op::R::Element* re = r.v();
			typename Op::A::Element const* ae = a.v();
			typename Op::B::Element const* be = b.v();
			int64_t length = a.length;
			int64_t i = 0;
			for(; i < length-3; i+=4) Map2VV<Op,4>::eval(thread, ae+i, be+i, re+i);
			for(; i < length; i++) Map2VV<Op,1>::eval(thread, ae+i, be+i, re+i);
			out = (Value&)r;
		}
		else if(a.length == 0 || b.length == 0) {
			Op::R::Init(out, 0);
		}
		else if(a.length > b.length) {
			typename Op::R r(a.length);
			typename Op::R::Element* re = r.v();
			typename Op::A::Element const* ae = a.v();
			typename Op::B::Element const* be = b.v();
			int64_t alength = a.length;
			int64_t blength = b.length;
			int64_t j = 0;
			for(int64_t i = 0; i < alength; ++i) {
				re[i] = Op::eval(thread, ae[i], be[j]);
				++j;
				if(j >= blength) j = 0;
			}
			out = (Value&)r;
		}
		else {
			typename Op::R r(b.length);
			typename Op::R::Element* re = r.v();
			typename Op::A::Element const* ae = a.v();
			typename Op::B::Element const* be = b.v();
			int64_t alength = a.length;
			int64_t blength = b.length;
			int64_t j = 0;
			for(int64_t i = 0; i < blength; ++i) {
				re[i] = Op::eval(thread, ae[j], be[i]);
				++j;
				if(j >= alength) j = 0;
			}
			out = (Value&)r;
		}
	}
};

template< class Op >
struct Zip2N {
	static void eval(Thread& thread, int64_t N, typename Op::AV const& a, typename Op::BV const& b, Value& out)
	{
		typename Op::A::Element const* ae = a.v();
		typename Op::B::Element const* be = b.v();
		typename Op::R r(N);
		typename Op::R::Element* re = r.v();
		int64_t j = 0, k = 0;
		for(int64_t i = 0; i < N; i++) {
			re[i] = Op::eval(thread, ae[j++], be[k++]);
			if(j >= a.length) j = 0;
			if(k >= b.length) k = 0;
		}
		out = (Value&)r;
	}
};

template< class Op >
struct FoldLeft {
	static void eval(Thread& thread, typename Op::B const& b, Value& out)
	{
		typename Op::B::Element const* be = b.v();
		typename Op::R::Element a = Op::base();
		int64_t length = b.length;
		for(int64_t i = 0; i < length; ++i) {
			a = Op::eval(thread, a, be[i]);
		}
		Op::R::InitScalar(out, a);
	}
};

template< class Op >
struct ScanLeft {
	static void eval(Thread& thread, typename Op::B const& b, Value& out)
	{
		typename Op::B::Element const* be = b.v();
		typename Op::R::Element a = Op::base();
		typename Op::R r(b.length);
		typename Op::R::Element* re = r.v();
		int64_t length = b.length;
		for(int64_t i = 0; i < length; ++i) {
			re[i] = a = Op::eval(thread, a, be[i]);
		}
		out = (Value&)r;
	}
};

#endif
