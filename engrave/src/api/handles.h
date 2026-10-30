// Element handles: what an id is worth, who gets one, and how a client finds the
// element it names. The public types are in <engrave/handle.h>; this is the
// library's own use of them (open, create, restore, commit, the edit batch).
//
// Two facts drive the design:
//   - an id is assigned by the writer, so only what the writer stores survives a
//     save, a reopen or a snapshot restore;
//   - the register lives on the master score, so it is the only place an id can be
//     looked up, and an entry that is not reachable from the document is stale.

#pragma once

#include <optional>
#include <span>
#include <vector>

#include <engrave/error.h>
#include <engrave/handle.h>

namespace mu::engraving {
class EID;
class EngravingItem;
class EngravingObject;
class MasterScore;
}

namespace engrave {

// What an element's id is worth after the score is saved and reopened.
enum struct IdClass {
    persisted, // written into the score and restored on open; the walk hands it out
    layout,    // created by layout; it has no id in the file and none is handed out
    address,   // no id at all; reached through an Address
    session,   // the file may carry an id, but layout or a read rebuilds the object,
               // so the walk never hands one out
};

IdClass id_class(const mu::engraving::EngravingObject& object);
// Whether the walk gives this object an id: a persisted type that the writer
// stores, and that layout does not rebuild.
bool takes_id(const mu::engraving::EngravingObject& object);

mu::engraving::EID to_engine(const ElementId& id);
ElementId from_engine(const mu::engraving::EID& id);

// Gives every live object an id where one is due and missing, and drops the id of every
// object that is no longer part of the document. It touches neither the undo stack nor
// the revision, so it runs after open, create, restore and commit alike.
void assign_missing_ids(mu::engraving::MasterScore& score);

// Where an element sits: the score it belongs to, its tick and its track. An
// operation that recreates an element leaves all three as they were, which is what
// separates a replacement from a removal.
struct Placement {
    int score = -1;
    int tick = -1;
    int track = -1;
    friend auto operator<=>(const Placement&, const Placement&) = default;
};

struct LiveId {
    ElementId id;
    mu::engraving::EngravingObject* object = nullptr;
    Placement placement;
};

// The objects of the document that carry an id of their own, sorted by id.
std::vector<LiveId> live_ids(mu::engraving::MasterScore& score);
// Every object of the document, in one stable order: per score (master first, then
// the excerpts) the score, its parts, its staves, then the elements. Layout objects
// and segments are included; each object appears once.
std::vector<mu::engraving::EngravingObject*> document_objects(mu::engraving::MasterScore& score);

// The ids an edit created, dropped, and dropped in place of a new one.
struct IdDiff {
    std::vector<ElementId> created;
    std::vector<ElementId> removed;  // gone, with nothing new where it was
    std::vector<ElementId> replaced; // gone, and a new id sits at the same place
};
IdDiff diff_ids(std::span<const LiveId> before, std::span<const LiveId> after);

// The object an id or an address names, and whether it may be edited. Pages,
// systems and spanner segments may not: layout rebuilds them.
struct Resolved {
    mu::engraving::EngravingObject* object = nullptr;
    bool writable = true;
};

// NotFound when no element has the id, StaleHandle when one has it but is no
// longer part of the document. A segment address resolves to the segment; staff
// and voice select the element inside it. A lookup walks the document, so a caller
// that resolves many references resolves each one once.
Result<Resolved> resolve(mu::engraving::MasterScore& score, const ElementId& id);
Result<Resolved> resolve(mu::engraving::MasterScore& score, const Address& address);

// Whether an object still hangs off one of the document's scores. Weaker than
// resolve(): an element on a hidden staff is in the score but outside the walk that
// assign_missing_ids and resolve() use, and an element an undo command removed is still
// in the ownership tree until it is destroyed.
bool in_score(mu::engraving::MasterScore& score, const mu::engraving::EngravingObject* object);

// The id or the address that names this object, when it has one.
std::optional<Address> address_of(const mu::engraving::EngravingObject& object);

// The warnings the reader's id repairs deserve: a malformed id was ignored, or a
// duplicate was given a fresh one. Empty when the input had neither.
std::vector<Diagnostic> id_repairs(mu::engraving::MasterScore& score);

} // namespace engrave
