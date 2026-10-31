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
ENGRAVE_OP(EnterNotes, Transaction)
ENGRAVE_OP(SetPitch, Transaction)
ENGRAVE_OP(SetDuration, Transaction)
ENGRAVE_OP(Remove, Transaction)

// measures (ops/measures.cpp)
ENGRAVE_OP(InsertMeasures, Snapshot)
ENGRAVE_OP(RemoveMeasures, Snapshot)
ENGRAVE_OP(SetTimeSig, Snapshot)
ENGRAVE_OP(SetKeySig, Transaction)
ENGRAVE_OP(InsertClef, Transaction)
ENGRAVE_OP(SetLayoutBreak, Snapshot)

// parts (ops/parts.cpp)
ENGRAVE_OP(AddPart, Transaction)
ENGRAVE_OP(RemovePart, Transaction)
ENGRAVE_OP(AddStaff, Snapshot)
ENGRAVE_OP(RemoveStaff, Transaction)
ENGRAVE_OP(GenerateExcerpts, Unexposed)

// spanners (ops/spanners.cpp)
ENGRAVE_OP(AddSpanner, Transaction)
ENGRAVE_OP(AddTie, Transaction)
ENGRAVE_OP(AddTuplet, Transaction)

// text (ops/text.cpp)
ENGRAVE_OP(AddText, Transaction)
ENGRAVE_OP(Add, Transaction)
