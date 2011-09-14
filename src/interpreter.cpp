#include <string>
#include <sstream>
#include <stdexcept>
#include <string>

#include "value.h"
#include "type.h"
#include "bc.h"
#include "ops.h"
#include "internal.h"
#include "interpreter.h"
#include "recording.h"
#include "compiler.h"

#define ALWAYS_INLINE __attribute__((always_inline))

static Instruction const* buildStackFrame(State& state, Environment* environment, bool ownEnvironment, Prototype const* prototype, Value* result, Instruction const* returnpc);

#ifndef __ICC
extern Instruction const* kget_op(State& state, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* get_op(State& state, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* assign_op(State& state, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* forend_op(State& state, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* add_op(State& state, Instruction const& inst) ALWAYS_INLINE;
#endif

#define REG(state, i) (*(state.base+i))

static void ExpandDots(State& state, List& arguments, Character& names, int64_t dots) {
	Environment* environment = state.frame.environment;
	uint64_t dotslength = environment->dots.size();
	// Expand dots into the parameter list...
	if(dots < arguments.length) {
		List a(arguments.length + dotslength - 1);
		for(int64_t i = 0; i < dots; i++) a[i] = arguments[i];
		for(uint64_t i = dots; i < dots+dotslength; i++) { a[i] = Function(Compiler::compile(state, Symbol(-(i-dots+1))), NULL).AsPromise(); } // TODO: should cache these.
		for(uint64_t i = dots+dotslength; i < arguments.length+dotslength-1; i++) a[i] = arguments[i-dotslength];

		arguments = a;
		
		uint64_t named = 0;
		for(uint64_t i = 0; i < dotslength; i++) if(environment->dots[i] != Symbols::empty) named++;

		if(names.length > 0 || named > 0) {
			Character n(arguments.length + dotslength - 1);
			for(int64_t i = 0; i < n.length; i++) n[i] = Symbols::empty;
			if(names.length > 0) {
				for(int64_t i = 0; i < dots; i++) n[i] = names[i];
				for(uint64_t i = dots+dotslength; i < arguments.length+dotslength-1; i++) n[i] = names[i-dotslength];
			}
			if(named > 0) {
				for(uint64_t i = dots; i < dots+dotslength; i++) n[i] = environment->dots[i]; 
			}
			names = n;
		}
	}
}

inline void argAssign(Environment* env, int64_t i, Value const& v, Environment* execution, Character const& parameters) {
	Value w = v;
	if(w.isPromise() && w.p == 0) w.p = execution;
	if(i >= 0)
		env->assign(parameters[i], w);
	else {
		env->assign(Symbol(i), w);
	}
}

static void MatchArgs(State& state, Environment* env, Environment* fenv, Function const& func, List const& arguments, Character const& anames) {
	List const& defaults = func.prototype()->defaults;
	Character const& parameters = func.prototype()->parameters;
	int64_t fdots = func.prototype()->dots;

	// set defaults
	for(int64_t i = 0; i < defaults.length; ++i) {
		argAssign(fenv, i, defaults[i], fenv, parameters);
	}

	// call arguments are not named, do posititional matching
	if(anames.length == 0) {
		int64_t end = std::min(arguments.length, fdots);
		for(int64_t i = 0; i < end; ++i) {
			if(!arguments[i].isNil()) argAssign(fenv, i, arguments[i], env, parameters);
		}

		// set dots if necessary
		if(fdots < parameters.length) {
			int64_t idx = 1;
			for(int64_t i = fdots; i < arguments.length; i++) {
				argAssign(fenv, -idx, arguments[i], env, parameters);
				fenv->dots.push_back(Symbols::empty);
				idx++;
			}
			end++;
		}
	}
	// call arguments are named, do matching by name
	else {
		// we should be able to cache and reuse this assignment for pairs of functions and call sites.
		static char assignment[64], set[64];
		for(int64_t i = 0; i < arguments.length; i++) assignment[i] = -1;
		for(int64_t i = 0; i < parameters.length; i++) set[i] = -(i+1);

		// named args, search for complete matches
		for(int64_t i = 0; i < arguments.length; ++i) {
			if(anames[i] != Symbols::empty) {
				for(int64_t j = 0; j < parameters.length; ++j) {
					if(j != fdots && anames[i] == parameters[j]) {
						assignment[i] = j;
						set[j] = i;
						break;
					}
				}
			}
		}
		// named args, search for incomplete matches
		for(int64_t i = 0; i < arguments.length; ++i) {
			if(anames[i] != Symbols::empty && assignment[i] < 0) {
				std::string a = state.SymToStr(anames[i]);
				for(int64_t j = 0; j < parameters.length; ++j) {
					if(set[j] < 0 && j != fdots &&
							state.SymToStr(parameters[j]).compare( 0, a.size(), a ) == 0 ) {	
						assignment[i] = j;
						set[j] = i;
						break;
					}
				}
			}
		}
		// unnamed args, fill into first missing spot.
		int64_t firstEmpty = 0;
		for(int64_t i = 0; i < arguments.length; ++i) {
			if(anames[i] == Symbols::empty) {
				for(; firstEmpty < fdots; ++firstEmpty) {
					if(set[firstEmpty] < 0) {
						assignment[i] = firstEmpty;
						set[firstEmpty] = i;
						break;
					}
				}
			}
		}

		// stuff that can't be cached...

		// assign all the arguments
		for(int64_t j = 0; j < parameters.length; ++j) if(j != fdots && set[j] >= 0 && !arguments[set[j]].isNil()) argAssign(fenv, j, arguments[set[j]], env, parameters);

		// put unused args into the dots
		if(fdots < parameters.length) {
			int64_t idx = 1;
			for(int64_t i = 0; i < arguments.length; i++) {
				if(assignment[i] < 0) {
					argAssign(fenv, -idx, arguments[i], env, parameters);
					fenv->dots.push_back(anames[i]);
					idx++;
				}
			}
		}
	}
}

static Environment* CreateEnvironment(State& state, Environment* s) {
	Environment* env;
	if(state.environments.size() == 0) {
		env = new Environment(s);
	} else {
		env = state.environments.back();
		state.environments.pop_back();
		env->init(s);
	}
	return env;
}
//track the heat of back edge operations and invoke the recorder on hot traces
//unused until we begin tracing loops again
static Instruction const * profile_back_edge(State & state, Instruction const * inst) {
	return inst;
}

Instruction const* call_op(State& state, Instruction const& inst) {
	Value f = REG(state, inst.a);
	List arguments;
	Character names;
	if(inst.b < 0) {
		CompiledCall const& call = state.frame.prototype->calls[-(inst.b+1)];
		arguments = call.arguments;
		names = call.names;
		if(call.dots < arguments.length)
			ExpandDots(state, arguments, names, call.dots);
	} else {
		Value const& reg = REG(state, inst.b);
		if(reg.isObject()) {
			arguments = List(((Object const&)reg).base());
			names = Character(((Object const&)reg).getNames());
		}
		else {
			arguments = List(reg);
		}
	}

	if(f.isFunction()) {
		Function func(f);
		Environment* fenv = CreateEnvironment(state, func.environment());
		MatchArgs(state, state.frame.environment, fenv, func, arguments, names);
		return buildStackFrame(state, fenv, true, func.prototype(), &REG(state, inst.c), &inst+1);
	} else if(f.isBuiltIn()) {
		REG(state, inst.c) = BuiltIn(f).func(state, arguments, names);
		return &inst+1;
	} else {
		_error(std::string("Non-function (") + Type::toString(f.type) + ") as first parameter to call\n");
		return &inst+1;
	}	
}

// Get a Value by Symbol from the current environment,
//  TODO: UseMethod also should search in some cached library locations.
static Value UseMethodSearch(State& state, Symbol s) {
	Environment* environment = state.frame.environment;
	Value value = environment->get(s);
	while(value.isNil() && environment->StaticParent() != 0) {
		environment = environment->StaticParent();
		value = environment->get(s);
	}
	if(value.isPromise()) {
		value = force(state, value);
		environment->assign(s, value);
	}
	return value;
}

Instruction const* UseMethod_op(State& state, Instruction const& inst) {
	Symbol generic(inst.a);
	
	CompiledCall const& call = state.frame.prototype->calls[inst.b];
	List arguments = call.arguments;
	Character names = call.names;
	if(call.dots < arguments.length)
		ExpandDots(state, arguments, names, call.dots);
	
	Value object = REG(state, inst.c);
	Character type = klass(state, object);

	//Search for type-specific method
	Symbol method = state.StrToSym(state.SymToStr(generic) + "." + state.SymToStr(type[0]));
	Value f = UseMethodSearch(state, method);
	
	//Search for default
	if(f.isNil()) {
		method = state.StrToSym(state.SymToStr(generic) + ".default");
		f = UseMethodSearch(state, method);
	}

	if(f.isFunction()) {
		Function func(f);
		Environment* fenv = CreateEnvironment(state, func.environment());
		MatchArgs(state, state.frame.environment, fenv, func, arguments, names);	
		fenv->assign(Symbols::dotGeneric, generic);
		fenv->assign(Symbols::dotMethod, method);
		fenv->assign(Symbols::dotClass, type); 
		return buildStackFrame(state, fenv, true, func.prototype(), &REG(state, inst.c), &inst+1);
	} else if(f.isBuiltIn()) {
		REG(state, inst.c) = BuiltIn(f).func(state, arguments, names);
		return &inst+1;
	} else {
		_error(std::string("no applicable method for '") + state.SymToStr(generic) + "' applied to an object of class \"" + state.SymToStr(type[0]) + "\"");
	}
}

Instruction const* get_op(State& state, Instruction const& inst) {
	// gets are always generated as a sequence of 3 instructions...
	//	1) the get with source symbol in a and dest register in c.
	//	2) an assign with dest symbol in a and source register in c.
	//		(for use by the promise evaluation. If no promise, step over this instruction.)
	//	3) an invalid instruction containing inline caching info.	

	// check if we can get the value through inline caching...
	bool ic = state.frame.environment->validRevision((&inst+2)->b);
	Value const& src = ic ?
		state.frame.environment->get((&inst+2)->a) :
		state.frame.environment->get(Symbol(inst.a));
	if(__builtin_expect(ic && src.isConcrete(), true)) {
		REG(state, inst.c) = src;
		return &inst+3;
	} 
	if(src.isConcrete()) {
		Environment::Pointer p = state.frame.environment->makePointer(Symbol(inst.a));
		((Instruction*)(&inst+2))->a = p.index;
		((Instruction*)(&inst+2))->b = p.revision;
		REG(state, inst.c) = src;
		return &inst+3;
	} else {
		Symbol s(inst.a);
		Value& dest = REG(state, inst.c);
		dest = src;
		Environment* environment = state.frame.environment;
		while(dest.isNil() && environment->StaticParent() != 0) {
			environment = environment->StaticParent();
			dest = environment->get(s);
		}
		if(dest.isPromise()) {
			Environment* env = Function(dest).environment();
			if(env == 0) env = state.frame.environment;
			Prototype* prototype = Function(dest).prototype();
			return buildStackFrame(state, env, false, prototype, &dest, &inst+1);
		}
		else if(dest.isNil()) 
			throw RiposteError(std::string("object '") + state.SymToStr(s) + "' not found");
		else
			return &inst+3;
	}
}

Instruction const* kget_op(State& state, Instruction const& inst) {
	REG(state, inst.c) = state.frame.prototype->constants[inst.a];
	return &inst+1;
}
Instruction const* iget_op(State& state, Instruction const& inst) {
	REG(state, inst.c) = state.path[0]->get(Symbol(inst.a));
	if(REG(state, inst.c).isNil()) throw RiposteError(std::string("object '") + state.SymToStr(Symbol(inst.a)) + "' not found");
	return &inst+1;
}
Instruction const* assign_op(State& state, Instruction const& inst) {
	// check if we can assign through inline caching
	if(state.frame.environment->validRevision((&inst+1)->b))
		state.frame.environment->assign((&inst+1)->a, REG(state, inst.c));
	else {
		state.frame.environment->assign(Symbol(inst.a), REG(state, inst.c));
		Environment::Pointer p = state.frame.environment->makePointer(Symbol(inst.a));
		((Instruction*)(&inst+1))->a = p.index;
		((Instruction*)(&inst+1))->b = p.revision;
	}
	return &inst+2;
}
// everything else should be in registers

Instruction const* iassign_op(State& state, Instruction const& inst) {
	// a = value, b = index, c = dest 
	SubsetAssign(state, REG(state,inst.c), REG(state,inst.b), REG(state,inst.a), REG(state,inst.c));
	return &inst+1;
}
Instruction const* eassign_op(State& state, Instruction const& inst) {
	// a = value, b = index, c = dest
	Subset2Assign(state, REG(state,inst.c), REG(state,inst.b), REG(state,inst.a), REG(state,inst.c));
	return &inst+1; 
}
Instruction const* subset_op(State& state, Instruction const& inst) {
	Subset(state, REG(state, inst.a), REG(state, inst.b), REG(state, inst.c));
	return &inst+1;
}
Instruction const* subset2_op(State& state, Instruction const& inst) {
	Subset2(state, REG(state, inst.a), REG(state, inst.b), REG(state, inst.c));
	return &inst+1;
}
Instruction const* forbegin_op(State& state, Instruction const& inst) {
	// inst.b-1 holds the loopVector
	if((int64_t)REG(state, inst.b-1).length <= 0) { return &inst+inst.a; }
	Element2(REG(state, inst.b-1), 0, REG(state, inst.c));
	REG(state, inst.b).header = REG(state, inst.b-1).length;	// warning: not a valid object, but saves a shift
	REG(state, inst.b).i = 1;
	return &inst+1;
}
Instruction const* forend_op(State& state, Instruction const& inst) {
	if(__builtin_expect((REG(state,inst.b).i) < REG(state,inst.b).header, true)) {
		Element2(REG(state, inst.b-1), REG(state, inst.b).i, REG(state, inst.c));
		REG(state, inst.b).i++;
		return profile_back_edge(state,&inst+inst.a);
	} else return &inst+1;
}
/*Instruction const* iforbegin_op(State& state, Instruction const& inst) {
	double m = asReal1(REG(state, inst.c-1));
	double n = asReal1(REG(state, inst.c));
	REG(state, inst.c-1) = Integer::c(n > m ? 1 : -1);
	REG(state, inst.c-1).length = (int64_t)n+1;	// danger! this register no longer holds a valid object, but it saves a register and makes the for and ifor cases more similar
	REG(state, inst.c) = Integer::c((int64_t)m);
	if(REG(state, inst.c).i >= (int64_t)REG(state, inst.c-1).length) { return &inst+inst.a; }
	state.frame.environment->hassign(Symbol(inst.b), Integer::c(m));
	REG(state, inst.c).i += REG(state, inst.c-1).i;
	return &inst+1;
}
Instruction const* iforend_op(State& state, Instruction const& inst) {
	if(REG(state, inst.c).i < REG(state, inst.c-1).length) { 
		state.frame.environment->hassign(Symbol(inst.b), REG(state, inst.c));
		REG(state, inst.c).i += REG(state, inst.c-1).i;
		return profile_back_edge(state,&inst+inst.a);
	} else return &inst+1;
}*/
Instruction const* jt_op(State& state, Instruction const& inst) {
	Logical l = As<Logical>(state, REG(state,inst.b));
	if(l.length == 0) _error("condition is of zero length");
	if(l[0]) return &inst+inst.a;
	else return &inst+1;
}
Instruction const* jf_op(State& state, Instruction const& inst) {
	Logical l = As<Logical>(state, REG(state, inst.b));
	if(l.length == 0) _error("condition is of zero length");
	if(l[0]) return &inst+1;
	else return &inst+inst.a;
}
Instruction const* colon_op(State& state, Instruction const& inst) {
	double from = asReal1(REG(state,inst.a));
	double to = asReal1(REG(state,inst.b));
	REG(state,inst.c) = Sequence(from, to>from?1:-1, fabs(to-from)+1);
	return &inst+1;
}
Instruction const* seq_op(State& state, Instruction const& inst) {
	int64_t len = As<Integer>(state, REG(state, inst.a))[0];
	REG(state, inst.c) = Sequence(len);
	return &inst+1;
}

bool isRecordable(Value const& a) {
	return (a.isDouble() || a.isInteger())
		&& a.length > TRACE_VECTOR_WIDTH
		&& a.length % TRACE_VECTOR_WIDTH == 0;
}
bool isRecordable(Value const& a, Value const& b) {
	bool valid_types =   (a.isDouble() || a.isInteger())
				      && (b.isDouble() || b.isInteger());
	size_t length = std::max(a.length,b.length);
	bool compatible_lengths = a.length == 1 || b.length == 1 || a.length == b.length;
	bool should_record_length = length > TRACE_VECTOR_WIDTH && length % TRACE_VECTOR_WIDTH == 0;
	return valid_types && compatible_lengths && should_record_length;
}


#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	Value & a =  REG(state, inst.a);	\
	Value & c = REG(state, inst.c);	\
	if(a.isDouble1()) { Op<TDouble>::RV::InitScalar(c, Op<TDouble>::eval(state, a.d)); return &inst+1; } \
	else if(a.isInteger1()) { Op<TDouble>::RV::InitScalar(c, Op<TInteger>::eval(state, a.i)); return &inst+1; } \
	if(isRecordable(a)) \
		return state.tracing.begin_tracing(state, &inst, a.length); \
	\
	unaryArith<Zip1, Op>(state, a, c); \
	return &inst+1; \
}
UNARY_ARITH_MAP_BYTECODES(OP)
#undef OP


#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryLogical<Zip1, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
UNARY_LOGICAL_MAP_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	Value & a =  REG(state, inst.a);	\
	Value & b =  REG(state, inst.b);	\
	Value & c = REG(state, inst.c);	\
        if(a.isDouble1()) {			\
                if(b.isDouble1())		\
                        { Op<TDouble>::RV::InitScalar(c, Op<TDouble>::eval(state, a.d, b.d)); return &inst+1; }	\
                else if(b.isInteger1())	\
                        { Op<TDouble>::RV::InitScalar(c, Op<TDouble>::eval(state, a.d, (double)b.i));return &inst+1; }	\
        }	\
        else if(a.isInteger1()) {	\
                if(b.isDouble1())	\
                        { Op<TDouble>::RV::InitScalar(c, Op<TDouble>::eval(state, (double)a.i, b.d)); return &inst+1; }	\
                else if(b.isInteger1())	\
                        { Op<TInteger>::RV::InitScalar(c, Op<TInteger>::eval(state, a.i, b.i)); return &inst+1;} \
        } \
	\
	if(isRecordable(a,b)) \
		return state.tracing.begin_tracing(state, &inst, a.length);	\
    \
	binaryArithSlow<Zip2, Op>(state, a, b, c);	\
	return &inst+1;	\
}
BINARY_ARITH_MAP_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	binaryLogical<Zip2, Op>(state, REG(state, inst.a), REG(state, inst.b), REG(state, inst.c)); \
	return &inst+1; \
}
BINARY_LOGICAL_MAP_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	binaryOrdinal<Zip2, Op>(state, REG(state, inst.a), REG(state, inst.b), REG(state, inst.c)); \
	return &inst+1; \
}
BINARY_ORDINAL_MAP_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryArith<FoldLeft, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
ARITH_FOLD_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryLogical<FoldLeft, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
LOGICAL_FOLD_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryOrdinal<FoldLeft, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
ORDINAL_FOLD_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryArith<ScanLeft, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
ARITH_SCAN_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryLogical<ScanLeft, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
LOGICAL_SCAN_BYTECODES(OP)
#undef OP

#define OP(name, string, Op) \
Instruction const* name##_op(State& state, Instruction const& inst) { \
	unaryOrdinal<ScanLeft, Op>(state, REG(state, inst.a), REG(state, inst.c)); \
	return &inst+1; \
}
ORDINAL_SCAN_BYTECODES(OP)
#undef OP

Instruction const* jmp_op(State& state, Instruction const& inst) {
	return &inst+inst.a;
}
Instruction const* sland_op(State& state, Instruction const& inst) {
	Logical l = As<Logical>(state, REG(state, inst.a));
	if(l.length == 0) _error("argument to && is zero length");
	if(Logical::isFalse(l[0])) {
		REG(state, inst.c) = Logical::False();
		return &inst+1;
	} else {
		Logical r = As<Logical>(state, REG(state, inst.b));
		if(r.length == 0) _error("argument to && is zero length");
		if(Logical::isFalse(r[0])) REG(state, inst.c) = Logical::False();
		else if(Logical::isNA(l[0]) || Logical::isNA(r[0])) REG(state, inst.c) = Logical::NA();
		else REG(state, inst.c) = Logical::True();
		return &inst+1;
	}
}
Instruction const* slor_op(State& state, Instruction const& inst) {
	Logical l = As<Logical>(state, REG(state, inst.a));
	if(l.length == 0) _error("argument to || is zero length");
	if(Logical::isTrue(l[0])) {
		REG(state, inst.c) = Logical::True();
		return &inst+1;
	} else {
		Logical r = As<Logical>(state, REG(state, inst.b));
		if(r.length == 0) _error("argument to || is zero length");
		if(Logical::isTrue(r[0])) REG(state, inst.c) = Logical::True();
		else if(Logical::isNA(l[0]) || Logical::isNA(r[0])) REG(state, inst.c) = Logical::NA();
		else REG(state, inst.c) = Logical::False();
		return &inst+1;
	}
}
Instruction const* function_op(State& state, Instruction const& inst) {
	REG(state, inst.c) = Function(state.frame.prototype->prototypes[inst.a], state.frame.environment);
	return &inst+1;
}
Instruction const* logical1_op(State& state, Instruction const& inst) {
	Integer i = As<Integer>(state, REG(state, inst.a));
	REG(state, inst.c) = Logical(i[0]);
	return &inst+1;
}
Instruction const* integer1_op(State& state, Instruction const& inst) {
	Integer i = As<Integer>(state, REG(state, inst.a));
	REG(state, inst.c) = Integer(i[0]);
	return &inst+1;
}
Instruction const* double1_op(State& state, Instruction const& inst) {
	Integer i = As<Integer>(state, REG(state, inst.a));
	REG(state, inst.c) = Double(i[0]);
	return &inst+1;
}
Instruction const* complex1_op(State& state, Instruction const& inst) {
	Integer i = As<Integer>(state, REG(state, inst.a));
	REG(state, inst.c) = Complex(i[0]);
	return &inst+1;
}
Instruction const* character1_op(State& state, Instruction const& inst) {
	Integer i = As<Integer>(state, REG(state, inst.a));
	Character r = Character(i[0]);
	for(int64_t j = 0; j < r.length; j++) r[j] = Symbols::empty;
	REG(state, inst.c) = r;
	return &inst+1;
}
Instruction const* raw1_op(State& state, Instruction const& inst) {
	Integer i = As<Integer>(state, REG(state, inst.a));
	REG(state, inst.c) = Raw(i[0]);
	return &inst+1;
}
Instruction const* type_op(State& state, Instruction const& inst) {
	Character c(1);
	// Should have a direct mapping from type to symbol.
	c[0] = state.StrToSym(Type::toString(REG(state, inst.a).type));
	REG(state, inst.c) = c;
	return &inst+1;
}

Instruction const* ret_op(State& state, Instruction const& inst) {
	*(state.frame.result) = REG(state, inst.c);
	// if this stack frame owns the environment, we can free it for reuse
	// as long as we don't return a closure...
	// TODO: but also can't if an assignment to an out of scope variable occurs (<<-, assign) with a value of a closure!
	if(state.frame.ownEnvironment && REG(state, inst.c).isClosureSafe())
		state.environments.push_back(state.frame.environment);
	state.base = state.frame.returnbase;
	Instruction const* returnpc = state.frame.returnpc;
	state.pop();
	return returnpc;
}
Instruction const* done_op(State& state, Instruction const& inst) {
	// not used. When this instruction is hit, interpreter exits.
	return 0;
}


static void printCode(State const& state, Prototype const* prototype) {
	std::string r = "block:\nconstants: " + intToStr(prototype->constants.size()) + "\n";
	for(int64_t i = 0; i < (int64_t)prototype->constants.size(); i++)
		r = r + intToStr(i) + "=\t" + state.stringify(prototype->constants[i]) + "\n";

	r = r + "code: " + intToStr(prototype->bc.size()) + "\n";
	for(int64_t i = 0; i < (int64_t)prototype->bc.size(); i++)
		r = r + intToStr(i) + ":\t" + prototype->bc[i].toString() + "\n";

	std::cout << r << std::endl;
}
#define THREADED_INTERPRETER

#ifdef THREADED_INTERPRETER
static const void** glabels = 0;
#endif

static Instruction const* buildStackFrame(State& state, Environment* environment, bool ownEnvironment, Prototype const* prototype, Value* result, Instruction const* returnpc) {
	//printCode(state, prototype);
	StackFrame& s = state.push();
	s.environment = environment;
	s.ownEnvironment = ownEnvironment;
	s.returnpc = returnpc;
	s.returnbase = state.base;
	s.result = result;
	s.prototype = prototype;
	state.base -= prototype->registers;
	if(state.base < state.registers)
		throw RiposteError("Register overflow");

#ifdef THREADED_INTERPRETER
	// Initialize threaded bytecode if not yet done 
	if(prototype->tbc.size() == 0)
	{
		for(int64_t i = 0; i < (int64_t)prototype->bc.size(); ++i) {
			Instruction const& inst = prototype->bc[i];
			prototype->tbc.push_back(
				Instruction(
					glabels[inst.bc],
					inst.a, inst.b, inst.c));
		}
	}
	return &(prototype->tbc[0]);
#else
	return &(prototype->bc[0]);
#endif
}

//
//    Main interpreter loop 
//
//__attribute__((__noinline__,__noclone__)) 
void interpret(State& state, Instruction const* pc) {
	if(state.tracing.is_tracing())
		pc = recording_interpret(state,pc);
#ifdef THREADED_INTERPRETER
    #define LABELS_THREADED(name,type,...) (void*)&&name##_label,
	static const void* labels[] = {BYTECODES(LABELS_THREADED)};
	if(pc == 0) { 
		glabels = labels;
		return;
	}

	goto *(pc->ibc);
	#define LABELED_OP(name,type,...) \
		name##_label: \
			{ pc = name##_op(state, *pc); goto *(pc->ibc); } 
	STANDARD_BYTECODES(LABELED_OP)
	done_label: {}
#else
	while(pc->bc != ByteCode::done) {
		switch(pc->bc) {
			#define SWITCH_OP(name,type,...) \
				case ByteCode::name: { pc = name##_op(state, *pc); } break;
			BYTECODES(SWITCH_OP)
		};
	}
#endif
}

// ensure glabels is inited before we need it.
void interpreter_init(State& state) {
#ifdef THREADED_INTERPRETER
	interpret(state, 0);
#endif
}

Value eval(State& state, Function const& function) {
	return eval(state, function.prototype(), function.environment());
}

Value eval(State& state, Prototype const* prototype) {
	return eval(state, prototype, state.frame.environment);
}

Value eval(State& state, Prototype const* prototype, Environment* environment) {
#ifdef THREADED_INTERPRETER
	static const Instruction* done = new Instruction(glabels[ByteCode::done]);
#else
	static const Instruction* done = new Instruction(ByteCode::done);
#endif
	int64_t stackSize = state.stack.size();
	// Build a half-hearted stack frame for the result. Necessary for the trace recorder.
	StackFrame& s = state.push();
	s.environment = 0;
	s.prototype = 0;
	s.returnbase = state.base;
	state.base -= 1;
	Value* result = state.base;
	
	Instruction const* run = buildStackFrame(state, environment, false, prototype, result, done);
	try {
		interpret(state, run);
		state.base = s.returnbase;
		state.pop();
	} catch(...) {
		state.base = s.returnbase;
		state.stack.resize(stackSize);
		throw;
	}
	return *result;
}

