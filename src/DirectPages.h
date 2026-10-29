// Direct mode: builds the panel's own pages from the gauges picked on the
// settings page and the latest Apex / Hydros readings.
#pragma once
#include "Model.h"

namespace direct {

int pageCount();
// Fills page i (title, layout, gauges with status from their limits).
void build(int i, panel::Page &out);

}  // namespace direct
