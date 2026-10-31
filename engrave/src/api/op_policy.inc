// The rollback policy of every edit operation, one row each:
//
//   ENGRAVE_OP(<operation>, Transaction | Snapshot | Unexposed)
//
// The rows follow the verdicts in engrave/tests/harness/op_evidence.json, and
// engrave_op_policy_test fails when they differ. An operation without evidence
// yet is Snapshot. Each group of operations keeps to its own section.

// property (ops/property.cpp)
ENGRAVE_OP(SetProperty, Transaction)
ENGRAVE_OP(ResetProperty, Transaction)
ENGRAVE_OP(SetStyle, Transaction)

// notes (ops/notes.cpp)
ENGRAVE_OP(EnterNotes, Snapshot)
ENGRAVE_OP(SetPitch, Snapshot)
ENGRAVE_OP(SetDuration, Snapshot)
ENGRAVE_OP(Remove, Snapshot)

// measures (ops/measures.cpp)

// parts (ops/parts.cpp)

// spanners (ops/spanners.cpp)

// text (ops/text.cpp)
