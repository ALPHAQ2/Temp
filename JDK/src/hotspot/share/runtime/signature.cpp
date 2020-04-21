/*
 * Copyright (c) 1997, 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "oops/typeArrayKlass.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/signature.hpp"

// Implementation of SignatureIterator

// Signature syntax:
//
// Signature  = "(" {Parameter} ")" ReturnType.
// Parameter  = FieldType.
// ReturnType = FieldType | "V".
// FieldType  = "B" | "C" | "D" | "F" | "I" | "J" | "S" | "Z" | "L" ClassName ";" | "[" FieldType.
// ClassName  = string.

// The ClassName string can be any JVM-style UTF8 string except:
//  - an empty string (the empty string is never a name of any kind)
//  - a string which begins or ends with slash '/' (the package separator)
//  - a string which contains adjacent slashes '//' (no empty package names)
//  - a string which contains a semicolon ';' (the end-delimiter)
//  - a string which contains a left bracket '[' (the array marker)
//  - a string which contains a dot '.' (the external package separator)
//
// Other "meta-looking" characters, such as '(' and '<' and '+',
// are perfectly legitimate within a class name, for the JVM.
// Class names which contain double slashes ('a//b') and non-initial
// brackets ('a[b]') are reserved for possible enrichment of the
// type language.

void SignatureIterator::set_fingerprint(fingerprint_t fingerprint) {
  if (!fp_is_valid(fingerprint)) {
    _fingerprint = fingerprint;
    _return_type = T_ILLEGAL;
  } else if (fingerprint != _fingerprint) {
    assert(_fingerprint == zero_fingerprint(), "consistent fingerprint values");
    _fingerprint = fingerprint;
    _return_type = fp_return_type(fingerprint);
  }
}

BasicType SignatureIterator::return_type() {
  if (_return_type == T_ILLEGAL) {
    SignatureStream ss(_signature);
    ss.skip_to_return_type();
    _return_type = ss.type();
    assert(_return_type != T_ILLEGAL, "illegal return type");
  }
  return _return_type;
}

bool SignatureIterator::fp_is_valid_type(BasicType type, bool for_return_type) {
  assert(type != (BasicType)fp_parameters_done, "fingerprint is incorrectly at done");
  assert(((int)type & ~fp_parameter_feature_mask) == 0, "fingerprint feature mask yielded non-zero value");
  return (is_java_primitive(type) ||
          is_reference_type(type) ||
          (for_return_type && type == T_VOID));
}

ArgumentSizeComputer::ArgumentSizeComputer(Symbol* signature)
  : SignatureIterator(signature)
{
  _size = 0;
  do_parameters_on(this);  // non-virtual template execution
}

ArgumentCount::ArgumentCount(Symbol* signature)
  : SignatureIterator(signature)
{
  _size = 0;
  do_parameters_on(this);  // non-virtual template execution
}

ReferenceArgumentCount::ReferenceArgumentCount(Symbol* signature)
  : SignatureIterator(signature)
{
  _refs = 0;
  do_parameters_on(this);  // non-virtual template execution
}

void Fingerprinter::compute_fingerprint_and_return_type(bool static_flag) {
  // See if we fingerprinted this method already
  if (_method != NULL) {
    assert(!static_flag, "must not be passed by caller");
    static_flag = _method->is_static();
    _fingerprint = _method->constMethod()->fingerprint();

    if (_fingerprint != zero_fingerprint()) {
      _return_type = _method->result_type();
      assert(is_java_type(_return_type), "return type must be a java type");
      return;
    }

    if (_method->size_of_parameters() > fp_max_size_of_parameters) {
      _fingerprint = overflow_fingerprint();
      _method->constMethod()->set_fingerprint(_fingerprint);
      // as long as we are here compute the return type:
      _return_type = ResultTypeFinder(_method->signature()).type();
      assert(is_java_type(_return_type), "return type must be a java type");
      return;
    }
  }

  // Note:  This will always take the slow path, since _fp==zero_fp.
  initialize_accumulator();
  do_parameters_on(this);
  assert(fp_is_valid_type(_return_type, true), "bad result type");

  // Fill in the return type and static bits:
  _accumulator |= _return_type << fp_static_feature_size;
  if (static_flag) {
    _accumulator |= fp_is_static_bit;
  } else {
    _param_size += 1;  // this is the convention for Method::compute_size_of_parameters
  }

  // Detect overflow.  (We counted _param_size correctly.)
  if (_method == NULL && _param_size > fp_max_size_of_parameters) {
    // We did a one-pass computation of argument size, return type,
    // and fingerprint.
    _fingerprint = overflow_fingerprint();
    return;
  }

  assert(_shift_count < BitsPerLong,
         "shift count overflow %d (%d vs. %d): %s",
         _shift_count, _param_size, fp_max_size_of_parameters,
         _signature->as_C_string());
  assert((_accumulator >> _shift_count) == fp_parameters_done, "must be zero");

  // This is the result, along with _return_type:
  _fingerprint = _accumulator;

  // Cache the result on the method itself:
  if (_method != NULL) {
    _method->constMethod()->set_fingerprint(_fingerprint);
  }
}

// Implementation of SignatureStream

static inline int decode_signature_char(int ch) {
  switch (ch) {
#define EACH_SIG(ch, bt, ignore) \
    case ch: return bt;
    SIGNATURE_TYPES_DO(EACH_SIG, ignore)
#undef EACH_SIG
  }
  return 0;
}

SignatureStream::SignatureStream(const Symbol* signature,
                                 bool is_method) {
  assert(!is_method || signature->starts_with(JVM_SIGNATURE_FUNC),
         "method signature required");
  _signature = signature;
  _limit = signature->utf8_length();
  int oz = (is_method ? 1 : 0);
  _state = oz;
  assert(_state == (int)(is_method ? _s_method : _s_field), "signature state incorrectly set");
  _begin = _end = oz; // skip first '(' in method signatures
  _array_prefix = 0;  // just for definiteness
  _previous_name = NULL;
  _names = NULL;
  next();
}

SignatureStream::~SignatureStream() {
  // decrement refcount for names created during signature parsing
  if (_names != NULL) {
    for (int i = 0; i < _names->length(); i++) {
      _names->at(i)->decrement_refcount();
    }
  } else if (_previous_name != NULL && !_previous_name->is_permanent()) {
    _previous_name->decrement_refcount();
  }
}

inline int SignatureStream::scan_non_primitive(BasicType type) {
  const u1* base = _signature->bytes();
  int end = _end;
  int limit = _limit;
  const u1* tem;
  switch (type) {
  case T_OBJECT:
    tem = (const u1*) memchr(&base[end], JVM_SIGNATURE_ENDCLASS, limit - end);
    end = (tem == NULL ? limit : tem+1 - base);
    break;

  case T_ARRAY:
    while ((end < limit) && ((char)base[end] == JVM_SIGNATURE_ARRAY)) { end++; }
    _array_prefix = end - _end;  // number of '[' chars just skipped
    if (Signature::has_envelope(base[end++])) {
      tem = (const u1*) memchr(&base[end], JVM_SIGNATURE_ENDCLASS, limit - end);
      end = (tem == NULL ? limit : tem+1 - base);
      break;
    }
    break;

  default : ShouldNotReachHere();
  }
  return end;
}

void SignatureStream::next() {
  const Symbol* sig = _signature;
  int len = _limit;
  if (_end >= len) { set_done(); return; }
  _begin = _end;
  int ch = sig->char_at(_begin);
  int btcode = decode_signature_char(ch);
  if (btcode == 0) {
    guarantee(ch == JVM_SIGNATURE_ENDFUNC, "bad signature char %c/%d", ch, ch);
    assert(_state == _s_method, "must be in method");
    _state = _s_method_return;
    _begin = ++_end;
    if (_end >= len) { set_done(); return; }
    ch = sig->char_at(_begin);
    btcode = decode_signature_char(ch);
  }
  BasicType bt = (BasicType) btcode;
  assert(ch == type2char(bt), "bad signature char %c/%d", ch, ch);
  _type = bt;
  if (!is_reference_type(bt)) {
    // Skip over a single character for a primitive type (or void).
    _end++;
    return;
  }
  _end = scan_non_primitive(bt);
}

int SignatureStream::skip_array_prefix(int max_skip_length) {
  if (_type != T_ARRAY) {
    return 0;
  }
  if (_array_prefix > max_skip_length) {
    // strip some but not all levels of T_ARRAY
    _array_prefix -= max_skip_length;
    _begin += max_skip_length;
    return max_skip_length;
  }
  // we are stripping all levels of T_ARRAY,
  // so we must decode the next character
  int whole_array_prefix = _array_prefix;
  int new_begin = _begin + whole_array_prefix;
  _begin = new_begin;
  int ch = _signature->char_at(new_begin);
  int btcode = decode_signature_char(ch);
  BasicType bt = (BasicType) btcode;
  assert(ch == type2char(bt), "bad signature char %c/%d", ch, ch);
  _type = bt;
  assert(bt != T_VOID && bt != T_ARRAY, "bad signature type");
  // Don't bother to call scan_non_primitive, since it won't
  // change the value of _end.
  return whole_array_prefix;
}

bool Signature::is_valid_array_signature(const Symbol* sig) {
  assert(sig->utf8_length() > 1, "this should already have been checked");
  assert(sig->char_at(0) == JVM_SIGNATURE_ARRAY, "this should already have been checked");
  // The first character is already checked
  int i = 1;
  int len = sig->utf8_length();
  // First skip all '['s
  while(i < len - 1 && sig->char_at(i) == JVM_SIGNATURE_ARRAY) i++;

  // Check type
  switch(sig->char_at(i)) {
  case JVM_SIGNATURE_BYTE:
  case JVM_SIGNATURE_CHAR:
  case JVM_SIGNATURE_DOUBLE:
  case JVM_SIGNATURE_FLOAT:
  case JVM_SIGNATURE_INT:
  case JVM_SIGNATURE_LONG:
  case JVM_SIGNATURE_SHORT:
  case JVM_SIGNATURE_BOOLEAN:
    // If it is an array, the type is the last character
    return (i + 1 == len);
  case JVM_SIGNATURE_CLASS:
    // If it is an object, the last character must be a ';'
    return sig->char_at(len - 1) == JVM_SIGNATURE_ENDCLASS;
  }
  return false;
}

BasicType Signature::basic_type(int ch) {
  int btcode = decode_signature_char(ch);
  if (btcode == 0)  return T_ILLEGAL;
  return (BasicType) btcode;
}

static const int jl_len = 10, object_len = 6, jl_object_len = jl_len + object_len;
static const char jl_str[] = "java/lang/";

#ifdef ASSERT
static bool signature_symbols_sane() {
  static bool done;
  if (done)  return true;
  done = true;
  // test some tense code that looks for common symbol names:
  assert(vmSymbols::java_lang_Object()->utf8_length() == jl_object_len &&
         vmSymbols::java_lang_Object()->starts_with(jl_str, jl_len) &&
         vmSymbols::java_lang_Object()->ends_with("Object", object_len) &&
         vmSymbols::java_lang_Object()->is_permanent() &&
         vmSymbols::java_lang_String()->utf8_length() == jl_object_len &&
         vmSymbols::java_lang_String()->starts_with(jl_str, jl_len) &&
         vmSymbols::java_lang_String()->ends_with("String", object_len) &&
         vmSymbols::java_lang_String()->is_permanent(),
         "sanity");
  return true;
}
#endif //ASSERT

// returns a symbol; the caller is responsible for decrementing it
Symbol* SignatureStream::find_symbol() {
  // Create a symbol from for string _begin _end
  int begin = raw_symbol_begin();
  int end   = raw_symbol_end();

  const char* symbol_chars = (const char*)_signature->base() + begin;
  int len = end - begin;

  // Quick check for common symbols in signatures
  assert(signature_symbols_sane(), "incorrect signature sanity check");
  if (len == jl_object_len &&
      memcmp(symbol_chars, jl_str, jl_len) == 0) {
    if (memcmp("String", symbol_chars + jl_len, object_len) == 0) {
      return vmSymbols::java_lang_String();
    } else if (memcmp("Object", symbol_chars + jl_len, object_len) == 0) {
      return vmSymbols::java_lang_Object();
    }
  }

  Symbol* name = _previous_name;
  if (name != NULL && name->equals(symbol_chars, len)) {
    return name;
  }

  // Save names for cleaning up reference count at the end of
  // SignatureStream scope.
  name = SymbolTable::new_symbol(symbol_chars, len);

  // Only allocate the GrowableArray for the _names buffer if more than
  // one name is being processed in the signature.
  if (_previous_name != NULL &&
      !_previous_name->is_permanent() &&
      !name->is_permanent() &&
      _names == NULL) {
    _names =  new GrowableArray<Symbol*>(10);
    _names->push(_previous_name);
  }
  if (!name->is_permanent() && _previous_name != NULL) {
    if (_names == NULL) {
      _names = new GrowableArray<Symbol*>(10);
    }
    _names->push(name);  // save new symbol for decrementing later
  }
  _previous_name = name;
  return name;
}

Klass* SignatureStream::as_klass(Handle class_loader, Handle protection_domain,
                                 FailureMode failure_mode, TRAPS) {
  if (!is_reference())  return NULL;
  Symbol* name = as_symbol();
  Klass* k = NULL;
  if (failure_mode == ReturnNull) {
    // Note:  SD::resolve_or_null returns NULL for most failure modes,
    // but not all.  Circularity errors, invalid PDs, etc., throw.
    k = SystemDictionary::resolve_or_null(name, class_loader, protection_domain, CHECK_NULL);
  } else if (failure_mode == CachedOrNull) {
    NoSafepointVerifier nsv;  // no loading, now, we mean it!
    assert(!HAS_PENDING_EXCEPTION, "");
    k = SystemDictionary::find(name, class_loader, protection_domain, CHECK_NULL);
    // SD::find does not trigger loading, so there should be no throws
    // Still, bad things can happen, so we CHECK_NULL and ask callers
    // to do likewise.
    return k;
  } else {
    // The only remaining failure mode is NCDFError.
    // The test here allows for an additional mode CNFException
    // if callers need to request the reflective error instead.
    bool throw_error = (failure_mode == NCDFError);
    k = SystemDictionary::resolve_or_fail(name, class_loader, protection_domain, throw_error, CHECK_NULL);
  }

  return k;
}

oop SignatureStream::as_java_mirror(Handle class_loader, Handle protection_domain,
                                    FailureMode failure_mode, TRAPS) {
  if (!is_reference())
    return Universe::java_mirror(type());
  Klass* klass = as_klass(class_loader, protection_domain, failure_mode, CHECK_NULL);
  if (klass == NULL)  return NULL;
  return klass->java_mirror();
}

void SignatureStream::skip_to_return_type() {
  while (!at_return_type()) {
    next();
  }
}

#ifdef ASSERT

extern bool signature_constants_sane(); // called from basic_types_init()

bool signature_constants_sane() {
  // for the lookup table, test every 8-bit code point, and then some:
  for (int i = -256; i <= 256; i++) {
    int btcode = 0;
    switch (i) {
#define EACH_SIG(ch, bt, ignore) \
    case ch: { btcode = bt; break; }
    SIGNATURE_TYPES_DO(EACH_SIG, ignore)
#undef EACH_SIG
    }
    int btc = decode_signature_char(i);
    assert(btc == btcode, "misconfigured table: %d => %d not %d", i, btc, btcode);
  }
  return true;
}

bool SignatureVerifier::is_valid_method_signature(Symbol* sig) {
  const char* method_sig = (const char*)sig->bytes();
  ssize_t len = sig->utf8_length();
  ssize_t index = 0;
  if (method_sig != NULL && len > 1 && method_sig[index] == JVM_SIGNATURE_FUNC) {
    ++index;
    while (index < len && method_sig[index] != JVM_SIGNATURE_ENDFUNC) {
      ssize_t res = is_valid_type(&method_sig[index], len - index);
      if (res == -1) {
        return false;
      } else {
        index += res;
      }
    }
    if (index < len && method_sig[index] == JVM_SIGNATURE_ENDFUNC) {
      // check the return type
      ++index;
      return (is_valid_type(&method_sig[index], len - index) == (len - index));
    }
  }
  return false;
}

bool SignatureVerifier::is_valid_type_signature(Symbol* sig) {
  const char* type_sig = (const char*)sig->bytes();
  ssize_t len = sig->utf8_length();
  return (type_sig != NULL && len >= 1 &&
          (is_valid_type(type_sig, len) == len));
}

// Checks to see if the type (not to go beyond 'limit') refers to a valid type.
// Returns -1 if it is not, or the index of the next character that is not part
// of the type.  The type encoding may end before 'limit' and that's ok.
ssize_t SignatureVerifier::is_valid_type(const char* type, ssize_t limit) {
  ssize_t index = 0;

  // Iterate over any number of array dimensions
  while (index < limit && type[index] == JVM_SIGNATURE_ARRAY) ++index;
  if (index >= limit) {
    return -1;
  }
  switch (type[index]) {
    case JVM_SIGNATURE_BYTE:
    case JVM_SIGNATURE_CHAR:
    case JVM_SIGNATURE_FLOAT:
    case JVM_SIGNATURE_DOUBLE:
    case JVM_SIGNATURE_INT:
    case JVM_SIGNATURE_LONG:
    case JVM_SIGNATURE_SHORT:
    case JVM_SIGNATURE_BOOLEAN:
    case JVM_SIGNATURE_VOID:
      return index + 1;
    case JVM_SIGNATURE_CLASS:
      for (index = index + 1; index < limit; ++index) {
        char c = type[index];
        switch (c) {
          case JVM_SIGNATURE_ENDCLASS:
            return index + 1;
          case '\0': case JVM_SIGNATURE_DOT: case JVM_SIGNATURE_ARRAY:
            return -1;
          default: ; // fall through
        }
      }
      // fall through
    default: ; // fall through
  }
  return -1;
}

#endif // ASSERT