/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSError.h"

#include "hermes/BCGen/HBC/DebugInfo.h"
#include "hermes/Support/OptValue.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSCallSite.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"

#include "llvh/ADT/ScopeExit.h"
#pragma GCC diagnostic push

#ifdef HERMES_COMPILER_SUPPORTS_WSHORTEN_64_TO_32
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#endif
namespace hermes {
namespace vm {
//===----------------------------------------------------------------------===//
// class JSError

const ObjectVTable JSError::vt{
    VTable(
        CellKind::JSErrorKind,
        cellSize<JSError>(),
        JSError::_finalizeImpl,
        JSError::_mallocSizeImpl),
    JSError::_getOwnIndexedRangeImpl,
    JSError::_haveOwnIndexedImpl,
    JSError::_getOwnIndexedPropertyFlagsImpl,
    JSError::_getOwnIndexedImpl,
    JSError::_setOwnIndexedImpl,
    JSError::_deleteOwnIndexedImpl,
    JSError::_checkAllOwnIndexedImpl,
};

void JSErrorBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSError>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSError *>(cell);
  mb.setVTable(&JSError::vt);
  mb.addField("funcNames", &self->funcNames_);
  mb.addField("domains", &self->domains_);
}

/// Given an object \p targetHandle which may be nullptr:
/// 1. Look for [[CapturedError]] in the object or its prototype chain and
///    return it a JSError.
/// 2. Otherwise, return llvh::None.
static llvh::Optional<Handle<JSError>> getErrorFromStackTarget(
    Runtime &runtime,
    Handle<JSObject> targetHandle) {
  MutableHandle<JSObject> mutHnd =
      runtime.makeMutableHandle<JSObject>(targetHandle.get());
  targetHandle = mutHnd;

  while (targetHandle) {
    NamedPropertyDescriptor desc;
    bool exists = JSObject::getOwnNamedDescriptor(
        targetHandle,
        runtime,
        Predefined::getSymbolID(Predefined::InternalPropertyCapturedError),
        desc);
    if (exists) {
      auto sv = JSObject::getNamedSlotValueUnsafe(*targetHandle, runtime, desc);
      return runtime.makeHandle(vmcast<JSError>(sv.getObject(runtime)));
    }
    if (vmisa<JSError>(*targetHandle)) {
      return Handle<JSError>::vmcast(targetHandle);
    }

    mutHnd.set(targetHandle->getParent(runtime));
  }
  return llvh::None;
}

CallResult<HermesValue>
errorStackGetter(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope(runtime);

  auto targetHandle = args.dyncastThis<JSObject>();
  auto errorHandleOpt = getErrorFromStackTarget(runtime, targetHandle);
  if (!errorHandleOpt.hasValue()) {
    return HermesValue::encodeUndefinedValue();
  }
  auto errorHandle = *errorHandleOpt;
  if (!errorHandle->stacktrace_) {
    // Stacktrace has not been set, we simply return empty string.
    // This is different from other VMs where stacktrace is created when
    // the error object is created. We only set it when the error
    // is raised.
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }
  // It's possible we're getting the stack for a stack overflow
  // RangeError.  Allow ourselves a little extra room to do this.
  vm::ScopedNativeDepthReducer reducer(runtime);
  SmallU16String<32> stack;

  auto errorCtor = Handle<JSObject>::vmcast(&runtime.errorConstructor);

  auto prepareStackTraceRes = JSObject::getNamed_RJS(
      errorCtor,
      runtime,
      Predefined::getSymbolID(Predefined::prepareStackTrace),
      PropOpFlags().plusThrowOnError());

  if (prepareStackTraceRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  MutableHandle<> stackTraceFormatted{runtime};

  auto prepareStackTrace = Handle<Callable>::dyn_vmcast(
      runtime.makeHandle(std::move(*prepareStackTraceRes)));
  if (LLVM_UNLIKELY(prepareStackTrace && !runtime.formattingStackTrace())) {
    const auto &recursionGuard = llvh::make_scope_exit(
        [&runtime]() { runtime.setFormattingStackTrace(false); });
    (void)recursionGuard;

    runtime.setFormattingStackTrace(true);

    auto callSitesRes = JSError::constructCallSitesArray(runtime, errorHandle);

    if (LLVM_UNLIKELY(callSitesRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto prepareRes = Callable::executeCall2(
        prepareStackTrace,
        runtime,
        runtime.getNullValue(),
        targetHandle.getHermesValue(),
        *callSitesRes);
    if (LLVM_UNLIKELY(prepareRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    stackTraceFormatted = std::move(*prepareRes);
  } else {
    if (JSError::constructStackTraceString_RJS(
            runtime, errorHandle, targetHandle, stack) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }

    auto strRes = StringPrimitive::create(runtime, stack);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      // StringPrimitive creation can throw if the stacktrace string is too
      // long. In that case, we replace it with a predefined string.
      stackTraceFormatted = HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::stacktraceTooLong));
      runtime.clearThrownValue();
    } else {
      stackTraceFormatted = std::move(*strRes);
    }
  }

  // We no longer need the accessor. Redefine the stack property to a regular
  // property.
  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  if (JSObject::defineOwnProperty(
          targetHandle,
          runtime,
          Predefined::getSymbolID(Predefined::stack),
          dpf,
          stackTraceFormatted) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return *stackTraceFormatted;
}

CallResult<HermesValue>
errorStackSetter(void *, Runtime &runtime, NativeArgs args) {
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto selfHandle = runtime.makeHandle<JSObject>(res.getValue());

  // Redefines the stack property to a regular property.
  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  if (JSObject::defineOwnProperty(
          selfHandle,
          runtime,
          Predefined::getSymbolID(Predefined::stack),
          dpf,
          args.getArgHandle(0)) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeUndefinedValue();
}

PseudoHandle<JSError> JSError::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle) {
  return create(runtime, parentHandle, /*catchable*/ true);
}

PseudoHandle<JSError> JSError::createUncatchable(
    Runtime &runtime,
    Handle<JSObject> parentHandle) {
  return create(runtime, parentHandle, /*catchable*/ false);
}

PseudoHandle<JSError> JSError::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle,
    bool catchable) {
  auto *cell = runtime.makeAFixed<JSError, HasFinalizer::Yes>(
      runtime,
      parentHandle,
      runtime.getHiddenClassForPrototype(
          *parentHandle, numOverlapSlots<JSError>()),
      catchable);
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

CallResult<Handle<StringPrimitive>> JSError::toString(
    Handle<JSObject> O,
    Runtime &runtime) {
  // 20.5.3.4 Error.prototype.toString ( )

  // 1. and 2. don't apply -- O is already an Object.

  // 3. Let name be ? Get(O, "name").
  auto propRes = JSObject::getNamed_RJS(
      O, runtime, Predefined::getSymbolID(Predefined::name), PropOpFlags());
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> name = runtime.makeHandle(std::move(*propRes));

  // 4. If name is undefined, set name to "Error"; otherwise set name to ?
  // ToString(name).
  MutableHandle<StringPrimitive> nameStr{runtime};
  if (name->isUndefined()) {
    nameStr = runtime.getPredefinedString(Predefined::Error);
  } else {
    auto strRes = toString_RJS(runtime, name);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    nameStr = strRes->get();
  }

  // 5. Let msg be ? Get(O, "message").
  if (LLVM_UNLIKELY(
          (propRes = JSObject::getNamed_RJS(
               O,
               runtime,
               Predefined::getSymbolID(Predefined::message),
               PropOpFlags())) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> msg = runtime.makeHandle(std::move(*propRes));

  // 6. If msg is undefined, set msg to the empty String;
  //    otherwise set msg to ? ToString(msg).
  MutableHandle<StringPrimitive> msgStr{runtime};
  if (msg->isUndefined()) {
    // If msg is undefined, then let msg be the empty String.
    msgStr = runtime.getPredefinedString(Predefined::emptyString);
  } else {
    auto strRes = toString_RJS(runtime, msg);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    msgStr = strRes->get();
  }

  // 7. If name is the empty String, return msg.
  if (nameStr->getStringLength() == 0) {
    return msgStr;
  }

  // 8. If msg is the empty String, return name.
  if (msgStr->getStringLength() == 0) {
    return nameStr;
  }

  // 9. Return the string-concatenation of name, the code unit 0x003A (COLON),
  // the code unit 0x0020 (SPACE), and msg.
  SafeUInt32 length{nameStr->getStringLength()};
  length.add(2);
  length.add(msgStr->getStringLength());
  CallResult<StringBuilder> builderRes =
      StringBuilder::createStringBuilder(runtime, length);
  if (LLVM_UNLIKELY(builderRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto builder = std::move(*builderRes);
  builder.appendStringPrim(nameStr);
  builder.appendASCIIRef({": ", 2});
  builder.appendStringPrim(msgStr);
  return builder.getStringPrimitive();
}

ExecutionStatus JSError::setMessage(
    Handle<JSError> selfHandle,
    Runtime &runtime,
    Handle<> message) {
  auto stringMessage = Handle<StringPrimitive>::dyn_vmcast(message);
  if (LLVM_UNLIKELY(!stringMessage)) {
    auto strRes = toString_RJS(runtime, message);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    stringMessage = runtime.makeHandle(std::move(*strRes));
  }

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  return JSObject::defineOwnProperty(
             selfHandle,
             runtime,
             Predefined::getSymbolID(Predefined::message),
             dpf,
             stringMessage)
      .getStatus();
}

/// \return a list of function names associated with the call stack \p frames.
/// Function names are first read out of 'displayName', followed by the 'name'
/// property of each Callable on the stack. Accessors are skipped. If a
/// Callable does not have a name, or if the name is an accessor, undefined is
/// set. Names are returned in reverse order (topmost frame is first).
/// In case of error returns a nullptr handle.
/// \param skipTopFrame if true, skip the top frame.
static Handle<PropStorage> getCallStackFunctionNames(
    Runtime &runtime,
    bool skipTopFrame,
    size_t sizeHint) {
  auto arrRes = PropStorage::create(runtime, sizeHint);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    runtime.clearThrownValue();
    return Runtime::makeNullHandle<PropStorage>();
  }
  MutableHandle<PropStorage> names{runtime, vmcast<PropStorage>(*arrRes)};

  GCScope gcScope(runtime);
  MutableHandle<> name{runtime};
  auto marker = gcScope.createMarker();

  uint32_t frameIndex = 0;
  uint32_t namesIndex = 0;
  for (StackFramePtr cf : runtime.getStackFrames()) {
    if (frameIndex++ == 0 && skipTopFrame)
      continue;

    name = HermesValue::encodeUndefinedValue();
    if (auto callableHandle = Handle<Callable>::dyn_vmcast(
            Handle<>(&cf.getCalleeClosureOrCBRef()))) {
      NamedPropertyDescriptor desc;
      JSObject *propObj = JSObject::getNamedDescriptorPredefined(
          callableHandle, runtime, Predefined::displayName, desc);

      if (!propObj) {
        propObj = JSObject::getNamedDescriptorPredefined(
            callableHandle, runtime, Predefined::name, desc);
      }

      if (propObj && !desc.flags.accessor &&
          LLVM_LIKELY(!desc.flags.proxyObject) &&
          LLVM_LIKELY(!desc.flags.hostObject)) {
        name = JSObject::getNamedSlotValueUnsafe(propObj, runtime, desc)
                   .unboxToHV(runtime);
      } else if (desc.flags.proxyObject) {
        name = HermesValue::encodeStringValue(
            runtime.getPredefinedString(Predefined::proxyTrap));
      }
    } else if (!cf.getCalleeClosureOrCBRef().isObject()) {
      // If CalleeClosureOrCB is not an object pointer, then it must be a native
      // pointer to a CodeBlock.
      auto *cb =
          cf.getCalleeClosureOrCBRef().getNativePointer<const CodeBlock>();
      if (cb->getNameMayAllocate().isValid())
        name = HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(cb->getNameMayAllocate()));
    }
    if (PropStorage::resize(names, runtime, namesIndex + 1) ==
        ExecutionStatus::EXCEPTION) {
      runtime.clearThrownValue();
      return Runtime::makeNullHandle<PropStorage>();
    }
    auto shv =
        SmallHermesValue::encodeHermesValue(name.getHermesValue(), runtime);
    names->set(namesIndex, shv, runtime.getHeap());
    ++namesIndex;
    gcScope.flushToMarker(marker);
  }

  return std::move(names);
}

ExecutionStatus JSError::recordStackTrace(
    Handle<JSError> selfHandle,
    Runtime &runtime,
    bool skipTopFrame,
    CodeBlock *codeBlock,
    const Inst *ip) {
  if (selfHandle->stacktrace_)
    return ExecutionStatus::RETURNED;

  auto frames = runtime.getStackFrames();

  // Check if the top frame is a JSFunction and we don't have the current
  // CodeBlock, do nothing.
  if (!skipTopFrame && !codeBlock && frames.begin() != frames.end() &&
      frames.begin()->getCalleeCodeBlock(runtime)) {
    return ExecutionStatus::RETURNED;
  }

  StackTracePtr stack{new StackTrace()};
  auto domainsRes = ArrayStorageSmall::create(runtime, 1);
  if (LLVM_UNLIKELY(domainsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto domains = runtime.makeMutableHandle<ArrayStorageSmall>(
      vmcast<ArrayStorageSmall>(*domainsRes));

  // Add the domain to the domains list, provided that it's not the same as the
  // last domain in the list. This allows us to save storage with a constant
  // time check, but we don't have to loop through and check every domain to
  // deduplicate.
  auto addDomain = [&domains,
                    &runtime](CodeBlock *codeBlock) -> ExecutionStatus {
    GCScopeMarkerRAII marker{runtime};
    Handle<Domain> domain = codeBlock->getRuntimeModule()->getDomain(runtime);
    if (domains->size() > 0 &&
        vmcast<Domain>(domains->at(domains->size() - 1).getObject(runtime)) ==
            domain.get()) {
      return ExecutionStatus::RETURNED;
    }
    return ArrayStorageSmall::push_back(domains, runtime, domain);
  };

  if (!skipTopFrame) {
    if (codeBlock) {
      stack->emplace_back(codeBlock, codeBlock->getOffsetOf(ip));
      if (LLVM_UNLIKELY(addDomain(codeBlock) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      stack->emplace_back(nullptr, 0);
    }
  }

  const StackFramePtr framesEnd = *runtime.getStackFrames().end();

  // Fill in the call stack.
  // Each stack frame tracks information about the caller.
  for (StackFramePtr cf : runtime.getStackFrames()) {
    CodeBlock *savedCodeBlock = cf.getSavedCodeBlock();
    const Inst *const savedIP = cf.getSavedIP();
    // Go up one frame and get the callee code block but use the current
    // frame's saved IP. This also allows us to account for bound functions,
    // which have savedCodeBlock == nullptr in order to allow proper returns in
    // the interpreter.
    StackFramePtr prev = cf->getPreviousFrame();
    if (prev != framesEnd) {
      if (CodeBlock *parentCB = prev->getCalleeCodeBlock(runtime)) {
        savedCodeBlock = parentCB;
      }
    }
    if (savedCodeBlock && savedIP) {
      stack->emplace_back(savedCodeBlock, savedCodeBlock->getOffsetOf(savedIP));
      if (LLVM_UNLIKELY(
              addDomain(savedCodeBlock) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      stack->emplace_back(nullptr, 0);
    }
  }
  selfHandle->domains_.set(runtime, domains.get(), runtime.getHeap());

  // Remove the last entry.
  stack->pop_back();

  auto funcNames =
      getCallStackFunctionNames(runtime, skipTopFrame, stack->size());

  // Either the function names is empty, or they have the same count.
  assert(
      (!funcNames || funcNames->size() == stack->size()) &&
      "Function names and stack trace must have same size.");

  selfHandle->stacktrace_ = std::move(stack);
  selfHandle->funcNames_.set(runtime, *funcNames, runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

/// Given a codeblock and opcode offset, \returns the debug information.
OptValue<hbc::DebugSourceLocation> JSError::getDebugInfo(
    CodeBlock *codeBlock,
    uint32_t bytecodeOffset) {
  auto offset = codeBlock->getDebugSourceLocationsOffset();
  if (!offset.hasValue()) {
    return llvh::None;
  }

  return codeBlock->getRuntimeModule()
      ->getBytecode()
      ->getDebugInfo()
      ->getLocationForAddress(offset.getValue(), bytecodeOffset);
}

Handle<StringPrimitive> JSError::getFunctionNameAtIndex(
    Runtime &runtime,
    Handle<JSError> selfHandle,
    size_t index) {
  IdentifierTable &idt = runtime.getIdentifierTable();
  MutableHandle<StringPrimitive> name{
      runtime, runtime.getPredefinedString(Predefined::emptyString)};

  // If funcNames_ is set and contains a string primitive, use that.
  if (selfHandle->funcNames_) {
    assert(
        index < selfHandle->funcNames_.getNonNull(runtime)->size() &&
        "Index out of bounds");
    name = dyn_vmcast<StringPrimitive>(
        selfHandle->funcNames_.getNonNull(runtime)->at(index).unboxToHV(
            runtime));
  }

  if (!name || name->getStringLength() == 0) {
    // We did not have an explicit function name, or it was not a nonempty
    // string. If we have a code block, try its debug info.
    if (const CodeBlock *codeBlock =
            selfHandle->stacktrace_->at(index).codeBlock) {
      name = idt.getStringPrim(runtime, codeBlock->getNameMayAllocate());
    }
  }

  if (!name || name->getStringLength() == 0) {
    return runtime.makeNullHandle<StringPrimitive>();
  }

  return std::move(name);
}

bool JSError::appendFunctionNameAtIndex(
    Runtime &runtime,
    Handle<JSError> selfHandle,
    size_t index,
    llvh::SmallVectorImpl<char16_t> &str) {
  auto name = getFunctionNameAtIndex(runtime, selfHandle, index);

  if (!name)
    return false;

  name->appendUTF16String(str);
  return true;
}

ExecutionStatus JSError::constructStackTraceString_RJS(
    Runtime &runtime,
    Handle<JSError> selfHandle,
    Handle<JSObject> targetHandle,
    SmallU16String<32> &stack) {
  // This method potentially runs javascript, so we need to protect it agains
  // stack overflow.
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  GCScope gcScope(runtime);
  // First of all, the stacktrace string starts with
  // %Error.prototype.toString%(target).
  CallResult<Handle<StringPrimitive>> res =
      JSError::toString(targetHandle, runtime);
  // Keep track whether targetHandle.toString() threw. If it did, the error
  // message will contain a string letting the user know that something went
  // awry.
  const bool targetHandleToStringThrew = res == ExecutionStatus::EXCEPTION;

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    // target.toString() threw an exception; if it is a catchable error try to
    // toString() it so the user has some indication of what went wrong.
    if (!isUncatchableError(runtime.getThrownValue())) {
      HermesValue thrownValue = runtime.getThrownValue();
      if (thrownValue.isObject()) {
        // Clear the pending exception, and try to convert thrownValue to string
        // with %Error.prototype.toString%(thrownValue).
        runtime.clearThrownValue();
        res = JSError::toString(
            runtime.makeHandle<JSObject>(thrownValue), runtime);
      }
    }
  }

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    // An exception happened while trying to get the description for the error.
    if (isUncatchableError(runtime.getThrownValue())) {
      // If JSError::toString throws an uncatchable exception, bubble it up.
      return ExecutionStatus::EXCEPTION;
    }
    // Clear the pending exception so the caller doesn't observe this side
    // effect.
    runtime.clearThrownValue();
    // Append a generic <error> string and move on.
    stack.append(u"<error>");
  } else {
    if (targetHandleToStringThrew) {
      stack.append(u"<while converting error to string: ");
    }
    res->get()->appendUTF16String(stack);
    if (targetHandleToStringThrew) {
      stack.append(u">");
    }
  }

  // Virtual offsets are computed by walking the list of bytecode functions. If
  // we have an extremely deep stack, this could get expensive. Assume that very
  // deep stacks are most likely due to runaway recursion and so use a local
  // cache of virtual offsets.
  llvh::DenseMap<const CodeBlock *, uint32_t> virtualOffsetCache;

  // Append each function location in the call stack to stack trace.
  auto marker = gcScope.createMarker();
  for (size_t index = 0,
              max = selfHandle->stacktrace_->size() -
           selfHandle->firstExposedFrameIndex_;
       index < max;
       index++) {
    char buf[NUMBER_TO_STRING_BUF_SIZE];

    // If the trace contains more than 100 entries, limit the string to the
    // first 50 and the last 50 entries and include a line about the truncation.
    static constexpr unsigned PRINT_HEAD = 50;
    static constexpr unsigned PRINT_TAIL = 50;
    if (LLVM_UNLIKELY(max > PRINT_HEAD + PRINT_TAIL)) {
      if (index == PRINT_HEAD) {
        stack.append("\n    ... skipping ");
        numberToString(max - PRINT_HEAD - PRINT_TAIL, buf, sizeof(buf));
        stack.append(buf);
        stack.append(" frames");
        continue;
      }

      // Skip the middle frames.
      if (index > PRINT_HEAD && index < max - PRINT_TAIL) {
        index = max - PRINT_TAIL;
      }
    }

    const size_t absIndex = index + selfHandle->firstExposedFrameIndex_;
    const StackTraceInfo &sti = selfHandle->stacktrace_->at(absIndex);
    gcScope.flushToMarker(marker);
    // For each stacktrace entry, we add a line with the following format:
    // at <functionName> (<fileName>:<lineNo>:<columnNo>)

    stack.append(u"\n    at ");

    if (!appendFunctionNameAtIndex(runtime, selfHandle, absIndex, stack))
      stack.append(u"anonymous");

    // If we have a null codeBlock, it's a native function, which do not have
    // lines and columns.
    if (!sti.codeBlock) {
      stack.append(u" (native)");
      continue;
    }

    // We are not a native function.
    int32_t lineNo;
    int32_t columnNo;
    OptValue<SymbolID> fileName;
    bool isAddress = false;
    OptValue<hbc::DebugSourceLocation> location =
        getDebugInfo(sti.codeBlock, sti.bytecodeOffset);
    if (location) {
      // Use the line and column from the debug info.
      lineNo = location->line;
      columnNo = location->column;
    } else {
      // Use a "line" and "column" synthesized from the bytecode.
      // In our synthesized stack trace, a line corresponds to a bytecode
      // module. This matches the interpretation in DebugInfo.cpp. Currently we
      // can only have one bytecode module without debug information, namely the
      // one loaded from disk, which is always at index 1.
      // TODO: find a way to track the bytecode modules explicitly.
      // TODO: we do not yet have a way of getting the file name separate from
      // the debug info. For now we end up leaving it as "unknown".
      auto pair = virtualOffsetCache.insert({sti.codeBlock, 0});
      uint32_t &virtualOffset = pair.first->second;
      if (pair.second) {
        // Code block was not in the cache, update the cache.
        virtualOffset = sti.codeBlock->getVirtualOffset();
      }
      // Add 1 to the SegmentID to account for 1-based indexing of
      // symbolication tools.
      lineNo =
          sti.codeBlock->getRuntimeModule()->getBytecode()->getSegmentID() + 1;
      columnNo = sti.bytecodeOffset + virtualOffset;
      isAddress = true;
    }

    stack.append(u" (");
    if (isAddress)
      stack.append(u"address at ");

    // Append the filename. If we have a source location, use the filename from
    // that location; otherwise use the RuntimeModule's sourceURL; otherwise
    // report unknown.
    RuntimeModule *runtimeModule = sti.codeBlock->getRuntimeModule();
    if (location) {
      stack.append(
          runtimeModule->getBytecode()->getDebugInfo()->getFilenameByID(
              location->filenameId));
    } else {
      auto sourceURL = runtimeModule->getSourceURL();
      stack.append(sourceURL.empty() ? "unknown" : sourceURL);
    }
    stack.push_back(u':');

    numberToString(lineNo, buf, NUMBER_TO_STRING_BUF_SIZE);
    stack.append(buf);

    stack.push_back(u':');

    numberToString(columnNo, buf, NUMBER_TO_STRING_BUF_SIZE);
    stack.append(buf);

    stack.push_back(u')');
  }
  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> JSError::constructCallSitesArray(
    Runtime &runtime,
    Handle<JSError> selfHandle) {
  auto max = selfHandle->stacktrace_
      ? selfHandle->stacktrace_->size() - selfHandle->firstExposedFrameIndex_
      : 0;
  auto arrayRes = JSArray::create(runtime, max, 0);
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto array = std::move(*arrayRes);
  if (!selfHandle->stacktrace_) {
    return array.getHermesValue();
  }

  size_t callSiteIndex = 0;

  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();

  for (size_t index = 0; index < max; index++) {
    // TODO: truncate traces? Support Error.stackTraceLimit?
    // Problem: The CallSite API doesn't provide a way to denote skipped frames.
    // V8 truncates bottom frames (and adds no marker) while we truncate middle
    // frames (and in string traces, add a marker with a count).

    auto absIndex = index + selfHandle->firstExposedFrameIndex_;

    // Each CallSite stores a reference to this JSError and a particular frame
    // index, and provides methods for querying information about that frame.
    auto callSiteRes = JSCallSite::create(runtime, selfHandle, absIndex);
    if (callSiteRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto callSite = runtime.makeHandle(*callSiteRes);

    JSArray::setElementAt(array, runtime, callSiteIndex++, callSite);

    gcScope.flushToMarker(marker);
  }

  auto cr =
      JSArray::setLengthProperty(array, runtime, callSiteIndex, PropOpFlags{});
  (void)cr;
  assert(
      cr != ExecutionStatus::EXCEPTION && *cr && "JSArray::setLength() failed");

  return array.getHermesValue();
}

/// \return the code block associated with \p callableHandle if it is a
/// (possibly bound) function, or nullptr otherwise.
static const CodeBlock *getLeafCodeBlock(
    Handle<Callable> callableHandle,
    Runtime &runtime) {
  const Callable *callable = callableHandle.get();
  while (callable) {
    if (auto *asFunction = dyn_vmcast<const JSFunction>(callable)) {
      return asFunction->getCodeBlock(runtime);
    }
    if (auto *asBoundFunction = dyn_vmcast<const BoundFunction>(callable)) {
      callable = asBoundFunction->getTarget(runtime);
    } else {
      break;
    }
  }

  return nullptr;
}

void JSError::popFramesUntilInclusive(
    Runtime &runtime,
    Handle<JSError> selfHandle,
    Handle<Callable> callableHandle) {
  assert(
      selfHandle->stacktrace_ && "Cannot pop frames when stacktrace_ is null");
  // By default, assume we won't encounter the sentinel function and skip the
  // entire stack.
  selfHandle->firstExposedFrameIndex_ = selfHandle->stacktrace_->size();
  auto codeBlock = getLeafCodeBlock(callableHandle, runtime);
  if (!codeBlock) {
    return;
  }
  for (size_t index = 0, max = selfHandle->stacktrace_->size(); index < max;
       index++) {
    const StackTraceInfo &sti = selfHandle->stacktrace_->at(index);
    if (sti.codeBlock == codeBlock) {
      selfHandle->firstExposedFrameIndex_ = index + 1;
      break;
    }
  }
}

void JSError::_finalizeImpl(GCCell *cell, GC &) {
  JSError *self = vmcast<JSError>(cell);
  self->~JSError();
}

size_t JSError::_mallocSizeImpl(GCCell *cell) {
  JSError *self = vmcast<JSError>(cell);
  auto stacktrace = self->stacktrace_.get();
  return stacktrace ? sizeof(StackTrace) +
          stacktrace->capacity() * sizeof(StackTrace::value_type)
                    : 0;
}

} // namespace vm
} // namespace hermes
