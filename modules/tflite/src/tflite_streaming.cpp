// SPDX-License-Identifier: MIT
// tflite with streaming (stateful) models: esp-vision's py_tflite.cpp, compiled
// as it is, with two of its TensorFlow Lite Micro types swapped for ones that
// add what streaming models need.
//
// A streaming model, such as microWakeWord's, keeps its state between calls in
// resource variables: VAR_HANDLE, READ_VARIABLE and ASSIGN_VARIABLE, set up
// once by CALL_ONCE. TensorFlow Lite Micro runs them only when the interpreter
// is given a MicroResourceVariables, and py_tflite.cpp builds its interpreters
// without one and registers none of those four operators. Its interpreter and
// resolver are made in functions we can't reach, so this file defines
//
//   StreamingInterpreter  a MicroInterpreter that owns its resource variables:
//                         20 of them, in a 1 KB arena on a 32-bit chip (2 KB
//                         on 64-bit), as ESPHome's micro_wake_word sets up;
//   StreamingOpResolver   a MicroMutableOpResolver with the four state
//                         operators already registered, and room for them;
//
// and has py_tflite.cpp use them in place of the TensorFlow Lite Micro names.
// Every other operator ESPHome registers for these models (Conv2D,
// DepthwiseConv2D, FullyConnected, StridedSlice, Concatenation, Pad, Pack,
// SplitV and the rest) is in py_tflite.cpp's own list already.
//
// Each interpreter gets variables of its own, so the probe interpreter that
// py_tflite.cpp builds and throws away to size the arena leaves nothing behind
// in the real one. The variables' contents live in the tensor arena; only
// their bookkeeping is in the small arena.

#include <cstddef>
#include <cstdint>

#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

namespace tflite {
namespace pydevices {

constexpr size_t kVariableArenaSize = 1024 * sizeof(void *) / 4;
constexpr int kMaxResourceVariables = 20;
constexpr unsigned int kStateOps = 4;

// Constructed before the interpreter it's a base of, so the variables exist by
// the time MicroInterpreter's constructor takes them.
struct ResourceVariableArena {
    alignas(16) uint8_t arena[kVariableArenaSize];
    MicroResourceVariables *variables;

    ResourceVariableArena() {
        MicroAllocator *allocator = MicroAllocator::Create(arena, sizeof(arena));
        variables = allocator ? MicroResourceVariables::Create(allocator, kMaxResourceVariables) : nullptr;
    }
};

class StreamingInterpreter : private ResourceVariableArena, public MicroInterpreter {
public:
    StreamingInterpreter(const Model *model, const MicroOpResolver &op_resolver,
                         uint8_t *tensor_arena, size_t tensor_arena_size)
        : ResourceVariableArena(),
          MicroInterpreter(model, op_resolver, tensor_arena, tensor_arena_size, variables) {}
};

template <unsigned int tOpCount>
class StreamingOpResolver : public MicroMutableOpResolver<tOpCount + kStateOps> {
public:
    StreamingOpResolver() {
        this->AddCallOnce();
        this->AddVarHandle();
        this->AddReadVariable();
        this->AddAssignVariable();
    }
};

}  // namespace pydevices
}  // namespace tflite

// The swap. The headers above are include-guarded, so py_tflite.cpp's own
// includes of them are skipped and the names change only in its code.
#define MicroInterpreter pydevices::StreamingInterpreter
#define MicroMutableOpResolver pydevices::StreamingOpResolver

#include "py_tflite.cpp"

#include "py/objstr.h"

// tflite.__revision__: which tflite this firmware was built from. The module
// is esp-vision's, compiled as it is, so its globals table can't carry the
// attribute; the module's attribute delegation answers for it instead. The
// build passes TFLITE_REVISION (git describe) as a compile definition; a build
// with no git says "unknown" rather than guessing.
#ifndef TFLITE_REVISION
#define TFLITE_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(tflite_revision_obj, TFLITE_REVISION);

extern "C" void tflite_module_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    (void)self_in;
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR___revision__) {
        dest[0] = MP_OBJ_FROM_PTR(&tflite_revision_obj);
    }
}
MP_REGISTER_MODULE_DELEGATION(tflite_module, tflite_module_attr);
