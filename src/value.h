
#ifndef _RIPOSTE_VALUE_H
#define _RIPOSTE_VALUE_H

#include <map>
#include <iostream>
#include <vector>
#include <stack>
#include <deque>
#include <assert.h>
#include <limits>
#include <complex>

#include <gc/gc_cpp.h>
#include <gc/gc_allocator.h>

#include "type.h"
#include "bc.h"
#include "common.h"
#include "enum.h"
#include "string.h"
#include "exceptions.h"


#include "ir.h"
#include "recording.h"

struct Value {
	
	union {
		struct {
			Type::Enum type:4;
			int64_t length:60;
		};
		int64_t header;
	};
	union {
		void* p;
		int64_t i;
		double d;
		unsigned char c;
		String s;
		struct {
			Type::Enum typ;
			uint32_t ref;
		} future;
	};

	static void Init(Value& v, Type::Enum type, int64_t length) {
		v.header =  type + (length<<4);
	}

	// Warning: shallow equality!
	bool operator==(Value const& other) const {
		return header == other.header && p == other.p;
	}
	
	bool operator!=(Value const& other) const {
		return header != other.header || p != other.p;
	}
	
	bool isNil() const { return header == 0; }
	bool isNull() const { return type == Type::Null; }
	bool isLogical() const { return type == Type::Logical; }
	bool isInteger() const { return type == Type::Integer; }
	bool isLogical1() const { return header == (1<<4) + Type::Logical; }
	bool isInteger1() const { return header == (1<<4) + Type::Integer; }
	bool isDouble() const { return type == Type::Double; }
	bool isDouble1() const { return header == (1<<4) + Type::Double; }
	bool isCharacter() const { return type == Type::Character; }
	bool isCharacter1() const { return header == (1<<4) + Type::Character; }
	bool isList() const { return type == Type::List; }
	bool isSymbol() const { return type == Type::Symbol; }
	bool isPromise() const { return type == Type::Promise && !isNil(); }
	bool isFunction() const { return type == Type::Function; }
	bool isObject() const { return type == Type::Object; }
	bool isFuture() const { return type == Type::Future; }
	bool isMathCoerce() const { return isDouble() || isInteger() || isLogical(); }
	bool isLogicalCoerce() const { return isDouble() || isInteger() || isLogical(); }
	bool isVector() const { return isNull() || isLogical() || isInteger() || isDouble() || isCharacter() || isList(); }
	bool isClosureSafe() const { return isNull() || isLogical() || isInteger() || isDouble() || isFuture() || isCharacter() || isSymbol() || (isList() && length==0); }
	bool isConcrete() const { return type != Type::Promise; }

	template<class T> T& scalar() { throw "not allowed"; }
	template<class T> T const& scalar() const { throw "not allowed"; }

	static Value const& Nil() { static const Value v = { {{Type::Promise, 0}}, {0} }; return v; }
};

template<> inline int64_t& Value::scalar<int64_t>() { return i; }
template<> inline double& Value::scalar<double>() { return d; }
template<> inline unsigned char& Value::scalar<unsigned char>() { return c; }
template<> inline String& Value::scalar<String>() { return s; }

template<> inline int64_t const& Value::scalar<int64_t>() const { return i; }
template<> inline double const& Value::scalar<double>() const { return d; }
template<> inline unsigned char const& Value::scalar<unsigned char>() const { return c; }
template<> inline String const& Value::scalar<String>() const { return s; }


//
// Value type implementations
//

class Prototype;
class Environment;
class State;

// A symbol has the same format as a 1-element character vector.
struct Symbol : public Value {
	Symbol() {
		Value::Init(*this, Type::Symbol, 1);
		s = Strings::NA;
	} 
	
	explicit Symbol(String str) {
		Value::Init(*this, Type::Symbol, 1);
		s = str; 
	} 

	explicit Symbol(Value const& v) {
		assert(v.isSymbol() || v.isCharacter1()); 
		header = v.header;
		s = v.s;
	}

	operator String() const {
		return s;
	}

	operator Value() const {
		return *(Value*)this;
	}

	bool operator==(Symbol const& other) const { return s == other.s; }
	bool operator!=(Symbol const& other) const { return s != other.s; }
	bool operator==(String other) const { return s == other; }
	bool operator!=(String other) const { return s != other; }

	Symbol Clone() const { return *this; }
};


template<Type::Enum VType, typename ElementType, bool Recursive,
	bool canPack = sizeof(ElementType) <= sizeof(int64_t) && !Recursive>
struct Vector : public Value {
	typedef ElementType Element;
	static const int64_t width = sizeof(ElementType); 
	static const Type::Enum VectorType = VType;

	bool isScalar() const {
		return length == 1;
	}

	ElementType& s() { return canPack ? Value::scalar<ElementType>() : *(ElementType*)p; }
	ElementType const& s() const { return canPack ? Value::scalar<ElementType>() : *(ElementType const*)p; }

	ElementType const* v() const { return (canPack && isScalar()) ? &Value::scalar<ElementType>() : (ElementType const*)p; }
	ElementType* v() { return (canPack && isScalar()) ? &Value::scalar<ElementType>() : (ElementType*)p; }
	
	ElementType& operator[](int64_t index) { return v()[index]; }
	ElementType const& operator[](int64_t index) const { return v()[index]; }

	explicit Vector(int64_t length=0) {
		Value::Init(*this, VectorType, length);
		if(canPack && length > 1)
			p = Recursive ? new (GC) Element[length] : 
				new (PointerFreeGC) Element[length];
		else if(!canPack && length > 0)
			p = Recursive ? new (GC) Element[length] : 
				new (PointerFreeGC) Element[length];
	}

	static Vector<VType, ElementType, Recursive>& Init(Value& v, int64_t length) {
		Value::Init(v, VectorType, length);
		if(canPack && length > 1)
			v.p = Recursive ? new (GC) Element[length] : 
				new (PointerFreeGC) Element[length];
		else if(!canPack && length > 0)
			v.p = Recursive ? new (GC) Element[length] : 
				new (PointerFreeGC) Element[length];
		return (Vector<VType, ElementType, Recursive>&)v;
	}

	static void InitScalar(Value& v, ElementType const& d) {
		Value::Init(v, VectorType, 1);
		if(canPack)
			v.scalar<ElementType>() = d;
		else {
			v.p = Recursive ? new (GC) Element[1] : new (PointerFreeGC) Element[1];
			*(Element*)v.p = d;
		}
	}

	explicit Vector(Value const& v) {
		assert(v.type == VType);
		type = VType;
		length = v.length;
		p = v.p;
	}

	operator Value() const {
		return (Value&)*this;
	}
};

union _doublena {
	int64_t i;
	double d;
};


#define VECTOR_IMPL(Name, Element, Recursive) 				\
struct Name : public Vector<Type::Name, Element, Recursive> { 			\
	explicit Name(int64_t length=0) : Vector<Type::Name, Element, Recursive>(length) {} 	\
	explicit Name(Value const& v) : Vector<Type::Name, Element, Recursive>(v) {} 	\
	static Name c() { Name c(0); return c; } \
	static Name c(Element v0) { Name c(1); c[0] = v0; return c; } \
	static Name c(Element v0, Element v1) { Name c(2); c[0] = v0; c[1] = v1; return c; } \
	static Name c(Element v0, Element v1, Element v2) { Name c(3); c[0] = v0; c[1] = v1; c[2] = v2; return c; } \
	static Name c(Element v0, Element v1, Element v2, Element v3) { Name c(4); c[0] = v0; c[1] = v1; c[2] = v2; c[3] = v3; return c; } \
	const static Element NAelement; \
	static Name NA() { static Name na = Name::c(NAelement); return na; }  \
	static Name& Init(Value& v, int64_t length) { return (Name&)Vector<Type::Name, Element, Recursive>::Init(v, length); } \
	static void InitScalar(Value& v, Element const& d) { Vector<Type::Name, Element, Recursive>::InitScalar(v, d); }\
	Name Clone() const { Name c(length); memcpy(c.v(), v(), length*width); return c; }
/* note missing }; */

VECTOR_IMPL(Null, unsigned char, false)  
	static Null Singleton() { static Null s = Null::c(); return s; } 
	static bool isNA() { return false; }
	static bool isCheckedNA() { return false; }
};

VECTOR_IMPL(Logical, unsigned char, false)
	static Logical True() { static Logical t = Logical::c(1); return t; }
	static Logical False() { static Logical f = Logical::c(0); return f; } 
	
	static bool isTrue(unsigned char c) { return c == 1; }
	static bool isFalse(unsigned char c) { return c == 0; }
	static bool isNA(unsigned char c) { return c == NAelement; }
	static bool isCheckedNA(unsigned char c) { return isNA(c); }
	static bool isNaN(unsigned char c) { return false; }
	static bool isFinite(unsigned char c) { return false; }
	static bool isInfinite(unsigned char c) { return false; }
};

VECTOR_IMPL(Integer, int64_t, false)
	static bool isNA(int64_t c) { return c == NAelement; }
	static bool isCheckedNA(int64_t c) { return isNA(c); }
	static bool isNaN(int64_t c) { return false; }
	static bool isFinite(int64_t c) { return c != NAelement; }
	static bool isInfinite(int64_t c) { return false; }
}; 

VECTOR_IMPL(Double, double, false)
	static Double Inf() { static Double i = Double::c(std::numeric_limits<double>::infinity()); return i; }
	static Double NInf() { static Double i = Double::c(-std::numeric_limits<double>::infinity()); return i; }
	static Double NaN() { static Double n = Double::c(std::numeric_limits<double>::quiet_NaN()); return n; } 
	
	static bool isNA(double c) { _doublena a, b; a.d = c; b.d = NAelement; return a.i==b.i; }
	static bool isCheckedNA(int64_t c) { return false; }
	static bool isNaN(double c) { return (c != c) && !isNA(c); }
	static bool isFinite(double c) { return c == c && c != std::numeric_limits<double>::infinity() && c != -std::numeric_limits<double>::infinity(); }
	static bool isInfinite(double c) { return c == std::numeric_limits<double>::infinity() || c == -std::numeric_limits<double>::infinity(); }
};

VECTOR_IMPL(Character, String, false)
	static bool isNA(String c) { return c == Strings::NA; }
	static bool isCheckedNA(String c) { return isNA(c); }
	static bool isNaN(String c) { return false; }
	static bool isFinite(String c) { return false; }
	static bool isInfinite(String c) { return false; }
};

VECTOR_IMPL(Raw, unsigned char, false) 
	static bool isNA(unsigned char c) { return false; }
	static bool isCheckedNA(unsigned char c) { return false; }
	static bool isNaN(unsigned char c) { return false; }
	static bool isFinite(unsigned char c) { return false; }
	static bool isInfinite(unsigned char c) { return false; }
};

VECTOR_IMPL(List, Value, true) 
	static bool isNA(Value const& c) { return c.isNil(); }
	static bool isCheckedNA(Value const& c) { return isNA(c); }
	static bool isNaN(Value const& c) { return false; }
	static bool isFinite(Value const& c) { return false; }
	static bool isInfinite(Value const& c) { return false; }
};


struct Future : public Value {
	static void Init(Value & f, Type::Enum typ,int64_t length, IRef ref) {
		Value::Init(f,Type::Future,length);
		f.future.ref = ref;
		f.future.typ = typ;
	}
	
	Future Clone() const { throw("shouldn't be cloning futures"); }
};


class Function {
private:
	Prototype* proto;
	Environment* env;
public:
	explicit Function(Prototype* proto, Environment* env)
		: proto(proto), env(env) {}
	
	explicit Function(Value const& v) {
		assert(v.isFunction() || v.isPromise());
		proto = (Prototype*)(v.length << 4);
		env = (Environment*)v.p;
	}

	operator Value() const {
		Value v;
		v.header = (int64_t)proto + Type::Function;
		v.p = env;
		return v;
	}

	Value AsPromise() const {
		Value v;
		v.header = (int64_t)proto + Type::Promise;
		v.p = env;
		return v;
	}

	Prototype* prototype() const { return proto; }
	Environment* environment() const { return env; }

	Function Clone() const { return *this; }
};

class REnvironment {
private:
	Environment* env;
public:
	explicit REnvironment(Environment* env) : env(env) {
	}
	explicit REnvironment(Value const& v) {
		assert(v.type == Type::Environment);
		env = (Environment*)v.p;
	}
	
	operator Value() const {
		Value v;
		Value::Init(v, Type::Environment, 0);
		v.p = (void*)env;
		return v;
	}
	Environment* ptr() const {
		return env;
	}

	REnvironment Clone() const { return *this; }
};

class Dictionary : public gc {
protected:
	static uint64_t globalRevision;
	uint64_t revision;

	static const uint64_t inlineSize = 16;
	struct Pair { String n; Value v; };
	Pair* d;
	Pair inlineDict[inlineSize];
	uint64_t size, load;

public:
	Dictionary() : revision(++globalRevision), d(inlineDict), size(inlineSize) {
		clear();
	}
	
	// for now, do linear probing
	// this returns the location of the String s (or where it was stored before being deleted).
	// or, if s doesn't exist, the location at which s should be inserted.
	uint64_t find(String s) const {
		uint64_t i = (uint64_t)s.i & (size-1);	// hash this?
		while(d[i].n != s && d[i].n != Strings::NA) {
			i = (i+1) & (size-1);
		}
		assert(i >= 0 && i < size);
		return i; 
	}
	
	uint64_t assign(String name, Value const& value) {
		uint64_t i = find(name);
		if(value.isNil()) {
			// deleting a value changes the revision number! 
			if(d[i].n != Strings::NA) { 
				load--; 
				d[i] = (Pair) { Strings::NA, Value::Nil() }; 
				revision = ++globalRevision; 
			}
		} else {
			if(d[i].n == Strings::NA) { 
				load++;
				if((load * 2) > size) {
					rehash(size * 2);
					i = find(name);
				}
				d[i] = (Pair) { name, value };
			} else {
				d[i].v = value;
			}
		}
		return i;
	}

	Value const& get(String name) const {
		uint64_t i = find(name);
		if(d[i].n != Strings::NA) return d[i].v;
		else return Value::Nil();
	}

	void rehash(uint64_t s) {
		uint64_t old_size = size;
		Pair* old_d = d;
		s = nextPow2(s);
		if(s <= size) return; // should rehash on shrinking sometimes, when?

		size = s;
		d = new (GC) Pair[size];
		clear();	// this increments the revision
		
		// copy over previous populated values...
		for(uint64_t i = 0; i < old_size; i++) {
			if(old_d[i].n != Strings::NA) {
				d[find(old_d[i].n)] = old_d[i];
			}
		}
	}

	void clear() {
		load = 0; 
		for(uint64_t i = 0; i < size; i++) {
			// wiping v too makes sure we're not holding unnecessary pointers
			d[i] = (Pair) { Strings::NA, Value::Nil() };
		}
		revision = ++globalRevision;
	}

	struct Pointer {
		Dictionary* env;
		String name;
		uint64_t revision;
		uint64_t index;
	};

	uint64_t getRevision() const { return revision; }
	bool equalRevision(uint64_t i) const { return i == revision; }
	
	Value const& get(uint64_t index) const {
		assert(index >= 0 && index < size);
		return d[index].v;
	}

	void assign(uint64_t index, Value const& value) {
		assert(index >= 0 && index < size);
		d[index].v = value;
	}
	
	// making a pointer only works if the entry already exists 
	Pointer makePointer(String name) {
		uint64_t i = find(name);
		if(d[i].n == Strings::NA) _error("Making pointer to non-existant variable"); 
		return (Pointer) { this, name, revision, i };
	}

	Value const& get(Pointer const& p) {
		if(p.revision == revision) return get(p.index);
		else return get(p.name);
	}

	void assign(Pointer const& p, Value const& value) {
		if(p.revision == revision) assign(p.index, value);
		else assign(p.name, value);
	}
};

struct Object : public Value {

	struct Inner : public gc {
		Value base;
		Dictionary attributes;
	};

	// Contract: base is a non-object type.
	Value& base() { return ((Inner*)p)->base; }
	Value const& base() const { return ((Inner*)p)->base; }
	Dictionary& attributes() { return ((Inner*)p)->attributes; }
	Dictionary const& attributes() const { return ((Inner*)p)->attributes; }

	static void Init(Value& v, Value const& _base) {
		Inner* inner = new (GC) Inner();
		inner->base = _base;
		// doing this after ensures that it will work even if base and v overlap.
		Value::Init(v, Type::Object, 0);
		v.p = (void*)inner;
	}

	static void InitWithNames(Value& v, Value const& _base, Value const& _names) {
		Init(v, _base);
		if(!_names.isNil())
			((Object&)v).setNames(_names);
	}
	
	static void Init(Value& v, Value const& _base, Value const& _names, Value const& className) {
		InitWithNames(v, _base, _names);
		((Object&)v).setClass(className);
	}

	bool hasAttribute(String s) const {
		return !attributes().get(s).isNil();
	}

	bool hasNames() const { return hasAttribute(Strings::names); }
	bool hasClass() const { return hasAttribute(Strings::classSym); }
	bool hasDim() const   { return hasAttribute(Strings::dim); }

	Value getAttribute(String s) const {
		return attributes().get(s);
	}

	Value getNames() const { return getAttribute(Strings::names); }
	Value getClass() const { return getAttribute(Strings::classSym); }
	Value getDim() const { return getAttribute(Strings::dim); }

	void setAttribute(String s, Value const& v) {
		attributes().assign(s, v);
	}
	
	void setNames(Value const& v) { setAttribute(Strings::names, v); }
	void setClass(Value const& v) { setAttribute(Strings::classSym, v); }
	void setDim(Value const& v)  { setAttribute(Strings::dim, v); }

	String className() const {
		if(!hasClass()) {
			return String::Init(base().type);	// TODO: make sure types line up correctly with strings
		}
		else {
			return Character(getClass())[0];
		}
	}

	Object Clone() const { 
		Value v; 
		Inner* inner = new (GC) Inner();
		inner->base = ((Inner*)p)->base;
		inner->attributes = ((Inner*)p)->attributes;
		// doing this after ensures that it will work even if base and v overlap.
		Value::Init(v, Type::Object, 0);
		v.p = (void*)inner;
		return (Object const&)v;
	}
};

inline Value CreateExpression(List const& list) {
	Value v;
	Object::Init(v, list, Value::Nil(), Character::c(Strings::Expression));
	return v;
}

inline Value CreateCall(List const& list, Value const& names = Value::Nil()) {
	Value v;
	Object::Init(v, list, names, Character::c(Strings::Call));
	return v;
}




////////////////////////////////////////////////////////////////////
// VM data structures
///////////////////////////////////////////////////////////////////


struct CompiledCall : public gc {
	List call;

	List arguments;
	Character names;
	int64_t dots;
	
	explicit CompiledCall(List const& call, List const& arguments, Character const& names, int64_t dots) 
		: call(call), arguments(arguments), names(names), dots(dots) {}
};

struct Prototype : public gc {
	Value expression;
	String string;

	Character parameters;
	List defaults;

	int dots;

	int registers;
	std::vector<Value, traceable_allocator<Value> > constants;
	std::vector<Prototype*, traceable_allocator<Prototype*> > prototypes; 	
	std::vector<CompiledCall, traceable_allocator<CompiledCall> > calls; 

	std::vector<Instruction> bc;			// bytecode
	mutable std::vector<Instruction> tbc;		// threaded bytecode
};

/*
 * Riposte execution environments are split into two parts:
 * 1) Environment -- the static part, exposed to the R level as an R environment, these may not obey stack discipline, thus allocated on heap, try to reuse to decrease allocation overhead.
 *     -static link to enclosing environment
 *     -dynamic link to calling environment (necessary for promises which need to see original call stack)
 *     -slots storing variables that may be accessible by inner functions that may be returned 
 *		(for now, conservatively assume all variables are accessible)
 *     -names of slots
 *     -map storing overflow variables or variables that are created dynamically 
 * 		(e.g. assign() in a nested function call)
 *
 * 2) Stack Frame -- the dynamic part, not exposed at the R level, these obey stack discipline
 *     -pointer to associated Environment
 *     -pointer to Stack Frame of calling function (the dynamic link)
 *     -pointer to constant file
 *     -pointer to registers
 *     -return PC
 *     -result register pointer
 */


class Environment : public Dictionary {
private:
	Environment* lexical, *dynamic;
	
public:
	Value call;
	std::vector<String> dots;

	explicit Environment(Environment* lexical=0, Environment* dynamic=0) : 
			lexical(lexical), dynamic(dynamic), call(Null::Singleton()) {}

	void init(Environment* l, Environment* d, Value const& call) {
		lexical = l;
		dynamic = d;
		this->call = call;
		clear();
		dots.clear();
	}
	
	Environment* LexicalScope() const { return lexical; }
	Environment* DynamicScope() const { return dynamic; }

};

struct StackFrame {
	Environment* environment;
	bool ownEnvironment;
	Prototype const* prototype;

	Instruction const* returnpc;
	Value* returnbase;
	Value* result;
};

#define TRACE_MAX_NODES (128)
#define TRACE_MAX_OUTPUTS (128)
#define TRACE_MAX_VECTOR_REGISTERS (32)
#define TRACE_VECTOR_WIDTH (64)
//maximum number of instructions to record before dropping out of the
//recording interpreter
#define TRACE_MAX_RECORDED (1024)

struct Trace {
	double registers [TRACE_MAX_VECTOR_REGISTERS][TRACE_VECTOR_WIDTH] __attribute__ ((aligned (16))); //for sse alignment

	IRNode nodes[TRACE_MAX_NODES] ;
	size_t n_nodes;
	size_t n_recorded;

	int64_t length;

	struct Location {
		enum Type {REG, VAR};
		Type type;
		Environment::Pointer pointer; //fat pointer to environment location
		//if type == REG, pointer.env points to the base pointer
		//and pointer.index is its offset
		//this should go into a union but Environment::Pointer can't go in a union since Symbol has a constructor...
	};
	struct Output {
		Location location; //location where an output might exist
		                   //if that location is live and contains a future then that is a live output
		IRef ref; //(used only to enable pretty printing) value of the output in the trace code,
	};

	Output outputs[TRACE_MAX_OUTPUTS];
	size_t n_outputs;
	Value * max_live_register_base;
	int64_t max_live_register;

	void reset();
	void execute(State & state);
	std::string toString(State & state);
};

//member of State, manages information for all traces
//and the currently recording trace (if any)

struct TraceState {
	TraceState() {
		active = false;
		enabled = true;
		verbose = false;
	}

	bool enabled;
	bool verbose;
	bool active;

	Trace current_trace;


	bool is_tracing() const { return active; }

	Instruction const * begin_tracing(State & state, Instruction const * inst, size_t length) {
		if(active) {
			_error("recursive record\n");
		}
		current_trace.reset();
		current_trace.length = length;
		active = true;
		return recording_interpret(state,inst);

	}

	void end_tracing(State & state) {
		if(active) {
			active = false;
			current_trace.execute(state);
		}
	}
};

// TODO: Careful, args and result might overlap!
typedef void (*InternalFunctionPtr)(State& s, Value const* args, Value& result);

struct InternalFunction {
	InternalFunctionPtr ptr;
	int64_t params;
};

#define DEFAULT_NUM_REGISTERS 10000

struct State {
	Value* base;
	Value* registers;

	std::vector<StackFrame, traceable_allocator<StackFrame> > stack;
	StackFrame frame;
	std::vector<Environment*, traceable_allocator<Environment*> > environments;

	std::vector<Environment*, traceable_allocator<Environment*> > path;
	Environment* global;

	StringTable strings;
	
	std::vector<std::string> warnings;

	std::vector<InternalFunction> internalFunctions;
	std::map<String, int64_t> internalFunctionIndex;
	
	TraceState tracing; //all state related to tracing compiler

	State(Environment* global, Environment* base) {
		this->global = global;
		path.push_back(base);
		registers = new (GC) Value[DEFAULT_NUM_REGISTERS];
		this->base = registers + DEFAULT_NUM_REGISTERS;
	}

	StackFrame& push() {
		stack.push_back(frame);
		return frame;
	}

	void pop() {
		frame = stack.back();
		stack.pop_back();
	}

	std::string stringify(Value const& v) const;
	std::string stringify(Trace const & t) const;
	std::string deparse(Value const& v) const;

	String internStr(std::string s) {
		return strings.in(s);
	}

	std::string externStr(String s) const {
		return strings.out(s);
	}

	void registerInternalFunction(String s, InternalFunctionPtr internalFunction, int64_t params) {
		InternalFunction i = { internalFunction, params };
		internalFunctions.push_back(i);
		internalFunctionIndex[s] = internalFunctions.size()-1;
	}
};



Value eval(State& state, Function const& function);
Value eval(State& state, Prototype const* prototype, Environment* environment); 
Value eval(State& state, Prototype const* prototype);
void interpreter_init(State& state);

#endif
