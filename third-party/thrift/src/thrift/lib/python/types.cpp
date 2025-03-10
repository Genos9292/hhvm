/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <thrift/lib/python/types.h>
#include <thrift/lib/python/types_api.h> // @manual

#include <folly/Indestructible.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/lang/New.h>
#include <folly/python/import.h>
#include <thrift/lib/cpp2/protocol/TableBasedSerializer.h>

namespace apache {
namespace thrift {
namespace python {

constexpr const size_t kHeadOffset = sizeof(PyVarObject);
constexpr const size_t kFieldOffset = sizeof(PyObject*);

/**
 * In this module, some of the functions have two versions: one for 'Mutable'
 * and one for "Immutable" in the name. There are slight differences between
 * these versions. For instance, when dealing with standard default value
 * related functions, immutable types utilize Python `tuple` for a Thrift list,
 * whereas mutable types utilize Python `list`.
 */

namespace {

/***
 * Imports the thrift python types module, and returns true iff it succeeded.
 */
bool ensure_module_imported() {
  static ::folly::python::import_cache_nocapture import(
      ::import_thrift__python__types);
  return import();
}

/**
 * Throws an exception if the thrift python types module could not be imported.
 */
void ensureImportOrThrow() {
  if (!ensure_module_imported()) {
    throw std::runtime_error("import_thrift__python__types failed");
  }
}

using GetStandardDefaultValueForTypeFunc =
    std::tuple<UniquePyObjectPtr, bool>(const detail::TypeInfo&);

PyObject* createStructTupleWithDefaultValues(
    const detail::StructInfo&, GetStandardDefaultValueForTypeFunc);

/**
 * Returns the appropriate standard immutable default value for the given
 * `typeInfo`, along with a boolean indicating whether it should be added to
 * the cache.
 *
 * The standard default values are as follows:
 *   * `0L` for integral numbers.
 *   * `0d` for floating-point numbers.
 *   * `false` for booleans.
 *   * `""` (i.e., the empty string) for strings and `binary` fields (or an
 *      empty `IOBuf` if applicable).
 *   * An empty `tuple` for lists and maps.
 *   * An empty `frozenset` for sets.
 *   * A recursively default-initialized instance for structs and unions.
 *
 * @throws if there is no standard default value
 */
std::tuple<UniquePyObjectPtr, bool> getStandardImmutableDefaultValueForType(
    const detail::TypeInfo& typeInfo) {
  UniquePyObjectPtr value = nullptr;
  bool addValueToCache = true;
  switch (typeInfo.type) {
    case protocol::TType::T_BYTE:
    case protocol::TType::T_I16:
    case protocol::TType::T_I32:
    case protocol::TType::T_I64:
      // For integral values, the default is `0L`.
      value = UniquePyObjectPtr(PyLong_FromLong(0));
      break;
    case protocol::TType::T_DOUBLE:
    case protocol::TType::T_FLOAT:
      // For floating point values, the default is `0d`.
      value = UniquePyObjectPtr(PyFloat_FromDouble(0));
      break;
    case protocol::TType::T_BOOL:
      // For booleans, the default is `false`.
      value = UniquePyObjectPtr(Py_False);
      Py_INCREF(Py_False);
      break;
    case protocol::TType::T_STRING:
      // For strings, the default value is the empty string (or, if `IOBuf`s are
      // used, an empty `IOBuf`).
      switch (*static_cast<const detail::StringFieldType*>(typeInfo.typeExt)) {
        case detail::StringFieldType::String:
        case detail::StringFieldType::StringView:
        case detail::StringFieldType::Binary:
        case detail::StringFieldType::BinaryStringView:
          value = UniquePyObjectPtr(PyBytes_FromString(""));
          break;
        case detail::StringFieldType::IOBuf:
        case detail::StringFieldType::IOBufPtr:
        case detail::StringFieldType::IOBufObj:
          auto buf = create_IOBuf(folly::IOBuf::create(0));
          value = UniquePyObjectPtr(buf);
          addValueToCache = false;
          break;
      }
      break;
    case protocol::TType::T_LIST:
    case protocol::TType::T_MAP:
      // For lists and maps, the default value is an empty tuple.
      value = UniquePyObjectPtr(PyTuple_New(0));
      break;
    case protocol::TType::T_SET:
      // For sets, the default value is an empty `frozenset`.
      value = UniquePyObjectPtr(PyFrozenSet_New(nullptr));
      break;
    case protocol::TType::T_STRUCT: {
      // For struct and unions, the default value is a (recursively)
      // default-initialized instance.
      auto structInfo =
          static_cast<const detail::StructInfo*>(typeInfo.typeExt);
      value = UniquePyObjectPtr(
          structInfo->unionExt != nullptr
              ? createUnionTuple()
              : createStructTupleWithDefaultValues(
                    *structInfo, getStandardImmutableDefaultValueForType));
      break;
    }
    default:
      LOG(FATAL) << "invalid typeInfo TType " << typeInfo.type;
  }
  if (value == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }
  return std::tuple(std::move(value), addValueToCache);
}

/**
 * Returns the appropriate standard mutable default value for the given
 * `typeInfo`, along with a boolean indicating whether it should be added to
 * the cache.
 *
 * The standard default values are as follows:
 *   * `0L` for integral numbers.
 *   * `0d` for floating-point numbers.
 *   * `false` for booleans.
 *   * `""` (i.e., the empty string) for strings and `binary` fields (or an
 *      empty `IOBuf` if applicable).
 *   * An empty `list` for lists.
 *   * An empty `tuple` for maps.
 *   * An empty `frozenset` for sets.
 *   * A recursively default-initialized instance for structs and unions.
 *
 * @throws if there is no standard default value
 */
std::tuple<UniquePyObjectPtr, bool> getStandardMutableDefaultValueForType(
    const detail::TypeInfo& typeInfo) {
  UniquePyObjectPtr value = nullptr;
  bool addValueToCache = true;
  switch (typeInfo.type) {
    case protocol::TType::T_BYTE:
    case protocol::TType::T_I16:
    case protocol::TType::T_I32:
    case protocol::TType::T_I64:
      // For integral values, the default is `0L`.
      value = UniquePyObjectPtr(PyLong_FromLong(0));
      break;
    case protocol::TType::T_DOUBLE:
    case protocol::TType::T_FLOAT:
      // For floating point values, the default is `0d`.
      value = UniquePyObjectPtr(PyFloat_FromDouble(0));
      break;
    case protocol::TType::T_BOOL:
      // For booleans, the default is `false`.
      value = UniquePyObjectPtr(Py_False);
      Py_INCREF(Py_False);
      break;
    case protocol::TType::T_STRING:
      // For strings, the default value is the empty string (or, if `IOBuf`s are
      // used, an empty `IOBuf`).
      switch (*static_cast<const detail::StringFieldType*>(typeInfo.typeExt)) {
        case detail::StringFieldType::String:
        case detail::StringFieldType::StringView:
        case detail::StringFieldType::Binary:
        case detail::StringFieldType::BinaryStringView:
          value = UniquePyObjectPtr(PyBytes_FromString(""));
          break;
        case detail::StringFieldType::IOBuf:
        case detail::StringFieldType::IOBufPtr:
        case detail::StringFieldType::IOBufObj:
          auto buf = create_IOBuf(folly::IOBuf::create(0));
          value = UniquePyObjectPtr(buf);
          addValueToCache = false;
          break;
      }
      break;
    case protocol::TType::T_LIST:
      value = UniquePyObjectPtr(PyList_New(0));
      break;
    case protocol::TType::T_MAP:
      // For maps, the default value is an empty tuple.
      value = UniquePyObjectPtr(PyTuple_New(0));
      break;
    case protocol::TType::T_SET:
      // For sets, the default value is an empty `set`.
      value = UniquePyObjectPtr(PySet_New(nullptr));
      break;
    case protocol::TType::T_STRUCT: {
      // For struct and unions, the default value is a (recursively)
      // default-initialized instance.
      auto structInfo =
          static_cast<const detail::StructInfo*>(typeInfo.typeExt);
      value = UniquePyObjectPtr(
          structInfo->unionExt != nullptr
              ? createUnionTuple()
              : createStructTupleWithDefaultValues(
                    *structInfo, getStandardMutableDefaultValueForType));
      break;
    }
    default:
      LOG(FATAL) << "invalid typeInfo TType " << typeInfo.type;
  }
  if (value == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }
  return std::tuple(std::move(value), addValueToCache);
}

/**
 * Returns the standard default value for a thrift field of the given type.
 *
 * The returned value will either be the one provided by the user (in the Thrift
 * IDL), or the standard default value for the given `typeInfo`.
 *
 * @param `index` of the field in `userDefaultValues`, i.e. insertion order (NOT
 *        field ID).
 *
 * @throws if the thrift python types module could not be imported.
 */
UniquePyObjectPtr getDefaultValueForField(
    const detail::TypeInfo* typeInfo,
    const FieldValueMap& userDefaultValues,
    int16_t index,
    GetStandardDefaultValueForTypeFunc getStandardDefaultValueForTypeFunc) {
  ensureImportOrThrow();

  // 1. If the user explicitly provided a default value, use it.
  auto userDefaultValueIt = userDefaultValues.find(index);
  if (userDefaultValueIt != userDefaultValues.end()) {
    PyObject* value = userDefaultValueIt->second;
    Py_INCREF(value);
    return UniquePyObjectPtr(value);
  }

  // 2. Check local cache for an existing default value.
  static folly::Indestructible<
      std::unordered_map<const detail::TypeInfo*, PyObject*>>
      defaultValueCache;
  auto cachedDefaultValueIt = defaultValueCache->find(typeInfo);
  if (cachedDefaultValueIt != defaultValueCache->end()) {
    UniquePyObjectPtr value(cachedDefaultValueIt->second);
    Py_INCREF(value.get());
    return value;
  }

  // 3. No cached value found. Determine the default value, and update cache (if
  // applicable).
  auto [value, addValueToCache] = getStandardDefaultValueForTypeFunc(*typeInfo);
  if (addValueToCache) {
    defaultValueCache->emplace(typeInfo, value.get());
    Py_INCREF(value.get());
  }
  return std::move(value);
}

const char* getIssetFlags(const void* object) {
  PyObject* isset =
      *toPyObjectPtr(static_cast<const char*>(object) + kHeadOffset);
  const char* issetFlags = PyBytes_AsString(isset);
  if (issetFlags == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }
  return issetFlags;
}

/**
 * Returns a new "struct tuple" with all its elements initialized.
 *
 * As in `createStructTuple()`, the first element of the tuple is a
 * 0-initialized bytearray with `numFields` bytes (to be used as isset flags).
 *
 * However, the remaining elements (1 through `numFields + 1`) are initialized
 * with the appropriate default value for the corresponding field (see below).
 * The order corresponds to the order of fields in the given `structInfo`
 * (i.e., the insertion order, NOT the field ids).
 *
 * The default value for optional fields is always `Py_None`. For other fields,
 * the default value is either specified by the user or the "standard" value
 * for the corresponding type. This is identified by the function parameter
 * `getStandardDefaultValueForTypeFunc`.
 *
 * For more information, see `getStandardImmutableDefaultValueForType()` and
 * `getStandardMutableDefaultValueType()` functions, which are potential values
 * for the `getStandardDefaultValueForTypeFunc` parameter.
 */
PyObject* createStructTupleWithDefaultValues(
    const detail::StructInfo& structInfo,
    GetStandardDefaultValueForTypeFunc getStandardDefaultValueForTypeFunc) {
  const int16_t numFields = structInfo.numFields;
  UniquePyObjectPtr tuple{createStructTuple(numFields)};
  if (tuple == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }

  // Initialize tuple[1:numFields+1] with default field values.
  const auto& defaultValues =
      *static_cast<const FieldValueMap*>(structInfo.customExt);
  for (int fieldIndex = 0; fieldIndex < numFields; ++fieldIndex) {
    const detail::FieldInfo& fieldInfo = structInfo.fieldInfos[fieldIndex];
    if (fieldInfo.qualifier == detail::FieldQualifier::Optional) {
      PyTuple_SET_ITEM(tuple.get(), fieldIndex + 1, Py_None);
      Py_INCREF(Py_None);
    } else {
      PyTuple_SET_ITEM(
          tuple.get(),
          fieldIndex + 1,
          getDefaultValueForField(
              fieldInfo.typeInfo,
              defaultValues,
              fieldIndex,
              getStandardDefaultValueForTypeFunc)
              .release());
    }
  }
  return tuple.release();
}

/**
 * Populates only the unset fields of a "struct tuple" with default values.
 *
 * The `tuple` parameter should be a valid Python `tuple` object, created by
 * the `createStructTuple()`.
 *
 * Iterates through the elements (from 1 to `numFields + 1`). If a field
 * is unset, it is populated with the corresponding default value.
 * The mechanism for determining the default value is the same as in the
 * `createStructTupleWithDefaultValues()` function. Please see the documentation
 * of `createStructTupleWithDefaultValues()` for details on how the default
 * value is identified.
 *
 * Throws on error
 */
void populateStructTupleUnsetFieldsWithDefaultValues(
    PyObject* tuple,
    const detail::StructInfo& structInfo,
    GetStandardDefaultValueForTypeFunc getStandardDefaultValueForTypeFunc) {
  if (tuple == nullptr) {
    throw std::runtime_error("null tuple!");
  }

  DCHECK(PyTuple_Check(tuple));
  const int16_t numFields = structInfo.numFields;
  DCHECK(PyTuple_Size(tuple) == numFields + 1);

  const auto& defaultValues =
      *static_cast<const FieldValueMap*>(structInfo.customExt);
  const char* issetFlags = getIssetFlags(tuple);
  for (int i = 0; i < numFields; ++i) {
    // If the field is already set, this implies that the constructor has
    // already assigned a value to the field. In this case, we skip it and
    // avoid overwriting it with the default value.
    if (issetFlags[i]) {
      continue;
    }

    const detail::FieldInfo& fieldInfo = structInfo.fieldInfos[i];
    PyObject* oldValue = PyTuple_GET_ITEM(tuple, i + 1);
    if (fieldInfo.qualifier == detail::FieldQualifier::Optional) {
      PyTuple_SET_ITEM(tuple, i + 1, Py_None);
      Py_INCREF(Py_None);
    } else {
      // getDefaultValueForField calls `Py_INCREF`
      PyTuple_SET_ITEM(
          tuple,
          i + 1,
          getDefaultValueForField(
              fieldInfo.typeInfo,
              defaultValues,
              i,
              getStandardDefaultValueForTypeFunc)
              .release());
    }
    Py_DECREF(oldValue);
  }
}

} // namespace

PyObject* createUnionTuple() {
  UniquePyObjectPtr tuple{
      PyTuple_New(2)}; // one for the type and the other for the value
  if (!tuple) {
    THRIFT_PY3_CHECK_ERROR();
  }
  PyTuple_SET_ITEM(
      tuple.get(), 0, PyLong_FromLong(0)); // init type to __EMPTY__ (0)
  PyTuple_SET_ITEM(tuple.get(), 1, Py_None); // init value to None
  Py_INCREF(Py_None);
  return tuple.release();
}

PyObject* createStructTuple(int16_t numFields) {
  // Allocate and 0-initialize numFields bytes.
  UniquePyObjectPtr issetArr{PyBytes_FromStringAndSize(nullptr, numFields)};
  if (issetArr == nullptr) {
    return nullptr;
  }
  char* flags = PyBytes_AsString(issetArr.get());
  if (flags == nullptr) {
    return nullptr;
  }
  for (Py_ssize_t i = 0; i < numFields; ++i) {
    flags[i] = '\0';
  }

  // Create tuple, with isset byte array as first element (followed by
  // `numFields` uninitialized elements).
  PyObject* tuple{PyTuple_New(numFields + 1)};
  if (tuple == nullptr) {
    return nullptr;
  }
  PyTuple_SET_ITEM(tuple, 0, issetArr.release());
  return tuple;
}

PyObject* createImmutableStructTupleWithDefaultValues(
    const detail::StructInfo& structInfo) {
  return createStructTupleWithDefaultValues(
      structInfo, getStandardImmutableDefaultValueForType);
}

PyObject* createMutableStructTupleWithDefaultValues(
    const detail::StructInfo& structInfo) {
  return createStructTupleWithDefaultValues(
      structInfo, getStandardMutableDefaultValueForType);
}

PyObject* createStructTupleWithNones(const detail::StructInfo& structInfo) {
  const int16_t numFields = structInfo.numFields;
  UniquePyObjectPtr tuple{createStructTuple(numFields)};
  if (tuple == nullptr) {
    return nullptr;
  }

  // Initialize tuple[1:numFields+1] with 'None'.
  for (int i = 0; i < numFields; ++i) {
    PyTuple_SET_ITEM(tuple.get(), i + 1, Py_None);
    Py_INCREF(Py_None);
  }
  return tuple.release();
}

void setStructIsset(void* object, int16_t index, bool value) {
  PyObject** issetPyBytesPtr =
      toPyObjectPtr(static_cast<char*>(object) + kHeadOffset);
  char* flags = PyBytes_AsString(*issetPyBytesPtr);
  if (flags == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }
  flags[index] = value;
}

void* setImmutableStruct(void* object, const detail::TypeInfo& typeInfo) {
  return setPyObject(
      object,
      UniquePyObjectPtr{createStructTupleWithDefaultValues(
          *static_cast<const detail::StructInfo*>(typeInfo.typeExt),
          getStandardImmutableDefaultValueForType)});
}

void* setUnion(void* object, const detail::TypeInfo& /* typeInfo */) {
  return setPyObject(object, UniquePyObjectPtr{createUnionTuple()});
}

bool getIsset(const void* object, ptrdiff_t offset) {
  const char* flags = getIssetFlags(object);
  return flags[offset];
}

void setIsset(void* object, ptrdiff_t offset, bool value) {
  return setStructIsset(object, offset, value);
}

void populateImmutableStructTupleUnsetFieldsWithDefaultValues(
    PyObject* tuple, const detail::StructInfo& structInfo) {
  populateStructTupleUnsetFieldsWithDefaultValues(
      tuple, structInfo, getStandardImmutableDefaultValueForType);
}

void populateMutableStructTupleUnsetFieldsWithDefaultValues(
    PyObject* tuple, const detail::StructInfo& structInfo) {
  populateStructTupleUnsetFieldsWithDefaultValues(
      tuple, structInfo, getStandardMutableDefaultValueForType);
}

void resetFieldToStandardDefault(
    PyObject* tuple, const detail::StructInfo& structInfo, int index) {
  if (tuple == nullptr) {
    throw std::runtime_error(fmt::format(
        "Received null tuple while resetting struct:`{}`, field-index:'{}'",
        structInfo.name,
        index));
  }

  DCHECK(PyTuple_Check(tuple));
  DCHECK(index < structInfo.numFields);

  const auto& defaultValues =
      *static_cast<const FieldValueMap*>(structInfo.customExt);
  const detail::FieldInfo& fieldInfo = structInfo.fieldInfos[index];
  PyObject* oldValue = PyTuple_GET_ITEM(tuple, index + 1);
  if (fieldInfo.qualifier == detail::FieldQualifier::Optional) {
    PyTuple_SET_ITEM(tuple, index + 1, Py_None);
    Py_INCREF(Py_None);
  } else {
    // getDefaultValueForField calls `Py_INCREF`
    PyTuple_SET_ITEM(
        tuple,
        index + 1,
        getDefaultValueForField(
            fieldInfo.typeInfo,
            defaultValues,
            index,
            getStandardMutableDefaultValueForType)
            .release());
  }
  Py_DECREF(oldValue);
  // DO_BEFORE(alperyoney,20240515): Figure out whether isset flag should be
  // cleared for non-optional fields.
  setStructIsset(tuple, index, false);
}

void clearUnion(void* object) {
  PyObject* pyObj = toPyObject(object);
  PyObject* oldType = PyTuple_GET_ITEM(pyObj, 0);
  UniquePyObjectPtr zero{PyLong_FromLong(0)};
  if (!zero) {
    THRIFT_PY3_CHECK_ERROR();
  }
  PyTuple_SET_ITEM(pyObj, 0, zero.release());
  Py_XDECREF(oldType);
  PyObject* oldValue = PyTuple_GET_ITEM(pyObj, 1);
  PyTuple_SET_ITEM(pyObj, 1, Py_None);
  Py_INCREF(Py_None);
  Py_XDECREF(oldValue);
}

int getActiveId(const void* object) {
  PyObject* const* pyObj = toPyObjectPtr(object);
  auto id = PyLong_AsLong(PyTuple_GET_ITEM(pyObj, 0));
  if (id == -1) {
    THRIFT_PY3_CHECK_POSSIBLE_ERROR();
  }
  return id;
}

void setActiveId(void* object, int value) {
  PyObject* pyObj = toPyObject(object);
  PyObject* oldValue = PyTuple_GET_ITEM(pyObj, 0);
  UniquePyObjectPtr valueObj{PyLong_FromLong(value)};
  if (!valueObj) {
    THRIFT_PY3_CHECK_ERROR();
  }
  PyTuple_SET_ITEM(pyObj, 0, valueObj.release());
  Py_DECREF(oldValue);
}

const detail::UnionExtN<1> unionExt = {
    /* .clear */ clearUnion,
    /* .unionTypeOffset */ 0,
    /* .getActiveId */ getActiveId,
    /* .setActiveId */ setActiveId,
    /* .initMember */ {nullptr},
};

/**
 * Returns a view into the string contained in the given Python bytes referred
 * to by `object`.
 *
 * Note that, if this method returns, the returned optional always holds a
 * `ThriftValue` with a `stringViewValue`.
 *
 * @param object double pointer to a Python bytes object (i.e., a
 *        [`PyBytesObject**`](https://docs.python.org/3/c-api/bytes.html#c.PyBytesObject))
 *
 * @throws if `object` does not contain a valid string.
 */
detail::OptionalThriftValue getString(
    const void* object, const detail::TypeInfo& /* typeInfo */) {
  // Note: `PyObject` is a parent class of `PyBytesObject`, so the following
  // assignment is correct.
  PyObject* pyBytesObject = *toPyObjectPtr(object);

  Py_ssize_t len = 0;
  char* buf = nullptr;
  if (PyBytes_AsStringAndSize(pyBytesObject, &buf, &len) == -1) {
    THRIFT_PY3_CHECK_ERROR();
  }
  return folly::make_optional<detail::ThriftValue>(
      folly::StringPiece{buf, static_cast<std::size_t>(len)});
}

/**
 * Copies the given string `value` into a new `PyBytesObject` instance, and
 * updates the given `object` to hold a pointer to that instance.
 *
 * @param object a `PyBytesObject**` (see `getString()` above).
 * @param value String whose copy will be in a new Python bytes object.
 *
 * @throws if `value` cannot be copied to a new `PyBytesObject`.
 */
void setString(void* object, const std::string& value) {
  UniquePyObjectPtr bytesObj{
      PyBytes_FromStringAndSize(value.data(), value.size())};
  if (bytesObj == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }
  setPyObject(object, std::move(bytesObj));
}

detail::OptionalThriftValue getIOBuf(
    const void* object, const detail::TypeInfo& /* typeInfo */) {
  ensureImportOrThrow();
  PyObject* pyObj = *toPyObjectPtr(object);
  folly::IOBuf* buf = pyObj != nullptr ? get_cIOBuf(pyObj) : nullptr;
  return buf ? folly::make_optional<detail::ThriftValue>(buf)
             : detail::OptionalThriftValue{};
}

void setIOBuf(void* object, const folly::IOBuf& value) {
  ensureImportOrThrow();
  const auto buf = create_IOBuf(value.clone());
  UniquePyObjectPtr iobufObj{buf};
  if (!buf) {
    THRIFT_PY3_CHECK_ERROR();
  }
  setPyObject(object, std::move(iobufObj));
}

detail::OptionalThriftValue getStruct(
    const void* object, const detail::TypeInfo& /* typeInfo */) {
  PyObject* pyObj = *toPyObjectPtr(object);
  return folly::make_optional<detail::ThriftValue>(pyObj);
}

detail::TypeInfo createImmutableStructTypeInfo(
    const DynamicStructInfo& dynamicStructInfo) {
  return {
      /* .type */ protocol::TType::T_STRUCT,
      /* .get */ getStruct,
      /* .set */
      reinterpret_cast<detail::VoidFuncPtr>(
          dynamicStructInfo.isUnion() ? setUnion : setImmutableStruct),
      /* .typeExt */ &dynamicStructInfo.getStructInfo(),
  };
}

void ListTypeInfo::read(
    const void* context,
    void* object,
    std::uint32_t listSize,
    void (*reader)(const void* /*context*/, void* /*val*/)) {
  // use a tuple to represent a list field for immutability
  UniquePyObjectPtr list{PyTuple_New(listSize)};
  if (!list) {
    THRIFT_PY3_CHECK_ERROR();
  }
  for (std::uint32_t i = 0; i < listSize; ++i) {
    PyObject* elem = nullptr;
    reader(context, &elem);
    PyTuple_SET_ITEM(list.get(), i, elem);
  }
  setPyObject(object, std::move(list));
}

size_t ListTypeInfo::write(
    const void* context,
    const void* object,
    size_t (*writer)(const void* /*context*/, const void* /*val*/)) {
  const PyObject* list = toPyObject(object);
  auto size = PyTuple_GET_SIZE(list);
  size_t written = 0;
  for (Py_ssize_t i = 0; i < size; i++) {
    auto elem = PyTuple_GET_ITEM(list, i);
    written += writer(context, &elem);
  }
  return written;
}

void ListTypeInfo::consumeElem(
    const void* context,
    void* object,
    void (*reader)(const void* /*context*/, void* /*val*/)) {
  PyObject* elem = nullptr;
  reader(context, &elem);
  PyObject** pyObjPtr = toPyObjectPtr(object);
  auto currentSize = PyTuple_GET_SIZE(*pyObjPtr);
  if (_PyTuple_Resize(pyObjPtr, currentSize + 1) == -1) {
    THRIFT_PY3_CHECK_ERROR();
  }

  PyTuple_SET_ITEM(*pyObjPtr, currentSize, elem);
}

void MutableListTypeInfo::read(
    const void* context,
    void* object,
    std::uint32_t listSize,
    void (*reader)(const void* /*context*/, void* /*val*/)) {
  // use a PyList to represent a list field
  UniquePyObjectPtr list{PyList_New(listSize)};
  if (list == nullptr) {
    THRIFT_PY3_CHECK_ERROR();
  }
  for (std::uint32_t i = 0; i < listSize; ++i) {
    PyObject* elem = nullptr;
    reader(context, &elem);
    PyList_SET_ITEM(list.get(), i, elem);
  }
  setPyObject(object, std::move(list));
}

size_t MutableListTypeInfo::write(
    const void* context,
    const void* object,
    size_t (*writer)(const void* /*context*/, const void* /*val*/)) {
  const PyObject* list = toPyObject(object);
  const Py_ssize_t size = PyList_GET_SIZE(list);
  size_t written = 0;
  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject* elem = PyList_GET_ITEM(list, i);
    written += writer(context, &elem);
  }
  return written;
}

void MutableListTypeInfo::consumeElem(
    const void* context,
    void* object,
    void (*reader)(const void* /*context*/, void* /*val*/)) {
  PyObject* elem = nullptr;
  reader(context, &elem);
  DCHECK(elem != nullptr);
  PyObject** pyObjPtr = toPyObjectPtr(object);
  if (PyList_Append(*pyObjPtr, elem) == -1) {
    THRIFT_PY3_CHECK_ERROR();
  }
}

void MapTypeInfo::read(
    const void* context,
    void* object,
    std::uint32_t mapSize,
    void (*keyReader)(const void* context, void* key),
    void (*valueReader)(const void* context, void* val)) {
  // use a tuple to represent a map for immutablitity and hashability
  UniquePyObjectPtr map{PyTuple_New(mapSize)};
  if (!map) {
    THRIFT_PY3_CHECK_ERROR();
  }
  for (std::uint32_t i = 0; i < mapSize; ++i) {
    auto read = [=](auto reader) {
      PyObject* obj = nullptr;
      reader(context, &obj);
      return UniquePyObjectPtr(obj);
    };
    UniquePyObjectPtr mkey = read(keyReader);
    UniquePyObjectPtr mvalue = read(valueReader);
    UniquePyObjectPtr elem{PyTuple_New(2)};
    if (!elem) {
      THRIFT_PY3_CHECK_ERROR();
    }
    PyTuple_SET_ITEM(elem.get(), 0, mkey.release());
    PyTuple_SET_ITEM(elem.get(), 1, mvalue.release());
    PyTuple_SET_ITEM(map.get(), i, elem.release());
  }
  setPyObject(object, std::move(map));
}

size_t MapTypeInfo::write(
    const void* context,
    const void* object,
    bool protocolSortKeys,
    size_t (*writer)(
        const void* context, const void* keyElem, const void* valueElem)) {
  size_t written = 0;
  PyObject* map = const_cast<PyObject*>(toPyObject(object));
  auto size = PyTuple_GET_SIZE(map);
  UniquePyObjectPtr seq;
  if (protocolSortKeys) {
    seq = UniquePyObjectPtr{PySequence_List(map)};
    if (!seq) {
      THRIFT_PY3_CHECK_ERROR();
    }
    map = seq.get();
    if (PyList_Sort(map) == -1) {
      THRIFT_PY3_CHECK_ERROR();
    }
  }
  for (std::uint32_t i = 0; i < size; ++i) {
    PyObject* pair = PyTuple_GET_ITEM(map, i);
    PyObject* key = PyTuple_GET_ITEM(pair, 0);
    PyObject* value = PyTuple_GET_ITEM(pair, 1);
    written += writer(context, &key, &value);
  }
  return written;
}

void MapTypeInfo::consumeElem(
    const void* context,
    void* object,
    void (*keyReader)(const void* context, void* key),
    void (*valueReader)(const void* context, void* val)) {
  PyObject** pyObjPtr = toPyObjectPtr(object);
  CHECK_NOTNULL(*pyObjPtr);
  PyObject* mkey = nullptr;
  keyReader(context, &mkey);
  PyObject* mval = nullptr;
  valueReader(context, &mval);
  UniquePyObjectPtr elem{PyTuple_New(2)};
  if (!elem) {
    THRIFT_PY3_CHECK_ERROR();
  }
  PyTuple_SET_ITEM(elem.get(), 0, mkey);
  PyTuple_SET_ITEM(elem.get(), 1, mval);
  auto currentSize = PyTuple_GET_SIZE(*pyObjPtr);
  if (_PyTuple_Resize(pyObjPtr, currentSize + 1) == -1) {
    THRIFT_PY3_CHECK_ERROR();
  }
  PyTuple_SET_ITEM(*pyObjPtr, currentSize, elem.release());
}

DynamicStructInfo::DynamicStructInfo(
    const char* name, int16_t numFields, bool isUnion)
    : name_{name} {
  structInfo_ = static_cast<detail::StructInfo*>(folly::operator_new(
      sizeof(detail::StructInfo) + sizeof(detail::FieldInfo) * numFields,
      std::align_val_t{alignof(detail::StructInfo)}));
  // reserve vector as we are assigning const char* from the string in
  // vector
  fieldNames_.reserve(numFields);
  structInfo_->numFields = numFields;
  structInfo_->name = name_.c_str();
  structInfo_->unionExt =
      isUnion ? reinterpret_cast<const detail::UnionExt*>(&unionExt) : nullptr;
  structInfo_->getIsset = getIsset;
  structInfo_->setIsset = setIsset;
  structInfo_->customExt = &fieldValues_;
}

DynamicStructInfo::~DynamicStructInfo() {
  for (auto kv : fieldValues_) {
    Py_DECREF(kv.second);
  }
  folly::operator_delete(
      structInfo_,
      sizeof(detail::StructInfo) +
          sizeof(detail::FieldInfo) * structInfo_->numFields,
      std::align_val_t{alignof(detail::StructInfo)});
}

void DynamicStructInfo::addFieldInfo(
    detail::FieldID id,
    detail::FieldQualifier qualifier,
    const char* name,
    const detail::TypeInfo* typeInfo) {
  fieldNames_.push_back(name);
  int16_t idx = fieldNames_.size() - 1;
  structInfo_->fieldInfos[idx] = detail::FieldInfo{
      /* .id */ id,
      /* .qualifier */ qualifier,
      /* .name */ fieldNames_[idx].c_str(),
      /* .memberOffset */
      static_cast<ptrdiff_t>(
          kHeadOffset + kFieldOffset * (isUnion() ? 1 : idx + 1)),
      /* .issetOffset */ isUnion() ? 0 : idx,
      /* .typeInfo */ typeInfo};
}

void DynamicStructInfo::addFieldValue(int16_t index, PyObject* fieldValue) {
  DCHECK(fieldValue);
  if (fieldValue == Py_None) {
    return;
  }
  Py_INCREF(fieldValue);
  fieldValues_.emplace(index, fieldValue);
}

const detail::TypeInfo& boolTypeInfo =
    PrimitiveTypeInfo<bool, protocol::TType::T_BOOL>::typeInfo;
const detail::TypeInfo& byteTypeInfo =
    PrimitiveTypeInfo<std::int8_t, protocol::TType::T_BYTE>::typeInfo;
const detail::TypeInfo& i16TypeInfo =
    PrimitiveTypeInfo<std::int16_t, protocol::TType::T_I16>::typeInfo;
const detail::TypeInfo& i32TypeInfo =
    PrimitiveTypeInfo<std::int32_t, protocol::TType::T_I32>::typeInfo;
const detail::TypeInfo& i64TypeInfo =
    PrimitiveTypeInfo<std::int64_t, protocol::TType::T_I64>::typeInfo;
const detail::TypeInfo& doubleTypeInfo =
    PrimitiveTypeInfo<double, protocol::TType::T_DOUBLE>::typeInfo;
const detail::TypeInfo& floatTypeInfo =
    PrimitiveTypeInfo<float, protocol::TType::T_FLOAT>::typeInfo;

const detail::StringFieldType stringFieldType =
    detail::StringFieldType::StringView;

const detail::StringFieldType binaryFieldType =
    detail::StringFieldType::BinaryStringView;

const detail::StringFieldType ioBufFieldType =
    detail::StringFieldType::IOBufObj;

const detail::TypeInfo stringTypeInfo{
    /* .type */ protocol::TType::T_STRING,
    /* .get */ getString,
    /* .set */ reinterpret_cast<detail::VoidFuncPtr>(setString),
    /* .typeExt */ &stringFieldType,
};

const detail::TypeInfo binaryTypeInfo{
    /* .type */ protocol::TType::T_STRING,
    /* .get */ getString,
    /* .set */ reinterpret_cast<detail::VoidFuncPtr>(setString),
    /* .typeExt */ &binaryFieldType,
};

const detail::TypeInfo iobufTypeInfo{
    /* .type */ protocol::TType::T_STRING,
    /* .get */ getIOBuf,
    /* .set */ reinterpret_cast<detail::VoidFuncPtr>(setIOBuf),
    /* .typeExt */ &ioBufFieldType,
};

namespace capi {
PyObject* FOLLY_NULLABLE getThriftData(PyObject* structOrUnion) {
  if (!ensure_module_imported()) {
    return nullptr;
  }
  return _get_fbthrift_data(structOrUnion);
}
PyObject* FOLLY_NULLABLE getExceptionThriftData(PyObject* generatedError) {
  if (!ensure_module_imported()) {
    return nullptr;
  }
  return _get_exception_fbthrift_data(generatedError);
}

/**
 * This is a cpp version is set_struct_field in .pyx, but it saves overhead
 * of checking PyErr_Occurred() that would be necessary with every capi call
 * because the cython version is `except *`.
 *
 * Also, this assumes that struct_tuple has been created from PyTuple_New
 * without setting any fields. If this is used with a struct_tuple created
 * from python, it will leak the old value at index.
 */
int setStructField(PyObject* struct_tuple, int16_t index, PyObject* value) {
  try {
    DCHECK_GT(index, 0);
    setStructIsset(struct_tuple, index - 1, 1);
  } catch (std::runtime_error& e) {
    // In error case, folly::handlePythonError clears error indicator
    // and throws std::runtime_error with message fetched from PyErr.
    //
    PyErr_SetString(PyExc_TypeError, e.what());
    return -1;
  }
  Py_INCREF(value);
  PyTuple_SET_ITEM(struct_tuple, index, value);
  return 0;
}

/**
 * This is a cpp version of Union._fbthrift_update_type_value, but it avoids the
 * overhead of checking PyErr_Occurred(), similar to setStructField.
 */
PyObject* unionTupleFromValue(int64_t type_key, PyObject* value) {
  PyObject* union_tuple = PyTuple_New(2);
  if (union_tuple == nullptr) {
    return nullptr;
  }
  PyObject* py_tag = PyLong_FromLong(type_key);
  if (py_tag == nullptr) {
    Py_DECREF(union_tuple);
    return nullptr;
  }
  Py_INCREF(py_tag);
  PyTuple_SET_ITEM(union_tuple, 0, py_tag);
  Py_INCREF(value);
  PyTuple_SET_ITEM(union_tuple, 1, value);
  return union_tuple;
}

} // namespace capi

} // namespace python
} // namespace thrift
} // namespace apache
