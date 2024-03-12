//
// SPDX-License-Identifier: Apache-2.0
// [LANL Copyright Statement]
// Copyright (C) 2023 by the ryujin authors
// Copyright (C) 2023 - 2024 by Triad National Security, LLC
//

#include "initial_state_library.template.h"

namespace ryujin
{
  template class InitialStateLibrary<Description, 1, NUMBER>;
  template class InitialStateLibrary<Description, 2, NUMBER>;
  template class InitialStateLibrary<Description, 3, NUMBER>;
} // namespace ryujin
