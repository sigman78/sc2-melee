// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CORE_BORROWED_HPP
#define UQM2_ENGINE_CORE_BORROWED_HPP

namespace uqm {

// A non-owning pointer, named so a bare star reads as a mistake in review
// (docs/cpp-conventions.md rule 13). Lifetime is the referent's owner's
// business and is documented at the member that stores one.
template <class T>
using Borrowed = T *;

}  // namespace uqm

#endif  // UQM2_ENGINE_CORE_BORROWED_HPP
