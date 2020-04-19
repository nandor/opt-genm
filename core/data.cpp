// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include "core/data.h"
#include "core/prog.h"



// -----------------------------------------------------------------------------
Data::~Data()
{
}


// -----------------------------------------------------------------------------
void Data::eraseFromParent()
{
  getParent()->erase(this->getIterator());
}

// -----------------------------------------------------------------------------
Atom *Data::CreateAtom(const std::string_view name)
{
  Atom *atom = getParent()->CreateAtom(this, name);
  atoms_.push_back(atom);
  return atom;
}

// -----------------------------------------------------------------------------
void Data::erase(iterator it)
{
  atoms_.erase(it);
}
