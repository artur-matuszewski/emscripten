
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <vector>
#include <ostream>
#include <iostream>
#include <iomanip>
#include <functional>
#include <unordered_set>
#include <unordered_map>

#include "parser.h"

#define err(str) fprintf(stderr, str "\n");
#define errv(str, ...) fprintf(stderr, str "\n", __VA_ARGS__);
#define printErr err

using namespace cashew;

class Ref;
struct Value;

void dump(const char *str, Ref node, bool pretty=false);

// Reference to a value, plus some operators for convenience
struct Ref {
  Value* inst;

  Ref(Value *v=nullptr) : inst(v) {}

  Value* get() { return inst; }

  Value& operator*() { return *inst; }
  Value* operator->() { return inst; }
  Ref& operator[](unsigned x);
  Ref& operator[](IString x);

  // special conveniences
  bool operator==(const char *str); // comparison to string, which is by value
  bool operator!=(const char *str);
  bool operator==(const IString &str);
  bool operator!=(const IString &str);
  bool operator==(double d) { assert(0); return false; } // prevent Ref == number, which is potentially ambiguous; use ->getNumber() == number
  bool operator==(Ref other);
  bool operator!(); // check if null, in effect
};

// Arena allocation, free it all on process exit

struct Arena {
  #define CHUNK_SIZE 1000
  std::vector<Value*> chunks;
  int index; // in last chunk

  Arena() : index(0) {}

  Ref alloc();
};

extern Arena arena;

// Main value type
struct Value {
  enum Type {
    String = 0,
    Number = 1,
    Array = 2,
    Null = 3,
    Bool = 4,
    Object = 5
  };

  Type type;

  typedef std::vector<Ref> ArrayStorage;
  typedef std::unordered_map<IString, Ref> ObjectStorage;

  union { // TODO: optimize
    IString str;
    double num;
    ArrayStorage *arr;
    bool boo;
    ObjectStorage *obj;
  };

  // constructors all copy their input
  Value() : type(Null), num(0) {}
  explicit Value(const char *s) : type(Null) {
    setString(s);
  }
  explicit Value(double n) : type(Null) {
    setNumber(n);
  }
  explicit Value(ArrayStorage &a) : type(Null) {
    setArray();
    *arr = a;
  }
  // no bool constructor - would endanger the double one (int might convert the wrong way)

  ~Value() {
    free();
  }

  void free() {
    if (type == Array) delete arr;
    else if (type == Object) delete obj;
    type = Null;
    num = 0;
  }

  Value& setString(const char *s) {
    free();
    type = String;
    str.set(s);
    return *this;
  }
  Value& setString(const IString &s) {
    free();
    type = String;
    str.set(s);
    return *this;
  }
  Value& setNumber(double n) {
    free();
    type = Number;
    num = n;
    return *this;
  }
  Value& setArray(ArrayStorage &a) {
    free();
    type = Array;
    arr = new ArrayStorage();
    *arr = a;
    return *this;
  }
  Value& setArray() {
    free();
    type = Array;
    arr = new ArrayStorage();
    return *this;
  }
  Value& setNull() {
    free();
    type = Null;
    return *this;
  }
  Value& setBool(bool b) { // Bool in the name, as otherwise might overload over int
    free();
    type = Bool;
    boo = b;
    return *this;
  }
  Value& setObject() {
    free();
    type = Object;
    obj = new ObjectStorage();
    return *this;
  }

  bool isString() { return type == String; }
  bool isNumber() { return type == Number; }
  bool isArray()  { return type == Array; }
  bool isNull()   { return type == Null; }
  bool isBool()   { return type == Bool; }
  bool isObject()  { return type == Object; }

  bool isBool(bool b) { return type == Bool && b == boo; } // avoid overloading == as it might overload over int

  const char* getCString() {
    assert(isString());
    return str.str;
  }
  IString& getIString() {
    assert(isString());
    return str;
  }
  double& getNumber() {
    assert(isNumber());
    return num;
  }
  ArrayStorage& getArray() {
    assert(isArray());
    return *arr;
  }
  bool& getBool() {
    assert(isBool());
    return boo;
  }

  Value& operator=(const Value& other) {
    free();
    switch (other.type) {
      case String:
        setString(other.str);
        break;
      case Number:
        setNumber(other.num);
        break;
      case Array:
        setArray(*other.arr);
        break;
      case Null:
        setNull();
        break;
      case Bool:
        setBool(other.boo);
        break;
      case Object:
        assert(0); // TODO
    }
    return *this;
  }

  bool operator==(const Value& other) {
    if (type != other.type) return false;
    switch (other.type) {
      case String:
        return str == other.str;
      case Number:
        return num == other.num;
      case Array:
        return this == &other; // if you want a deep compare, use deepCompare
      case Null:
        break;
      case Bool:
        return boo == other.boo;
      case Object:
        return this == &other; // if you want a deep compare, use deepCompare
    }
    return true;
  }

  bool deepCompare(Ref ref) {
    Value& other = *ref;
    if (*this == other) return true; // either same pointer, or identical value type (string, number, null or bool)
    if (type != other.type) return false;
    if (type == Array) {
      if (arr->size() != other.arr->size()) return false;
      for (unsigned i = 0; i < arr->size(); i++) {
        if (!(*arr)[i]->deepCompare((*other.arr)[i])) return false;
      }
      return true;
    } else if (type == Object) {
      if (obj->size() != other.obj->size()) return false;
      for (auto i : *obj) {
        if (other.obj->count(i.first) == 0) return false;
        if (i.second->deepCompare((*other.obj)[i.first])) return false;
      }
      return true;
    }
    return false;
  }

  char* parse(char* curr) {
    #define is_json_space(x) (x == 32 || x == 9 || x == 10 || x == 13) /* space, tab, linefeed/newline, or return */
    #define skip() { while (*curr && is_json_space(*curr)) curr++; }
    skip();
    if (*curr == '"') {
      // String
      curr++;
      char *close = strchr(curr, '"');
      assert(close);
      *close = 0; // end this string, and reuse it straight from the input
      setString(curr);
      curr = close+1;
    } else if (*curr == '[') {
      // Array
      curr++;
      skip();
      setArray();
      while (*curr != ']') {
        Ref temp = arena.alloc();
        arr->push_back(temp);
        curr = temp->parse(curr);
        skip();
        if (*curr == ']') break;
        assert(*curr == ',');
        curr++;
        skip();
      }
      curr++;
    } else if (*curr == 'n') {
      // Null
      assert(strncmp(curr, "null", 4) == 0);
      setNull();
      curr += 4;
    } else if (*curr == 't') {
      // Bool true
      assert(strncmp(curr, "true", 4) == 0);
      setBool(true);
      curr += 4;
    } else if (*curr == 'f') {
      // Bool false
      assert(strncmp(curr, "false", 5) == 0);
      setBool(false);
      curr += 5;
    } else if (*curr == '{') {
      // Object
      curr++;
      skip();
      setObject();
      while (*curr != '}') {
        assert(*curr == '"');
        curr++;
        char *close = strchr(curr, '"');
        assert(close);
        *close = 0; // end this string, and reuse it straight from the input
        IString key(curr);
        curr = close+1;
        skip();
        assert(*curr == ':');
        curr++;
        skip();
        Ref value = arena.alloc();
        curr = value->parse(curr);
        (*obj)[key] = value;
        skip();
        if (*curr == '}') break;
        assert(*curr == ',');
        curr++;
        skip();
      }
      curr++;
    } else {
      // Number
      char *after;
      setNumber(strtod(curr, &after));
      curr = after;
    }
    return curr;
  }

  void stringify(std::ostream &os, bool pretty=false) {
    static int indent = 0;
    #define indentify() { for (int i = 0; i < indent; i++) os << "  "; }
    switch (type) {
      case String:
        os << '"' << str.str << '"';
        break;
      case Number:
        os << std::setprecision(17) << num; // doubles can have 17 digits of precision
        break;
      case Array:
        if (arr->size() == 0) {
          os << "[]";
          break;
        }
        os << '[';
        if (pretty) {
          os << std::endl;
          indent++;
        }
        for (unsigned i = 0; i < arr->size(); i++) {
          if (i > 0) {
            if (pretty) os << "," << std::endl;
            else os << ", ";
          }
          indentify();
          (*arr)[i]->stringify(os, pretty);
        }
        if (pretty) {
          os << std::endl;
          indent--;
        }
        indentify();
        os << ']';
        break;
      case Null:
        os << "null";
        break;
      case Bool:
        os << (boo ? "true" : "false");
        break;
      case Object:
        os << '{';
        if (pretty) {
          os << std::endl;
          indent++;
        }
        bool first = true;
        for (auto i : *obj) {
          if (first) {
            first = false;
          } else {
            os << ", ";
            if (pretty) os << std::endl;
          }
          indentify();
          os << '"' << i.first.c_str() << "\": ";
          i.second->stringify(os, pretty);
        }
        if (pretty) {
          os << std::endl;
          indent--;
        }
        indentify();
        os << '}';
        break;
    }
  }

  // String operations

  // Number operations

  // Array operations

  unsigned size() {
    assert(isArray());
    return arr->size();
  }

  void setSize(unsigned size) {
    assert(isArray());
    unsigned old = arr->size();
    if (old != size) arr->resize(size);
    if (old < size) {
      for (unsigned i = old; i < size; i++) {
        (*arr)[i] = arena.alloc();
      }
    }
  }

  Ref& operator[](unsigned x) {
    assert(isArray());
    assert(x < arr->size());
    return (*arr)[x];
  }

  Value& push_back(Ref r) {
    assert(isArray());
    arr->push_back(r);
    return *this;
  }
  Ref pop_back() {
    assert(isArray());
    Ref ret = arr->back();
    arr->pop_back();
    return ret;
  }

  Ref back() {
    assert(isArray());
    if (arr->size() == 0) return nullptr;
    return arr->back();
  }

  void splice(int x, int num) {
    assert(isArray());
    arr->erase(arr->begin() + x, arr->begin() + x + num);
  }

  void insert(int x, int num) {
    arr->insert(arr->begin() + x, num, Ref());
  }
  void insert(int x, Ref node) {
    arr->insert(arr->begin() + x, 1, node);
  }

  int indexOf(Ref other) {
    assert(isArray());
    for (unsigned i = 0; i < arr->size(); i++) {
      if (other == (*arr)[i]) return i;
    }
    return -1;
  }

  Ref map(std::function<Ref (Ref node)> func) {
    assert(isArray());
    Ref ret = arena.alloc();
    ret->setArray();
    for (unsigned i = 0; i < arr->size(); i++) {
      ret->push_back(func((*arr)[i]));
    }
    return ret;
  }

  Ref filter(std::function<bool (Ref node)> func) {
    assert(isArray());
    Ref ret = arena.alloc();
    ret->setArray();
    for (unsigned i = 0; i < arr->size(); i++) {
      Ref curr = (*arr)[i];
      if (func(curr)) ret->push_back(curr);
    }
    return ret;
  }

  /*
  void forEach(std::function<void (Ref)> func) {
    for (unsigned i = 0; i < arr->size(); i++) {
      func((*arr)[i]);
    }
  }
  */

  // Null operations

  // Bool operations

  // Object operations

  Ref& operator[](IString x) {
    assert(isObject());
    return (*obj)[x];
  }

  bool has(IString x) {
    assert(isObject());
    return obj->count(x) > 0;
  }
};

// cashew builder

class ValueBuilder {
  static IStringSet statable;

  static Ref makeRawString(const IString& s) {
    return &arena.alloc()->setString(s);
  }

  static Ref makeRawArray() {
    return &arena.alloc()->setArray();
  }

  static Ref makeNull() {
    return &arena.alloc()->setNull();
  }

public:
  static Ref makeToplevel() {
    return &makeRawArray()->push_back(makeRawString(TOPLEVEL))
                           .push_back(makeRawArray());
  }

  static Ref makeString(IString str) {
    return &makeRawArray()->push_back(makeRawString(STRING))
                           .push_back(makeRawString(str));
  }

  static Ref makeBlock() {
    return &makeRawArray()->push_back(makeRawString(BLOCK))
                           .push_back(makeRawArray());
  }

  static Ref makeName(IString name) {
    return &makeRawArray()->push_back(makeRawString(NAME))
                           .push_back(makeRawString(name));
  }

  static void appendToBlock(Ref block, Ref element) {
    if (block[0] == BLOCK || block[0] == TOPLEVEL) {
      block[1]->push_back(element);
    } else if (block[0] == DEFUN) {
      block[3]->push_back(element);
    } else assert(0);
  }

  static Ref makeCall(Ref target) {
    return &makeRawArray()->push_back(makeRawString(CALL))
                           .push_back(target)
                           .push_back(makeRawArray());
  }

  static void appendToCall(Ref call, Ref element) {
    assert(call[0] == CALL);
    call[2]->push_back(element);
  }

  static Ref makeStatement(Ref contents) {
    if (statable.has(contents[0]->getIString())) {
      return &makeRawArray()->push_back(makeRawString(STAT))
                             .push_back(contents);
    } else {
      return contents; // only very specific things actually need to be stat'ed
    }
  }

  static Ref makeNumber(double num) {
    return &makeRawArray()->push_back(makeRawString(NUM))
                           .push_back(&arena.alloc()->setNumber(num));
  }

  static Ref makeBinary(Ref left, IString op, Ref right) {
    if (op == SET) {
      return &makeRawArray()->push_back(makeRawString(ASSIGN))
                             .push_back(&arena.alloc()->setBool(true))
                             .push_back(left)
                             .push_back(right);
    } else if (op == COMMA) {
      return &makeRawArray()->push_back(makeRawString(SEQ))
                             .push_back(left)
                             .push_back(right);
    } else {
      return &makeRawArray()->push_back(makeRawString(BINARY))
                             .push_back(makeRawString(op))
                             .push_back(left)
                             .push_back(right);
    }
  }

  static Ref makePrefix(IString op, Ref right) {
    return &makeRawArray()->push_back(makeRawString(UNARY_PREFIX))
                           .push_back(makeRawString(op))
                           .push_back(right);
  }

  static Ref makeFunction(IString name) {
    return &makeRawArray()->push_back(makeRawString(DEFUN))
                           .push_back(makeRawString(name))
                           .push_back(makeRawArray())
                           .push_back(makeRawArray());
  }

  static void appendArgumentToFunction(Ref func, IString arg) {
    assert(func[0] == DEFUN);
    func[2]->push_back(makeRawString(arg));
  }

  static Ref makeVar() {
    return &makeRawArray()->push_back(makeRawString(VAR))
                           .push_back(makeRawArray());
  }

  static void appendToVar(Ref var, IString name, Ref value) {
    assert(var[0] == VAR);
    Ref array = &makeRawArray()->push_back(makeRawString(name));
    if (!!value) array->push_back(value);
    var[1]->push_back(array);
  }

  static Ref makeReturn(Ref value) {
    return &makeRawArray()->push_back(makeRawString(RETURN)).push_back(!!value ? value : makeNull());
  }

  static Ref makeIndexing(Ref target, Ref index) {
    return &makeRawArray()->push_back(makeRawString(SUB))
                           .push_back(target)
                           .push_back(index);
  }

  static Ref makeIf(Ref condition, Ref ifTrue, Ref ifFalse) {
    return &makeRawArray()->push_back(makeRawString(IF))
                           .push_back(condition)
                           .push_back(ifTrue)
                           .push_back(!!ifFalse ? ifFalse : makeNull());
  }

  static Ref makeConditional(Ref condition, Ref ifTrue, Ref ifFalse) {
    return &makeRawArray()->push_back(makeRawString(CONDITIONAL))
                           .push_back(condition)
                           .push_back(ifTrue)
                           .push_back(ifFalse);
  }

  static Ref makeDo(Ref body, Ref condition) {
    return &makeRawArray()->push_back(makeRawString(DO))
                           .push_back(condition)
                           .push_back(body);
  }

  static Ref makeWhile(Ref condition, Ref body) {
    return &makeRawArray()->push_back(makeRawString(WHILE))
                           .push_back(condition)
                           .push_back(body);
  }

  static Ref makeBreak(IString label) {
    return &makeRawArray()->push_back(makeRawString(BREAK)).push_back(!!label ? makeRawString(label) : makeNull());
  }

  static Ref makeContinue(IString label) {
    return &makeRawArray()->push_back(makeRawString(CONTINUE)).push_back(!!label ? makeRawString(label) : makeNull());
  }

  static Ref makeLabel(IString name, Ref body) {
    return &makeRawArray()->push_back(makeRawString(LABEL))
                           .push_back(makeRawString(name))
                           .push_back(body);
  }

  static Ref makeSwitch(Ref input) {
    return &makeRawArray()->push_back(makeRawString(SWITCH))
                           .push_back(input)
                           .push_back(makeRawArray());
  }

  static void appendCaseToSwitch(Ref switch_, Ref arg) {
    assert(switch_[0] == SWITCH);
    switch_[2]->push_back(&makeRawArray()->push_back(arg).push_back(makeRawArray()));
  }

  static void appendDefaultToSwitch(Ref switch_) {
    assert(switch_[0] == SWITCH);
    switch_[2]->push_back(&makeRawArray()->push_back(makeNull()).push_back(makeRawArray()));
  }

  static void appendCodeToSwitch(Ref switch_, Ref code, bool explicitBlock) {
    assert(switch_[0] == SWITCH);
    assert(code[0] == BLOCK);
    if (!explicitBlock) {
      for (int i = 0; i < code[1]->size(); i++) {
        switch_[2]->back()->back()->push_back(code[1][i]);
      }
    } else {
      switch_[2]->back()->back()->push_back(code);
    }
  }

  static Ref makeDot(Ref obj, IString key) {
    return &makeRawArray()->push_back(makeRawString(DOT))
                           .push_back(obj)
                           .push_back(makeRawString(key));
  }

  static Ref makeNew(Ref call) {
    return &makeRawArray()->push_back(makeRawString(NEW))
                           .push_back(call);
  }

  static Ref makeArray() {
    return &makeRawArray()->push_back(makeRawString(ARRAY))
                           .push_back(makeRawArray());
  }

  static void appendToArray(Ref array, Ref element) {
    assert(array[0] == ARRAY);
    array[1]->push_back(element);
  }

  static Ref makeObject() {
    return &makeRawArray()->push_back(makeRawString(OBJECT))
                           .push_back(makeRawArray());
  }

  static void appendToObject(Ref array, IString key, Ref value) {
    assert(array[0] == OBJECT);
    array[1]->push_back(&makeRawArray()->push_back(makeRawString(key))
                                        .push_back(value));
  }
};
